// SPDX-License-Identifier: GPL-2.0-or-later
// Host test allocator only. iPhone builds use debugger-prepared RX/RW aliases.
#include "executor/dynamic_translation/code_memory.h"
#include "executor/dynamic_translation/apple_signal_code_ranges.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

namespace Lsx4::Translation {
struct ExecutableArena::Slab {
    std::vector<std::uint8_t> draft;
    void* executable{MAP_FAILED};
    std::size_t cursor{};
    AppleSignalCodeRange signalRange;
    ~Slab() { signalRange.Clear();if (executable != MAP_FAILED) munmap(executable, draft.size()); }
};
ExecutableArena::ExecutableArena(std::size_t bytes) noexcept : preferred_slab_bytes_(bytes) {}
ExecutableArena::~ExecutableArena() = default;
std::unique_ptr<ExecutableArena::Slab> ExecutableArena::AllocateSlab(std::size_t minimum) noexcept {
    try {
        auto slab = std::make_unique<Slab>();
        const auto page = std::size_t(getpagesize());
        const auto bytes = (std::max(minimum, preferred_slab_bytes_) + page - 1) / page * page;
        slab->draft.resize(bytes);
        slab->executable = mmap(nullptr, bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                                MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        if (slab->executable == MAP_FAILED) return {};
        if(!slab->signalRange.Publish(reinterpret_cast<std::uintptr_t>(slab->executable),bytes))return {};
        return slab;
    } catch (...) { return {}; }
}
CodeReservation ExecutableArena::Reserve(std::size_t bytes, std::size_t alignment) noexcept {
    if (!bytes || !std::has_single_bit(alignment) || alignment > 16384 ||
        bytes > 64 * 1024 * 1024 - alignment) return {};
    std::scoped_lock lock(mutex_);
    const auto reserve = [&](Slab& slab) -> CodeReservation {
        const auto offset = (slab.cursor + alignment - 1) & ~(alignment - 1);
        if (offset > slab.draft.size() || bytes > slab.draft.size() - offset) return {};
        slab.cursor = offset + bytes;
        committed_bytes_ += bytes;
        return {{slab.draft.data() + offset, bytes},
                {static_cast<const std::uint8_t*>(slab.executable) + offset, bytes}};
    };
    for (auto& slab : slabs_) if (auto result = reserve(*slab); result.IsValid()) return result;
    auto slab = AllocateSlab(bytes + alignment);
    if (!slab) return {};
    try { slabs_.push_back(std::move(slab)); } catch (...) { return {}; }
    return reserve(*slabs_.back());
}
void ExecutableArena::Publish(const CodeReservation& reservation) noexcept {
    pthread_jit_write_protect_np(0);
    std::memcpy(const_cast<std::uint8_t*>(reservation.executable.data()),
                reservation.writable.data(), reservation.writable.size());
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(const_cast<std::uint8_t*>(reservation.executable.data()),
                          reservation.executable.size());
}
bool ExecutableArena::Owns(std::uintptr_t address) const noexcept {
    std::scoped_lock lock(mutex_);
    for (const auto& slab : slabs_) {
        const auto base = reinterpret_cast<std::uintptr_t>(slab->executable);
        if (address >= base && address - base < slab->cursor) return true;
    }
    return false;
}
std::size_t ExecutableArena::CommittedBytes() const noexcept {
    std::scoped_lock lock(mutex_);
    return committed_bytes_;
}
}
