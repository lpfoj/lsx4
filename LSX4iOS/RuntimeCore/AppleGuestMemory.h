// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>
namespace Lsx4::Apple {
struct BackingSpanLease {
    std::uint8_t* data{};
    std::shared_ptr<void> owner;
};
// Owns only memory selected by the host VM allocator. Common Funnel mapping,
// physical aliasing, protections and invalidation still operate on these VAs.
class GuestMemoryLayout {
public:
    explicit GuestMemoryLayout(std::size_t backingBytes, bool needsExecutorHeap = true);
    ~GuestMemoryLayout();
    GuestMemoryLayout(const GuestMemoryLayout&)=delete;
    GuestMemoryLayout& operator=(const GuestMemoryLayout&)=delete;
    int backingFd{-1};
    std::size_t backingBytes{};
    std::uint8_t* guest{};
    std::size_t managedBytes{512ULL<<20};
    std::size_t reservedBytes{128ULL<<20};
    std::size_t userBytes{};
    std::size_t GuestBytes() const { return managedBytes+reservedBytes+userBytes; }
    struct HostRange { std::uintptr_t address{}; std::size_t bytes{}; };
    // expectedAddress is a non-destructive mmap hint, accepted only on an exact match.
    HostRange ReserveAdditionalRange(std::size_t bytes, std::size_t alignment,
                                     std::uintptr_t expectedAddress = 0);
    bool ContainsAdditionalRange(std::uintptr_t address, std::size_t bytes) const;
    BackingSpanLease AcquireBackingSpan(std::uint64_t offset,std::size_t bytes);
    std::size_t CachedBackingBytes() const;
private:
    struct View {
        std::uint8_t* address{};
        std::size_t bytes{};
        std::uint64_t lastUse{};
        ~View();
    };
    void TrimViewsLocked(std::size_t targetBytes,bool idleOnly);
    mutable std::shared_mutex additionalMutex;
    std::vector<HostRange> additionalRanges;
    mutable std::mutex viewMutex;
    // Non-contained coverage index: both starts and ends increase.
    std::map<std::uint64_t,std::shared_ptr<View>> views;
    // A caller lease pins its mapping independently of cache/layout lifetime.
    // The cache may forget a pinned view but can never revoke the lease.
    std::size_t cachedViewBytes{};
    std::uint64_t viewSequence{};
};
}
