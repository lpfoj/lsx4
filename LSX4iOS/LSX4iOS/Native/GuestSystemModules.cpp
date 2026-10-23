// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestSystemModules.h"
#include <array>
#include "core/libraries/sysmodule/sysmodule_table.h"
#include "core/libraries/sysmodule/sysmodule_error.h"
namespace Lsx4::Apple {
void GuestSystemModules::AddLoaded(std::string name) {
    if(name.ends_with(".prx"))name.resize(name.size()-4);
    if(name.ends_with(".sprx"))name.resize(name.size()-5);
    loaded_.insert(std::move(name));
}
std::optional<std::uint32_t> GuestSystemModules::Call(std::string_view identity,std::uint64_t argument) const {
    constexpr std::string_view suffix="#libSceSysmodule#libSceSysmodule";
    if(!identity.ends_with(suffix))return std::nullopt;
    const auto nid=identity.substr(0,identity.size()-suffix.size());
    const bool external=nid=="g8cM39EUZ6o" || nid=="fMP5NHUOaMk";
    const bool internal=nid=="39iV5E1HoCk" || nid=="ynFKQ5bfGks";
    if(!external && !internal)return std::nullopt;
    const auto id=external?std::uint16_t(argument):std::uint32_t(argument);
    if(!(id&0x7fffffff))return ORBIS_SYSMODULE_INVALID_ID;
    for(const auto& module:Libraries::SysModule::g_modules_array) {
        if(module.id!=id)continue;
        if(!module.name || (module.flags&Libraries::SysModule::IsDebug))return ORBIS_SYSMODULE_INVALID_ID;
        // Availability is based on the actual loaded guest PRXs and registered
        // Apple HLE services. Missing modules must not report successful loads.
        return loaded_.contains(module.name)?0:ORBIS_SYSMODULE_NOT_LOADED;
    }
    return ORBIS_SYSMODULE_INVALID_ID;
}
}
