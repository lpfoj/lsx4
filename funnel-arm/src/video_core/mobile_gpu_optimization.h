// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace VideoCore {

using MobileGpuOptimizationTransitionCallback =
    void (*)(void* context, bool enabled) noexcept;

struct MobileGpuOptimizationStats {
    u64 enabled{};
    u64 coarse_shading_available{};
    u64 physical_drs_available{};
    u64 observed_frames{};
    u64 observed_memory_samples{};
    u64 submitted_ticks{};
    u64 completed_ticks{};
    u64 max_gpu_backlog{};
    u64 frame_interval_nanoseconds{};
    u64 memory_used_bytes{};
    u64 memory_budget_bytes{};
    u64 effective_scale_percent{};
    u64 scale_down_events{};
    u64 scale_up_events{};
    u64 full_rate_draws{};
    u64 coarse_rate_draws{};
    u64 physical_scaled_draws{};
    u64 physical_scale_fallbacks{};
    u64 physical_resolve_blits{};
    u64 physical_pixels_saved{};
    u64 fsr_frames{};
    u64 rcas_frames{};
    u64 source_smaller_than_output_frames{};
    u64 fsr_active{};
    u64 rcas_active{};
    u64 fsr_source_width{};
    u64 fsr_source_height{};
    u64 fsr_output_width{};
    u64 fsr_output_height{};
    u64 exact_surface_reuses{};
    u64 exact_surface_reuse_bytes{};
    u64 residency_gc_passes{};
    u64 residency_gc_evictions{};
    u64 residency_gc_no_victim_skips{};
    u64 texture_gc_passes{};
    u64 texture_gc_evictions{};
    u64 texture_gc_no_victim_skips{};
    u64 texture_gc_forced_passes{};
    u64 texture_gc_audit_passes{};
    u64 buffer_gc_passes{};
    u64 buffer_gc_evictions{};
    u64 buffer_gc_no_victim_skips{};
    u64 buffer_gc_forced_passes{};
    u64 buffer_gc_audit_passes{};
    u64 android_memory_pressure_callbacks{};
    u64 android_memory_pressure_consumed{};
    u64 android_memory_pressure_max_level{};
    u64 android_memory_info_samples{};
    u64 android_memory_available_bytes{};
    u64 android_memory_threshold_bytes{};
    u64 android_memory_low_samples{};
    u64 android_process_memory_samples{};
    u64 android_process_total_pss_bytes{};
    u64 android_process_graphics_bytes{};
    u64 android_process_total_swap_bytes{};
    u64 upscaler_requested_mode{};
    u64 upscaler_effective_mode{};
    u64 upscaler_frames{};
    u64 upscaler_fallback_reason{};
    u64 temporal_native_inputs{};
};

// Narrow process-memory view for the exact A750 recording-pressure route.
// That route is queried for every guest draw, so it must not materialize the
// complete HUD statistics structure (roughly fifty atomic loads) merely to
// discover that Android's 10-second process sample has not changed.
struct AndroidProcessMemoryStats {
    u64 sample_serial{};
    u64 total_pss_bytes{};
    u64 graphics_bytes{};
    u64 total_swap_bytes{};
    u64 available_memory_bytes{};
};

/// Enables the Android/mobile GPU policy without changing the configured output
/// extent. Disabling it immediately restores full-rate shading and leaves all
/// existing renderer configuration untouched.
void SetManagedMobileGpuOptimization(bool enabled) noexcept;

[[nodiscard]] bool IsManagedMobileGpuOptimization() noexcept;

/// Controls whether the mobile presenter may run FSR for a genuine upscale.
/// This managed preference is authoritative over both the native config and
/// the adaptive mobile policy.
void SetManagedMobileFsrEnabled(bool enabled) noexcept;
[[nodiscard]] bool IsManagedMobileFsrEnabled() noexcept;

/// Controls the optional RCAS pass independently from EASU upscaling.
void SetManagedMobileRcasEnabled(bool enabled) noexcept;
[[nodiscard]] bool IsManagedMobileRcasEnabled() noexcept;

/// Suppresses periodic diagnostic logcat output in performance runs. Runtime
/// behavior and telemetry counters remain active.
[[nodiscard]] bool IsExecutorLiveQuiet() noexcept;

/// Registers the single live renderer for managed-policy transition
/// notifications. Registration and removal are serialized with callbacks, so
/// renderer shutdown cannot race an in-progress notification.
void SetManagedMobileGpuOptimizationTransitionCallback(
    MobileGpuOptimizationTransitionCallback callback,
    void* context) noexcept;

/// Publishes device capability after Vulkan feature/rate negotiation.
void SetMobileGpuCoarseShadingAvailable(bool available) noexcept;

/// Publishes whether the renderer can use a physically smaller color target
/// while preserving the canonical guest image and resolving it before any
/// guest-visible hazard.
void SetMobileGpuPhysicalDrsAvailable(bool available) noexcept;
void SetMobileGpuPhysicalDrsResolutionFloor(bool active) noexcept;

/// Feed one presented-frame sample to the hysteretic controller. Timeline
/// backlog is used to avoid reducing quality for CPU-bound frames.
void ObserveMobileGpuFrame(u64 interval_nanoseconds, u64 submitted_tick,
                           u64 completed_tick) noexcept;

/// Feed the latest VK_EXT_memory_budget sample.
void ObserveMobileGpuMemory(u64 used_bytes, u64 budget_bytes) noexcept;

/// 100 is full-rate shading. 50 is the linear-resolution workload equivalent
/// of a safe 2x2 coarse-fragment rate, preserving the configured output size.
/// This does not resize guest GNM render targets: their address/pitch/tile and
/// inter-pass alias contracts cannot be changed by a local presenter policy.
[[nodiscard]] u32 MobileGpuEffectiveScalePercent() noexcept;

/// Pin the render scale instead of letting the overload heuristic pick it.
/// 0 restores adaptive behaviour. Anything else is honoured verbatim so the
/// user-visible resolution setting maps onto real attachment work.
void SetMobileGpuForcedScalePercent(u32 percent) noexcept;
[[nodiscard]] u32 MobileGpuForcedScalePercent() noexcept;

[[nodiscard]] bool MobileGpuWantsCoarseShading() noexcept;
[[nodiscard]] bool MobileGpuWantsPhysicalDrs() noexcept;

/// FSR is selected only for a genuine upscale. It is never inserted for an
/// equal-size image or a downscale.
[[nodiscard]] bool MobileGpuWantsFsr(u32 source_width, u32 source_height,
                                    u32 output_width, u32 output_height) noexcept;

/// Publishes the result of the actual presenter path. Active frames are
/// recorded only after the EASU (and optional RCAS) compute dispatches have
/// been encoded, so HUD/proof consumers never confuse an enabled preference
/// or an eligible extent with work that really ran.
void RecordMobileGpuFsrDecision(bool active, bool rcas, u32 source_width,
                                u32 source_height, u32 output_width,
                                u32 output_height) noexcept;

/// Publishes the actual selected backend after Auto routing and fallbacks.
/// Mode values use Config::UpscalerMode's stable persisted integer ABI.
void RecordMobileGpuUpscalerDecision(u32 requested_mode, u32 effective_mode,
                                     bool active, u32 fallback_reason,
                                     bool temporal_native_inputs) noexcept;

void RecordMobileGpuShadingDecision(bool coarse) noexcept;
void RecordMobileGpuPhysicalDrsDecision(bool scaled, bool fallback,
                                        u64 canonical_pixels,
                                        u64 physical_pixels) noexcept;
void RecordMobileGpuPhysicalDrsResolve() noexcept;
void RecordMobileGpuSurfaceReuse(u64 bytes) noexcept;
void RecordMobileGpuResidencyGc(u64 evictions) noexcept;
void RecordMobileGpuTextureResidencyGc(u64 evictions, bool forced,
                                       bool audit) noexcept;
void RecordMobileGpuTextureResidencyGcNoVictimSkip() noexcept;
void RecordMobileGpuBufferResidencyGc(u64 evictions, bool forced,
                                      bool audit) noexcept;
void RecordMobileGpuBufferResidencyGcNoVictimSkip() noexcept;

/// Publishes an Android lifecycle memory-pressure callback without walking
/// process memory or polling a Java service. The renderer consumes the maximum
/// pending level only on an explicitly selected device policy.
void ReportAndroidMemoryPressure(u32 level) noexcept;
void ReportAndroidMemoryInfo(u32 level, u64 available_bytes,
                             u64 threshold_bytes, bool low_memory) noexcept;
void ReportAndroidProcessMemoryInfo(u64 total_pss_bytes, u64 graphics_bytes,
                                    u64 total_swap_bytes) noexcept;
[[nodiscard]] u64 AndroidProcessMemorySampleSerial() noexcept;
[[nodiscard]] AndroidProcessMemoryStats GetAndroidProcessMemoryStats() noexcept;
[[nodiscard]] u32 ConsumeAndroidMemoryPressure() noexcept;

/// Publishes the renderer-owned exact route to Android orchestration. Android
/// may sample ActivityManager off the render thread only while this is true.
void SetAndroidMemoryPressureMonitorRequested(bool requested) noexcept;
[[nodiscard]] bool IsAndroidMemoryPressureMonitorRequested() noexcept;

[[nodiscard]] MobileGpuOptimizationStats GetMobileGpuOptimizationStats() noexcept;
void ResetMobileGpuOptimizationStats() noexcept;

} // namespace VideoCore
