// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "common/enum.h"
#include "common/shared_first_mutex.h"
#include "common/singleton.h"
#include "common/types.h"
#include "core/address_space.h"
#include "core/libraries/kernel/memory.h"

namespace Vulkan {
class Rasterizer;
}

namespace Libraries::Kernel {
struct OrbisQueryInfo;
}

namespace Core::Devtools::Widget {
class MemoryMapViewer;
}

namespace Core {

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
u64 ExecutorCopySparseCaller() noexcept;
#endif

constexpr u64 DEFAULT_MAPPING_BASE = 0x200000000;

#if defined(LSX4_APPLE_RUNTIME)
// Chosen inside the host-owned guest arena; never an unowned fixed host VA.
VAddr ExecutorLibcMspaceBase();
#else
inline constexpr VAddr EXECUTOR_LIBC_MSPACE_BASE = 0x780000000ULL;
inline constexpr VAddr ExecutorLibcMspaceBase() { return EXECUTOR_LIBC_MSPACE_BASE; }
#endif
inline constexpr u64 EXECUTOR_LIBC_MSPACE_INITIAL_SIZE = 0x20000000ULL;
inline constexpr u64 EXECUTOR_LIBC_MSPACE_CAPACITY = 0x60000000ULL;
inline constexpr u64 EXECUTOR_LIBC_MSPACE_GROW_GRANULARITY = 0x10000000ULL;

enum class MemoryProt : u32 {
    NoAccess = 0,
    CpuRead = 1,
    CpuWrite = 2,
    CpuReadWrite = 3,
    CpuExec = 4,
    GpuRead = 16,
    GpuWrite = 32,
    GpuReadWrite = 48,
};
DECLARE_ENUM_FLAG_OPERATORS(MemoryProt)

enum class MemoryMapFlags : u32 {
    NoFlags = 0,
    Shared = 1,
    Private = 2,
    Fixed = 0x10,
    NoOverwrite = 0x80,
    Void = 0x100,
    Stack = 0x400,
    NoSync = 0x800,
    Anon = 0x1000,
    NoCore = 0x20000,
    NoCoalesce = 0x400000,
};
DECLARE_ENUM_FLAG_OPERATORS(MemoryMapFlags)

enum class PhysicalMemoryType : u32 {
    Free = 0,
    Allocated = 1,
    Mapped = 2,
    Pooled = 3,
    Committed = 4,
    Flexible = 5,
};

struct PhysicalMemoryArea {
    PAddr base = 0;
    u64 size = 0;
    s32 memory_type = 0;
    PhysicalMemoryType dma_type = PhysicalMemoryType::Free;

    PAddr GetEnd() const {
        return base + size;
    }

    bool CanMergeWith(const PhysicalMemoryArea& next) const {
        if (base + size != next.base) {
            return false;
        }
        if (memory_type != next.memory_type) {
            return false;
        }
        if (dma_type != next.dma_type) {
            return false;
        }
        return true;
    }
};

enum class VMAType : u32 {
    Free = 0,
    Reserved = 1,
    Direct = 2,
    Flexible = 3,
    Pooled = 4,
    PoolReserved = 5,
    Stack = 6,
    Code = 7,
    File = 8,
    ExecutorHeap = 9,
};

struct VirtualMemoryArea {
    VAddr base = 0;
    u64 size = 0;
    std::map<uintptr_t, PhysicalMemoryArea> phys_areas;
    VMAType type = VMAType::Free;
    MemoryProt prot = MemoryProt::NoAccess;
    std::string name = "";
    s32 fd = 0;
    bool disallow_merge = false;

    bool Contains(VAddr addr, u64 size) const {
        return addr >= base && (addr + size) <= (base + this->size);
    }

    bool Overlaps(VAddr addr, u64 size) const {
        return addr <= (base + this->size) && (addr + size) >= base;
    }

    bool IsFree() const noexcept {
        return type == VMAType::Free;
    }

    bool IsMapped() const noexcept {
        return type != VMAType::Free && type != VMAType::Reserved && type != VMAType::PoolReserved;
    }

    bool CanMergeWith(VirtualMemoryArea& next) {
        if (disallow_merge || next.disallow_merge) {
            return false;
        }
        if (base + size != next.base) {
            return false;
        }
        if (type == VMAType::Direct && next.type == VMAType::Direct) {
            auto& last_phys = std::prev(phys_areas.end())->second;
            auto& first_next_phys = next.phys_areas.begin()->second;
            if (last_phys.base + last_phys.size != first_next_phys.base ||
                last_phys.memory_type != first_next_phys.memory_type) {
                return false;
            }
        }
        if (prot != next.prot || type != next.type) {
            return false;
        }
        if (name.compare(next.name) != 0) {
            return false;
        }

        return true;
    }
};

class MemoryManager {
    struct SnapshotWritePlanData;

    using PhysMap = std::map<PAddr, PhysicalMemoryArea>;
    using PhysHandle = PhysMap::iterator;

    using VMAMap = std::map<VAddr, VirtualMemoryArea>;
    using VMAHandle = VMAMap::iterator;

public:
    using SnapshotRange = std::pair<VAddr, u64>;

    class SnapshotReadGuard {
    public:
        SnapshotReadGuard(SnapshotReadGuard&&) noexcept = default;
        SnapshotReadGuard& operator=(SnapshotReadGuard&&) noexcept = default;
        SnapshotReadGuard(const SnapshotReadGuard&) = delete;
        SnapshotReadGuard& operator=(const SnapshotReadGuard&) = delete;

    private:
        friend class MemoryManager;
        explicit SnapshotReadGuard(MemoryManager& owner_)
            : owner{&owner_}, topology_lock{owner_.unmap_mutex}, lock{owner_.mutex} {}

        MemoryManager* owner;
        std::unique_lock<std::mutex> topology_lock;
        std::shared_lock<Common::SharedFirstMutex> lock;
    };

    // Immutable topology-derived ranges. A plan may own freshly built vectors
    // or share a generation-keyed cache entry; callers only receive spans, so
    // a cache hit never materializes the vectors again.
    class SnapshotWritePlan {
    public:
        SnapshotWritePlan() = default;
        ~SnapshotWritePlan();
        SnapshotWritePlan(SnapshotWritePlan&& other) noexcept;
        SnapshotWritePlan& operator=(SnapshotWritePlan&& other) noexcept;
        SnapshotWritePlan(const SnapshotWritePlan&) = delete;
        SnapshotWritePlan& operator=(const SnapshotWritePlan&) = delete;

        [[nodiscard]] std::span<const SnapshotRange> WriteRanges() const noexcept;
        [[nodiscard]] std::span<const SnapshotRange> AliasRanges() const noexcept;
        [[nodiscard]] std::span<const SnapshotRange> SiblingAliasRanges() const noexcept;

    private:
        friend class MemoryManager;
        void Reset() noexcept;

        std::vector<SnapshotRange> owned_write_ranges;
        std::vector<SnapshotRange> owned_alias_ranges;
        std::vector<SnapshotRange> owned_sibling_alias_ranges;
        std::shared_ptr<const SnapshotWritePlanData> cached_data;
        // The D70 cache is thread_local. A non-atomic lease pins one immutable
        // entry until this stack plan is destroyed; a colliding build never
        // overwrites an entry whose lease count is non-zero.
        const SnapshotWritePlanData* thread_local_cached_data{};
        u32* thread_local_cache_leases{};
    };

    struct SnapshotAccessDiagnostic {
        u32 reason{};
        VAddr cursor{};
        VAddr area_base{};
        u64 area_size{};
        VMAType area_type{VMAType::Free};
        MemoryProt area_protection{MemoryProt::NoAccess};
    };

    explicit MemoryManager();
    ~MemoryManager();

    void SetRasterizer(Vulkan::Rasterizer* rasterizer_,
                       bool discard_released_backing_ = false);

    u64 GrowExecutorLibcMspace(u64 required_size);

    AddressSpace& GetAddressSpace() {
        return impl;
    }

    u64 GetTotalDirectSize() const {
        return total_direct_size;
    }

    u64 GetTotalFlexibleSize() const {
        return total_flexible_size;
    }

    u64 GetAvailableFlexibleSize() const {
        return total_flexible_size - flexible_usage;
    }

    VAddr ExecutorLibcMspaceBase() const noexcept {
#if defined(LSX4_APPLE_RUNTIME)
        return executor_libc_mspace_base;
#else
        return EXECUTOR_LIBC_MSPACE_BASE;
#endif
    }

    VAddr SystemReservedVirtualBase() noexcept {
        return impl.SystemReservedVirtualBase();
    }

    bool IsValidGpuMapping(VAddr virtual_addr, u64 size) {
        const VAddr max_gpu_address{0x10000000000};
        return virtual_addr + size < max_gpu_address;
    }

    bool IsValidMapping(const VAddr virtual_addr, const u64 size = 0) {
        if (vma_map.empty()) {
            return false;
        }

        auto vma_handle = vma_map.upper_bound(virtual_addr);
        if (vma_handle == vma_map.begin()) {
            return false;
        }
        --vma_handle;
        const auto& first_vma = vma_handle->second;
        if (first_vma.size > std::numeric_limits<VAddr>::max() - first_vma.base) {
            return false;
        }
        const VAddr first_end = first_vma.base + first_vma.size;
        if (virtual_addr < first_vma.base || virtual_addr >= first_end) {
            return false;
        }

        if (size == 0) {
            return true;
        }
        if (virtual_addr > std::numeric_limits<VAddr>::max() - size) {
            return false;
        }

        const VAddr range_end = virtual_addr + size;
        VAddr cursor = virtual_addr;
        while (cursor < range_end) {
            if (vma_handle == vma_map.end()) {
                return false;
            }
            const auto& vma = vma_handle->second;
            if (vma.size > std::numeric_limits<VAddr>::max() - vma.base) {
                return false;
            }
            const VAddr vma_end = vma.base + vma.size;
            if (cursor < vma.base || cursor >= vma_end) {
                return false;
            }

            cursor = std::min(range_end, vma_end);
            if (cursor < range_end) {
                ++vma_handle;
                if (vma_handle == vma_map.end() || vma_handle->second.base != cursor) {
                    return false;
                }
            }
        }

        return true;
    }

    u64 ClampRangeSize(VAddr virtual_addr, u64 size);

    void SetPrtArea(u32 id, VAddr address, u64 size);

    void CopySparseMemory(VAddr source, u8* dest, u64 size);

    bool IsReadableMapping(VAddr virtual_addr, u64 size, bool* address_managed = nullptr);

    bool TryCopyReadableMemory(VAddr virtual_addr, void* dest, u64 size,
                               bool gpu_backing_synchronized = false);

    bool TryCopyReadableMemory(const SnapshotReadGuard& guard,
                               VAddr virtual_addr, void* dest, u64 size,
                               bool gpu_backing_synchronized = false);

    bool TryCopyBackingMemory(VAddr virtual_addr, void* dest, u64 size);

    [[nodiscard]] SnapshotReadGuard AcquireSnapshotReadGuard() {
        return SnapshotReadGuard{*this};
    }

    [[nodiscard]] bool SnapshotGpuTrackingActive(
        const SnapshotReadGuard& guard) const noexcept;

    [[nodiscard]] bool ValidateSnapshotAccess(const SnapshotReadGuard& guard,
                                              VAddr virtual_addr, u64 size,
                                              bool read, bool write,
                                              SnapshotAccessDiagnostic* diagnostic = nullptr) const;

    bool TryCopyBackingMemory(const SnapshotReadGuard& guard,
                              VAddr virtual_addr, void* dest, u64 size);

    bool BuildSnapshotWriteRanges(
        const SnapshotReadGuard& guard, VAddr virtual_addr, u64 size,
        std::vector<SnapshotRange>& write_ranges,
        std::vector<SnapshotRange>& alias_ranges);

    bool BuildSnapshotWritePlan(const SnapshotReadGuard& guard,
                                VAddr virtual_addr, u64 size,
                                SnapshotWritePlan& plan);

    enum class SnapshotSiblingPlanResult : u8 {
        Failed,
        AliasFree,
        HasSiblingAliases,
    };

    // Build the ordinary immutable snapshot plan unless an exact
    // manager+VMA-generation+VA+size entry has already proven that the range
    // has no sibling aliases. Alias-free hits intentionally return an empty
    // plan; callers must branch on the result before inspecting it.
    [[nodiscard]] SnapshotSiblingPlanResult BuildSnapshotSiblingPlan(
        const SnapshotReadGuard& guard, VAddr virtual_addr, u64 size,
        SnapshotWritePlan& plan);

    // Resolves the virtual mappings which cover an exact physical backing
    // range. This is intentionally a cold-path query for command packets that
    // name backing memory directly instead of using one of its guest VAs.
    bool FindMappedBackingAliases(PAddr physical_addr, u64 size,
                                  std::vector<VAddr>& aliases);

    [[nodiscard]] bool PhysicalAliasingMayExist() const noexcept {
        return physical_aliasing_may_exist.load(std::memory_order_acquire);
    }

    // Read-only diagnostic generation used to correlate a command-stream
    // snapshot with changes to the guest virtual/physical alias topology.
    [[nodiscard]] u64 DebugVmaGeneration() const noexcept {
        return vma_generation.load(std::memory_order_acquire);
    }

    bool IsWritableMapping(VAddr virtual_addr, u64 size);

    bool TryWriteBacking(void* address, const void* data, u64 size);
    // The guard pins VMA topology; GPU ownership must separately serialize the
    // byte publication. Never upgrade its shared lock while reading back GPU data.
    bool TryWriteBacking(const SnapshotReadGuard& guard, void* address,
                         const void* data, u64 size);

    struct ReplayRange {
        VAddr base;
        u64 size;
        const u8* backing;
    };
    void RegisterReplayMemory(VAddr base, u64 size, const u8* backing);
    void ClearReplayMemory();
    const ReplayRange* FindReplayRange(VAddr addr) const;

    void RegisterReplayCanary(VAddr addr, u32 expected);
    void VerifyReplayCanaries(const char* where);

    void SetupMemoryRegions(u64 flexible_size, bool use_extended_mem1, bool use_extended_mem2);

    PAddr PoolExpand(PAddr search_start, PAddr search_end, u64 size, u64 alignment);

    PAddr Allocate(PAddr search_start, PAddr search_end, u64 size, u64 alignment, s32 memory_type);

    s32 Free(PAddr phys_addr, u64 size, bool is_checked);

    s32 PoolCommit(VAddr virtual_addr, u64 size, MemoryProt prot, s32 mtype);

    s32 MapMemory(void** out_addr, VAddr virtual_addr, u64 size, MemoryProt prot,
                  MemoryMapFlags flags, VMAType type, std::string_view name = "anon",
                  bool validate_dmem = false, PAddr phys_addr = -1, u64 alignment = 0);

    s32 MapFile(void** out_addr, VAddr virtual_addr, u64 size, MemoryProt prot,
                MemoryMapFlags flags, s32 fd, s64 phys_addr);

    s32 PoolDecommit(VAddr virtual_addr, u64 size);

    s32 UnmapMemory(VAddr virtual_addr, u64 size);

    s32 QueryProtection(VAddr addr, void** start, void** end, u32* prot);

    s32 Protect(VAddr addr, u64 size, MemoryProt prot);

    s64 ProtectBytes(VAddr addr, VirtualMemoryArea& vma_base, u64 size, MemoryProt prot);

    s32 VirtualQuery(VAddr addr, s32 flags, ::Libraries::Kernel::OrbisVirtualQueryInfo* info);

    s32 DirectMemoryQuery(PAddr addr, bool find_next,
                          ::Libraries::Kernel::OrbisQueryInfo* out_info);

    s32 DirectQueryAvailable(PAddr search_start, PAddr search_end, u64 alignment,
                             PAddr* phys_addr_out, u64* size_out);

    s32 GetDirectMemoryType(PAddr addr, s32* directMemoryTypeOut, void** directMemoryStartOut,
                            void** directMemoryEndOut);

    s32 IsStack(VAddr addr, void** start, void** end);

    s32 SetDirectMemoryType(VAddr addr, u64 size, s32 memory_type);

    void NameVirtualRange(VAddr virtual_addr, u64 size, std::string_view name);

    s32 GetMemoryPoolStats(::Libraries::Kernel::OrbisKernelMemoryPoolBlockStats* stats);

    void InvalidateMemory(VAddr addr, u64 size) const;
private:
#if defined(LSX4_APPLE_RUNTIME)
    VAddr executor_libc_mspace_base{};
#endif
    VMAHandle FindVMA(VAddr target) {
        return std::prev(vma_map.upper_bound(target));
    }

    PhysHandle FindDmemArea(PAddr target) {
        return std::prev(dmem_map.upper_bound(target));
    }

    PhysHandle FindFmemArea(PAddr target) {
        return std::prev(fmem_map.upper_bound(target));
    }

    bool HasPhysicalBacking(VirtualMemoryArea vma) {
        return vma.type == VMAType::Direct || vma.type == VMAType::Flexible ||
               vma.type == VMAType::Pooled;
    }

    VMAHandle CreateArea(VAddr virtual_addr, u64 size, MemoryProt prot, MemoryMapFlags flags,
                         VMAType type, std::string_view name, u64 alignment);

    VAddr SearchFree(VAddr virtual_addr, u64 size, u32 alignment);

    VMAHandle MergeAdjacent(VMAMap& map, VMAHandle iter);

    PhysHandle MergeAdjacent(PhysMap& map, PhysHandle iter);

    VMAHandle CarveVMA(VAddr virtual_addr, u64 size);

    PhysHandle CarvePhysArea(PhysMap& map, PAddr addr, u64 size);

    VMAHandle Split(VMAHandle vma_handle, u64 offset_in_vma);

    PhysHandle Split(PhysMap& map, PhysHandle dmem_handle, u64 offset_in_area);

    u64 UnmapBytesFromEntry(VAddr virtual_addr, VirtualMemoryArea vma_base, u64 size);

    s32 UnmapMemoryImpl(VAddr virtual_addr, u64 size);

    bool TryWriteBackingLocked(void* address, const void* data, u64 size);
    bool TryCopyBackingMemoryLocked(VAddr virtual_addr, void* dest, u64 size);
    bool TryCopyReadableMemoryLocked(VAddr virtual_addr, void* dest, u64 size,
                                     bool gpu_backing_synchronized);
    bool BuildSnapshotWriteRangesUncached(
        const SnapshotReadGuard& guard, VAddr virtual_addr, u64 size,
        std::vector<SnapshotRange>& write_ranges,
        std::vector<SnapshotRange>& alias_ranges);
    bool RebuildWritableAliasCacheLocked();
    bool TryDiscardReleasedBacking(PAddr phys_addr, u64 size,
                                   const char* source) noexcept;
    void BumpVmaGeneration() noexcept {
        vma_generation.fetch_add(1, std::memory_order_release);
    }

private:
    AddressSpace impl;
    PhysMap dmem_map;
    PhysMap fmem_map;
    VMAMap vma_map;
    std::vector<ReplayRange> replay_ranges;
    struct ReplayCanary {
        VAddr addr;
        u32 expected;
    };
    std::vector<ReplayCanary> replay_canaries;
    Common::SharedFirstMutex mutex{};
    std::mutex unmap_mutex{};
    std::atomic<bool> physical_aliasing_may_exist{false};
    struct WritableAliasRecord {
        PAddr physical_begin{};
        PAddr physical_end{};
        VAddr virtual_begin{};
        bool cpu_writable{};
    };
    std::mutex alias_cache_mutex{};
    u64 alias_cache_generation{};
    bool writable_alias_cache_valid{true};
    std::vector<WritableAliasRecord> writable_alias_cache;
    std::vector<PAddr> writable_alias_prefix_max_end;
    std::atomic<u64> vma_generation{1};
    u64 snapshot_plan_cache_identity{};
    u64 total_direct_size{};
    u64 total_flexible_size{};
    u64 flexible_usage{};
    u64 pool_budget{};
    s32 sdk_version{};
    Vulkan::Rasterizer* rasterizer{};
    bool discard_released_backing{};
    u64 discarded_backing_requests{};
    u64 discarded_backing_failures{};
    u64 discarded_backing_requested_bytes{};
    u64 discarded_backing_removed_bytes{};
    u64 executor_libc_mspace_size{};

    struct PrtArea {
        VAddr start;
        VAddr end;
        bool mapped;

        bool Overlaps(VAddr test_address, u64 test_size) const {
            const VAddr overlap_end = test_address + test_size;
            return start < overlap_end && test_address < end;
        }
    };
    std::array<PrtArea, 3> prt_areas{};

    friend class ::Core::Devtools::Widget::MemoryMapViewer;
};

using Memory = Common::Singleton<MemoryManager>;

}
