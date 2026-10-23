// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "GuestAddressSpace.h"
#include "GuestUserService.h"
#include <array>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

namespace Lsx4::Apple {
// Offline NP state follows funnel-arm's np_manager.cpp SignedOut route.
// Title ID/secret ABI sizes: psOff_public/modules/libSceNpManager/{types,codes}.h.
// Configuration is session-local and never signs in or sends network requests.
class GuestNpServices {
public:
    std::string_view TitleId() const {return {reinterpret_cast<const char*>(title_.data()),12};}
    std::size_t ContentRuleCount() const {return contentRules_.size()/4;}
    std::optional<std::uint32_t> Call(std::string_view identity,std::uint64_t first,
                                     std::uint64_t second,GuestAddressSpace& memory) {
        constexpr std::uint32_t invalid=0x80550003,signedOut=0x80550006;
        if(identity=="GWnWQNXZH5M#libSceNpScore#libSceNpScore") {
            if(std::uint32_t(first)==UINT32_MAX)return invalid;
            if(std::uint32_t(second)!=GuestUserService::UserId)return 0x80550007;
            if(scoreContexts_.size()>=32 || nextScoreContext_>=INT32_MAX)return 0x80550013;
            const auto handle=nextScoreContext_++;
            scoreContexts_.emplace(handle,std::uint32_t(first));return handle;
        }
        if(identity=="G0pE+RNCwfk#libSceNpScore#libSceNpScore")
            return scoreContexts_.erase(std::uint32_t(first))?0:0x80550015;
        // The trophy module has not been loaded by GuestSystemModules. Report
        // that explicit failure; no context/handle is fabricated in guest memory.
        if(identity=="XbkjbobZlCY#libSceNpTrophy#libSceNpTrophy" ||
           identity=="q7U6tEAQf7c#libSceNpTrophy#libSceNpTrophy")return 0x80551601;
        constexpr std::string_view suffix="#libSceNpManager#libSceNpManager";
        if(!identity.ends_with(suffix))return std::nullopt;
        const auto nid=identity.substr(0,identity.size()-suffix.size());
        if(nid=="Ec63y59l9tw") { // sceNpSetNpTitleId
            std::array<std::uint8_t,13> title{};
            std::array<std::uint8_t,128> secret{};
            if(!first || !second || !memory.Read(first,title) || !memory.Read(second,secret) || title[12])return invalid;
            for(unsigned i=0;i<12;++i)if(!title[i])return invalid;
            title_=title;secret_=secret;return 0;
        }
        if(nid=="A2CQ3kgSopQ") { // sceNpSetContentRestriction, 24-byte parameter ABI
            if(!first || !memory.CanRead(first,24) || memory.Get(first)!=24)return invalid;
            const auto count=std::int32_t(memory.Get(first+12));
            const auto pointer=memory.Get(first+16);
            if(count<0 || count>256 || (count && !pointer))return invalid;
            std::array<std::uint8_t,1> age{};
            std::vector<std::uint8_t> rules(std::size_t(count)*4);
            if(!memory.Read(first+8,age) || (count && !memory.Read(pointer,rules)))return invalid;
            // Per-country entries contain two letters, a terminator and age.
            for(unsigned i=0;i<rules.size();i+=4)if(rules[i+2])return invalid;
            defaultAge_=age[0];contentRules_=std::move(rules);return 0;
        }
        if(nid=="eQH7nWPcAgc" || nid=="e-ZuhGEoeC4") { // state / reachability
            if(!second || !memory.CanWrite(second,4))return invalid;
            memory.Put32(second,nid=="eQH7nWPcAgc"?1:0);return 0;
        }
        if(nid=="Oad3rvY-NJQ") { // has signed up
            const std::array<std::uint8_t,1> no{};
            return second && memory.Write(second,no)?0:invalid;
        }
        // These synchronous queries cannot produce an online identity while
        // signed out. As in the common HLE, leave the output untouched.
        if(nid=="a8R9-75u4iM" || nid=="rbknaUjpqWo" || nid=="p-o74CnoNzY" || nid=="XDncXQIJUSk")
            return second?signedOut:invalid;
        return std::nullopt;
    }
private:
    std::array<std::uint8_t,13> title_{};
    std::array<std::uint8_t,128> secret_{};
    std::uint8_t defaultAge_{};
    std::vector<std::uint8_t> contentRules_;
    std::map<std::uint32_t,std::uint32_t> scoreContexts_;
    std::uint32_t nextScoreContext_{1};
};
}
