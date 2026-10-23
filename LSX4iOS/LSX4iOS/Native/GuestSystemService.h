// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "GuestAddressSpace.h"
#include <optional>
#include <string_view>

namespace Lsx4::Apple {
class GuestApplicationState {
public:
    struct Snapshot {bool active;std::uint64_t resumes;};
    void SetActive(bool active) {
        std::scoped_lock lock(mutex_);
        if(active && !active_)++resumes_;
        active_=active;
    }
    Snapshot Read() const {std::scoped_lock lock(mutex_);return {active_,resumes_};}
private:
    mutable std::mutex mutex_;
    bool active_{true};
    std::uint64_t resumes_{};
};
inline GuestApplicationState& PlatformApplicationState() {static GuestApplicationState state;return state;}
class GuestSystemService {
public:
    explicit GuestSystemService(GuestApplicationState& state):state_(state),consumed_(state.Read().resumes) {}
    std::optional<std::uint32_t> Call(std::string_view identity,std::uint64_t output,GuestAddressSpace& memory) {
        constexpr std::string_view suffix="#libSceSystemService#libSceSystemService";
        if(!identity.ends_with(suffix))return std::nullopt;
        const auto nid=identity.substr(0,identity.size()-suffix.size());
        constexpr std::uint32_t Parameter=0x80a10003,NoEvent=0x80a10004;
        const auto snapshot=state_.Read();
        if(nid=="d-15YTCUMVU")return !snapshot.active;
        if(nid=="rPo6tV8D9bM") {
            // Original OrbisSystemServiceStatus: s32 event_num followed by five
            // bools. Reserved tail belongs to the caller and remains untouched.
            if(!output || !memory.CanWrite(output,9))return Parameter;
            std::array<std::uint8_t,9> status{};
            const auto pending=std::min<std::uint64_t>(snapshot.resumes-consumed_,INT32_MAX);
            for(unsigned i=0;i<4;++i)status[i]=pending>>(i*8);
            status[5]=!snapshot.active;status[6]=1;
            memory.Write(output,status);return 0;
        }
        if(nid=="656LMQSrg6U") {
            if(!output)return Parameter;
            if(snapshot.resumes==consumed_)return NoEvent;
            std::array<std::uint8_t,8196> event{};
            event[3]=0x10; // OrbisSystemServiceEventType::OnResume, 0x10000000.
            if(!memory.Write(output,event))return Parameter;
            ++consumed_;return 0;
        }
        if(nid=="1n37q1Bvc5Y") {
            if(!output || !memory.CanWrite(output,4))return Parameter;
            // The guest renders its entire virtual display. Host cutouts and
            // safe-area layout are applied outside that framebuffer.
            memory.Put32(output,0x3f800000);return 0;
        }
        return std::nullopt;
    }
private:
    GuestApplicationState& state_;
    std::uint64_t consumed_;
};
}
