// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "../AppleConsoleInput.h"
extern "C" int executor_lsx4_runtime_set_pad_axis(int,int);
static bool ScheduleAppleInputTrace(unsigned duration) {
    const char* path=std::getenv("LSX4_PROBE_INPUT_TRACE");
    if(!path) return true;
    std::ifstream input(path);
    if(!input) return false;
    // Existing button rows remain: seconds hex-mask pressed.
    // Analog rows use: seconds axis index byte-value (0..5, 0..255).
    struct Event {double seconds;std::uint32_t button;int pressed;int axis=-1;};
    std::vector<Event> events;
    std::string line;
    double previous=0;
    while(std::getline(input,line)) {
        const auto first=line.find_first_not_of(" \t\r");
        if(first==std::string::npos || line[first]=='#') continue;
        Event event{};std::istringstream fields(line);std::string extra,kind;
        if(!(fields>>event.seconds>>kind)) return false;
        if(kind=="axis") {
            if(!(fields>>event.axis>>event.pressed) || event.axis<0 || event.axis>5 ||
               event.pressed<0 || event.pressed>255) return false;
        } else {
            std::istringstream mask(kind);
            if(!(mask>>std::hex>>event.button) || (mask>>extra) ||
               !(fields>>event.pressed) || event.button==0 || (event.button&~LSX4_APPLE_PAD_BUTTON_MASK) ||
               (event.pressed!=0 && event.pressed!=1)) return false;
        }
        if(
            (fields>>extra) || !std::isfinite(event.seconds) || event.seconds<previous ||
            event.seconds>=duration || events.size()>=1000) return false;
        previous=event.seconds;events.push_back(event);
    }
    for(const auto event:events) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(event.seconds*NSEC_PER_SEC)),
            dispatch_get_main_queue(),^{
                if(event.axis>=0) {
                    const int result=executor_lsx4_runtime_set_pad_axis(event.axis,event.pressed);
                    std::fprintf(stderr,"[APPLE_INPUT_TRACE] seconds=%.3f axis=%d value=%d result=%d\n",
                        event.seconds,event.axis,event.pressed,result);
                } else {
                    const int result=executor_lsx4_apple_set_pad_button(event.button,event.pressed);
                    std::fprintf(stderr,"[APPLE_INPUT_TRACE] seconds=%.3f button=0x%x pressed=%d result=%d\n",
                        event.seconds,event.button,event.pressed,result);
                }
            });
    }
    return true;
}
