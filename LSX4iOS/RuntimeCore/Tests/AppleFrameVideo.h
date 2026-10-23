// SPDX-License-Identifier: GPL-2.0-or-later
// Opt-in viewport diagnostics. No system screen capture, microphone or UI automation.
#pragma once
#import <AVFoundation/AVFoundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <atomic>
#include <mutex>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <cmath>

namespace AppleFrameVideo {
struct State {
    std::mutex mutex;
    std::atomic<bool> active{false};
    AVAssetWriter* writer{};
    AVAssetWriterInput* input{};
    AVAssetWriterInputPixelBufferAdaptor* adaptor{};
    id<MTLCommandQueue> queue{};
    id<MTLBuffer> staging{};
    NSString* directory{};
    NSString* failureReason{};
    NSUInteger width{}, height{}, stride{};
    double firstFrame{}, lastFrame{};
    unsigned frames{}, dropped{};
    bool fragmented{};
};
static State state;

static void WriteReport(NSString* status) {
    NSMutableDictionary* report=[@{@"status":status, @"frames":@(state.frames),
        @"droppedRequests":@(state.dropped), @"width":@(state.width),
        @"height":@(state.height), @"firstFrameMonotonic":@(state.firstFrame),
        @"lastFrameMonotonic":@(state.lastFrame),
        @"finishedUnix":@(NSDate.date.timeIntervalSince1970),
        @"includesAudio":@NO, @"includesOverlay":@NO, @"fpsAcceptanceEligible":@NO,
        @"fragmented":@(state.fragmented),
        @"writerStatus":@(state.writer ? state.writer.status : AVAssetWriterStatusUnknown)} mutableCopy];
    if(state.failureReason)report[@"failureReason"]=state.failureReason;
    if(NSError* error=state.writer.error) {
        report[@"writerError"]=@{@"domain":error.domain, @"code":@(error.code),
            @"description":error.description};
    }
    NSData* data=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil];
    [data writeToFile:[state.directory stringByAppendingPathComponent:@"recording.json"] atomically:YES];
    [report release];
    std::fprintf(stderr,"[APPLE_VIEWPORT_VIDEO] status=%s frames=%u dropped=%u performanceRun=0\n",
                 status.UTF8String,state.frames,state.dropped);
    if(state.failureReason || state.writer.error)
        std::fprintf(stderr,"[APPLE_VIEWPORT_VIDEO_ERROR] reason=%s writer=%s\n",
            state.failureReason ? state.failureReason.UTF8String : "none",
            state.writer.error ? state.writer.error.description.UTF8String : "none");
}

// Encoder failure is terminal for this recording. Stop scheduling GPU readbacks
// immediately rather than issuing waitIdle/blits for the rest of the window.
static void Fail(NSString* reason) {
    state.active.store(false,std::memory_order_release);
    [state.failureReason release];state.failureReason=[reason copy];
    WriteReport(@"failed");
}

static void Finish() {
    std::lock_guard lock(state.mutex);
    state.active.store(false,std::memory_order_release);
    [state.staging release];state.staging=nil;
    [state.queue release];state.queue=nil;
    if(state.writer && state.writer.status==AVAssetWriterStatusWriting) {
        [state.input markAsFinished];
        [state.writer finishWritingWithCompletionHandler:^{
            std::lock_guard completed(state.mutex);
            WriteReport(state.writer.status==AVAssetWriterStatusCompleted?@"completed":@"failed");
            [state.adaptor release];state.adaptor=nil;
            [state.input release];state.input=nil;
            [state.writer release];state.writer=nil;
        }];
    } else {
        WriteReport(state.writer || state.failureReason ? @"failed" : @"no_frames");
        [state.adaptor release];state.adaptor=nil;
        [state.input release];state.input=nil;
        [state.writer release];state.writer=nil;
    }
}

static void Capture(void* image) noexcept {
    if(!state.active.load(std::memory_order_acquire))return;
    @autoreleasepool {
        std::lock_guard lock(state.mutex);
        if(!state.active.load(std::memory_order_relaxed))return;
        CVPixelBufferRef pixel=nullptr;
        @try {
            id<MTLTexture> texture=nil;
            vkGetMTLTextureMVK(image,&texture);
            if(!texture || texture.framebufferOnly || !texture.width || !texture.height ||
               (texture.pixelFormat!=MTLPixelFormatBGRA8Unorm &&
                texture.pixelFormat!=MTLPixelFormatBGRA8Unorm_sRGB)) {
                ++state.dropped;return;
            }
            if(!state.writer) {
                state.width=texture.width;state.height=texture.height;
                state.stride=(state.width*4+255)&~NSUInteger(255);
                NSError* error=nil;
                NSURL* url=[NSURL fileURLWithPath:[state.directory stringByAppendingPathComponent:@"viewport.mp4"]];
                state.writer=[[AVAssetWriter alloc] initWithURL:url fileType:AVFileTypeMPEG4 error:&error];
                // Fragmented output remains diagnostic-only: the physical iPhone
                // writer failed with -16341 mid-recording in this mode. Ordinary
                // recordings use the previously working finalized-file path.
                const char* fragments=std::getenv("LSX4_PROBE_VIDEO_FRAGMENTS");
                state.fragmented=fragments && std::strcmp(fragments,"1")==0;
                if(state.fragmented) {
                    state.writer.movieFragmentInterval=CMTimeMake(5,1);
                    state.writer.initialMovieFragmentInterval=CMTimeMake(1,1);
                }
                NSDictionary* settings=@{AVVideoCodecKey:AVVideoCodecTypeH264,
                    AVVideoWidthKey:@(state.width), AVVideoHeightKey:@(state.height),
                    AVVideoColorPropertiesKey:@{
                        AVVideoColorPrimariesKey:AVVideoColorPrimaries_ITU_R_709_2,
                        AVVideoTransferFunctionKey:AVVideoTransferFunction_ITU_R_709_2,
                        AVVideoYCbCrMatrixKey:AVVideoYCbCrMatrix_ITU_R_709_2},
                    AVVideoCompressionPropertiesKey:@{AVVideoAverageBitRateKey:@6000000}};
                state.input=[[AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:settings] retain];
                state.input.expectsMediaDataInRealTime=YES;
                state.adaptor=[[AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:state.input
                    sourcePixelBufferAttributes:@{(NSString*)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA),
                    (NSString*)kCVPixelBufferWidthKey:@(state.width), (NSString*)kCVPixelBufferHeightKey:@(state.height),
                    (NSString*)kCVPixelBufferIOSurfacePropertiesKey:@{}}] retain];
                state.queue=[texture.device newCommandQueue];
                state.staging=[texture.device newBufferWithLength:state.stride*state.height options:MTLResourceStorageModeShared];
                if(!state.writer || !state.input || !state.adaptor || !state.queue || !state.staging ||
                   ![state.writer canAddInput:state.input]) {
                    Fail(error ? error.description : @"Writer resources or input unavailable");return;
                }
                [state.writer addInput:state.input];
                if(![state.writer startWriting]) {Fail(@"startWriting failed");return;}
                [state.writer startSessionAtSourceTime:kCMTimeZero];
            }
            if(state.writer.status!=AVAssetWriterStatusWriting) {
                Fail(@"Writer stopped accepting frames");return;
            }
            if(texture.width!=state.width || texture.height!=state.height || !state.input.readyForMoreMediaData) {
                ++state.dropped;return;
            }
            // A fixed pool threshold and backpressure bound memory; never queue
            // an unbounded backlog of full-resolution frames on a slow encoder.
            NSDictionary* limit=@{(NSString*)kCVPixelBufferPoolAllocationThresholdKey:@3};
            if(!state.adaptor.pixelBufferPool || CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(
                nullptr,state.adaptor.pixelBufferPool,(CFDictionaryRef)limit,&pixel)!=kCVReturnSuccess) {
                ++state.dropped;return;
            }
            id<MTLCommandBuffer> commands=[state.queue commandBuffer];
            id<MTLBlitCommandEncoder> blit=[commands blitCommandEncoder];
            if(!commands || !blit) {++state.dropped;return;}
            [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                sourceSize:MTLSizeMake(state.width,state.height,1) toBuffer:state.staging destinationOffset:0
                destinationBytesPerRow:state.stride destinationBytesPerImage:state.stride*state.height];
            [blit endEncoding];[commands commit];[commands waitUntilCompleted];
            if(commands.status!=MTLCommandBufferStatusCompleted) {++state.dropped;return;}
            if(CVPixelBufferLockBaseAddress(pixel,0)!=kCVReturnSuccess) {++state.dropped;return;}
            auto* dst=static_cast<unsigned char*>(CVPixelBufferGetBaseAddress(pixel));
            const auto dstStride=CVPixelBufferGetBytesPerRow(pixel);
            const auto* src=static_cast<const unsigned char*>(state.staging.contents);
            for(NSUInteger y=0;y<state.height;++y)std::memcpy(dst+y*dstStride,src+y*state.stride,state.width*4);
            CVPixelBufferUnlockBaseAddress(pixel,0);
            const double now=CACurrentMediaTime();
            if(!state.frames)state.firstFrame=now;
            if([state.adaptor appendPixelBuffer:pixel withPresentationTime:CMTimeMakeWithSeconds(now-state.firstFrame,60000)]) {
                ++state.frames;state.lastFrame=now;
            } else {
                ++state.dropped;
                if(state.writer.status!=AVAssetWriterStatusWriting)Fail(@"appendPixelBuffer failed");
            }
        } @catch(NSException* error) {
            Fail(error.description);
        } @finally {
            if(pixel)CVPixelBufferRelease(pixel);
        }
    }
}

static bool Schedule(unsigned seconds) {
    const char* directory=std::getenv("LSX4_PROBE_VIDEO_DIR");
    if(!directory)return true;
    if(std::getenv("LSX4_PROBE_CAPTURE_DIR"))return false;
    const char* window=std::getenv("LSX4_PROBE_VIDEO_WINDOW");
    if(!window)return false;
    double begin{},end{},fps{};std::string extra;std::istringstream fields(window);
    if(!(fields>>begin>>end>>fps) || fields>>extra || !std::isfinite(begin) ||
       !std::isfinite(end) || !std::isfinite(fps) || begin<0 || end<=begin ||
       end>seconds-5 || end-begin>180 || fps<1 || fps>15)return false;
    NSString* path=directory[0]=='/'?@(directory):[NSHomeDirectory() stringByAppendingPathComponent:@(directory)];
    if(![NSFileManager.defaultManager createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:nil])return false;
    state.directory=[path copy];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,int64_t(begin*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
        state.active.store(true,std::memory_order_release);
    });
    for(double at=begin;at<end;at+=1.0/fps) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,int64_t(at*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
            if(state.active.load(std::memory_order_acquire))Lsx4::Apple::RequestDiagnosticFrameCapture(Capture);
        });
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,int64_t(end*NSEC_PER_SEC)),dispatch_get_main_queue(),^{Finish();});
    return true;
}
} // namespace AppleFrameVideo
