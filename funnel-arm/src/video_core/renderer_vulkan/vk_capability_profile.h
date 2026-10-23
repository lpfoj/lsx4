// SPDX-FileCopyrightText: Copyright 2026 LSX4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

#include "common/types.h"

namespace Vulkan {

constexpr u32 MakeVulkanApiVersion(const u32 major, const u32 minor,
                                   const u32 patch = 0) {
    return (major << 22) | (minor << 12) | patch;
}

constexpr u32 VulkanApiVersion10 = MakeVulkanApiVersion(1, 0);
constexpr u32 VulkanApiVersion11 = MakeVulkanApiVersion(1, 1);
constexpr u32 VulkanApiVersion12 = MakeVulkanApiVersion(1, 2);
constexpr u32 VulkanApiVersion13 = MakeVulkanApiVersion(1, 3);
constexpr u32 VulkanApiVersion14 = MakeVulkanApiVersion(1, 4);

enum class CapabilityRoute : u8 {
    Unavailable,
    Core,
    KhrExtension,
    Legacy,
};

enum class CapabilityQuirk : u64 {
    AvoidInRenderingBarrier = 1ULL << 0,
    RequireStaticDepthStencil = 1ULL << 1,
    DisablePhysicalDrsResolve = 1ULL << 2,
    DisablePs5WaveShader = 1ULL << 3,
    DeferAsyncPipelineReplacement = 1ULL << 4,
    ForegroundPipelineWithoutDisableOptimization = 1ULL << 5,
    BrokenSpirvFloatControls = 1ULL << 6,
    RequirePostBindDynamicState = 1ULL << 7,
    AllowManualBufferBoundsOverride = 1ULL << 8,
    CompactLegacyUnusedColorAttachments = 1ULL << 9,
    ConservativeImageTransitionStages = 1ULL << 10,
    ExplicitFullImageSubresourceCounts = 1ULL << 11,
    ReadOnlyDepthStencilAccess = 1ULL << 12,
    SplitDepthStencilImageBarriers = 1ULL << 13,
    RequireStorageBufferReadWriteBarrier = 1ULL << 14,
    AvoidRequiredSubgroupSizeForSubgroupIndependentCompute = 1ULL << 15,
    DeferFormattedBufferTypesUntilLowering = 1ULL << 16,
    LowerAliasedSharedU64ToU32 = 1ULL << 17,
    LowerNon32BitSharedToU32 = 1ULL << 18,
    ElideCpuCompletionSidecarBarrier = 1ULL << 19,
    ExplicitTilingBufferBounds = 1ULL << 20,
#if defined(LSX4_APPLE_RUNTIME)
    SkipEmptyDirectDispatch = 1ULL << 21,
#endif
};

// PA_SU_VTX_CNTL.PIX_CENTER=0 adds 0.5 to guest framebuffer X/Y before
// rasterization (AMD CIK 3D registers, PA_SU_VTX_CNTL). This is guest state
// translation, not a floating-point epsilon or a texture/shader-specific fix.
// Preserve the legacy route for profiles and raster modes not validated yet.
constexpr float GuestPixelCenterOffset(const bool emulate, const u32 vertex_control,
                                       const bool single_sample, const bool clip_disabled) {
    return emulate && !(vertex_control & 1u) && single_sample && !clip_disabled ? 0.5f : 0.f;
}
static_assert(GuestPixelCenterOffset(true, 0x2c, true, false) == 0.5f);
static_assert(GuestPixelCenterOffset(true, 0x2d, true, false) == 0.f);
static_assert(GuestPixelCenterOffset(false, 0x2c, true, false) == 0.f);
static_assert(GuestPixelCenterOffset(true, 0x04, true, false) == 0.5f);
static_assert(GuestPixelCenterOffset(true, 0x2c, false, false) == 0.f);
static_assert(GuestPixelCenterOffset(true, 0x2c, true, true) == 0.f);

constexpr u64 RuntimeOnlyBarrierQuirks =
    static_cast<u64>(CapabilityQuirk::ConservativeImageTransitionStages) |
    static_cast<u64>(CapabilityQuirk::ExplicitFullImageSubresourceCounts) |
    static_cast<u64>(CapabilityQuirk::ReadOnlyDepthStencilAccess) |
    static_cast<u64>(CapabilityQuirk::SplitDepthStencilImageBarriers) |
    static_cast<u64>(CapabilityQuirk::RequireStorageBufferReadWriteBarrier) |
    static_cast<u64>(CapabilityQuirk::ElideCpuCompletionSidecarBarrier);

constexpr u64 PipelineStorageDriverQuirks(const u64 driver_quirks) {
#if defined(LSX4_APPLE_RUNTIME)
    // Command emission policy does not change shader/pipeline cache identity.
    return driver_quirks & ~(RuntimeOnlyBarrierQuirks |
        static_cast<u64>(CapabilityQuirk::SkipEmptyDirectDispatch));
#endif
    return driver_quirks & ~RuntimeOnlyBarrierQuirks;
}

enum class MobileDriverFamily : u8 {
    Unknown,
    ArmProprietary,
    QualcommProprietary,
    MesaTurnip,
#if defined(LSX4_APPLE_RUNTIME)
    MoltenVk,
#endif
};

enum class PreferredSpatialUpscaler : u8 {
    Fsr1,
    Sgsr1,
};

constexpr PreferredSpatialUpscaler PreferredSpatialUpscalerForFamily(
    const MobileDriverFamily family) {
    return family == MobileDriverFamily::QualcommProprietary ||
                   family == MobileDriverFamily::MesaTurnip
               ? PreferredSpatialUpscaler::Sgsr1
               : PreferredSpatialUpscaler::Fsr1;
}

// Hardware and ICD are independent compatibility axes.  In particular,
// Adreno 750 + Turnip must not inherit proprietary-A750 workarounds, while
// Adreno 840 + Qualcomm must not inherit the measured A840+Turnip route.
// Keep this classification data-only; renderer branches consume the resolved
// CapabilityProfile instead of parsing device names or Android properties.
enum class MobileGpuModel : u8 {
    Unknown,
    Mali,
    AdrenoOther,
    Adreno750,
    Adreno840,
};

// Hardware generation is a separate compatibility layer from both the exact
// model and the loaded ICD.  Mesa's device database is deliberately layered in
// the same way: an A750 terminates at the A7xx-gen3 profile, while an A840 adds
// A8xx-gen2 properties on top of the shared lineage.  Keep this data-only until
// a feature/limit probe or a focused micro-test proves an LSX4 policy consumer.
// Unknown Adreno devices are not guessed into a generation.
enum class MobileGpuGeneration : u8 {
    Unknown,
    ArmMali,
    Adreno7xxGen3,
    Adreno8xxGen2,
};

// Canonical branch point for GPU compatibility policy.  The route is resolved
// once from the actual VkPhysicalDevice identity and reported ICD family, then
// carried by CapabilityProfile.  It deliberately does not contain a title ID:
// guest/workload policy lives above the renderer, while driver workarounds and
// memory topology branch here.  Unknown ICDs stay on PortableDefault even when
// the vendor ID resembles a known device, so a missing driver identity cannot
// silently inherit an exact-device/build policy.  A separately visible resolved
// family may still retain conservative vendor-fallback safety quirks on Vulkan
// implementations that do not report VkDriverId.
enum class GpuCompatibilityRoute : u8 {
    PortableDefault,
    MaliArmProprietary,
    AdrenoOtherQualcommProprietary,
    Adreno750QualcommProprietary,
    Adreno840QualcommProprietary,
    AdrenoOtherMesaTurnip,
    Adreno750MesaTurnip,
    Adreno840MesaTurnip,
};

// A driver family is not a sufficiently narrow compatibility boundary for
// allocator topology.  Promote measured defaults only for an exact package
// build that was also proven by the live Vulkan device identity.
enum class MobileDriverBuild : u8 {
    Unknown,
    MesaTurnipA750Cf8e1b81a6,
    MesaTurnipA750Da14d65e44,
    MesaTurnipA750Da14d65e44Ib32Rollover,
    MesaTurnipA750Da14d65e44Ib32RolloverBoStats,
    // D19 is byte-for-byte the D9 experiment outside tu_knl_kgsl.cc, where
    // BO census formatting is retained but the dynamic liblog writes are
    // removed.  Keep a distinct identity so the logger can be the sole A/B
    // variable while app-side D9 policies remain equivalent.
    MesaTurnipA750D19FormattedNoLog,
    // D20 removes ordinary-run BO accounting and census work while retaining
    // the D9/D19 KGSL correctness changes.
    MesaTurnipA750D20NoBoCensus,
};

// Identity and promotion are deliberately separate. Recognizing a package is
// enough to run a reversible exact-build experiment, but never enough to make
// it a product default. This mirrors staged GPU workaround registries used by
// ANGLE/Dawn and prevents a short successful smoke from escaping quarantine.
enum class MobileDriverBuildPolicyState : u8 {
    Unrecognized,
    Candidate,
    ProductDefault,
};

// Memory topology is an independent compatibility axis.  Keep allocator policy
// out of shader/barrier quirks: changing block geometry or budget accounting
// must not alter pipeline identity or leak from one ICD family to another.
enum class MemoryBudgetAccounting : u8 {
    // Historical behavior: GetTotalMemoryBudget is both the cache-owned
    // allowance and the absolute device-usage pressure threshold.
    LegacyRendererShare,
    // VK_EXT_memory_budget heapUsage is absolute.  Compare it against the
    // initial heapUsage baseline plus the renderer-owned incremental share,
    // while cache sizing continues to use only the incremental share.
    ReportedUsageWithInitialBaseline,
    // Exact imported-driver experiment: cache sizing retains the conservative
    // renderer share, while the absolute heap-usage pressure ceiling is 70%
    // of the driver's reported budget. This leaves measured headroom above
    // the 2.4-2.5 GiB stable working set and below the 5.28 GiB kill point.
    ReportedBudget70Percent,
};

enum class ResidencyRetirementMode : u8 {
    // Existing caches own their retirement and may use the established
    // critical-pressure finish path.
    LegacyCacheLocal,
    // Both caches select victims first; the rasterizer publishes their shared
    // deferred lifetime epoch once without waiting for the GPU.
    CoordinatedNonBlocking,
    // A driver can keep external backing alive while allocator-owned usage is
    // flat. Re-run victim collection and publish deferred lifetimes at a
    // bounded cadence while pressure remains high, still without a GPU wait.
    CoordinatedSustainedNonBlocking,
};

enum class BdaPageTableMode : u8 {
    // Existing renderer contract: one 512 MiB storage-buffer descriptor backed
    // by a fully resident allocation.
    FlatResident,
    // Preserve the proven one-descriptor shader ABI while replacing only the
    // physical 512 MiB backing with demand-bound sparse residency.  This is a
    // distinct experiment from SegmentedSparse: no extra descriptor bindings,
    // shader branches, or pipeline identities are introduced.
    FlatSparse,
    // Preserve the full 40-bit guest address space while exposing the logical
    // table through descriptor ranges accepted by the ICD. Physical residency
    // is managed independently by sparse binds.
    SegmentedSparse,
};

enum class UploadReservationMode : u8 {
    // Reserve the complete page-aligned guest span before dirty-range
    // discovery. This remains the compatibility default for every unmeasured
    // GPU/ICD pair.
    FullSpan,
    // Measure the immutable snapshot plan first and reserve only the bytes
    // that will actually be copied. This avoids ring turns and fence waits on
    // large mostly-clean FEX buffer spans.
    ExactDirtyRanges,
};

enum class UploadReservationScope : u8 {
    // Preserve the already-tested proprietary-A750 behavior: exact planning
    // is used only while a FEX capture/snapshot boundary is active.
    CaptureBoundaryOnly,
    // Use the same read-only measure + validated commit + race-retry protocol
    // for ordinary CPU-dirty uploads. This is opt-in for one measured ICD
    // build because it changes which reservations can avoid a full guest span.
    AllCpuDirtyUploads,
};

// Startup scheduling is a policy layer, not a renderer quirk.  Resolve it once
// from the actual reported ICD and let the cache consumer execute the result
// without re-discovering vendor identity.  The defaults preserve the existing
// Android warmup behavior exactly.
struct PipelineWarmupPolicy {
    u32 maximum_pipelines{8};
    u32 time_budget_ms{250};
    bool allow_background_warmup{true};
};

// Optional ImGui upload experiments must consume an already-resolved policy.
// Keeping driver discovery here prevents utility/render-backend code from
// growing its own independent vendor and driver taxonomy.
struct ImGuiUploadPolicy {
    bool allow_persistent_mapping_ab{false};
    // CPU atlas copies can retire only after the upload completion boundary.
    bool release_font_cpu_pixels{false};
};

// Descriptor-command elision is deliberately separate from feature support.
// A driver may support push descriptors correctly while still requiring every
// write to be re-emitted.  This policy only admits a marker-gated experiment;
// it never enables the optimization by itself.
struct DescriptorBindingPolicy {
    bool allow_exact_write_dedup_ab{false};
    u8 legacy_allocation_batch{32};
};

struct CompletionSidecarPolicy {
    bool diagnostic_allowed{false};
    bool elide_by_default{false};
};

// Command-buffer granularity is driver-facing policy, not a title workaround.
// The proprietary A750 ICD retains substantially more command-stream backing
// for a long recording than the measured A840 path.  Keep every ordering and
// lifetime boundary intact, but publish a smaller bounded recording on that
// exact GPU/ICD route so two in-flight recordings cannot grow into multi-GiB
// driver-owned KGSL memory.
struct CommandSubmissionPolicy {
    u32 maximum_deferred_guest_submit_boundaries{64};
};

// Cache pressure is resolved once from the actual GPU/ICD route, memory-budget
// support and the selected retirement mode.  Hot cache collectors consume the
// resulting booleans instead of reconstructing device identity every frame.
struct AggregateMemoryPressurePolicy {
    bool cache_pressure_active{false};
    bool proprietary_vma_sampling{false};
    bool coordinated_turnip_retirement{false};
    bool combined_gc{false};
};

// Graphics-key lookup policy is resolved once from the canonical GPU/ICD
// route. The marker admission intentionally remains behavior-compatible with
// the existing reversible experiment; only the exact proprietary A750 route
// receives the already-accepted default. Consumers must not rediscover the GPU
// identity while hashing draw state.
struct GraphicsLookupPolicy {
    bool structural_signature_reuse_default{false};
    bool allow_generation_guarded_reuse_ab{true};
};

struct BdaPageTablePolicy {
    BdaPageTableMode mode{BdaPageTableMode::FlatResident};
    u8 descriptor_segments{1};
    u64 descriptor_segment_size{};
};

struct MemoryPolicy {
    u64 preferred_large_heap_block_size{};
    MemoryBudgetAccounting budget_accounting{
        MemoryBudgetAccounting::LegacyRendererShare};
    ResidencyRetirementMode residency_retirement{
        ResidencyRetirementMode::LegacyCacheLocal};
    BdaPageTablePolicy bda_page_table{};
    UploadReservationMode upload_reservation{UploadReservationMode::FullSpan};
    UploadReservationScope upload_reservation_scope{
        UploadReservationScope::CaptureBoundaryOnly};
};

constexpr MobileDriverFamily MobileDriverFamilyFromVendorId(const u32 vendor_id) {
    constexpr u32 ArmVendorId = 0x13b5;
    constexpr u32 QualcommVendorId = 0x5143;
    if (vendor_id == ArmVendorId) {
        return MobileDriverFamily::ArmProprietary;
    }
    if (vendor_id == QualcommVendorId) {
        return MobileDriverFamily::QualcommProprietary;
    }
    return MobileDriverFamily::Unknown;
}

constexpr MobileDriverFamily ResolveMobileDriverFamily(const MobileDriverFamily reported_family,
                                                        const u32 vendor_id) {
    return reported_family != MobileDriverFamily::Unknown
               ? reported_family
               : MobileDriverFamilyFromVendorId(vendor_id);
}

constexpr PipelineWarmupPolicy ResolvePipelineWarmupPolicy(
    const MobileDriverFamily reported_family) {
    if (reported_family == MobileDriverFamily::MesaTurnip) {
        return {
            .maximum_pipelines = 32,
            .time_budget_ms = 1250,
            .allow_background_warmup = false,
        };
    }
    return {};
}

constexpr ImGuiUploadPolicy ResolveImGuiUploadPolicy(
    const MobileDriverFamily reported_family) {
    return {
        .allow_persistent_mapping_ab =
            reported_family == MobileDriverFamily::MesaTurnip,
    };
}

constexpr DescriptorBindingPolicy ResolveDescriptorBindingPolicy(
    const GpuCompatibilityRoute route, const MobileDriverBuild driver_build,
    const bool bounded_qti_a750_batch_requested = false) {
    return {
        .allow_exact_write_dedup_ab =
            route == GpuCompatibilityRoute::Adreno750MesaTurnip &&
            driver_build == MobileDriverBuild::MesaTurnipA750Cf8e1b81a6,
        .legacy_allocation_batch = static_cast<u8>(
            route == GpuCompatibilityRoute::Adreno750QualcommProprietary &&
                    bounded_qti_a750_batch_requested
                ? 4
                : 32),
    };
}

constexpr CompletionSidecarPolicy ResolveCompletionSidecarPolicy(
    const MobileDriverFamily reported_family,
    const bool has_elision_quirk) {
    return {
        .diagnostic_allowed = reported_family == MobileDriverFamily::MesaTurnip,
        .elide_by_default = has_elision_quirk,
    };
}

constexpr CommandSubmissionPolicy ResolveCommandSubmissionPolicy(
    const GpuCompatibilityRoute route,
    const bool exact_qcom_a750_single_boundary_requested = false) {
    return {
        .maximum_deferred_guest_submit_boundaries =
            route == GpuCompatibilityRoute::Adreno750QualcommProprietary
                ? (exact_qcom_a750_single_boundary_requested ? 1u : 8u)
                : 64u,
    };
}

constexpr AggregateMemoryPressurePolicy ResolveAggregateMemoryPressurePolicy(
    const GpuCompatibilityRoute route, const bool can_report_memory_usage,
    const ResidencyRetirementMode retirement_mode,
    const bool combined_gc_requested) {
    const bool proprietary_vma_sampling =
        route == GpuCompatibilityRoute::Adreno750QualcommProprietary &&
        !can_report_memory_usage;
    const bool coordinated_turnip_retirement =
        route == GpuCompatibilityRoute::Adreno750MesaTurnip &&
        retirement_mode != ResidencyRetirementMode::LegacyCacheLocal;
    return {
        .cache_pressure_active =
            proprietary_vma_sampling || coordinated_turnip_retirement,
        .proprietary_vma_sampling = proprietary_vma_sampling,
        .coordinated_turnip_retirement = coordinated_turnip_retirement,
        .combined_gc = proprietary_vma_sampling && combined_gc_requested,
    };
}

constexpr GraphicsLookupPolicy ResolveGraphicsLookupPolicy(
    const GpuCompatibilityRoute route,
    const bool qcom_a750_structural_reuse_disabled = false) {
    return {
        .structural_signature_reuse_default =
            route == GpuCompatibilityRoute::Adreno750QualcommProprietary &&
            !qcom_a750_structural_reuse_disabled,
        // Preserve the previous process-lifetime marker semantics on every
        // route. Device-specific promotion remains a separate proof decision.
        .allow_generation_guarded_reuse_ab = true,
    };
}

constexpr u32 QualcommVendorId = 0x5143;
constexpr u32 ArmVendorId = 0x13b5;
constexpr u32 Adreno750DeviceId = 0x43051401;
constexpr u32 Adreno840DeviceId = 0x44050a31;
constexpr u32 MesaTurnipA750Cf8e1b81a6DriverVersion = 0x06402063;
constexpr std::string_view MesaTurnipA750Cf8e1b81a6Sha256 =
    "c694033f25b722c36385300b6eecbf3b2eaac57a0a6b6f39a62de75efebc1eab";
constexpr u32 MesaTurnipA750Da14d65e44DriverVersion = 0x06802001;
constexpr std::string_view MesaTurnipA750Da14d65e44Sha256 =
    "b56a4cec9af33af61ec068efb1d8673b54bb325635ca7a2300db77df82d752cd";
constexpr u32 MesaTurnipA750Da14d65e44Ib32RolloverDriverVersion = 0x06802001;
constexpr std::string_view MesaTurnipA750Da14d65e44Ib32RolloverSha256 =
    "591258eb0cfd45e9dbbcbc1f60828dae6bcbbdff3901db2e490aa0811464d62b";
constexpr u32 MesaTurnipA750Da14d65e44Ib32RolloverBoStatsDriverVersion =
    0x06802001;
constexpr std::string_view
    MesaTurnipA750Da14d65e44Ib32RolloverBoStatsSha256 =
        "b0c0ea26586d63d708df19ee203df260881c00e4135c0d58e119d2660cfbe47e";
constexpr u32 MesaTurnipA750D19FormattedNoLogDriverVersion = 0x06802001;
constexpr std::string_view MesaTurnipA750D19FormattedNoLogSha256 =
    "90a23dc8aa854c5c9bb1fe43531f81a27de7ee69cd5e4330f49ae7d6a6a2c176";
constexpr u32 MesaTurnipA750D20NoBoCensusDriverVersion = 0x06802001;
constexpr std::string_view MesaTurnipA750D20NoBoCensusSha256 =
    "df53cdaefc249f142b3c35c8f4df696466d3191d9ca2deb60845210c67b0c68f";

constexpr MobileGpuModel ResolveMobileGpuModel(const u32 vendor_id,
                                                const u32 device_id) {
    if (vendor_id == ArmVendorId) {
        return MobileGpuModel::Mali;
    }
    if (vendor_id != QualcommVendorId) {
        return MobileGpuModel::Unknown;
    }
    if (device_id == Adreno750DeviceId) {
        return MobileGpuModel::Adreno750;
    }
    if (device_id == Adreno840DeviceId) {
        return MobileGpuModel::Adreno840;
    }
    return MobileGpuModel::AdrenoOther;
}

constexpr MobileGpuGeneration ResolveMobileGpuGeneration(
    const MobileGpuModel model) {
    switch (model) {
    case MobileGpuModel::Mali:
        return MobileGpuGeneration::ArmMali;
    case MobileGpuModel::Adreno750:
        return MobileGpuGeneration::Adreno7xxGen3;
    case MobileGpuModel::Adreno840:
        return MobileGpuGeneration::Adreno8xxGen2;
    case MobileGpuModel::Unknown:
    case MobileGpuModel::AdrenoOther:
        return MobileGpuGeneration::Unknown;
    }
    return MobileGpuGeneration::Unknown;
}

constexpr GpuCompatibilityRoute ResolveGpuCompatibilityRoute(
    const MobileDriverFamily reported_family, const u32 vendor_id,
    const u32 device_id) {
    const MobileGpuModel model = ResolveMobileGpuModel(vendor_id, device_id);
    if (reported_family == MobileDriverFamily::ArmProprietary &&
        model == MobileGpuModel::Mali) {
        return GpuCompatibilityRoute::MaliArmProprietary;
    }
    if (reported_family == MobileDriverFamily::QualcommProprietary) {
        switch (model) {
        case MobileGpuModel::Adreno750:
            return GpuCompatibilityRoute::Adreno750QualcommProprietary;
        case MobileGpuModel::Adreno840:
            return GpuCompatibilityRoute::Adreno840QualcommProprietary;
        case MobileGpuModel::AdrenoOther:
            return GpuCompatibilityRoute::AdrenoOtherQualcommProprietary;
        default:
            return GpuCompatibilityRoute::PortableDefault;
        }
    }
    if (reported_family == MobileDriverFamily::MesaTurnip) {
        switch (model) {
        case MobileGpuModel::Adreno750:
            return GpuCompatibilityRoute::Adreno750MesaTurnip;
        case MobileGpuModel::Adreno840:
            return GpuCompatibilityRoute::Adreno840MesaTurnip;
        case MobileGpuModel::AdrenoOther:
            return GpuCompatibilityRoute::AdrenoOtherMesaTurnip;
        default:
            return GpuCompatibilityRoute::PortableDefault;
        }
    }
    return GpuCompatibilityRoute::PortableDefault;
}

constexpr bool IsExactProprietaryAdreno750(const MobileDriverFamily reported_family,
                                           const u32 vendor_id, const u32 device_id) {
    return reported_family == MobileDriverFamily::QualcommProprietary &&
           vendor_id == QualcommVendorId && device_id == Adreno750DeviceId;
}

constexpr bool IsExactProprietaryAdreno840(const MobileDriverFamily reported_family,
                                           const u32 vendor_id, const u32 device_id) {
    return reported_family == MobileDriverFamily::QualcommProprietary &&
           vendor_id == QualcommVendorId && device_id == Adreno840DeviceId;
}

constexpr bool IsExactMesaTurnipAdreno750(const MobileDriverFamily reported_family,
                                          const u32 vendor_id, const u32 device_id) {
    return reported_family == MobileDriverFamily::MesaTurnip &&
           vendor_id == QualcommVendorId && device_id == Adreno750DeviceId;
}

constexpr bool IsExactMesaTurnipAdreno840(const MobileDriverFamily reported_family,
                                          const u32 vendor_id, const u32 device_id) {
    return reported_family == MobileDriverFamily::MesaTurnip &&
           vendor_id == QualcommVendorId && device_id == Adreno840DeviceId;
}

constexpr MobileDriverBuild ResolveMobileDriverBuild(
    const MobileDriverFamily reported_family, const u32 vendor_id,
    const u32 device_id, const u32 driver_version,
    const std::string_view effective_package_sha256) {
    if (IsExactMesaTurnipAdreno750(reported_family, vendor_id, device_id) &&
        driver_version == MesaTurnipA750Cf8e1b81a6DriverVersion &&
        effective_package_sha256 == MesaTurnipA750Cf8e1b81a6Sha256) {
        return MobileDriverBuild::MesaTurnipA750Cf8e1b81a6;
    }
    if (IsExactMesaTurnipAdreno750(reported_family, vendor_id, device_id) &&
        driver_version == MesaTurnipA750Da14d65e44DriverVersion &&
        effective_package_sha256 == MesaTurnipA750Da14d65e44Sha256) {
        return MobileDriverBuild::MesaTurnipA750Da14d65e44;
    }
    if (IsExactMesaTurnipAdreno750(reported_family, vendor_id, device_id) &&
        driver_version == MesaTurnipA750Da14d65e44Ib32RolloverDriverVersion &&
        effective_package_sha256 == MesaTurnipA750Da14d65e44Ib32RolloverSha256) {
        return MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32Rollover;
    }
    if (IsExactMesaTurnipAdreno750(reported_family, vendor_id, device_id) &&
        driver_version ==
            MesaTurnipA750Da14d65e44Ib32RolloverBoStatsDriverVersion &&
        effective_package_sha256 ==
            MesaTurnipA750Da14d65e44Ib32RolloverBoStatsSha256) {
        return MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats;
    }
    if (IsExactMesaTurnipAdreno750(reported_family, vendor_id, device_id) &&
        driver_version == MesaTurnipA750D19FormattedNoLogDriverVersion &&
        effective_package_sha256 == MesaTurnipA750D19FormattedNoLogSha256) {
        return MobileDriverBuild::MesaTurnipA750D19FormattedNoLog;
    }
    if (IsExactMesaTurnipAdreno750(reported_family, vendor_id, device_id) &&
        driver_version == MesaTurnipA750D20NoBoCensusDriverVersion &&
        effective_package_sha256 == MesaTurnipA750D20NoBoCensusSha256) {
        return MobileDriverBuild::MesaTurnipA750D20NoBoCensus;
    }
    return MobileDriverBuild::Unknown;
}

constexpr bool IsMesaTurnipA750D9LineageBuild(
    const MobileDriverBuild driver_build) {
    return driver_build ==
               MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats ||
           driver_build == MobileDriverBuild::MesaTurnipA750D19FormattedNoLog ||
           driver_build == MobileDriverBuild::MesaTurnipA750D20NoBoCensus;
}

constexpr MobileDriverBuildPolicyState ResolveMobileDriverBuildPolicyState(
    const MobileDriverBuild driver_build) {
    if (driver_build == MobileDriverBuild::MesaTurnipA750Cf8e1b81a6 ||
        driver_build == MobileDriverBuild::MesaTurnipA750Da14d65e44 ||
        driver_build == MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32Rollover ||
        IsMesaTurnipA750D9LineageBuild(driver_build)) {
        return MobileDriverBuildPolicyState::Candidate;
    }
    return MobileDriverBuildPolicyState::Unrecognized;
}

constexpr bool HasCandidateA750TurnipComposedMemoryPolicy(
    const MobileDriverBuild driver_build) {
    return ResolveMobileDriverBuildPolicyState(driver_build) ==
           MobileDriverBuildPolicyState::Candidate;
}

// A recognized package is only the identity boundary for an isolated experiment.
// Product-default promotion additionally requires repeated long-load/gameplay passes.
// The c694 build reduced short-run residency, but two marker-free product smokes were
// killed under system-wide memory pressure and one exposed a corrupt first gameplay
// frame. Keep the composed route opt-in until that acceptance gate is closed.
constexpr bool HasProductDefaultA750TurnipComposedMemoryPolicy(
    const MobileDriverBuild driver_build) {
    return ResolveMobileDriverBuildPolicyState(driver_build) ==
           MobileDriverBuildPolicyState::ProductDefault;
}

constexpr u64 LogicalBdaPageTableSize = 512ULL * 1024ULL * 1024ULL;
constexpr u64 MinimumBdaDescriptorSegmentSize = 128ULL * 1024ULL * 1024ULL;
constexpr u8 MaximumBdaDescriptorSegments = 4;

constexpr BdaPageTablePolicy ResolveBdaPageTablePolicy(
    const MobileDriverFamily reported_family, const u32 vendor_id,
    const u32 device_id, const MobileDriverBuild driver_build,
    const bool turnip_segmented_sparse_requested,
    const bool qcom_segmented_sparse_requested,
    const bool validated_policy_disabled,
    const bool sparse_residency_supported, const u64 max_storage_buffer_range,
    const bool turnip_flat_sparse_requested = false) {
    const bool exact_turnip_a750_d9 =
        IsExactMesaTurnipAdreno750(reported_family, vendor_id, device_id) &&
        IsMesaTurnipA750D9LineageBuild(driver_build);
    const bool exact_qcom_a750 =
        IsExactProprietaryAdreno750(reported_family, vendor_id, device_id);
    const bool default_turnip_enabled =
        exact_turnip_a750_d9 &&
        HasProductDefaultA750TurnipComposedMemoryPolicy(driver_build);
    if (!validated_policy_disabled && sparse_residency_supported &&
        max_storage_buffer_range == MinimumBdaDescriptorSegmentSize &&
        exact_turnip_a750_d9 && turnip_flat_sparse_requested) {
        return {
            .mode = BdaPageTableMode::FlatSparse,
            .descriptor_segments = 1,
            .descriptor_segment_size = 0,
        };
    }
    const bool exact_route_requested =
        (exact_turnip_a750_d9 &&
         (turnip_segmented_sparse_requested || default_turnip_enabled)) ||
        (exact_qcom_a750 && qcom_segmented_sparse_requested);
    if (validated_policy_disabled ||
        !exact_route_requested ||
        !sparse_residency_supported ||
        max_storage_buffer_range != MinimumBdaDescriptorSegmentSize) {
        return {};
    }
    return {
        .mode = BdaPageTableMode::SegmentedSparse,
        .descriptor_segments = MaximumBdaDescriptorSegments,
        .descriptor_segment_size = MinimumBdaDescriptorSegmentSize,
    };
}

constexpr MemoryPolicy ResolveMobileMemoryPolicy(
    const MobileDriverFamily reported_family, const u32 vendor_id,
    const u32 device_id, const MobileDriverBuild driver_build,
    const bool turnip_a750_budget_baseline_requested = false,
    const bool validated_policy_disabled = false,
    const bool turnip_a750_sparse_upload_requested = false,
    const bool turnip_a750_bounded_residency_requested = false,
    const bool turnip_a750_sustained_residency_requested = false,
    const bool qcom_a750_all_upload_reservation_requested = false) {
    constexpr u64 A750QualcommLargeHeapBlockSize = 64ULL * 1024ULL * 1024ULL;
    constexpr u64 A750TurnipD9LargeHeapBlockSize = 32ULL * 1024ULL * 1024ULL;
    if (IsExactProprietaryAdreno750(reported_family, vendor_id, device_id)) {
        return MemoryPolicy{
            .preferred_large_heap_block_size = A750QualcommLargeHeapBlockSize,
            .upload_reservation = UploadReservationMode::ExactDirtyRanges,
            .upload_reservation_scope =
                qcom_a750_all_upload_reservation_requested
                    ? UploadReservationScope::AllCpuDirtyUploads
                    : UploadReservationScope::CaptureBoundaryOnly,
        };
    }
    if (IsExactMesaTurnipAdreno750(reported_family, vendor_id, device_id)) {
        const bool bounded_residency =
            !validated_policy_disabled &&
            (turnip_a750_bounded_residency_requested ||
             turnip_a750_sustained_residency_requested) &&
            HasCandidateA750TurnipComposedMemoryPolicy(driver_build);
        const bool sustained_residency =
            bounded_residency && turnip_a750_sustained_residency_requested;
        // Bloodborne's loading-to-gameplay transition creates many small image
        // and buffer allocations at once.  D9 measurements on A750 showed
        // 2.50 GiB of unused VMA capacity with the allocator default and
        // 1.02 GiB with 64 MiB blocks. Keep the existing candidate gate and
        // halve only this exact bounded D9 route; A840, other Turnip packages
        // and marker-free A750 remain on their established allocator policy.
        const bool compact_d9_blocks =
            bounded_residency && IsMesaTurnipA750D9LineageBuild(driver_build);
        return MemoryPolicy{
            .preferred_large_heap_block_size =
                compact_d9_blocks
                    ? A750TurnipD9LargeHeapBlockSize
                    : !validated_policy_disabled &&
                        HasProductDefaultA750TurnipComposedMemoryPolicy(driver_build)
                          ? A750QualcommLargeHeapBlockSize
                          : 0,
            .budget_accounting =
                bounded_residency
                    ? MemoryBudgetAccounting::ReportedBudget70Percent
                    : turnip_a750_budget_baseline_requested
                          ? MemoryBudgetAccounting::ReportedUsageWithInitialBaseline
                          : MemoryBudgetAccounting::LegacyRendererShare,
            .residency_retirement =
                sustained_residency
                    ? ResidencyRetirementMode::CoordinatedSustainedNonBlocking
                    : bounded_residency
                          ? ResidencyRetirementMode::CoordinatedNonBlocking
                          : ResidencyRetirementMode::LegacyCacheLocal,
            .upload_reservation =
                !validated_policy_disabled && turnip_a750_sparse_upload_requested &&
                        HasCandidateA750TurnipComposedMemoryPolicy(driver_build)
                    ? UploadReservationMode::ExactDirtyRanges
                    : UploadReservationMode::FullSpan,
            .upload_reservation_scope =
                !validated_policy_disabled && turnip_a750_sparse_upload_requested &&
                        HasCandidateA750TurnipComposedMemoryPolicy(driver_build)
                    ? UploadReservationScope::AllCpuDirtyUploads
                    : UploadReservationScope::CaptureBoundaryOnly,
        };
    }
    return {};
}

constexpr std::string_view MemoryBudgetAccountingName(
    const MemoryBudgetAccounting accounting) {
    switch (accounting) {
    case MemoryBudgetAccounting::LegacyRendererShare:
        return "legacy-renderer-share";
    case MemoryBudgetAccounting::ReportedUsageWithInitialBaseline:
        return "reported-usage-with-initial-baseline";
    case MemoryBudgetAccounting::ReportedBudget70Percent:
        return "reported-budget-70-percent";
    }
    return "legacy-renderer-share";
}

constexpr std::string_view ResidencyRetirementModeName(
    const ResidencyRetirementMode mode) {
    switch (mode) {
    case ResidencyRetirementMode::LegacyCacheLocal:
        return "legacy-cache-local";
    case ResidencyRetirementMode::CoordinatedNonBlocking:
        return "coordinated-nonblocking";
    case ResidencyRetirementMode::CoordinatedSustainedNonBlocking:
        return "coordinated-sustained-nonblocking";
    }
    return "legacy-cache-local";
}

constexpr std::string_view BdaPageTableModeName(const BdaPageTableMode mode) {
    switch (mode) {
    case BdaPageTableMode::FlatResident:
        return "flat-resident";
    case BdaPageTableMode::FlatSparse:
        return "flat-sparse";
    case BdaPageTableMode::SegmentedSparse:
        return "segmented-sparse";
    }
    return "flat-resident";
}

constexpr std::string_view UploadReservationModeName(
    const UploadReservationMode mode) {
    switch (mode) {
    case UploadReservationMode::FullSpan:
        return "full-span";
    case UploadReservationMode::ExactDirtyRanges:
        return "exact-dirty-ranges";
    }
    return "full-span";
}

constexpr std::string_view UploadReservationScopeName(
    const UploadReservationScope scope) {
    switch (scope) {
    case UploadReservationScope::CaptureBoundaryOnly:
        return "capture-boundary-only";
    case UploadReservationScope::AllCpuDirtyUploads:
        return "all-cpu-dirty-uploads";
    }
    return "capture-boundary-only";
}

constexpr std::string_view MobileDriverBuildName(const MobileDriverBuild build) {
    switch (build) {
    case MobileDriverBuild::Unknown:
        return "unknown";
    case MobileDriverBuild::MesaTurnipA750Cf8e1b81a6:
        return "mesa-turnip-a750-cf8e1b81a6";
    case MobileDriverBuild::MesaTurnipA750Da14d65e44:
        return "mesa-turnip-a750-da14d65e44";
    case MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32Rollover:
        return "mesa-turnip-a750-da14d65e44-ib32-rollover";
    case MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats:
        return "mesa-turnip-a750-da14d65e44-ib32-rollover-bo-stats";
    case MobileDriverBuild::MesaTurnipA750D19FormattedNoLog:
        return "mesa-turnip-a750-d19-formatted-no-log";
    case MobileDriverBuild::MesaTurnipA750D20NoBoCensus:
        return "mesa-turnip-a750-d20-no-bo-census";
    }
    return "unknown";
}

constexpr std::string_view MobileDriverBuildPolicyStateName(
    const MobileDriverBuildPolicyState state) {
    switch (state) {
    case MobileDriverBuildPolicyState::Unrecognized:
        return "unrecognized";
    case MobileDriverBuildPolicyState::Candidate:
        return "candidate";
    case MobileDriverBuildPolicyState::ProductDefault:
        return "product-default";
    }
    return "unrecognized";
}

constexpr u64 ExactMesaTurnipAdreno840RuntimeQuirks(
    const MobileDriverFamily reported_family, const u32 vendor_id,
    const u32 device_id) {
    if (!IsExactMesaTurnipAdreno840(reported_family, vendor_id, device_id)) {
        return 0;
    }
    // Managed completion labels are CPU retirement sidecars. On A840 Turnip,
    // their remaining global ALL_COMMANDS barriers repeatedly terminate
    // dynamic rendering even though buffer/image hazards keep their own
    // dependencies. Keep this runtime-only and exact to the measured route.
    return static_cast<u64>(CapabilityQuirk::ElideCpuCompletionSidecarBarrier);
}

constexpr bool EnableExactProprietaryAdreno750BarrierCapture(
    const MobileDriverFamily reported_family, const u32 vendor_id, const u32 device_id,
    const bool requested) {
    return requested &&
           IsExactProprietaryAdreno750(reported_family, vendor_id, device_id);
}

constexpr bool EnableExactA750TurnipD9ResidencyTelemetry(
    const GpuCompatibilityRoute route, const MobileDriverBuild driver_build,
    const bool requested) {
    return requested && route == GpuCompatibilityRoute::Adreno750MesaTurnip &&
           IsMesaTurnipA750D9LineageBuild(driver_build);
}

constexpr u64 ExactProprietaryAdreno750RuntimeQuirks(
    const MobileDriverFamily reported_family, const u32 vendor_id, const u32 device_id) {
    if (!IsExactProprietaryAdreno750(reported_family, vendor_id, device_id)) {
        return 0;
    }
    // The proprietary A750 ICD does not reliably make consecutive read/write SSBO
    // accesses visible inside one command buffer without an explicit dependency.
    // Keep this separate from pipeline-storage identity: it changes synchronization
    // only and must never leak into A840, Turnip, or Mali routes.
    return static_cast<u64>(CapabilityQuirk::RequireStorageBufferReadWriteBarrier);
}

constexpr u64 ExactProprietaryAdreno750PipelineQuirks(
    const MobileDriverFamily reported_family, const u32 vendor_id,
    const u32 device_id,
    const bool defer_formatted_buffer_types = true,
    const bool lower_aliased_shared_u64 = true) {
    if (!IsExactProprietaryAdreno750(reported_family, vendor_id, device_id)) {
        return 0;
    }
    // Keep proprietary A750 compiler refinements compile-time only. Formatted
    // buffers retain only the raw type selected by lowering. Mixed U32/U64 LDS
    // views avoid an overlapping explicit-layout U64 variable when all U64
    // accesses are non-atomic. The failed subgroup-size omission A/B remains
    // available as infrastructure but is not enabled here.
    return (defer_formatted_buffer_types
                ? static_cast<u64>(
                      CapabilityQuirk::DeferFormattedBufferTypesUntilLowering)
                : 0) |
           (lower_aliased_shared_u64
                ? static_cast<u64>(CapabilityQuirk::LowerAliasedSharedU64ToU32)
                : 0);
}

constexpr u64 ExactProprietaryAdreno840PipelineQuirks(
    const MobileDriverFamily reported_family, const u32 vendor_id, const u32 device_id) {
    if (!IsExactProprietaryAdreno840(reported_family, vendor_id, device_id)) {
        return 0;
    }
    // The proprietary A840 compiler can crash while consuming an unnecessary
    // required-subgroup-size pNext for subgroup-independent compute shaders.
    // It also dereferences null for explicit-layout LDS with non-32-bit views.
    // Lower non-atomic U16/U64 accesses to U32 operations, matching the
    // device-tested missing-explicit-layout simplification while keeping the
    // advertised capability available to unaffected shaders. Both workarounds
    // remain exact to this proprietary GPU.
    return static_cast<u64>(
               CapabilityQuirk::AvoidRequiredSubgroupSizeForSubgroupIndependentCompute) |
           static_cast<u64>(CapabilityQuirk::LowerNon32BitSharedToU32);
}

constexpr u64 ExactProprietaryAdreno750Quirks(
    const MobileDriverFamily reported_family, const u32 vendor_id, const u32 device_id,
    const bool conservative_image_transition_stages,
    const bool explicit_full_image_subresource_counts,
    const bool read_only_depth_stencil_access,
    const bool split_depth_stencil_image_barriers) {
    if (!IsExactProprietaryAdreno750(reported_family, vendor_id, device_id)) {
        return 0;
    }
    return (conservative_image_transition_stages
                ? static_cast<u64>(CapabilityQuirk::ConservativeImageTransitionStages)
                : 0) |
           (explicit_full_image_subresource_counts
                ? static_cast<u64>(CapabilityQuirk::ExplicitFullImageSubresourceCounts)
                : 0) |
           (read_only_depth_stencil_access
                ? static_cast<u64>(CapabilityQuirk::ReadOnlyDepthStencilAccess)
                : 0) |
           (split_depth_stencil_image_barriers
                ? static_cast<u64>(CapabilityQuirk::SplitDepthStencilImageBarriers)
                : 0);
}

constexpr u64 DriverQuirksForFamily(const MobileDriverFamily family) {
    switch (family) {
    case MobileDriverFamily::ArmProprietary:
        return static_cast<u64>(CapabilityQuirk::RequireStaticDepthStencil) |
               static_cast<u64>(CapabilityQuirk::DisablePhysicalDrsResolve) |
               static_cast<u64>(CapabilityQuirk::DisablePs5WaveShader) |
               static_cast<u64>(CapabilityQuirk::ForegroundPipelineWithoutDisableOptimization);
    case MobileDriverFamily::QualcommProprietary:
        return static_cast<u64>(CapabilityQuirk::AvoidInRenderingBarrier) |
               static_cast<u64>(CapabilityQuirk::DisablePhysicalDrsResolve) |
               static_cast<u64>(CapabilityQuirk::DeferAsyncPipelineReplacement) |
               static_cast<u64>(CapabilityQuirk::BrokenSpirvFloatControls) |
               static_cast<u64>(CapabilityQuirk::RequirePostBindDynamicState) |
               static_cast<u64>(CapabilityQuirk::AllowManualBufferBoundsOverride) |
               static_cast<u64>(CapabilityQuirk::CompactLegacyUnusedColorAttachments);
    case MobileDriverFamily::MesaTurnip:
        // Turnip runs on Qualcomm hardware, but it must not inherit workarounds for the
        // proprietary Qualcomm driver. Deferred replacement remains required because our
        // live pipeline hand-off can otherwise race the old pipeline lifetime after
        // VK_PIPELINE_COMPILE_REQUIRED_EXT. Physical DRS must also remain disabled until its
        // unmap-time resolve lifetime is fixed: Turnip otherwise faults in the first
        // scale=60 resolve on the GPU communication thread.
        return static_cast<u64>(CapabilityQuirk::AvoidInRenderingBarrier) |
               static_cast<u64>(CapabilityQuirk::DisablePhysicalDrsResolve) |
               static_cast<u64>(CapabilityQuirk::DeferAsyncPipelineReplacement);
#if defined(LSX4_APPLE_RUNTIME)
    case MobileDriverFamily::MoltenVk:
        // Carry Android's physical-DRS lifetime quarantine across the bridge.
        // All Android ICD families above already exclude this scratch-resolve
        // path. Fixed source/output scaling and spatial upscalers stay enabled.
        return static_cast<u64>(CapabilityQuirk::DisablePhysicalDrsResolve) |
               static_cast<u64>(CapabilityQuirk::SkipEmptyDirectDispatch);
#endif
    case MobileDriverFamily::Unknown:
        return 0;
    }
    return 0;
}

constexpr std::string_view MobileDriverFamilyName(const MobileDriverFamily family) {
    switch (family) {
#if defined(LSX4_APPLE_RUNTIME)
    case MobileDriverFamily::MoltenVk:
        return "moltenvk";
#endif
    case MobileDriverFamily::Unknown:
        return "unknown";
    case MobileDriverFamily::ArmProprietary:
        return "arm-proprietary";
    case MobileDriverFamily::QualcommProprietary:
        return "qualcomm-proprietary";
    case MobileDriverFamily::MesaTurnip:
        return "mesa-turnip";
    }
    return "unknown";
}

constexpr std::string_view MobileGpuModelName(const MobileGpuModel model) {
    switch (model) {
    case MobileGpuModel::Unknown:
        return "unknown";
    case MobileGpuModel::Mali:
        return "mali";
    case MobileGpuModel::AdrenoOther:
        return "adreno-other";
    case MobileGpuModel::Adreno750:
        return "adreno-750";
    case MobileGpuModel::Adreno840:
        return "adreno-840";
    }
    return "unknown";
}

constexpr std::string_view MobileGpuGenerationName(
    const MobileGpuGeneration generation) {
    switch (generation) {
    case MobileGpuGeneration::Unknown:
        return "unknown";
    case MobileGpuGeneration::ArmMali:
        return "arm-mali";
    case MobileGpuGeneration::Adreno7xxGen3:
        return "adreno-7xx-gen3";
    case MobileGpuGeneration::Adreno8xxGen2:
        return "adreno-8xx-gen2";
    }
    return "unknown";
}

constexpr std::string_view GpuCompatibilityRouteName(
    const GpuCompatibilityRoute route) {
    switch (route) {
    case GpuCompatibilityRoute::PortableDefault:
        return "portable-default";
    case GpuCompatibilityRoute::MaliArmProprietary:
        return "mali/arm-proprietary";
    case GpuCompatibilityRoute::AdrenoOtherQualcommProprietary:
        return "adreno-other/qualcomm-proprietary";
    case GpuCompatibilityRoute::Adreno750QualcommProprietary:
        return "adreno-750/qualcomm-proprietary";
    case GpuCompatibilityRoute::Adreno840QualcommProprietary:
        return "adreno-840/qualcomm-proprietary";
    case GpuCompatibilityRoute::AdrenoOtherMesaTurnip:
        return "adreno-other/mesa-turnip";
    case GpuCompatibilityRoute::Adreno750MesaTurnip:
        return "adreno-750/mesa-turnip";
    case GpuCompatibilityRoute::Adreno840MesaTurnip:
        return "adreno-840/mesa-turnip";
    }
    return "portable-default";
}

constexpr std::string_view CapabilityRouteName(const CapabilityRoute route) {
    switch (route) {
    case CapabilityRoute::Unavailable:
        return "unavailable";
    case CapabilityRoute::Core:
        return "core";
    case CapabilityRoute::KhrExtension:
        return "khr";
    case CapabilityRoute::Legacy:
        return "legacy";
    }
    return "unavailable";
}

constexpr CapabilityRoute SelectPromotedRoute(const u32 api_version,
                                               const u32 promoted_version,
                                               const bool extension_enabled,
                                               const bool feature_enabled,
                                               const CapabilityRoute fallback) {
    if (!feature_enabled) {
        return fallback;
    }
    if (api_version >= promoted_version) {
        return CapabilityRoute::Core;
    }
    if (extension_enabled) {
        return CapabilityRoute::KhrExtension;
    }
    return fallback;
}

constexpr u32 SpirvVersionForVulkan(const u32 api_version) {
    if (api_version >= VulkanApiVersion13) {
        return 0x00010600U;
    }
    if (api_version >= VulkanApiVersion12) {
        return 0x00010500U;
    }
    if (api_version >= VulkanApiVersion11) {
        return 0x00010300U;
    }
    return 0x00010000U;
}

constexpr u32 CompactTrailingAttachmentCount(u32 count, const u32 active_mask) {
    while (count != 0 && (active_mask & (1U << (count - 1U))) == 0) {
        --count;
    }
    return count;
}

struct CapabilityProfile {
    u32 api_version{VulkanApiVersion10};
    u32 spirv_version{0x00010000U};
    u32 driver_version{};
    MobileDriverFamily reported_mobile_driver_family{MobileDriverFamily::Unknown};
    MobileDriverFamily mobile_driver_family{MobileDriverFamily::Unknown};
    MobileGpuModel mobile_gpu_model{MobileGpuModel::Unknown};
    MobileGpuGeneration mobile_gpu_generation{MobileGpuGeneration::Unknown};
    GpuCompatibilityRoute gpu_compatibility_route{
        GpuCompatibilityRoute::PortableDefault};
    MobileDriverBuild mobile_driver_build{MobileDriverBuild::Unknown};
    PipelineWarmupPolicy pipeline_warmup{};
    ImGuiUploadPolicy imgui_upload{};
    DescriptorBindingPolicy descriptor_binding{};
    CompletionSidecarPolicy completion_sidecar{};
    CommandSubmissionPolicy command_submission{};
    MemoryPolicy memory_policy{};
    AggregateMemoryPressurePolicy aggregate_memory_pressure{};
    GraphicsLookupPolicy graphics_lookup{};

    CapabilityRoute timeline_semaphore{CapabilityRoute::Unavailable};
    CapabilityRoute buffer_device_address{CapabilityRoute::Unavailable};
    CapabilityRoute dynamic_rendering{CapabilityRoute::Legacy};
    CapabilityRoute synchronization2{CapabilityRoute::Legacy};
    CapabilityRoute maintenance4{CapabilityRoute::Legacy};
    CapabilityRoute push_descriptors{CapabilityRoute::Legacy};
    CapabilityRoute physical_device_features2{CapabilityRoute::Legacy};
    CapabilityRoute dispatch_base{CapabilityRoute::Unavailable};

    bool vertex_attribute_divisor{};
    bool dynamic_rendering_local_read{};
    bool fragment_shading_rate{};
    bool extended_dynamic_state3{};
    bool robustness2{};
    bool memory_budget{};
    bool shader_float16{};
    // VK_EXT_multisampled_render_to_single_sampled is kept as a complete
    // extension+feature route. Consumers may use it only with dynamic rendering;
    // the legacy render-pass path retains the established sample-count fallback.
    bool multisampled_render_to_single_sampled{};
    PreferredSpatialUpscaler preferred_spatial_upscaler{PreferredSpatialUpscaler::Fsr1};
    bool exact_proprietary_adreno750{};
    bool exact_proprietary_adreno840{};
    bool exact_mesa_turnip_adreno750{};
    bool exact_mesa_turnip_adreno840{};
    bool exact_mesa_turnip_a750_d9_lineage{};
    // Default-OFF exact D9/D19/D20 A750 experiment. Both cache collectors may
    // skip only a proven all-identities-protected LRU scan, never a pressure,
    // emergency, critical, or periodic audit scan.
    bool optimization_a750_protected_gc_fast_noop_ab{};
    bool diagnostic_a750_barrier_capture{};
    bool diagnostic_a750_residency_telemetry{};
    bool diagnostic_a750_present_source_submit_split{};
    bool diagnostic_a750_videoout_candidate_uid{};
    bool diagnostic_a750_videoout_slot0_tracking{};
    bool diagnostic_a750_descriptor_topology{};
    bool diagnostic_a750_image_topology{};
    // Default-OFF exact A750/Turnip experiment. Restricts mutable texture-cache
    // images to the alias domain already accepted by LSX4 compatibility logic.
    bool optimization_a750_bounded_image_format_list{};
    // Default-OFF exact A750/Turnip experiment. EXTENDED_USAGE is omitted only
    // when the selected base format natively supports every declared image
    // usage. Mutable compatible views remain available; unsupported base
    // formats retain the historical flag.
    bool optimization_a750_narrow_image_flags{};
    // Default-OFF exact Turnip A750/D9 experiment. Create texture-cache
    // images with the binding role known at first use instead of declaring
    // sampled+storage+attachment on every small texture. A rare later role
    // expansion copies the full backing before deferred owner retirement.
    bool optimization_a750_role_image_usage{};
    // The proprietary A750 process has a materially smaller Android memory
    // envelope than the validated A840 route. Keep large sparse host metadata
    // demand-backed there and release font-atlas CPU pixels after the completed
    // Vulkan upload. This never changes guest data or GPU image lifetime.
    bool optimization_a750_host_metadata_compaction{};
    // Initially enabled for the physically validated Metal viewport route.
    bool emulate_integer_pixel_centers{false};
    u64 driver_quirks{};

#if defined(LSX4_APPLE_RUNTIME)
    [[nodiscard]] constexpr bool ShouldSubmitDirectDispatch(u32 x, u32 y, u32 z) const {
        // Vulkan permits zero workgroups; Metal rejects the direct empty grid.
        // Test dimensions individually to avoid multiplication overflow.
        return !(driver_quirks & static_cast<u64>(CapabilityQuirk::SkipEmptyDirectDispatch)) ||
               (x != 0 && y != 0 && z != 0);
    }
#endif

    [[nodiscard]] bool HasQuirk(const CapabilityQuirk quirk) const {
        return (driver_quirks & static_cast<u64>(quirk)) != 0;
    }

    [[nodiscard]] constexpr bool ShouldCompactLegacyUnusedColorAttachments() const {
        return dynamic_rendering == CapabilityRoute::Legacy &&
               (driver_quirks & static_cast<u64>(
                                    CapabilityQuirk::CompactLegacyUnusedColorAttachments)) != 0;
    }

    [[nodiscard]] constexpr bool RuntimeCompatible() const {
        return timeline_semaphore != CapabilityRoute::Unavailable &&
               buffer_device_address != CapabilityRoute::Unavailable;
    }
};

static_assert(SpirvVersionForVulkan(VulkanApiVersion10) == 0x00010000U);
static_assert(SpirvVersionForVulkan(VulkanApiVersion11) == 0x00010300U);
static_assert(SpirvVersionForVulkan(VulkanApiVersion12) == 0x00010500U);
static_assert(SpirvVersionForVulkan(VulkanApiVersion13) == 0x00010600U);
static_assert(SpirvVersionForVulkan(VulkanApiVersion14) == 0x00010600U);
static_assert(SelectPromotedRoute(VulkanApiVersion13, VulkanApiVersion13, false, true,
                                  CapabilityRoute::Legacy) == CapabilityRoute::Core);
static_assert(SelectPromotedRoute(VulkanApiVersion11, VulkanApiVersion13, true, true,
                                  CapabilityRoute::Legacy) == CapabilityRoute::KhrExtension);
static_assert(SelectPromotedRoute(VulkanApiVersion13, VulkanApiVersion13, true, false,
                                  CapabilityRoute::Legacy) == CapabilityRoute::Legacy);
static_assert(SelectPromotedRoute(VulkanApiVersion11, VulkanApiVersion13, false, true,
                                  CapabilityRoute::Unavailable) == CapabilityRoute::Unavailable);
static_assert(MobileDriverFamilyFromVendorId(0x13b5) == MobileDriverFamily::ArmProprietary);
static_assert(MobileDriverFamilyFromVendorId(0x5143) == MobileDriverFamily::QualcommProprietary);
static_assert(MobileDriverFamilyFromVendorId(0xffff) == MobileDriverFamily::Unknown);
static_assert(ResolveMobileDriverFamily(MobileDriverFamily::Unknown, 0x5143) ==
              MobileDriverFamily::QualcommProprietary);
static_assert(ResolveMobileDriverFamily(MobileDriverFamily::Unknown, 0x13b5) ==
              MobileDriverFamily::ArmProprietary);
static_assert(ResolveMobileDriverFamily(MobileDriverFamily::ArmProprietary, 0x5143) ==
              MobileDriverFamily::ArmProprietary);
static_assert(ResolveMobileDriverFamily(MobileDriverFamily::MesaTurnip, 0x5143) ==
              MobileDriverFamily::MesaTurnip);
static_assert(ResolvePipelineWarmupPolicy(MobileDriverFamily::MesaTurnip)
                      .maximum_pipelines == 32);
static_assert(ResolvePipelineWarmupPolicy(MobileDriverFamily::MesaTurnip)
                      .time_budget_ms == 1250);
static_assert(!ResolvePipelineWarmupPolicy(MobileDriverFamily::MesaTurnip)
                   .allow_background_warmup);
static_assert(ResolvePipelineWarmupPolicy(MobileDriverFamily::QualcommProprietary)
                      .maximum_pipelines == 8);
static_assert(ResolvePipelineWarmupPolicy(MobileDriverFamily::ArmProprietary)
                      .time_budget_ms == 250);
static_assert(ResolvePipelineWarmupPolicy(MobileDriverFamily::ArmProprietary)
                  .allow_background_warmup);
static_assert(ResolveImGuiUploadPolicy(MobileDriverFamily::MesaTurnip)
                  .allow_persistent_mapping_ab);
static_assert(!ResolveImGuiUploadPolicy(MobileDriverFamily::QualcommProprietary)
                   .allow_persistent_mapping_ab);
static_assert(!ResolveImGuiUploadPolicy(MobileDriverFamily::ArmProprietary)
                   .allow_persistent_mapping_ab);
static_assert(ResolveDescriptorBindingPolicy(
                  GpuCompatibilityRoute::Adreno750MesaTurnip,
                  MobileDriverBuild::MesaTurnipA750Cf8e1b81a6)
                  .allow_exact_write_dedup_ab);
static_assert(!ResolveDescriptorBindingPolicy(
                   GpuCompatibilityRoute::Adreno750QualcommProprietary,
                   MobileDriverBuild::MesaTurnipA750Cf8e1b81a6)
                   .allow_exact_write_dedup_ab);
static_assert(ResolveDescriptorBindingPolicy(
                  GpuCompatibilityRoute::Adreno750QualcommProprietary,
                  MobileDriverBuild::Unknown, true)
                  .legacy_allocation_batch == 4);
static_assert(ResolveDescriptorBindingPolicy(
                  GpuCompatibilityRoute::Adreno750QualcommProprietary,
                  MobileDriverBuild::Unknown, false)
                  .legacy_allocation_batch == 32);
static_assert(ResolveDescriptorBindingPolicy(
                  GpuCompatibilityRoute::Adreno840QualcommProprietary,
                  MobileDriverBuild::Unknown, true)
                  .legacy_allocation_batch == 32);
static_assert(ResolveCompletionSidecarPolicy(MobileDriverFamily::MesaTurnip, false)
                  .diagnostic_allowed);
static_assert(!ResolveCompletionSidecarPolicy(
                   MobileDriverFamily::QualcommProprietary, false)
                   .diagnostic_allowed);
static_assert(ResolveCompletionSidecarPolicy(MobileDriverFamily::MesaTurnip, true)
                  .elide_by_default);
static_assert(ResolveCommandSubmissionPolicy(
                  GpuCompatibilityRoute::Adreno750QualcommProprietary)
                  .maximum_deferred_guest_submit_boundaries == 8);
static_assert(ResolveCommandSubmissionPolicy(
                  GpuCompatibilityRoute::Adreno750QualcommProprietary, true)
                  .maximum_deferred_guest_submit_boundaries == 1);
static_assert(ResolveCommandSubmissionPolicy(
                  GpuCompatibilityRoute::Adreno840QualcommProprietary)
                  .maximum_deferred_guest_submit_boundaries == 64);
static_assert(ResolveCommandSubmissionPolicy(
                  GpuCompatibilityRoute::Adreno750MesaTurnip)
                  .maximum_deferred_guest_submit_boundaries == 64);
static_assert(ResolveCommandSubmissionPolicy(
                  GpuCompatibilityRoute::MaliArmProprietary)
                  .maximum_deferred_guest_submit_boundaries == 64);
static_assert(ResolveAggregateMemoryPressurePolicy(
                  GpuCompatibilityRoute::Adreno750QualcommProprietary, false,
                  ResidencyRetirementMode::LegacyCacheLocal, true)
                  .cache_pressure_active);
static_assert(ResolveAggregateMemoryPressurePolicy(
                  GpuCompatibilityRoute::Adreno750QualcommProprietary, false,
                  ResidencyRetirementMode::LegacyCacheLocal, true)
                  .proprietary_vma_sampling);
static_assert(ResolveAggregateMemoryPressurePolicy(
                  GpuCompatibilityRoute::Adreno750QualcommProprietary, false,
                  ResidencyRetirementMode::LegacyCacheLocal, true)
                  .combined_gc);
static_assert(!ResolveAggregateMemoryPressurePolicy(
                   GpuCompatibilityRoute::Adreno750QualcommProprietary, true,
                   ResidencyRetirementMode::LegacyCacheLocal, true)
                   .cache_pressure_active);
static_assert(ResolveAggregateMemoryPressurePolicy(
                  GpuCompatibilityRoute::Adreno750MesaTurnip, true,
                  ResidencyRetirementMode::CoordinatedNonBlocking, true)
                  .coordinated_turnip_retirement);
static_assert(!ResolveAggregateMemoryPressurePolicy(
                   GpuCompatibilityRoute::Adreno750MesaTurnip, true,
                   ResidencyRetirementMode::CoordinatedNonBlocking, true)
                   .combined_gc);
static_assert(!ResolveAggregateMemoryPressurePolicy(
                   GpuCompatibilityRoute::Adreno840MesaTurnip, false,
                   ResidencyRetirementMode::CoordinatedSustainedNonBlocking, true)
                   .cache_pressure_active);
static_assert(!ResolveAggregateMemoryPressurePolicy(
                   GpuCompatibilityRoute::MaliArmProprietary, false,
                   ResidencyRetirementMode::CoordinatedSustainedNonBlocking, true)
                   .cache_pressure_active);
static_assert(ResolveGraphicsLookupPolicy(
                  GpuCompatibilityRoute::Adreno750QualcommProprietary)
                  .structural_signature_reuse_default);
static_assert(!ResolveGraphicsLookupPolicy(
                   GpuCompatibilityRoute::Adreno750MesaTurnip)
                   .structural_signature_reuse_default);
static_assert(!ResolveGraphicsLookupPolicy(
                   GpuCompatibilityRoute::Adreno750QualcommProprietary, true)
                   .structural_signature_reuse_default);
static_assert(!ResolveGraphicsLookupPolicy(
                   GpuCompatibilityRoute::Adreno840MesaTurnip)
                   .structural_signature_reuse_default);
static_assert(!ResolveGraphicsLookupPolicy(
                   GpuCompatibilityRoute::MaliArmProprietary)
                   .structural_signature_reuse_default);
static_assert(ResolveGraphicsLookupPolicy(GpuCompatibilityRoute::PortableDefault)
                  .allow_generation_guarded_reuse_ab);
static_assert(!ResolveDescriptorBindingPolicy(
                   GpuCompatibilityRoute::Adreno840MesaTurnip,
                   MobileDriverBuild::MesaTurnipA750Cf8e1b81a6)
                   .allow_exact_write_dedup_ab);
static_assert(!ResolveDescriptorBindingPolicy(
                   GpuCompatibilityRoute::Adreno750MesaTurnip,
                   MobileDriverBuild::Unknown)
                   .allow_exact_write_dedup_ab);
static_assert(ResolveMobileGpuModel(QualcommVendorId, Adreno750DeviceId) ==
              MobileGpuModel::Adreno750);
static_assert(ResolveMobileGpuModel(QualcommVendorId, Adreno840DeviceId) ==
              MobileGpuModel::Adreno840);
static_assert(ResolveMobileGpuModel(QualcommVendorId, 0x12345678) ==
              MobileGpuModel::AdrenoOther);
static_assert(ResolveMobileGpuModel(ArmVendorId, 0) == MobileGpuModel::Mali);
static_assert(ResolveMobileGpuModel(0xffff, 0) == MobileGpuModel::Unknown);
static_assert(ResolveMobileGpuGeneration(MobileGpuModel::Adreno750) ==
              MobileGpuGeneration::Adreno7xxGen3);
static_assert(ResolveMobileGpuGeneration(MobileGpuModel::Adreno840) ==
              MobileGpuGeneration::Adreno8xxGen2);
static_assert(ResolveMobileGpuGeneration(MobileGpuModel::AdrenoOther) ==
              MobileGpuGeneration::Unknown);
static_assert(ResolveMobileGpuGeneration(MobileGpuModel::Mali) ==
              MobileGpuGeneration::ArmMali);
static_assert(ResolveGpuCompatibilityRoute(MobileDriverFamily::ArmProprietary,
                                           ArmVendorId, 0) ==
              GpuCompatibilityRoute::MaliArmProprietary);
static_assert(ResolveGpuCompatibilityRoute(
                  MobileDriverFamily::QualcommProprietary, QualcommVendorId,
                  Adreno750DeviceId) ==
              GpuCompatibilityRoute::Adreno750QualcommProprietary);
static_assert(ResolveGpuCompatibilityRoute(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId) ==
              GpuCompatibilityRoute::Adreno750MesaTurnip);
static_assert(ResolveGpuCompatibilityRoute(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno840DeviceId) ==
              GpuCompatibilityRoute::Adreno840MesaTurnip);
// Unknown ICD identity cannot inherit either A750 or A840 policy by vendor ID.
static_assert(ResolveGpuCompatibilityRoute(
                  MobileDriverFamily::Unknown, QualcommVendorId,
                  Adreno750DeviceId) == GpuCompatibilityRoute::PortableDefault);
static_assert(ResolveGpuCompatibilityRoute(
                  MobileDriverFamily::MesaTurnip, ArmVendorId, 0) ==
              GpuCompatibilityRoute::PortableDefault);
static_assert((DriverQuirksForFamily(MobileDriverFamily::ArmProprietary) &
               static_cast<u64>(CapabilityQuirk::DisablePs5WaveShader)) != 0);
static_assert((DriverQuirksForFamily(MobileDriverFamily::QualcommProprietary) &
               static_cast<u64>(CapabilityQuirk::AvoidInRenderingBarrier)) != 0);
static_assert((DriverQuirksForFamily(MobileDriverFamily::MesaTurnip) &
               static_cast<u64>(CapabilityQuirk::DeferAsyncPipelineReplacement)) != 0);
static_assert((DriverQuirksForFamily(MobileDriverFamily::MesaTurnip) &
               static_cast<u64>(CapabilityQuirk::BrokenSpirvFloatControls)) == 0);
static_assert((DriverQuirksForFamily(MobileDriverFamily::MesaTurnip) &
               static_cast<u64>(CapabilityQuirk::DisablePhysicalDrsResolve)) != 0);
static_assert((DriverQuirksForFamily(MobileDriverFamily::QualcommProprietary) &
               static_cast<u64>(
                   CapabilityQuirk::CompactLegacyUnusedColorAttachments)) != 0);
static_assert(IsExactProprietaryAdreno750(MobileDriverFamily::QualcommProprietary,
                                         QualcommVendorId, Adreno750DeviceId));
// Adreno 840 / Snapdragon 8 Elite Gen 5 must never match A750-only experiments.
static_assert(!IsExactProprietaryAdreno750(MobileDriverFamily::QualcommProprietary,
                                           QualcommVendorId, 0x44050a31));
static_assert(IsExactProprietaryAdreno840(MobileDriverFamily::QualcommProprietary,
                                         QualcommVendorId, Adreno840DeviceId));
static_assert(!IsExactProprietaryAdreno840(MobileDriverFamily::QualcommProprietary,
                                          QualcommVendorId, Adreno750DeviceId));
static_assert(!IsExactProprietaryAdreno840(MobileDriverFamily::MesaTurnip,
                                          QualcommVendorId, Adreno840DeviceId));
static_assert(!IsExactProprietaryAdreno840(MobileDriverFamily::ArmProprietary, 0x13b5,
                                           Adreno840DeviceId));
static_assert(IsExactMesaTurnipAdreno840(MobileDriverFamily::MesaTurnip,
                                        QualcommVendorId, Adreno840DeviceId));
static_assert(!IsExactMesaTurnipAdreno840(MobileDriverFamily::MesaTurnip,
                                         QualcommVendorId, Adreno750DeviceId));
static_assert(!IsExactMesaTurnipAdreno840(MobileDriverFamily::QualcommProprietary,
                                         QualcommVendorId, Adreno840DeviceId));
static_assert((ExactMesaTurnipAdreno840RuntimeQuirks(
                   MobileDriverFamily::MesaTurnip, QualcommVendorId,
                   Adreno840DeviceId) &
               static_cast<u64>(CapabilityQuirk::ElideCpuCompletionSidecarBarrier)) != 0);
static_assert(ExactMesaTurnipAdreno840RuntimeQuirks(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId) == 0);
// Turnip must not inherit experiments for the proprietary Qualcomm ICD.
static_assert(!IsExactProprietaryAdreno750(MobileDriverFamily::MesaTurnip, QualcommVendorId,
                                          Adreno750DeviceId));
static_assert(!IsExactProprietaryAdreno750(MobileDriverFamily::ArmProprietary, 0x13b5,
                                          Adreno750DeviceId));
static_assert(!IsExactProprietaryAdreno750(MobileDriverFamily::Unknown, QualcommVendorId,
                                          Adreno750DeviceId));
static_assert(!IsExactProprietaryAdreno750(MobileDriverFamily::QualcommProprietary, 0,
                                          Adreno750DeviceId));
static_assert(!IsExactProprietaryAdreno750(MobileDriverFamily::QualcommProprietary,
                                          QualcommVendorId, 0));
static_assert(IsExactMesaTurnipAdreno750(MobileDriverFamily::MesaTurnip,
                                        QualcommVendorId, Adreno750DeviceId));
static_assert(!IsExactMesaTurnipAdreno750(MobileDriverFamily::QualcommProprietary,
                                         QualcommVendorId, Adreno750DeviceId));
static_assert(!IsExactMesaTurnipAdreno750(MobileDriverFamily::MesaTurnip,
                                         QualcommVendorId, Adreno840DeviceId));
static_assert(ResolveMobileDriverBuild(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId, MesaTurnipA750Cf8e1b81a6DriverVersion,
                  MesaTurnipA750Cf8e1b81a6Sha256) ==
              MobileDriverBuild::MesaTurnipA750Cf8e1b81a6);
static_assert(ResolveMobileDriverBuild(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId, MesaTurnipA750Da14d65e44DriverVersion,
                  MesaTurnipA750Da14d65e44Sha256) ==
              MobileDriverBuild::MesaTurnipA750Da14d65e44);
static_assert(ResolveMobileDriverBuild(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId,
                  MesaTurnipA750Da14d65e44Ib32RolloverDriverVersion,
                  MesaTurnipA750Da14d65e44Ib32RolloverSha256) ==
              MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32Rollover);
static_assert(ResolveMobileDriverBuild(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId,
                  MesaTurnipA750Da14d65e44Ib32RolloverBoStatsDriverVersion,
                  MesaTurnipA750Da14d65e44Ib32RolloverBoStatsSha256) ==
              MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats);
static_assert(ResolveMobileDriverBuild(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId, MesaTurnipA750D19FormattedNoLogDriverVersion,
                  MesaTurnipA750D19FormattedNoLogSha256) ==
              MobileDriverBuild::MesaTurnipA750D19FormattedNoLog);
static_assert(ResolveMobileDriverBuild(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno840DeviceId, MesaTurnipA750D19FormattedNoLogDriverVersion,
                  MesaTurnipA750D19FormattedNoLogSha256) == MobileDriverBuild::Unknown);
static_assert(ResolveMobileDriverBuild(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId, MesaTurnipA750D20NoBoCensusDriverVersion,
                  MesaTurnipA750D20NoBoCensusSha256) ==
              MobileDriverBuild::MesaTurnipA750D20NoBoCensus);
static_assert(ResolveMobileDriverBuild(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno840DeviceId, MesaTurnipA750D20NoBoCensusDriverVersion,
                  MesaTurnipA750D20NoBoCensusSha256) == MobileDriverBuild::Unknown);
static_assert(IsMesaTurnipA750D9LineageBuild(
    MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats));
static_assert(IsMesaTurnipA750D9LineageBuild(
    MobileDriverBuild::MesaTurnipA750D19FormattedNoLog));
static_assert(IsMesaTurnipA750D9LineageBuild(
    MobileDriverBuild::MesaTurnipA750D20NoBoCensus));
static_assert(!IsMesaTurnipA750D9LineageBuild(
    MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32Rollover));
static_assert(ResolveMobileDriverBuild(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId,
                  MesaTurnipA750Da14d65e44Ib32RolloverBoStatsDriverVersion - 1,
                  MesaTurnipA750Da14d65e44Ib32RolloverBoStatsSha256) ==
              MobileDriverBuild::Unknown);
static_assert(ResolveMobileDriverBuild(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno840DeviceId,
                  MesaTurnipA750Da14d65e44Ib32RolloverBoStatsDriverVersion,
                  MesaTurnipA750Da14d65e44Ib32RolloverBoStatsSha256) ==
              MobileDriverBuild::Unknown);
static_assert(ResolveMobileDriverBuild(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno840DeviceId,
                  MesaTurnipA750Da14d65e44Ib32RolloverDriverVersion,
                  MesaTurnipA750Da14d65e44Ib32RolloverSha256) ==
              MobileDriverBuild::Unknown);
static_assert(ResolveMobileDriverBuild(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno840DeviceId, MesaTurnipA750Da14d65e44DriverVersion,
                  MesaTurnipA750Da14d65e44Sha256) == MobileDriverBuild::Unknown);
static_assert(ResolveMobileDriverBuild(
                  MobileDriverFamily::QualcommProprietary, QualcommVendorId,
                  Adreno750DeviceId, MesaTurnipA750Da14d65e44DriverVersion,
                  MesaTurnipA750Da14d65e44Sha256) == MobileDriverBuild::Unknown);
static_assert(ResolveMobileDriverBuild(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId, MesaTurnipA750Da14d65e44DriverVersion,
                  "different-package") == MobileDriverBuild::Unknown);
static_assert(ResolveMobileDriverBuild(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno840DeviceId, MesaTurnipA750Cf8e1b81a6DriverVersion,
                  MesaTurnipA750Cf8e1b81a6Sha256) == MobileDriverBuild::Unknown);
static_assert(ResolveMobileDriverBuild(
                  MobileDriverFamily::QualcommProprietary, QualcommVendorId,
                  Adreno750DeviceId, MesaTurnipA750Cf8e1b81a6DriverVersion,
                  MesaTurnipA750Cf8e1b81a6Sha256) == MobileDriverBuild::Unknown);
static_assert(ResolveMobileDriverBuild(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId, MesaTurnipA750Cf8e1b81a6DriverVersion,
                  "different-package") == MobileDriverBuild::Unknown);
static_assert(HasCandidateA750TurnipComposedMemoryPolicy(
    MobileDriverBuild::MesaTurnipA750Cf8e1b81a6));
static_assert(HasCandidateA750TurnipComposedMemoryPolicy(
    MobileDriverBuild::MesaTurnipA750Da14d65e44));
static_assert(HasCandidateA750TurnipComposedMemoryPolicy(
    MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32Rollover));
static_assert(HasCandidateA750TurnipComposedMemoryPolicy(
    MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats));
static_assert(HasCandidateA750TurnipComposedMemoryPolicy(
    MobileDriverBuild::MesaTurnipA750D19FormattedNoLog));
static_assert(HasCandidateA750TurnipComposedMemoryPolicy(
    MobileDriverBuild::MesaTurnipA750D20NoBoCensus));
static_assert(!HasProductDefaultA750TurnipComposedMemoryPolicy(
    MobileDriverBuild::MesaTurnipA750Cf8e1b81a6));
static_assert(!HasProductDefaultA750TurnipComposedMemoryPolicy(
    MobileDriverBuild::MesaTurnipA750Da14d65e44));
static_assert(!HasProductDefaultA750TurnipComposedMemoryPolicy(
    MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32Rollover));
static_assert(!HasProductDefaultA750TurnipComposedMemoryPolicy(
    MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats));
static_assert(ResolveBdaPageTablePolicy(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId,
                  MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats,
                  true, false, false, true, 128ULL * 1024ULL * 1024ULL)
                  .mode == BdaPageTableMode::SegmentedSparse);
static_assert(ResolveBdaPageTablePolicy(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId,
                  MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats,
                  true, false, false, true, 128ULL * 1024ULL * 1024ULL)
                  .descriptor_segments == 4);
static_assert(ResolveBdaPageTablePolicy(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId,
                  MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats,
                  false, false, false,
                  true, 128ULL * 1024ULL * 1024ULL)
                  .mode == BdaPageTableMode::FlatResident);
static_assert(ResolveBdaPageTablePolicy(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId,
                  MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats,
                  false, false, false, true, 128ULL * 1024ULL * 1024ULL, true)
                  .mode == BdaPageTableMode::FlatSparse);
static_assert(ResolveBdaPageTablePolicy(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno840DeviceId,
                  MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats,
                  false, false, false, true, 128ULL * 1024ULL * 1024ULL, true)
                  .mode == BdaPageTableMode::FlatResident);
static_assert(ResolveBdaPageTablePolicy(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId, MobileDriverBuild::Unknown, true, false, false,
                  true, 128ULL * 1024ULL * 1024ULL)
                  .mode == BdaPageTableMode::FlatResident);
static_assert(ResolveBdaPageTablePolicy(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId,
                  MobileDriverBuild::MesaTurnipA750Cf8e1b81a6, true, false, true,
                  true, 128ULL * 1024ULL * 1024ULL)
                  .mode == BdaPageTableMode::FlatResident);
static_assert(ResolveBdaPageTablePolicy(
                  MobileDriverFamily::QualcommProprietary, QualcommVendorId,
                  Adreno750DeviceId,
                  MobileDriverBuild::MesaTurnipA750Cf8e1b81a6, true, false, false,
                  true, 128ULL * 1024ULL * 1024ULL)
                  .mode == BdaPageTableMode::FlatResident);
static_assert(ResolveBdaPageTablePolicy(
                  MobileDriverFamily::QualcommProprietary, QualcommVendorId,
                  Adreno750DeviceId, MobileDriverBuild::Unknown, false, true, false,
                  true, 128ULL * 1024ULL * 1024ULL)
                  .mode == BdaPageTableMode::SegmentedSparse);
static_assert(ResolveBdaPageTablePolicy(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno840DeviceId,
                  MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats,
                  true, false, false,
                  true, 128ULL * 1024ULL * 1024ULL)
                  .mode == BdaPageTableMode::FlatResident);
static_assert(ResolveBdaPageTablePolicy(
                  MobileDriverFamily::ArmProprietary, ArmVendorId, 0,
                  MobileDriverBuild::Unknown, true, true, false, true,
                  128ULL * 1024ULL * 1024ULL)
                  .mode == BdaPageTableMode::FlatResident);
static_assert(ResolveBdaPageTablePolicy(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId,
                  MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats,
                  true, false, false, false, 128ULL * 1024ULL * 1024ULL)
                  .mode == BdaPageTableMode::FlatResident);
static_assert(ResolveBdaPageTablePolicy(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId,
                  MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats,
                  true, false, false, true, 192ULL * 1024ULL * 1024ULL)
                  .mode == BdaPageTableMode::FlatResident);
static_assert(ResolveBdaPageTablePolicy(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId,
                  MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats,
                  true, false, false, true, 256ULL * 1024ULL * 1024ULL)
                  .mode == BdaPageTableMode::FlatResident);
static_assert(ResolveBdaPageTablePolicy(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId,
                  MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats,
                  true, false, false, true, 512ULL * 1024ULL * 1024ULL)
                  .mode == BdaPageTableMode::FlatResident);
static_assert(ResolveMobileMemoryPolicy(MobileDriverFamily::QualcommProprietary,
                                        QualcommVendorId, Adreno750DeviceId,
                                        MobileDriverBuild::Unknown)
                  .preferred_large_heap_block_size == 64ULL * 1024ULL * 1024ULL);
static_assert(ResolveMobileMemoryPolicy(MobileDriverFamily::QualcommProprietary,
                                        QualcommVendorId, Adreno750DeviceId,
                                        MobileDriverBuild::Unknown)
                  .upload_reservation == UploadReservationMode::ExactDirtyRanges);
static_assert(ResolveMobileMemoryPolicy(MobileDriverFamily::QualcommProprietary,
                                        QualcommVendorId, Adreno750DeviceId,
                                        MobileDriverBuild::Unknown)
                  .upload_reservation_scope ==
              UploadReservationScope::CaptureBoundaryOnly);
static_assert(ResolveMobileMemoryPolicy(
                  MobileDriverFamily::QualcommProprietary, QualcommVendorId,
                  Adreno750DeviceId, MobileDriverBuild::Unknown, false, false,
                  false, false, false, true)
                  .upload_reservation_scope ==
              UploadReservationScope::AllCpuDirtyUploads);
static_assert(ResolveMobileMemoryPolicy(
                  MobileDriverFamily::QualcommProprietary, QualcommVendorId,
                  Adreno840DeviceId, MobileDriverBuild::Unknown, false, false,
                  false, false, false, true)
                  .upload_reservation_scope ==
              UploadReservationScope::CaptureBoundaryOnly);
static_assert(ResolveMobileMemoryPolicy(MobileDriverFamily::QualcommProprietary,
                                        QualcommVendorId, Adreno840DeviceId,
                                        MobileDriverBuild::Unknown)
                  .preferred_large_heap_block_size == 0);
static_assert(ResolveMobileMemoryPolicy(MobileDriverFamily::MesaTurnip,
                                        QualcommVendorId, Adreno750DeviceId,
                                        MobileDriverBuild::MesaTurnipA750Cf8e1b81a6)
                  .preferred_large_heap_block_size == 0);
static_assert(ResolveMobileMemoryPolicy(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId,
                  MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats,
                  false, false, false, true)
                  .preferred_large_heap_block_size == 32ULL * 1024ULL * 1024ULL);
static_assert(ResolveMobileMemoryPolicy(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno840DeviceId,
                  MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats,
                  false, false, false, true)
                  .preferred_large_heap_block_size == 0);
static_assert(ResolveMobileMemoryPolicy(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId,
                  MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats,
                  false, true, false, true)
                  .preferred_large_heap_block_size == 0);
static_assert(ResolveMobileMemoryPolicy(MobileDriverFamily::MesaTurnip,
                                        QualcommVendorId, Adreno750DeviceId,
                                        MobileDriverBuild::MesaTurnipA750Cf8e1b81a6,
                                        false, false, true)
                  .upload_reservation == UploadReservationMode::ExactDirtyRanges);
static_assert(ResolveMobileMemoryPolicy(MobileDriverFamily::MesaTurnip,
                                        QualcommVendorId, Adreno750DeviceId,
                                        MobileDriverBuild::MesaTurnipA750Cf8e1b81a6,
                                        false, false, true)
                  .upload_reservation_scope ==
              UploadReservationScope::AllCpuDirtyUploads);
static_assert(ResolveMobileMemoryPolicy(MobileDriverFamily::MesaTurnip,
                                        QualcommVendorId, Adreno750DeviceId,
                                        MobileDriverBuild::MesaTurnipA750Da14d65e44,
                                        false, false, true)
                  .upload_reservation == UploadReservationMode::ExactDirtyRanges);
static_assert(ResolveMobileMemoryPolicy(MobileDriverFamily::MesaTurnip,
                                        QualcommVendorId, Adreno750DeviceId,
                                        MobileDriverBuild::MesaTurnipA750Da14d65e44,
                                        false, false, true)
                  .upload_reservation_scope ==
              UploadReservationScope::AllCpuDirtyUploads);
static_assert(ResolveMobileMemoryPolicy(MobileDriverFamily::MesaTurnip,
                                        QualcommVendorId, Adreno750DeviceId,
                                        MobileDriverBuild::Unknown, false, false, true)
                  .upload_reservation == UploadReservationMode::FullSpan);
static_assert(ResolveMobileMemoryPolicy(MobileDriverFamily::MesaTurnip,
                                        QualcommVendorId, Adreno840DeviceId,
                                        MobileDriverBuild::MesaTurnipA750Cf8e1b81a6,
                                        false, false, true)
                  .upload_reservation == UploadReservationMode::FullSpan);
static_assert(ResolveMobileMemoryPolicy(MobileDriverFamily::MesaTurnip,
                                        QualcommVendorId, Adreno840DeviceId,
                                        MobileDriverBuild::MesaTurnipA750Cf8e1b81a6,
                                        false, false, true)
                  .upload_reservation_scope ==
              UploadReservationScope::CaptureBoundaryOnly);
static_assert(ResolveMobileMemoryPolicy(MobileDriverFamily::ArmProprietary,
                                        ArmVendorId, 0,
                                        MobileDriverBuild::MesaTurnipA750Cf8e1b81a6,
                                        false, false, true)
                  .upload_reservation == UploadReservationMode::FullSpan);
static_assert(ResolveMobileMemoryPolicy(MobileDriverFamily::MesaTurnip,
                                        QualcommVendorId, Adreno750DeviceId,
                                        MobileDriverBuild::MesaTurnipA750Cf8e1b81a6,
                                        false, true)
                  .preferred_large_heap_block_size == 0);
static_assert(ResolveMobileMemoryPolicy(MobileDriverFamily::ArmProprietary,
                                        ArmVendorId, 0, MobileDriverBuild::Unknown)
                  .preferred_large_heap_block_size == 0);
static_assert(ResolveMobileMemoryPolicy(MobileDriverFamily::MesaTurnip,
                                        QualcommVendorId, Adreno750DeviceId,
                                        MobileDriverBuild::Unknown, true)
                  .budget_accounting ==
              MemoryBudgetAccounting::ReportedUsageWithInitialBaseline);
static_assert(ResolveMobileMemoryPolicy(MobileDriverFamily::MesaTurnip,
                                        QualcommVendorId, Adreno750DeviceId,
                                        MobileDriverBuild::Unknown, false)
                  .budget_accounting ==
              MemoryBudgetAccounting::LegacyRendererShare);
static_assert(ResolveMobileMemoryPolicy(MobileDriverFamily::MesaTurnip,
                                        QualcommVendorId, Adreno840DeviceId,
                                        MobileDriverBuild::Unknown, true)
                  .budget_accounting ==
              MemoryBudgetAccounting::LegacyRendererShare);
static_assert(ResolveMobileMemoryPolicy(MobileDriverFamily::QualcommProprietary,
                                        QualcommVendorId, Adreno750DeviceId,
                                        MobileDriverBuild::Unknown, true)
                  .budget_accounting ==
              MemoryBudgetAccounting::LegacyRendererShare);
static_assert(ResolveMobileMemoryPolicy(MobileDriverFamily::ArmProprietary,
                                        ArmVendorId, 0, MobileDriverBuild::Unknown,
                                        true)
                  .budget_accounting ==
              MemoryBudgetAccounting::LegacyRendererShare);
static_assert(ResolveMobileMemoryPolicy(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId, MobileDriverBuild::MesaTurnipA750Cf8e1b81a6,
                  false, false, false, true)
                  .budget_accounting ==
              MemoryBudgetAccounting::ReportedBudget70Percent);
static_assert(ResolveMobileMemoryPolicy(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId, MobileDriverBuild::MesaTurnipA750Cf8e1b81a6,
                  false, false, false, true)
                  .residency_retirement ==
              ResidencyRetirementMode::CoordinatedNonBlocking);
static_assert(ResolveMobileMemoryPolicy(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId, MobileDriverBuild::MesaTurnipA750Cf8e1b81a6,
                  false, false, false, false, true)
                  .residency_retirement ==
              ResidencyRetirementMode::CoordinatedSustainedNonBlocking);
static_assert(ResolveMobileMemoryPolicy(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno840DeviceId, MobileDriverBuild::MesaTurnipA750Cf8e1b81a6,
                  false, false, false, false, true)
                  .residency_retirement == ResidencyRetirementMode::LegacyCacheLocal);
static_assert(ResolveMobileMemoryPolicy(
                  MobileDriverFamily::ArmProprietary, ArmVendorId, 0,
                  MobileDriverBuild::MesaTurnipA750Cf8e1b81a6,
                  false, false, false, false, true)
                  .budget_accounting == MemoryBudgetAccounting::LegacyRendererShare);
static_assert(ResolveMobileMemoryPolicy(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId, MobileDriverBuild::Unknown,
                  false, false, false, false, true)
                  .residency_retirement == ResidencyRetirementMode::LegacyCacheLocal);
static_assert(EnableExactProprietaryAdreno750BarrierCapture(
    MobileDriverFamily::QualcommProprietary, QualcommVendorId, Adreno750DeviceId, true));
static_assert(!EnableExactProprietaryAdreno750BarrierCapture(
    MobileDriverFamily::QualcommProprietary, QualcommVendorId, 0x44050a31, true));
static_assert(!EnableExactProprietaryAdreno750BarrierCapture(
    MobileDriverFamily::MesaTurnip, QualcommVendorId, Adreno750DeviceId, true));
static_assert(!EnableExactProprietaryAdreno750BarrierCapture(
    MobileDriverFamily::QualcommProprietary, QualcommVendorId, Adreno750DeviceId, false));
static_assert(EnableExactA750TurnipD9ResidencyTelemetry(
    GpuCompatibilityRoute::Adreno750MesaTurnip,
    MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats, true));
static_assert(EnableExactA750TurnipD9ResidencyTelemetry(
    GpuCompatibilityRoute::Adreno750MesaTurnip,
    MobileDriverBuild::MesaTurnipA750D19FormattedNoLog, true));
static_assert(EnableExactA750TurnipD9ResidencyTelemetry(
    GpuCompatibilityRoute::Adreno750MesaTurnip,
    MobileDriverBuild::MesaTurnipA750D20NoBoCensus, true));
static_assert(!EnableExactA750TurnipD9ResidencyTelemetry(
    GpuCompatibilityRoute::Adreno840MesaTurnip,
    MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats, true));
static_assert(!EnableExactA750TurnipD9ResidencyTelemetry(
    GpuCompatibilityRoute::Adreno840QualcommProprietary,
    MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats, true));
static_assert(!EnableExactA750TurnipD9ResidencyTelemetry(
    GpuCompatibilityRoute::MaliArmProprietary,
    MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats, true));
static_assert(!EnableExactA750TurnipD9ResidencyTelemetry(
    GpuCompatibilityRoute::Adreno750QualcommProprietary,
    MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats, true));
static_assert(!EnableExactA750TurnipD9ResidencyTelemetry(
    GpuCompatibilityRoute::Adreno750MesaTurnip,
    MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32Rollover, true));
static_assert(!EnableExactA750TurnipD9ResidencyTelemetry(
    GpuCompatibilityRoute::Adreno750MesaTurnip, MobileDriverBuild::Unknown, true));
static_assert(!EnableExactA750TurnipD9ResidencyTelemetry(
    GpuCompatibilityRoute::PortableDefault, MobileDriverBuild::Unknown, true));
static_assert(!EnableExactA750TurnipD9ResidencyTelemetry(
    GpuCompatibilityRoute::Adreno750MesaTurnip,
    MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats, false));
static_assert((ExactProprietaryAdreno750RuntimeQuirks(
                   MobileDriverFamily::QualcommProprietary, QualcommVendorId,
                   Adreno750DeviceId) &
               static_cast<u64>(CapabilityQuirk::RequireStorageBufferReadWriteBarrier)) != 0);
static_assert(ExactProprietaryAdreno750RuntimeQuirks(
                  MobileDriverFamily::QualcommProprietary, QualcommVendorId,
                  0x44050a31) == 0);
static_assert(ExactProprietaryAdreno750RuntimeQuirks(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId) == 0);
static_assert(ExactProprietaryAdreno750RuntimeQuirks(
                  MobileDriverFamily::ArmProprietary, 0x13b5,
                  Adreno750DeviceId) == 0);
static_assert((ExactProprietaryAdreno750PipelineQuirks(
                   MobileDriverFamily::QualcommProprietary, QualcommVendorId,
                   Adreno750DeviceId) &
               static_cast<u64>(
                   CapabilityQuirk::DeferFormattedBufferTypesUntilLowering)) != 0);
static_assert((ExactProprietaryAdreno750PipelineQuirks(
                   MobileDriverFamily::QualcommProprietary, QualcommVendorId,
                   Adreno750DeviceId) &
               static_cast<u64>(CapabilityQuirk::LowerAliasedSharedU64ToU32)) != 0);
static_assert(ExactProprietaryAdreno750PipelineQuirks(
                  MobileDriverFamily::QualcommProprietary, QualcommVendorId,
                  Adreno750DeviceId, false, false) == 0);
static_assert((ExactProprietaryAdreno750PipelineQuirks(
                   MobileDriverFamily::QualcommProprietary, QualcommVendorId,
                   Adreno750DeviceId, false, true) &
               static_cast<u64>(CapabilityQuirk::LowerAliasedSharedU64ToU32)) != 0);
static_assert((ExactProprietaryAdreno750PipelineQuirks(
                   MobileDriverFamily::QualcommProprietary, QualcommVendorId,
                   Adreno750DeviceId, true, false) &
               static_cast<u64>(
                   CapabilityQuirk::DeferFormattedBufferTypesUntilLowering)) != 0);
static_assert((ExactProprietaryAdreno750PipelineQuirks(
                   MobileDriverFamily::QualcommProprietary, QualcommVendorId,
                   Adreno750DeviceId) &
               static_cast<u64>(
                   CapabilityQuirk::AvoidRequiredSubgroupSizeForSubgroupIndependentCompute)) == 0);
static_assert(ExactProprietaryAdreno750PipelineQuirks(
                  MobileDriverFamily::QualcommProprietary, QualcommVendorId,
                  0x44050a31) == 0);
static_assert(ExactProprietaryAdreno750PipelineQuirks(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId) == 0);
static_assert(ExactProprietaryAdreno750PipelineQuirks(
                   MobileDriverFamily::ArmProprietary, 0x13b5,
                   Adreno750DeviceId) == 0);
static_assert((ExactProprietaryAdreno840PipelineQuirks(
                   MobileDriverFamily::QualcommProprietary, QualcommVendorId,
                   Adreno840DeviceId) &
               static_cast<u64>(
                   CapabilityQuirk::AvoidRequiredSubgroupSizeForSubgroupIndependentCompute)) != 0);
static_assert((ExactProprietaryAdreno840PipelineQuirks(
                   MobileDriverFamily::QualcommProprietary, QualcommVendorId,
                   Adreno840DeviceId) &
               static_cast<u64>(CapabilityQuirk::LowerNon32BitSharedToU32)) != 0);
static_assert((ExactProprietaryAdreno840PipelineQuirks(
                   MobileDriverFamily::QualcommProprietary, QualcommVendorId,
                   Adreno840DeviceId) &
               static_cast<u64>(CapabilityQuirk::LowerAliasedSharedU64ToU32)) == 0);
static_assert(ExactProprietaryAdreno840PipelineQuirks(
                  MobileDriverFamily::QualcommProprietary, QualcommVendorId,
                  Adreno750DeviceId) == 0);
static_assert(ExactProprietaryAdreno840PipelineQuirks(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno840DeviceId) == 0);
static_assert(ExactProprietaryAdreno840PipelineQuirks(
                  MobileDriverFamily::ArmProprietary, 0x13b5,
                  Adreno840DeviceId) == 0);
static_assert((ExactProprietaryAdreno750Quirks(
                   MobileDriverFamily::QualcommProprietary, QualcommVendorId,
                   Adreno750DeviceId, true, false, false, false) &
               static_cast<u64>(CapabilityQuirk::ConservativeImageTransitionStages)) != 0);
static_assert(ExactProprietaryAdreno750Quirks(
                  MobileDriverFamily::QualcommProprietary, QualcommVendorId,
                  0x44050a31, true, true, true, true) == 0);
static_assert(ExactProprietaryAdreno750Quirks(
                  MobileDriverFamily::MesaTurnip, QualcommVendorId,
                  Adreno750DeviceId, true, true, true, true) == 0);
static_assert(ExactProprietaryAdreno750Quirks(
                  MobileDriverFamily::ArmProprietary, 0x13b5,
                  Adreno750DeviceId, true, true, true, true) == 0);
static_assert(ExactProprietaryAdreno750Quirks(
                  MobileDriverFamily::QualcommProprietary, QualcommVendorId,
                  Adreno750DeviceId, false, false, false, false) == 0);
static_assert((ExactProprietaryAdreno750Quirks(
                   MobileDriverFamily::QualcommProprietary, QualcommVendorId,
                   Adreno750DeviceId, false, true, false, false) &
               static_cast<u64>(CapabilityQuirk::ExplicitFullImageSubresourceCounts)) != 0);
static_assert((ExactProprietaryAdreno750Quirks(
                   MobileDriverFamily::QualcommProprietary, QualcommVendorId,
                   Adreno750DeviceId, false, false, true, false) &
               static_cast<u64>(CapabilityQuirk::ReadOnlyDepthStencilAccess)) != 0);
static_assert((ExactProprietaryAdreno750Quirks(
                   MobileDriverFamily::QualcommProprietary, QualcommVendorId,
                   Adreno750DeviceId, false, false, false, true) &
               static_cast<u64>(CapabilityQuirk::SplitDepthStencilImageBarriers)) != 0);
static_assert(PipelineStorageDriverQuirks(RuntimeOnlyBarrierQuirks) == 0);
static_assert((PipelineStorageDriverQuirks(
                   RuntimeOnlyBarrierQuirks |
                   static_cast<u64>(CapabilityQuirk::RequireStaticDepthStencil)) &
               static_cast<u64>(CapabilityQuirk::RequireStaticDepthStencil)) != 0);
static_assert(CapabilityProfile{.exact_proprietary_adreno750 = true}
                  .exact_proprietary_adreno750);
static_assert(!CapabilityProfile{}.exact_proprietary_adreno750);
static_assert(!CapabilityProfile{}.exact_proprietary_adreno840);
static_assert(!CapabilityProfile{}.exact_mesa_turnip_adreno750);
static_assert(!CapabilityProfile{}.exact_mesa_turnip_adreno840);
static_assert(!CapabilityProfile{}.optimization_a750_protected_gc_fast_noop_ab);
static_assert(CapabilityProfile{}.gpu_compatibility_route ==
              GpuCompatibilityRoute::PortableDefault);
static_assert(!CapabilityProfile{}.aggregate_memory_pressure.cache_pressure_active);
static_assert(!CapabilityProfile{}.aggregate_memory_pressure.proprietary_vma_sampling);
static_assert(!CapabilityProfile{}.aggregate_memory_pressure.coordinated_turnip_retirement);
static_assert(!CapabilityProfile{}.aggregate_memory_pressure.combined_gc);
static_assert(!CapabilityProfile{}.diagnostic_a750_present_source_submit_split);
static_assert(!CapabilityProfile{}.diagnostic_a750_videoout_candidate_uid);
static_assert(!CapabilityProfile{}.diagnostic_a750_videoout_slot0_tracking);
static_assert(!CapabilityProfile{}.diagnostic_a750_descriptor_topology);
static_assert(!CapabilityProfile{}.diagnostic_a750_image_topology);
static_assert(!CapabilityProfile{}.optimization_a750_bounded_image_format_list);
static_assert(!CapabilityProfile{}.optimization_a750_narrow_image_flags);
static_assert(!CapabilityProfile{}.optimization_a750_role_image_usage);
static_assert(CompactTrailingAttachmentCount(1, 0) == 0);
static_assert(CompactTrailingAttachmentCount(4, 0b0011) == 2);
static_assert(CompactTrailingAttachmentCount(4, 0b0101) == 3);
static_assert(CapabilityProfile{
                  .dynamic_rendering = CapabilityRoute::Legacy,
                  .driver_quirks =
                      DriverQuirksForFamily(MobileDriverFamily::QualcommProprietary),
              }.ShouldCompactLegacyUnusedColorAttachments());
static_assert(!CapabilityProfile{
                   .dynamic_rendering = CapabilityRoute::Core,
                   .driver_quirks =
                       DriverQuirksForFamily(MobileDriverFamily::QualcommProprietary),
               }.ShouldCompactLegacyUnusedColorAttachments());
static_assert(!CapabilityProfile{
                   .dynamic_rendering = CapabilityRoute::Legacy,
                   .driver_quirks = DriverQuirksForFamily(MobileDriverFamily::ArmProprietary),
               }.ShouldCompactLegacyUnusedColorAttachments());
static_assert(CapabilityProfile{
                  .timeline_semaphore = CapabilityRoute::Legacy,
                  .buffer_device_address = CapabilityRoute::Legacy,
              }.RuntimeCompatible());
static_assert(!CapabilityProfile{
                   .timeline_semaphore = CapabilityRoute::Unavailable,
                   .buffer_device_address = CapabilityRoute::Legacy,
               }.RuntimeCompatible());

} // namespace Vulkan
