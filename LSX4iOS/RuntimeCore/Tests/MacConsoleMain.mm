// SPDX-License-Identifier: GPL-2.0-or-later
// Host diagnostic only; no iPhone performance claims.
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <vector>
#include <cmath>
#include <unistd.h>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include "../AppleHostServices.h"
#include "core/libraries/audio/audio_mix.h"
#include "AppleFrameCapture.h"
#include "MacInputTrace.h"
#include "AppleTextInputTrace.h"
#include "AppleMetalValidationState.h"
#include "AppleEmptyFragmentFixture.h"
#include "AppleRasterPrecisionFixture.h"
namespace Vulkan {
std::uint64_t ExecutorGetLivePresentCount() noexcept;
std::uint64_t ExecutorGetGuestFlipCount() noexcept;
}
int LSX4RunSharedCoreProbe(bool);
const char* LSX4SharedCoreProbeReport();
extern "C" int executor_lsx4_runtime_attach_metal_surface(void*,int,int);
extern "C" const char* executor_lsx4_runtime_gpu_residency_metrics();
int main(int argc,char** argv) {
    @autoreleasepool {
        if(!std::getenv("LSX4_PROBE_ROOT")) {
            std::fprintf(stderr,"Set LSX4_PROBE_ROOT to an isolated scratch directory\n");return 2;
        }
        NSWindow* window=nil;
        if(std::getenv("LSX4_CORE_GAME")) {
            [NSApplication sharedApplication];
            window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,960,540)
                styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
            window.title=@"LSX4 — проверка общего runtime на Mac";
            window.contentView.wantsLayer=YES;
            CAMetalLayer* layer=[CAMetalLayer layer];
            layer.device=MTLCreateSystemDefaultDevice();
            layer.pixelFormat=MTLPixelFormatBGRA8Unorm;
            layer.drawableSize=CGSizeMake(960,540);
            window.contentView.layer=layer;
            [window orderFront:nil];
            if(executor_lsx4_runtime_attach_metal_surface((__bridge void*)layer,960,540)!=0)return 2;
        }
        __block int result=1;
        unsigned duration=45;
        if(const char* setting=std::getenv("LSX4_PROBE_SECONDS")) {
            char* end=nullptr;const auto parsed=std::strtoul(setting,&end,10);
            if(!end || *end || parsed<1 || parsed>600) return 2;
            duration=static_cast<unsigned>(parsed);
        }
        std::vector<AppleTextTraceEvent> textEvents;
        if(!ReadAppleTextInputTrace(duration,textEvents)) {
            std::fprintf(stderr,"Invalid text input trace\n");return 2;
        }
        if(!ScheduleAppleInputTrace(duration)) {
            std::fprintf(stderr,"Invalid input trace\n");return 2;
        }
        ScheduleAppleTextInputTrace(textEvents);
        StartMacInputPipe();
        dispatch_source_t captureTimer=nullptr;
        if(const char* captureDirectory=std::getenv("LSX4_PROBE_CAPTURE_DIR")) {
            NSError* error=nil;
            if(![[NSFileManager defaultManager] createDirectoryAtPath:@(captureDirectory)
                withIntermediateDirectories:YES attributes:nil error:&error]) {
                std::fprintf(stderr,"Cannot create capture directory: %s\n",error.localizedDescription.UTF8String);
                return 2;
            }
            double begin=5, end=duration, interval=5;
            if(const char* setting=std::getenv("LSX4_PROBE_CAPTURE_WINDOW")) {
                std::istringstream fields(setting);std::string extra;
                if(!(fields>>begin>>end>>interval) || (fields>>extra) ||
                    !std::isfinite(begin) || !std::isfinite(end) || !std::isfinite(interval) ||
                    begin<0 || end<=begin || end>duration || interval<0.1) return 2;
            }
            const auto started=std::chrono::steady_clock::now();
            captureTimer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_main_queue());
            dispatch_source_set_timer(captureTimer,
                dispatch_time(DISPATCH_TIME_NOW,int64_t(begin*NSEC_PER_SEC)),
                uint64_t(interval*NSEC_PER_SEC),NSEC_PER_MSEC*10);
            dispatch_source_set_event_handler(captureTimer,^{
                const double elapsed=std::chrono::duration<double>(
                    std::chrono::steady_clock::now()-started).count();
                if(elapsed>=end) {
                    dispatch_source_cancel(captureTimer);
                    std::fprintf(stderr,"[APPLE_CAPTURE_WINDOW_END] seconds=%.3f\n",elapsed);
                    return;
                }
                Lsx4::Apple::RequestDiagnosticFrameCapture(CaptureAppleFrame);
            });
            std::fprintf(stderr,"[APPLE_CAPTURE_WINDOW] begin=%.3f end=%.3f interval=%.3f performanceRun=0\n",
                begin,end,interval);
            dispatch_resume(captureTimer);
        }
        // Two atomic counter reads every five seconds; no per-frame logging,
        // screenshots, GPU readbacks or full runtime-status serialization.
        __block auto previousTime=std::chrono::steady_clock::now();
        const auto observationStarted=previousTime;
        const bool traceMemoryOwners=std::getenv("LSX4_PROBE_MEMORY_OWNER_TRACE")!=nullptr;
        id<MTLDevice> memoryDevice=traceMemoryOwners?MTLCreateSystemDefaultDevice():nil;
        __block std::uint64_t previousPresents=0, previousFlips=0;
        dispatch_source_t counters=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,
                                                         dispatch_get_main_queue());
        dispatch_source_set_timer(counters,dispatch_time(DISPATCH_TIME_NOW,5*NSEC_PER_SEC),
                                  5*NSEC_PER_SEC,NSEC_PER_MSEC*100);
        dispatch_source_set_event_handler(counters,^{
            const auto now=std::chrono::steady_clock::now();
            const double seconds=std::chrono::duration<double>(now-previousTime).count();
            const auto presents=Vulkan::ExecutorGetLivePresentCount();
            const auto flips=Vulkan::ExecutorGetGuestFlipCount();
            std::fprintf(stderr,"[APPLE_HOST_COUNTERS] presents=%llu flips=%llu presentRate=%.2f flipRate=%.2f seconds=%.3f gameplayVerified=0\n",
                presents,flips,(presents-previousPresents)/seconds,(flips-previousFlips)/seconds,seconds);
            previousTime=now;previousPresents=presents;previousFlips=flips;
            if(traceMemoryOwners) {
                const auto queryStart=CACurrentMediaTime();
                task_vm_info_data_t memory{};mach_msg_type_number_t count=TASK_VM_INFO_COUNT;
                const auto memoryQuery=task_info(mach_task_self(),TASK_VM_INFO,
                    reinterpret_cast<task_info_t>(&memory),&count);
                malloc_statistics_t allocations{};malloc_zone_statistics(nullptr,&allocations);
                const char* gpu=executor_lsx4_runtime_gpu_residency_metrics();
                // Opt-in only; overlapping counters are not an additive breakdown.
                std::fprintf(stderr,"[APPLE_HOST_MEMORY] seconds=%.3f footprint=%llu mallocInUse=%zu "
                    "mallocAllocated=%zu metalAllocated=%llu memoryQuery=%d queryMilliseconds=%.3f gpu=%s\n",
                    std::chrono::duration<double>(now-observationStarted).count(),memory.phys_footprint,
                    allocations.size_in_use,allocations.size_allocated,
                    static_cast<unsigned long long>(memoryDevice.currentAllocatedSize),memoryQuery,
                    (CACurrentMediaTime()-queryStart)*1000,gpu);
            }
            if(std::getenv("LSX4_PROBE_AUDIO_STATS")) {
                const auto audio=Libraries::AudioOut::GetAudioOptimizationStats();
                std::fprintf(stderr,"[APPLE_AUDIO_COUNTERS] convertedFrames=%llu writeNanoseconds=%llu writeErrors=%llu partialWrites=%llu schedulerBatches=%llu guestCopyBytes=%llu\n",
                    audio.converted_frames,audio.write_nanoseconds,audio.write_errors,
                    audio.partial_writes,audio.scheduler_batches,audio.guest_copy_bytes);
            }
        });
        dispatch_resume(counters);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
            result=LSX4RunSharedCoreProbe(argc>1);
            std::printf("%s\n",LSX4SharedCoreProbeReport());std::fflush(stdout);
            CFRunLoopStop(CFRunLoopGetMain());
        });
        // This diagnostic is bounded even if guest code or module bootstrap
        // waits forever. Timeout is a failure/unfinished run, never a pass.
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,int64_t(duration)*NSEC_PER_SEC),
            dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
                // Do not fflush(nullptr): a blocking input FIFO may own its
                // FILE lock. The timeout must terminate independently of it.
                constexpr char message[]="LSX4_HOST_PROBE_TIMEOUT gameplay_not_verified=1\n";
                (void)::write(STDERR_FILENO,message,sizeof(message)-1);
                std::_Exit(124);
            });
        CFRunLoopRun();
        (void)window;
        // Android keeps the runtime DSO loaded; the iPhone fixture keeps its
        // application alive. This bounded host process checks execution only.
        // Static runtime shutdown ordering is a separate, still pending gate.
        std::fflush(stdout);std::fflush(stderr);
        std::_Exit(result);
    }
}
