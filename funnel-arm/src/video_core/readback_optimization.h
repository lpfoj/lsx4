// SPDX-FileCopyrightText: Copyright 2026 LSX4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace VideoCore {

// Process-wide counters intentionally use monotonically increasing totals.
// Managed Android can sample/reset them around an A/B interval without
// retaining a BufferCache or Liverpool instance.
struct ReadbackOptimizationStats {
    u64 snapshots{};
    u64 submit_snapshots{};
    u64 demand_snapshots{};
    u64 ranges{};
    u64 pages{};
    u64 copy_commands{};
    u64 copy_regions{};
    u64 source_buffers{};
    u64 regions_merged{};
    u64 timeline_waits{};
    u64 bytes{};
    u64 cpu_plan_ns{};
    u64 cpu_publish_ns{};
    u64 cpu_wait_ns{};
    u64 gpu_completion_ns{};
    u64 fallbacks{};
    u64 publication_failures{};
    u64 duplicate_submit_skips{};
    u64 overflow_allocations{};
    u64 demand_flushes{};
    u64 baseline_requests{};
    u64 sidecar_labels{};
    u64 sidecar_waits{};
};

enum class TextureReadbackClassification : u8 {
    GarbageCollection,
    LinearImage,
};

// Diagnostic-only counters. Call sites are guarded by
// IsReadbackClassificationEnabled(), so ordinary gameplay does not touch these
// atomics or read a clock. For buffer waves, requested bytes are the exact
// guest access extent and downloaded bytes include the expanded/coalesced
// window plus any recurring adjacent windows included in that wave.
struct ReadbackClassificationStats {
    u64 buffer_demand_waves{};
    u64 buffer_demand_requested_bytes{};
    u64 buffer_demand_downloaded_bytes{};
    u64 buffer_demand_wait_ns{};
    u64 buffer_demand_total_ns{};
    u64 buffer_demand_max_wait_ns{};
    u64 buffer_demand_max_total_ns{};
    u64 texture_gc_readbacks{};
    u64 texture_gc_bytes{};
    u64 texture_gc_map_wait_ns{};
    u64 texture_gc_total_ns{};
    u64 texture_gc_max_map_wait_ns{};
    u64 texture_gc_max_total_ns{};
    u64 texture_gc_critical_finishes{};
    u64 texture_gc_critical_finish_wait_ns{};
    u64 texture_linear_readbacks{};
    u64 texture_linear_bytes{};
    u64 texture_linear_map_wait_ns{};
    u64 texture_linear_total_ns{};
    u64 texture_linear_max_map_wait_ns{};
    u64 texture_linear_max_total_ns{};
    u64 buffer_clean_evictions{};
};

[[nodiscard]] ReadbackOptimizationStats GetReadbackOptimizationStats() noexcept;
void ResetReadbackOptimizationStats() noexcept;

// Explicit marker-only diagnostic:
//   lsx4-home/run-diagnostic-readback-optimization-stats
// The marker is sampled once per process. With it absent, production gameplay
// skips the aggregate atomics and their call-site clocks while retaining every
// readback, wait, retirement callback and memory publication.
[[nodiscard]] bool IsReadbackOptimizationStatsEnabled() noexcept;

// Internal event API shared by BufferCache and the PM4 fence/label sidecar.
void AccumulateReadbackOptimizationStats(const ReadbackOptimizationStats& delta) noexcept;
// Retirement callbacks for the same scheduler tick share one Vulkan timeline
// wait. This helper deduplicates their metric without changing callback order.
void RecordReadbackTimelineRetirement(u64 gpu_tick) noexcept;

// Explicit marker-only diagnostic:
//   lsx4-home/run-diagnostic-readback-classify
// The marker is sampled once per process. The logger is called only from the
// submit/GpuComm path and rate-limits itself to at most five lines per second.
[[nodiscard]] bool IsReadbackClassificationEnabled() noexcept;
void RecordBufferDemandWaveClassification(u64 requested_bytes, u64 downloaded_bytes, u64 wait_ns,
                                          u64 total_ns) noexcept;
void RecordTextureReadbackClassification(TextureReadbackClassification classification, u64 bytes,
                                         u64 map_wait_ns, u64 total_ns) noexcept;
void RecordTextureGcCriticalFinishClassification(u64 wait_ns) noexcept;
void RecordBufferCleanEvictionsClassification(u64 evictions) noexcept;
[[nodiscard]] ReadbackClassificationStats GetReadbackClassificationStats() noexcept;
void MaybeLogReadbackClassification() noexcept;

} // namespace VideoCore
