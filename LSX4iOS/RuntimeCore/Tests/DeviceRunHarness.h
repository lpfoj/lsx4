// SPDX-License-Identifier: GPL-2.0-or-later
// Explicit bounded device diagnostics, disabled in ordinary app operation.
#include "AppleInputTrace.h"
#include "AppleTextInputTrace.h"
#include "../AppleHostServices.h"
#include "AppleFrameCapture.h"
#include "AppleFrameVideo.h"
#include "AppleLiveControls.h"
#include "AppleCodeMemory.h"
#include "core/libraries/audio/audio_mix.h"
#include <mach/mach.h>
#include <malloc/malloc.h>
#import <Metal/Metal.h>
extern "C" const char* executor_lsx4_runtime_gpu_residency_metrics();
#if defined(LSX4_APPLE_FEX)
extern "C" size_t LSX4FexPreparedPoolCapacityBytes();
extern "C" size_t LSX4FexPreparedPoolConsumedBytes();
#endif
extern "C" __attribute__((noinline,used,visibility("default")))
void LSX4RuntimeProbeTimedRunReady() { asm volatile("" ::: "memory"); }
extern "C" __attribute__((noinline,used,visibility("default")))
void LSX4RuntimeProbeObservationReady() { asm volatile("" ::: "memory"); }
extern "C" __attribute__((noinline,used,visibility("default")))
void LSX4RuntimeProbePreparedPoolReady() { asm volatile("" ::: "memory"); }

static bool StartDeviceTimedRun() {
    const char* setting=std::getenv("LSX4_PROBE_SECONDS");
    if(!setting) return true;
    char* end=nullptr;const auto seconds=std::strtoul(setting,&end,10);
    if(!end || *end || seconds<5 || seconds>600) return false;
    if(const char* trace=std::getenv("LSX4_PROBE_INPUT_TRACE"); trace && trace[0]!='/') {
        NSString* path=[NSHomeDirectory() stringByAppendingPathComponent:@(trace)];
        setenv("LSX4_PROBE_INPUT_TRACE",path.fileSystemRepresentation,1);
    }
    if(const char* trace=std::getenv("LSX4_PROBE_TEXT_INPUT_TRACE"); trace && trace[0]!='/') {
        NSString* path=[NSHomeDirectory() stringByAppendingPathComponent:@(trace)];
        setenv("LSX4_PROBE_TEXT_INPUT_TRACE",path.fileSystemRepresentation,1);
    }
    std::vector<AppleTextTraceEvent> textEvents;
    if(!ReadAppleTextInputTrace(static_cast<unsigned>(seconds),textEvents))return false;
    if(!ScheduleAppleInputTrace(static_cast<unsigned>(seconds))) return false;
    ScheduleAppleTextInputTrace(textEvents);
    if(!AppleFrameVideo::Schedule(static_cast<unsigned>(seconds)))return false;
    NSURL* documents=[NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
    const CFTimeInterval started=CACurrentMediaTime();
    if(const char* detach=std::getenv("LSX4_PROBE_DETACH_SECONDS")) {
        char* stop=nullptr;const auto when=std::strtoul(detach,&stop,10);
        if(!stop || *stop || when<5 || when>=seconds) return false;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,when*NSEC_PER_SEC),dispatch_get_main_queue(),^{
            LSX4RuntimeProbeObservationReady();
        });
    }
    if(const char* capture=std::getenv("LSX4_PROBE_CAPTURE_DIR")) {
        NSString* path=capture[0]=='/'?@(capture):[NSHomeDirectory() stringByAppendingPathComponent:@(capture)];
        setenv("LSX4_PROBE_CAPTURE_DIR",path.fileSystemRepresentation,1);
        if(![NSFileManager.defaultManager createDirectoryAtPath:path withIntermediateDirectories:YES
            attributes:nil error:nil]) return false;
        double begin{},finish{},interval{};std::string extra;
        const char* window=std::getenv("LSX4_PROBE_CAPTURE_WINDOW");
        if(!window) return false;
        std::istringstream fields(window);
        if(!(fields>>begin>>finish>>interval) || (fields>>extra) ||
            !std::isfinite(begin) || !std::isfinite(finish) || !std::isfinite(interval) ||
            begin<0 || finish<=begin || finish>seconds || interval<1) return false;
        for(double when=begin;when<finish;when+=interval) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,int64_t(when*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
                Lsx4::Apple::RequestDiagnosticFrameCapture(CaptureAppleFrame);
            });
        }
    }
    if(!StartAppleLiveControls(static_cast<unsigned>(seconds)))return false;
    __block NSMutableArray* samples=[NSMutableArray new];
    const bool traceProgress=std::getenv("LSX4_PROBE_PROGRESS_TRACE")!=nullptr;
    const bool traceMemoryOwners=std::getenv("LSX4_PROBE_MEMORY_OWNER_TRACE")!=nullptr;
    id<MTLDevice> memoryDevice=traceMemoryOwners?MTLCreateSystemDefaultDevice():nil;
    dispatch_source_t timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_main_queue());
    dispatch_source_set_timer(timer,dispatch_time(DISPATCH_TIME_NOW,5*NSEC_PER_SEC),
                              5*NSEC_PER_SEC,100*NSEC_PER_MSEC);
    dispatch_source_set_event_handler(timer,^{
        const auto sampleQueryStart=CACurrentMediaTime();
        task_vm_info_data_t memory{};mach_msg_type_number_t count=TASK_VM_INFO_COUNT;
        const auto memoryQuery=task_info(mach_task_self(),TASK_VM_INFO,reinterpret_cast<task_info_t>(&memory),&count);
        const auto audio=Libraries::AudioOut::GetAudioOptimizationStats();
        NSMutableDictionary* measuredSample=[@{@"seconds":@(CACurrentMediaTime()-started),
            @"presents":@(Vulkan::ExecutorGetLivePresentCount()),
            @"flips":@(Vulkan::ExecutorGetGuestFlipCount()),@"footprint":@(memory.phys_footprint),
            @"memoryLimitBytesRemaining":@(memory.limit_bytes_remaining),
            @"virtualAddressLimit":@(memory.max_address),
            @"virtualBytes":@(memory.virtual_size),
            @"memoryQueryResult":@(memoryQuery),@"memoryInfoCount":@(count),
            @"preparedCodeClaimedBytes":@(Lsx4::Apple::PreparedExecutableArenaPoolClaimedBytes()),
            @"audioFrames":@(audio.converted_frames),@"audioErrors":@(audio.write_errors),
            @"audioPartialWrites":@(audio.partial_writes),
            // One OS state snapshot per five-second sample; no thermal polling
            // thread or per-frame work. Preserve it beside FPS to distinguish
            // runtime regressions from device power/thermal restrictions.
            @"thermalState":@(NSProcessInfo.processInfo.thermalState),
            @"lowPowerModeEnabled":@(NSProcessInfo.processInfo.lowPowerModeEnabled)} mutableCopy];
#if defined(LSX4_APPLE_FEX)
        measuredSample[@"fexPreparedCodeCapacityBytes"]=@(LSX4FexPreparedPoolCapacityBytes());
        measuredSample[@"fexPreparedCodeConsumedBytes"]=@(LSX4FexPreparedPoolConsumedBytes());
#endif
        measuredSample[@"sampleQueryMilliseconds"]=@((CACurrentMediaTime()-sampleQueryStart)*1000);
        [samples addObject:measuredSample];
        // Preserve the existing five-second sample when iOS terminates the
        // process before the final report. Opt-in only; no extra memory query.
        if(traceProgress || traceMemoryOwners) {
            NSMutableDictionary* sample=[samples.lastObject mutableCopy];
            if(traceMemoryOwners) {
                // Diagnostic only. These ownership counters overlap with the
                // process footprint and must not be added together.
                const auto queryStart=CACurrentMediaTime();
                malloc_statistics_t stats{};
                malloc_zone_statistics(nullptr,&stats);
                sample[@"mallocBytesInUse"]=@(stats.size_in_use);
                sample[@"mallocBytesAllocated"]=@(stats.size_allocated);
                sample[@"metalDefaultDeviceAvailable"]=@(memoryDevice!=nil);
                sample[@"metalDefaultDeviceAllocatedBytes"]=@(memoryDevice.currentAllocatedSize);
                // Read the common renderer's published snapshot, without
                // walking Vulkan allocations or waiting for the render thread.
                const char* gpu=executor_lsx4_runtime_gpu_residency_metrics();
                NSData* gpuData=[NSData dataWithBytes:gpu length:std::strlen(gpu)];
                id gpuSnapshot=[NSJSONSerialization JSONObjectWithData:gpuData options:0 error:nil];
                if(gpuSnapshot) sample[@"gpuResidency"]=gpuSnapshot;
                sample[@"ownerQueryMilliseconds"]=@((CACurrentMediaTime()-queryStart)*1000);
            }
            NSData* json=[NSJSONSerialization dataWithJSONObject:sample options:0 error:nil];
            std::fprintf(stderr,"[APPLE_DEVICE_SAMPLE] %.*s\n",int(json.length),
                         static_cast<const char*>(json.bytes));
        }
    });
    dispatch_resume(timer);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,seconds*NSEC_PER_SEC),dispatch_get_main_queue(),^{
        dispatch_source_cancel(timer);
        // An attended run hands control back to the existing overlay. Do not
        // cancel a finger the user is still holding at the sampling boundary.
        if(!std::getenv("LSX4_PROBE_LEAVE_RUNNING")) {
            executor_lsx4_apple_set_pad_button(LSX4_APPLE_PAD_BUTTON_MASK,0);
            for(int axis=0;axis<6;++axis)
                executor_lsx4_runtime_set_pad_axis(axis,axis<4?128:0);
        }
        NSDictionary* report=@{@"timestamp":@(NSDate.date.timeIntervalSince1970),
            @"timedRunCompleted":@YES,@"gameplayVerified":@NO,
            @"idleTimerDisabled":@(UIApplication.sharedApplication.idleTimerDisabled),@"samples":samples};
        NSData* json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil];
        [json writeToURL:[documents URLByAppendingPathComponent:@"device-timed-run.json"] atomically:YES];
        // The host collects the report, then either leaves manual play active
        // or restarts this app in awake mode. Guest teardown is a separate gate.
        LSX4RuntimeProbeTimedRunReady();
    });
    return true;
}
