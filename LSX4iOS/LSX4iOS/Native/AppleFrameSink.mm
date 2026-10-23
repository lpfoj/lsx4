// SPDX-License-Identifier: GPL-2.0-or-later
#import "LSX4Bridge.h"
#import <UIKit/UIKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "AppleFrameSink.h"
#import "../../RuntimeCore/AppleSystemTextInput.h"
#include "GuestRuntime.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <deque>

namespace {
std::mutex presentationMutex;
std::string presentationDetail;
void PresentationDetail(const char* text) {
    std::lock_guard lock(presentationMutex);presentationDetail=text;
    // Status remains available in failure reports. Per-frame console traffic
    // is an explicit diagnostic option, never part of ordinary game execution.
    static const bool verbose=[NSProcessInfo.processInfo.environment[@"LSX4_FRAME_LOG"] isEqualToString:@"1"];
    if(verbose)NSLog(@"LSX4_FRAME %s",text);
}
}
struct FrameCompletion {
    std::atomic<int> result{0}; // 0 pending, 1 displayed, 2 dropped, 3 GPU error, 4 timeout
    dispatch_semaphore_t semaphore{dispatch_semaphore_create(0)};
    bool Finish(int value) {int pending=0;return result.compare_exchange_strong(pending,value);}
};
@interface LSX4MetalGameView : UIView
- (BOOL)encodeFrame:(const Lsx4::Apple::GuestFrame&)frame
        completion:(std::shared_ptr<FrameCompletion>)completion;
@end
static __weak LSX4MetalGameView* currentGameView;
static std::atomic<std::uint64_t> presentedFrames{0}, droppedFrames{0};
static std::mutex frameTimesMutex;
static std::deque<double> frameTimes;
static double DisplayedFramesPerSecond() {
    std::lock_guard lock(frameTimesMutex);
    if(frameTimes.size()<2 || CACurrentMediaTime()-frameTimes.back()>2)return 0;
    const auto duration=frameTimes.back()-frameTimes.front();
    return duration>0?(frameTimes.size()-1)/duration:0;
}
@implementation LSX4MetalGameView {
    id<MTLDevice> _device;
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _pipeline;
}
+ (Class)layerClass {return CAMetalLayer.class;}
- (instancetype)initWithFrame:(CGRect)frame {
    if((self=[super initWithFrame:frame])) {
        self.backgroundColor=UIColor.blackColor;self.opaque=YES;
        _device=MTLCreateSystemDefaultDevice();_queue=[_device newCommandQueue];
        CAMetalLayer* layer=(CAMetalLayer*)self.layer;layer.device=_device;
        layer.pixelFormat=MTLPixelFormatBGRA8Unorm;layer.framebufferOnly=YES;
        CGColorSpaceRef space=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);layer.colorspace=space;CGColorSpaceRelease(space);
        NSString* source=@"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct V {float4 position [[position]]; float2 uv;};\n"
            "vertex V vertexMain(uint id [[vertex_id]]) {\n"
            "float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};\n"
            "V out;out.position=float4(p[id],0,1);out.uv=float2((p[id].x+1)*0.5,(1-p[id].y)*0.5);return out;}\n"
            "fragment float4 fragmentMain(V in [[stage_in]],texture2d<float> image [[texture(0)]]) {\n"
            "constexpr sampler s(coord::normalized,address::clamp_to_edge,filter::linear);return image.sample(s,in.uv);}";
        NSError* error=nil;id<MTLLibrary> library=[_device newLibraryWithSource:source options:nil error:&error];
        MTLRenderPipelineDescriptor* descriptor=[MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction=[library newFunctionWithName:@"vertexMain"];
        descriptor.fragmentFunction=[library newFunctionWithName:@"fragmentMain"];
        descriptor.colorAttachments[0].pixelFormat=layer.pixelFormat;
        _pipeline=[_device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if(!_pipeline)NSLog(@"LSX4 presentation pipeline failed: %@",error);
        currentGameView=self;
    }
    return self;
}
- (void)layoutSubviews {
    [super layoutSubviews];CAMetalLayer* layer=(CAMetalLayer*)self.layer;
    layer.contentsScale=self.window.screen.scale?:UIScreen.mainScreen.scale;
    layer.drawableSize=CGSizeMake(self.bounds.size.width*layer.contentsScale,self.bounds.size.height*layer.contentsScale);
}
- (BOOL)encodeFrame:(const Lsx4::Apple::GuestFrame&)frame
        completion:(std::shared_ptr<FrameCompletion>)completion {
    if(!self.window) {PresentationDetail("View has no window");return NO;}
    if(self.hidden) {PresentationDetail("View is hidden");return NO;}
    if(!_pipeline || !_queue) {PresentationDetail("Metal presentation pipeline unavailable");return NO;}
    if(UIApplication.sharedApplication.applicationState!=UIApplicationStateActive) {PresentationDetail("Application is not active");return NO;}
    CAMetalLayer* layer=(CAMetalLayer*)self.layer;id<CAMetalDrawable> drawable=[layer nextDrawable];if(!drawable){PresentationDetail("No drawable available");return NO;}
    MTLTextureDescriptor* descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:frame.width height:frame.height mipmapped:NO];
    descriptor.storageMode=MTLStorageModeShared;descriptor.usage=MTLTextureUsageShaderRead;
    id<MTLTexture> texture=[_device newTextureWithDescriptor:descriptor];if(!texture){PresentationDetail("Frame texture allocation failed");return NO;}
    [texture replaceRegion:MTLRegionMake2D(0,0,frame.width,frame.height) mipmapLevel:0 withBytes:frame.rgba.data() bytesPerRow:frame.width*4];
    id<MTLCommandBuffer> buffer=[_queue commandBuffer];if(!buffer){PresentationDetail("Command buffer unavailable");return NO;}
    MTLRenderPassDescriptor* pass=[MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture=drawable.texture;pass.colorAttachments[0].loadAction=MTLLoadActionClear;
    pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);
    id<MTLRenderCommandEncoder> encoder=[buffer renderCommandEncoderWithDescriptor:pass];if(!encoder){PresentationDetail("Render encoder unavailable");return NO;}
    const double scale=std::min(double(drawable.texture.width)/frame.width,double(drawable.texture.height)/frame.height);
    const double width=frame.width*scale,height=frame.height*scale;
    [encoder setViewport:MTLViewport{(drawable.texture.width-width)*0.5,(drawable.texture.height-height)*0.5,width,height,0,1}];
    [encoder setRenderPipelineState:_pipeline];[encoder setFragmentTexture:texture atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[encoder endEncoding];
    [drawable addPresentedHandler:^(id<MTLDrawable> presented) {
        const bool visible=presented.presentedTime>0;
        if(!completion->Finish(visible?1:2))return;
        // A retired drawable can legitimately be dropped by the compositor.
        // This releases its guest frame, but never counts as screen visibility.
        if(visible) {
            ++presentedFrames;PresentationDetail("Drawable displayed");
            {
                std::lock_guard lock(frameTimesMutex);
                frameTimes.push_back(presented.presentedTime);
                while(frameTimes.size()>2 && (frameTimes.size()>240 || frameTimes.front()<frameTimes.back()-2))frameTimes.pop_front();
            }
            dispatch_async(dispatch_get_main_queue(),^{[NSNotificationCenter.defaultCenter postNotificationName:@"LSX4FramePresented" object:nil];});
        } else {
            ++droppedFrames;PresentationDetail("Drawable retired without display");
        }
        dispatch_semaphore_signal(completion->semaphore);
    }];
    [buffer addCompletedHandler:^(id<MTLCommandBuffer> command) {
        if(command.status==MTLCommandBufferStatusError && completion->Finish(3)) {
            PresentationDetail(command.error.localizedDescription.UTF8String?:"Metal command failed");
            dispatch_semaphore_signal(completion->semaphore);
        }
    }];
    PresentationDetail("Frame submitted, waiting for display");
    [buffer presentDrawable:drawable];[buffer commit];return YES;
}
@end
@implementation LSX4Bridge (VideoOutput)
+ (NSString*)presentationDetail {std::lock_guard lock(presentationMutex);return [NSString stringWithUTF8String:presentationDetail.c_str()];}
#if DEBUG
+ (NSDictionary*)runVideoProbe {
    Lsx4::Apple::GuestFrame frame{320,180,std::vector<std::uint8_t>(320*180*4)};
    for(unsigned y=0;y<180;++y)for(unsigned x=0;x<320;++x) {
        auto* pixel=&frame.rgba[(y*320+x)*4];pixel[0]=x<160?180:20;pixel[1]=y<90?160:20;pixel[2]=80;pixel[3]=255;
    }
    const auto before=presentedFrames.load();
    const bool passed=Lsx4::Apple::PresentAppleFrame(frame) && presentedFrames.load()>before;
    return @{@"passed":@(passed),@"screenFramesPresented":@(presentedFrames.load()),@"detail":[self presentationDetail]};
}
#endif
+ (NSUInteger)presentedFrameCount {return presentedFrames.load();}
+ (NSUInteger)droppedFrameCount {return droppedFrames.load();}
+ (double)displayedFramesPerSecond {return DisplayedFramesPerSecond();}
+ (UIView*)makeVideoView {
    {std::lock_guard lock(frameTimesMutex);frameTimes.clear();}
    UIView* view=[[LSX4MetalGameView alloc] initWithFrame:CGRectZero];
    LSX4InstallSystemTextInput(view);
    return view;
}
@end
namespace Lsx4::Apple {
bool PresentAppleFrame(const GuestFrame& frame) {
    return PresentAppleFrameControlled(frame,nullptr);
}
bool PresentAppleFrameControlled(const GuestFrame& frame,const GuestRunControl* control) {
    if(NSThread.isMainThread || !frame.width || !frame.height || frame.width>16384 || frame.height>16384 ||
       frame.rgba.size()!=std::uint64_t(frame.width)*frame.height*4 || frame.rgba.size()>64ull*1024*1024)return false;
    @autoreleasepool {
        auto completion=std::make_shared<FrameCompletion>();
        __block BOOL submitted=NO;
        // Xcode's UI-test runner and system overlays can briefly deactivate
        // the game. Hold this owned frame until foreground rendering resumes;
        // do not retire it or report visibility while the app is inactive.
        const auto foregroundDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        for(;;) {
            if(control && control->IsCancelled())return false;
            __block BOOL inactive=NO;
            dispatch_sync(dispatch_get_main_queue(),^{
                LSX4MetalGameView* view=currentGameView;
                if(!view) {PresentationDetail("No game view registered");return;}
                if(UIApplication.sharedApplication.applicationState!=UIApplicationStateActive) {
                    inactive=YES;PresentationDetail("Waiting for foreground rendering");return;
                }
                submitted=[view encodeFrame:frame completion:completion];
            });
            if(!inactive)break;
            if(std::chrono::steady_clock::now()>=foregroundDeadline) {
                PresentationDetail("Foreground rendering did not resume within 30 seconds");return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if(!submitted)return false;
        const auto displayDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(dispatch_semaphore_wait(completion->semaphore,dispatch_time(DISPATCH_TIME_NOW,50*NSEC_PER_MSEC))) {
            if(control && control->IsCancelled()) {completion->Finish(4);return false;}
            if(std::chrono::steady_clock::now()>=displayDeadline) {
                completion->Finish(4);
                PresentationDetail("Timed out waiting for drawable presentation");return false;
            }
        }
        const auto result=completion->result.load();
#if DEBUG
        // A small live receipt lets Wi-Fi tests distinguish ongoing execution
        // from a retained image without attaching a second debugger.
        const auto retired=presentedFrames.load()+droppedFrames.load();
        static NSString* runID=NSProcessInfo.processInfo.environment[@"LSX4_TEST_RUN_ID"];
        static const bool progressEnabled=![NSProcessInfo.processInfo.environment[@"LSX4_FRAME_PROGRESS"] isEqualToString:@"0"];
        if(runID && progressEnabled && (result==1 || result==2) && (retired==1 || retired%30==0)) {
            NSDictionary* progress=@{@"runID":runID,@"screenFramesPresented":@(presentedFrames.load()),
                @"screenFramesDropped":@(droppedFrames.load()),@"displayedFramesPerSecond":@(DisplayedFramesPerSecond()),
                @"timestamp":@(NSDate.date.timeIntervalSince1970)};
            NSData* data=[NSJSONSerialization dataWithJSONObject:progress options:NSJSONWritingSortedKeys error:nil];
            NSURL* documents=[NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
            [data writeToURL:[documents URLByAppendingPathComponent:@"ios-frame-progress.json"] options:NSDataWritingAtomic error:nil];
        }
#endif
        return result==1 || result==2;
    }
}
}
