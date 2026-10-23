// SPDX-License-Identifier: GPL-2.0-or-later
// Finite opt-in test controller. No polling in ordinary gameplay.
#pragma once
#include "AppleLiveControlProtocol.h"
#include "AppleInputTrace.h"
#include "../AppleHostServices.h"
#include <cstring>
#include <memory>
static void CaptureAppleFrame(void*) noexcept;

static bool StartAppleLiveControls(unsigned duration) {
    if(!std::getenv("LSX4_PROBE_LIVE_INPUT")) return true;
    const char* identity=std::getenv("LSX4_PROBE_RUN_ID");
    if(!identity || std::strlen(identity)!=36 ||
       std::strspn(identity,"0123456789abcdef-")!=36 ||
       std::getenv("LSX4_PROBE_INPUT_TRACE") || std::getenv("LSX4_PROBE_CAPTURE_WINDOW")) return false;
    const std::string run=identity;
    NSString* folder=[NSHomeDirectory() stringByAppendingPathComponent:
        [@"Documents/live-control-" stringByAppendingString:@(identity)]];
    if(![NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil]) return false;
    setenv("LSX4_PROBE_CAPTURE_DIR",folder.fileSystemRepresentation,1);
    const std::string commandPath=[[folder stringByAppendingPathComponent:@"command.txt"] fileSystemRepresentation];
    // DeviceProbeMain uses manual reference counting. A C++ lambda does not
    // retain an NSString capture there; own its bytes across autorelease pools.
    const std::string statePath=[[folder stringByAppendingPathComponent:@"state.json"] fileSystemRepresentation];
    struct State { std::uint64_t sequence{};std::uint32_t buttons{};unsigned packets{},captures{};bool stopped{}; };
    const auto state=std::make_shared<State>();
    const auto release=[state] {
        if(state->buttons) executor_lsx4_apple_set_pad_button(state->buttons,0);
        for(int axis=0;axis<6;++axis) executor_lsx4_runtime_set_pad_axis(axis,axis<4?128:0);
        state->buttons=0;
    };
    const auto publish=[state,statePath](const char* phase) {
        NSDictionary* receipt=@{@"sequence":@(state->sequence),@"phase":@(phase),@"fpsAcceptanceEligible":@NO};
        [[NSJSONSerialization dataWithJSONObject:receipt options:0 error:nil] writeToFile:@(statePath.c_str()) atomically:YES];
    };
    publish("ready");
    dispatch_source_t timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_main_queue());
    dispatch_source_set_timer(timer,dispatch_time(DISPATCH_TIME_NOW,250*NSEC_PER_MSEC),250*NSEC_PER_MSEC,25*NSEC_PER_MSEC);
    dispatch_source_set_event_handler(timer,^{
        if(state->stopped || state->packets>=256) return;
        std::ifstream input(commandPath); if(!input) return;
        char bytes[513]{};input.read(bytes,sizeof(bytes));const auto count=input.gcount();
        if(count<=0 || count>512) return;
        AppleLiveControl::Command command;
        if(!AppleLiveControl::Parse(std::string(bytes,count),run,state->sequence,LSX4_APPLE_PAD_BUTTON_MASK,command)) return;
        ++state->packets;
        release();state->sequence=command.sequence;state->buttons=command.buttons;
        if(command.buttons) executor_lsx4_apple_set_pad_button(command.buttons,1);
        for(int axis=0;axis<6;++axis) executor_lsx4_runtime_set_pad_axis(axis,command.axes[axis]);
        publish("applied");
        std::fprintf(stderr,"[APPLE_LIVE_INPUT] sequence=%llu buttons=0x%x hold=%.3f capture=%d performanceRun=0\n",
            static_cast<unsigned long long>(command.sequence),command.buttons,command.holdSeconds,command.capture);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,int64_t(command.holdSeconds*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
            if(state->stopped || state->sequence!=command.sequence) return;
            release();
            if(command.capture && state->captures<64) {
                ++state->captures;Lsx4::Apple::RequestDiagnosticFrameCapture(CaptureAppleFrame);
            }
            publish("released");
        });
    });
    dispatch_resume(timer);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,duration*NSEC_PER_SEC),dispatch_get_main_queue(),^{
        state->stopped=true;dispatch_source_cancel(timer);release();publish("stopped");
    });
    std::fprintf(stderr,"[APPLE_LIVE_INPUT] run=%s duration=%u directory=%s performanceRun=0\n",identity,duration,folder.fileSystemRepresentation);
    return true;
}
