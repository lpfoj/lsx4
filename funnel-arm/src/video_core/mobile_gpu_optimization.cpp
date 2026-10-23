// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/mobile_gpu_optimization.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace VideoCore {
namespace {

struct AtomicMobileGpuState {
    std::atomic_bool enabled{false};
    std::atomic_bool fsr_enabled{true};
    std::atomic_bool rcas_enabled{true};
    std::atomic_bool coarse_shading_available{false};
    std::atomic_bool physical_drs_available{false};
    std::atomic_bool physical_drs_resolution_floor{false};
    std::atomic<u32> scale_percent{100};
    std::atomic<u32> forced_scale_percent{0};
    std::atomic<u32> overload_streak{0};
    std::atomic<u32> recovery_streak{0};
    std::atomic<u64> frame_interval_ema_ns{0};
    std::atomic<u64> memory_used_bytes{0};
    std::atomic<u64> memory_budget_bytes{0};

    std::atomic<u64> observed_frames{0};
    std::atomic<u64> observed_memory_samples{0};
    std::atomic<u64> submitted_ticks{0};
    std::atomic<u64> completed_ticks{0};
    std::atomic<u64> max_gpu_backlog{0};
    std::atomic<u64> scale_down_events{0};
    std::atomic<u64> scale_up_events{0};
    std::atomic<u64> full_rate_draws{0};
    std::atomic<u64> coarse_rate_draws{0};
    std::atomic<u64> physical_scaled_draws{0};
    std::atomic<u64> physical_scale_fallbacks{0};
    std::atomic<u64> physical_resolve_blits{0};
    std::atomic<u64> physical_pixels_saved{0};
    std::atomic<u64> shading_stats_epoch{1};
    std::atomic<u64> fsr_frames{0};
    std::atomic<u64> rcas_frames{0};
    std::atomic<u64> source_smaller_than_output_frames{0};
    std::atomic_bool fsr_active{false};
    std::atomic_bool rcas_active{false};
    std::atomic<u32> fsr_source_width{0};
    std::atomic<u32> fsr_source_height{0};
    std::atomic<u32> fsr_output_width{0};
    std::atomic<u32> fsr_output_height{0};
    std::atomic<u64> exact_surface_reuses{0};
    std::atomic<u64> exact_surface_reuse_bytes{0};
    std::atomic<u64> surface_stats_epoch{1};
    std::atomic<u64> residency_gc_passes{0};
    std::atomic<u64> residency_gc_evictions{0};
    std::atomic<u64> residency_gc_no_victim_skips{0};
    std::atomic<u64> texture_gc_passes{0};
    std::atomic<u64> texture_gc_evictions{0};
    std::atomic<u64> texture_gc_no_victim_skips{0};
    std::atomic<u64> texture_gc_forced_passes{0};
    std::atomic<u64> texture_gc_audit_passes{0};
    std::atomic<u64> buffer_gc_passes{0};
    std::atomic<u64> buffer_gc_evictions{0};
    std::atomic<u64> buffer_gc_no_victim_skips{0};
    std::atomic<u64> buffer_gc_forced_passes{0};
    std::atomic<u64> buffer_gc_audit_passes{0};
    std::atomic<u32> android_memory_pressure_pending_level{0};
    std::atomic<u64> android_memory_pressure_callbacks{0};
    std::atomic<u64> android_memory_pressure_consumed{0};
    std::atomic<u64> android_memory_pressure_max_level{0};
    std::atomic<u64> android_memory_info_samples{0};
    std::atomic<u64> android_memory_available_bytes{0};
    std::atomic<u64> android_memory_threshold_bytes{0};
    std::atomic<u64> android_memory_low_samples{0};
    std::atomic<u64> android_process_memory_samples{0};
    std::atomic<u64> android_process_total_pss_bytes{0};
    std::atomic<u64> android_process_graphics_bytes{0};
    std::atomic<u64> android_process_total_swap_bytes{0};
    std::atomic<bool> android_memory_pressure_monitor_requested{false};
    std::atomic<u32> upscaler_requested_mode{0};
    std::atomic<u32> upscaler_effective_mode{0};
    std::atomic<u64> upscaler_frames{0};
    std::atomic<u32> upscaler_fallback_reason{0};
    std::atomic_bool temporal_native_inputs{false};
};

AtomicMobileGpuState state;
std::mutex transition_callback_mutex;
MobileGpuOptimizationTransitionCallback transition_callback{};
void* transition_callback_context{};

void UpdateMaximum(std::atomic<u64>& destination, const u64 value) noexcept {
    u64 current = destination.load(std::memory_order_relaxed);
    while (current < value &&
           !destination.compare_exchange_weak(current, value, std::memory_order_relaxed,
                                               std::memory_order_relaxed)) {
    }
}

u64 UpdateEma(std::atomic<u64>& destination, const u64 sample) noexcept {
    u64 current = destination.load(std::memory_order_relaxed);
    for (;;) {
        // 1/8 new sample: responsive enough for scene changes without toggling
        // quality on individual shader/pipeline stutters.
        const u64 next = current == 0 ? sample : current - current / 8 + sample / 8;
        if (destination.compare_exchange_weak(current, next, std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {
            return next;
        }
    }
}

bool MemoryPressureAtLeast(const u64 numerator, const u64 denominator) noexcept {
    const u64 budget = state.memory_budget_bytes.load(std::memory_order_relaxed);
    const u64 used = state.memory_used_bytes.load(std::memory_order_relaxed);
    return budget != 0 && used >= budget / denominator * numerator;
}

u32 NextLowerScale(const u32 current) noexcept {
    if (current > 85) {
        return 85;
    }
    if (current > 70) {
        return 70;
    }
    if (current > 60) {
        return 60;
    }
    return 50;
}

u32 NextHigherScale(const u32 current) noexcept {
    if (current < 60) {
        return 60;
    }
    if (current < 70) {
        return 70;
    }
    if (current < 85) {
        return 85;
    }
    return 100;
}

} // namespace

bool IsExecutorLiveQuiet() noexcept {
#ifdef __ANDROID__
    static const bool quiet = [] {
        const char* value = std::getenv("EXECUTOR_LIVE_QUIET");
        return value != nullptr && value[0] != '\0' &&
               std::strcmp(value, "0") != 0;
    }();
    return quiet;
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
    return !verbose;
#else
    return false;
#endif
}

void SetManagedMobileGpuOptimization(const bool enabled) noexcept {
    // Calls are also a per-launch controller reset. Temporarily unpublish the
    // policy even when its value is unchanged so a new title cannot inherit
    // the previous title's EMA, memory pressure, or reduced shading rate.
    state.enabled.store(false, std::memory_order_release);
    state.overload_streak.store(0, std::memory_order_relaxed);
    state.recovery_streak.store(0, std::memory_order_relaxed);
    state.frame_interval_ema_ns.store(0, std::memory_order_relaxed);
    state.memory_used_bytes.store(0, std::memory_order_relaxed);
    state.memory_budget_bytes.store(0, std::memory_order_relaxed);
    state.android_memory_pressure_pending_level.store(0,
                                                      std::memory_order_relaxed);
    state.physical_drs_resolution_floor.store(false,
                                              std::memory_order_relaxed);
    state.submitted_ticks.store(0, std::memory_order_relaxed);
    state.completed_ticks.store(0, std::memory_order_relaxed);
    state.scale_percent.store(100, std::memory_order_release);
    if (enabled) {
        // Publish the policy only after all controller inputs have been reset,
        // so the render thread cannot briefly act on stale pressure data.
        state.enabled.store(true, std::memory_order_release);
    }
    std::scoped_lock callback_lock{transition_callback_mutex};
    if (transition_callback != nullptr) {
        transition_callback(transition_callback_context, enabled);
    }
}

bool IsManagedMobileGpuOptimization() noexcept {
    return state.enabled.load(std::memory_order_acquire);
}

void SetManagedMobileFsrEnabled(const bool enabled) noexcept {
    state.fsr_enabled.store(enabled, std::memory_order_release);
}

bool IsManagedMobileFsrEnabled() noexcept {
    return state.fsr_enabled.load(std::memory_order_acquire);
}

void SetManagedMobileRcasEnabled(const bool enabled) noexcept {
    state.rcas_enabled.store(enabled, std::memory_order_release);
}

bool IsManagedMobileRcasEnabled() noexcept {
    return state.rcas_enabled.load(std::memory_order_acquire);
}

void SetManagedMobileGpuOptimizationTransitionCallback(
    const MobileGpuOptimizationTransitionCallback callback,
    void* const context) noexcept {
    std::scoped_lock callback_lock{transition_callback_mutex};
    transition_callback = callback;
    transition_callback_context = context;
}

void SetMobileGpuCoarseShadingAvailable(const bool available) noexcept {
    state.coarse_shading_available.store(available, std::memory_order_release);
    if (!available &&
        !state.physical_drs_available.load(std::memory_order_acquire)) {
        state.scale_percent.store(100, std::memory_order_release);
        state.overload_streak.store(0, std::memory_order_relaxed);
    }
}

void SetMobileGpuPhysicalDrsAvailable(const bool available) noexcept {
    state.physical_drs_available.store(available, std::memory_order_release);
    if (!available &&
        !state.coarse_shading_available.load(std::memory_order_acquire)) {
        state.scale_percent.store(100, std::memory_order_release);
        state.overload_streak.store(0, std::memory_order_relaxed);
    }
}

void SetMobileGpuPhysicalDrsResolutionFloor(const bool active) noexcept {
    state.physical_drs_resolution_floor.store(active,
                                              std::memory_order_release);
    if (active) {
        state.scale_percent.store(100, std::memory_order_release);
        state.overload_streak.store(0, std::memory_order_relaxed);
        state.recovery_streak.store(0, std::memory_order_relaxed);
    }
}

void ObserveMobileGpuFrame(const u64 interval_nanoseconds, const u64 submitted_tick,
                           const u64 completed_tick) noexcept {
    if (!IsManagedMobileGpuOptimization()) {
        return;
    }

    state.observed_frames.fetch_add(1, std::memory_order_relaxed);
    state.submitted_ticks.store(submitted_tick, std::memory_order_relaxed);
    state.completed_ticks.store(completed_tick, std::memory_order_relaxed);
    const u64 backlog = submitted_tick > completed_tick ? submitted_tick - completed_tick : 0;
    UpdateMaximum(state.max_gpu_backlog, backlog);
    const u64 interval_ema =
        interval_nanoseconds == 0
            ? state.frame_interval_ema_ns.load(std::memory_order_relaxed)
            : UpdateEma(state.frame_interval_ema_ns, interval_nanoseconds);
    if (state.physical_drs_resolution_floor.load(
            std::memory_order_acquire)) {
        state.scale_percent.store(100, std::memory_order_release);
        state.overload_streak.store(0, std::memory_order_relaxed);
        state.recovery_streak.store(0, std::memory_order_relaxed);
        return;
    }

    // A slow presented-frame interval by itself is not GPU pressure: emulated
    // games frequently miss their frame target on the CPU while the graphics
    // timeline is already caught up. Treating that case as overload reduced
    // the complete render target (including text and HUD) to 50% without
    // improving frame rate. Scale only when timeline or memory pressure proves
    // that reducing attachment/depth/fragment work can help.
    const bool severely_slow =
        interval_ema >= 50'000'000ULL;
    const bool overloaded =
        backlog >= 3 || MemoryPressureAtLeast(9, 10) ||
        (backlog >= 2 && interval_ema >= 36'000'000ULL);
    const bool recovered =
        backlog <= 1 && !MemoryPressureAtLeast(3, 4) &&
        (interval_ema == 0 || interval_ema <= 29'000'000ULL);

    if (overloaded) {
        state.recovery_streak.store(0, std::memory_order_relaxed);
        const u32 streak =
            state.overload_streak.fetch_add(1, std::memory_order_relaxed) + 1;
        if (streak >= 8 && (streak % 8) == 0 &&
            (state.coarse_shading_available.load(std::memory_order_acquire) ||
             state.physical_drs_available.load(std::memory_order_acquire))) {
            const u32 current =
                state.scale_percent.load(std::memory_order_acquire);
            // At <=20 FPS, 85% is too small a reduction to separate fragment
            // pressure from noise. Move directly to 70% (51% fewer pixels),
            // then to 50% (75% fewer pixels) if the interval remains severe.
            const u32 next =
                severely_slow ? (current > 70 ? 70u : 50u)
                              : NextLowerScale(current);
            if (next != current) {
                state.scale_percent.store(next, std::memory_order_release);
                state.scale_down_events.fetch_add(1,
                                                  std::memory_order_relaxed);
            }
        }
        return;
    }

    state.overload_streak.store(0, std::memory_order_relaxed);
    if (!recovered) {
        state.recovery_streak.store(0, std::memory_order_relaxed);
        return;
    }
    const u32 recovery =
        state.recovery_streak.fetch_add(1, std::memory_order_relaxed) + 1;
    if (recovery >= 120) {
        const u32 current =
            state.scale_percent.load(std::memory_order_acquire);
        const u32 next = NextHigherScale(current);
        if (next != current) {
            state.scale_percent.store(next, std::memory_order_release);
            state.scale_up_events.fetch_add(1, std::memory_order_relaxed);
        }
        state.recovery_streak.store(0, std::memory_order_relaxed);
    }
}

void ObserveMobileGpuMemory(const u64 used_bytes, const u64 budget_bytes) noexcept {
    if (!IsManagedMobileGpuOptimization()) {
        return;
    }
    state.observed_memory_samples.fetch_add(1, std::memory_order_relaxed);
    state.memory_used_bytes.store(used_bytes, std::memory_order_relaxed);
    state.memory_budget_bytes.store(budget_bytes, std::memory_order_relaxed);
}

void SetMobileGpuForcedScalePercent(const u32 percent) noexcept {
    state.forced_scale_percent.store(percent, std::memory_order_release);
}

u32 MobileGpuForcedScalePercent() noexcept {
    return state.forced_scale_percent.load(std::memory_order_acquire);
}

namespace {
u32 ActiveScalePercent() noexcept {
    const u32 forced = state.forced_scale_percent.load(std::memory_order_acquire);
    return forced != 0 ? forced
                       : state.scale_percent.load(std::memory_order_acquire);
}

bool AdaptiveScalingSuppressedByResolutionFloor() noexcept {
    return state.physical_drs_resolution_floor.load(std::memory_order_relaxed) &&
           state.forced_scale_percent.load(std::memory_order_relaxed) == 0;
}
}

u32 MobileGpuEffectiveScalePercent() noexcept {
    return IsManagedMobileGpuOptimization() ? ActiveScalePercent() : 100;
}

bool MobileGpuWantsCoarseShading() noexcept {
    return IsManagedMobileGpuOptimization() &&
           !AdaptiveScalingSuppressedByResolutionFloor() &&
           state.coarse_shading_available.load(std::memory_order_acquire) &&
           ActiveScalePercent() <= 50;
}

bool MobileGpuWantsPhysicalDrs() noexcept {
    return IsManagedMobileGpuOptimization() &&
           !AdaptiveScalingSuppressedByResolutionFloor() &&
           state.physical_drs_available.load(std::memory_order_acquire) &&
           ActiveScalePercent() < 100;
}

bool MobileGpuWantsFsr(const u32 source_width, const u32 source_height,
                       const u32 output_width, const u32 output_height) noexcept {
    if (!IsManagedMobileGpuOptimization()) {
        return false;
    }
    return source_width < output_width && source_height < output_height;
}

void RecordMobileGpuFsrDecision(const bool active, const bool rcas,
                                const u32 source_width, const u32 source_height,
                                const u32 output_width, const u32 output_height) noexcept {
    state.fsr_source_width.store(source_width, std::memory_order_relaxed);
    state.fsr_source_height.store(source_height, std::memory_order_relaxed);
    state.fsr_output_width.store(output_width, std::memory_order_relaxed);
    state.fsr_output_height.store(output_height, std::memory_order_relaxed);
    state.fsr_active.store(active, std::memory_order_release);
    state.rcas_active.store(active && rcas, std::memory_order_release);
    if (source_width < output_width && source_height < output_height) {
        state.source_smaller_than_output_frames.fetch_add(1, std::memory_order_relaxed);
    }
    if (!active) {
        return;
    }
    state.fsr_frames.fetch_add(1, std::memory_order_relaxed);
    if (rcas) {
        state.rcas_frames.fetch_add(1, std::memory_order_relaxed);
    }
}

void RecordMobileGpuUpscalerDecision(const u32 requested_mode,
                                     const u32 effective_mode,
                                     const bool active,
                                     const u32 fallback_reason,
                                     const bool temporal_native_inputs) noexcept {
    state.upscaler_requested_mode.store(requested_mode, std::memory_order_relaxed);
    state.upscaler_effective_mode.store(active ? effective_mode : 0,
                                        std::memory_order_release);
    state.upscaler_fallback_reason.store(fallback_reason,
                                         std::memory_order_relaxed);
    state.temporal_native_inputs.store(active && temporal_native_inputs,
                                       std::memory_order_relaxed);
    if (active) {
        state.upscaler_frames.fetch_add(1, std::memory_order_relaxed);
    }
}

void RecordMobileGpuShadingDecision(const bool coarse) noexcept {
    if (!state.enabled.load(std::memory_order_relaxed) ||
        AdaptiveScalingSuppressedByResolutionFloor()) {
        return;
    }
    // This is a per-draw hot path. Keep the common operation thread-local and
    // amortize the atomic publication so telemetry cannot become a renderer
    // bottleneck of its own.
    struct LocalShadingStats {
        u64 epoch{};
        u32 full{};
        u32 coarse{};
    };
    static thread_local LocalShadingStats local;
    const u64 epoch = state.shading_stats_epoch.load(std::memory_order_relaxed);
    if (local.epoch != epoch) {
        local = {.epoch = epoch};
    }
    if (coarse) {
        ++local.coarse;
    } else {
        ++local.full;
    }
    constexpr u32 PublishInterval = 256;
    if (local.full + local.coarse < PublishInterval) {
        return;
    }
    if (state.shading_stats_epoch.load(std::memory_order_relaxed) == epoch) {
        state.full_rate_draws.fetch_add(local.full, std::memory_order_relaxed);
        state.coarse_rate_draws.fetch_add(local.coarse, std::memory_order_relaxed);
    }
    local.full = 0;
    local.coarse = 0;
}

void RecordMobileGpuPhysicalDrsDecision(const bool scaled, const bool fallback,
                                        const u64 canonical_pixels,
                                        const u64 physical_pixels) noexcept {
    if (!state.enabled.load(std::memory_order_relaxed)) {
        return;
    }
    if (scaled) {
        state.physical_scaled_draws.fetch_add(1, std::memory_order_relaxed);
        if (canonical_pixels > physical_pixels) {
            state.physical_pixels_saved.fetch_add(canonical_pixels - physical_pixels,
                                                  std::memory_order_relaxed);
        }
    } else if (fallback) {
        state.physical_scale_fallbacks.fetch_add(1, std::memory_order_relaxed);
    }
}

void RecordMobileGpuPhysicalDrsResolve() noexcept {
    if (state.enabled.load(std::memory_order_relaxed)) {
        state.physical_resolve_blits.fetch_add(1, std::memory_order_relaxed);
    }
}

void RecordMobileGpuSurfaceReuse(const u64 bytes) noexcept {
    if (!state.enabled.load(std::memory_order_relaxed) ||
        AdaptiveScalingSuppressedByResolutionFloor()) {
        return;
    }
    // FindImage can hit this path many times per draw. Batch both counters just
    // like shading telemetry to keep cache-hit accounting out of the hot path.
    struct LocalSurfaceStats {
        u64 epoch{};
        u64 count{};
        u64 bytes{};
    };
    static thread_local LocalSurfaceStats local;
    const u64 epoch = state.surface_stats_epoch.load(std::memory_order_relaxed);
    if (local.epoch != epoch) {
        local = {.epoch = epoch};
    }
    ++local.count;
    local.bytes += bytes;
    constexpr u64 PublishInterval = 128;
    if (local.count < PublishInterval) {
        return;
    }
    if (state.surface_stats_epoch.load(std::memory_order_relaxed) == epoch) {
        state.exact_surface_reuses.fetch_add(local.count, std::memory_order_relaxed);
        state.exact_surface_reuse_bytes.fetch_add(local.bytes, std::memory_order_relaxed);
    }
    local.count = 0;
    local.bytes = 0;
}

void RecordMobileGpuResidencyGc(const u64 evictions) noexcept {
    if (!IsManagedMobileGpuOptimization()) {
        return;
    }
    state.residency_gc_passes.fetch_add(1, std::memory_order_relaxed);
    state.residency_gc_evictions.fetch_add(evictions, std::memory_order_relaxed);
}

void RecordMobileGpuTextureResidencyGc(const u64 evictions,
                                       const bool forced,
                                       const bool audit) noexcept {
    if (!IsManagedMobileGpuOptimization()) {
        return;
    }
    state.residency_gc_passes.fetch_add(1, std::memory_order_relaxed);
    state.residency_gc_evictions.fetch_add(evictions, std::memory_order_relaxed);
    state.texture_gc_passes.fetch_add(1, std::memory_order_relaxed);
    state.texture_gc_evictions.fetch_add(evictions, std::memory_order_relaxed);
    state.texture_gc_forced_passes.fetch_add(forced ? 1u : 0u,
                                             std::memory_order_relaxed);
    state.texture_gc_audit_passes.fetch_add(audit ? 1u : 0u,
                                            std::memory_order_relaxed);
}

void RecordMobileGpuTextureResidencyGcNoVictimSkip() noexcept {
    if (!IsManagedMobileGpuOptimization()) {
        return;
    }
    state.residency_gc_no_victim_skips.fetch_add(1, std::memory_order_relaxed);
    state.texture_gc_no_victim_skips.fetch_add(1, std::memory_order_relaxed);
}

void RecordMobileGpuBufferResidencyGc(const u64 evictions,
                                      const bool forced,
                                      const bool audit) noexcept {
    if (!IsManagedMobileGpuOptimization()) {
        return;
    }
    state.residency_gc_passes.fetch_add(1, std::memory_order_relaxed);
    state.residency_gc_evictions.fetch_add(evictions, std::memory_order_relaxed);
    state.buffer_gc_passes.fetch_add(1, std::memory_order_relaxed);
    state.buffer_gc_evictions.fetch_add(evictions, std::memory_order_relaxed);
    state.buffer_gc_forced_passes.fetch_add(forced ? 1u : 0u,
                                            std::memory_order_relaxed);
    state.buffer_gc_audit_passes.fetch_add(audit ? 1u : 0u,
                                           std::memory_order_relaxed);
}

void RecordMobileGpuBufferResidencyGcNoVictimSkip() noexcept {
    if (!IsManagedMobileGpuOptimization()) {
        return;
    }
    state.residency_gc_no_victim_skips.fetch_add(1, std::memory_order_relaxed);
    state.buffer_gc_no_victim_skips.fetch_add(1, std::memory_order_relaxed);
}

void ReportAndroidMemoryPressure(const u32 level) noexcept {
    if (level == 0) {
        return;
    }
    state.android_memory_pressure_callbacks.fetch_add(1,
                                                       std::memory_order_relaxed);
    UpdateMaximum(state.android_memory_pressure_max_level,
                  static_cast<u64>(level));
    u32 current = state.android_memory_pressure_pending_level.load(
        std::memory_order_relaxed);
    while (current < level &&
           !state.android_memory_pressure_pending_level.compare_exchange_weak(
               current, level, std::memory_order_release,
               std::memory_order_relaxed)) {
    }
}

void ReportAndroidMemoryInfo(const u32 level, const u64 available_bytes,
                             const u64 threshold_bytes,
                             const bool low_memory) noexcept {
    state.android_memory_info_samples.fetch_add(1, std::memory_order_relaxed);
    state.android_memory_available_bytes.store(available_bytes,
                                                std::memory_order_relaxed);
    state.android_memory_threshold_bytes.store(threshold_bytes,
                                                std::memory_order_relaxed);
    if (low_memory) {
        state.android_memory_low_samples.fetch_add(1,
                                                    std::memory_order_relaxed);
    }
    if (level != 0) {
        ReportAndroidMemoryPressure(level);
    }
}

void ReportAndroidProcessMemoryInfo(const u64 total_pss_bytes,
                                    const u64 graphics_bytes,
                                    const u64 total_swap_bytes) noexcept {
    state.android_process_total_pss_bytes.store(total_pss_bytes,
                                                 std::memory_order_relaxed);
    state.android_process_graphics_bytes.store(graphics_bytes,
                                                std::memory_order_relaxed);
    state.android_process_total_swap_bytes.store(total_swap_bytes,
                                                  std::memory_order_relaxed);
    // Publish the serial last. A render-thread acquire that observes it also
    // observes the three values above, without putting a mutex on either the
    // Java monitor thread or the draw path.
    state.android_process_memory_samples.fetch_add(1,
                                                    std::memory_order_release);
}

u64 AndroidProcessMemorySampleSerial() noexcept {
    return state.android_process_memory_samples.load(std::memory_order_acquire);
}

AndroidProcessMemoryStats GetAndroidProcessMemoryStats() noexcept {
    const u64 sample_serial = AndroidProcessMemorySampleSerial();
    return {
        .sample_serial = sample_serial,
        .total_pss_bytes = state.android_process_total_pss_bytes.load(
            std::memory_order_relaxed),
        .graphics_bytes = state.android_process_graphics_bytes.load(
            std::memory_order_relaxed),
        .total_swap_bytes = state.android_process_total_swap_bytes.load(
            std::memory_order_relaxed),
        .available_memory_bytes = state.android_memory_available_bytes.load(
            std::memory_order_relaxed),
    };
}

u32 ConsumeAndroidMemoryPressure() noexcept {
    const u32 level = state.android_memory_pressure_pending_level.exchange(
        0, std::memory_order_acq_rel);
    if (level != 0) {
        state.android_memory_pressure_consumed.fetch_add(
            1, std::memory_order_relaxed);
    }
    return level;
}

void SetAndroidMemoryPressureMonitorRequested(const bool requested) noexcept {
    state.android_memory_pressure_monitor_requested.store(
        requested, std::memory_order_release);
}

bool IsAndroidMemoryPressureMonitorRequested() noexcept {
    return state.android_memory_pressure_monitor_requested.load(
        std::memory_order_acquire);
}

MobileGpuOptimizationStats GetMobileGpuOptimizationStats() noexcept {
    return {
        .enabled = IsManagedMobileGpuOptimization() ? 1ULL : 0ULL,
        .coarse_shading_available =
            state.coarse_shading_available.load(std::memory_order_relaxed) ? 1ULL : 0ULL,
        .physical_drs_available =
            state.physical_drs_available.load(std::memory_order_relaxed) ? 1ULL : 0ULL,
        .observed_frames = state.observed_frames.load(std::memory_order_relaxed),
        .observed_memory_samples =
            state.observed_memory_samples.load(std::memory_order_relaxed),
        .submitted_ticks = state.submitted_ticks.load(std::memory_order_relaxed),
        .completed_ticks = state.completed_ticks.load(std::memory_order_relaxed),
        .max_gpu_backlog = state.max_gpu_backlog.load(std::memory_order_relaxed),
        .frame_interval_nanoseconds =
            state.frame_interval_ema_ns.load(std::memory_order_relaxed),
        .memory_used_bytes = state.memory_used_bytes.load(std::memory_order_relaxed),
        .memory_budget_bytes = state.memory_budget_bytes.load(std::memory_order_relaxed),
        .effective_scale_percent = MobileGpuEffectiveScalePercent(),
        .scale_down_events = state.scale_down_events.load(std::memory_order_relaxed),
        .scale_up_events = state.scale_up_events.load(std::memory_order_relaxed),
        .full_rate_draws = state.full_rate_draws.load(std::memory_order_relaxed),
        .coarse_rate_draws = state.coarse_rate_draws.load(std::memory_order_relaxed),
        .physical_scaled_draws =
            state.physical_scaled_draws.load(std::memory_order_relaxed),
        .physical_scale_fallbacks =
            state.physical_scale_fallbacks.load(std::memory_order_relaxed),
        .physical_resolve_blits =
            state.physical_resolve_blits.load(std::memory_order_relaxed),
        .physical_pixels_saved =
            state.physical_pixels_saved.load(std::memory_order_relaxed),
        .fsr_frames = state.fsr_frames.load(std::memory_order_relaxed),
        .rcas_frames = state.rcas_frames.load(std::memory_order_relaxed),
        .source_smaller_than_output_frames =
            state.source_smaller_than_output_frames.load(std::memory_order_relaxed),
        .fsr_active = state.fsr_active.load(std::memory_order_acquire) ? 1ULL : 0ULL,
        .rcas_active = state.rcas_active.load(std::memory_order_acquire) ? 1ULL : 0ULL,
        .fsr_source_width = state.fsr_source_width.load(std::memory_order_relaxed),
        .fsr_source_height = state.fsr_source_height.load(std::memory_order_relaxed),
        .fsr_output_width = state.fsr_output_width.load(std::memory_order_relaxed),
        .fsr_output_height = state.fsr_output_height.load(std::memory_order_relaxed),
        .exact_surface_reuses =
            state.exact_surface_reuses.load(std::memory_order_relaxed),
        .exact_surface_reuse_bytes =
            state.exact_surface_reuse_bytes.load(std::memory_order_relaxed),
        .residency_gc_passes =
            state.residency_gc_passes.load(std::memory_order_relaxed),
        .residency_gc_evictions =
            state.residency_gc_evictions.load(std::memory_order_relaxed),
        .residency_gc_no_victim_skips =
            state.residency_gc_no_victim_skips.load(std::memory_order_relaxed),
        .texture_gc_passes =
            state.texture_gc_passes.load(std::memory_order_relaxed),
        .texture_gc_evictions =
            state.texture_gc_evictions.load(std::memory_order_relaxed),
        .texture_gc_no_victim_skips =
            state.texture_gc_no_victim_skips.load(std::memory_order_relaxed),
        .texture_gc_forced_passes =
            state.texture_gc_forced_passes.load(std::memory_order_relaxed),
        .texture_gc_audit_passes =
            state.texture_gc_audit_passes.load(std::memory_order_relaxed),
        .buffer_gc_passes =
            state.buffer_gc_passes.load(std::memory_order_relaxed),
        .buffer_gc_evictions =
            state.buffer_gc_evictions.load(std::memory_order_relaxed),
        .buffer_gc_no_victim_skips =
            state.buffer_gc_no_victim_skips.load(std::memory_order_relaxed),
        .buffer_gc_forced_passes =
            state.buffer_gc_forced_passes.load(std::memory_order_relaxed),
        .buffer_gc_audit_passes =
            state.buffer_gc_audit_passes.load(std::memory_order_relaxed),
        .android_memory_pressure_callbacks =
            state.android_memory_pressure_callbacks.load(
                std::memory_order_relaxed),
        .android_memory_pressure_consumed =
            state.android_memory_pressure_consumed.load(
                std::memory_order_relaxed),
        .android_memory_pressure_max_level =
            state.android_memory_pressure_max_level.load(
                std::memory_order_relaxed),
        .android_memory_info_samples =
            state.android_memory_info_samples.load(std::memory_order_relaxed),
        .android_memory_available_bytes =
            state.android_memory_available_bytes.load(std::memory_order_relaxed),
        .android_memory_threshold_bytes =
            state.android_memory_threshold_bytes.load(std::memory_order_relaxed),
        .android_memory_low_samples =
            state.android_memory_low_samples.load(std::memory_order_relaxed),
        .android_process_memory_samples =
            state.android_process_memory_samples.load(std::memory_order_acquire),
        .android_process_total_pss_bytes =
            state.android_process_total_pss_bytes.load(std::memory_order_relaxed),
        .android_process_graphics_bytes =
            state.android_process_graphics_bytes.load(std::memory_order_relaxed),
        .android_process_total_swap_bytes =
            state.android_process_total_swap_bytes.load(std::memory_order_relaxed),
        .upscaler_requested_mode =
            state.upscaler_requested_mode.load(std::memory_order_relaxed),
        .upscaler_effective_mode =
            state.upscaler_effective_mode.load(std::memory_order_acquire),
        .upscaler_frames = state.upscaler_frames.load(std::memory_order_relaxed),
        .upscaler_fallback_reason =
            state.upscaler_fallback_reason.load(std::memory_order_relaxed),
        .temporal_native_inputs =
            state.temporal_native_inputs.load(std::memory_order_relaxed) ? 1ULL : 0ULL,
    };
}

void ResetMobileGpuOptimizationStats() noexcept {
    // Per-thread batches from the previous generation are discarded on their
    // next draw instead of being published into freshly reset counters.
    state.shading_stats_epoch.fetch_add(1, std::memory_order_acq_rel);
    state.surface_stats_epoch.fetch_add(1, std::memory_order_acq_rel);
    state.observed_frames.store(0, std::memory_order_relaxed);
    state.observed_memory_samples.store(0, std::memory_order_relaxed);
    state.submitted_ticks.store(0, std::memory_order_relaxed);
    state.completed_ticks.store(0, std::memory_order_relaxed);
    state.max_gpu_backlog.store(0, std::memory_order_relaxed);
    state.frame_interval_ema_ns.store(0, std::memory_order_relaxed);
    state.memory_used_bytes.store(0, std::memory_order_relaxed);
    state.memory_budget_bytes.store(0, std::memory_order_relaxed);
    state.scale_down_events.store(0, std::memory_order_relaxed);
    state.scale_up_events.store(0, std::memory_order_relaxed);
    state.full_rate_draws.store(0, std::memory_order_relaxed);
    state.coarse_rate_draws.store(0, std::memory_order_relaxed);
    state.physical_scaled_draws.store(0, std::memory_order_relaxed);
    state.physical_scale_fallbacks.store(0, std::memory_order_relaxed);
    state.physical_resolve_blits.store(0, std::memory_order_relaxed);
    state.physical_pixels_saved.store(0, std::memory_order_relaxed);
    state.fsr_frames.store(0, std::memory_order_relaxed);
    state.rcas_frames.store(0, std::memory_order_relaxed);
    state.source_smaller_than_output_frames.store(0, std::memory_order_relaxed);
    state.fsr_active.store(false, std::memory_order_relaxed);
    state.rcas_active.store(false, std::memory_order_relaxed);
    state.fsr_source_width.store(0, std::memory_order_relaxed);
    state.fsr_source_height.store(0, std::memory_order_relaxed);
    state.fsr_output_width.store(0, std::memory_order_relaxed);
    state.fsr_output_height.store(0, std::memory_order_relaxed);
    state.upscaler_requested_mode.store(0, std::memory_order_relaxed);
    state.upscaler_effective_mode.store(0, std::memory_order_relaxed);
    state.upscaler_frames.store(0, std::memory_order_relaxed);
    state.upscaler_fallback_reason.store(0, std::memory_order_relaxed);
    state.temporal_native_inputs.store(false, std::memory_order_relaxed);
    state.exact_surface_reuses.store(0, std::memory_order_relaxed);
    state.exact_surface_reuse_bytes.store(0, std::memory_order_relaxed);
    state.residency_gc_passes.store(0, std::memory_order_relaxed);
    state.residency_gc_evictions.store(0, std::memory_order_relaxed);
    state.residency_gc_no_victim_skips.store(0, std::memory_order_relaxed);
    state.texture_gc_passes.store(0, std::memory_order_relaxed);
    state.texture_gc_evictions.store(0, std::memory_order_relaxed);
    state.texture_gc_no_victim_skips.store(0, std::memory_order_relaxed);
    state.texture_gc_forced_passes.store(0, std::memory_order_relaxed);
    state.texture_gc_audit_passes.store(0, std::memory_order_relaxed);
    state.buffer_gc_passes.store(0, std::memory_order_relaxed);
    state.buffer_gc_evictions.store(0, std::memory_order_relaxed);
    state.buffer_gc_no_victim_skips.store(0, std::memory_order_relaxed);
    state.buffer_gc_forced_passes.store(0, std::memory_order_relaxed);
    state.buffer_gc_audit_passes.store(0, std::memory_order_relaxed);
    state.android_memory_pressure_callbacks.store(0,
                                                   std::memory_order_relaxed);
    state.android_memory_pressure_consumed.store(0,
                                                  std::memory_order_relaxed);
    state.android_memory_pressure_max_level.store(0,
                                                   std::memory_order_relaxed);
    state.android_memory_info_samples.store(0, std::memory_order_relaxed);
    state.android_memory_available_bytes.store(0, std::memory_order_relaxed);
    state.android_memory_threshold_bytes.store(0, std::memory_order_relaxed);
    state.android_memory_low_samples.store(0, std::memory_order_relaxed);
    state.android_process_memory_samples.store(0, std::memory_order_relaxed);
    state.android_process_total_pss_bytes.store(0, std::memory_order_relaxed);
    state.android_process_graphics_bytes.store(0, std::memory_order_relaxed);
    state.android_process_total_swap_bytes.store(0, std::memory_order_relaxed);
    state.android_memory_pressure_pending_level.store(0,
                                                       std::memory_order_relaxed);
}

} // namespace VideoCore
