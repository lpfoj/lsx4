// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <optional>
#include <string_view>
namespace Lsx4::Apple {
// Session ownership replaces the process globals in common commondialog.cpp.
// Dialog presentation calls remain unsupported until connected to the iOS UI.
class GuestCommonDialog {
public:
    std::optional<std::uint32_t> Call(std::string_view identity) {
        if(identity=="uoUpLGNkygk#libSceCommonDialog#libSceCommonDialog") {
            if(initialized_)return 0x80b80002;
            initialized_=true;return 0;
        }
        if(identity=="BQ3tey0JmQM#libSceCommonDialog#libSceCommonDialog")return saveInitialized_;
        constexpr std::string_view suffix="#libSceSaveDataDialog#libSceSaveDataDialog";
        if(!identity.ends_with(suffix))return std::nullopt;
        const auto nid=identity.substr(0,identity.size()-suffix.size());
        if(nid=="KK3Bdg1RWK0" || nid=="ERKzksauAJA")return saveInitialized_?1:0;
        if(nid=="s9e3+YpRnzw") {
            if(!initialized_)return 0x80b80001;
            if(saveInitialized_)return 0x80b80004;
            saveInitialized_=true;return 0;
        }
        if(nid=="YuH2FA7azqQ") {
            if(!saveInitialized_)return 0x80b80003;
            saveInitialized_=false;return 0;
        }
        if(nid=="yEiJ-qqr6Cg")return 0x80b80005; // No displayed/finished dialog yet.
        if(nid=="fH46Lag88XY" || nid=="V-uEeFKARJU" || nid=="hay1CfTmLyA")return 0x80b8000b;
        // Open remains a real platform boundary; polling an idle service never
        // invents a completed dialog or a user's save-slot selection.
        return std::nullopt;
    }
private:
    bool initialized_{};
    bool saveInitialized_{};
};
}
