// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <locale>
#include <sstream>
#include <string>

namespace AppleLiveControl {
struct Command {
    std::uint64_t sequence{};
    double holdSeconds{};
    std::uint32_t buttons{};
    std::array<int,6> axes{128,128,128,128,0,0};
    bool capture{};
};

// One bounded state packet, scoped to a fresh host-generated launch UUID.
// A stale or malformed packet never changes the current controller state.
inline bool Parse(const std::string& packet, const std::string& run,
                  std::uint64_t lastSequence, std::uint32_t buttonMask, Command& result) {
    if(packet.empty() || packet.size()>512 || run.empty()) return false;
    std::istringstream fields(packet);fields.imbue(std::locale::classic());
    std::string identity, sequence, buttons, extra; Command value; int capture{};
    if(!(fields>>identity>>sequence>>value.holdSeconds>>buttons) || identity!=run ||
       sequence.empty() || sequence.size()>9 || sequence.find_first_not_of("0123456789")!=std::string::npos ||
       buttons.empty() || buttons.size()>8 || buttons.find_first_not_of("0123456789abcdefABCDEF")!=std::string::npos ||
       !std::isfinite(value.holdSeconds) || value.holdSeconds<0 || value.holdSeconds>10) return false;
    value.sequence=std::stoull(sequence);value.buttons=std::stoul(buttons,nullptr,16);
    if(!value.sequence || value.sequence<=lastSequence || (value.buttons & ~buttonMask)) return false;
    for(auto& axis:value.axes) if(!(fields>>axis) || axis<0 || axis>255) return false;
    if(!(fields>>capture) || (capture!=0 && capture!=1) || (fields>>extra)) return false;
    if(value.holdSeconds==0 && (value.buttons || value.axes!=Command{}.axes)) return false;
    value.capture=capture!=0;result=value;return true;
}
}
