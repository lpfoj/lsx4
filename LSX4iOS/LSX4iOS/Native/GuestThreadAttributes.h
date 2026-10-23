// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "executor/dynamic_translation/guest_memory.h"
#include <optional>
#include <set>
#include <string_view>

namespace Lsx4::Apple {
struct GuestThreadParameters {
    std::uint32_t policy{2}, inherit{4}, priority{700}, suspended{}, flags{2}, reserved{};
    std::uint64_t stackAddress{}, stackBytes{1024*1024}, guardBytes{}, cpuSetBytes{}, cpuSet{};
};
static_assert(sizeof(GuestThreadParameters)==64);
static_assert(offsetof(GuestThreadParameters,stackAddress)==24);
class GuestThreadAttributes {
public:
    static constexpr std::uint64_t StorageBase=0x510000000000,StorageBytes=1024*1024;
    std::optional<std::uint32_t> Call(std::string_view identity,std::uint64_t first,
        std::uint64_t second,std::uint64_t third,const Translation::GuestMemoryPort& memory);
    std::optional<GuestThreadParameters> Get(std::uint64_t cell,const Translation::GuestMemoryPort& memory) const;
    bool Set(std::uint64_t cell,const GuestThreadParameters& value,const Translation::GuestMemoryPort& memory);
private:
    std::set<std::uint64_t> handles_;
    std::uint64_t next_{StorageBase};
};
}
