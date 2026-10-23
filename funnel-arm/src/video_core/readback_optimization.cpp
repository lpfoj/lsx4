// SPDX-FileCopyrightText: Copyright 2026 LSX4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <mutex>

#if defined(LSX4_APPLE_RUNTIME)
#include <cstdlib>
#include <cstring>
#endif

#ifdef __ANDROID__
#include <android/log.h>
#include <unistd.h>
#endif

#include "video_core/readback_optimization.h"

namespace VideoCore {
namespace {

struct AtomicReadbackOptimizationStats {
    std::atomic<u64> snapshots{};
    std::atomic<u64> submit_snapshots{};
    std::atomic<u64> demand_snapshots{};
    std::atomic<u64> ranges{};
    std::atomic<u64> pages{};
    std::atomic<u64> copy_commands{};
    std::atomic<u64> copy_regions{};
    std::atomic<u64> source_buffers{};
    std::atomic<u64> regions_merged{};
    std::atomic<u64> timeline_waits{};
    std::atomic<u64> bytes{};
    std::atomic<u64> cpu_plan_ns{};
    std::atomic<u64> cpu_publish_ns{};
    std::atomic<u64> cpu_wait_ns{};
    std::atomic<u64> gpu_completion_ns{};
    std::atomic<u64> fallbacks{};
    std::atomic<u64> publication_failures{};
    std::atomic<u64> duplicate_submit_skips{};
    std::atomic<u64> overflow_allocations{};
    std::atomic<u64> demand_flushes{};
    std::atomic<u64> baseline_requests{};
    std::atomic<u64> sidecar_labels{};
    std::atomic<u64> sidecar_waits{};
};

AtomicReadbackOptimizationStats g_readback_stats;
std::atomic<u64> g_last_readback_retirement_tick{};

struct AtomicReadbackClassificationStats {
    std::atomic<u64> buffer_demand_waves{};
    std::atomic<u64> buffer_demand_requested_bytes{};
    std::atomic<u64> buffer_demand_downloaded_bytes{};
    std::atomic<u64> buffer_demand_wait_ns{};
    std::atomic<u64> buffer_demand_total_ns{};
    std::atomic<u64> buffer_demand_max_wait_ns{};
    std::atomic<u64> buffer_demand_max_total_ns{};
    std::atomic<u64> texture_gc_readbacks{};
    std::atomic<u64> texture_gc_bytes{};
    std::atomic<u64> texture_gc_map_wait_ns{};
    std::atomic<u64> texture_gc_total_ns{};
    std::atomic<u64> texture_gc_max_map_wait_ns{};
    std::atomic<u64> texture_gc_max_total_ns{};
    std::atomic<u64> texture_gc_critical_finishes{};
    std::atomic<u64> texture_gc_critical_finish_wait_ns{};
    std::atomic<u64> texture_linear_readbacks{};
    std::atomic<u64> texture_linear_bytes{};
    std::atomic<u64> texture_linear_map_wait_ns{};
    std::atomic<u64> texture_linear_total_ns{};
    std::atomic<u64> texture_linear_max_map_wait_ns{};
    std::atomic<u64> texture_linear_max_total_ns{};
    std::atomic<u64> buffer_clean_evictions{};
};

AtomicReadbackClassificationStats g_readback_classification_stats;

#define LSX4_READBACK_STATS_FIELDS(X)                                                              \
    X(snapshots)                                                                                   \
    X(submit_snapshots)                                                                            \
    X(demand_snapshots)                                                                            \
    X(ranges)                                                                                      \
    X(pages)                                                                                       \
    X(copy_commands)                                                                               \
    X(copy_regions)                                                                                \
    X(source_buffers)                                                                              \
    X(regions_merged)                                                                              \
    X(timeline_waits)                                                                              \
    X(bytes)                                                                                       \
    X(cpu_plan_ns)                                                                                 \
    X(cpu_publish_ns)                                                                              \
    X(cpu_wait_ns)                                                                                 \
    X(gpu_completion_ns)                                                                           \
    X(fallbacks)                                                                                   \
    X(publication_failures)                                                                        \
    X(duplicate_submit_skips)                                                                      \
    X(overflow_allocations)                                                                        \
    X(demand_flushes)                                                                              \
    X(baseline_requests)                                                                           \
    X(sidecar_labels)                                                                              \
    X(sidecar_waits)

#define LSX4_READBACK_CLASSIFICATION_FIELDS(X)                                                     \
    X(buffer_demand_waves)                                                                         \
    X(buffer_demand_requested_bytes)                                                               \
    X(buffer_demand_downloaded_bytes)                                                              \
    X(buffer_demand_wait_ns)                                                                       \
    X(buffer_demand_total_ns)                                                                      \
    X(buffer_demand_max_wait_ns)                                                                   \
    X(buffer_demand_max_total_ns)                                                                  \
    X(texture_gc_readbacks)                                                                        \
    X(texture_gc_bytes)                                                                            \
    X(texture_gc_map_wait_ns)                                                                      \
    X(texture_gc_total_ns)                                                                         \
    X(texture_gc_max_map_wait_ns)                                                                  \
    X(texture_gc_max_total_ns)                                                                     \
    X(texture_gc_critical_finishes)                                                                \
    X(texture_gc_critical_finish_wait_ns)                                                          \
    X(texture_linear_readbacks)                                                                    \
    X(texture_linear_bytes)                                                                        \
    X(texture_linear_map_wait_ns)                                                                  \
    X(texture_linear_total_ns)                                                                     \
    X(texture_linear_max_map_wait_ns)                                                              \
    X(texture_linear_max_total_ns)                                                                 \
    X(buffer_clean_evictions)

void RecordMaximum(std::atomic<u64>& maximum, const u64 value) noexcept {
    u64 observed = maximum.load(std::memory_order_relaxed);
    while (observed < value &&
           !maximum.compare_exchange_weak(observed, value, std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {
    }
}

} // namespace

ReadbackOptimizationStats GetReadbackOptimizationStats() noexcept {
    ReadbackOptimizationStats result{};
    if (!IsReadbackOptimizationStatsEnabled()) {
        return result;
    }
#define LSX4_LOAD_READBACK_STAT(field)                                                             \
    result.field = g_readback_stats.field.load(std::memory_order_relaxed);
    LSX4_READBACK_STATS_FIELDS(LSX4_LOAD_READBACK_STAT)
#undef LSX4_LOAD_READBACK_STAT
    return result;
}

void ResetReadbackOptimizationStats() noexcept {
#define LSX4_RESET_READBACK_STAT(field) g_readback_stats.field.store(0, std::memory_order_relaxed);
    LSX4_READBACK_STATS_FIELDS(LSX4_RESET_READBACK_STAT)
#undef LSX4_RESET_READBACK_STAT
    g_last_readback_retirement_tick.store(0, std::memory_order_relaxed);
#define LSX4_RESET_READBACK_CLASSIFICATION_STAT(field)                                             \
    g_readback_classification_stats.field.store(0, std::memory_order_relaxed);
    LSX4_READBACK_CLASSIFICATION_FIELDS(LSX4_RESET_READBACK_CLASSIFICATION_STAT)
#undef LSX4_RESET_READBACK_CLASSIFICATION_STAT
}

bool IsReadbackOptimizationStatsEnabled() noexcept {
#ifdef __ANDROID__
    // Aggregate readback timing/counters are useful for an explicit profiling
    // interval, but they are not part of the readback correctness contract.
    // Cache the marker so production event paths pay only a predictable branch.
    static const bool enabled =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-diagnostic-readback-optimization-stats",
                 F_OK) == 0;
    return enabled;
#elif defined(LSX4_APPLE_RUNTIME)
    // Same production/diagnostic separation as Android. Map the Apple launch
    // verbosity switch; never read a marker or clock on a hot path.
    static const bool verbose = [] {
        const char* control = std::getenv("LSX4_APPLE_RENDER_DIAGNOSTICS");
        if (control && (std::strcmp(control, "legacy") == 0 ||
                        std::strcmp(control, "verbose") == 0)) return true;
        const char* value = std::getenv("LSX4_RUNTIME_VERBOSE");
        return value && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return verbose;
#else
    return true;
#endif
}

void AccumulateReadbackOptimizationStats(const ReadbackOptimizationStats& delta) noexcept {
    if (!IsReadbackOptimizationStatsEnabled()) {
        return;
    }
#define LSX4_ADD_READBACK_STAT(field)                                                              \
    if (delta.field != 0) {                                                                        \
        g_readback_stats.field.fetch_add(delta.field, std::memory_order_relaxed);                  \
    }
    LSX4_READBACK_STATS_FIELDS(LSX4_ADD_READBACK_STAT)
#undef LSX4_ADD_READBACK_STAT
}

void RecordReadbackTimelineRetirement(const u64 gpu_tick) noexcept {
    if (!IsReadbackOptimizationStatsEnabled() || gpu_tick == 0) {
        return;
    }
    const u64 previous =
        g_last_readback_retirement_tick.exchange(gpu_tick, std::memory_order_relaxed);
    if (previous != gpu_tick) {
        g_readback_stats.timeline_waits.fetch_add(1, std::memory_order_relaxed);
    }
}

bool IsReadbackClassificationEnabled() noexcept {
#ifdef __ANDROID__
    // Marker-only and cached for the process lifetime. With the marker absent,
    // call sites take one predictable branch and never read a clock or touch
    // diagnostic counters.
    static const bool enabled = ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                                         "run-diagnostic-readback-classify",
                                         F_OK) == 0;
    return enabled;
#else
    return false;
#endif
}

void RecordBufferDemandWaveClassification(const u64 requested_bytes, const u64 downloaded_bytes,
                                          const u64 wait_ns, const u64 total_ns) noexcept {
    g_readback_classification_stats.buffer_demand_waves.fetch_add(1, std::memory_order_relaxed);
    g_readback_classification_stats.buffer_demand_requested_bytes.fetch_add(
        requested_bytes, std::memory_order_relaxed);
    g_readback_classification_stats.buffer_demand_downloaded_bytes.fetch_add(
        downloaded_bytes, std::memory_order_relaxed);
    g_readback_classification_stats.buffer_demand_wait_ns.fetch_add(wait_ns,
                                                                    std::memory_order_relaxed);
    g_readback_classification_stats.buffer_demand_total_ns.fetch_add(total_ns,
                                                                     std::memory_order_relaxed);
    RecordMaximum(g_readback_classification_stats.buffer_demand_max_wait_ns, wait_ns);
    RecordMaximum(g_readback_classification_stats.buffer_demand_max_total_ns, total_ns);
}

void RecordTextureReadbackClassification(const TextureReadbackClassification classification,
                                         const u64 bytes, const u64 map_wait_ns,
                                         const u64 total_ns) noexcept {
    if (classification == TextureReadbackClassification::GarbageCollection) {
        g_readback_classification_stats.texture_gc_readbacks.fetch_add(1,
                                                                       std::memory_order_relaxed);
        g_readback_classification_stats.texture_gc_bytes.fetch_add(bytes,
                                                                   std::memory_order_relaxed);
        g_readback_classification_stats.texture_gc_map_wait_ns.fetch_add(map_wait_ns,
                                                                         std::memory_order_relaxed);
        g_readback_classification_stats.texture_gc_total_ns.fetch_add(total_ns,
                                                                      std::memory_order_relaxed);
        RecordMaximum(g_readback_classification_stats.texture_gc_max_map_wait_ns, map_wait_ns);
        RecordMaximum(g_readback_classification_stats.texture_gc_max_total_ns, total_ns);
        return;
    }
    g_readback_classification_stats.texture_linear_readbacks.fetch_add(1,
                                                                       std::memory_order_relaxed);
    g_readback_classification_stats.texture_linear_bytes.fetch_add(bytes,
                                                                   std::memory_order_relaxed);
    g_readback_classification_stats.texture_linear_map_wait_ns.fetch_add(map_wait_ns,
                                                                         std::memory_order_relaxed);
    g_readback_classification_stats.texture_linear_total_ns.fetch_add(total_ns,
                                                                      std::memory_order_relaxed);
    RecordMaximum(g_readback_classification_stats.texture_linear_max_map_wait_ns, map_wait_ns);
    RecordMaximum(g_readback_classification_stats.texture_linear_max_total_ns, total_ns);
}

void RecordTextureGcCriticalFinishClassification(const u64 wait_ns) noexcept {
    g_readback_classification_stats.texture_gc_critical_finishes.fetch_add(
        1, std::memory_order_relaxed);
    g_readback_classification_stats.texture_gc_critical_finish_wait_ns.fetch_add(
        wait_ns, std::memory_order_relaxed);
}

void RecordBufferCleanEvictionsClassification(const u64 evictions) noexcept {
    g_readback_classification_stats.buffer_clean_evictions.fetch_add(evictions,
                                                                     std::memory_order_relaxed);
}

ReadbackClassificationStats GetReadbackClassificationStats() noexcept {
    ReadbackClassificationStats result{};
#define LSX4_LOAD_READBACK_CLASSIFICATION_STAT(field)                                              \
    result.field = g_readback_classification_stats.field.load(std::memory_order_relaxed);
    LSX4_READBACK_CLASSIFICATION_FIELDS(LSX4_LOAD_READBACK_CLASSIFICATION_STAT)
#undef LSX4_LOAD_READBACK_CLASSIFICATION_STAT
    return result;
}

void MaybeLogReadbackClassification() noexcept {
#ifdef __ANDROID__
    if (!IsReadbackClassificationEnabled()) {
        return;
    }
    constexpr u64 LogIntervalNs = 200'000'000;
    const u64 now_ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch())
                                            .count());
    struct LogState {
        std::mutex mutex;
        u64 last_log_ns{};
        ReadbackClassificationStats last{};
    };
    static LogState log_state;
    std::scoped_lock lock{log_state.mutex};
    if (log_state.last_log_ns != 0 && now_ns - log_state.last_log_ns < LogIntervalNs) {
        return;
    }
    const ReadbackClassificationStats current = GetReadbackClassificationStats();
    const auto delta = [](const u64 value, const u64 previous) {
        // ResetReadbackOptimizationStats can run between diagnostic windows.
        return value >= previous ? value - previous : value;
    };
    const ReadbackClassificationStats change{
#define LSX4_DELTA_READBACK_CLASSIFICATION_STAT(field)                                             \
    .field = delta(current.field, log_state.last.field),
        LSX4_READBACK_CLASSIFICATION_FIELDS(LSX4_DELTA_READBACK_CLASSIFICATION_STAT)
#undef LSX4_DELTA_READBACK_CLASSIFICATION_STAT
    };
    const u64 window_ms =
        log_state.last_log_ns == 0 ? 0 : (now_ns - log_state.last_log_ns) / 1'000'000;
    log_state.last = current;
    log_state.last_log_ns = now_ns;
    const auto amplification = [](const u64 downloaded, const u64 requested) {
        return requested == 0 ? 0.0
                              : static_cast<double>(downloaded) / static_cast<double>(requested);
    };

    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_READBACK_CLASSIFY] windowMs=%llu "
        "dBufWave=%llu dBufReqBytes=%llu dBufDownloadedBytes=%llu dBufAmplification=%.3f "
        "dBufWaitMs=%.3f dBufTotalMs=%.3f tBufWave=%llu tBufReqBytes=%llu "
        "tBufDownloadedBytes=%llu tBufAmplification=%.3f tBufWaitMs=%.3f "
        "tBufTotalMs=%.3f bufMaxWaitMs=%.3f bufMaxTotalMs=%.3f "
        "dTexGc=%llu dTexGcBytes=%llu dTexGcMapWaitMs=%.3f dTexGcTotalMs=%.3f "
        "tTexGc=%llu tTexGcBytes=%llu tTexGcMapWaitMs=%.3f tTexGcTotalMs=%.3f "
        "texGcMaxMapWaitMs=%.3f texGcMaxTotalMs=%.3f "
        "dGcCriticalFinish=%llu dGcCriticalFinishWaitMs=%.3f "
        "tGcCriticalFinish=%llu tGcCriticalFinishWaitMs=%.3f "
        "dTexLinear=%llu dTexLinearBytes=%llu dTexLinearMapWaitMs=%.3f "
        "dTexLinearTotalMs=%.3f tTexLinear=%llu tTexLinearBytes=%llu "
        "tTexLinearMapWaitMs=%.3f tTexLinearTotalMs=%.3f "
        "texLinearMaxMapWaitMs=%.3f texLinearMaxTotalMs=%.3f "
        "dBufCleanEvict=%llu tBufCleanEvict=%llu",
        static_cast<unsigned long long>(window_ms),
        static_cast<unsigned long long>(change.buffer_demand_waves),
        static_cast<unsigned long long>(change.buffer_demand_requested_bytes),
        static_cast<unsigned long long>(change.buffer_demand_downloaded_bytes),
        amplification(change.buffer_demand_downloaded_bytes, change.buffer_demand_requested_bytes),
        static_cast<double>(change.buffer_demand_wait_ns) / 1'000'000.0,
        static_cast<double>(change.buffer_demand_total_ns) / 1'000'000.0,
        static_cast<unsigned long long>(current.buffer_demand_waves),
        static_cast<unsigned long long>(current.buffer_demand_requested_bytes),
        static_cast<unsigned long long>(current.buffer_demand_downloaded_bytes),
        amplification(current.buffer_demand_downloaded_bytes,
                      current.buffer_demand_requested_bytes),
        static_cast<double>(current.buffer_demand_wait_ns) / 1'000'000.0,
        static_cast<double>(current.buffer_demand_total_ns) / 1'000'000.0,
        static_cast<double>(current.buffer_demand_max_wait_ns) / 1'000'000.0,
        static_cast<double>(current.buffer_demand_max_total_ns) / 1'000'000.0,
        static_cast<unsigned long long>(change.texture_gc_readbacks),
        static_cast<unsigned long long>(change.texture_gc_bytes),
        static_cast<double>(change.texture_gc_map_wait_ns) / 1'000'000.0,
        static_cast<double>(change.texture_gc_total_ns) / 1'000'000.0,
        static_cast<unsigned long long>(current.texture_gc_readbacks),
        static_cast<unsigned long long>(current.texture_gc_bytes),
        static_cast<double>(current.texture_gc_map_wait_ns) / 1'000'000.0,
        static_cast<double>(current.texture_gc_total_ns) / 1'000'000.0,
        static_cast<double>(current.texture_gc_max_map_wait_ns) / 1'000'000.0,
        static_cast<double>(current.texture_gc_max_total_ns) / 1'000'000.0,
        static_cast<unsigned long long>(change.texture_gc_critical_finishes),
        static_cast<double>(change.texture_gc_critical_finish_wait_ns) / 1'000'000.0,
        static_cast<unsigned long long>(current.texture_gc_critical_finishes),
        static_cast<double>(current.texture_gc_critical_finish_wait_ns) / 1'000'000.0,
        static_cast<unsigned long long>(change.texture_linear_readbacks),
        static_cast<unsigned long long>(change.texture_linear_bytes),
        static_cast<double>(change.texture_linear_map_wait_ns) / 1'000'000.0,
        static_cast<double>(change.texture_linear_total_ns) / 1'000'000.0,
        static_cast<unsigned long long>(current.texture_linear_readbacks),
        static_cast<unsigned long long>(current.texture_linear_bytes),
        static_cast<double>(current.texture_linear_map_wait_ns) / 1'000'000.0,
        static_cast<double>(current.texture_linear_total_ns) / 1'000'000.0,
        static_cast<double>(current.texture_linear_max_map_wait_ns) / 1'000'000.0,
        static_cast<double>(current.texture_linear_max_total_ns) / 1'000'000.0,
        static_cast<unsigned long long>(change.buffer_clean_evictions),
        static_cast<unsigned long long>(current.buffer_clean_evictions));
#endif
}

#undef LSX4_READBACK_STATS_FIELDS
#undef LSX4_READBACK_CLASSIFICATION_FIELDS

} // namespace VideoCore
