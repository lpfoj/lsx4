// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "GuestEqueue.h"
namespace Lsx4::Apple {
class GuestVideoOut;
// Graphics event subscriptions. The renderer must deliver the corresponding
// interrupt after GPU work completes; registration itself never signals it.
class GuestGnm {
public:
    std::optional<std::uint32_t> Build(std::string_view identity,
        const Translation::CpuFrame& frame,GuestAddressSpace& memory,const GuestVideoOut* video=nullptr);
    std::optional<std::uint64_t> Call(std::string_view identity,
        const Translation::CpuFrame& frame,GuestEqueues& queues) {
        constexpr std::string_view suffix="#libSceGnmDriver#libSceGnmDriver";
        if(!identity.ends_with(suffix))return std::nullopt;
        const auto nid=identity.substr(0,identity.size()-suffix.size());
        const auto a=frame.integer[7],b=frame.integer[6];
        if(nid=="b0xyllnVY-I")return queues.Add(a,{b,-14,1,0,0,frame.integer[2]});
        if(nid=="PVT+fuoS9gU") {
            if(!queues.Exists(a))return 0x80020009;
            queues.Remove(a,b,-14);return 0;
        }
        return std::nullopt;
    }
};
}
