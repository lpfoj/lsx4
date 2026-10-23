// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "executor/dynamic_translation/guest_memory.h"
#include <map>
#include <mutex>
#include <optional>
#include <string_view>

namespace Lsx4::Apple {
struct GuestMutexResult {std::uint32_t code{};bool waiting{};};
// Guest handles and ownership are independent of Apple's pthread ABI. A
// contended blocking lock yields to the guest scheduler, never succeeds early.
class GuestMutexes {
public:
    static constexpr std::uint64_t StorageBase=0x500000000000;
    static constexpr std::uint64_t StorageBytes=8*1024*1024;
    std::optional<GuestMutexResult> Call(std::string_view identity,std::uint64_t cell,
        std::uint64_t argument,std::uint64_t thread,const Translation::GuestMemoryPort& memory);
private:
    struct Attribute {std::uint32_t type{1}, protocol{};};
    struct Mutex {Attribute attribute;std::uint64_t owner{};std::uint32_t depth{};};
    std::map<std::uint64_t,Attribute> attributes_;
    std::map<std::uint64_t,Mutex> mutexes_;
    std::uint64_t nextHandle_{StorageBase};
    std::mutex lock_;
};
}
