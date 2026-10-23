// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "GuestAddressSpace.h"
#include <map>
#include <optional>
#include <string_view>

namespace Lsx4::Apple {
struct GuestEvent {
    std::uint64_t ident{};
    std::int16_t filter{};
    std::uint16_t flags{};
    std::uint32_t fflags{};
    std::uint64_t data{},userData{};
};
static_assert(sizeof(GuestEvent)==32 && offsetof(GuestEvent,userData)==24);
struct GuestEqueueResult {std::uint64_t value{};bool waiting{};};
// Queue registration and user events have real state. An empty blocking wait
// yields at the import boundary; only a producer may make its events ready.
// Timer scheduling and graphics completion are not fabricated here.
class GuestEqueues {
public:
    bool Exists(std::uint64_t handle) const;
    std::uint32_t Add(std::uint64_t handle,GuestEvent event);
    std::uint32_t Remove(std::uint64_t handle,std::uint64_t ident,std::int16_t filter);
    void Trigger(std::uint64_t ident,std::int16_t filter,std::uint64_t data);
    void RemoveFilter(std::int16_t filter);
    std::optional<GuestEqueueResult> Call(std::string_view identity,
        const Translation::CpuFrame& frame,GuestAddressSpace& memory);
private:
    struct Event {GuestEvent value;bool triggered{};};
    struct Queue {std::string name;std::vector<Event> events;};
    std::map<std::uint64_t,Queue> queues_;
    std::uint64_t nextHandle_{0x520000000000};
    mutable std::recursive_mutex mutex_;
};
}
