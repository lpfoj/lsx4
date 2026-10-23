// SPDX-License-Identifier: GPL-2.0-or-later
#import <UIKit/UIKit.h>
#import <MetricKit/MetricKit.h>
#import "LSX4LaunchPlan.h"
extern "C" void LSX4LocalDetachDebugger();
extern "C" bool LSX4PrepareFexPool(size_t);
extern "C" bool LSX4WaitFexDetached();
extern "C" bool LSX4FexPreflightBeforePool();
#include <string>
#include <sys/sysctl.h>
#include <unistd.h>
#if defined(LSX4_FULL_CONSOLE_PROBE)
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import "LSX4Overlay-Swift.h"
#import "../AppleSystemTextInput.h"
#include "../AppleConsoleInput.h"
#include <array>
namespace Vulkan {
std::uint64_t ExecutorGetLivePresentCount() noexcept;
std::uint64_t ExecutorGetGuestFlipCount() noexcept;
}
#include "DeviceRunHarness.h"
#include "AppleMetalValidationState.h"
#include "AppleEmptyFragmentFixture.h"
#include "AppleRasterPrecisionFixture.h"
#include "AndroidRenderLaunch.h"
namespace AppleOutputResolution {
struct Profile { int outputWidth,outputHeight,displayMode; };
inline const Profile& Current() {
    // Frozen for this game session. Changes in Settings apply on next launch.
    static const Profile profile{int([LSX4RenderResolutionSettings width]),
        int([LSX4RenderResolutionSettings height]),AndroidDisplayScaling::FIT};
    return profile;
}
inline bool Enabled() { return true; }
inline NSString* Prepare() {
    if(NSString* error=[LSX4RenderResolutionSettings prepareForGame]) return error;
    setenv("LSX4_APPLE_RENDER_LAUNCH","on",1);
    return nil;
}
inline void RecordSurface(CAMetalLayer* layer,int attached) {
    static int lastWidth=0,lastHeight=0;
    const int width=int(layer.drawableSize.width),height=int(layer.drawableSize.height);
    if(width==lastWidth && height==lastHeight) return;
    lastWidth=width;lastHeight=height;
    NSString* path=[NSHomeDirectory() stringByAppendingPathComponent:@"Documents/render-resolution-status.json"];
    NSMutableDictionary* status=[[NSJSONSerialization JSONObjectWithData:
        [NSData dataWithContentsOfFile:path] ?: [NSData data] options:NSJSONReadingMutableContainers error:nil] mutableCopy];
    if(!status) status=[NSMutableDictionary new];
    status[@"drawableWidth"]=@(width);status[@"drawableHeight"]=@(height);
    status[@"contentsScale"]=@(layer.contentsScale);status[@"surfaceAttachResult"]=@(attached);
    status[@"surfaceBoundsWidth"]=@(layer.bounds.size.width);
    status[@"surfaceBoundsHeight"]=@(layer.bounds.size.height);
    status[@"stage"]=@"metal-surface-attached";
    [[NSJSONSerialization dataWithJSONObject:status options:NSJSONWritingSortedKeys error:nil] writeToFile:path atomically:YES];
    NSLog(@"[APPLE_RENDER_SURFACE] drawable=%dx%d layerBounds=%.0fx%.0f contentsScale=%.1f attach=%d",
        width,height,layer.bounds.size.width,layer.bounds.size.height,layer.contentsScale,attached);
}
}

extern "C" int executor_lsx4_runtime_attach_metal_surface(void*,int,int);
extern "C" int executor_lsx4_runtime_set_paused(int);
extern "C" int executor_lsx4_runtime_set_pad_axis(int,int);
extern "C" int executor_lsx4_runtime_set_touchpad(int,float,float);
@interface LSX4ProbeMetalView : UIView
@end
@implementation LSX4ProbeMetalView
+ (Class)layerClass { return CAMetalLayer.class; }
- (void)layoutSubviews {
    [super layoutSubviews];
    CAMetalLayer* layer=(CAMetalLayer*)self.layer;
    const CGFloat scale=self.window.screen.scale ?: UIScreen.mainScreen.scale;
    const auto& profile=AppleOutputResolution::Current();
    const bool fixed=AppleOutputResolution::Enabled() && profile.outputWidth>0 && profile.outputHeight>0;
    // MoltenVK exposes bounds * contentsScale as VkSurfaceCapabilities.currentExtent.
    // Keep the fixed Android surface in output-pixel coordinates; the view transform
    // performs the compositor scaling to Android's measured display rectangle.
    layer.contentsScale=fixed?1.0:scale;
    layer.drawableSize=fixed?CGSizeMake(profile.outputWidth,profile.outputHeight):
        CGSizeMake(self.bounds.size.width*scale,self.bounds.size.height*scale);
    const int attached=executor_lsx4_runtime_attach_metal_surface((__bridge void*)layer,
        (int)layer.drawableSize.width,(int)layer.drawableSize.height);
    AppleOutputResolution::RecordSurface(layer,attached);
}
@end

@interface LSX4ProbeGameController : UIViewController
@property(strong,nonatomic) UIView* videoView;
@property(strong,nonatomic) UILabel* counter;
@end
@implementation LSX4ProbeGameController
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
- (UIInterfaceOrientation)preferredInterfaceOrientationForPresentation { return UIInterfaceOrientationLandscapeRight; }
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self setNeedsUpdateOfSupportedInterfaceOrientations];
    if(self.view.window.windowScene) {
        UIWindowSceneGeometryPreferencesIOS* geometry=[[UIWindowSceneGeometryPreferencesIOS alloc]
            initWithInterfaceOrientations:UIInterfaceOrientationMaskLandscapeRight];
        [self.view.window.windowScene requestGeometryUpdateWithPreferences:geometry errorHandler:^(NSError* error) {
            NSLog(@"LSX4 landscape request: %@",error.localizedDescription);
        }];
    }
}
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    if(!self.videoView)return;
    CGRect videoFrame=self.view.bounds;
    if(AppleOutputResolution::Enabled()) {
        const auto& profile=AppleOutputResolution::Current();
        const CGFloat scale=self.view.window.screen.scale ?: UIScreen.mainScreen.scale;
        const int width=(int)std::lround(videoFrame.size.width*scale);
        const int height=(int)std::lround(videoFrame.size.height*scale);
        const auto measured=AndroidDisplayScaling::measure(width,height,
            profile.outputWidth,profile.outputHeight,profile.displayMode,
            profile.outputWidth>0 && profile.outputHeight>0);
        videoFrame=CGRectMake(CGRectGetMidX(videoFrame)-measured.width/scale/2,
            CGRectGetMidY(videoFrame)-measured.height/scale/2,measured.width/scale,measured.height/scale);
        self.view.clipsToBounds=YES;
    }
    const auto& profile=AppleOutputResolution::Current();
    const bool fixed=AppleOutputResolution::Enabled() && profile.outputWidth>0 && profile.outputHeight>0;
    self.videoView.transform=CGAffineTransformIdentity;
    if(fixed) {
        self.videoView.bounds=CGRectMake(0,0,profile.outputWidth,profile.outputHeight);
        self.videoView.center=CGPointMake(CGRectGetMidX(videoFrame),CGRectGetMidY(videoFrame));
        self.videoView.transform=CGAffineTransformMakeScale(
            videoFrame.size.width/profile.outputWidth,videoFrame.size.height/profile.outputHeight);
    } else {
        self.videoView.frame=videoFrame;
    }
    CGRect safe=UIEdgeInsetsInsetRect(self.view.bounds,self.view.safeAreaInsets);
    self.counter.frame=CGRectMake(CGRectGetMidX(safe)-105,CGRectGetMinY(safe)+4,210,24);
}
@end
#endif
int LSX4RunSharedCoreProbe(bool interpreter);
const char* LSX4SharedCoreProbeReport();
extern "C" __attribute__((noinline,used,visibility("default")))
void LSX4RuntimeProbeReportReady() { asm volatile("" ::: "memory"); }

@interface LSX4RuntimeProbeDelegate : UIResponder <UIApplicationDelegate>
@property(strong,nonatomic) UIWindow* window;
#if defined(LSX4_FULL_CONSOLE_PROBE)
@property(strong,nonatomic) LSX4DualShockOverlayView* controls;
@property(strong,nonatomic) NSTimer* frameTimer;
@property(nonatomic) BOOL lifecyclePaused;
#endif
@end
@interface LSX4RuntimeProbeDelegate ()
@property(copy,nonatomic) NSString* localGameTitle;
- (void)startConsole;
@end
@implementation LSX4RuntimeProbeDelegate
- (BOOL)application:(UIApplication*)app didFinishLaunchingWithOptions:(NSDictionary*)options {
    app.idleTimerDisabled=YES;
    if(const char* language=std::getenv("LSX4_UI_LANGUAGE");language && std::strcmp(language,"en")==0) {
        [NSUserDefaults.standardUserDefaults setObject:@"en" forKey:@"ui_language"];
        [NSUserDefaults.standardUserDefaults setObject:@[@"en"] forKey:@"AppleLanguages"];
    }
    [LSX4ScreencastRecorder beginIfRequested];
    if(const char* mode=std::getenv("LSX4_EXECUTION_MODE"); mode &&
       std::strcmp(mode,"jit")==0) {
        [NSUserDefaults.standardUserDefaults setObject:@(mode) forKey:@"lsx4.executionMode"];
    }
    self.window=[[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
#if defined(LSX4_DEVICE_LOCAL_JIT)
    [LSX4LibraryControllerFactory setLocalLaunchHandler:^NSString*(NSString* title) {
        NSRegularExpression* identifier=[NSRegularExpression regularExpressionWithPattern:@"^CUSA[0-9]{5}$" options:0 error:nil];
        if([identifier numberOfMatchesInString:title options:0 range:NSMakeRange(0,title.length)]!=1) return @"Invalid game identifier.";
        if(self.localGameTitle) return @"Close this session and reopen the app to launch another game.";
        NSDictionary* launch=LSX4ResolveLaunchPlan(title,NSBundle.mainBundle.bundleURL);
        if(!launch) return @"Could not load the default launch configuration. Reinstall this version over the existing app.";
        NSString* documents=[NSHomeDirectory() stringByAppendingPathComponent:@"Documents"];
        NSString* relative=[@"Games" stringByAppendingPathComponent:[title stringByAppendingPathComponent:@"eboot.bin"]];
        if(![NSFileManager.defaultManager fileExistsAtPath:[documents stringByAppendingPathComponent:relative]]) {
            relative=[title stringByAppendingPathComponent:@"eboot.bin"];
            if(![NSFileManager.defaultManager fileExistsAtPath:[documents stringByAppendingPathComponent:relative]]) return @"Game files were not found on iPhone.";
        }
        NSData* profile=launch[@"cpu"];
        NSString* destination=[documents stringByAppendingPathComponent:@"shared-runtime/runtime-config"];
        NSError* error=nil;
        [NSFileManager.defaultManager createDirectoryAtPath:destination withIntermediateDirectories:YES attributes:nil error:&error];
        if(error || !profile || ![profile writeToFile:[destination stringByAppendingPathComponent:@"android-cpu-tuning-v1.ini"] atomically:YES]) return @"Could not prepare the game profile.";
        NSDictionary* receipt=@{@"title":title,@"genericProfile":launch[@"generic"],@"version":[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"] ?: @"",@"stage":@"configured"};
        [[NSJSONSerialization dataWithJSONObject:receipt options:NSJSONWritingSortedKeys error:nil] writeToFile:[documents stringByAppendingPathComponent:@"local-game-launch.json"] atomically:YES];
        NSDictionary* environment=launch[@"environment"];
        for(NSString* key in environment) setenv(key.UTF8String,[environment[key] UTF8String],1);
        setenv("LSX4_CORE_GAME",relative.UTF8String,1);setenv("LSX4_CORE_GAME_STAGE","run",1);
        setenv("LSX4_JIT_PROVIDER","local",1);unsetenv("LSX4_CORE_KEEP_AWAKE");
        if(NSString* failure=AppleOutputResolution::Prepare()) return failure;
        self.localGameTitle=title;
        [LSX4LocalJITProvider begin:^(NSString* failure) { if(failure) NSLog(@"[LSX4LocalJIT] %@",failure); }];
        [self startConsole]; return nil;
    }];
#endif

#if defined(LSX4_FULL_CONSOLE_PROBE)
    if(!NSProcessInfo.processInfo.environment[@"LSX4_CORE_GAME"] &&
       !NSProcessInfo.processInfo.environment[@"LSX4_CORE_FIXTURE"] &&
       YES) {
        self.window.rootViewController=[LSX4LibraryControllerFactory makeController];
        [self.window makeKeyAndVisible];
        if(NSProcessInfo.processInfo.environment[@"LSX4_LIBRARY_SCREENSHOT"]) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,2*NSEC_PER_SEC),dispatch_get_main_queue(),^{
                UIGraphicsImageRenderer* renderer=[[UIGraphicsImageRenderer alloc] initWithSize:self.window.bounds.size];
                UIImage* shot=[renderer imageWithActions:^(UIGraphicsImageRendererContext*) {
                    [self.window drawViewHierarchyInRect:self.window.bounds afterScreenUpdates:YES];
                }];
                [UIImagePNGRepresentation(shot) writeToFile:[NSHomeDirectory() stringByAppendingPathComponent:@"Documents/library-screen.png"] atomically:YES];
            });
        }
        return YES;
    }
#endif
    [self startConsole];return YES;
}
- (void)startConsole {
#if defined(LSX4_FULL_CONSOLE_PROBE)
    if(!self.localGameTitle) {
        if(NSString* failure=AppleOutputResolution::Prepare()) {
            NSLog(@"[APPLE_RENDER_RESOLUTION_ERROR] %@",failure);
            UIAlertController* alert=[UIAlertController alertControllerWithTitle:@"LSX4" message:failure preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
            [self.window.rootViewController presentViewController:alert animated:YES completion:nil];
            return;
        }
    }
    LSX4ProbeGameController* controller=[LSX4ProbeGameController new];
#else
    UIViewController* controller=[UIViewController new];
#endif
    controller.view.backgroundColor=UIColor.systemBackgroundColor;
    UILabel* label=[UILabel new];
    label.translatesAutoresizingMaskIntoConstraints=NO;
    label.numberOfLines=0;
    label.textAlignment=NSTextAlignmentCenter;
    label.lineBreakMode=NSLineBreakByWordWrapping;
    label.font=[UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    label.adjustsFontForContentSizeCategory=YES;
    label.accessibilityIdentifier=@"launch-preparation-status";
    label.text=@"LSX4\nPreparing on-device JIT…";
    [controller.view addSubview:label];
    UILayoutGuide* safe=controller.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [label.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:16],
        [label.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-16],
        [label.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor],
        [label.topAnchor constraintGreaterThanOrEqualToAnchor:safe.topAnchor constant:16],
        [label.bottomAnchor constraintLessThanOrEqualToAnchor:safe.bottomAnchor constant:-16]
    ]];
    self.window.rootViewController=controller;
    [self.window makeKeyAndVisible];
    if ([NSProcessInfo.processInfo.environment[@"LSX4_CORE_KEEP_AWAKE"] isEqualToString:@"1"]) {
        label.text=@"LSX4\nDevelopment session is active.";
        return;
    }
#if defined(LSX4_FULL_CONSOLE_PROBE)
    if (std::getenv("LSX4_CORE_GAME")) {
        LSX4ProbeMetalView* video=[[LSX4ProbeMetalView alloc] initWithFrame:controller.view.bounds];
        controller.view.backgroundColor=UIColor.blackColor;
        label.textColor=UIColor.whiteColor;
        controller.videoView=video;
        LSX4InstallSystemTextInput(controller.view);
        CAMetalLayer* layer=(CAMetalLayer*)video.layer;
        layer.device=MTLCreateSystemDefaultDevice();
        layer.pixelFormat=MTLPixelFormatBGRA8Unorm;
        [controller.view insertSubview:video belowSubview:label];
        [controller.view setNeedsLayout];[controller.view layoutIfNeeded];
        self.controls=[[LSX4DualShockOverlayView alloc] initWithFrame:controller.view.bounds];
        __block uint32_t previousMask=0;
        __block std::array<uint8_t,6> previousAxes={128,128,128,128,0,0};
        __block BOOL previousTouch=NO;
        self.controls.inputHandler=^(uint32_t mask,NSData* axes,BOOL connected,uint64_t timestamp,
                BOOL touchActive,uint16_t touchX,uint16_t touchY) {
            const uint32_t buttons=(connected?mask:0)&~0x100000u;
            const uint32_t released=previousMask&~buttons,pressed=buttons&~previousMask;
            if(released) executor_lsx4_apple_set_pad_button(released,0);
            if(pressed) executor_lsx4_apple_set_pad_button(pressed,1);
            previousMask=buttons;
            if(axes.length==6) {
                const auto* values=static_cast<const uint8_t*>(axes.bytes);
                for(int axis=0;axis<6;++axis) {
                    const uint8_t value=connected?values[axis]:(axis<4?128:0);
                    if(value!=previousAxes[axis]) executor_lsx4_runtime_set_pad_axis(axis,value);
                    previousAxes[axis]=value;
                }
            }
            const BOOL touching=connected&&touchActive;
            if(touching||previousTouch) executor_lsx4_runtime_set_touchpad(touching,touchX/1919.f,touchY/949.f);
            previousTouch=touching;
        };
        [self.controls setInputReady:NO];
        self.controls.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
        [controller.view addSubview:self.controls];
        controller.counter=[UILabel new];
        controller.counter.textColor=UIColor.whiteColor;
        controller.counter.textAlignment=NSTextAlignmentCenter;
        controller.counter.font=[UIFont monospacedDigitSystemFontOfSize:12 weight:UIFontWeightMedium];
        controller.counter.backgroundColor=[UIColor.blackColor colorWithAlphaComponent:0.45];
        controller.counter.layer.cornerRadius=6;controller.counter.clipsToBounds=YES;
        controller.counter.userInteractionEnabled=NO;
        [controller.view addSubview:controller.counter];
        [controller.view setNeedsLayout];
        __block uint64_t previousFlips=Vulkan::ExecutorGetGuestFlipCount();
        __block CFTimeInterval previousTime=CACurrentMediaTime();
        __block BOOL imageStarted=NO;
        self.frameTimer=[NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer*) {
            const uint64_t flips=Vulkan::ExecutorGetGuestFlipCount();
            const CFTimeInterval now=CACurrentMediaTime();
            if(Vulkan::ExecutorGetLivePresentCount()>0) {
                if(!imageStarted) {
                    imageStarted=YES;
                    label.hidden=YES;
                    [self.controls setInputReady:YES];
                }
                const double fps=(flips-previousFlips)/(now-previousTime);
                const BOOL showMode=[NSUserDefaults.standardUserDefaults boolForKey:@"lsx4.showExecutionMode"];
                NSString* mode=@"JIT";
                controller.counter.text=showMode
                    ? [NSString stringWithFormat:@"LSX4 · %@ · %.1f FPS",mode,fps]
                    : [NSString stringWithFormat:@"LSX4 · %.1f FPS",fps];
            }
            previousFlips=flips;previousTime=now;
        }];
        // Controls track simultaneous touches; sampling continues while UIKit
        // is in its tracking run-loop mode. No screenshots or status dumps.
        [NSRunLoop.mainRunLoop addTimer:self.frameTimer forMode:NSRunLoopCommonModes];
    }
#endif
    const BOOL interpreter=NO;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
        std::string startupError="Debugger attach timed out";
        BOOL ready=interpreter;
        for(int attempt=0;!ready && attempt<600;++attempt) {
            int mib[]={CTL_KERN,KERN_PROC,KERN_PROC_PID,getpid()};
            struct kinfo_proc process{}; size_t size=sizeof(process);
            ready=sysctl(mib,4,&process,&size,nullptr,0)==0 && (process.kp_proc.p_flag&P_TRACED);
#if defined(LSX4_DEVICE_LOCAL_JIT)
            if(NSString* failure=[LSX4LocalJITProvider lastFailure]) {
                startupError=failure.UTF8String;ready=NO;break;
            }
#endif
            if(!ready) [NSThread sleepForTimeInterval:0.5];
        }
#if defined(LSX4_FULL_CONSOLE_PROBE)
        if(ready && std::getenv("LSX4_APPLE_CPU_BACKEND") && std::strcmp(std::getenv("LSX4_APPLE_CPU_BACKEND"),"fex")==0) {
            ready=LSX4FexPreflightBeforePool();
            if(!ready)startupError="FEX preflight failed; see probe-stderr.log";
        }
        if(ready) {
            if(const char* pool=std::getenv("LSX4_PROBE_PREPARED_POOL_MIB")) {
                char* end=nullptr;const auto mib=std::strtoul(pool,&end,10);std::string error;
                ready=end && !*end && mib>=4 && mib<=Lsx4::Apple::ExecutablePoolLimitBytes/(1024*1024) && mib%4==0 &&
                    (std::getenv("LSX4_APPLE_CPU_BACKEND") && std::strcmp(std::getenv("LSX4_APPLE_CPU_BACKEND"),"fex")==0
                        ? (mib >= 256 && LSX4PrepareFexPool((mib-64)*1024*1024) && Lsx4::Apple::PrepareExecutableArenaPool(64*1024*1024,error))
                        : Lsx4::Apple::PrepareExecutableArenaPool(mib*1024*1024,error));
                if(ready) {
                    LSX4RuntimeProbePreparedPoolReady();
#if defined(LSX4_DEVICE_LOCAL_JIT)
                    if(const char* provider=std::getenv("LSX4_JIT_PROVIDER");provider && std::strcmp(provider,"local")==0) {
                        LSX4LocalDetachDebugger();
                        // Current and FEX must both wait for actual detach before execution.
                        for(int attempt=0;attempt<100;++attempt) {
                            int query[]={CTL_KERN,KERN_PROC,KERN_PROC_PID,getpid()};
                            struct kinfo_proc p{};size_t length=sizeof(p);
                            if(sysctl(query,4,&p,&length,nullptr,0)==0 && !(p.kp_proc.p_flag&P_TRACED))break;
                            if(attempt==99) { ready=NO;startupError="Local JIT debugger did not detach"; }
                            [NSThread sleepForTimeInterval:0.05];
                        }
                    }
#endif
                    if(std::getenv("LSX4_APPLE_CPU_BACKEND") && std::strcmp(std::getenv("LSX4_APPLE_CPU_BACKEND"),"fex")==0)
                        ready=LSX4WaitFexDetached();
                }
                else {
                    startupError="JIT pool preparation failed: "+(error.empty()?std::string("invalid pool geometry"):error);
                    std::fprintf(stderr,"[APPLE_PREPARED_POOL_FAILED] %s\n",startupError.c_str());
                }
            }
        }
        if(ready && NSProcessInfo.processInfo.environment[@"LSX4_PROBE_SECONDS"]) {
            __block BOOL scheduled=NO;
            dispatch_sync(dispatch_get_main_queue(), ^{ scheduled=StartDeviceTimedRun(); });
            ready=scheduled;
            if(!ready) startupError="Timed observation setup failed";
        }
#endif
        int result=ready?LSX4RunSharedCoreProbe(interpreter):1;
        NSString* detail=ready?@(LSX4SharedCoreProbeReport()):@(startupError.c_str());
        NSDictionary* report=@{@"passed":@(result==0),@"interpreter":@(interpreter),
            @"detail":detail,@"gameplayTested":@NO,@"timestamp":@(NSDate.date.timeIntervalSince1970)};
        NSURL* documents=[NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
        NSData* json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil];
        [json writeToURL:[documents URLByAppendingPathComponent:@"shared-core-probe.json"] atomically:YES];
        dispatch_async(dispatch_get_main_queue(), ^{
#if defined(LSX4_FULL_CONSOLE_PROBE)
            [self.frameTimer invalidate];
            self.frameTimer=nil;
            [self.controls setInputReady:NO];
#endif
            label.text=detail;label.hidden=NO;
        });
        LSX4RuntimeProbeReportReady();
    });
    return;
}
- (void)applicationDidBecomeActive:(UIApplication*)app {
    app.idleTimerDisabled=YES;
#if defined(LSX4_FULL_CONSOLE_PROBE)
    if(self.controls) {
        int result=0;
        if(self.lifecyclePaused) {
            result=executor_lsx4_runtime_set_paused(0);
            if(result==0) self.lifecyclePaused=NO;
        }
        std::fprintf(stderr,"[APPLE_LIFECYCLE] active=1 resumeResult=%d\n",result);
    }
#endif
}
- (void)applicationWillResignActive:(UIApplication*)app {
#if defined(LSX4_FULL_CONSOLE_PROBE)
    [self.controls releaseAllButtons];
    if(self.controls) {
        // Use the same guest-thread/audio pause owner as Android. GPU queue
        // quiescence across background transitions is verified separately;
        // pausing CPU producers alone is not proof of a safe GPU drain.
        int result=0;
        const bool started=Vulkan::ExecutorGetGuestFlipCount()>0;
        if(started && !self.lifecyclePaused) {
            result=executor_lsx4_runtime_set_paused(1);
            if(result==0) self.lifecyclePaused=YES;
        }
        std::fprintf(stderr,"[APPLE_LIFECYCLE] active=0 guestStarted=%d pauseResult=%d\n",started,result);
    }
#endif
}
@end
int main(int argc,char** argv) {
    @autoreleasepool { return UIApplicationMain(argc,argv,nil,NSStringFromClass(LSX4RuntimeProbeDelegate.class)); }
}
