// SPDX-License-Identifier: GPL-2.0-or-later
#include "AppleCodeMemory.h"
#include "executor/dynamic_translation/code_memory.h"
#include "executor/dynamic_translation/apple_signal_code_ranges.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <libkern/OSCacheControl.h>
#include <vector>

namespace {
struct PreparedRange {
    std::shared_ptr<Lsx4::Apple::CodeMemory> memory;
    std::size_t offset{}, bytes{};
};
struct PreparedPool {
    std::mutex mutex;
    std::vector<PreparedRange> ranges;
    std::size_t capacity{}, claimed{};
};
PreparedPool& Pool() {
    // Published code can remain reachable from another guest thread. Keep the
    // bounded pool for the process lifetime; never recycle retired arena slots.
    static auto* pool=new PreparedPool;
    return *pool;
}
PreparedRange ClaimPreparedRange(std::size_t bytes) {
    auto& pool=Pool();std::scoped_lock lock(pool.mutex);
    for(auto& range:pool.ranges) if(range.bytes>=bytes) {
        PreparedRange result{range.memory,range.offset,bytes};
        range.offset+=bytes;range.bytes-=bytes;pool.claimed+=bytes;
        return result;
    }
    return {};
}
}

namespace Lsx4::Apple {
bool PrepareExecutableArenaPool(std::size_t bytes,std::string& error) {
    constexpr std::size_t chunk=4*1024*1024;
    if(bytes<chunk || bytes>ExecutablePoolLimitBytes || bytes%chunk) {
        error="Prepared code pool must be 4–512 MiB in 4 MiB chunks";return false;
    }
    auto& pool=Pool();std::scoped_lock lock(pool.mutex);
    try {
        while(pool.capacity<bytes) {
            auto memory=std::make_shared<CodeMemory>();
            if(!memory->Allocate(chunk,error)) return false;
            pool.ranges.push_back({std::move(memory),0,chunk});pool.capacity+=chunk;
        }
        return true;
    } catch(const std::exception& failure) { error=failure.what();return false; }
}
std::size_t PreparedExecutableArenaPoolClaimedBytes() noexcept {
    auto& pool=Pool();std::scoped_lock lock(pool.mutex);return pool.claimed;
}
}

namespace Lsx4::Translation {
struct ExecutableArena::Slab {
    std::shared_ptr<Apple::CodeMemory> memory;
    std::size_t offset{},capacity{};
    std::size_t cursor{};
    AppleSignalCodeRange signalRange;
    std::span<std::byte> Writable() {return memory->Writable().subspan(offset,capacity);}
    const std::uint8_t* Executable() const {
        return static_cast<const std::uint8_t*>(memory->Executable())+offset;
    }
};
ExecutableArena::ExecutableArena(std::size_t slabBytes) noexcept
    : preferred_slab_bytes_(std::clamp<std::size_t>(slabBytes,16384,4*1024*1024)) {}
ExecutableArena::~ExecutableArena()=default;
std::unique_ptr<ExecutableArena::Slab> ExecutableArena::AllocateSlab(std::size_t minimum) noexcept {
    try {
        std::size_t reserved=0;for(auto& slab:slabs_) reserved+=slab->capacity;
        const auto size=(std::max(minimum,preferred_slab_bytes_)+16383)&~std::size_t(16383);
        if(size>4*1024*1024 || reserved>Apple::ExecutablePoolLimitBytes-size) return {};
        auto slab=std::make_unique<Slab>();std::string error;
        if(auto prepared=ClaimPreparedRange(size);prepared.memory) {
            slab->memory=std::move(prepared.memory);slab->offset=prepared.offset;
        } else {
            slab->memory=std::make_shared<Apple::CodeMemory>();
            if(!slab->memory->Allocate(size,error)) return {};
        }
        slab->capacity=size;
        if(!slab->signalRange.Publish(reinterpret_cast<std::uintptr_t>(slab->Executable()),size))return {};
        return slab;
    } catch(...) {return {};}
}
CodeReservation ExecutableArena::Reserve(std::size_t bytes,std::size_t alignment) noexcept {
    if(!bytes || !std::has_single_bit(alignment) || alignment>16384 || bytes>4*1024*1024-alignment) return {};
    std::scoped_lock lock(mutex_);
    auto reserve=[&](Slab& slab)->CodeReservation {
        auto data=slab.Writable();const auto offset=(slab.cursor+alignment-1)&~(alignment-1);
        if(offset>data.size() || bytes>data.size()-offset) return {};
        slab.cursor=offset+bytes;committed_bytes_+=bytes;
        return {{reinterpret_cast<std::uint8_t*>(data.data())+offset,bytes},
                {slab.Executable()+offset,bytes}};
    };
    for(auto& slab:slabs_) if(auto result=reserve(*slab);result.IsValid()) return result;
    auto slab=AllocateSlab(bytes+alignment);if(!slab) return {};
    try {slabs_.push_back(std::move(slab));} catch(...) {return {};}
    return reserve(*slabs_.back());
}
void ExecutableArena::Publish(const CodeReservation& reservation) noexcept {
    // Reserve supplies views into the same debugger-prepared physical pages.
    std::atomic_thread_fence(std::memory_order_release);
    sys_dcache_flush(reservation.writable.data(),reservation.writable.size());
    sys_icache_invalidate(const_cast<std::uint8_t*>(reservation.executable.data()),reservation.executable.size());
}
bool ExecutableArena::Owns(std::uintptr_t address) const noexcept {
    std::scoped_lock lock(mutex_);
    for(const auto& slab:slabs_) {
        const auto base=reinterpret_cast<std::uintptr_t>(slab->Executable());
        if(address>=base && address-base<slab->cursor) return true;
    }
    return false;
}
std::size_t ExecutableArena::CommittedBytes() const noexcept {
    std::scoped_lock lock(mutex_);return committed_bytes_;
}
}
