// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>
#include <boost/container/small_vector.hpp>

#include "common/debug.h"
#include "common/scope_exit.h"
#include "common/types.h"
#include "video_core/buffer_cache/region_manager.h"

namespace VideoCore {

[[nodiscard]] bool ExecutorDirectRegionModifiedRangeEnabled() noexcept;
[[nodiscard]] bool ExecutorSm8650CombinedCpuSnapshotQueryEnabled() noexcept;
void RecordDescriptorProtectionCohortTransaction(u64 write_ranges) noexcept;

enum class SnapshotTransactionResult : u8 {
    Complete,
    NoUpload,
    GpuDirty,
    Busy,
    Overlap,
    Unsupported,
    Invalid,
    CopyFailed,
};

enum class SnapshotTransactionAction : u8 {
    Complete,
    ResolveGpu,
    Retry,
    FailClosed,
};

[[nodiscard]] constexpr SnapshotTransactionAction DecideSnapshotTransactionAction(
    const SnapshotTransactionResult result) noexcept {
    switch (result) {
    case SnapshotTransactionResult::Complete:
    case SnapshotTransactionResult::NoUpload:
        return SnapshotTransactionAction::Complete;
    case SnapshotTransactionResult::GpuDirty:
        return SnapshotTransactionAction::ResolveGpu;
    case SnapshotTransactionResult::Busy:
    case SnapshotTransactionResult::Overlap:
        return SnapshotTransactionAction::Retry;
    case SnapshotTransactionResult::Unsupported:
    case SnapshotTransactionResult::Invalid:
    case SnapshotTransactionResult::CopyFailed:
        return SnapshotTransactionAction::FailClosed;
    }
    return SnapshotTransactionAction::FailClosed;
}

[[nodiscard]] constexpr SnapshotTransactionResult SnapshotTrackFailureResult(
    const DescriptorSnapshotTrackResult result) noexcept {
    switch (result) {
    case DescriptorSnapshotTrackResult::Tracked:
        return SnapshotTransactionResult::Complete;
    case DescriptorSnapshotTrackResult::Busy:
        return SnapshotTransactionResult::Busy;
    case DescriptorSnapshotTrackResult::Overlap:
        return SnapshotTransactionResult::Overlap;
    case DescriptorSnapshotTrackResult::Unsupported:
        return SnapshotTransactionResult::Unsupported;
    case DescriptorSnapshotTrackResult::Invalid:
        return SnapshotTransactionResult::Invalid;
    }
    return SnapshotTransactionResult::Invalid;
}

class MemoryTracker {
public:
    static constexpr size_t MAX_CPU_PAGE_BITS = 40;
    static constexpr size_t NUM_HIGH_PAGES = 1ULL << (MAX_CPU_PAGE_BITS - TRACKER_HIGHER_PAGE_BITS);
    static constexpr size_t MANAGER_POOL_SIZE = 32;

public:
    explicit MemoryTracker(PageManager& tracker_)
        : tracker{&tracker_},
          direct_region_modified_range{
              ExecutorDirectRegionModifiedRangeEnabled()},
          combined_cpu_snapshot_query{
              ExecutorSm8650CombinedCpuSnapshotQueryEnabled()} {
        for (auto& manager : published_top_tier) {
            manager.store(nullptr, std::memory_order_relaxed);
        }
    }
    ~MemoryTracker() = default;

    bool IsRegionCpuModified(VAddr query_cpu_addr, u64 query_size) noexcept {
        if (direct_region_modified_range) {
            return IteratePages<true>(
                query_cpu_addr, query_size,
                [](RegionManager* manager, u64 offset, size_t size) {
                    std::scoped_lock lk{manager->lock};
                    return manager->template IsRegionModifiedDirect<Type::CPU>(
                        offset, size);
                });
        }
        return IteratePages<true>(
            query_cpu_addr, query_size, [](RegionManager* manager, u64 offset, size_t size) {
                std::scoped_lock lk{manager->lock};
                return manager->template IsRegionModified<Type::CPU>(offset, size);
            });
    }

    bool IsRegionGpuModified(VAddr query_cpu_addr, u64 query_size) noexcept {
        if (direct_region_modified_range) {
            return IteratePages<false>(
                query_cpu_addr, query_size,
                [](RegionManager* manager, u64 offset, size_t size) {
                    std::scoped_lock lk{manager->lock};
                    return manager->template IsRegionModifiedDirect<Type::GPU>(
                        offset, size);
                });
        }
        return IteratePages<false>(
            query_cpu_addr, query_size, [](RegionManager* manager, u64 offset, size_t size) {
                std::scoped_lock lk{manager->lock};
                return manager->template IsRegionModified<Type::GPU>(offset, size);
            });
    }

    bool IsRegionSnapshotStale(VAddr query_cpu_addr, u64 query_size) noexcept {
        return IteratePages<false>(
            query_cpu_addr, query_size,
            [](RegionManager* manager, u64 offset, size_t size) {
                std::scoped_lock lk{manager->lock};
                return manager->IsRegionSnapshotStale(offset, size);
            });
    }

    bool IsRegionCpuModifiedOrSnapshotStale(
        VAddr query_cpu_addr, u64 query_size) noexcept {
        if (!combined_cpu_snapshot_query) {
            return IsRegionCpuModified(query_cpu_addr, query_size) ||
                   IsRegionSnapshotStale(query_cpu_addr, query_size);
        }
        return IteratePages<true>(
            query_cpu_addr, query_size,
            [this](RegionManager* manager, u64 offset, size_t size) {
                std::scoped_lock lk{manager->lock};
                const bool cpu_modified =
                    direct_region_modified_range
                        ? manager->template IsRegionModifiedDirect<Type::CPU>(
                              offset, size)
                        : manager->template IsRegionModified<Type::CPU>(
                              offset, size);
                return cpu_modified ||
                       manager->IsRegionSnapshotStale(offset, size);
            });
    }

    bool AreSnapshotRangesDirty(const auto& snapshot_write_ranges) noexcept {
        for (const auto& [range_addr, range_size] : snapshot_write_ranges) {
            if (IsRegionCpuModifiedOrSnapshotStale(range_addr, range_size)) {
                return true;
            }
        }
        return false;
    }

    bool AreSnapshotRangesGpuDirty(const auto& snapshot_write_ranges) noexcept {
        for (const auto& [range_addr, range_size] : snapshot_write_ranges) {
            if (IsRegionGpuModified(range_addr, range_size)) {
                return true;
            }
        }
        return false;
    }

    void MarkSnapshotRangesStale(const auto& snapshot_write_ranges) noexcept {
        const u64 ownership_stamp =
            NextSnapshotOwnershipStamp(SnapshotOwnership::Host);
        for (const auto& [range_addr, range_size] : snapshot_write_ranges) {
            IteratePages<false>(
                range_addr, range_size,
                [ownership_stamp](RegionManager* manager, u64 offset, size_t size) {
                    std::scoped_lock lk{manager->lock};
                    manager->MarkSnapshotStale(
                        manager->GetCpuAddr() + offset, size, ownership_stamp);
                });
        }
    }

    void ClearSnapshotRangesStale(const auto& snapshot_ranges) noexcept {
        for (const auto& [range_addr, range_size] : snapshot_ranges) {
            IteratePages<false>(
                range_addr, range_size,
                [](RegionManager* manager, u64 offset, size_t size) {
                    std::scoped_lock lk{manager->lock};
                    manager->ClearSnapshotStale(
                        manager->GetCpuAddr() + offset, size);
                });
        }
    }

    void MarkRegionAsCpuModified(VAddr dirty_cpu_addr, u64 query_size) {
        const u64 ownership_stamp =
            NextSnapshotOwnershipStamp(SnapshotOwnership::Host);
        IteratePages<false>(dirty_cpu_addr, query_size,
                            [ownership_stamp](RegionManager* manager, u64 offset,
                                              size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->template ChangeRegionState<Type::CPU, true>(
                                    manager->GetCpuAddr() + offset, size,
                                    ownership_stamp);
                            });
    }

    void UnmarkRegionAsGpuModified(VAddr dirty_cpu_addr, u64 query_size) noexcept {
        IteratePages<false>(dirty_cpu_addr, query_size,
                            [](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->template ChangeRegionState<Type::GPU, false>(
                                    manager->GetCpuAddr() + offset, size);
                            });
        tracker->UpdateGpuDirtyPages(dirty_cpu_addr, query_size, false);
    }

    void MarkRegionAsGpuModified(VAddr dirty_cpu_addr, u64 query_size) noexcept {
        const u64 ownership_stamp =
            NextSnapshotOwnershipStamp(SnapshotOwnership::Gpu);
        tracker->UpdateGpuDirtyPages(dirty_cpu_addr, query_size, true);
        IteratePages<false>(dirty_cpu_addr, query_size,
                            [ownership_stamp](RegionManager* manager, u64 offset,
                                              size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->template ChangeRegionState<Type::GPU, true>(
                                    manager->GetCpuAddr() + offset, size,
                                    ownership_stamp);
                            });
    }

    [[nodiscard]] CpuOwnedWritePublishResult
    TryPublishCpuOwnedWriteIfGpuClean(VAddr dirty_cpu_addr, u64 query_size) {
        if (query_size == 0 ||
            dirty_cpu_addr > std::numeric_limits<VAddr>::max() - query_size) {
            return CpuOwnedWritePublishResult::MissingManager;
        }

        struct Slice {
            RegionManager* manager;
            u64 offset;
            size_t size;
        };
        boost::container::small_vector<Slice, 2> slices;
        boost::container::small_vector<RegionManager*, 2> managers;
        size_t remaining_size = query_size;
        size_t page_index = dirty_cpu_addr >> TRACKER_HIGHER_PAGE_BITS;
        u64 page_offset = dirty_cpu_addr & TRACKER_HIGHER_PAGE_MASK;
        while (remaining_size != 0) {
            if (page_index >= NUM_HIGH_PAGES) {
                return CpuOwnedWritePublishResult::MissingManager;
            }
            RegionManager* const manager = published_top_tier[page_index].load(
                std::memory_order_acquire);
            if (manager == nullptr) {
                return CpuOwnedWritePublishResult::MissingManager;
            }
            const size_t slice_size = std::min<size_t>(
                TRACKER_HIGHER_PAGE_SIZE - page_offset, remaining_size);
            slices.push_back({manager, page_offset, slice_size});
            managers.push_back(manager);
            ++page_index;
            page_offset = 0;
            remaining_size -= slice_size;
        }

        std::ranges::sort(managers, {}, &RegionManager::GetCpuAddr);
        managers.erase(std::ranges::unique(managers).begin(), managers.end());
        size_t locked_count = 0;
        SCOPE_EXIT {
            while (locked_count != 0) {
                managers[--locked_count]->lock.unlock();
            }
        };
        for (RegionManager* const manager : managers) {
            if (!manager->lock.try_lock()) {
                return CpuOwnedWritePublishResult::Busy;
            }
            ++locked_count;
        }

        for (const Slice& slice : slices) {
            if (slice.manager->template IsRegionModified<Type::GPU>(
                    slice.offset, slice.size)) {
                return CpuOwnedWritePublishResult::GpuDirty;
            }
        }

        const u64 ownership_stamp =
            NextSnapshotOwnershipStamp(SnapshotOwnership::Host);
        for (const Slice& slice : slices) {
            const VAddr address = slice.manager->GetCpuAddr() + slice.offset;
            slice.manager->MarkSnapshotStale(address, slice.size,
                                             ownership_stamp);
            slice.manager->template ChangeRegionState<Type::CPU, true>(
                address, slice.size, ownership_stamp);
        }
        return CpuOwnedWritePublishResult::Complete;
    }

    void InvalidateRegion(VAddr cpu_addr, u64 size, auto&& on_flush) noexcept {
        const u64 ownership_stamp =
            NextSnapshotOwnershipStamp(SnapshotOwnership::Host);
        IteratePages<false>(
            cpu_addr, size, [&on_flush, ownership_stamp](RegionManager* manager,
                                                        u64 offset, size_t size) {
                const bool should_flush = [&] {
                    std::scoped_lock lk{manager->lock};
                    if (Config::getReadbacksMode() != Config::GpuReadbacksMode::Disabled &&
                        manager->template IsRegionModified<Type::GPU>(offset, size)) {
                        return true;
                    }
                    manager->template ChangeRegionState<Type::CPU, true>(
                        manager->GetCpuAddr() + offset, size,
                        ownership_stamp);
                    return false;
                }();
                if (should_flush) {
                    on_flush();
                }
            });
    }

    SnapshotTransactionResult MeasureUploadRanges(
        VAddr query_cpu_range, u64 query_size,
        const auto& snapshot_alias_ranges, auto&& func) {
        struct Slice {
            RegionManager* manager;
            u64 offset;
            size_t size;
        };
        boost::container::small_vector<Slice, 4> slices;
        boost::container::small_vector<Slice, 8> snapshot_slices;
        boost::container::small_vector<RegionManager*, 8> locked_managers;
        IteratePages<true>(
            query_cpu_range, query_size,
            [&slices, &locked_managers](RegionManager* manager, u64 offset,
                                        size_t size) {
                slices.push_back({manager, offset, size});
                locked_managers.push_back(manager);
            });
        for (const auto& [range_addr, range_size] : snapshot_alias_ranges) {
            IteratePages<true>(
                range_addr, range_size,
                [&snapshot_slices, &locked_managers](RegionManager* manager,
                                                     u64 offset, size_t size) {
                    snapshot_slices.push_back({manager, offset, size});
                    locked_managers.push_back(manager);
                });
        }
        std::ranges::sort(locked_managers, {}, &RegionManager::GetCpuAddr);
        const auto unique_end = std::ranges::unique(locked_managers).begin();
        locked_managers.erase(unique_end, locked_managers.end());
        for (RegionManager* manager : locked_managers) {
            manager->lock.lock();
        }
        SCOPE_EXIT {
            for (auto it = locked_managers.rbegin(); it != locked_managers.rend(); ++it) {
                (*it)->lock.unlock();
            }
        };

        const auto gpu_dirty = [](const Slice& slice) {
            return slice.manager->template IsRegionModified<Type::GPU>(
                slice.offset, slice.size);
        };
        const auto upload_overlaps_gpu = [](const Slice& slice) {
            return slice.manager->HasGpuDirtyUploadOverlap(slice.offset,
                                                            slice.size);
        };
        const bool gpu_conflict =
            std::ranges::any_of(slices, upload_overlaps_gpu) ||
            std::ranges::any_of(snapshot_slices, gpu_dirty);
        if (gpu_conflict) {
            return SnapshotTransactionResult::GpuDirty;
        }

        const auto snapshot_dirty = [](const Slice& slice) {
            return slice.manager->template IsRegionModified<Type::CPU>(
                       slice.offset, slice.size) ||
                   slice.manager->IsRegionSnapshotStale(slice.offset, slice.size);
        };
        const bool alias_source_dirty =
            std::ranges::any_of(snapshot_slices, snapshot_dirty);
        if (alias_source_dirty && std::ranges::any_of(slices, gpu_dirty)) {
            return SnapshotTransactionResult::GpuDirty;
        }

        bool has_upload_range = false;
        for (const Slice& slice : slices) {
            // A dirty physical sibling makes the complete primary range stale
            // in ForEachUploadRange. Include that prospective state in this
            // read-only plan without publishing an ownership stamp or changing
            // page protection.
            slice.manager->ForEachUploadRangeReadOnly(
                slice.manager->GetCpuAddr() + slice.offset, slice.size,
                alias_source_dirty,
                [&](const u64 upload_addr, const u64 upload_size) {
                    has_upload_range = true;
                    func(upload_addr, upload_size);
                });
        }
        return has_upload_range ? SnapshotTransactionResult::Complete
                                : SnapshotTransactionResult::NoUpload;
    }

    SnapshotTransactionResult ForEachUploadRange(
        VAddr query_cpu_range, u64 query_size, bool is_written,
        bool stable_read_only_candidate, u64* stable_promotions,
        const auto& snapshot_write_ranges, const auto& snapshot_alias_ranges,
        auto&& func, auto&& on_upload) {
        RecordDescriptorProtectionCohortTransaction(
            static_cast<u64>(snapshot_write_ranges.size()));
        struct Slice {
            RegionManager* manager;
            u64 offset;
            size_t size;
        };
        boost::container::small_vector<Slice, 4> slices;
        boost::container::small_vector<Slice, 8> snapshot_slices;
        boost::container::small_vector<RegionManager*, 8> locked_managers;
        IteratePages<true>(
            query_cpu_range, query_size,
            [&slices, &locked_managers](RegionManager* manager, u64 offset, size_t size) {
                slices.push_back({manager, offset, size});
                locked_managers.push_back(manager);
            });
        for (const auto& [range_addr, range_size] : snapshot_alias_ranges) {
            IteratePages<true>(
                range_addr, range_size,
                [&snapshot_slices, &locked_managers](RegionManager* manager,
                                                     u64 offset, size_t size) {
                    snapshot_slices.push_back({manager, offset, size});
                    locked_managers.push_back(manager);
                });
        }
        std::ranges::sort(locked_managers, {}, &RegionManager::GetCpuAddr);
        const auto unique_end = std::ranges::unique(locked_managers).begin();
        locked_managers.erase(unique_end, locked_managers.end());
        for (RegionManager* manager : locked_managers) {
            manager->lock.lock();
        }
        size_t tracked_range_count = 0;
        const auto finish_descriptor_snapshot = [&] {
            bool clean = true;
            while (tracked_range_count != 0) {
                const auto& [range_addr, range_size] =
                    snapshot_write_ranges[--tracked_range_count];
                clean = tracker->UntrackDescriptorSnapshotWrite(
                            range_addr, range_size) &&
                        clean;
            }
            return clean;
        };
        SCOPE_EXIT {
            (void)finish_descriptor_snapshot();
            for (auto it = locked_managers.rbegin(); it != locked_managers.rend(); ++it) {
                (*it)->lock.unlock();
            }
        };

        const u64 gpu_ownership_stamp =
            is_written
                ? NextSnapshotOwnershipStamp(SnapshotOwnership::Gpu)
                : 0;
        const auto publish_gpu_ownership = [&] {
            if (!is_written) {
                return;
            }
            tracker->UpdateGpuDirtyPages(query_cpu_range, query_size, true);
            for (const Slice& slice : slices) {
                slice.manager->template ChangeRegionState<Type::CPU, false>(
                    slice.manager->GetCpuAddr() + slice.offset, slice.size);
                slice.manager->ClearSnapshotStale(
                    slice.manager->GetCpuAddr() + slice.offset, slice.size);
                slice.manager->template ChangeRegionState<Type::GPU, true>(
                    slice.manager->GetCpuAddr() + slice.offset, slice.size,
                    gpu_ownership_stamp);
            }
        };

        const auto gpu_dirty = [](const Slice& slice) {
            return slice.manager->template IsRegionModified<Type::GPU>(
                slice.offset, slice.size);
        };
        const auto upload_overlaps_gpu = [](const Slice& slice) {
            return slice.manager->HasGpuDirtyUploadOverlap(slice.offset,
                                                            slice.size);
        };
        const bool gpu_conflict =
            std::ranges::any_of(slices, upload_overlaps_gpu) ||
            std::ranges::any_of(snapshot_slices, gpu_dirty);
        const bool stable_read_only = stable_read_only_candidate && !is_written &&
                                      snapshot_alias_ranges.empty() && !gpu_conflict;
        if (!stable_read_only) {
            for (const Slice& slice : slices) {
                slice.manager->DemoteReadOnlyStreaming(slice.offset, slice.size);
            }
        }
        if (gpu_conflict) {
            return SnapshotTransactionResult::GpuDirty;
        }

        for (const auto& [range_addr, range_size] : snapshot_write_ranges) {
            const auto track_result = tracker->TryTrackDescriptorSnapshotWrite(
                range_addr, range_size);
            if (track_result == DescriptorSnapshotTrackResult::Tracked) {
                ++tracked_range_count;
                continue;
            }
            while (tracked_range_count != 0) {
                const auto& [tracked_addr, tracked_size] =
                    snapshot_write_ranges[--tracked_range_count];
                (void)tracker->UntrackDescriptorSnapshotWrite(
                    tracked_addr, tracked_size);
            }
            return SnapshotTrackFailureResult(track_result);
        }
        const auto snapshot_dirty = [](const Slice& slice) {
            return slice.manager->template IsRegionModified<Type::CPU>(
                       slice.offset, slice.size) ||
                   slice.manager->IsRegionSnapshotStale(slice.offset, slice.size);
        };
        const bool alias_source_dirty =
            std::ranges::any_of(snapshot_slices, snapshot_dirty);
        // A dirty sibling alias makes the primary Vulkan copy stale, but it
        // does not make every page of a large primary buffer host-owned.  If
        // the primary still contains GPU-newer pages, resolve/publish those
        // exact pages first.  Only after that is it safe to upload the shared
        // backing range without overwriting unrelated GPU results.
        if (alias_source_dirty && std::ranges::any_of(slices, gpu_dirty)) {
            return SnapshotTransactionResult::GpuDirty;
        }
        const u64 alias_source_stamp =
            alias_source_dirty
                ? NextSnapshotOwnershipStamp(SnapshotOwnership::Host)
                : 0;
        for (const Slice& slice : snapshot_slices) {
            slice.manager->template ForEachModifiedRange<Type::CPU, true>(
                slice.manager->GetCpuAddr() + slice.offset, slice.size,
                [manager = slice.manager,
                 alias_source_stamp](const VAddr dirty_addr,
                                     const u64 dirty_size) {
                    manager->MarkSnapshotStale(dirty_addr, dirty_size);
                    manager->SetSnapshotOwnershipStamp(
                        dirty_addr, dirty_size, alias_source_stamp);
                });
        }
        if (alias_source_dirty) {
            for (const Slice& slice : slices) {
                slice.manager->MarkSnapshotStale(
                    slice.manager->GetCpuAddr() + slice.offset, slice.size,
                    alias_source_stamp);
            }
        }
        bool has_upload_range = false;
        boost::container::small_vector<RegionBits, 4> stable_upload_masks;
        stable_upload_masks.reserve(slices.size());
        for (const Slice& slice : slices) {
            stable_upload_masks.emplace_back(
                stable_read_only
                    ? slice.manager->CaptureCpuUploadMask(slice.offset, slice.size)
                    : RegionBits{});
            slice.manager->ForEachUploadRange(
                slice.manager->GetCpuAddr() + slice.offset, slice.size,
                [&](const u64 upload_addr, const u64 upload_size) {
                    has_upload_range = true;
                    func(upload_addr, upload_size);
                });
        }
        if (!has_upload_range) {
            if (!finish_descriptor_snapshot()) {
                const u64 interrupted_host_stamp =
                    NextSnapshotOwnershipStamp(SnapshotOwnership::Host);
                for (const Slice& slice : slices) {
                    slice.manager->MarkSnapshotStale(
                        slice.manager->GetCpuAddr() + slice.offset, slice.size,
                        interrupted_host_stamp);
                    slice.manager->template ChangeRegionState<Type::CPU, true>(
                        slice.manager->GetCpuAddr() + slice.offset, slice.size,
                        interrupted_host_stamp);
                }
                for (const Slice& slice : snapshot_slices) {
                    slice.manager->MarkSnapshotStale(
                        slice.manager->GetCpuAddr() + slice.offset, slice.size,
                        interrupted_host_stamp);
                }
                return SnapshotTransactionResult::Busy;
            }
            publish_gpu_ownership();
            return SnapshotTransactionResult::NoUpload;
        }
        const bool upload_succeeded = on_upload();
        // Direct-VA fallback reads must finish before GPU read protection is
        // installed; otherwise the same thread faults while holding the region
        // lock.  Still publish ownership before unlock on every normal path.
        if (!upload_succeeded) {
            return finish_descriptor_snapshot()
                       ? SnapshotTransactionResult::CopyFailed
                       : SnapshotTransactionResult::Busy;
        }
        if (!finish_descriptor_snapshot()) {
            const u64 interrupted_host_stamp =
                NextSnapshotOwnershipStamp(SnapshotOwnership::Host);
            for (const Slice& slice : slices) {
                slice.manager->MarkSnapshotStale(
                    slice.manager->GetCpuAddr() + slice.offset, slice.size,
                    interrupted_host_stamp);
                slice.manager->template ChangeRegionState<Type::CPU, true>(
                    slice.manager->GetCpuAddr() + slice.offset, slice.size,
                    interrupted_host_stamp);
            }
            for (const Slice& slice : snapshot_slices) {
                slice.manager->MarkSnapshotStale(
                    slice.manager->GetCpuAddr() + slice.offset, slice.size,
                    interrupted_host_stamp);
            }
            return SnapshotTransactionResult::Busy;
        }
        if (stable_read_only) {
            u64 promotions = 0;
            for (size_t index = 0; index < slices.size(); ++index) {
                const Slice& slice = slices[index];
                promotions += slice.manager->CommitReadOnlyStreamingUpload(
                    slice.offset, slice.size, stable_upload_masks[index]);
            }
            if (stable_promotions != nullptr) {
                *stable_promotions += promotions;
            }
        }
        if (upload_succeeded) {
            for (const Slice& slice : slices) {
                slice.manager->ClearSnapshotStale(
                    slice.manager->GetCpuAddr() + slice.offset, slice.size);
            }
        }
        publish_gpu_ownership();
        return SnapshotTransactionResult::Complete;
    }

    template <typename SnapshotFunc>
    SnapshotTransactionResult WithCpuSnapshot(
        VAddr query_cpu_range, u64 query_size,
        const auto& snapshot_write_ranges, const auto& snapshot_alias_ranges,
        SnapshotFunc&& snapshot) {
        RecordDescriptorProtectionCohortTransaction(
            static_cast<u64>(snapshot_write_ranges.size()));
        struct Slice {
            RegionManager* manager;
            u64 offset;
            size_t size;
        };
        boost::container::small_vector<Slice, 4> slices;
        boost::container::small_vector<Slice, 8> snapshot_slices;
        boost::container::small_vector<RegionManager*, 8> locked_managers;
        IteratePages<true>(
            query_cpu_range, query_size,
            [&slices, &locked_managers](RegionManager* manager, u64 offset, size_t size) {
                slices.push_back({manager, offset, size});
                locked_managers.push_back(manager);
            });
        for (const auto& [range_addr, range_size] : snapshot_alias_ranges) {
            IteratePages<true>(
                range_addr, range_size,
                [&snapshot_slices, &locked_managers](RegionManager* manager,
                                                     u64 offset, size_t size) {
                    snapshot_slices.push_back({manager, offset, size});
                    locked_managers.push_back(manager);
                });
        }
        std::ranges::sort(locked_managers, {}, &RegionManager::GetCpuAddr);
        const auto unique_end = std::ranges::unique(locked_managers).begin();
        locked_managers.erase(unique_end, locked_managers.end());
        for (RegionManager* manager : locked_managers) {
            manager->lock.lock();
        }
        size_t tracked_range_count = 0;
        const auto finish_descriptor_snapshot = [&] {
            bool clean = true;
            while (tracked_range_count != 0) {
                const auto& [range_addr, range_size] =
                    snapshot_write_ranges[--tracked_range_count];
                clean = tracker->UntrackDescriptorSnapshotWrite(
                            range_addr, range_size) &&
                        clean;
            }
            return clean;
        };
        SCOPE_EXIT {
            (void)finish_descriptor_snapshot();
            for (auto it = locked_managers.rbegin(); it != locked_managers.rend(); ++it) {
                (*it)->lock.unlock();
            }
        };

        for (const Slice& slice : slices) {
            if (slice.manager->template IsRegionModified<Type::GPU>(
                    slice.offset, slice.size)) {
                return SnapshotTransactionResult::GpuDirty;
            }
        }
        for (const Slice& slice : snapshot_slices) {
            if (slice.manager->template IsRegionModified<Type::GPU>(
                    slice.offset, slice.size)) {
                return SnapshotTransactionResult::GpuDirty;
            }
        }
        for (const auto& [range_addr, range_size] : snapshot_write_ranges) {
            const auto track_result = tracker->TryTrackDescriptorSnapshotWrite(
                range_addr, range_size);
            if (track_result != DescriptorSnapshotTrackResult::Tracked) {
                while (tracked_range_count != 0) {
                    const auto& [tracked_addr, tracked_size] =
                        snapshot_write_ranges[--tracked_range_count];
                    (void)tracker->UntrackDescriptorSnapshotWrite(
                        tracked_addr, tracked_size);
                }
                return SnapshotTrackFailureResult(track_result);
            }
            ++tracked_range_count;
        }
        const auto snapshot_dirty = [](const Slice& slice) {
            return slice.manager->template IsRegionModified<Type::CPU>(
                       slice.offset, slice.size) ||
                   slice.manager->IsRegionSnapshotStale(slice.offset, slice.size);
        };
        const bool alias_source_dirty =
            std::ranges::any_of(snapshot_slices, snapshot_dirty);
        const u64 alias_source_stamp =
            alias_source_dirty
                ? NextSnapshotOwnershipStamp(SnapshotOwnership::Host)
                : 0;
        for (const Slice& slice : snapshot_slices) {
            slice.manager->template ForEachModifiedRange<Type::CPU, true>(
                slice.manager->GetCpuAddr() + slice.offset, slice.size,
                [manager = slice.manager,
                 alias_source_stamp](const VAddr dirty_addr,
                                     const u64 dirty_size) {
                    manager->MarkSnapshotStale(dirty_addr, dirty_size);
                    manager->SetSnapshotOwnershipStamp(
                        dirty_addr, dirty_size, alias_source_stamp);
                });
        }
        if (alias_source_dirty) {
            for (const Slice& slice : slices) {
                slice.manager->MarkSnapshotStale(
                    slice.manager->GetCpuAddr() + slice.offset, slice.size,
                    alias_source_stamp);
            }
        }
        const bool copy_succeeded = snapshot();
        if (!finish_descriptor_snapshot()) {
            const u64 interrupted_host_stamp =
                NextSnapshotOwnershipStamp(SnapshotOwnership::Host);
            for (const Slice& slice : slices) {
                slice.manager->MarkSnapshotStale(
                    slice.manager->GetCpuAddr() + slice.offset, slice.size,
                    interrupted_host_stamp);
            }
            for (const Slice& slice : snapshot_slices) {
                slice.manager->MarkSnapshotStale(
                    slice.manager->GetCpuAddr() + slice.offset, slice.size,
                    interrupted_host_stamp);
            }
            return SnapshotTransactionResult::Busy;
        }
        return copy_succeeded ? SnapshotTransactionResult::Complete
                              : SnapshotTransactionResult::CopyFailed;
    }

    template <typename Func>
    [[nodiscard]] u64 ForEachSnapshotGpuConflictRange(
        VAddr query_cpu_range, u64 query_size, Func&& func) noexcept {
        u64 classified_bytes = 0;
        IteratePages<false>(
            query_cpu_range, query_size,
            [&func, &classified_bytes](RegionManager* manager, u64 offset,
                                       size_t size) {
                std::scoped_lock lk{manager->lock};
                classified_bytes += manager->ForEachSnapshotGpuConflictRange(
                    offset, size, func);
            });
        return classified_bytes;
    }

    template <bool clear>
    void ForEachDownloadRange(VAddr query_cpu_range, u64 query_size, auto&& func) {
        IteratePages<false>(query_cpu_range, query_size,
                            [&func](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->template ForEachModifiedRange<Type::GPU, clear>(
                                    manager->GetCpuAddr() + offset, size, func);
                            });
    }

private:
    [[nodiscard]] u64 NextSnapshotOwnershipStamp(
        const SnapshotOwnership ownership) noexcept {
        const u64 epoch =
            snapshot_ownership_epoch.fetch_add(1, std::memory_order_relaxed);
        ASSERT(epoch < (u64{1} << 62));
        return MakeSnapshotOwnershipStamp(epoch, ownership);
    }

    template <bool create_region_on_fail, typename Func>
    bool IteratePages(VAddr cpu_address, size_t size, Func&& func) {
        RENDERER_TRACE;
        using FuncReturn = typename std::invoke_result<Func, RegionManager*, u64, size_t>::type;
        static constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
        std::size_t remaining_size{size};
        std::size_t page_index{cpu_address >> TRACKER_HIGHER_PAGE_BITS};
        u64 page_offset{cpu_address & TRACKER_HIGHER_PAGE_MASK};
        while (remaining_size > 0) {
            const std::size_t copy_amount{
                std::min<std::size_t>(TRACKER_HIGHER_PAGE_SIZE - page_offset, remaining_size)};
            auto* manager{top_tier[page_index]};
            if (manager) {
                if constexpr (BOOL_BREAK) {
                    if (func(manager, page_offset, copy_amount)) {
                        return true;
                    }
                } else {
                    func(manager, page_offset, copy_amount);
                }
            } else if constexpr (create_region_on_fail) {
                CreateRegion(page_index);
                manager = top_tier[page_index];
                if constexpr (BOOL_BREAK) {
                    if (func(manager, page_offset, copy_amount)) {
                        return true;
                    }
                } else {
                    func(manager, page_offset, copy_amount);
                }
            }
            page_index++;
            page_offset = 0;
            remaining_size -= copy_amount;
        }
        return false;
    }

    void CreateRegion(std::size_t page_index) {
        const VAddr base_cpu_addr = page_index << TRACKER_HIGHER_PAGE_BITS;
        if (free_managers.empty()) {
            manager_pool.emplace_back();
            auto& last_pool = manager_pool.back();
            for (size_t i = 0; i < MANAGER_POOL_SIZE; i++) {
                std::construct_at(&last_pool[i], tracker, 0);
                free_managers.push_back(&last_pool[i]);
            }
        }
        auto* new_manager = free_managers.back();
        new_manager->SetCpuAddress(base_cpu_addr);
        free_managers.pop_back();
        top_tier[page_index] = new_manager;
        published_top_tier[page_index].store(new_manager,
                                             std::memory_order_release);
    }

    PageManager* tracker;
    const bool direct_region_modified_range;
    const bool combined_cpu_snapshot_query;
    std::atomic<u64> snapshot_ownership_epoch{1};
    std::deque<std::array<RegionManager, MANAGER_POOL_SIZE>> manager_pool;
    std::vector<RegionManager*> free_managers;
    std::array<RegionManager*, NUM_HIGH_PAGES> top_tier{};
    // Fast HLE publication is allowed only for already-existing managers.
    // Keep a release-published mirror so the guest thread never races the
    // renderer-owned pool/free-list or creates a RegionManager itself.
    std::array<std::atomic<RegionManager*>, NUM_HIGH_PAGES>
        published_top_tier{};
};

}
