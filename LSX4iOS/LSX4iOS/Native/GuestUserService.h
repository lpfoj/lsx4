// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "GuestAddressSpace.h"
#include <optional>
#include <string_view>

namespace Lsx4::Apple {
// Same local user, event order and name as the common headless UserService.
// Each title run owns its session; NP sign-in remains a separate offline state.
class GuestUserService {
public:
    static constexpr std::uint32_t UserId=1000;
    std::optional<std::uint32_t> Call(std::string_view identity,std::uint64_t a,
                                     std::uint64_t b,std::uint64_t c,GuestAddressSpace& memory) {
        constexpr std::uint32_t invalid=0x80960005,noEvent=0x80960007,shortBuffer=0x8096000a;
        constexpr std::string_view suffix="#libSceUserService#libSceUserService";
        if(!identity.ends_with(suffix))return std::nullopt;
        const auto nid=identity.substr(0,identity.size()-suffix.size());
        if(nid=="j3YMu1MVNNo") {
            if(a && !memory.CanRead(a,4))return invalid;
            initialized_=true;return 0;
        }
        if(nid=="bwFjS+bX9mA") {initialized_=false;return 0;}
        if(nid=="CdWp0oHWGr0") {
            if(!a || !memory.CanWrite(a,4))return invalid;
            initialized_=true;memory.Put32(a,UserId);return 0;
        }
        if(nid=="fPhymKNvK-A") {
            if(!a || !memory.CanWrite(a,16))return invalid;
            initialized_=true;memory.Put32(a,UserId);
            for(unsigned i=1;i<4;++i)memory.Put32(a+4*i,UINT32_MAX);
            return 0;
        }
        if(nid=="yH17Q6NWtVg") {
            if(!a || !memory.CanWrite(a,8))return invalid;
            initialized_=true;
            if(!loginPending_)return noEvent;
            memory.Put32(a,0);memory.Put32(a+4,UserId);loginPending_=false;return 0;
        }
        if(nid=="lUoqwTQu4Go") {
            if(std::uint32_t(a)!=UserId || !b || !memory.CanWrite(b,4))return invalid;
            initialized_=true;memory.Put32(b,0);return 0;
        }
        if(nid=="1xxcMiGu2fo") {
            if(std::uint32_t(a)!=UserId || !b)return invalid;
            constexpr std::array<std::uint8_t,5> name={'L','S','X','4',0};
            if(c<name.size())return shortBuffer;
            if(!memory.Write(b,name))return invalid;
            initialized_=true;return 0;
        }
        return std::nullopt;
    }
    bool Initialized() const {return initialized_;}
private:
    bool initialized_{},loginPending_{true};
};
}
