// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <mutex>

#include "common/config.h"
#include "common/div_ceil.h"
#include "common/logging/log.h"

#ifdef __linux__
#include "common/adaptive_mutex.h"
#endif
#if !defined(__linux__) || !defined(PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP)
#include "common/spin_lock.h"
#endif
#include "common/debug.h"
#include "common/types.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/page_manager.h"

namespace VideoCore {

enum class SnapshotOwnership : u8 {
    Unknown,
    Host,
    Gpu,
};

enum class SnapshotGpuConflictDisposition : u8 {
    ResolveGpu,
    PreferHost,
    CpuConflict,
    Unknown,
};

[[nodiscard]] constexpr u64 MakeSnapshotOwnershipStamp(
    const u64 epoch, const SnapshotOwnership ownership) noexcept {
    return (epoch << 2) | static_cast<u64>(ownership);
}

[[nodiscard]] constexpr SnapshotOwnership SnapshotOwnershipFromStamp(
    const u64 stamp) noexcept {
    return static_cast<SnapshotOwnership>(stamp & 3u);
}

#ifdef __ANDROID__
using LockType = Common::ParkingMutex;
#elif defined(PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP)
using LockType = Common::AdaptiveMutex;
#else
using LockType = Common::SpinLock;
#endif

class RegionManager {
public:
    explicit RegionManager(PageManager* tracker_, VAddr cpu_addr_)
        : tracker{tracker_}, cpu_addr{cpu_addr_} {
        cpu.Fill();
        gpu.Clear();
        snapshot_stale.Clear();
        writeable.Fill();
        readable.Fill();
    }
    explicit RegionManager() = default;

    void SetCpuAddress(VAddr new_cpu_addr) {
        cpu_addr = new_cpu_addr;
    }

    VAddr GetCpuAddr() const {
        return cpu_addr;
    }

    static constexpr size_t SanitizeAddress(size_t address) {
        return static_cast<size_t>(std::max<s64>(static_cast<s64>(address), 0LL));
    }

    template <Type type>
    RegionBits& GetRegionBits() noexcept {
        if constexpr (type == Type::CPU) {
            return cpu;
        } else if constexpr (type == Type::GPU) {
            return gpu;
        }
    }

    template <Type type>
    const RegionBits& GetRegionBits() const noexcept {
        if constexpr (type == Type::CPU) {
            return cpu;
        } else if constexpr (type == Type::GPU) {
            return gpu;
        }
    }

    template <Type type, bool enable>
    void ChangeRegionState(u64 dirty_addr, u64 size,
                           u64 ownership_stamp = 0) noexcept(type == Type::GPU) {
        RENDERER_TRACE;
        const size_t offset = dirty_addr - cpu_addr;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return;
        }

        RegionBits& bits = GetRegionBits<type>();
        if constexpr (enable) {
            bits.SetRange(start_page, end_page);
            if constexpr (type == Type::CPU || type == Type::GPU) {
                std::fill(ownership_stamps.begin() + start_page,
                          ownership_stamps.begin() + end_page,
                          ownership_stamp);
            }
        } else {
            bits.UnsetRange(start_page, end_page);
        }
        if constexpr (type == Type::CPU) {
            UpdateProtection<!enable, false>();
        } else if (Config::getReadbacksMode() == Config::GpuReadbacksMode::Precise) {
            UpdateProtection<enable, true>();
        }
    }

    template <Type type, bool clear>
    void ForEachModifiedRange(VAddr query_cpu_range, s64 size, auto&& func) {
        RENDERER_TRACE;
        const size_t offset = query_cpu_range - cpu_addr;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return;
        }

        RegionBits& bits = GetRegionBits<type>();
        RegionBits mask(bits, start_page, end_page);

        if constexpr (clear) {
            bits.UnsetRange(start_page, end_page);
            if constexpr (type == Type::CPU) {
                UpdateProtection<true, false>();
            } else if (Config::getReadbacksMode() != Config::GpuReadbacksMode::Disabled) {
                UpdateProtection<false, true>();
            }
        }

        for (const auto& [start, end] : mask) {
            func(cpu_addr + start * TRACKER_BYTES_PER_PAGE, (end - start) * TRACKER_BYTES_PER_PAGE);
        }
    }

    void MarkSnapshotStale(VAddr dirty_addr, u64 size,
                           u64 ownership_stamp) noexcept {
        const size_t offset = dirty_addr - cpu_addr;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return;
        }
        snapshot_stale.SetRange(start_page, end_page);
        std::fill(ownership_stamps.begin() + start_page,
                  ownership_stamps.begin() + end_page, ownership_stamp);
    }

    void MarkSnapshotStale(VAddr dirty_addr, u64 size) noexcept {
        MarkSnapshotStale(dirty_addr, size, 0);
    }

    void SetSnapshotOwnershipStamp(VAddr dirty_addr, u64 size,
                                   u64 ownership_stamp) noexcept {
        const size_t offset = dirty_addr - cpu_addr;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return;
        }
        std::fill(ownership_stamps.begin() + start_page,
                  ownership_stamps.begin() + end_page, ownership_stamp);
    }

    void ClearSnapshotStale(VAddr clean_addr, u64 size) noexcept {
        const size_t offset = clean_addr - cpu_addr;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return;
        }
        snapshot_stale.UnsetRange(start_page, end_page);
    }

    [[nodiscard]] bool IsRegionSnapshotStale(u64 offset, u64 size) const noexcept {
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        return start_page < NUM_PAGES_PER_REGION && end_page > start_page &&
               snapshot_stale.AnyInRange(start_page, end_page);
    }

    void ForEachUploadRangeReadOnly(VAddr query_cpu_range, s64 size,
                                    bool include_full_range, auto&& func) const {
        const size_t offset = query_cpu_range - cpu_addr;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return;
        }

        RegionBits upload_mask;
        if (include_full_range) {
            upload_mask.SetRange(start_page, end_page);
        } else {
            upload_mask = RegionBits(snapshot_stale, start_page, end_page);
            upload_mask |= RegionBits(cpu, start_page, end_page);
        }
        for (const auto& [start, end] : upload_mask) {
            func(cpu_addr + start * TRACKER_BYTES_PER_PAGE,
                 (end - start) * TRACKER_BYTES_PER_PAGE);
        }
    }

    void ForEachUploadRange(VAddr query_cpu_range, s64 size, auto&& func) {
        const size_t offset = query_cpu_range - cpu_addr;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return;
        }

        RegionBits cpu_mask(cpu, start_page, end_page);
        RegionBits upload_mask(snapshot_stale, start_page, end_page);
        upload_mask |= cpu_mask;
        snapshot_stale |= cpu_mask;
        RegionBits retained_cpu_mask(read_only_streaming, start_page, end_page);
        retained_cpu_mask &= cpu_mask;
        cpu.UnsetRange(start_page, end_page);
        cpu |= retained_cpu_mask;
        UpdateProtection<true, false>();

        for (const auto& [start, end] : upload_mask) {
            func(cpu_addr + start * TRACKER_BYTES_PER_PAGE,
                 (end - start) * TRACKER_BYTES_PER_PAGE);
        }
    }

    [[nodiscard]] RegionBits CaptureCpuUploadMask(u64 offset, u64 size) const noexcept {
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        return RegionBits(cpu, start_page, std::min<size_t>(end_page, NUM_PAGES_PER_REGION));
    }

    void DemoteReadOnlyStreaming(u64 offset, u64 size) noexcept {
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page = std::min<size_t>(
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE),
            NUM_PAGES_PER_REGION);
        if (start_page >= end_page) {
            return;
        }
        read_only_streaming.UnsetRange(start_page, end_page);
        std::fill(read_only_stream_heat.begin() + start_page,
                  read_only_stream_heat.begin() + end_page, 0);
    }

    [[nodiscard]] u64 CommitReadOnlyStreamingUpload(
        u64 offset, u64 size, const RegionBits& uploaded_cpu_mask) {
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page = std::min<size_t>(
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE),
            NUM_PAGES_PER_REGION);
        if (start_page >= end_page) {
            return 0;
        }
        u64 promoted_pages = 0;
        bool writeability_changed = false;
        for (size_t page = start_page; page < end_page; ++page) {
            if (!uploaded_cpu_mask.Get(page)) {
                continue;
            }
            u8& heat = read_only_stream_heat[page];
            heat = std::min<u8>(3, static_cast<u8>(heat + 1));
            if (heat != 3 || read_only_streaming.Get(page)) {
                continue;
            }
            read_only_streaming.Set(page);
            cpu.Set(page);
            writeability_changed = true;
            ++promoted_pages;
        }
        if (writeability_changed) {
            UpdateProtection<false, false>();
        }
        return promoted_pages;
    }

    template <Type type>
    [[nodiscard]] bool IsRegionModified(u64 offset, u64 size) noexcept {
        RENDERER_TRACE;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return false;
        }

        const RegionBits& bits = GetRegionBits<type>();
        RegionBits test(bits, start_page, end_page);
        return test.Any();
    }

    template <Type type>
    [[nodiscard]] bool IsRegionModifiedDirect(u64 offset, u64 size) noexcept {
        RENDERER_TRACE;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return false;
        }

        // Exact equivalent of constructing a range-masked RegionBits and
        // calling Any(), without materializing and scanning the zero-filled
        // words outside the query. Dirty ownership and mutation stay under
        // the existing RegionManager lock held by MemoryTracker.
        return GetRegionBits<type>().AnyInRange(start_page, end_page);
    }

    [[nodiscard]] bool HasGpuDirtyUploadOverlap(u64 offset, u64 size) noexcept {
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return false;
        }
        RegionBits upload_mask(cpu, start_page, end_page);
        const RegionBits stale_mask(snapshot_stale, start_page, end_page);
        upload_mask |= stale_mask;
        const RegionBits gpu_mask(gpu, start_page, end_page);
        upload_mask &= gpu_mask;
        return upload_mask.Any();
    }

    template <typename Func>
    [[nodiscard]] u64 ForEachSnapshotGpuConflictRange(
        u64 offset, u64 size, Func&& func) const noexcept {
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return 0;
        }

        const size_t clamped_end = std::min<size_t>(end_page, NUM_PAGES_PER_REGION);
        u64 classified_bytes = 0;
        size_t run_start = start_page;
        SnapshotGpuConflictDisposition run_disposition{};
        bool have_run = false;
        const auto flush_run = [&](const size_t run_end) {
            if (!have_run || run_start >= run_end) {
                return;
            }
            const VAddr begin = std::max<VAddr>(
                cpu_addr + run_start * TRACKER_BYTES_PER_PAGE,
                cpu_addr + offset);
            const VAddr end = std::min<VAddr>(
                cpu_addr + run_end * TRACKER_BYTES_PER_PAGE,
                cpu_addr + offset + size);
            if (begin < end) {
                func(begin, end - begin, run_disposition);
                classified_bytes += end - begin;
            }
        };

        for (size_t page = start_page; page < clamped_end; ++page) {
            if (!gpu.Get(page)) {
                flush_run(page);
                have_run = false;
                continue;
            }
            const SnapshotGpuConflictDisposition disposition = [&] {
                if (cpu.Get(page)) {
                    // CPU and GPU dirty bits can legitimately overlap until
                    // the next synchronization transaction.  The monotonic
                    // ownership stamp records which publication happened
                    // last, so resolve the overlap instead of discarding the
                    // whole draw.  A zero/unknown stamp remains fail-closed.
                    switch (SnapshotOwnershipFromStamp(ownership_stamps[page])) {
                    case SnapshotOwnership::Host:
                        return SnapshotGpuConflictDisposition::PreferHost;
                    case SnapshotOwnership::Gpu:
                        return SnapshotGpuConflictDisposition::ResolveGpu;
                    case SnapshotOwnership::Unknown:
                        return SnapshotGpuConflictDisposition::CpuConflict;
                    }
                    return SnapshotGpuConflictDisposition::CpuConflict;
                }
                if (!snapshot_stale.Get(page)) {
                    return SnapshotGpuConflictDisposition::ResolveGpu;
                }
                switch (SnapshotOwnershipFromStamp(ownership_stamps[page])) {
                case SnapshotOwnership::Host:
                    return SnapshotGpuConflictDisposition::PreferHost;
                case SnapshotOwnership::Gpu:
                    return SnapshotGpuConflictDisposition::ResolveGpu;
                case SnapshotOwnership::Unknown:
                    return SnapshotGpuConflictDisposition::Unknown;
                }
                return SnapshotGpuConflictDisposition::Unknown;
            }();
            if (!have_run || disposition != run_disposition) {
                flush_run(page);
                run_start = page;
                run_disposition = disposition;
                have_run = true;
            }
        }
        flush_run(clamped_end);
        return classified_bytes;
    }

    LockType lock;

private:
    template <bool track, bool is_read>
    void UpdateProtection() {
        RENDERER_TRACE;
        RegionBits mask = is_read ? (~gpu ^ readable) : (cpu ^ writeable);
        if (mask.None()) {
            return;
        }
        if constexpr (is_read) {
            readable = ~gpu;
        } else {
            writeable = cpu;
        }
        tracker->UpdatePageWatchersForRegion<track, is_read>(cpu_addr, mask);
    }

    PageManager* tracker;
    VAddr cpu_addr = 0;
    RegionBits cpu;
    RegionBits gpu;
    RegionBits snapshot_stale;
    RegionBits read_only_streaming;
    std::array<u8, NUM_PAGES_PER_REGION> read_only_stream_heat{};
    std::array<u64, NUM_PAGES_PER_REGION> ownership_stamps{};
    RegionBits writeable;
    RegionBits readable;
};

}
