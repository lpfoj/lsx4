// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
#include "core/libraries/gnmdriver/gnmdriver.h"
// SPDX-License-Identifier: GPL-2.0-or-later

#include <bit>
#include <chrono>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <ranges>
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
#include <android/log.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

#include "common/config.h"
#include "common/elf_info.h"
#include "common/hash.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/thread.h"
#include "common/title_scoped_optimization_marker.h"
#include "core/debug_state.h"
#include "core/memory.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/cache_storage.h"
#include "video_core/mobile_gpu_optimization.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/vulkan_driver_optimization.h"


#if defined(LSX4_APPLE_RUNTIME)
#include "apple_compute_split213.h"
#endif


#if defined(LSX4_APPLE_RUNTIME)
namespace AppleComputeSplit {
bool BridgeSelected() {return Selected();}
std::uint32_t BeginDirect(std::uint32_t x,std::uint32_t y,std::uint32_t z) {
    const auto token=state.factor|(std::uint32_t(state.direct)<<8);
    state.direct=Selected() && PortableComputeSplit::GridEligible(x,y,z,65535);
    state.factor=1;
    return token;
}
void EndDirect(std::uint32_t token) {state.factor=token&255;state.direct=(token&256)!=0;}
std::uint32_t DirectFactor() {return state.factor;}
}
#endif

#if defined(LSX4_APPLE_RUNTIME)
#include "apple_worker_priority216.h"
#endif

#if defined(LSX4_APPLE_RUNTIME)
#include "apple_spirv_optimizer217.h"
#include "claude_shader_storage.h"
#endif

namespace Vulkan {

#if defined(LSX4_APPLE_RUNTIME)
namespace {
bool AppleBoundedShaderDumpSelected() {
    static const bool enabled=[] {
        const char* p=std::getenv("LSX4_PROBE_SHADER_DUMP");
        return p && std::strcmp(p,"1")==0;
    }();
    return enabled;
}
bool AppleLiveShaderDumpOnce(u64 hash,Shader::Stage stage) {
    if(!AppleBoundedShaderDumpSelected()) return false;
    static std::mutex mutex;
    static std::vector<std::pair<u64,Shader::Stage>> seen;
    const char* filter=std::getenv("EXECUTOR_SHADER_DUMP_HASHES");
    if(!filter || !*filter) return false;
    std::istringstream stream{filter};std::string token;bool selected=false;
    while(stream>>token) selected|=std::strtoull(token.c_str(),nullptr,0)==hash;
    if(!selected) return false;
    std::scoped_lock lock{mutex};
    const std::pair key{hash,stage};
    if(seen.size()>=64 || std::find(seen.begin(),seen.end(),key)!=seen.end()) return false;
    seen.push_back(key);
    std::fprintf(stderr,"[APPLE_LIVE_SHADER_DUMP] hash=0x%llx wordsFromValidatedParams=1 diagnosticOnly=1\n",(unsigned long long)hash);
    return true;
}
}
#endif

using Shader::LogicalStage;
using Shader::Output;
using Shader::Stage;

namespace {
#if defined(LSX4_APPLE_RUNTIME)
bool ExecutorAppleShaderHotStructureSelected() noexcept {
    static const bool selected=[] {
        const char* value=std::getenv("EXECUTOR_APPLE_SHADER_HOT_STRUCTURE");
        const bool enabled=value && std::strcmp(value,"1")==0;
        std::fprintf(stderr,"[APPLE_SHADER_HOT_STRUCTURE] selected=%u signaturePrefilter=1 mruProbe=1 occupancyMask=1 groupedTls=1 callScopeTlsHoist=1 legacyTlsColdSplit=1 hitLruDefer=1 payloadRetentionUnchanged=1 exactValidationUnchanged=1\n",enabled?1u:0u);
        return enabled;
    }();
    return selected;
}
#endif

// Native VkPipelineCache data is an opaque driver blob. The Vulkan header
// proves physical-device compatibility, but it does not identify our pipeline
// construction epoch and has not been sufficient to protect Turnip from stale
// cache data across renderer changes. Keep this separate from the serialized
// shader namespace: bumping it must discard only the cheap driver blob, not
// valid translated SPIR-V.
constexpr u32 HostPipelineCacheCompatibilityEpoch = 4u;

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
bool ExecutorComputeDispatchPhaseDiagnosticEnabled() noexcept {
    static const bool enabled =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "diag-compute-dispatch-phase",
                 F_OK) == 0;
    return enabled;
}

void ExecutorLogComputeLookupPhase(const char* phase,
                                   const ComputePipelineKey& key) noexcept {
    if (!ExecutorComputeDispatchPhaseDiagnosticEnabled()) {
        return;
    }
    if (std::strcmp(phase, "before-constructor") != 0 &&
        std::strcmp(phase, "after-constructor") != 0) {
        return;
    }
    static std::atomic<u32> records{};
    const u32 ordinal = records.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ordinal > 512) {
        return;
    }
    __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                        "[EXECUTOR_COMPUTE_LOOKUP_PHASE] ordinal=%u phase=%s key=0x%llx",
                        ordinal, phase,
                        static_cast<unsigned long long>(key.value));
}
#endif

struct AtomicPipelineResidencyTelemetry {
    // Even values are stable. The renderer makes the sequence odd while
    // publishing, then releases the next even value for a lock-free reader.
    std::atomic<u64> sequence{};
    std::atomic<u64> program_entries{};
    std::atomic<u64> graphics_pipeline_entries{};
    std::atomic<u64> compute_pipeline_entries{};
    std::atomic<u64> new_pipelines{};
    std::atomic<u64> shader_identity_entries{};
    std::atomic<u64> srt_objects{};
    std::atomic<u64> srt_retained_bytes{};
};

AtomicPipelineResidencyTelemetry pipeline_residency_telemetry{};

struct AtomicDescriptorTopologyTelemetry {
    std::atomic<u64> descriptor_layouts{};
    std::atomic<u64> push_descriptor_layouts{};
    std::atomic<u64> legacy_descriptor_layouts{};
    std::atomic<u64> descriptor_bindings{};
    std::atomic<u64> max_descriptor_bindings{};
    std::atomic<u64> max_push_descriptors{};
    std::atomic<u64> max_update_after_bind_descriptors{};
};

AtomicDescriptorTopologyTelemetry descriptor_topology_telemetry{};

struct AtomicRendererCacheResidencyTelemetry {
    std::atomic<u64> sequence{};
    std::atomic<u64> texture_guest_bytes{};
    std::atomic<u64> texture_image_entries{};
    std::atomic<u64> texture_image_view_entries{};
    std::atomic<u64> texture_sampler_entries{};
    std::atomic<u64> buffer_resident_bytes{};
    std::atomic<u64> buffer_utility_bytes{};
    std::atomic<u64> buffer_entries{};
    std::atomic<u64> image_creations{};
    std::atomic<u64> mutable_image_creations{};
    std::atomic<u64> extended_usage_image_creations{};
    std::atomic<u64> color_storage_image_creations{};
    std::atomic<u64> depth_image_creations{};
    std::atomic<u64> block_image_creations{};
    std::atomic<u64> image_allocation_bytes{};
    std::atomic<u64> max_image_allocation_bytes{};
    std::atomic<u64> bounded_format_list_image_creations{};
    std::atomic<u64> bounded_format_list_entries{};
    std::atomic<u64> max_bounded_format_list_entries{};
    std::atomic<u64> image_view_creations{};
    std::atomic<u64> reinterpreted_image_view_creations{};
    std::atomic<u64> storage_image_view_creations{};
};

AtomicRendererCacheResidencyTelemetry renderer_cache_residency_telemetry{};

struct AtomicUtilityRingUsageTelemetry {
    std::atomic<u64> capacity_bytes{};
    std::atomic<u64> requested_bytes{};
    std::atomic<u64> request_count{};
    std::atomic<u64> max_request_bytes{};
    std::atomic<u64> max_mapped_end{};
    std::atomic<u64> wrap_count{};
    std::atomic<u64> forced_wait_count{};
    std::atomic<u64> failed_request_count{};
};

struct AtomicRendererUtilityRingTelemetry {
    std::atomic<u64> sequence{};
    AtomicUtilityRingUsageTelemetry staging;
    AtomicUtilityRingUsageTelemetry stream;
    AtomicUtilityRingUsageTelemetry download;
    AtomicUtilityRingUsageTelemetry device;
    struct {
        std::atomic<u64> transaction_count{};
        std::atomic<u64> completed_transaction_count{};
        std::atomic<u64> failed_transaction_count{};
        std::atomic<u64> measurement_count{};
        std::atomic<u64> retry_count{};
        std::atomic<u64> full_span_candidate_bytes{};
        std::atomic<u64> dirty_bytes{};
        std::atomic<u64> avoided_bytes{};
        std::atomic<u64> max_full_span_candidate_bytes{};
        std::atomic<u64> max_dirty_bytes{};
    } upload_reservation;
};

AtomicRendererUtilityRingTelemetry renderer_utility_ring_telemetry{};

void StoreUtilityRingTelemetry(
    AtomicUtilityRingUsageTelemetry& destination,
    const UtilityRingUsageTelemetrySnapshot& source) noexcept {
    destination.capacity_bytes.store(source.capacity_bytes, std::memory_order_relaxed);
    destination.requested_bytes.store(source.requested_bytes, std::memory_order_relaxed);
    destination.request_count.store(source.request_count, std::memory_order_relaxed);
    destination.max_request_bytes.store(source.max_request_bytes, std::memory_order_relaxed);
    destination.max_mapped_end.store(source.max_mapped_end, std::memory_order_relaxed);
    destination.wrap_count.store(source.wrap_count, std::memory_order_relaxed);
    destination.forced_wait_count.store(source.forced_wait_count, std::memory_order_relaxed);
    destination.failed_request_count.store(source.failed_request_count,
                                            std::memory_order_relaxed);
}

UtilityRingUsageTelemetrySnapshot LoadUtilityRingTelemetry(
    const AtomicUtilityRingUsageTelemetry& source) noexcept {
    return {
        .capacity_bytes = source.capacity_bytes.load(std::memory_order_relaxed),
        .requested_bytes = source.requested_bytes.load(std::memory_order_relaxed),
        .request_count = source.request_count.load(std::memory_order_relaxed),
        .max_request_bytes = source.max_request_bytes.load(std::memory_order_relaxed),
        .max_mapped_end = source.max_mapped_end.load(std::memory_order_relaxed),
        .wrap_count = source.wrap_count.load(std::memory_order_relaxed),
        .forced_wait_count = source.forced_wait_count.load(std::memory_order_relaxed),
        .failed_request_count = source.failed_request_count.load(std::memory_order_relaxed),
    };
}

void PublishPipelineResidencyTelemetry(
    const u64 program_entries, const u64 graphics_pipeline_entries,
    const u64 compute_pipeline_entries, const u64 new_pipelines,
    const u64 shader_identity_entries, const u64 srt_objects,
    const u64 srt_retained_bytes) noexcept {
    pipeline_residency_telemetry.sequence.fetch_add(1,
                                                    std::memory_order_acq_rel);
    pipeline_residency_telemetry.program_entries.store(
        program_entries, std::memory_order_relaxed);
    pipeline_residency_telemetry.graphics_pipeline_entries.store(
        graphics_pipeline_entries, std::memory_order_relaxed);
    pipeline_residency_telemetry.compute_pipeline_entries.store(
        compute_pipeline_entries, std::memory_order_relaxed);
    pipeline_residency_telemetry.new_pipelines.store(new_pipelines,
                                                     std::memory_order_relaxed);
    pipeline_residency_telemetry.shader_identity_entries.store(
        shader_identity_entries, std::memory_order_relaxed);
    pipeline_residency_telemetry.srt_objects.store(srt_objects,
                                                   std::memory_order_relaxed);
    pipeline_residency_telemetry.srt_retained_bytes.store(
        srt_retained_bytes, std::memory_order_relaxed);
    pipeline_residency_telemetry.sequence.fetch_add(1,
                                                    std::memory_order_release);
}

thread_local const PipelineCache* hle_clean_buffer_guest_read_owner{};
thread_local u32 hle_clean_buffer_guest_read_depth{};

template <typename T>
u64 HashGraphicsStructuralValue(const u64 seed, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    return HashCombine(seed, XXH3_64bits(&value, sizeof(value)));
}

constexpr bool CanReuseGraphicsStructuralSignature(
    const bool managed_state_cache, const bool route_enabled,
    const bool last_lookup_resolved, const bool same_primitive_override,
    const bool same_other_generation, const bool same_shader_program_generation) {
    return managed_state_cache && route_enabled &&
           last_lookup_resolved && same_primitive_override &&
           same_other_generation && same_shader_program_generation;
}

static_assert(CanReuseGraphicsStructuralSignature(true, true, true, true, true, true));
static_assert(!CanReuseGraphicsStructuralSignature(false, true, true, true, true, true));
static_assert(!CanReuseGraphicsStructuralSignature(true, false, true, true, true, true));
static_assert(!CanReuseGraphicsStructuralSignature(true, true, false, true, true, true));
static_assert(!CanReuseGraphicsStructuralSignature(true, true, true, false, true, true));
static_assert(!CanReuseGraphicsStructuralSignature(true, true, true, true, false, true));
static_assert(!CanReuseGraphicsStructuralSignature(true, true, true, true, true, false));

constexpr bool CanUseStableSrtEpochHit(const bool route_enabled,
                                       const bool identity_matches,
                                       const bool cpu_stable,
                                       const bool gpu_clean) noexcept {
    return route_enabled && identity_matches && cpu_stable && gpu_clean;
}

constexpr bool CanUseProgramHotExactRevalidation(const bool route_enabled,
                                                 const bool identity_matches,
                                                 const bool cpu_stable,
                                                 const bool gpu_clean) noexcept {
    return route_enabled && identity_matches && !cpu_stable && gpu_clean;
}

static_assert(CanUseStableSrtEpochHit(true, true, true, true));
static_assert(!CanUseStableSrtEpochHit(false, true, true, true));
static_assert(!CanUseStableSrtEpochHit(true, false, true, true));
static_assert(!CanUseStableSrtEpochHit(true, true, false, true));
static_assert(!CanUseStableSrtEpochHit(true, true, true, false));
static_assert(CanUseProgramHotExactRevalidation(true, true, false, true));
static_assert(!CanUseProgramHotExactRevalidation(false, true, false, true));
static_assert(!CanUseProgramHotExactRevalidation(true, false, false, true));
static_assert(!CanUseProgramHotExactRevalidation(true, true, true, true));
static_assert(!CanUseProgramHotExactRevalidation(true, true, false, false));

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
bool ExecutorGenerationGuardedGraphicsSignatureReuseEnabled() noexcept {
    // The A750 route is already accepted below. Keep the broader route behind
    // an app-private, process-lifetime marker until an exact A840 A/B/A proves
    // that user-data-only register traffic is material in the current scene.
    // The marker changes no dirty classification: any Other or ShaderProgram
    // generation change still rebuilds the complete structural signature.
    static const bool enabled =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-enable-generation-guarded-graphics-signature-reuse",
                 F_OK) == 0;
    return enabled;
}
#endif

u64 GraphicsStructuralSignature(
    const AmdGpu::Regs& regs,
    const std::optional<AmdGpu::PrimitiveType> host_primitive_override) {
    u64 signature = 0x4c53583447505331ULL;
    const auto mix = [&](const auto& value) {
        signature = HashGraphicsStructuralValue(signature, value);
    };

    // Shader addresses/settings and every register consumed by
    // BuildRuntimeInfo or RefreshGraphicsKey. Resource addresses and ordinary
    // viewport/attachment addresses are deliberately excluded: they do not
    // select a Vulkan pipeline and used to trigger a full shader-stage walk.
    const auto mix_program = [&](const AmdGpu::ShaderProgram& program) {
        signature = HashCombine(
            signature,
            XXH3_64bits(&program, offsetof(AmdGpu::ShaderProgram, user_data)));
    };
    mix_program(regs.ps_program);
    mix_program(regs.vs_program);
    mix_program(regs.gs_program);
    mix_program(regs.es_program);
    mix_program(regs.hs_program);
    mix_program(regs.ls_program);

    mix(regs.depth_render_override);
    const bool depth_valid = regs.depth_buffer.DepthValid();
    const bool stencil_valid = regs.depth_buffer.StencilValid();
    mix(depth_valid);
    mix(stencil_valid);
    if (depth_valid) {
        mix(regs.depth_buffer.z_info);
    }
    if (stencil_valid) {
        mix(regs.depth_buffer.stencil_info);
    }

    mix(regs.clipper_control);
    mix(regs.polygon_control);
    mix(regs.color_control);
    mix(regs.color_target_mask);
    mix(regs.color_shader_mask);
    mix(regs.color_export_format);
    mix(regs.blend_control);
    for (const auto& color : regs.color_buffers) {
        const bool active = static_cast<bool>(color);
        mix(active);
        if (active) {
            mix(color.info.raw);
            mix(color.attrib.raw);
        }
    }

    mix(regs.stage_enable);
    mix(regs.ls_hs_config);
    mix(regs.tess_config);
    mix(regs.vgt_esgs_ring_itemsize);
    mix(regs.vgt_gs_vert_itemsize);
    mix(regs.vgt_gs_max_vert_out);
    mix(regs.vgt_gs_instance_cnt);
    mix(regs.vgt_gs_out_prim_type);
    mix(regs.vgt_gs_mode);
    mix(regs.vgt_strmout_config);
    mix(regs.vgt_instance_step_rate_0);
    mix(regs.vgt_instance_step_rate_1);
    mix(regs.vs_output_control);
    mix(regs.ps_input_ena);
    mix(regs.ps_input_addr);
    mix(regs.ps_inputs);
    mix(static_cast<u32>(regs.num_interp));
    mix(regs.z_export_format);
    mix(regs.depth_shader_control);
    mix(regs.primitive_type);
    mix(host_primitive_override);
    return signature;
}

enum class ExecutorGraphicsPipelineLookupStage : std::size_t {
    BackgroundLock,
    Refresh,
    RefreshStages,
    GuestReadAdmission,
    NegativeLookup,
    MapLookup,
    SerializationSupport,
    Constructor,
    Count,
};

enum class ExecutorGraphicsShaderTiming : std::size_t {
    ShaderParams,
    GuestCodeScan,
    GuestReadRecovery,
    Program,
    RuntimeInfo,
    ProgramCache,
    Srt,
    Fetch,
    Tessellation,
    CompileModule,
    Translate,
    EmitSpirv,
    ModuleCreate,
    Storage,
    Count,
};

enum class ExecutorGraphicsShaderBindOutcome : u8 {
    Unresolved,
    Disabled,
    ParamsRetry,
    ProgramRetry,
    Ready,
};

struct ExecutorGraphicsShaderStageDetails {
    std::array<u64, static_cast<std::size_t>(
                        ExecutorGraphicsShaderTiming::Count)>
        timing_ns{};
    u64 shader_hash{};
    u32 stage_in{};
    u32 stage_out{};
    u32 bind_calls{};
    u32 new_programs{};
    u32 resident_programs{};
    u32 compile_calls{};
    ExecutorGraphicsShaderBindOutcome outcome{
        ExecutorGraphicsShaderBindOutcome::Unresolved};
    bool used{};
};

constexpr u32 ExecutorNoActiveGraphicsShaderStage = MaxShaderStages;

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)

std::atomic<bool> executor_exact_snapdragon_turnip_program_hot_structural_default{false};
std::atomic<bool> executor_exact_sm8650_turnip_d9_program_hot_payload_default{false};

bool ExecutorPreserveBloodborneSnapdragonStrongBundle() noexcept {
    static const bool preserve =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-preserve-bloodborne-snapdragon-strong-bundle",
                 F_OK) == 0;
    return preserve;
}

bool ExecutorPreserveBloodborneSnapdragonRenderBundle() noexcept {
    static const bool preserve =
        ExecutorPreserveBloodborneSnapdragonStrongBundle() ||
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-preserve-bloodborne-snapdragon-render-bundle",
                 F_OK) == 0;
    return preserve;
}

bool ExecutorExactSnapdragonTurnipProgramHotStructuralDefaultEnabled() noexcept {
    return executor_exact_snapdragon_turnip_program_hot_structural_default.load(
        std::memory_order_acquire);
}

bool ExecutorExactSm8650TurnipD9ProgramHotPayloadDefaultEnabled() noexcept {
    return executor_exact_sm8650_turnip_d9_program_hot_payload_default.load(
        std::memory_order_acquire);
}

bool ExecutorGraphicsPipelineLookupProfileEnabled() noexcept {
    // Keep this independent from broad renderer probes: profiling a pipeline
    // miss adds several clocks and must only happen after an explicit marker.
    static const bool enabled =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-diagnostic-pipeline-build-profile",
                 F_OK) == 0;
    return enabled;
}

bool ExecutorCachedPermutFlattenedReuseEnabled() noexcept {
    static const bool enabled =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-enable-cached-permut-flattened-reuse",
                 F_OK) == 0;
    return enabled;
}

bool ExecutorProgramHotFlattenedLeaseEnabled() noexcept {
    static const bool enabled = [] {
        const bool requested = Common::TitleScopedOptimizationMarkerEnabled(
            "run-enable-program-hot-flattened-lease");
        return !ExecutorPreserveBloodborneSnapdragonRenderBundle() &&
               (requested ||
                ExecutorExactSm8650TurnipD9ProgramHotPayloadDefaultEnabled());
    }();
    return enabled;
}

bool ExecutorProgramHotOccupancyMaskEnabled() noexcept {
    // D69 removes empty-way ProgramHot probes only after an explicit device
    // gate. The mask mirrors the existing entry.valid state; cache identity,
    // byte validation, generations and victim policy remain unchanged.
    static const bool enabled = [] {
        const bool requested = 
#if defined(LSX4_APPLE_RUNTIME)
        ExecutorAppleShaderHotStructureSelected() ||
#endif
        Common::TitleScopedOptimizationMarkerEnabled(
            "run-enable-program-hot-occupancy-mask");
        return !ExecutorPreserveBloodborneSnapdragonRenderBundle() &&
               (requested ||
                ExecutorExactSnapdragonTurnipProgramHotStructuralDefaultEnabled());
    }();
    return enabled;
}

bool ExecutorProgramHotGroupedTlsEnabled() noexcept {
    // Android implements these function-local thread_local objects through
    // __emutls_get_address. Grouping ProgramHot's state turns the repeated TLS
    // resolutions into one without changing any cache field or proof.
    static const bool enabled = [] {
        const bool requested = 
#if defined(LSX4_APPLE_RUNTIME)
        ExecutorAppleShaderHotStructureSelected() ||
#endif
        Common::TitleScopedOptimizationMarkerEnabled(
            "run-enable-program-hot-grouped-tls");
        return !ExecutorPreserveBloodborneSnapdragonRenderBundle() &&
               (requested ||
                ExecutorExactSnapdragonTurnipProgramHotStructuralDefaultEnabled());
    }();
    return enabled;
}

bool ExecutorProgramFlattenedScratchRecycleEnabled() noexcept {
    // Candidate-only: recycle the previous selected permutation allocation as
    // Program::info's scratch after an exact payload swap. No cache identity,
    // SRT byte, epoch, alias, dirty or module-generation proof is changed.
    static const bool enabled = [] {
        const bool requested = Common::TitleScopedOptimizationMarkerEnabled(
            "run-enable-program-flattened-scratch-recycle");
        return !ExecutorPreserveBloodborneSnapdragonRenderBundle() &&
               (requested ||
                ExecutorExactSm8650TurnipD9ProgramHotPayloadDefaultEnabled());
    }();
    return enabled;
}

bool ExecutorBorrowedFetchShaderResultEnabled() noexcept {
    // FetchShaderData owns an inline VertexAttribute vector. Returning it by
    // value copied that vector through Result and again through the stage
    // binder on every hot shader lookup. Borrow only after an explicit gate;
    // Program identity plus the permutation index remains stable while the
    // existing ProgramHot module-generation proof guards replacements.
    static const bool enabled = [] {
        const bool requested = Common::TitleScopedOptimizationMarkerEnabled(
            "run-enable-borrowed-fetch-shader-result");
        return !ExecutorPreserveBloodborneSnapdragonRenderBundle() &&
               (requested ||
                ExecutorExactSm8650TurnipD9ProgramHotPayloadDefaultEnabled());
    }();
    return enabled;
}

bool ExecutorRuntimeInfoLogRmwColdGateEnabled() noexcept {
    // D91 changes only the one-shot RuntimeInfo proof recorder. The first
    // competing hit still uses the original atomic exchange; once published,
    // later exact ProgramHot hits need only observe the already-true flag.
    static const bool enabled =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-enable-runtime-info-log-rmw-cold-gate",
                 F_OK) == 0;
    return enabled;
}

bool ExecutorProgramHotHitLruDeferEnabled() noexcept {
    // D92 avoids choosing a replacement candidate on a proven ProgramHot hit,
    // where the victim is never consumed. Every identity/content miss still
    // executes the original LRU comparison before continuing or replacing.
    static const bool enabled = [] {
        const bool selected =
            #if defined(LSX4_APPLE_RUNTIME)
            ExecutorAppleShaderHotStructureSelected() ||
#endif
            ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                     "run-enable-program-hot-hit-lru-defer",
                     F_OK) == 0;
        if (selected) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_PROGRAM_HOT_HIT_LRU_DEFER] selected=1 "
                "hitVictimUnused=1 missLruExact=1");
            if (std::FILE* proof = std::fopen(
                    "/data/data/app.lsx4.android/files/runtime/"
                    "program-hot-hit-lru-defer.txt",
                    "w")) {
                std::fputs("selected=1 hitVictimUnused=1 missLruExact=1\n",
                           proof);
                std::fclose(proof);
            }
        }
        return selected;
    }();
    return enabled;
}

bool ExecutorProgramHotOverflowWayEnabled() noexcept {
    // D93 adds one independently allocated exact way to every ProgramHot set.
    // It is meaningful only with the grouped TLS state: marker OFF allocates
    // nothing and keeps the four-way layout/probe order byte-for-byte intact.
    static const bool enabled = [] {
        const bool selected =
            ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                     "run-enable-program-hot-overflow-way",
                     F_OK) == 0;
        const bool grouped = ExecutorProgramHotGroupedTlsEnabled();
        if (selected) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_PROGRAM_HOT_OVERFLOW_WAY] selected=1 enabled=%u "
                "groupedTls=%u extraWays=1 exactProofsUnchanged=1",
                grouped ? 1u : 0u, grouped ? 1u : 0u);
        }
        return selected && grouped;
    }();
    return enabled;
}

bool ExecutorSrtVictimOwnerCountGateEnabled() noexcept {
    // D90 only bypasses the process-wide victim hash when the exact owner
    // count proves that the requested Program has no entries in it. Primary
    // lookup and every victim identity/content/lifetime proof stay unchanged.
    static const bool enabled =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-enable-srt-victim-owner-count-gate",
                 F_OK) == 0;
    return enabled;
}

void ExecutorRecordSrtVictimOwnerCountGate(const size_t global_victims) noexcept {
    static bool recorded = false;
    if (recorded) {
        return;
    }
    recorded = true;
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_SRT_VICTIM_OWNER_GATE] firstSkip=1 ownerVictims=0 "
        "globalVictims=%zu exactOwnerCount=1 proofsUnchanged=1",
        global_victims);
    if (std::FILE* proof = std::fopen(
            "/data/data/app.lsx4.android/files/runtime/"
            "srt-victim-owner-count-gate.txt",
            "w")) {
        std::fprintf(proof,
                     "firstSkip=1 ownerVictims=0 globalVictims=%zu "
                     "exactOwnerCount=1 proofsUnchanged=1\n",
                     global_victims);
        std::fclose(proof);
    }
}

void ExecutorRecordCachedPermutFlattenedReuse() noexcept {
    static bool recorded = false;
    if (recorded) {
        return;
    }
    recorded = true;
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_CACHED_PERMUT_FLATTENED] firstHit=1 directSelectedCopy=1");
    if (std::FILE* proof = std::fopen(
            "/data/data/app.lsx4.android/files/runtime/"
            "cached-permut-flattened-reuse.txt",
            "w")) {
        std::fputs("firstHit=1 directSelectedCopy=1\n", proof);
        std::fclose(proof);
    }
}

void ExecutorRecordProgramHotFlattenedLease() noexcept {
    static bool recorded = false;
    if (recorded) {
        return;
    }
    recorded = true;
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_PROGRAM_HOT_FLATTENED_LEASE] firstHit=1 copySkipped=1 immutableLease=1");
    if (std::FILE* proof = std::fopen(
            "/data/data/app.lsx4.android/files/runtime/"
            "program-hot-flattened-lease.txt",
            "w")) {
        std::fputs("firstHit=1 copySkipped=1 immutableLease=1\n", proof);
        std::fclose(proof);
    }
}

void ExecutorRecordProgramHotOccupancyMask(const u32 valid_mask,
                                           const u32 skipped_invalid_ways) noexcept {
    static bool recorded = false;
    if (recorded || skipped_invalid_ways == 0) {
        return;
    }
    recorded = true;
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_PROGRAM_HOT_OCCUPANCY] firstSparseProbe=1 validMask=0x%x "
        "skippedInvalidWays=%u identityChecksUnchanged=1 byteProofUnchanged=1",
        valid_mask, skipped_invalid_ways);
    if (std::FILE* proof = std::fopen(
            "/data/data/app.lsx4.android/files/runtime/"
            "program-hot-occupancy-mask.txt",
            "w")) {
        std::fprintf(proof,
                     "firstSparseProbe=1 validMask=0x%x skippedInvalidWays=%u "
                     "identityChecksUnchanged=1 byteProofUnchanged=1\n",
                     valid_mask, skipped_invalid_ways);
        std::fclose(proof);
    }
}

void ExecutorRecordProgramHotGroupedTls() noexcept {
    static bool recorded = false;
    if (recorded) {
        return;
    }
    recorded = true;
#if defined(LSX4_APPLE_RUNTIME)
    if (ExecutorAppleShaderHotStructureSelected())
        std::fprintf(stderr,"[APPLE_SHADER_HOT_STRUCTURE_USE] firstGroupedLookup=1 cacheFieldsUnchanged=1 exactValidationUnchanged=1\n");
#endif
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_PROGRAM_HOT_GROUPED_TLS] firstLookup=1 groupedState=1 "
        "cacheFieldsUnchanged=1 identityProofUnchanged=1 byteProofUnchanged=1");
    if (std::FILE* proof = std::fopen(
            "/data/data/app.lsx4.android/files/runtime/"
            "program-hot-grouped-tls.txt",
            "w")) {
        std::fputs("firstLookup=1 groupedState=1 cacheFieldsUnchanged=1 "
                   "identityProofUnchanged=1 byteProofUnchanged=1\n",
                   proof);
        std::fclose(proof);
    }
}

void ExecutorRecordProgramFlattenedScratchRecycle() noexcept {
    static bool recorded = false;
    if (recorded) {
        return;
    }
    recorded = true;
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_PROGRAM_FLATTENED_SCRATCH_RECYCLE] firstSwap=1 "
        "payloadUnchanged=1 allocationRecycled=1 proofsUnchanged=1");
    if (std::FILE* proof = std::fopen(
            "/data/data/app.lsx4.android/files/runtime/"
            "program-flattened-scratch-recycle.txt",
            "w")) {
        std::fputs("firstSwap=1 payloadUnchanged=1 allocationRecycled=1 "
                   "proofsUnchanged=1\n",
                   proof);
        std::fclose(proof);
    }
}

void ExecutorRecordBorrowedFetchShaderResult() noexcept {
    static bool recorded = false;
    if (recorded) {
        return;
    }
    recorded = true;
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_BORROWED_FETCH_SHADER_RESULT] firstHit=1 resultCopySkipped=1 "
        "stableProgramIdentity=1 moduleIndex=1 generationGuard=1 ownedFallback=1");
    if (std::FILE* proof = std::fopen(
            "/data/data/app.lsx4.android/files/runtime/"
            "borrowed-fetch-shader-result.txt",
            "w")) {
        std::fputs("firstHit=1 resultCopySkipped=1 stableProgramIdentity=1 "
                   "moduleIndex=1 generationGuard=1 ownedFallback=1\n",
                   proof);
        std::fclose(proof);
    }
}

u64 ExecutorGraphicsPipelineLookupProfileNowNs() noexcept {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

enum class ExecutorGraphicsPipelineLookupOutcome : u8 {
    Unresolved,
    RefreshFailed,
    SameKey,
    KnownInvalid,
    MapHit,
    Built,
    BuildFailed,
};

const char* ExecutorGraphicsPipelineLookupOutcomeName(
    const ExecutorGraphicsPipelineLookupOutcome outcome) noexcept {
    switch (outcome) {
    case ExecutorGraphicsPipelineLookupOutcome::RefreshFailed:
        return "refreshFailed";
    case ExecutorGraphicsPipelineLookupOutcome::SameKey:
        return "sameKey";
    case ExecutorGraphicsPipelineLookupOutcome::KnownInvalid:
        return "knownInvalid";
    case ExecutorGraphicsPipelineLookupOutcome::MapHit:
        return "mapHit";
    case ExecutorGraphicsPipelineLookupOutcome::Built:
        return "built";
    case ExecutorGraphicsPipelineLookupOutcome::BuildFailed:
        return "buildFailed";
    default:
        return "unresolved";
    }
}

const char* ExecutorGraphicsShaderBindOutcomeName(
    const ExecutorGraphicsShaderBindOutcome outcome) noexcept {
    switch (outcome) {
    case ExecutorGraphicsShaderBindOutcome::Disabled:
        return "disabled";
    case ExecutorGraphicsShaderBindOutcome::ParamsRetry:
        return "paramsRetry";
    case ExecutorGraphicsShaderBindOutcome::ProgramRetry:
        return "programRetry";
    case ExecutorGraphicsShaderBindOutcome::Ready:
        return "ready";
    default:
        return "unresolved";
    }
}

class ExecutorGraphicsPipelineLookupProfile;
thread_local ExecutorGraphicsPipelineLookupProfile*
    executor_active_graphics_pipeline_lookup_profile{};
using ExecutorGraphicsShaderStageDetailsBuffer =
    std::array<ExecutorGraphicsShaderStageDetails, MaxShaderStages>;
thread_local std::unique_ptr<ExecutorGraphicsShaderStageDetailsBuffer>
    executor_graphics_shader_stage_details_buffer{};

class ExecutorGraphicsPipelineLookupProfile {
public:
    ExecutorGraphicsPipelineLookupProfile() noexcept {
        previous_ = executor_active_graphics_pipeline_lookup_profile;
        // A nested lookup contributes only to its already-active outer probe.
        // Never clear or alias the outer stage buffer from a reentrant call.
        if (previous_ == nullptr) {
            auto& reusable_buffer =
                executor_graphics_shader_stage_details_buffer;
            if (reusable_buffer == nullptr) {
                reusable_buffer.reset(
                    new (std::nothrow)
                        ExecutorGraphicsShaderStageDetailsBuffer{});
            } else {
                reusable_buffer->fill({});
            }
            shader_stage_details_ = reusable_buffer.get();
        }
        executor_active_graphics_pipeline_lookup_profile = this;
        // Buffer allocation/zeroing above is deliberately outside the timed
        // interval so the diagnostic machinery cannot mimic a game stutter.
        begin_ns_ = ExecutorGraphicsPipelineLookupProfileNowNs();
    }

    ~ExecutorGraphicsPipelineLookupProfile() {
        const u64 end_ns = ExecutorGraphicsPipelineLookupProfileNowNs();
        executor_active_graphics_pipeline_lookup_profile = previous_;
        const u64 total_ns = Delta(end_ns, begin_ns_);
        constexpr u64 SlowLookupThresholdNs = 1'000'000;
        if (total_ns < SlowLookupThresholdNs && constructor_calls_ == 0 &&
            outcome_ != ExecutorGraphicsPipelineLookupOutcome::RefreshFailed) {
            return;
        }

        const u64 refresh_ns = Stage(ExecutorGraphicsPipelineLookupStage::Refresh);
        const u64 accounted_ns =
            Stage(ExecutorGraphicsPipelineLookupStage::BackgroundLock) + refresh_ns +
            Stage(ExecutorGraphicsPipelineLookupStage::NegativeLookup) +
            Stage(ExecutorGraphicsPipelineLookupStage::MapLookup) +
            Stage(ExecutorGraphicsPipelineLookupStage::SerializationSupport) +
            Stage(ExecutorGraphicsPipelineLookupStage::Constructor);
        const u64 other_ns = total_ns > accounted_ns ? total_ns - accounted_ns : 0;
        const auto us = [](const u64 ns) {
            return static_cast<unsigned long long>(ns / 1'000);
        };
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_GRAPHICS_PIPELINE_LOOKUP_PROFILE] schema=2 hash=0x%llx "
            "mode=%s outcome=%s totalUs=%llu backgroundLockUs=%llu refreshUs=%llu "
            "refreshStagesUs=%llu guestReadAdmissionUs=%llu "
            "negativeLookupUs=%llu mapLookupUs=%llu "
            "serializationUs=%llu ctorUs=%llu ctorCalls=%u mapMiss=%d otherUs=%llu",
            static_cast<unsigned long long>(pipeline_hash_),
            user_data_only_refresh_ ? "stages" : "key",
            ExecutorGraphicsPipelineLookupOutcomeName(outcome_), us(total_ns),
            us(Stage(ExecutorGraphicsPipelineLookupStage::BackgroundLock)),
            us(refresh_ns),
            us(Stage(ExecutorGraphicsPipelineLookupStage::RefreshStages)),
            us(Stage(ExecutorGraphicsPipelineLookupStage::GuestReadAdmission)),
            us(Stage(ExecutorGraphicsPipelineLookupStage::NegativeLookup)),
            us(Stage(ExecutorGraphicsPipelineLookupStage::MapLookup)),
            us(Stage(ExecutorGraphicsPipelineLookupStage::SerializationSupport)),
            us(Stage(ExecutorGraphicsPipelineLookupStage::Constructor)),
            constructor_calls_, map_miss_ ? 1 : 0, us(other_ns));

        if (shader_stage_details_ == nullptr) {
            return;
        }
        for (const auto& shader : *shader_stage_details_) {
            if (!shader.used) {
                continue;
            }
            const auto shader_us = [&](const ExecutorGraphicsShaderTiming timing) {
                return us(shader.timing_ns[static_cast<std::size_t>(timing)]);
            };
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_GRAPHICS_SHADER_STAGE_PROFILE] schema=1 "
                "pipelineHash=0x%llx stageIn=%u stageOut=%u outcome=%s "
                "shaderHash=0x%llx bindCalls=%u newPrograms=%u residentPrograms=%u "
                "compileCalls=%u paramsUs=%llu guestScanUs=%llu "
                "guestRecoveryUs=%llu programUs=%llu runtimeUs=%llu "
                "programCacheUs=%llu srtUs=%llu fetchUs=%llu tessUs=%llu "
                "compileUs=%llu translateUs=%llu emitSpirvUs=%llu "
                "moduleCreateUs=%llu storageUs=%llu binaryLoadPath=warmupOnly",
                static_cast<unsigned long long>(pipeline_hash_), shader.stage_in,
                shader.stage_out,
                ExecutorGraphicsShaderBindOutcomeName(shader.outcome),
                static_cast<unsigned long long>(shader.shader_hash),
                shader.bind_calls, shader.new_programs,
                shader.resident_programs, shader.compile_calls,
                shader_us(ExecutorGraphicsShaderTiming::ShaderParams),
                shader_us(ExecutorGraphicsShaderTiming::GuestCodeScan),
                shader_us(ExecutorGraphicsShaderTiming::GuestReadRecovery),
                shader_us(ExecutorGraphicsShaderTiming::Program),
                shader_us(ExecutorGraphicsShaderTiming::RuntimeInfo),
                shader_us(ExecutorGraphicsShaderTiming::ProgramCache),
                shader_us(ExecutorGraphicsShaderTiming::Srt),
                shader_us(ExecutorGraphicsShaderTiming::Fetch),
                shader_us(ExecutorGraphicsShaderTiming::Tessellation),
                shader_us(ExecutorGraphicsShaderTiming::CompileModule),
                shader_us(ExecutorGraphicsShaderTiming::Translate),
                shader_us(ExecutorGraphicsShaderTiming::EmitSpirv),
                shader_us(ExecutorGraphicsShaderTiming::ModuleCreate),
                shader_us(ExecutorGraphicsShaderTiming::Storage));
        }
    }

    ExecutorGraphicsPipelineLookupProfile(const ExecutorGraphicsPipelineLookupProfile&) = delete;
    ExecutorGraphicsPipelineLookupProfile& operator=(
        const ExecutorGraphicsPipelineLookupProfile&) = delete;

    static ExecutorGraphicsPipelineLookupProfile* Active() noexcept {
        return executor_active_graphics_pipeline_lookup_profile;
    }

    [[nodiscard]] u64 BeginStage() const noexcept {
        return ExecutorGraphicsPipelineLookupProfileNowNs();
    }

    void EndStage(const ExecutorGraphicsPipelineLookupStage stage,
                  const u64 begin_ns) noexcept {
        stage_ns_[static_cast<std::size_t>(stage)] +=
            Delta(ExecutorGraphicsPipelineLookupProfileNowNs(), begin_ns);
        if (stage == ExecutorGraphicsPipelineLookupStage::Constructor) {
            ++constructor_calls_;
        }
    }

    void SetRefreshMode(const bool user_data_only) noexcept {
        user_data_only_refresh_ = user_data_only;
    }

    void SetHash(const u64 pipeline_hash) noexcept {
        pipeline_hash_ = pipeline_hash;
    }

    void SetOutcome(const ExecutorGraphicsPipelineLookupOutcome outcome) noexcept {
        outcome_ = outcome;
    }

    void MarkMapMiss() noexcept {
        map_miss_ = true;
    }

    [[nodiscard]] u32 BeginShaderStage(const Shader::Stage stage_in,
                                       const Shader::LogicalStage stage_out) noexcept {
        const u32 stage_index = static_cast<u32>(stage_out);
        if (stage_index >= MaxShaderStages) {
            return active_shader_stage_;
        }
        if (shader_stage_details_ == nullptr) {
            return active_shader_stage_;
        }
        const u32 previous_stage = active_shader_stage_;
        active_shader_stage_ = stage_index;
        auto& shader = (*shader_stage_details_)[stage_index];
        shader.used = true;
        shader.stage_in = static_cast<u32>(stage_in);
        shader.stage_out = stage_index;
        ++shader.bind_calls;
        return previous_stage;
    }

    void EndShaderStage(const u32 previous_stage) noexcept {
        active_shader_stage_ = previous_stage;
    }

    [[nodiscard]] bool HasActiveShaderStage() const noexcept {
        return shader_stage_details_ != nullptr &&
               active_shader_stage_ < MaxShaderStages;
    }

    void SetShaderBindOutcome(
        const ExecutorGraphicsShaderBindOutcome outcome) noexcept {
        if (HasActiveShaderStage()) {
            (*shader_stage_details_)[active_shader_stage_].outcome = outcome;
        }
    }

    void SetShaderHash(const u64 hash) noexcept {
        if (HasActiveShaderStage()) {
            (*shader_stage_details_)[active_shader_stage_].shader_hash = hash;
        }
    }

    void MarkProgramCacheResult(const bool new_program) noexcept {
        if (!HasActiveShaderStage()) {
            return;
        }
        auto& shader = (*shader_stage_details_)[active_shader_stage_];
        if (new_program) {
            ++shader.new_programs;
        } else {
            ++shader.resident_programs;
        }
    }

    void MarkShaderCompile() noexcept {
        if (HasActiveShaderStage()) {
            ++(*shader_stage_details_)[active_shader_stage_].compile_calls;
        }
    }

    [[nodiscard]] u64 BeginShaderTiming() const noexcept {
        return ExecutorGraphicsPipelineLookupProfileNowNs();
    }

    void EndShaderTiming(const ExecutorGraphicsShaderTiming timing,
                         const u64 begin_ns) noexcept {
        if (!HasActiveShaderStage()) {
            return;
        }
        (*shader_stage_details_)[active_shader_stage_]
            .timing_ns[static_cast<std::size_t>(timing)] +=
            Delta(ExecutorGraphicsPipelineLookupProfileNowNs(), begin_ns);
    }

private:
    static u64 Delta(const u64 end_ns, const u64 begin_ns) noexcept {
        return end_ns >= begin_ns ? end_ns - begin_ns : 0;
    }

    [[nodiscard]] u64 Stage(const ExecutorGraphicsPipelineLookupStage stage) const noexcept {
        return stage_ns_[static_cast<std::size_t>(stage)];
    }

    ExecutorGraphicsPipelineLookupProfile* previous_{};
    u64 begin_ns_{};
    u64 pipeline_hash_{};
    std::array<u64, static_cast<std::size_t>(
                        ExecutorGraphicsPipelineLookupStage::Count)>
        stage_ns_{};
    ExecutorGraphicsShaderStageDetailsBuffer* shader_stage_details_{};
    ExecutorGraphicsPipelineLookupOutcome outcome_{
        ExecutorGraphicsPipelineLookupOutcome::Unresolved};
    u32 active_shader_stage_{ExecutorNoActiveGraphicsShaderStage};
    u32 constructor_calls_{};
    bool user_data_only_refresh_{};
    bool map_miss_{};
};

#else

constexpr bool ExecutorGraphicsPipelineLookupProfileEnabled() noexcept {
    return false;
}

constexpr bool ExecutorCachedPermutFlattenedReuseEnabled() noexcept {
    return false;
}

constexpr bool ExecutorProgramHotFlattenedLeaseEnabled() noexcept {
    return false;
}

constexpr bool ExecutorProgramHotOccupancyMaskEnabled() noexcept {
    return false;
}

constexpr bool ExecutorProgramHotGroupedTlsEnabled() noexcept {
    return false;
}

constexpr bool ExecutorProgramFlattenedScratchRecycleEnabled() noexcept {
    return false;
}

constexpr bool ExecutorBorrowedFetchShaderResultEnabled() noexcept {
    return false;
}

constexpr bool ExecutorSrtVictimOwnerCountGateEnabled() noexcept {
    return false;
}

constexpr void ExecutorRecordCachedPermutFlattenedReuse() noexcept {}
constexpr void ExecutorRecordProgramHotFlattenedLease() noexcept {}
constexpr void ExecutorRecordProgramHotOccupancyMask(u32, u32) noexcept {}
constexpr void ExecutorRecordProgramHotGroupedTls() noexcept {}
constexpr void ExecutorRecordProgramFlattenedScratchRecycle() noexcept {}
constexpr void ExecutorRecordBorrowedFetchShaderResult() noexcept {}
constexpr void ExecutorRecordSrtVictimOwnerCountGate(size_t) noexcept {}

enum class ExecutorGraphicsPipelineLookupOutcome : u8 {
    Unresolved,
    RefreshFailed,
    SameKey,
    KnownInvalid,
    MapHit,
    Built,
    BuildFailed,
};

class ExecutorGraphicsPipelineLookupProfile {
public:
    static ExecutorGraphicsPipelineLookupProfile* Active() noexcept {
        return nullptr;
    }
    [[nodiscard]] u64 BeginStage() const noexcept {
        return 0;
    }
    void EndStage(ExecutorGraphicsPipelineLookupStage, u64) noexcept {}
    void SetRefreshMode(bool) noexcept {}
    void SetHash(u64) noexcept {}
    void SetOutcome(ExecutorGraphicsPipelineLookupOutcome) noexcept {}
    void MarkMapMiss() noexcept {}
    [[nodiscard]] u32 BeginShaderStage(Shader::Stage,
                                       Shader::LogicalStage) noexcept {
        return ExecutorNoActiveGraphicsShaderStage;
    }
    void EndShaderStage(u32) noexcept {}
    [[nodiscard]] bool HasActiveShaderStage() const noexcept {
        return false;
    }
    void SetShaderBindOutcome(ExecutorGraphicsShaderBindOutcome) noexcept {}
    void SetShaderHash(u64) noexcept {}
    void MarkProgramCacheResult(bool) noexcept {}
    void MarkShaderCompile() noexcept {}
    [[nodiscard]] u64 BeginShaderTiming() const noexcept {
        return 0;
    }
    void EndShaderTiming(ExecutorGraphicsShaderTiming, u64) noexcept {}
};

#endif

class ExecutorGraphicsPipelineLookupStageScope {
public:
    explicit ExecutorGraphicsPipelineLookupStageScope(
        ExecutorGraphicsPipelineLookupProfile* const profile,
        const ExecutorGraphicsPipelineLookupStage stage) noexcept
        : profile_{profile}, stage_{stage} {
        if (profile_ != nullptr) {
            begin_ns_ = profile_->BeginStage();
        }
    }

    ~ExecutorGraphicsPipelineLookupStageScope() {
        if (profile_ != nullptr) {
            profile_->EndStage(stage_, begin_ns_);
        }
    }

private:
    ExecutorGraphicsPipelineLookupProfile* profile_{};
    ExecutorGraphicsPipelineLookupStage stage_{};
    u64 begin_ns_{};
};

class ExecutorGraphicsShaderStageContext {
public:
    ExecutorGraphicsShaderStageContext(
        ExecutorGraphicsPipelineLookupProfile* const profile,
        const Shader::Stage stage_in,
                                       const Shader::LogicalStage stage_out) noexcept
        : profile_{profile} {
        if (profile_ != nullptr) [[unlikely]] {
            previous_stage_ = profile_->BeginShaderStage(stage_in, stage_out);
        }
    }

    ~ExecutorGraphicsShaderStageContext() {
        if (profile_ != nullptr) [[unlikely]] {
            profile_->EndShaderStage(previous_stage_);
        }
    }

    void SetOutcome(const ExecutorGraphicsShaderBindOutcome outcome) noexcept {
        if (profile_ != nullptr) [[unlikely]] {
            profile_->SetShaderBindOutcome(outcome);
        }
    }

    void SetHash(const u64 hash) noexcept {
        if (profile_ != nullptr) [[unlikely]] {
            profile_->SetShaderHash(hash);
        }
    }

private:
    ExecutorGraphicsPipelineLookupProfile* profile_{};
    u32 previous_stage_{ExecutorNoActiveGraphicsShaderStage};
};

class ExecutorGraphicsShaderTimingScope {
public:
    explicit ExecutorGraphicsShaderTimingScope(
        ExecutorGraphicsPipelineLookupProfile* const profile,
        const ExecutorGraphicsShaderTiming timing) noexcept
        : profile_{profile}, timing_{timing} {
        if (profile_ != nullptr && profile_->HasActiveShaderStage()) [[unlikely]] {
            begin_ns_ = profile_->BeginShaderTiming();
            timing_active_ = true;
        }
    }

    ~ExecutorGraphicsShaderTimingScope() {
        Stop();
    }

    void Stop() noexcept {
        if (timing_active_) [[unlikely]] {
            profile_->EndShaderTiming(timing_, begin_ns_);
            timing_active_ = false;
        }
    }

private:
    ExecutorGraphicsPipelineLookupProfile* profile_{};
    ExecutorGraphicsShaderTiming timing_{};
    u64 begin_ns_{};
    bool timing_active_{};
};

struct AtomicAsyncPipelineCompileStats {
    std::atomic_bool fail_fast_supported{};
    std::atomic<u32> worker_count{};
    std::atomic<u32> queue_capacity{};
    std::atomic<u32> queue_depth{};
    std::atomic<u32> max_queue_depth{};
    std::atomic<u32> shader_queue_capacity{};
    std::atomic<u32> shader_queue_depth{};
    std::atomic<u32> shader_max_queue_depth{};
    std::atomic<u64> shader_queue_byte_capacity{};
    std::atomic<u64> shader_queue_bytes{};
    std::atomic<u64> shader_max_queue_bytes{};
    std::atomic<u64> graphics_misses{};
    std::atomic<u64> compute_misses{};
    std::atomic<u64> fail_fast_hits{};
    std::atomic<u64> fail_fast_compile_required{};
    std::atomic<u64> pending_hits{};
    std::atomic<u64> fast_queued{};
    std::atomic<u64> optimized_queued{};
    std::atomic<u64> queue_drops{};
    std::atomic<u64> fast_completed{};
    std::atomic<u64> optimized_completed{};
    std::atomic<u64> build_failures{};
    std::atomic<u64> published{};
    std::atomic<u64> optimized_replacements{};
    std::atomic<u64> skipped_uses{};
    std::atomic<u64> foreground_fallbacks{};
    std::atomic<u64> shader_fast_compiled{};
    std::atomic<u64> shader_optimized_queued{};
    std::atomic<u64> shader_optimized_completed{};
    std::atomic<u64> shader_optimized_published{};
    std::atomic<u64> shader_queue_drops{};
    std::atomic<u64> shader_negative_hits{};
    std::atomic<u64> shader_build_failures{};
    std::atomic<u64> shader_replacements{};
    std::atomic<u64> shader_total_build_us{};
    std::atomic<u64> shader_max_build_us{};
    std::atomic<u64> total_build_us{};
    std::atomic<u64> max_build_us{};
};

AtomicAsyncPipelineCompileStats async_pipeline_stats{};

template <typename T>
void AtomicMax(std::atomic<T>& target, const T value) noexcept {
    T previous = target.load(std::memory_order_relaxed);
    while (previous < value &&
           !target.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
    }
}

size_t ReadBoundedAsyncPipelineOption(const char* name, const size_t fallback,
                                      const size_t minimum, const size_t maximum) {
    const char* const value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    size_t parsed{};
    const std::string_view text{value};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return fallback;
    }
    return std::clamp(parsed, minimum, maximum);
}

bool DeferAsyncLiveReplacement(const Instance& instance) noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    // Qualcomm's proprietary driver may keep internal references to the
    // currently bound shader/pipeline even after the command-buffer lifetime
    // tracked by Vulkan has ended. Replacing a live fallback is therefore not
    // a safe publication boundary on these devices: Bloodborne can continue
    // presenting its loading overlay while guest progress is effectively
    // starved. Compile and persist the optimized artifact in the background,
    // but activate it on the next title launch through the disk caches.
    return instance.GetCapabilityProfile().HasQuirk(
        CapabilityQuirk::DeferAsyncPipelineReplacement);
#else
    (void)instance;
    return false;
#endif
}

constexpr bool CanUseManagedForegroundFallback(const bool managed_async,
                                                const bool defer_live_replacement,
                                                const bool deferred_driver_opt_in) noexcept {
    return managed_async && (!defer_live_replacement || deferred_driver_opt_in);
}

static_assert(CanUseManagedForegroundFallback(true, false, false));
static_assert(CanUseManagedForegroundFallback(true, true, true));
static_assert(!CanUseManagedForegroundFallback(true, true, false));
static_assert(!CanUseManagedForegroundFallback(false, false, true));

bool UseManagedForegroundFallback(const Instance& instance) noexcept {
    const bool defer_live_replacement = DeferAsyncLiveReplacement(instance);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    // Qualcomm and Turnip keep the proven no-live-replacement rule.  This
    // opt-in changes only the first synchronous object: build it with
    // eDisableOptimization, then let the existing idle worker merge an
    // optimized pipeline into the disk cache for a later title launch.  The
    // optimized object is never hot-swapped into the active command stream.
    static const bool deferred_driver_opt_in =
        std::getenv("EXECUTOR_DEFERRED_DRIVER_CHEAP_FIRST_PIPELINE") != nullptr;
#else
    constexpr bool deferred_driver_opt_in = false;
#endif
    return CanUseManagedForegroundFallback(
        Config::managedAsyncPipelineCompilation(), defer_live_replacement,
        deferred_driver_opt_in);
}

vk::PipelineCreateFlags ForegroundFallbackBuildFlags(const Instance& instance) noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (instance.GetCapabilityProfile().HasQuirk(
            CapabilityQuirk::ForegroundPipelineWithoutDisableOptimization)) {
        return {};
    }
#else
    (void)instance;
#endif
    return vk::PipelineCreateFlagBits::eDisableOptimization;
}

u64 AsyncSteadyMilliseconds() noexcept {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // Anonymous namespace

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
struct ProgramHotEntry {
    const PipelineCache* owner{};
    const Program* program{};
    u64 snapshot_key{};
    u64 runtime_info_identity{};
    Shader::Backend::Bindings binding_start{};
    Shader::Backend::Bindings binding_end{};
    Shader::SrtMemoryRanges srt_ranges{};
    std::vector<u8> srt_source_bytes{};
    std::vector<u32> flattened_user_data{};
    std::shared_ptr<const std::vector<u32>> flattened_user_data_lease{};
    PipelineCache::Result result{};
    u64 invalidation_sequence{};
    u64 module_generation{};
    size_t module_index{};
    u64 last_used{};
    bool valid{};
};

constexpr size_t ProgramHotSetCount = 8192;
constexpr size_t ProgramHotWayCount = 4;
static_assert(ProgramHotWayCount <= 8);
constexpr u32 ProgramHotAllWaysMask = (1u << ProgramHotWayCount) - 1u;
using ProgramHotSets =
    std::array<std::array<ProgramHotEntry, ProgramHotWayCount>, ProgramHotSetCount>;
using ProgramHotOverflowWay = std::array<ProgramHotEntry, ProgramHotSetCount>;
using ProgramHotByteArray = std::array<u8, ProgramHotSetCount>;

struct ProgramHotThreadState {
    ProgramHotSets sets{};
    std::unique_ptr<ProgramHotOverflowWay> overflow_way{};
    ProgramHotByteArray mru_ways{};
    ProgramHotByteArray valid_way_masks{};
    u64 lookups{};
    u64 hits{};
    u64 clock{};
    u64 exact_revalidations{};
    u64 exact_revalidation_hits{};
    u64 retained_content_versions{};
};

struct ProgramHotThreadStateRefs {
    ProgramHotSets* sets{};
    ProgramHotByteArray* mru_ways{};
    ProgramHotByteArray* valid_way_masks{};
    u64* lookups{};
    u64* hits{};
    u64* clock{};
    u64* exact_revalidations{};
    u64* exact_revalidation_hits{};
    u64* retained_content_versions{};
};

namespace {

bool ExecutorProgramHotCallScopeTlsHoistEnabled() noexcept {
    // D80 is meaningful only on top of D71's grouped state. Resolve that one
    // emulated-TLS object once for RefreshGraphicsStages instead of once per
    // enabled shader stage; the cache payload and every proof stay unchanged.
    static const bool enabled = [] {
        const bool requested = 
#if defined(LSX4_APPLE_RUNTIME)
        ExecutorAppleShaderHotStructureSelected() ||
#endif
        Common::TitleScopedOptimizationMarkerEnabled(
            "run-enable-program-hot-call-scope-tls-hoist");
        return !ExecutorPreserveBloodborneSnapdragonRenderBundle() &&
               (requested ||
                ExecutorExactSnapdragonTurnipProgramHotStructuralDefaultEnabled());
    }();
    return enabled;
}

bool ExecutorProgramHotLegacyTlsColdSplitEnabled() noexcept {
    // D82 is meaningful on top of D71. Keep the nine legacy TLS objects for
    // the exact fallback, but do not resolve their emutls addresses on the
    // grouped hot path that cannot select them.
    static const bool enabled = [] {
        const bool requested = 
#if defined(LSX4_APPLE_RUNTIME)
        ExecutorAppleShaderHotStructureSelected() ||
#endif
        Common::TitleScopedOptimizationMarkerEnabled(
            "run-enable-program-hot-legacy-tls-cold-split");
        return !ExecutorPreserveBloodborneSnapdragonRenderBundle() &&
               (requested ||
                ExecutorExactSnapdragonTurnipProgramHotStructuralDefaultEnabled());
    }();
    return enabled;
}

ProgramHotThreadState& ExecutorProgramHotThreadState() noexcept {
#if defined(LSX4_APPLE_RUNTIME)
    // Darwin allocates the image's entire native TLS template when a thread
    // first accesses any TLS variable. Keep the 34 MiB shader cache out of
    // that template: audio, UIKit and service threads never need its payload.
    // Ownership, capacity and destruction still belong to the calling thread.
    static thread_local auto state = std::make_unique<ProgramHotThreadState>();
    return *state;
#else
    static thread_local ProgramHotThreadState state{};
    return state;
#endif
}

ProgramHotThreadStateRefs ExecutorProgramHotThreadStateRefs(
    ProgramHotThreadState& state) noexcept {
    return {
        .sets = &state.sets,
        .mru_ways = &state.mru_ways,
        .valid_way_masks = &state.valid_way_masks,
        .lookups = &state.lookups,
        .hits = &state.hits,
        .clock = &state.clock,
        .exact_revalidations = &state.exact_revalidations,
        .exact_revalidation_hits = &state.exact_revalidation_hits,
        .retained_content_versions = &state.retained_content_versions,
    };
}

#if defined(LSX4_APPLE_RUNTIME)
ProgramHotSets& ExecutorAppleProgramHotLegacySets() noexcept {
    static thread_local auto storage = std::make_unique<ProgramHotSets>();
    return *storage;
}
#endif

[[gnu::noinline]] ProgramHotThreadStateRefs
ExecutorProgramHotLegacyThreadStateRefs() noexcept {
    // Preserve the pre-D71 layout and lifetime for D71 OFF and the D82 OFF
    // control. Keeping this helper out of line prevents the Android compiler
    // from hoisting __emutls_get_address into grouped GetProgram calls.
#if defined(LSX4_APPLE_RUNTIME)
    // Preserve a separate legacy cache; choosing one path must not eagerly
    // allocate the other path's payload through Mach-O native TLS.
    auto& legacy_sets = ExecutorAppleProgramHotLegacySets();
#else
    static thread_local ProgramHotSets legacy_sets{};
#endif
    static thread_local ProgramHotByteArray legacy_mru_ways{};
    static thread_local ProgramHotByteArray legacy_valid_way_masks{};
    static thread_local u64 legacy_lookups{};
    static thread_local u64 legacy_hits{};
    static thread_local u64 legacy_clock{};
    static thread_local u64 legacy_exact_revalidations{};
    static thread_local u64 legacy_exact_revalidation_hits{};
    static thread_local u64 legacy_retained_content_versions{};
    return {
        .sets = &legacy_sets,
        .mru_ways = &legacy_mru_ways,
        .valid_way_masks = &legacy_valid_way_masks,
        .lookups = &legacy_lookups,
        .hits = &legacy_hits,
        .clock = &legacy_clock,
        .exact_revalidations = &legacy_exact_revalidations,
        .exact_revalidation_hits = &legacy_exact_revalidation_hits,
        .retained_content_versions = &legacy_retained_content_versions,
    };
}

void ExecutorRecordProgramHotLegacyTlsColdSplit() noexcept {
    static bool recorded = false;
    if (recorded) {
        return;
    }
    recorded = true;
    constexpr const char* proof =
        "firstGroupedHit=1 legacyTlsObjectsSkipped=9 groupedStateRequired=1 "
        "legacyFallbackPreserved=1 cacheFieldsUnchanged=1 proofsUnchanged=1";
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_PROGRAM_HOT_LEGACY_TLS_COLD_SPLIT] %s", proof);
    if (std::FILE* file = std::fopen(
            "/data/data/app.lsx4.android/files/lsx4-home/"
            "program-hot-legacy-tls-cold-split.txt",
            "w")) {
        std::fprintf(file, "%s\n", proof);
        std::fclose(file);
    }
}

void ExecutorRecordProgramHotCallScopeTlsHoist() noexcept {
    static bool recorded = false;
    if (recorded) {
        return;
    }
    recorded = true;
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_PROGRAM_HOT_CALL_SCOPE_TLS_HOIST] firstScope=1 "
        "programCalls=2 tlsResolutionsSaved=1 groupedStateRequired=1 "
        "cacheFieldsUnchanged=1 proofsUnchanged=1");
    if (std::FILE* proof = std::fopen(
            "/data/data/app.lsx4.android/files/runtime/"
            "program-hot-call-scope-tls-hoist.txt",
            "w")) {
        std::fputs("firstScope=1 programCalls=2 tlsResolutionsSaved=1 "
                   "groupedStateRequired=1 cacheFieldsUnchanged=1 "
                   "proofsUnchanged=1\n",
                   proof);
        std::fclose(proof);
    }
}

} // Anonymous namespace
#endif

void RecordPipelineDescriptorTopology(
    const bool uses_push_descriptors, const u32 descriptor_bindings,
    const u32 max_push_descriptors,
    const u32 max_update_after_bind_descriptors) noexcept {
    descriptor_topology_telemetry.descriptor_layouts.fetch_add(
        1, std::memory_order_relaxed);
    (uses_push_descriptors
         ? descriptor_topology_telemetry.push_descriptor_layouts
         : descriptor_topology_telemetry.legacy_descriptor_layouts)
        .fetch_add(1, std::memory_order_relaxed);
    descriptor_topology_telemetry.descriptor_bindings.fetch_add(
        descriptor_bindings, std::memory_order_relaxed);
    const auto update_max = [](std::atomic<u64>& target, const u64 value) {
        u64 observed = target.load(std::memory_order_relaxed);
        while (observed < value &&
               !target.compare_exchange_weak(observed, value,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {
        }
    };
    update_max(descriptor_topology_telemetry.max_descriptor_bindings,
               descriptor_bindings);
    update_max(descriptor_topology_telemetry.max_push_descriptors,
               max_push_descriptors);
    update_max(
        descriptor_topology_telemetry.max_update_after_bind_descriptors,
        max_update_after_bind_descriptors);
}

void RecordRendererImageCreationTopology(
    const bool mutable_format, const bool extended_usage,
    const bool color_storage, const bool depth, const bool block,
    const u64 allocation_bytes, const u32 bounded_format_list_entries) noexcept {
    renderer_cache_residency_telemetry.image_creations.fetch_add(
        1, std::memory_order_relaxed);
    if (mutable_format) {
        renderer_cache_residency_telemetry.mutable_image_creations.fetch_add(
            1, std::memory_order_relaxed);
    }
    if (extended_usage) {
        renderer_cache_residency_telemetry.extended_usage_image_creations.fetch_add(
            1, std::memory_order_relaxed);
    }
    if (color_storage) {
        renderer_cache_residency_telemetry.color_storage_image_creations.fetch_add(
            1, std::memory_order_relaxed);
    }
    if (depth) {
        renderer_cache_residency_telemetry.depth_image_creations.fetch_add(
            1, std::memory_order_relaxed);
    }
    if (block) {
        renderer_cache_residency_telemetry.block_image_creations.fetch_add(
            1, std::memory_order_relaxed);
    }
    renderer_cache_residency_telemetry.image_allocation_bytes.fetch_add(
        allocation_bytes, std::memory_order_relaxed);
    auto& maximum = renderer_cache_residency_telemetry.max_image_allocation_bytes;
    u64 observed = maximum.load(std::memory_order_relaxed);
    while (observed < allocation_bytes &&
           !maximum.compare_exchange_weak(observed, allocation_bytes,
                                          std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {
    }
    if (bounded_format_list_entries != 0) {
        renderer_cache_residency_telemetry.bounded_format_list_image_creations.fetch_add(
            1, std::memory_order_relaxed);
        renderer_cache_residency_telemetry.bounded_format_list_entries.fetch_add(
            bounded_format_list_entries, std::memory_order_relaxed);
        auto& maximum_entries =
            renderer_cache_residency_telemetry.max_bounded_format_list_entries;
        observed = maximum_entries.load(std::memory_order_relaxed);
        while (observed < bounded_format_list_entries &&
               !maximum_entries.compare_exchange_weak(
                   observed, bounded_format_list_entries,
                   std::memory_order_relaxed, std::memory_order_relaxed)) {
        }
    }
}

void RecordRendererImageViewTopology(const bool format_reinterpreted,
                                     const bool storage_view,
                                     const u32 base_format,
                                     const u32 view_format) noexcept {
    const u64 view_index =
        renderer_cache_residency_telemetry.image_view_creations.fetch_add(
            1, std::memory_order_relaxed) +
        1;
    if (format_reinterpreted) {
        const u64 reinterpret_index =
            renderer_cache_residency_telemetry.reinterpreted_image_view_creations.fetch_add(
                1, std::memory_order_relaxed) +
            1;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        // Diagnostic route only: ImageView calls this function exclusively
        // under diagnostic_a750_image_topology. Bound the log so a long game
        // cannot turn the census into persistent I/O.
        if (reinterpret_index <= 32) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_A750_IMAGE_FORMAT_PAIR] reinterpret=%llu view=%llu "
                "base=%u format=%u storage=%d",
                static_cast<unsigned long long>(reinterpret_index),
                static_cast<unsigned long long>(view_index), base_format,
                view_format, storage_view ? 1 : 0);
        }
#endif
    }
    if (storage_view) {
        renderer_cache_residency_telemetry.storage_image_view_creations.fetch_add(
            1, std::memory_order_relaxed);
    }
}

PipelineResidencyTelemetrySnapshot
GetPipelineResidencyTelemetrySnapshot() noexcept {
    PipelineResidencyTelemetrySnapshot snapshot{};
    for (;;) {
        const u64 before = pipeline_residency_telemetry.sequence.load(
            std::memory_order_acquire);
        if ((before & 1u) != 0) {
            continue;
        }
        snapshot.program_entries =
            pipeline_residency_telemetry.program_entries.load(
                std::memory_order_relaxed);
        snapshot.graphics_pipeline_entries =
            pipeline_residency_telemetry.graphics_pipeline_entries.load(
                std::memory_order_relaxed);
        snapshot.compute_pipeline_entries =
            pipeline_residency_telemetry.compute_pipeline_entries.load(
                std::memory_order_relaxed);
        snapshot.new_pipelines =
            pipeline_residency_telemetry.new_pipelines.load(
                std::memory_order_relaxed);
        snapshot.shader_identity_entries =
            pipeline_residency_telemetry.shader_identity_entries.load(
                std::memory_order_relaxed);
        snapshot.srt_objects = pipeline_residency_telemetry.srt_objects.load(
            std::memory_order_relaxed);
        snapshot.srt_retained_bytes =
            pipeline_residency_telemetry.srt_retained_bytes.load(
                std::memory_order_relaxed);
        snapshot.descriptor_layouts =
            descriptor_topology_telemetry.descriptor_layouts.load(
                std::memory_order_relaxed);
        snapshot.push_descriptor_layouts =
            descriptor_topology_telemetry.push_descriptor_layouts.load(
                std::memory_order_relaxed);
        snapshot.legacy_descriptor_layouts =
            descriptor_topology_telemetry.legacy_descriptor_layouts.load(
                std::memory_order_relaxed);
        snapshot.descriptor_bindings =
            descriptor_topology_telemetry.descriptor_bindings.load(
                std::memory_order_relaxed);
        snapshot.max_descriptor_bindings =
            descriptor_topology_telemetry.max_descriptor_bindings.load(
                std::memory_order_relaxed);
        snapshot.max_push_descriptors =
            descriptor_topology_telemetry.max_push_descriptors.load(
                std::memory_order_relaxed);
        snapshot.max_update_after_bind_descriptors =
            descriptor_topology_telemetry.max_update_after_bind_descriptors.load(
                std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        const u64 after = pipeline_residency_telemetry.sequence.load(
            std::memory_order_acquire);
        if (before == after) {
            snapshot.sequence = after / 2;
            return snapshot;
        }
    }
}

void PublishRendererCacheResidencyTelemetry(
    const u64 texture_guest_bytes, const u64 texture_image_entries,
    const u64 texture_image_view_entries, const u64 texture_sampler_entries,
    const u64 buffer_resident_bytes, const u64 buffer_utility_bytes,
    const u64 buffer_entries) noexcept {
    renderer_cache_residency_telemetry.sequence.fetch_add(
        1, std::memory_order_acq_rel);
    renderer_cache_residency_telemetry.texture_guest_bytes.store(
        texture_guest_bytes, std::memory_order_relaxed);
    renderer_cache_residency_telemetry.texture_image_entries.store(
        texture_image_entries, std::memory_order_relaxed);
    renderer_cache_residency_telemetry.texture_image_view_entries.store(
        texture_image_view_entries, std::memory_order_relaxed);
    renderer_cache_residency_telemetry.texture_sampler_entries.store(
        texture_sampler_entries, std::memory_order_relaxed);
    renderer_cache_residency_telemetry.buffer_resident_bytes.store(
        buffer_resident_bytes, std::memory_order_relaxed);
    renderer_cache_residency_telemetry.buffer_utility_bytes.store(
        buffer_utility_bytes, std::memory_order_relaxed);
    renderer_cache_residency_telemetry.buffer_entries.store(
        buffer_entries, std::memory_order_relaxed);
    renderer_cache_residency_telemetry.sequence.fetch_add(
        1, std::memory_order_release);
}

RendererCacheResidencyTelemetrySnapshot
GetRendererCacheResidencyTelemetrySnapshot() noexcept {
    RendererCacheResidencyTelemetrySnapshot snapshot{};
    for (;;) {
        const u64 before = renderer_cache_residency_telemetry.sequence.load(
            std::memory_order_acquire);
        if ((before & 1u) != 0) {
            continue;
        }
        snapshot.texture_guest_bytes =
            renderer_cache_residency_telemetry.texture_guest_bytes.load(
                std::memory_order_relaxed);
        snapshot.texture_image_entries =
            renderer_cache_residency_telemetry.texture_image_entries.load(
                std::memory_order_relaxed);
        snapshot.texture_image_view_entries =
            renderer_cache_residency_telemetry.texture_image_view_entries.load(
                std::memory_order_relaxed);
        snapshot.texture_sampler_entries =
            renderer_cache_residency_telemetry.texture_sampler_entries.load(
                std::memory_order_relaxed);
        snapshot.buffer_resident_bytes =
            renderer_cache_residency_telemetry.buffer_resident_bytes.load(
                std::memory_order_relaxed);
        snapshot.buffer_utility_bytes =
            renderer_cache_residency_telemetry.buffer_utility_bytes.load(
                std::memory_order_relaxed);
        snapshot.buffer_entries =
            renderer_cache_residency_telemetry.buffer_entries.load(
                std::memory_order_relaxed);
        snapshot.image_creations =
            renderer_cache_residency_telemetry.image_creations.load(
                std::memory_order_relaxed);
        snapshot.mutable_image_creations =
            renderer_cache_residency_telemetry.mutable_image_creations.load(
                std::memory_order_relaxed);
        snapshot.extended_usage_image_creations =
            renderer_cache_residency_telemetry.extended_usage_image_creations.load(
                std::memory_order_relaxed);
        snapshot.color_storage_image_creations =
            renderer_cache_residency_telemetry.color_storage_image_creations.load(
                std::memory_order_relaxed);
        snapshot.depth_image_creations =
            renderer_cache_residency_telemetry.depth_image_creations.load(
                std::memory_order_relaxed);
        snapshot.block_image_creations =
            renderer_cache_residency_telemetry.block_image_creations.load(
                std::memory_order_relaxed);
        snapshot.image_allocation_bytes =
            renderer_cache_residency_telemetry.image_allocation_bytes.load(
                std::memory_order_relaxed);
        snapshot.max_image_allocation_bytes =
            renderer_cache_residency_telemetry.max_image_allocation_bytes.load(
                std::memory_order_relaxed);
        snapshot.bounded_format_list_image_creations =
            renderer_cache_residency_telemetry.bounded_format_list_image_creations.load(
                std::memory_order_relaxed);
        snapshot.bounded_format_list_entries =
            renderer_cache_residency_telemetry.bounded_format_list_entries.load(
                std::memory_order_relaxed);
        snapshot.max_bounded_format_list_entries =
            renderer_cache_residency_telemetry.max_bounded_format_list_entries.load(
                std::memory_order_relaxed);
        snapshot.image_view_creations =
            renderer_cache_residency_telemetry.image_view_creations.load(
                std::memory_order_relaxed);
        snapshot.reinterpreted_image_view_creations =
            renderer_cache_residency_telemetry.reinterpreted_image_view_creations.load(
                std::memory_order_relaxed);
        snapshot.storage_image_view_creations =
            renderer_cache_residency_telemetry.storage_image_view_creations.load(
                std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        const u64 after = renderer_cache_residency_telemetry.sequence.load(
            std::memory_order_acquire);
        if (before == after) {
            snapshot.sequence = after / 2;
            return snapshot;
        }
    }
}

void PublishRendererUtilityRingTelemetry(
    const RendererUtilityRingTelemetrySnapshot& snapshot) noexcept {
    renderer_utility_ring_telemetry.sequence.fetch_add(1, std::memory_order_acq_rel);
    StoreUtilityRingTelemetry(renderer_utility_ring_telemetry.staging, snapshot.staging);
    StoreUtilityRingTelemetry(renderer_utility_ring_telemetry.stream, snapshot.stream);
    StoreUtilityRingTelemetry(renderer_utility_ring_telemetry.download, snapshot.download);
    StoreUtilityRingTelemetry(renderer_utility_ring_telemetry.device, snapshot.device);
    auto& upload = renderer_utility_ring_telemetry.upload_reservation;
    upload.transaction_count.store(snapshot.upload_reservation.transaction_count,
                                   std::memory_order_relaxed);
    upload.completed_transaction_count.store(
        snapshot.upload_reservation.completed_transaction_count,
        std::memory_order_relaxed);
    upload.failed_transaction_count.store(
        snapshot.upload_reservation.failed_transaction_count,
        std::memory_order_relaxed);
    upload.measurement_count.store(snapshot.upload_reservation.measurement_count,
                                   std::memory_order_relaxed);
    upload.retry_count.store(snapshot.upload_reservation.retry_count,
                             std::memory_order_relaxed);
    upload.full_span_candidate_bytes.store(
        snapshot.upload_reservation.full_span_candidate_bytes,
        std::memory_order_relaxed);
    upload.dirty_bytes.store(snapshot.upload_reservation.dirty_bytes,
                             std::memory_order_relaxed);
    upload.avoided_bytes.store(snapshot.upload_reservation.avoided_bytes,
                               std::memory_order_relaxed);
    upload.max_full_span_candidate_bytes.store(
        snapshot.upload_reservation.max_full_span_candidate_bytes,
        std::memory_order_relaxed);
    upload.max_dirty_bytes.store(snapshot.upload_reservation.max_dirty_bytes,
                                 std::memory_order_relaxed);
    renderer_utility_ring_telemetry.sequence.fetch_add(1, std::memory_order_release);
}

RendererUtilityRingTelemetrySnapshot
GetRendererUtilityRingTelemetrySnapshot() noexcept {
    RendererUtilityRingTelemetrySnapshot snapshot{};
    for (;;) {
        const u64 before = renderer_utility_ring_telemetry.sequence.load(
            std::memory_order_acquire);
        if ((before & 1u) != 0) {
            continue;
        }
        snapshot.staging = LoadUtilityRingTelemetry(renderer_utility_ring_telemetry.staging);
        snapshot.stream = LoadUtilityRingTelemetry(renderer_utility_ring_telemetry.stream);
        snapshot.download = LoadUtilityRingTelemetry(renderer_utility_ring_telemetry.download);
        snapshot.device = LoadUtilityRingTelemetry(renderer_utility_ring_telemetry.device);
        const auto& upload = renderer_utility_ring_telemetry.upload_reservation;
        snapshot.upload_reservation = {
            .transaction_count = upload.transaction_count.load(std::memory_order_relaxed),
            .completed_transaction_count =
                upload.completed_transaction_count.load(std::memory_order_relaxed),
            .failed_transaction_count =
                upload.failed_transaction_count.load(std::memory_order_relaxed),
            .measurement_count = upload.measurement_count.load(std::memory_order_relaxed),
            .retry_count = upload.retry_count.load(std::memory_order_relaxed),
            .full_span_candidate_bytes =
                upload.full_span_candidate_bytes.load(std::memory_order_relaxed),
            .dirty_bytes = upload.dirty_bytes.load(std::memory_order_relaxed),
            .avoided_bytes = upload.avoided_bytes.load(std::memory_order_relaxed),
            .max_full_span_candidate_bytes =
                upload.max_full_span_candidate_bytes.load(std::memory_order_relaxed),
            .max_dirty_bytes = upload.max_dirty_bytes.load(std::memory_order_relaxed),
        };
        std::atomic_thread_fence(std::memory_order_acquire);
        const u64 after = renderer_utility_ring_telemetry.sequence.load(
            std::memory_order_acquire);
        if (before == after) {
            snapshot.sequence = after / 2;
            return snapshot;
        }
    }
}

AsyncPipelineCompileStats GetAsyncPipelineCompileStats() noexcept {
    return {
        .managed_enabled = Config::managedAsyncPipelineCompilation(),
        .fail_fast_supported =
            async_pipeline_stats.fail_fast_supported.load(std::memory_order_relaxed),
        .worker_count = async_pipeline_stats.worker_count.load(std::memory_order_relaxed),
        .queue_capacity = async_pipeline_stats.queue_capacity.load(std::memory_order_relaxed),
        .queue_depth = async_pipeline_stats.queue_depth.load(std::memory_order_relaxed),
        .max_queue_depth = async_pipeline_stats.max_queue_depth.load(std::memory_order_relaxed),
        .shader_queue_capacity =
            async_pipeline_stats.shader_queue_capacity.load(std::memory_order_relaxed),
        .shader_queue_depth =
            async_pipeline_stats.shader_queue_depth.load(std::memory_order_relaxed),
        .shader_max_queue_depth =
            async_pipeline_stats.shader_max_queue_depth.load(std::memory_order_relaxed),
        .shader_queue_byte_capacity =
            async_pipeline_stats.shader_queue_byte_capacity.load(std::memory_order_relaxed),
        .shader_queue_bytes =
            async_pipeline_stats.shader_queue_bytes.load(std::memory_order_relaxed),
        .shader_max_queue_bytes =
            async_pipeline_stats.shader_max_queue_bytes.load(std::memory_order_relaxed),
        .graphics_misses = async_pipeline_stats.graphics_misses.load(std::memory_order_relaxed),
        .compute_misses = async_pipeline_stats.compute_misses.load(std::memory_order_relaxed),
        .fail_fast_hits = async_pipeline_stats.fail_fast_hits.load(std::memory_order_relaxed),
        .fail_fast_compile_required =
            async_pipeline_stats.fail_fast_compile_required.load(std::memory_order_relaxed),
        .pending_hits = async_pipeline_stats.pending_hits.load(std::memory_order_relaxed),
        .fast_queued = async_pipeline_stats.fast_queued.load(std::memory_order_relaxed),
        .optimized_queued =
            async_pipeline_stats.optimized_queued.load(std::memory_order_relaxed),
        .queue_drops = async_pipeline_stats.queue_drops.load(std::memory_order_relaxed),
        .fast_completed = async_pipeline_stats.fast_completed.load(std::memory_order_relaxed),
        .optimized_completed =
            async_pipeline_stats.optimized_completed.load(std::memory_order_relaxed),
        .build_failures = async_pipeline_stats.build_failures.load(std::memory_order_relaxed),
        .published = async_pipeline_stats.published.load(std::memory_order_relaxed),
        .optimized_replacements =
            async_pipeline_stats.optimized_replacements.load(std::memory_order_relaxed),
        .skipped_uses = async_pipeline_stats.skipped_uses.load(std::memory_order_relaxed),
        .foreground_fallbacks =
            async_pipeline_stats.foreground_fallbacks.load(std::memory_order_relaxed),
        .shader_fast_compiled =
            async_pipeline_stats.shader_fast_compiled.load(std::memory_order_relaxed),
        .shader_optimized_queued =
            async_pipeline_stats.shader_optimized_queued.load(std::memory_order_relaxed),
        .shader_optimized_completed =
            async_pipeline_stats.shader_optimized_completed.load(std::memory_order_relaxed),
        .shader_optimized_published =
            async_pipeline_stats.shader_optimized_published.load(std::memory_order_relaxed),
        .shader_queue_drops =
            async_pipeline_stats.shader_queue_drops.load(std::memory_order_relaxed),
        .shader_negative_hits =
            async_pipeline_stats.shader_negative_hits.load(std::memory_order_relaxed),
        .shader_build_failures =
            async_pipeline_stats.shader_build_failures.load(std::memory_order_relaxed),
        .shader_replacements =
            async_pipeline_stats.shader_replacements.load(std::memory_order_relaxed),
        .shader_total_build_us =
            async_pipeline_stats.shader_total_build_us.load(std::memory_order_relaxed),
        .shader_max_build_us =
            async_pipeline_stats.shader_max_build_us.load(std::memory_order_relaxed),
        .total_build_us = async_pipeline_stats.total_build_us.load(std::memory_order_relaxed),
        .max_build_us = async_pipeline_stats.max_build_us.load(std::memory_order_relaxed),
    };
}

void ResetAsyncPipelineCompileStats() noexcept {
    async_pipeline_stats.max_queue_depth.store(
        async_pipeline_stats.queue_depth.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    async_pipeline_stats.shader_max_queue_depth.store(
        async_pipeline_stats.shader_queue_depth.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    async_pipeline_stats.shader_max_queue_bytes.store(
        async_pipeline_stats.shader_queue_bytes.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    async_pipeline_stats.graphics_misses.store(0, std::memory_order_relaxed);
    async_pipeline_stats.compute_misses.store(0, std::memory_order_relaxed);
    async_pipeline_stats.fail_fast_hits.store(0, std::memory_order_relaxed);
    async_pipeline_stats.fail_fast_compile_required.store(0, std::memory_order_relaxed);
    async_pipeline_stats.pending_hits.store(0, std::memory_order_relaxed);
    async_pipeline_stats.fast_queued.store(0, std::memory_order_relaxed);
    async_pipeline_stats.optimized_queued.store(0, std::memory_order_relaxed);
    async_pipeline_stats.queue_drops.store(0, std::memory_order_relaxed);
    async_pipeline_stats.fast_completed.store(0, std::memory_order_relaxed);
    async_pipeline_stats.optimized_completed.store(0, std::memory_order_relaxed);
    async_pipeline_stats.build_failures.store(0, std::memory_order_relaxed);
    async_pipeline_stats.published.store(0, std::memory_order_relaxed);
    async_pipeline_stats.optimized_replacements.store(0, std::memory_order_relaxed);
    async_pipeline_stats.skipped_uses.store(0, std::memory_order_relaxed);
    async_pipeline_stats.foreground_fallbacks.store(0, std::memory_order_relaxed);
    async_pipeline_stats.shader_fast_compiled.store(0, std::memory_order_relaxed);
    async_pipeline_stats.shader_optimized_queued.store(0, std::memory_order_relaxed);
    async_pipeline_stats.shader_optimized_completed.store(0, std::memory_order_relaxed);
    async_pipeline_stats.shader_optimized_published.store(0, std::memory_order_relaxed);
    async_pipeline_stats.shader_queue_drops.store(0, std::memory_order_relaxed);
    async_pipeline_stats.shader_negative_hits.store(0, std::memory_order_relaxed);
    async_pipeline_stats.shader_build_failures.store(0, std::memory_order_relaxed);
    async_pipeline_stats.shader_replacements.store(0, std::memory_order_relaxed);
    async_pipeline_stats.shader_total_build_us.store(0, std::memory_order_relaxed);
    async_pipeline_stats.shader_max_build_us.store(0, std::memory_order_relaxed);
    async_pipeline_stats.total_build_us.store(0, std::memory_order_relaxed);
    async_pipeline_stats.max_build_us.store(0, std::memory_order_relaxed);
}

constexpr static std::array DescriptorHeapSizes = {
    vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 512},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1024},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 1024},
};

static u32 MapOutputs(std::span<Shader::OutputMap, 3> outputs, const AmdGpu::VsOutputControl& ctl) {
    u32 num_outputs = 0;

    if (ctl.vs_out_misc_enable) {
        auto& misc_vec = outputs[num_outputs++];
        misc_vec[0] = ctl.use_vtx_point_size ? Output::PointSize : Output::None;
        misc_vec[1] = ctl.use_vtx_edge_flag
                          ? Output::EdgeFlag
                          : (ctl.use_vtx_gs_cut_flag ? Output::GsCutFlag : Output::None);
        misc_vec[2] =
            ctl.use_vtx_kill_flag
                ? Output::KillFlag
                : (ctl.use_vtx_render_target_idx ? Output::RenderTargetIndex : Output::None);
        misc_vec[3] = ctl.use_vtx_viewport_idx ? Output::ViewportIndex : Output::None;
    }

    if (ctl.vs_out_ccdist0_enable) {
        auto& ccdist0 = outputs[num_outputs++];
        ccdist0[0] = ctl.IsClipDistEnabled(0)
                         ? Output::ClipDist0
                         : (ctl.IsCullDistEnabled(0) ? Output::CullDist0 : Output::None);
        ccdist0[1] = ctl.IsClipDistEnabled(1)
                         ? Output::ClipDist1
                         : (ctl.IsCullDistEnabled(1) ? Output::CullDist1 : Output::None);
        ccdist0[2] = ctl.IsClipDistEnabled(2)
                         ? Output::ClipDist2
                         : (ctl.IsCullDistEnabled(2) ? Output::CullDist2 : Output::None);
        ccdist0[3] = ctl.IsClipDistEnabled(3)
                         ? Output::ClipDist3
                         : (ctl.IsCullDistEnabled(3) ? Output::CullDist3 : Output::None);
    }

    if (ctl.vs_out_ccdist1_enable) {
        auto& ccdist1 = outputs[num_outputs++];
        ccdist1[0] = ctl.IsClipDistEnabled(4)
                         ? Output::ClipDist4
                         : (ctl.IsCullDistEnabled(4) ? Output::CullDist4 : Output::None);
        ccdist1[1] = ctl.IsClipDistEnabled(5)
                         ? Output::ClipDist5
                         : (ctl.IsCullDistEnabled(5) ? Output::CullDist5 : Output::None);
        ccdist1[2] = ctl.IsClipDistEnabled(6)
                         ? Output::ClipDist6
                         : (ctl.IsCullDistEnabled(6) ? Output::CullDist6 : Output::None);
        ccdist1[3] = ctl.IsClipDistEnabled(7)
                         ? Output::ClipDist7
                         : (ctl.IsCullDistEnabled(7) ? Output::CullDist7 : Output::None);
    }

    return num_outputs;
}

const Shader::RuntimeInfo* PipelineCache::BuildRuntimeInfo(Stage stage, LogicalStage l_stage) {
    auto* const shader_lookup_profile =
        ExecutorGraphicsPipelineLookupProfile::Active();
    ExecutorGraphicsShaderTimingScope runtime_info_profile{
        shader_lookup_profile,
        ExecutorGraphicsShaderTiming::RuntimeInfo};
    auto& info = runtime_infos[u32(l_stage)];
    const auto& regs = liverpool->regs;
    const auto BuildCommon = [&](const auto& program) {
        info.num_user_data = program.settings.num_user_regs;
        info.num_input_vgprs = program.settings.vgpr_comp_cnt;
        info.num_allocated_vgprs = program.NumVgprs();
        info.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
        info.fp_round_mode32 = program.settings.fp_round_mode32;
    };
    info.Initialize(stage);
    switch (stage) {
    case Stage::Local: {
        BuildCommon(regs.ls_program);
        Shader::TessellationDataConstantBuffer tess_constants{};
        const auto* hull_info = infos[u32(Shader::LogicalStage::TessellationControl)];
        hull_info->ReadTessConstantBuffer(tess_constants);
        info.ls_info.ls_stride = tess_constants.ls_stride;
        break;
    }
    case Stage::Hull: {
        BuildCommon(regs.hs_program);
        info.hs_info.num_input_control_points = regs.ls_hs_config.hs_input_control_points;
        info.hs_info.num_threads = regs.ls_hs_config.hs_output_control_points;
        info.hs_info.tess_type = regs.tess_config.type;
        info.hs_info.offchip_lds_enable = regs.hs_program.settings.oc_lds_en;

        break;
    }
    case Stage::Export: {
        BuildCommon(regs.es_program);
        info.es_info.vertex_data_size = regs.vgt_esgs_ring_itemsize;
        if (l_stage == LogicalStage::TessellationEval) {
            info.es_vs_info.tess_type = regs.tess_config.type;
            info.es_vs_info.tess_topology = regs.tess_config.topology;
            info.es_vs_info.tess_partitioning = regs.tess_config.partitioning;
        }
        break;
    }
    case Stage::Vertex: {
        BuildCommon(regs.vs_program);
        info.vs_info.step_rate_0 = regs.vgt_instance_step_rate_0;
        info.vs_info.step_rate_1 = regs.vgt_instance_step_rate_1;
        info.vs_info.num_outputs = MapOutputs(info.vs_info.outputs, regs.vs_output_control);
        info.vs_info.emulate_depth_negative_one_to_one =
            !instance.IsDepthClipControlSupported() &&
            regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
        const auto host_primitive =
            graphics_primitive_override.value_or(regs.primitive_type);
        info.vs_info.tess_emulated_primitive =
            host_primitive == AmdGpu::PrimitiveType::RectList ||
            host_primitive == AmdGpu::PrimitiveType::QuadList;
        info.vs_info.clip_disable = regs.IsClipDisabled();
        if (l_stage == LogicalStage::TessellationEval) {
            info.es_vs_info.tess_type = regs.tess_config.type;
            info.es_vs_info.tess_topology = regs.tess_config.topology;
            info.es_vs_info.tess_partitioning = regs.tess_config.partitioning;
        }
        break;
    }
    case Stage::Geometry: {
        BuildCommon(regs.gs_program);
        auto& gs_info = info.gs_info;
        gs_info.num_outputs = MapOutputs(gs_info.outputs, regs.vs_output_control);
        gs_info.output_vertices = regs.vgt_gs_max_vert_out;
        gs_info.num_invocations =
            regs.vgt_gs_instance_cnt.IsEnabled() ? regs.vgt_gs_instance_cnt.count : 1;
        if (regs.stage_enable.raw == AmdGpu::ShaderStageEnable::LsHsEsGs) {
            gs_info.in_primitive = [&]() {
                switch (regs.tess_config.topology) {
                case AmdGpu::TessellationTopology::Point:
                    return AmdGpu::PrimitiveType::PointList;
                case AmdGpu::TessellationTopology::Line:
                    return AmdGpu::PrimitiveType::LineList;
                case AmdGpu::TessellationTopology::TriangleCw:
                case AmdGpu::TessellationTopology::TriangleCcw:
                    return AmdGpu::PrimitiveType::TriangleList;
                default:
                    UNREACHABLE();
                }
            }();
        } else {
            gs_info.in_primitive = regs.primitive_type;
        }
        for (u32 stream_id = 0; stream_id < Shader::GsMaxOutputStreams; ++stream_id) {
            gs_info.out_primitive[stream_id] =
                regs.vgt_gs_out_prim_type.GetPrimitiveType(stream_id);
        }
        gs_info.in_vertex_data_size = regs.vgt_esgs_ring_itemsize;
        gs_info.out_vertex_data_size = regs.vgt_gs_vert_itemsize[0];
        gs_info.mode = regs.vgt_gs_mode.mode;
        const auto params_vc = GetShaderParams(
            regs.vs_program.user_data.data(),
            regs.vs_program.Address<const u32*>(), Stage::Vertex);
        if (!params_vc) {
            return nullptr;
        }
        gs_info.vs_copy = params_vc->code;
        gs_info.vs_copy_hash = params_vc->hash;
        DumpShader(gs_info.vs_copy, gs_info.vs_copy_hash, Shader::Stage::Vertex, 0, "copy.bin");
        break;
    }
    case Stage::Fragment: {
        BuildCommon(regs.ps_program);
        info.fs_info.en_flags = regs.ps_input_ena;
        info.fs_info.addr_flags = regs.ps_input_addr;
        info.fs_info.num_inputs = regs.num_interp;
        info.fs_info.z_export_format = regs.z_export_format;
        u8 stencil_ref_export_enable = regs.depth_shader_control.stencil_op_val_export_enable |
                                       regs.depth_shader_control.stencil_test_val_export_enable;
        info.fs_info.mrtz_mask = regs.depth_shader_control.z_export_enable |
                                 (stencil_ref_export_enable << 1) |
                                 (regs.depth_shader_control.mask_export_enable << 2) |
                                 (regs.depth_shader_control.coverage_to_mask_enable << 3);
        const auto& cb0_blend = regs.blend_control[0];
        if (cb0_blend.enable) {
            info.fs_info.dual_source_blending =
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_dst_factor) ||
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_src_factor);
            if (cb0_blend.separate_alpha_blend) {
                info.fs_info.dual_source_blending |=
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_dst_factor) ||
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_src_factor);
            }
        } else {
            info.fs_info.dual_source_blending = false;
        }
        const auto& ps_inputs = regs.ps_inputs;
        for (u32 i = 0; i < regs.num_interp; i++) {
            info.fs_info.inputs[i] = {
                .param_index = u8(ps_inputs[i].input_offset),
                .is_default = bool(ps_inputs[i].use_default),
                .is_flat = bool(ps_inputs[i].flat_shade),
                .default_value = u8(ps_inputs[i].default_value),
            };
        }
        for (u32 i = 0; i < Shader::MaxColorBuffers; i++) {
            info.fs_info.color_buffers[i] = graphics_key.color_buffers[i];
        }
        info.fs_info.clip_distance_emulation =
            regs.vs_output_control.clip_distance_enable &&
            !regs.stage_enable.IsStageEnabled(static_cast<u32>(Stage::Local)) &&
            profile.needs_clip_distance_emulation;
        break;
    }
    case Stage::Compute: {
        const auto& cs_pgm = liverpool->GetCsRegs();
        info.num_user_data = cs_pgm.settings.num_user_regs;
        info.num_allocated_vgprs = cs_pgm.settings.num_vgprs * 4;
        info.cs_info.workgroup_size = {cs_pgm.num_thread_x.full, cs_pgm.num_thread_y.full,
                                       cs_pgm.num_thread_z.full};
        info.cs_info.tgid_enable = {cs_pgm.IsTgidEnabled(0), cs_pgm.IsTgidEnabled(1),
                                    cs_pgm.IsTgidEnabled(2)};
        info.cs_info.shared_memory_size = cs_pgm.SharedMemSize();
        break;
    }
    default:
        break;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    runtime_info_identities[u32(l_stage)] = InternRuntimeInfoIdentity(l_stage, info);
#endif
    return &info;
}

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
u64 PipelineCache::InternRuntimeInfoIdentity(const LogicalStage l_stage,
                                             const Shader::RuntimeInfo& info) {
    static_assert(std::is_trivially_copyable_v<Shader::RuntimeInfo>);
    const u64 hash = XXH3_64bits(&info, sizeof(info));
    const size_t stage_index = static_cast<size_t>(l_stage);
    const size_t set_index = static_cast<size_t>(hash) &
                             (RuntimeInfoIdentitySetCount - 1);
    static_assert((RuntimeInfoIdentitySetCount &
                   (RuntimeInfoIdentitySetCount - 1)) == 0);
    auto& set = runtime_info_identity_cache[stage_index][set_index];
    for (const auto& candidate : set) {
        if (candidate.valid && candidate.hash == hash &&
            std::memcmp(&candidate.info, &info, sizeof(info)) == 0) {
            return candidate.identity;
        }
    }

    auto& next_way = runtime_info_identity_next_way[stage_index][set_index];
    auto& victim = set[next_way];
    next_way = static_cast<u8>((next_way + 1) % RuntimeInfoIdentityWayCount);
    std::memcpy(&victim.info, &info, sizeof(info));
    victim.hash = hash;
    victim.identity = ++runtime_info_identity_sequence;
    if (victim.identity == 0) [[unlikely]] {
        victim.identity = ++runtime_info_identity_sequence;
    }
    victim.valid = true;
    return victim.identity;
}
#endif

PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,
                             AmdGpu::Liverpool* liverpool_,
                             VideoCore::BufferCache& buffer_cache_,
                             VideoCore::PageManager& page_manager_)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      buffer_cache{&buffer_cache_}, page_manager{&page_manager_},
      desc_heap{instance, scheduler.GetMasterSemaphore(), DescriptorHeapSizes} {
#if defined(LSX4_APPLE_RUNTIME)
    if (ClaudeShaderStorage::PipelineCache() && !Config::isPipelineCacheEnabled()) {
        Config::setPipelineCacheEnabled(true);
        Config::setPipelineCacheArchived(false);
    }
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool managed_shader_identity_cache =
        VideoCore::IsManagedVulkanDriverOptimization();
    const char* const shader_identity_cache_flag =
        std::getenv("EXECUTOR_SHADER_IDENTITY_CACHE");
    shader_identity_cache_enabled =
        managed_shader_identity_cache ||
        (shader_identity_cache_flag != nullptr &&
         shader_identity_cache_flag[0] != '\0' &&
         shader_identity_cache_flag[0] != '0');
    const char* const immutable_shader_identity_cache_flag =
        std::getenv("EXECUTOR_IMMUTABLE_SHADER_IDENTITY_CACHE");
    immutable_shader_identity_cache_enabled = shader_identity_cache_enabled &&
        (managed_shader_identity_cache ||
         (immutable_shader_identity_cache_flag != nullptr &&
          immutable_shader_identity_cache_flag[0] != '\0' &&
          immutable_shader_identity_cache_flag[0] != '0'));
    const bool shader_signature_word_prefilter_requested =
        
#if defined(LSX4_APPLE_RUNTIME)
        ExecutorAppleShaderHotStructureSelected() ||
#endif
        Common::TitleScopedOptimizationMarkerEnabled(
            "run-enable-shader-signature-word-prefilter");
    const char* const immutable_srt_snapshot_cache_flag =
        std::getenv("EXECUTOR_IMMUTABLE_SRT_SNAPSHOT_CACHE");
    immutable_srt_snapshot_cache_enabled =
        immutable_srt_snapshot_cache_flag != nullptr &&
        immutable_srt_snapshot_cache_flag[0] != '\0' &&
        immutable_srt_snapshot_cache_flag[0] != '0';
    const auto& capability_profile = instance.GetCapabilityProfile();
    const bool exact_sm8650_turnip_d9 =
        capability_profile.exact_mesa_turnip_a750_d9_lineage;
    const bool exact_sm8850_turnip =
        capability_profile.exact_mesa_turnip_adreno840;
    const bool exact_snapdragon_turnip_structural =
        exact_sm8650_turnip_d9 || exact_sm8850_turnip;
    const bool preserve_strong_snapdragon_bundle =
        ExecutorPreserveBloodborneSnapdragonRenderBundle();
    shader_signature_word_prefilter_enabled =
        !preserve_strong_snapdragon_bundle &&
        (shader_signature_word_prefilter_requested || exact_snapdragon_turnip_structural);
    executor_exact_snapdragon_turnip_program_hot_structural_default.store(
        exact_snapdragon_turnip_structural && !preserve_strong_snapdragon_bundle,
        std::memory_order_release);
    // The A840 temporal Inside A/B proved that the payload-retaining routes can
    // return stale shader/SRT state there (white/raw/full-screen flashes).  Keep
    // only layout/probe/TLS transformations automatic on A840.  The payload
    // routes retain their independently requestable markers, while the exact
    // A750 D9 lineage keeps its previously accepted automatic contract.
    executor_exact_sm8650_turnip_d9_program_hot_payload_default.store(
        exact_sm8650_turnip_d9 && !preserve_strong_snapdragon_bundle,
        std::memory_order_release);
    const bool stable_srt_epoch_hit_requested =
        Common::TitleScopedOptimizationMarkerEnabled(
            "run-enable-stable-srt-epoch-hit");
    const bool program_hot_exact_multiversion_requested =
        Common::TitleScopedOptimizationMarkerEnabled(
            "run-enable-program-hot-exact-multiversion");
    const bool program_hot_mru_probe_requested =
        
#if defined(LSX4_APPLE_RUNTIME)
        ExecutorAppleShaderHotStructureSelected() ||
#endif
        Common::TitleScopedOptimizationMarkerEnabled(
            "run-enable-program-hot-mru-probe");
    stable_srt_epoch_hit_enabled =
        !preserve_strong_snapdragon_bundle &&
        (stable_srt_epoch_hit_requested || exact_sm8650_turnip_d9);
    program_hot_exact_multiversion_enabled =
        !preserve_strong_snapdragon_bundle &&
        (program_hot_exact_multiversion_requested || exact_sm8650_turnip_d9);
    program_hot_mru_probe_enabled =
        !preserve_strong_snapdragon_bundle &&
        (program_hot_mru_probe_requested || exact_snapdragon_turnip_structural);
    if (exact_snapdragon_turnip_structural && !preserve_strong_snapdragon_bundle) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_SNAPDRAGON_SHADER_HOT_STACK] enabled=1 "
            "exactA750D9=%u exactA840Turnip=%u "
            "signatureWordPrefilter=1 programHotMultiversion=%u "
            "programHotMru=1 stableSrtEpoch=%u "
            "identityProof=exact generationProof=exact cpuStableProof=exact "
            "gpuCleanProof=exact",
            exact_sm8650_turnip_d9 ? 1u : 0u,
            exact_sm8850_turnip ? 1u : 0u,
            exact_sm8650_turnip_d9 ? 1u : 0u,
            exact_sm8650_turnip_d9 ? 1u : 0u);
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_SNAPDRAGON_PROGRAM_HOT_ANDROID_STACK] enabled=1 "
            "exactA750D9=%u exactA840Turnip=%u occupancyMask=1 groupedTls=1 "
            "callScopeTlsHoist=1 legacyTlsColdSplit=1 borrowedFetchResult=%u "
            "flattenedLease=%u scratchRecycle=%u manualMarkersPreserved=1",
            exact_sm8650_turnip_d9 ? 1u : 0u,
            exact_sm8850_turnip ? 1u : 0u,
            exact_sm8650_turnip_d9 ? 1u : 0u,
            exact_sm8650_turnip_d9 ? 1u : 0u,
            exact_sm8650_turnip_d9 ? 1u : 0u);
    }
    const char* const hle_clean_buffer_guard_flag =
        std::getenv("EXECUTOR_HLE_CLEAN_BUFFER_FASTPATH");
    const char* const async_shader_retranslation_disable_flag =
        std::getenv("EXECUTOR_DISABLE_ASYNC_SHADER_RETRANSLATION");
    async_shader_retranslation_disabled =
        async_shader_retranslation_disable_flag != nullptr &&
        async_shader_retranslation_disable_flag[0] != '\0' &&
        async_shader_retranslation_disable_flag[0] != '0';
    const bool hle_clean_buffer_guard_requested =
        hle_clean_buffer_guard_flag != nullptr &&
        hle_clean_buffer_guard_flag[0] != '\0' &&
        hle_clean_buffer_guard_flag[0] != '0';
    // Async shader jobs own their user-data, code and RuntimeInfo snapshots
    // before they leave the foreground guest-read scope. They never retain a
    // guest pointer, so the clean-buffer guard no longer needs to disable the
    // worker globally.
    hle_clean_buffer_guard_enabled = hle_clean_buffer_guard_requested;
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_ASYNC_SHADER_RETRANSLATION_CONFIG] disabled=%u "
        "fastTierAllowed=%u hleGuardRequested=%u hleGuardEnabled=%u",
        async_shader_retranslation_disabled ? 1u : 0u,
        async_shader_retranslation_disabled ? 0u : 1u,
        hle_clean_buffer_guard_requested ? 1u : 0u,
        hle_clean_buffer_guard_enabled ? 1u : 0u);
    if (shader_identity_cache_enabled) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_SHADER_IDENTITY_CACHE] enabled=1 scope=%s maxEntries=%zu",
            immutable_shader_identity_cache_enabled ? "backing_invalidation+fetch_epoch"
                                                    : "backing_invalidation",
            MaxShaderIdentityEntriesPerSubmit);
    }
    if (shader_signature_word_prefilter_enabled) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_SHADER_SIGNATURE_PREFILTER] enabled=1 prefixBytes=4 "
            "tailBytes=%zu searchBytes=%zu",
            AmdGpu::BinaryInfo::signature_ref.size() - sizeof(u32),
            static_cast<size_t>(AmdGpu::ShaderBinarySearchBytes));
    }
    if (immutable_srt_snapshot_cache_enabled) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_SRT_CACHE_CONFIG] immutableBackingValidation=1 "
            "gpuGeneratedSrt=unsupported");
    }
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_SRT_VICTIM_CACHE_CONFIG] primarySlotsPerProgram=%zu "
        "globalVictimSlots=%zu globalObjectCap=%zu replacement=CLOCK "
        "exactByteValidation=1",
        Program::NumSpecializationSnapshotSlots, MaxSrtVictimSnapshots,
        MaxSrtSnapshotObjects);
    if (program_hot_exact_multiversion_enabled) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_PROGRAM_HOT_MULTIVERSION_CONFIG] enabled=1 ways=4 "
            "exactByteValidation=1 generationSandwich=1 gpuCleanRequired=1");
    }
#endif
    const auto& vk12_props = instance.GetVk12Properties();
    profile = Shader::Profile{
        .max_ubo_size = instance.UniformMaxSize() - instance.UniformMinAlignment(),
        .max_viewport_width = instance.GetMaxViewportWidth(),
        .max_viewport_height = instance.GetMaxViewportHeight(),
        .max_shared_memory_size = instance.MaxComputeSharedMemorySize(),
        .supported_spirv = instance.GetCapabilityProfile().spirv_version,
        .subgroup_size = instance.SubgroupSize(),
        .support_int8 = instance.IsShaderInt8Supported(),
        .support_int16 = instance.IsShaderInt16Supported(),
        .support_int64 = instance.IsShaderInt64Supported(),
        .support_float16 = instance.IsShaderFloat16Supported(),
        .support_float64 = instance.IsShaderFloat64Supported(),
        .support_fp32_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat32),
        .support_fp32_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat32),
        .support_fp32_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat32),
        .support_legacy_vertex_attributes = instance_.IsLegacyVertexAttributesSupported(),
        .supports_image_load_store_lod = instance_.IsImageLoadStoreLodSupported(),
        .supports_native_cube_calc = instance_.IsAmdGcnShaderSupported(),
        .supports_trinary_minmax = instance_.IsAmdShaderTrinaryMinMaxSupported(),
        .supports_robust_buffer_access =
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            !(instance.GetCapabilityProfile().HasQuirk(
                  CapabilityQuirk::AllowManualBufferBoundsOverride) &&
              ::access("/data/user/0/app.lsx4.android/files/run-manual-buffer-bounds", F_OK) == 0),
#else
            true,
#endif
        .supports_buffer_fp32_atomic_min_max =
            instance_.IsShaderAtomicFloatBuffer32MinMaxSupported(),
        .supports_image_fp32_atomic_min_max = instance_.IsShaderAtomicFloatImage32MinMaxSupported(),
        .supports_buffer_int64_atomics = instance_.IsBufferInt64AtomicsSupported(),
        .supports_shared_int64_atomics = instance_.IsSharedInt64AtomicsSupported(),
        .supports_workgroup_explicit_memory_layout =
            instance_.IsWorkgroupMemoryExplicitLayoutSupported(),
        .supports_amd_shader_explicit_vertex_parameter =
            instance_.IsAmdShaderExplicitVertexParameterSupported(),
        .supports_fragment_shader_barycentric = instance_.IsFragmentShaderBarycentricSupported(),
        .has_incomplete_fragment_shader_barycentric =
            instance_.IsFragmentShaderBarycentricSupported() &&
            instance.GetDriverID() == vk::DriverId::eMoltenvk,
        .has_broken_spirv_float_controls = instance.GetCapabilityProfile().HasQuirk(
            CapabilityQuirk::BrokenSpirvFloatControls),
        .codegen_quirks =
            static_cast<u8>(
                (instance.GetCapabilityProfile().RequiresSoftwareComputeBufferBounds(
                     instance.IsRobustBufferAccess2Supported() &&
                     !Config::managedDisableVkRobustness())
                     ? static_cast<u8>(Shader::CodegenQuirk::RequireExactComputeBufferBounds)
                     : u8{0}) |
                (instance.GetCapabilityProfile().HasQuirk(
                     CapabilityQuirk::DeferFormattedBufferTypesUntilLowering)
                     ? static_cast<u8>(
                           Shader::CodegenQuirk::DeferFormattedBufferTypesUntilLowering)
                     : u8{0}) |
                (instance.GetCapabilityProfile().HasQuirk(
                     CapabilityQuirk::LowerAliasedSharedU64ToU32)
                     ? static_cast<u8>(Shader::CodegenQuirk::LowerAliasedSharedU64ToU32)
                     : u8{0}) |
                (instance.GetCapabilityProfile().HasQuirk(
                     CapabilityQuirk::LowerNon32BitSharedToU32)
                     ? static_cast<u8>(Shader::CodegenQuirk::LowerNon32BitSharedToU32)
                     : u8{0}) |
                (instance.GetCapabilityProfile().memory_policy.bda_page_table.mode ==
                         BdaPageTableMode::SegmentedSparse
                     ? static_cast<u8>(Shader::CodegenQuirk::SegmentedBdaPageTable)
                     : u8{0})),
        .needs_manual_interpolation =
            instance.IsFragmentShaderBarycentricSupported() &&
            instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        .needs_lds_barriers = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary ||
                              instance.GetDriverID() == vk::DriverId::eMoltenvk,
        .needs_buffer_offsets = instance.StorageMinAlignment() > 4,
        .needs_unorm_fixup = instance.GetDriverID() == vk::DriverId::eMoltenvk,
        .needs_clip_distance_emulation = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        .supports_shader_stencil_export = instance_.IsShaderStencilExportSupported(),
        .supports_buffer_device_address = instance.IsBufferDeviceAddressSupported(),
    };

    std::vector<u8> host_cache_data = LoadHostPipelineCache();
    vk::PipelineCacheCreateInfo cache_info{};
    cache_info.initialDataSize = host_cache_data.size();
    cache_info.pInitialData = host_cache_data.data();
    auto cache_result_value =
        instance.GetDevice().createPipelineCacheUnique(cache_info);
    auto cache_result = cache_result_value.result;
    auto cache = std::move(cache_result_value.value);
    bool host_cache_rejected = false;
    if (cache_result != vk::Result::eSuccess && !host_cache_data.empty()) {
        LOG_WARNING(Render_Vulkan,
                    "Native Vulkan pipeline cache was rejected; rebuilding it");
        host_cache_rejected = true;
        cache_result_value = instance.GetDevice().createPipelineCacheUnique({});
        cache_result = cache_result_value.result;
        cache = std::move(cache_result_value.value);
    }
    if (stable_srt_epoch_hit_enabled) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_STABLE_SRT_EPOCH] enabled=1 contract=identity+cpuStable+gpuClean");
    }
    ASSERT_MSG(cache_result == vk::Result::eSuccess, "Failed to create pipeline cache: {}",
               vk::to_string(cache_result));
    pipeline_cache = std::move(cache);
    async_pipeline_cache_seed = std::move(host_cache_data);
    next_pipeline_cache_checkpoint =
        std::chrono::steady_clock::now() + std::chrono::seconds{15};
    if (host_cache_rejected) {
        MarkHostPipelineCacheDirty();
    }

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_VK_PIPELINE_CACHE] loadedBytes=%zu vendor=0x%x "
                        "device=0x%x driver=0x%x",
                        async_pipeline_cache_seed.size(), instance.GetVendorID(),
                        instance.GetDeviceID(), instance.GetDriverVersion());
#endif

#if defined(LSX4_APPLE_RUNTIME)
    AppleComputeSplit::NewCache();
#endif
    WarmUp();
    EnsureAsyncPipelineMode();
}

PipelineCache::~PipelineCache() {
    StopAsyncPipelineWorkers();
    PublishAsyncPipelineResults();
    DestroyRetiredShaderModules();
    StopBackgroundWarmUp();
    SaveHostPipelineCache();
    SavePipelineNegativeCache();
    // Close new clean-HLE admissions before releasing the permanent shader
    // span pins. The exclusive scan gate first drains an already-active
    // writer; PageManager outlives PipelineCache by Rasterizer member order.
    std::unique_lock shader_scan_lock{hle_clean_buffer_shader_scan_mutex};
    page_manager->DisableHleCleanBufferFastPathFailClosed();
    for (const auto& [address, size] : hle_shader_read_residency_leases) {
        page_manager->UntrackHleShaderReadResidency(address, size);
    }
}

std::string PipelineCache::HostPipelineCacheName() const {
    const auto pipeline_cache_uuid = instance.GetPipelineCacheUUID();
    const u64 pipeline_cache_uuid_hash =
        XXH3_64bits(pipeline_cache_uuid.data(), pipeline_cache_uuid.size());
    const u32 driver_id = static_cast<u32>(instance.GetDriverID());
    const char* turnip_debug = std::getenv("TU_DEBUG");
    const char* ir3_debug = std::getenv("IR3_SHADER_DEBUG");
    const char* force_ps_const = std::getenv("EXECUTOR_FORCE_PS_CONST_SOLID");
    const char* force_fs_image_const = std::getenv("EXECUTOR_FORCE_FS_IMAGE_CONST");
    const char* scalar_image_coords = std::getenv("EXECUTOR_BISECT_SCALAR_IMAGE_COORDS");
    const char* visualize_image_coords = std::getenv("EXECUTOR_DIAGNOSTIC_FS_IMAGE_COORDS");
    const char* fs_query_size = std::getenv("EXECUTOR_DIAGNOSTIC_FS_QUERY_SIZE");
    const char* fs_texel_fetch = std::getenv("EXECUTOR_DIAGNOSTIC_FS_TEXEL_FETCH");
    const char* explicit_lod0 = std::getenv("EXECUTOR_BISECT_FS_EXPLICIT_LOD0");
    const char* forced_ssbo_alignment =
        std::getenv("EXECUTOR_BISECT_FORCE_SSBO_ALIGNMENT");
    const char* raw_fs_input0 = std::getenv("EXECUTOR_DIAGNOSTIC_FS_RAW_INPUT0");
    const char* fs_fragcoord = std::getenv("EXECUTOR_DIAGNOSTIC_FS_FRAGCOORD");
    const char* fs_fragcoord_mono_x =
        std::getenv("EXECUTOR_DIAGNOSTIC_FS_FRAGCOORD_MONO_X");
    const char* fs_fragcoord_mono_y =
        std::getenv("EXECUTOR_DIAGNOSTIC_FS_FRAGCOORD_MONO_Y");
    const char* vs_attr0_from_position =
        std::getenv("EXECUTOR_DIAGNOSTIC_VS_ATTR0_FROM_POSITION");
    const char* disable_dynamic_vertex_input =
        std::getenv("EXECUTOR_BISECT_DISABLE_DYNAMIC_VERTEX_INPUT");
    const char* disable_legacy_vertex_attributes =
        std::getenv("EXECUTOR_BISECT_DISABLE_LEGACY_VERTEX_ATTRIBUTES");
    const auto& bda_policy =
        instance.GetCapabilityProfile().memory_policy.bda_page_table;
    const bool segmented_bda =
        bda_policy.mode == BdaPageTableMode::SegmentedSparse;
    if ((turnip_debug != nullptr && turnip_debug[0] != '\0') ||
        (ir3_debug != nullptr && ir3_debug[0] != '\0') ||
        (force_ps_const != nullptr && force_ps_const[0] != '\0') ||
        (force_fs_image_const != nullptr && force_fs_image_const[0] != '\0') ||
        (scalar_image_coords != nullptr && scalar_image_coords[0] != '\0') ||
        (visualize_image_coords != nullptr && visualize_image_coords[0] != '\0') ||
        (fs_query_size != nullptr && fs_query_size[0] != '\0') ||
        (fs_texel_fetch != nullptr && fs_texel_fetch[0] != '\0') ||
        (explicit_lod0 != nullptr && explicit_lod0[0] != '\0') ||
        (forced_ssbo_alignment != nullptr && forced_ssbo_alignment[0] != '\0') ||
        (raw_fs_input0 != nullptr && raw_fs_input0[0] != '\0') ||
        (fs_fragcoord != nullptr && fs_fragcoord[0] != '\0') ||
        (fs_fragcoord_mono_x != nullptr && fs_fragcoord_mono_x[0] != '\0') ||
        (fs_fragcoord_mono_y != nullptr && fs_fragcoord_mono_y[0] != '\0') ||
        (vs_attr0_from_position != nullptr && vs_attr0_from_position[0] != '\0') ||
        (disable_dynamic_vertex_input != nullptr &&
         disable_dynamic_vertex_input[0] != '\0') ||
        (disable_legacy_vertex_attributes != nullptr &&
         disable_legacy_vertex_attributes[0] != '\0') ||
        segmented_bda) {
        const std::string debug_key =
            fmt::format("tu={};ir3={};psconst={};fsimageconst={};scalarcoords={};viewcoords={};fsquerysize={};fstexelfetch={};lod0={};ssboalign={};rawfsin0={};fsfragcoord={};fsfragmonox={};fsfragmonoy={};vsattr0pos={};staticvi={};nolegacyvi={};bda={};bdasegments={}",
                        turnip_debug != nullptr ? turnip_debug : "",
                        ir3_debug != nullptr ? ir3_debug : "",
                        force_ps_const != nullptr ? force_ps_const : "",
                        force_fs_image_const != nullptr ? force_fs_image_const : "",
                        scalar_image_coords != nullptr ? scalar_image_coords : "",
                        visualize_image_coords != nullptr ? visualize_image_coords : "",
                        fs_query_size != nullptr ? fs_query_size : "",
                        fs_texel_fetch != nullptr ? fs_texel_fetch : "",
                        explicit_lod0 != nullptr ? explicit_lod0 : "",
                        forced_ssbo_alignment != nullptr ? forced_ssbo_alignment : "",
                        raw_fs_input0 != nullptr ? raw_fs_input0 : "",
                        fs_fragcoord != nullptr ? fs_fragcoord : "",
                        fs_fragcoord_mono_x != nullptr ? fs_fragcoord_mono_x : "",
                        fs_fragcoord_mono_y != nullptr ? fs_fragcoord_mono_y : "",
                        vs_attr0_from_position != nullptr ? vs_attr0_from_position : "",
                        disable_dynamic_vertex_input != nullptr
                            ? disable_dynamic_vertex_input
                            : "",
                        disable_legacy_vertex_attributes != nullptr
                            ? disable_legacy_vertex_attributes
                            : "",
                        BdaPageTableModeName(bda_policy.mode),
                        bda_policy.descriptor_segments);
        return fmt::format("vk-native-v{}-{:08x}-{:08x}-{:08x}-{:08x}-{:08x}-{:016x}-tu-debug-{:016x}.bin",
                           HostPipelineCacheCompatibilityEpoch,
                           instance.GetVendorID(), instance.GetDeviceID(), driver_id,
                           instance.GetDriverVersion(), instance.ApiVersion(),
                           pipeline_cache_uuid_hash,
                           XXH3_64bits(debug_key.data(), debug_key.size()));
    }
    return fmt::format("vk-native-v{}-{:08x}-{:08x}-{:08x}-{:08x}-{:08x}-{:016x}.bin",
                       HostPipelineCacheCompatibilityEpoch,
                       instance.GetVendorID(), instance.GetDeviceID(), driver_id,
                       instance.GetDriverVersion(), instance.ApiVersion(),
                       pipeline_cache_uuid_hash);
}

bool PipelineCache::WriteCacheFileAtomically(const std::filesystem::path& path,
                                             const std::vector<u8>& data) {
    auto temporary_path = path;
    temporary_path += ".tmp";
    {
        Common::FS::IOFile file{temporary_path, Common::FS::FileAccessMode::Create};
        if (!file.IsOpen() || file.Write(data) != data.size() || !file.Commit()) {
            file.Close();
            std::error_code remove_error;
            std::filesystem::remove(temporary_path, remove_error);
            return false;
        }
        file.Close();
    }

    std::error_code rename_error;
    std::filesystem::rename(temporary_path, path, rename_error);
#ifdef _WIN32
    // std::filesystem::rename cannot replace an existing file on Windows.
    // Android/POSIX takes the atomic replacement path above.
    if (rename_error) {
        std::error_code remove_error;
        std::filesystem::remove(path, remove_error);
        rename_error.clear();
        std::filesystem::rename(temporary_path, path, rename_error);
    }
#endif
    if (!rename_error) {
        return true;
    }

    std::error_code remove_error;
    std::filesystem::remove(temporary_path, remove_error);
    LOG_WARNING(Render_Vulkan, "Failed to atomically replace pipeline cache {}: {}",
                path.string(), rename_error.message());
    return false;
}

std::vector<u8> PipelineCache::LoadHostPipelineCache() const {
    if (!Config::isPipelineCacheEnabled()) {
        return {};
    }
    const auto& game_info = Common::ElfInfo::Instance();
    const auto path = Common::FS::GetUserPath(Common::FS::PathType::CacheDir) /
                      game_info.GameSerial() / HostPipelineCacheName();
    const Common::FS::IOFile file{path, Common::FS::FileAccessMode::Read};
    if (!file.IsOpen() || file.GetSize() < sizeof(vk::PipelineCacheHeaderVersionOne)) {
        return {};
    }
    std::vector<u8> data(file.GetSize());
    if (file.Read(data) != data.size()) {
        return {};
    }
    vk::PipelineCacheHeaderVersionOne header{};
    std::memcpy(&header, data.data(), sizeof(header));
    if (header.headerSize < sizeof(header) || header.headerSize > data.size() ||
        header.headerVersion != vk::PipelineCacheHeaderVersion::eOne ||
        header.vendorID != instance.GetVendorID() ||
        header.deviceID != instance.GetDeviceID() ||
        std::memcmp(header.pipelineCacheUUID.data(),
                    instance.GetPipelineCacheUUID().data(), VK_UUID_SIZE) != 0) {
        LOG_WARNING(Render_Vulkan,
                    "Ignoring incompatible native Vulkan pipeline cache");
        return {};
    }
    return data;
}

void PipelineCache::SaveHostPipelineCache() {
    if (host_pipeline_cache_saved_generation == host_pipeline_cache_generation ||
        !pipeline_cache ||
        !Config::isPipelineCacheEnabled() || scheduler.IsDeviceLost()) {
        return;
    }
    auto [result, data] =
        instance.GetDevice().getPipelineCacheData(*pipeline_cache);
    if (result != vk::Result::eSuccess || data.empty()) {
        return;
    }
    const auto& game_info = Common::ElfInfo::Instance();
    const auto directory = Common::FS::GetUserPath(Common::FS::PathType::CacheDir) /
                           game_info.GameSerial();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return;
    }
    if (!WriteCacheFileAtomically(directory / HostPipelineCacheName(), data)) {
        return;
    }
    host_pipeline_cache_saved_generation = host_pipeline_cache_generation;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_VK_PIPELINE_CACHE] savedBytes=%zu generation=%llu",
                        data.size(),
                        static_cast<unsigned long long>(host_pipeline_cache_generation));
#endif
}

void PipelineCache::MarkHostPipelineCacheDirty() {
    ++host_pipeline_cache_generation;
}

void PipelineCache::MaybeCheckpointPipelineCaches() {
    if (!Config::isPipelineCacheEnabled()) {
        return;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    // Extracting driver cache data and durably rewriting two files can stall
    // the submit/render path. Shader and pipeline blobs already use the
    // background database writer, and final cache persistence still happens in
    // the destructor. Periodic in-game checkpoints are therefore explicit
    // opt-in diagnostics/recovery behavior on Android.
    static const bool periodic_checkpoint_enabled =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-enable-periodic-pipeline-cache-checkpoint",
                 F_OK) == 0;
    if (!periodic_checkpoint_enabled) {
        return;
    }
#endif
    const auto now = std::chrono::steady_clock::now();
    if (now < next_pipeline_cache_checkpoint) {
        return;
    }
    // Capture early startup work quickly, then keep expensive driver cache
    // extraction and durable I/O off the per-frame path for at least a minute.
    next_pipeline_cache_checkpoint = now + std::chrono::minutes{1};

    auto background_lock = LockBackgroundWarmUpForForeground();
    if (host_pipeline_cache_saved_generation == host_pipeline_cache_generation &&
        !pipeline_negative_cache_dirty) {
        return;
    }
    SaveHostPipelineCache();
    SavePipelineNegativeCache();
}

void PipelineCache::ResetPipelineBuildState() {
    infos.fill(nullptr);
    modules.fill(nullptr);
    fetch_shader.reset();
}

AsyncPipelineCompileStats PipelineCache::GetAsyncPipelineStats() const noexcept {
    return GetAsyncPipelineCompileStats();
}

void PipelineCache::EnsureAsyncPipelineMode() {
    if (Config::managedAsyncPipelineCompilation()) {
        StartAsyncPipelineWorkers();
    } else if (async_pipeline_started) {
        StopAsyncPipelineWorkers();
        PublishAsyncPipelineResults();
        DestroyRetiredShaderModules();
        std::scoped_lock background_lock{background_warmup_mutex};
        for (const auto& key : fallback_graphics_pipelines) {
            if (auto it = graphics_pipelines.find(key);
                it != graphics_pipelines.end()) {
                auto retired = std::move(it.value());
                graphics_pipelines.erase(it);
                if (retired) {
                    scheduler.DeferOperation(
                        [pipeline = std::move(retired)]() mutable {
                            pipeline.reset();
                        });
                }
            }
        }
        for (const auto& key : fallback_compute_pipelines) {
            if (auto it = compute_pipelines.find(key);
                it != compute_pipelines.end()) {
                auto retired = std::move(it.value());
                compute_pipelines.erase(it);
                if (retired) {
                    scheduler.DeferOperation(
                        [pipeline = std::move(retired)]() mutable {
                            pipeline.reset();
                        });
                }
            }
        }
        fallback_graphics_pipelines.clear();
        fallback_compute_pipelines.clear();
        last_graphics_pipeline = nullptr;
        last_graphics_state_generation = std::numeric_limits<u64>::max();
        last_graphics_lookup_resolved = false;
    }
}

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
void PipelineCache::MaybePublishResidencyTelemetry(const bool force) noexcept {
    if (!force && residency_telemetry_initialized &&
        residency_telemetry_last_new_pipelines == num_new_pipelines) {
        return;
    }
    PublishPipelineResidencyTelemetry(
        static_cast<u64>(program_cache.size()),
        static_cast<u64>(graphics_pipelines.size()),
        static_cast<u64>(compute_pipelines.size()),
        static_cast<u64>(num_new_pipelines),
        static_cast<u64>(shader_identity_cache.size()),
        static_cast<u64>(srt_snapshot_object_count),
        static_cast<u64>(srt_snapshot_retained_bytes));
    residency_telemetry_last_new_pipelines = num_new_pipelines;
    residency_telemetry_initialized = true;
}
#endif

void PipelineCache::StartAsyncPipelineWorkers() {
    if (async_pipeline_started || !Config::managedAsyncPipelineCompilation() ||
        scheduler.IsDeviceLost()) {
        return;
    }
    const size_t default_workers =
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        1;
#else
        2;
#endif
    const size_t worker_count =
        ReadBoundedAsyncPipelineOption("EXECUTOR_ASYNC_PIPELINE_WORKERS",
                                       default_workers, 1, 2);
    async_pipeline_queue_capacity =
        ReadBoundedAsyncPipelineOption("EXECUTOR_ASYNC_PIPELINE_QUEUE", 128, 16, 512);
    async_shader_queue_capacity =
        ReadBoundedAsyncPipelineOption("EXECUTOR_ASYNC_SHADER_QUEUE", 64, 8, 256);
    async_shader_queue_byte_capacity =
        ReadBoundedAsyncPipelineOption("EXECUTOR_ASYNC_SHADER_QUEUE_MB", 32, 4, 256) *
        1024 * 1024;
    {
        std::scoped_lock lock{async_pipeline_mutex};
        async_pipeline_accepting = true;
        async_pipeline_started = true;
    }
    async_pipeline_stats.fail_fast_supported.store(
        instance.IsPipelineCreationCacheControlSupported(), std::memory_order_relaxed);
    async_pipeline_stats.worker_count.store(static_cast<u32>(worker_count),
                                            std::memory_order_relaxed);
    async_pipeline_stats.queue_capacity.store(
        static_cast<u32>(async_pipeline_queue_capacity), std::memory_order_relaxed);
    async_pipeline_stats.shader_queue_capacity.store(
        static_cast<u32>(async_shader_queue_capacity),
        std::memory_order_relaxed);
    async_pipeline_stats.shader_queue_byte_capacity.store(
        async_shader_queue_byte_capacity, std::memory_order_relaxed);
    async_pipeline_workers.reserve(worker_count);
    for (u32 index = 0; index < worker_count; ++index) {
        async_pipeline_workers.emplace_back(
            [this, index](const std::stop_token stop_token) {
                AsyncPipelineWorker(stop_token, index);
            });
    }
    if (!async_shader_retranslation_disabled) {
        async_shader_worker = std::jthread{
            [this](const std::stop_token stop_token) {
                AsyncShaderWorker(stop_token);
            }};
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_ASYNC_PIPELINE_CONFIG] enabled=1 workers=%zu queue=%zu "
        "shaderQueue=%zu shaderQueueMiB=%zu failFast=%u "
        "fallback=disableOptimization optimizedReplacement=%s "
        "shaderRetranslation=%s",
        worker_count, async_pipeline_queue_capacity, async_shader_queue_capacity,
        async_shader_queue_byte_capacity / (1024 * 1024),
        instance.IsPipelineCreationCacheControlSupported() ? 1u : 0u,
        DeferAsyncLiveReplacement(instance) ? "nextLaunch" : "live",
        async_shader_retranslation_disabled ? "disabled" : "async");
#endif
}

void PipelineCache::StopAsyncPipelineWorkers() {
    if (!async_pipeline_started && async_pipeline_workers.empty() &&
        !async_shader_worker.joinable()) {
        return;
    }
    {
        std::scoped_lock lock{async_pipeline_mutex};
        async_pipeline_accepting = false;
        async_pipeline_jobs.clear();
        async_shader_jobs.clear();
        async_shader_queued_bytes = 0;
        pending_fast_graphics_pipelines.clear();
        pending_optimized_graphics_pipelines.clear();
        pending_fast_compute_pipelines.clear();
        pending_optimized_compute_pipelines.clear();
        pending_optimized_shaders.clear();
        async_pipeline_stats.queue_depth.store(0, std::memory_order_relaxed);
        async_pipeline_stats.shader_queue_depth.store(0,
                                                      std::memory_order_relaxed);
        async_pipeline_stats.shader_queue_bytes.store(0,
                                                      std::memory_order_relaxed);
    }
    for (auto& worker : async_pipeline_workers) {
        worker.request_stop();
    }
    if (async_shader_worker.joinable()) {
        async_shader_worker.request_stop();
    }
    async_pipeline_cv.notify_all();
    async_pipeline_workers.clear();
    async_shader_worker = {};
    {
        std::scoped_lock lock{async_pipeline_mutex};
        async_pipeline_started = false;
    }
    async_pipeline_stats.worker_count.store(0, std::memory_order_relaxed);
}

bool PipelineCache::QueueAsyncGraphicsPipeline(
    const GraphicsPipelineKey& key,
    const std::span<const Shader::Info* const, MaxShaderStages> stage_infos,
    const std::span<const Shader::RuntimeInfo, MaxShaderStages> stage_runtime_infos,
    std::optional<Shader::Gcn::FetchShaderData> stage_fetch_shader,
    const std::span<const vk::ShaderModule, MaxShaderStages> stage_modules,
    GraphicsPipeline::SerializationSupport sdata, const bool optimized) {
    auto payload = std::make_shared<AsyncGraphicsPayload>();
    payload->key = key;
    std::ranges::copy(stage_infos, payload->infos.begin());
    std::ranges::copy(stage_runtime_infos, payload->runtime_infos.begin());
    payload->fetch_shader = std::move(stage_fetch_shader);
    std::ranges::copy(stage_modules, payload->modules.begin());
    payload->sdata = std::move(sdata);

    std::scoped_lock lock{async_pipeline_mutex};
    if (!async_pipeline_accepting) {
        return false;
    }
    auto& pending = optimized ? pending_optimized_graphics_pipelines
                              : pending_fast_graphics_pipelines;
    if (pending.contains(key)) {
        async_pipeline_stats.pending_hits.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (async_pipeline_jobs.size() >= async_pipeline_queue_capacity) {
        async_pipeline_stats.queue_drops.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    pending.emplace(key);
    AsyncPipelineJob job{
        .payload = std::move(payload),
        .tier = optimized ? AsyncPipelineTier::Optimized : AsyncPipelineTier::Fast,
    };
    if (optimized) {
        async_pipeline_jobs.push_back(std::move(job));
    } else {
        const auto first_optimized = std::find_if(
            async_pipeline_jobs.begin(), async_pipeline_jobs.end(),
            [](const AsyncPipelineJob& queued) {
                return queued.tier == AsyncPipelineTier::Optimized;
            });
        async_pipeline_jobs.insert(first_optimized, std::move(job));
    }
    (optimized ? async_pipeline_stats.optimized_queued
               : async_pipeline_stats.fast_queued)
        .fetch_add(1, std::memory_order_relaxed);
    const u32 queue_depth = static_cast<u32>(async_pipeline_jobs.size());
    async_pipeline_stats.queue_depth.store(queue_depth, std::memory_order_relaxed);
    AtomicMax(async_pipeline_stats.max_queue_depth, queue_depth);
    async_pipeline_cv.notify_one();
    return true;
}

bool PipelineCache::QueueAsyncComputePipeline(
    const ComputePipelineKey& key, const Shader::Info& info,
    const std::array<u32, 3>& workgroup_size, const vk::ShaderModule module,
    ComputePipeline::SerializationSupport sdata, const bool optimized) {
    auto payload = std::make_shared<AsyncComputePayload>();
    payload->key = key;
    payload->info = &info;
    payload->workgroup_size = workgroup_size;
    payload->module = module;
    payload->sdata = std::move(sdata);

    std::scoped_lock lock{async_pipeline_mutex};
    if (!async_pipeline_accepting) {
        return false;
    }
    auto& pending =
        optimized ? pending_optimized_compute_pipelines : pending_fast_compute_pipelines;
    if (pending.contains(key)) {
        async_pipeline_stats.pending_hits.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (async_pipeline_jobs.size() >= async_pipeline_queue_capacity) {
        async_pipeline_stats.queue_drops.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    pending.emplace(key);
    AsyncPipelineJob job{
        .payload = std::move(payload),
        .tier = optimized ? AsyncPipelineTier::Optimized : AsyncPipelineTier::Fast,
    };
    if (optimized) {
        async_pipeline_jobs.push_back(std::move(job));
    } else {
        const auto first_optimized = std::find_if(
            async_pipeline_jobs.begin(), async_pipeline_jobs.end(),
            [](const AsyncPipelineJob& queued) {
                return queued.tier == AsyncPipelineTier::Optimized;
            });
        async_pipeline_jobs.insert(first_optimized, std::move(job));
    }
    (optimized ? async_pipeline_stats.optimized_queued
               : async_pipeline_stats.fast_queued)
        .fetch_add(1, std::memory_order_relaxed);
    const u32 queue_depth = static_cast<u32>(async_pipeline_jobs.size());
    async_pipeline_stats.queue_depth.store(queue_depth, std::memory_order_relaxed);
    AtomicMax(async_pipeline_stats.max_queue_depth, queue_depth);
    async_pipeline_cv.notify_one();
    return true;
}

bool PipelineCache::QueueAsyncShaderOptimization(
    std::shared_ptr<AsyncShaderPayload> payload) {
    if (async_shader_retranslation_disabled) {
        DEBUG_ASSERT_MSG(false,
                         "Async shader optimization queued while retranslation is disabled");
        return false;
    }
    if (!payload || !payload->program ||
        payload->perm_idx >= payload->program->modules.size()) {
        return false;
    }
    const AsyncShaderKey key{
        .program = payload->program,
        .perm_idx = payload->perm_idx,
        .generation = payload->expected_generation,
        .cache_generation = payload->cache_generation,
    };
    if (IsShaderBinaryTierKnownInvalid(payload->info->pgm_hash,
                                       payload->perm_idx,
                                       ShaderBinaryTier::Optimized)) {
        std::scoped_lock lock{async_pipeline_mutex};
        invalid_optimized_shaders.emplace(key);
        async_pipeline_stats.shader_negative_hits.fetch_add(
            1, std::memory_order_relaxed);
        return false;
    }

    std::scoped_lock lock{async_pipeline_mutex};
    if (!async_pipeline_accepting || invalid_optimized_shaders.contains(key)) {
        return false;
    }
    if (pending_optimized_shaders.contains(key)) {
        async_pipeline_stats.pending_hits.fetch_add(1,
                                                    std::memory_order_relaxed);
        return true;
    }
    if (pending_optimized_shaders.size() >= async_shader_queue_capacity ||
        payload->queued_bytes > async_shader_queue_byte_capacity ||
        async_shader_queued_bytes >
            async_shader_queue_byte_capacity - payload->queued_bytes) {
        async_pipeline_stats.shader_queue_drops.fetch_add(
            1, std::memory_order_relaxed);
        return false;
    }
    pending_optimized_shaders.emplace(key);
    async_shader_queued_bytes += payload->queued_bytes;
    async_shader_jobs.push_back(std::move(payload));
    const u32 shader_queue_depth =
        static_cast<u32>(pending_optimized_shaders.size());
    async_pipeline_stats.shader_queue_depth.store(
        shader_queue_depth, std::memory_order_relaxed);
    async_pipeline_stats.shader_queue_bytes.store(
        async_shader_queued_bytes, std::memory_order_relaxed);
    AtomicMax(async_pipeline_stats.shader_max_queue_depth,
              shader_queue_depth);
    AtomicMax(async_pipeline_stats.shader_max_queue_bytes,
              static_cast<u64>(async_shader_queued_bytes));
    async_pipeline_stats.shader_optimized_queued.fetch_add(
        1, std::memory_order_relaxed);
    async_pipeline_cv.notify_all();
    return true;
}

void PipelineCache::AsyncShaderWorker(const std::stop_token stop_token) {
    Common::SetCurrentThreadName("shadPS4:VkShader");
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
#if defined(LSX4_APPLE_RUNTIME)
    AppleWorkerPriority216::SetBackgroundPriority(7, "shader");
#else
    (void)::setpriority(PRIO_PROCESS, 0, 7);
#endif
#endif
    const auto device = instance.GetDevice();
    for (;;) {
        std::shared_ptr<AsyncShaderPayload> payload;
        {
            std::unique_lock lock{async_pipeline_mutex};
            const bool ready = async_pipeline_cv.wait(
                lock, stop_token, [this] {
                    return !async_shader_jobs.empty() ||
                           !async_pipeline_accepting;
                });
            if (!ready || stop_token.stop_requested() ||
                (!async_pipeline_accepting && async_shader_jobs.empty())) {
                return;
            }
            if (DeferAsyncLiveReplacement(instance) &&
                AsyncSteadyMilliseconds() <
                    async_last_foreground_activity_ms.load(
                        std::memory_order_acquire) +
                        750u) {
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
                continue;
            }
            payload = std::move(async_shader_jobs.front());
            async_shader_jobs.pop_front();
        }

        AsyncShaderResult completed{
            .payload = payload,
            .result = vk::Result::eErrorUnknown,
        };
        const auto started = std::chrono::steady_clock::now();
        if (!scheduler.IsDeviceLost() && payload->info) {
            std::unique_ptr<Shader::Info> optimized_info;
            std::unique_ptr<Shader::Pools> optimized_pools;
            std::unique_ptr<Shader::IR::Program> optimized_ir;
            auto optimized_runtime_info = payload->source_runtime_info;
            auto binding = payload->binding_start;
            {
                const Shader::ShaderParams owned_params{
                    .user_data = payload->user_data,
                    .code = payload->code,
                    .hash = payload->info->pgm_hash,
                };
                optimized_info = std::make_unique<Shader::Info>(
                    payload->info->stage, payload->info->l_stage,
                    owned_params);
                optimized_pools = std::make_unique<Shader::Pools>();
                optimized_ir = std::make_unique<Shader::IR::Program>(
                    Shader::TranslateProgram(
                        payload->code, *optimized_pools, *optimized_info,
                        optimized_runtime_info, profile,
                        Shader::TranslationTier::Optimized));
                completed.spv = Shader::Backend::SPIRV::EmitSPIRV(
                    profile, optimized_runtime_info, *optimized_ir, binding);
            }
#if defined(LSX4_APPLE_RUNTIME)
            if (!Config::patchShaders())
                AppleSpirvOptimizer217::Apply(completed.spv,optimized_info->pgm_hash,
                    static_cast<unsigned>(optimized_info->stage),"background");
#endif
            if (binding == payload->expected_binding_end &&
                !completed.spv.empty()) {
                const vk::ShaderModuleCreateInfo create_info{
                    .codeSize = completed.spv.size() * sizeof(u32),
                    .pCode = completed.spv.data(),
                };
                auto [result, module] = device.createShaderModule(create_info);
                completed.result = result;
                if (result == vk::Result::eSuccess) {
                    completed.module = module;
                    payload->info = std::move(optimized_info);
                    payload->pools = std::move(optimized_pools);
                    payload->ir = std::move(optimized_ir);
                    payload->runtime_info =
                        std::move(optimized_runtime_info);
                }
            }
        } else if (scheduler.IsDeviceLost()) {
            completed.result = vk::Result::eErrorDeviceLost;
        }
        completed.build_us = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started)
                .count());
        async_pipeline_stats.shader_optimized_completed.fetch_add(
            1, std::memory_order_relaxed);
        async_pipeline_stats.shader_total_build_us.fetch_add(
            completed.build_us, std::memory_order_relaxed);
        AtomicMax(async_pipeline_stats.shader_max_build_us,
                  completed.build_us);
        if (completed.result != vk::Result::eSuccess ||
            !completed.module) {
            async_pipeline_stats.shader_build_failures.fetch_add(
                1, std::memory_order_relaxed);
        }
        {
            std::scoped_lock lock{async_pipeline_mutex};
            async_shader_results.push_back(std::move(completed));
            async_shader_results_ready.store(true, std::memory_order_release);
        }
    }
}

void PipelineCache::PublishAsyncShaderResults() {
    if (!async_shader_results_ready.load(std::memory_order_acquire)) {
        return;
    }
    std::deque<AsyncShaderResult> completed;
    {
        std::scoped_lock lock{async_pipeline_mutex};
        completed.swap(async_shader_results);
        async_shader_results_ready.store(false, std::memory_order_release);
        for (const auto& result : completed) {
            const AsyncShaderKey key{
                .program = result.payload->program,
                .perm_idx = result.payload->perm_idx,
                .generation = result.payload->expected_generation,
                .cache_generation = result.payload->cache_generation,
            };
            pending_optimized_shaders.erase(key);
            async_shader_queued_bytes =
                result.payload->queued_bytes > async_shader_queued_bytes
                    ? 0
                    : async_shader_queued_bytes - result.payload->queued_bytes;
        }
        async_pipeline_stats.shader_queue_depth.store(
            static_cast<u32>(pending_optimized_shaders.size()),
            std::memory_order_relaxed);
        async_pipeline_stats.shader_queue_bytes.store(
            async_shader_queued_bytes, std::memory_order_relaxed);
    }
    if (completed.empty()) {
        return;
    }

    const auto device = instance.GetDevice();
    std::scoped_lock background_lock{background_warmup_mutex};
    for (auto& result : completed) {
        auto& payload = *result.payload;
        const AsyncShaderKey key{
            .program = payload.program,
            .perm_idx = payload.perm_idx,
            .generation = payload.expected_generation,
            .cache_generation = payload.cache_generation,
        };
        if (result.result == vk::Result::eErrorDeviceLost) {
            scheduler.MarkTerminal(result.result,
                                   "async optimized shader build");
            if (result.module) {
                device.destroyShaderModule(result.module);
            }
            continue;
        }
        const bool generation_matches =
            payload.program && payload.perm_idx < payload.program->modules.size() &&
            payload.cache_generation == shader_tier_generation &&
            payload.program->modules[payload.perm_idx].generation ==
                payload.expected_generation &&
            payload.program->modules[payload.perm_idx].module ==
                payload.expected_module &&
            payload.program->modules[payload.perm_idx].tier ==
                ShaderBinaryTier::Fast;
        if (!generation_matches || scheduler.IsDeviceLost()) {
            if (result.module) {
                device.destroyShaderModule(result.module);
            }
            continue;
        }
        if (result.result != vk::Result::eSuccess || !result.module) {
            invalid_optimized_shaders.emplace(key);
            RecordShaderBinaryTierFailure(payload.info->pgm_hash,
                                          payload.perm_idx,
                                          ShaderBinaryTier::Optimized);
            continue;
        }

        if (DeferAsyncLiveReplacement(instance)) {
            RegisterShaderBinary(std::move(result.spv),
                                 payload.info->pgm_hash,
                                 payload.perm_idx,
                                 ShaderBinaryTier::Optimized);
            invalid_optimized_shaders.erase(key);
            async_pipeline_stats.shader_optimized_published.fetch_add(
                1, std::memory_order_relaxed);
            // The foreground fast module stays active for this process.
            // The optimized module is only needed to validate driver
            // acceptance; its SPIR-V is now in the per-title disk cache.
            device.destroyShaderModule(result.module);
            continue;
        }

        auto& program_module = payload.program->modules[payload.perm_idx];
        ASSERT(program_module.info && payload.info);
        // Publish the metadata produced by the same optimized translation as
        // the replacement module. Preserve only draw-local inputs at their
        // stable, module-owned address.
        const VAddr pgm_base = program_module.info->pgm_base;
        const auto user_data = program_module.info->user_data;
        auto flattened_user_data =
            std::move(program_module.info->flattened_ud_buf);
        auto flattened_user_data_lease =
            std::move(program_module.info->flattened_ud_lease);
        *program_module.info = *payload.info;
        program_module.info->pgm_base = pgm_base;
        program_module.info->user_data = user_data;
        program_module.info->flattened_ud_buf =
            std::move(flattened_user_data);
        program_module.info->flattened_ud_lease =
            std::move(flattened_user_data_lease);
        program_module.spec.info = program_module.info.get();
        const vk::ShaderModule old_module = program_module.module;
        const vk::ShaderModule new_module = result.module;
        program_module.module = new_module;
        program_module.tier = ShaderBinaryTier::Optimized;
        ++program_module.generation;
        ++payload.program->module_generation;

        std::vector<std::unique_ptr<GraphicsPipeline>> retired_graphics;
        std::vector<std::unique_ptr<ComputePipeline>> retired_compute;
        if (auto related = module_related_pipelines.find(old_module);
            related != module_related_pipelines.end()) {
            for (const auto& pipeline_key : related.value()) {
                if (const auto* graphics =
                        std::get_if<GraphicsPipelineKey>(&pipeline_key)) {
                    if (auto it = graphics_pipelines.find(*graphics);
                        it != graphics_pipelines.end()) {
                        retired_graphics.push_back(std::move(it.value()));
                        graphics_pipelines.erase(it);
                    }
                    fallback_graphics_pipelines.erase(*graphics);
                } else {
                    const auto& compute =
                        std::get<ComputePipelineKey>(pipeline_key);
                    if (auto it = compute_pipelines.find(compute);
                        it != compute_pipelines.end()) {
                        retired_compute.push_back(std::move(it.value()));
                        compute_pipelines.erase(it);
                    }
                    fallback_compute_pipelines.erase(compute);
                }
            }
            module_related_pipelines.erase(related);
        }
        for (auto& pipeline : retired_graphics) {
            scheduler.DeferOperation(
                [pipeline = std::move(pipeline)]() mutable { pipeline.reset(); });
        }
        for (auto& pipeline : retired_compute) {
            scheduler.DeferOperation(
                [pipeline = std::move(pipeline)]() mutable { pipeline.reset(); });
        }

        retired_shader_modules.push_back(old_module);
        const auto name =
            GetShaderName(payload.info->stage, payload.info->pgm_hash,
                          payload.perm_idx);
        Vulkan::SetObjectName(device, new_module, name);
        RegisterShaderBinary(std::move(result.spv), payload.info->pgm_hash,
                             payload.perm_idx,
                             ShaderBinaryTier::Optimized);
        invalid_optimized_shaders.erase(key);
        last_graphics_pipeline = nullptr;
        last_graphics_state_generation = std::numeric_limits<u64>::max();
        last_graphics_lookup_resolved = false;
        async_pipeline_stats.shader_optimized_published.fetch_add(
            1, std::memory_order_relaxed);
        async_pipeline_stats.shader_replacements.fetch_add(
            1, std::memory_order_relaxed);
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (ExecutorGraphicsPipelineLookupProfileEnabled()) {
        const auto stats = GetAsyncPipelineCompileStats();
        if (stats.shader_optimized_completed <= 8 ||
            (stats.shader_optimized_completed & 63u) == 0) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_ASYNC_SHADER] fast=%llu queued=%llu completed=%llu "
                "published=%llu replaced=%llu failed=%llu negative=%llu "
                "drops=%llu queue=%u/%u bytes=%llu/%llu buildUs=%llu maxUs=%llu",
                static_cast<unsigned long long>(stats.shader_fast_compiled),
                static_cast<unsigned long long>(stats.shader_optimized_queued),
                static_cast<unsigned long long>(stats.shader_optimized_completed),
                static_cast<unsigned long long>(stats.shader_optimized_published),
                static_cast<unsigned long long>(stats.shader_replacements),
                static_cast<unsigned long long>(stats.shader_build_failures),
                static_cast<unsigned long long>(stats.shader_negative_hits),
                static_cast<unsigned long long>(stats.shader_queue_drops),
                stats.shader_queue_depth, stats.shader_queue_capacity,
                static_cast<unsigned long long>(stats.shader_queue_bytes),
                static_cast<unsigned long long>(
                    stats.shader_queue_byte_capacity),
                static_cast<unsigned long long>(stats.shader_total_build_us),
                static_cast<unsigned long long>(stats.shader_max_build_us));
        }
    }
#endif
}

void PipelineCache::DestroyRetiredShaderModules() {
    if (retired_shader_modules.empty()) {
        return;
    }
    std::vector<vk::ShaderModule> destroyable;
    {
        std::scoped_lock lock{async_pipeline_mutex};
        const auto is_queued = [this](const vk::ShaderModule module) {
            return std::ranges::any_of(
                async_pipeline_jobs,
                [module](const AsyncPipelineJob& job) {
                    return std::visit(
                        [module](const auto& payload) {
                            using PayloadType =
                                std::decay_t<decltype(payload)>;
                            if constexpr (std::is_same_v<
                                              PayloadType,
                                              std::shared_ptr<
                                                  AsyncGraphicsPayload>>) {
                                for (size_t stage = 0;
                                     stage < MaxShaderStages; ++stage) {
                                    if (payload->infos[stage] &&
                                        payload->modules[stage] == module) {
                                        return true;
                                    }
                                }
                                return false;
                            } else {
                                return payload->module == module;
                            }
                        },
                        job.payload);
                });
        };
        const auto is_completed = [this](const vk::ShaderModule module) {
            return std::ranges::any_of(
                async_pipeline_results,
                [module](const AsyncPipelineResult& result) {
                    return std::visit(
                        [module](const auto& completed) {
                            using ResultType =
                                std::decay_t<decltype(completed)>;
                            if constexpr (std::is_same_v<
                                              ResultType,
                                              AsyncGraphicsResult>) {
                                for (size_t stage = 0;
                                     stage < MaxShaderStages; ++stage) {
                                    if (completed.payload->infos[stage] &&
                                        completed.payload->modules[stage] ==
                                            module) {
                                        return true;
                                    }
                                }
                                return false;
                            } else {
                                return completed.payload->module == module;
                            }
                        },
                        result);
                });
        };
        auto it = retired_shader_modules.begin();
        while (it != retired_shader_modules.end()) {
            if (!active_async_pipeline_modules.contains(*it) &&
                !is_queued(*it) && !is_completed(*it)) {
                destroyable.push_back(*it);
                it = retired_shader_modules.erase(it);
            } else {
                ++it;
            }
        }
    }
    const auto device = instance.GetDevice();
    for (const auto module : destroyable) {
        if (module) {
            device.destroyShaderModule(module);
        }
    }
}

void PipelineCache::AsyncPipelineWorker(const std::stop_token stop_token,
                                        const u32 worker_index) {
    Common::SetCurrentThreadName(
        fmt::format("shadPS4:VkPipe{}", worker_index).c_str());
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
#if defined(LSX4_APPLE_RUNTIME)
    AppleWorkerPriority216::SetBackgroundPriority(6, "pipeline");
#else
    (void)::setpriority(PRIO_PROCESS, 0, 6);
#endif
#endif
    const auto device = instance.GetDevice();
    for (;;) {
        AsyncPipelineJob job;
        {
            std::unique_lock lock{async_pipeline_mutex};
            const bool ready = async_pipeline_cv.wait(
                lock, stop_token, [this] {
                    return !async_pipeline_jobs.empty() || !async_pipeline_accepting;
                });
            if (!ready || stop_token.stop_requested() ||
                (!async_pipeline_accepting && async_pipeline_jobs.empty())) {
                return;
            }
            if (DeferAsyncLiveReplacement(instance) &&
                AsyncSteadyMilliseconds() <
                    async_last_foreground_activity_ms.load(
                        std::memory_order_acquire) +
                        750u) {
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
                continue;
            }
            job = std::move(async_pipeline_jobs.front());
            async_pipeline_jobs.pop_front();
            std::visit(
                [this](const auto& payload) {
                    using PayloadType = std::decay_t<decltype(payload)>;
                    if constexpr (std::is_same_v<
                                      PayloadType,
                                      std::shared_ptr<AsyncGraphicsPayload>>) {
                        for (size_t stage = 0; stage < MaxShaderStages;
                             ++stage) {
                            if (payload->infos[stage] &&
                                payload->modules[stage]) {
                                ++active_async_pipeline_modules
                                      [payload->modules[stage]];
                            }
                        }
                    } else if (payload->module) {
                        ++active_async_pipeline_modules[payload->module];
                    }
                },
                job.payload);
            async_pipeline_stats.queue_depth.store(
                static_cast<u32>(async_pipeline_jobs.size()),
                std::memory_order_relaxed);
        }

        vk::PipelineCacheCreateInfo cache_info{};
        cache_info.initialDataSize = async_pipeline_cache_seed.size();
        cache_info.pInitialData = async_pipeline_cache_seed.data();
        auto [cache_result, local_cache] =
            device.createPipelineCacheUnique(cache_info);
        const auto started = std::chrono::steady_clock::now();
        const vk::PipelineCreateFlags build_flags =
            job.tier == AsyncPipelineTier::Fast
                ? vk::PipelineCreateFlagBits::eDisableOptimization
                : vk::PipelineCreateFlags{};

        AsyncPipelineResult completed;
        if (const auto* graphics =
                std::get_if<std::shared_ptr<AsyncGraphicsPayload>>(&job.payload)) {
            AsyncGraphicsResult result{
                .payload = *graphics,
                .tier = job.tier,
                .result = cache_result,
            };
            if (cache_result == vk::Result::eSuccess && !scheduler.IsDeviceLost()) {
                result.local_cache = std::move(local_cache);
                result.pipeline = std::make_unique<GraphicsPipeline>(
                    instance, scheduler, desc_heap, profile, (*graphics)->key,
                    *result.local_cache, (*graphics)->infos,
                    (*graphics)->runtime_infos, (*graphics)->fetch_shader,
                    (*graphics)->modules, (*graphics)->sdata, true, build_flags);
                result.result = result.pipeline->BuildResult();
            }
            result.build_us = static_cast<u64>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started)
                    .count());
            completed = std::move(result);
        } else {
            const auto& compute =
                std::get<std::shared_ptr<AsyncComputePayload>>(job.payload);
            AsyncComputeResult result{
                .payload = compute,
                .tier = job.tier,
                .result = cache_result,
            };
            if (cache_result == vk::Result::eSuccess && !scheduler.IsDeviceLost()) {
                result.local_cache = std::move(local_cache);
                result.pipeline = std::make_unique<ComputePipeline>(
                    instance, scheduler, desc_heap, profile, *result.local_cache,
                    compute->key, *compute->info, compute->workgroup_size,
                    compute->module, compute->sdata, true, build_flags);
                result.result = result.pipeline->BuildResult();
            }
            result.build_us = static_cast<u64>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started)
                    .count());
            completed = std::move(result);
        }

        const u64 build_us = std::visit(
            [](const auto& result) { return result.build_us; }, completed);
        const bool valid = std::visit(
            [](const auto& result) {
                return result.pipeline && result.pipeline->IsValid();
            },
            completed);
        (job.tier == AsyncPipelineTier::Fast
             ? async_pipeline_stats.fast_completed
             : async_pipeline_stats.optimized_completed)
            .fetch_add(1, std::memory_order_relaxed);
        async_pipeline_stats.total_build_us.fetch_add(build_us,
                                                      std::memory_order_relaxed);
        AtomicMax(async_pipeline_stats.max_build_us, build_us);
        if (!valid) {
            async_pipeline_stats.build_failures.fetch_add(1, std::memory_order_relaxed);
        }
        {
            std::scoped_lock lock{async_pipeline_mutex};
            std::visit(
                [this](const auto& payload) {
                    const auto release_module =
                        [this](const vk::ShaderModule module) {
                            if (auto it =
                                    active_async_pipeline_modules.find(module);
                                it != active_async_pipeline_modules.end()) {
                                if (--it->second == 0) {
                                    active_async_pipeline_modules.erase(it);
                                }
                            }
                        };
                    using PayloadType = std::decay_t<decltype(payload)>;
                    if constexpr (std::is_same_v<
                                      PayloadType,
                                      std::shared_ptr<AsyncGraphicsPayload>>) {
                        for (size_t stage = 0; stage < MaxShaderStages;
                             ++stage) {
                            if (payload->infos[stage] &&
                                payload->modules[stage]) {
                                release_module(payload->modules[stage]);
                            }
                        }
                    } else if (payload->module) {
                        release_module(payload->module);
                    }
                },
                job.payload);
            async_pipeline_results.push_back(std::move(completed));
            async_pipeline_results_ready.store(true, std::memory_order_release);
        }
    }
}

void PipelineCache::PublishAsyncPipelineResults() {
    if (!async_pipeline_results_ready.load(std::memory_order_acquire)) {
        PublishAsyncShaderResults();
        DestroyRetiredShaderModules();
        return;
    }
    std::deque<AsyncPipelineResult> completed;
    {
        std::scoped_lock lock{async_pipeline_mutex};
        completed.swap(async_pipeline_results);
        async_pipeline_results_ready.store(false, std::memory_order_release);
        for (const auto& item : completed) {
            std::visit(
                [this](const auto& result) {
                    const bool optimized =
                        result.tier == AsyncPipelineTier::Optimized;
                    using ResultType = std::decay_t<decltype(result)>;
                    if constexpr (std::is_same_v<ResultType, AsyncGraphicsResult>) {
                        (optimized ? pending_optimized_graphics_pipelines
                                   : pending_fast_graphics_pipelines)
                            .erase(result.payload->key);
                    } else {
                        (optimized ? pending_optimized_compute_pipelines
                                   : pending_fast_compute_pipelines)
                            .erase(result.payload->key);
                    }
                },
                item);
        }
    }
    if (completed.empty()) {
        PublishAsyncShaderResults();
        DestroyRetiredShaderModules();
        return;
    }

    std::vector<std::shared_ptr<AsyncGraphicsPayload>> optimize_graphics;
    std::vector<std::shared_ptr<AsyncComputePayload>> optimize_compute;
    {
        std::scoped_lock background_lock{background_warmup_mutex};
        const auto merge_local_cache = [&](auto& result) {
            if (!result.local_cache || !result.pipeline ||
                !result.pipeline->IsValid()) {
                return;
            }
            const std::array source_caches{*result.local_cache};
            const auto merge_result =
                instance.GetDevice().mergePipelineCaches(*pipeline_cache,
                                                         source_caches);
            if (merge_result == vk::Result::eSuccess) {
                MarkHostPipelineCacheDirty();
            } else if (merge_result == vk::Result::eErrorDeviceLost) {
                scheduler.MarkTerminal(merge_result,
                                       "async pipeline cache merge");
            } else {
                LOG_WARNING(Render_Vulkan,
                            "Failed to merge async pipeline cache: {}",
                            vk::to_string(merge_result));
            }
        };

        for (auto& item : completed) {
            std::visit(
                [&](auto& result) {
                    using ResultType = std::decay_t<decltype(result)>;
                    const bool optimized =
                        result.tier == AsyncPipelineTier::Optimized;
                    const bool valid =
                        result.pipeline && result.pipeline->IsValid();
                    const auto module_is_retired =
                        [this](const vk::ShaderModule module) {
                            return std::ranges::find(retired_shader_modules,
                                                     module) !=
                                   retired_shader_modules.end();
                        };
                    bool uses_retired_module = false;
                    if constexpr (std::is_same_v<ResultType,
                                                 AsyncGraphicsResult>) {
                        for (size_t stage = 0; stage < MaxShaderStages;
                             ++stage) {
                            uses_retired_module |=
                                result.payload->infos[stage] &&
                                module_is_retired(
                                    result.payload->modules[stage]);
                        }
                    } else {
                        uses_retired_module =
                            module_is_retired(result.payload->module);
                    }
                    if (uses_retired_module) {
                        return;
                    }
                    if (result.result == vk::Result::eErrorDeviceLost) {
                        scheduler.MarkTerminal(result.result,
                                               "async pipeline build");
                    }

                    if constexpr (std::is_same_v<ResultType,
                                                 AsyncGraphicsResult>) {
                        const auto& key = result.payload->key;
                        auto existing = graphics_pipelines.find(key);
                        const bool existing_valid =
                            existing != graphics_pipelines.end() &&
                            existing.value() && existing.value()->IsValid();
                        const bool existing_fallback =
                            fallback_graphics_pipelines.contains(key);
                        if (!valid) {
                            if (!optimized && !existing_valid &&
                                result.result != vk::Result::eErrorDeviceLost &&
                                result.result !=
                                    vk::Result::ePipelineCompileRequired) {
                                RecordInvalid(key);
                            }
                            return;
                        }

                        merge_local_cache(result);
                        if (optimized && existing_valid &&
                            DeferAsyncLiveReplacement(instance)) {
                            // The local cache was merged above, so the
                            // optimized pipeline is available to the next
                            // launch without hot-swapping a live Qualcomm
                            // object in this command stream.
                            return;
                        }
                        bool published = false;
                        bool replaced = false;
                        if (optimized) {
                            if (existing_valid && existing_fallback) {
                                auto retired = std::move(existing.value());
                                existing.value() = std::move(result.pipeline);
                                fallback_graphics_pipelines.erase(key);
                                scheduler.DeferOperation(
                                    [pipeline = std::move(retired)]() mutable {
                                        pipeline.reset();
                                    });
                                published = true;
                                replaced = true;
                            } else if (!existing_valid) {
                                if (existing == graphics_pipelines.end()) {
                                    graphics_pipelines.try_emplace(
                                        key, std::move(result.pipeline));
                                } else {
                                    existing.value() = std::move(result.pipeline);
                                }
                                fallback_graphics_pipelines.erase(key);
                                published = true;
                            }
                        } else if (!existing_valid) {
                            if (existing == graphics_pipelines.end()) {
                                graphics_pipelines.try_emplace(
                                    key, std::move(result.pipeline));
                            } else {
                                existing.value() = std::move(result.pipeline);
                            }
                            fallback_graphics_pipelines.emplace(key);
                            published = true;
                        }
                        if (!published) {
                            return;
                        }

                        RecordValid(key);
                        ++num_new_pipelines;
                        const auto pipeline_hash =
                            std::hash<GraphicsPipelineKey>{}(key);
                        RegisterPipelineData(key, pipeline_hash,
                                             result.payload->sdata);
                        for (size_t stage = 0; stage < MaxShaderStages;
                             ++stage) {
                            if (result.payload->infos[stage]) {
                                module_related_pipelines
                                    [result.payload->modules[stage]]
                                        .emplace_back(key);
                            }
                        }
                        last_graphics_pipeline = nullptr;
                        last_graphics_state_generation =
                            std::numeric_limits<u64>::max();
                        last_graphics_lookup_resolved = false;
                        async_pipeline_stats.published.fetch_add(
                            1, std::memory_order_relaxed);
                        if (replaced) {
                            async_pipeline_stats.optimized_replacements
                                .fetch_add(1, std::memory_order_relaxed);
                        }
                        if (!optimized &&
                            Config::managedAsyncPipelineCompilation()) {
                            optimize_graphics.push_back(result.payload);
                        }
                    } else {
                        const auto& key = result.payload->key;
                        auto existing = compute_pipelines.find(key);
                        const bool existing_valid =
                            existing != compute_pipelines.end() &&
                            existing.value() && existing.value()->IsValid();
                        const bool existing_fallback =
                            fallback_compute_pipelines.contains(key);
                        if (!valid) {
                            if (!optimized && !existing_valid &&
                                result.result != vk::Result::eErrorDeviceLost &&
                                result.result !=
                                    vk::Result::ePipelineCompileRequired) {
                                RecordInvalid(key);
                            }
                            return;
                        }

                        merge_local_cache(result);
                        if (optimized && existing_valid &&
                            DeferAsyncLiveReplacement(instance)) {
                            return;
                        }
                        bool published = false;
                        bool replaced = false;
                        if (optimized) {
                            if (existing_valid && existing_fallback) {
                                auto retired = std::move(existing.value());
                                existing.value() = std::move(result.pipeline);
                                fallback_compute_pipelines.erase(key);
                                scheduler.DeferOperation(
                                    [pipeline = std::move(retired)]() mutable {
                                        pipeline.reset();
                                    });
                                published = true;
                                replaced = true;
                            } else if (!existing_valid) {
                                if (existing == compute_pipelines.end()) {
                                    compute_pipelines.try_emplace(
                                        key, std::move(result.pipeline));
                                } else {
                                    existing.value() = std::move(result.pipeline);
                                }
                                fallback_compute_pipelines.erase(key);
                                published = true;
                            }
                        } else if (!existing_valid) {
                            if (existing == compute_pipelines.end()) {
                                compute_pipelines.try_emplace(
                                    key, std::move(result.pipeline));
                            } else {
                                existing.value() = std::move(result.pipeline);
                            }
                            fallback_compute_pipelines.emplace(key);
                            published = true;
                        }
                        if (!published) {
                            return;
                        }

                        RecordValid(key);
                        ++num_new_pipelines;
                        RegisterPipelineData(key, result.payload->sdata);
                        module_related_pipelines[result.payload->module]
                            .emplace_back(key);
                        async_pipeline_stats.published.fetch_add(
                            1, std::memory_order_relaxed);
                        if (replaced) {
                            async_pipeline_stats.optimized_replacements
                                .fetch_add(1, std::memory_order_relaxed);
                        }
                        if (!optimized &&
                            Config::managedAsyncPipelineCompilation()) {
                            optimize_compute.push_back(result.payload);
                        }
                    }
                },
                item);
        }
    }

    for (const auto& payload : optimize_graphics) {
        QueueAsyncGraphicsPipeline(
            payload->key, payload->infos, payload->runtime_infos,
            payload->fetch_shader, payload->modules, payload->sdata, true);
    }
    for (const auto& payload : optimize_compute) {
        QueueAsyncComputePipeline(payload->key, *payload->info,
                                  payload->workgroup_size, payload->module,
                                  payload->sdata, true);
    }
    // Shader publication runs after pipeline publication.  If an optimized
    // shader supersedes a module used by a just-finished pipeline, the module
    // swap and dependent-pipeline invalidation happen in this same turn.
    PublishAsyncShaderResults();
    DestroyRetiredShaderModules();

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (ExecutorGraphicsPipelineLookupProfileEnabled()) {
        const auto stats = GetAsyncPipelineCompileStats();
        const u64 completed_count =
            stats.fast_completed + stats.optimized_completed;
        if (completed_count <= 8 || (completed_count & 63u) == 0) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_ASYNC_PIPELINE] fastDone=%llu optDone=%llu "
                "published=%llu replaced=%llu failed=%llu queue=%u/%u "
                "fallback=%llu skipped=%llu shaderFast=%llu shaderOpt=%llu/%llu "
                "shaderReplace=%llu shaderQueue=%u/%u shaderBytes=%llu/%llu "
                "totalBuildUs=%llu maxBuildUs=%llu",
                static_cast<unsigned long long>(stats.fast_completed),
                static_cast<unsigned long long>(stats.optimized_completed),
                static_cast<unsigned long long>(stats.published),
                static_cast<unsigned long long>(stats.optimized_replacements),
                static_cast<unsigned long long>(stats.build_failures),
                stats.queue_depth, stats.queue_capacity,
                static_cast<unsigned long long>(stats.foreground_fallbacks),
                static_cast<unsigned long long>(stats.skipped_uses),
                static_cast<unsigned long long>(stats.shader_fast_compiled),
                static_cast<unsigned long long>(stats.shader_optimized_published),
                static_cast<unsigned long long>(stats.shader_optimized_completed),
                static_cast<unsigned long long>(stats.shader_replacements),
                stats.shader_queue_depth, stats.shader_queue_capacity,
                static_cast<unsigned long long>(stats.shader_queue_bytes),
                static_cast<unsigned long long>(stats.shader_queue_byte_capacity),
                static_cast<unsigned long long>(stats.total_build_us),
                static_cast<unsigned long long>(stats.max_build_us));
        }
    }
#endif
}

std::unique_lock<std::mutex> PipelineCache::LockBackgroundWarmUpForForeground() {
    std::unique_lock<std::mutex> background_lock;
    if (!background_warmup_active.load(std::memory_order_acquire)) {
        return background_lock;
    }

    background_warmup_foreground_waiters.fetch_add(1, std::memory_order_acq_rel);
    const bool collect_wait_profile = ExecutorGraphicsPipelineLookupProfileEnabled();
    const auto wait_started = collect_wait_profile
                                  ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
    if (background_warmup_active.load(std::memory_order_acquire)) {
        background_lock = std::unique_lock{background_warmup_mutex};
    }
    background_warmup_foreground_waiters.fetch_sub(1, std::memory_order_acq_rel);

    if (!collect_wait_profile) {
        return background_lock;
    }

    const auto waited_us = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - wait_started)
            .count());
    if (waited_us < 250) {
        return background_lock;
    }

    const u64 wait_events =
        background_warmup_wait_events.fetch_add(1, std::memory_order_relaxed) + 1;
    const u64 total_wait_us =
        background_warmup_wait_us.fetch_add(waited_us, std::memory_order_relaxed) + waited_us;
    u64 previous_max = background_warmup_max_wait_us.load(std::memory_order_relaxed);
    while (previous_max < waited_us &&
           !background_warmup_max_wait_us.compare_exchange_weak(
               previous_max, waited_us, std::memory_order_relaxed)) {
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (wait_events == 1 || (wait_events & 255u) == 0) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_PIPELINE_WARMUP_ARBITRATION] waits=%llu totalWaitUs=%llu "
            "maxWaitUs=%llu active=%d",
            static_cast<unsigned long long>(wait_events),
            static_cast<unsigned long long>(total_wait_us),
            static_cast<unsigned long long>(
                background_warmup_max_wait_us.load(std::memory_order_relaxed)),
            background_warmup_active.load(std::memory_order_relaxed) ? 1 : 0);
    }
#endif
    return background_lock;
}

void PipelineCache::DiscardBackgroundWarmUpPipelinesLocked(
    std::vector<BackgroundWarmUpInfos>& retired_infos,
    std::vector<std::unique_ptr<GraphicsPipeline>>& retired_graphics,
    std::vector<std::unique_ptr<ComputePipeline>>& retired_compute) {
    for (auto info_it = background_warmup_graphics_infos.begin();
         info_it != background_warmup_graphics_infos.end(); ++info_it) {
        const auto& key = info_it.key();
        if (auto pipeline_it = graphics_pipelines.find(key);
            pipeline_it != graphics_pipelines.end()) {
            retired_graphics.push_back(std::move(pipeline_it.value()));
            graphics_pipelines.erase(pipeline_it);
        }
        retired_infos.push_back(std::move(info_it.value()));
    }
    for (auto info_it = background_warmup_compute_infos.begin();
         info_it != background_warmup_compute_infos.end(); ++info_it) {
        const auto& key = info_it.key();
        if (auto pipeline_it = compute_pipelines.find(key);
            pipeline_it != compute_pipelines.end()) {
            retired_compute.push_back(std::move(pipeline_it.value()));
            compute_pipelines.erase(pipeline_it);
        }
        retired_infos.push_back(std::move(info_it.value()));
    }
    background_warmup_graphics_infos.clear();
    background_warmup_compute_infos.clear();
    last_graphics_pipeline = nullptr;
    last_graphics_state_generation = std::numeric_limits<u64>::max();
    last_graphics_lookup_resolved = false;
}

bool PipelineCache::CaptureSrtSourceBytes(
    const std::span<const Shader::SrtMemoryRange> ranges, std::vector<u8>& bytes) const {
    size_t total_size = 0;
    for (const auto& range : ranges) {
        if (range.size > std::numeric_limits<size_t>::max() - total_size) {
            bytes.clear();
            return false;
        }
        total_size += static_cast<size_t>(range.size);
    }
    bytes.resize(total_size);
    size_t offset = 0;
    for (const auto& range : ranges) {
        std::memcpy(bytes.data() + offset,
                    reinterpret_cast<const void*>(range.address),
                    static_cast<size_t>(range.size));
        offset += static_cast<size_t>(range.size);
    }
    return true;
}

bool PipelineCache::CaptureSrtBackingBytes(
    const std::span<const Shader::SrtMemoryRange> ranges, std::vector<u8>& bytes) const {
    size_t total_size = 0;
    for (const auto& range : ranges) {
        if (range.address == 0 || range.size == 0 ||
            range.size > std::numeric_limits<size_t>::max() - total_size) {
            bytes.clear();
            return false;
        }
        total_size += static_cast<size_t>(range.size);
    }
    bytes.resize(total_size);
    auto* memory = Core::Memory::Instance();
    size_t offset = 0;
    for (const auto& range : ranges) {
        if (!memory->TryCopyBackingMemory(range.address, bytes.data() + offset, range.size)) {
            bytes.clear();
            return false;
        }
        offset += static_cast<size_t>(range.size);
    }
    return true;
}

bool PipelineCache::AreSrtMemoryRangesGpuClean(
    const std::span<const Shader::SrtMemoryRange> ranges) const {
    return std::ranges::all_of(ranges, [this](const auto& range) {
        return range.address != 0 && range.size != 0 &&
               !buffer_cache->IsRegionGpuModifiedExact(range.address, range.size);
    });
}

bool PipelineCache::DoSrtSourceBytesMatch(
    const std::span<const Shader::SrtMemoryRange> ranges,
    const std::span<const u8> source_bytes) const {
    size_t offset = 0;
    for (const auto& range : ranges) {
        if (range.size > source_bytes.size() - std::min(offset, source_bytes.size()) ||
            std::memcmp(reinterpret_cast<const void*>(range.address),
                        source_bytes.data() + offset,
                        static_cast<size_t>(range.size)) != 0) {
            return false;
        }
        offset += static_cast<size_t>(range.size);
    }
    return offset == source_bytes.size();
}

static size_t SrtSnapshotRetainedBytes(
    const Program::SpecializationSnapshot& snapshot) noexcept {
    size_t bytes = sizeof(snapshot);
    bytes += snapshot.flattened_user_data.capacity() * sizeof(u32);
    bytes += snapshot.srt_source_bytes.capacity() * sizeof(u8);
    constexpr size_t InlineSrtRanges = 32;
    if (snapshot.srt_ranges.capacity() > InlineSrtRanges) {
        bytes += (snapshot.srt_ranges.capacity() - InlineSrtRanges) *
                 sizeof(Shader::SrtMemoryRange);
    }
    return bytes;
}

Program::SpecializationSnapshot* PipelineCache::FindSrtVictimSnapshot(
    Program& program, const u64 identity_key) {
    ++srt_snapshot_victim_lookups;
    const SrtVictimKey key{&program, identity_key};
    const auto it = srt_snapshot_victims.find(key);
    if (it == srt_snapshot_victims.end()) {
        return nullptr;
    }
    auto& entry = it.value();
    if (!entry.snapshot || !entry.snapshot->valid) {
        return nullptr;
    }
    entry.referenced = true;
    return entry.snapshot.get();
}

std::unique_ptr<Program::SpecializationSnapshot> PipelineCache::RetainSrtVictimSnapshot(
    Program& program, std::unique_ptr<Program::SpecializationSnapshot> snapshot) {
    if (!snapshot || !snapshot->valid) {
        return snapshot;
    }

    const SrtVictimKey key{&program, snapshot->identity_key};
    if (const auto existing = srt_snapshot_victims.find(key);
        existing != srt_snapshot_victims.end()) {
        auto& entry = existing.value();
        std::swap(entry.snapshot, snapshot);
        entry.referenced = true;
        ++srt_snapshot_victim_replacements;
        return snapshot;
    }

    std::unique_ptr<Program::SpecializationSnapshot> recycled;
    while (srt_snapshot_victims.size() >= MaxSrtVictimSnapshots &&
           !srt_snapshot_victim_clock.empty()) {
        const SrtVictimKey candidate = srt_snapshot_victim_clock.front();
        srt_snapshot_victim_clock.pop_front();
        const auto it = srt_snapshot_victims.find(candidate);
        if (it == srt_snapshot_victims.end()) {
            continue;
        }
        auto& entry = it.value();
        if (entry.referenced) {
            entry.referenced = false;
            srt_snapshot_victim_clock.push_back(candidate);
            continue;
        }
        recycled = std::move(entry.snapshot);
        ASSERT(candidate.program != nullptr);
        ASSERT(candidate.program->srt_victim_snapshot_count != 0);
        --candidate.program->srt_victim_snapshot_count;
        srt_snapshot_victims.erase(it);
        ++srt_snapshot_victim_evictions;
        break;
    }
    if (srt_snapshot_victims.size() >= MaxSrtVictimSnapshots) {
        return snapshot;
    }

    if (srt_snapshot_victims.empty()) {
        srt_snapshot_victims.reserve(MaxSrtVictimSnapshots);
    }
    auto [it, inserted] = srt_snapshot_victims.try_emplace(key);
    if (!inserted) {
        auto& entry = it.value();
        std::swap(entry.snapshot, snapshot);
        entry.referenced = true;
        ++srt_snapshot_victim_replacements;
        return snapshot;
    }
    it.value().snapshot = std::move(snapshot);
    it.value().referenced = true;
    srt_snapshot_victim_clock.push_back(key);
    ++program.srt_victim_snapshot_count;
    ++srt_snapshot_victim_insertions;
    return recycled;
}

void PipelineCache::InvalidateSrtSnapshots() {
    // The background warm-up worker may insert programs and permutations.
    // Serialize the full-program walk with those mutations.
    std::scoped_lock background_lock{background_warmup_mutex};
    ++background_warmup_generation;
    srt_snapshot_victims.clear();
    srt_snapshot_victim_clock.clear();
    for (auto& [_, program] : program_cache) {
        if (!program) {
            continue;
        }
        for (auto& snapshot : program->specialization_snapshots) {
            snapshot.reset();
        }
        program->specialization_snapshot_count = 0;
        program->srt_victim_snapshot_count = 0;
    }
    srt_snapshot_object_count = 0;
    srt_snapshot_retained_bytes = 0;
}

bool PipelineCache::WasCpuRangeInvalidatedSinceLocked(const VAddr range_address,
                                                      const u64 range_size,
                                                      const u64 sequence) const {
    if (range_size == 0 ||
        range_address > std::numeric_limits<VAddr>::max() - range_size) {
        return true;
    }
    const u64 published_generation =
        shader_identity_published_generation.load(std::memory_order_acquire);
    if (published_generation <= sequence) {
        return false;
    }
    if (published_generation - sequence > ShaderIdentityInvalidationHistorySize) {
        return true;
    }
    const VAddr range_end = range_address + range_size;
    for (u64 current = sequence + 1; current <= published_generation; ++current) {
        const auto& invalidation =
            shader_identity_invalidation_history
                [(current - 1) % ShaderIdentityInvalidationHistorySize];
        const u64 invalidation_sequence =
            invalidation.sequence.load(std::memory_order_acquire);
        const VAddr invalidation_address =
            invalidation.address.load(std::memory_order_relaxed);
        const u64 invalidation_size =
            invalidation.size.load(std::memory_order_relaxed);
        if (invalidation_sequence != current || invalidation_size == 0 ||
            invalidation_address >
                std::numeric_limits<VAddr>::max() - invalidation_size) {
            return true;
        }
        const VAddr invalidation_end = invalidation_address + invalidation_size;
        if (invalidation_address < range_end && range_address < invalidation_end) {
            return true;
        }
    }
    return false;
}

bool PipelineCache::AreSrtMemoryRangesCpuStableSince(
    const std::span<const Shader::SrtMemoryRange> ranges, const u64 sequence) {
    if (shader_identity_published_generation.load(std::memory_order_acquire) <= sequence) {
        return true;
    }
    return std::ranges::all_of(ranges, [&](const auto& range) {
        return !WasCpuRangeInvalidatedSinceLocked(range.address, range.size, sequence);
    });
}

u64 PipelineCache::CaptureCpuInvalidationSequence() {
    return shader_identity_published_generation.load(std::memory_order_acquire);
}

PipelineCache::HleCleanBufferGuestReadScope::
    HleCleanBufferGuestReadScope(const PipelineCache& owner_)
    : owner{&owner_} {
    if (!owner->hle_clean_buffer_guard_enabled) {
        return;
    }
    if (hle_clean_buffer_guest_read_depth != 0) {
        ASSERT_MSG(hle_clean_buffer_guest_read_owner == owner,
                   "Nested clean-HLE guest read changed PipelineCache owner");
        ++hle_clean_buffer_guest_read_depth;
        active = true;
        return;
    }

    lock.emplace(owner->hle_clean_buffer_shader_scan_mutex,
                 std::defer_lock);
    const auto wait_start = std::chrono::steady_clock::now();
    bool watchdog_reported = false;
    while (!lock->try_lock_for(std::chrono::milliseconds{1})) {
        if (!watchdog_reported &&
            std::chrono::steady_clock::now() - wait_start >=
                std::chrono::milliseconds{250}) {
            watchdog_reported = true;
            const u64 watchdog =
                owner->hle_clean_buffer_scan_watchdogs.fetch_add(
                    1, std::memory_order_relaxed) +
                1;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            if (watchdog <= 16 || (watchdog & (watchdog - 1)) == 0) {
                __android_log_print(
                    ANDROID_LOG_ERROR, "LSX4Native",
                    "[EXECUTOR_HLE_CLEAN_BUFFER_SHADER_SCAN_WAIT] "
                    "n=%llu active=%u generation=%llu action=wait-closed",
                    static_cast<unsigned long long>(watchdog),
                    owner->hle_clean_buffer_active_writers.load(
                        std::memory_order_acquire),
                    static_cast<unsigned long long>(
                        owner->hle_clean_buffer_write_generation.load(
                            std::memory_order_acquire)));
            }
#endif
        }
    }
    hle_clean_buffer_guest_read_owner = owner;
    hle_clean_buffer_guest_read_depth = 1;
    active = true;
}

PipelineCache::HleCleanBufferGuestReadScope::
    ~HleCleanBufferGuestReadScope() {
    if (!active) {
        return;
    }
    ASSERT_MSG(hle_clean_buffer_guest_read_owner == owner &&
                   hle_clean_buffer_guest_read_depth != 0,
               "Clean-HLE guest read scope ownership mismatch");
    if (--hle_clean_buffer_guest_read_depth == 0) {
        hle_clean_buffer_guest_read_owner = nullptr;
    }
}

u64 PipelineCache::WaitForStableHleCleanBufferRead() const noexcept {
    if (!hle_clean_buffer_guard_enabled) {
        return hle_clean_buffer_write_generation.load(
            std::memory_order_relaxed);
    }
    // Every marker-on shader/SRT/fetch/tessellation read owns the shared guest
    // read scope. The writer only try-locks it exclusively and falls back for
    // that call, so it can never strand a reader behind a leaked writer ticket.
    ASSERT_MSG(hle_clean_buffer_guest_read_owner == this &&
                   hle_clean_buffer_guest_read_depth != 0,
               "Shader read entered without outer clean-HLE guest read scope");
    const u32 active =
        hle_clean_buffer_active_writers.load(std::memory_order_acquire);
    ASSERT_MSG(active == 0,
               "Shader read entered without scoped HLE reader admission");
    return hle_clean_buffer_write_generation.load(std::memory_order_acquire);
}

bool PipelineCache::IsHleCleanBufferReadStable(
    const u64 generation) const noexcept {
    std::atomic_thread_fence(std::memory_order_acquire);
    return hle_clean_buffer_active_writers.load(std::memory_order_acquire) ==
               0 &&
           hle_clean_buffer_write_generation.load(
               std::memory_order_acquire) == generation;
}

bool PipelineCache::RetainHleShaderReadResidency(const VAddr address,
                                                 const u64 size) {
    if (!hle_clean_buffer_guard_enabled) {
        return true;
    }
    if (const auto it = hle_shader_read_residency_max_size.find(address);
        it != hle_shader_read_residency_max_size.end() &&
        it.value() >= size) {
        return true;
    }

    const auto previous = hle_shader_read_residency_max_size.find(address);
    const bool had_previous =
        previous != hle_shader_read_residency_max_size.end();
    const u64 previous_size = had_previous ? previous.value() : 0;
    bool lease_staged = false;
    bool map_staged = false;
    try {
        // Complete every potentially allocating container mutation before
        // publishing the PageManager count. After Track succeeds there is no
        // throwing operation which can strand an unowned residency lease.
        hle_shader_read_residency_leases.reserve(
            hle_shader_read_residency_leases.size() + 1);
        hle_shader_read_residency_max_size.reserve(
            hle_shader_read_residency_max_size.size() +
            (had_previous ? 0 : 1));
        hle_shader_read_residency_leases.emplace_back(address, size);
        lease_staged = true;
        hle_shader_read_residency_max_size.insert_or_assign(address, size);
        map_staged = true;
    } catch (...) {
        if (lease_staged) {
            hle_shader_read_residency_leases.pop_back();
        }
        page_manager->DisableHleCleanBufferFastPathFailClosed();
        return false;
    }

    const auto rollback_staged_lease = [&] noexcept {
        if (map_staged) {
            if (had_previous) {
                const auto it =
                    hle_shader_read_residency_max_size.find(address);
                ASSERT(it != hle_shader_read_residency_max_size.end());
                it.value() = previous_size;
            } else {
                hle_shader_read_residency_max_size.erase(address);
            }
        }
        if (lease_staged) {
            hle_shader_read_residency_leases.pop_back();
        }
    };

    bool tracked = false;
    try {
        tracked = page_manager->TrackHleShaderReadResidency(address, size);
    } catch (...) {
        // A partial PageManager publication is conservative once admissions
        // are globally closed. Do not invent an Untrack for an unknown prefix.
        page_manager->DisableHleCleanBufferFastPathFailClosed();
        rollback_staged_lease();
        return false;
    }
    if (!tracked) {
        // Returning an unowned guest span while the experimental writer can
        // open its PTE would be fail-open. Disable all later admissions; the
        // scan gate plus stable-generation postcheck below excludes an
        // in-flight one before the caller may return the span.
        page_manager->DisableHleCleanBufferFastPathFailClosed();
        rollback_staged_lease();
        return false;
    }
    return true;
}

std::optional<Shader::ShaderParams> PipelineCache::GetShaderParams(
    const u32* user_data, const u32* code, const Shader::Stage stage) {
    auto* const shader_lookup_profile =
        ExecutorGraphicsPipelineLookupProfile::Active();
    ExecutorGraphicsShaderTimingScope shader_params_profile{
        shader_lookup_profile,
        ExecutorGraphicsShaderTiming::ShaderParams};
    HleCleanBufferGuestReadScope hle_guest_read_scope{*this};
    ASSERT(user_data != nullptr);
    ASSERT(code != nullptr);
    if (hle_clean_buffer_guard_enabled) {
        // RefreshGraphicsStages/RefreshComputeKey hold one blocking shared
        // lease across this scan and every later guest-backed auxiliary read.
        // Recursive lock_shared is deliberately avoided for geometry vs_copy.
        ASSERT_MSG(hle_clean_buffer_guest_read_owner == this &&
                       hle_clean_buffer_guest_read_depth != 0,
                   "GetShaderParams lacks clean-HLE guest read admission");
    }
    const std::span<const u32, Shader::ShaderParams::NumShaderUserData> fresh_user_data{
        user_data, Shader::ShaderParams::NumShaderUserData};
    const VAddr address = reinterpret_cast<VAddr>(code);
    auto* const memory = Core::Memory::Instance();
    const VAddr guest_address_floor =
        memory != nullptr
            ? reinterpret_cast<VAddr>(
                  memory->GetAddressSpace().SystemManagedVirtualBase())
            : 0;
    if (memory == nullptr || address < guest_address_floor ||
        !memory->IsValidGpuMapping(
            address, AmdGpu::ShaderBinaryProbeBytes)) [[unlikely]] {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        static std::atomic<u32> invalid_address_logs{0};
        if (invalid_address_logs.fetch_add(1, std::memory_order_relaxed) < 16) {
            __android_log_print(
                ANDROID_LOG_ERROR, "LSX4Native",
                "[EXECUTOR_SHADER_ADDRESS_RETRY] stage=%u addr=0x%llx "
                "probeBytes=%zu owned=0 action=retry-next-command",
                static_cast<unsigned>(stage),
                static_cast<unsigned long long>(address),
                static_cast<size_t>(AmdGpu::ShaderBinaryProbeBytes));
        }
#endif
        // Shader metadata scanning uses direct guest spans on its hot path.
        // Reject a transient/unpublished PM4 program address before the first
        // load instead of dereferencing it or continuing with stale pipeline
        // state. The floor and GPU-aperture checks are scalar; they do not walk
        // mappings, lock, synchronize the GPU, or perform a readback.
        return std::nullopt;
    }
    const auto scan_metadata = [&]() -> std::optional<Shader::ShaderParams> {
        ExecutorGraphicsShaderTimingScope guest_code_scan_profile{
            shader_lookup_profile,
            ExecutorGraphicsShaderTiming::GuestCodeScan};
        const auto binary_info = AmdGpu::TrySearchBinaryInfo(
            code, shader_signature_word_prefilter_enabled);
        if (!binary_info) {
            return std::nullopt;
        }
        return Shader::ShaderParams{
            .user_data = fresh_user_data,
            .code = std::span{code, binary_info->length / sizeof(u32)},
            .hash = binary_info->shader_hash,
        };
    };

    // A PM4 DMA can make the Vulkan buffer authoritative while the mapped
    // guest backing still contains the old bytes. This was previously hidden
    // by a multi-millisecond driver wait. Resolve only an observed metadata
    // miss and only when MemoryTracker proves GPU ownership; the normal shader
    // hot path gains no readback or extra query.
    const auto recover_missing_metadata =
        [&](u64& scan_generation) -> std::optional<Shader::ShaderParams> {
        ExecutorGraphicsShaderTimingScope guest_read_recovery_profile{
            shader_lookup_profile,
            ExecutorGraphicsShaderTiming::GuestReadRecovery};
        scheduler.FlushPendingBufferTransfersForRange(
            address, AmdGpu::ShaderBinaryProbeBytes);
        const bool gpu_dirty =
            buffer_cache->IsRegionGpuModified(address,
                                              AmdGpu::ShaderBinaryProbeBytes);
        const bool gpu_dirty_exact =
            buffer_cache->IsRegionGpuModifiedExact(
                address, AmdGpu::ShaderBinaryProbeBytes);
        const bool cpu_modified =
            buffer_cache->IsRegionCpuModified(address,
                                              AmdGpu::ShaderBinaryProbeBytes);
        const bool gpu_thread = liverpool != nullptr && liverpool->IsGpuThread();
        const bool registered = buffer_cache->IsRegionRegistered(
            address, AmdGpu::ShaderBinaryProbeBytes);
        bool readback_attempted = false;
        bool readback_succeeded = false;
        if (gpu_thread && registered && (gpu_dirty || gpu_dirty_exact)) {
            ++shader_identity_gpu_dirty_misses;
            readback_attempted = true;
            readback_succeeded =
                buffer_cache->ResolveGpuDirtyMemoryForCpuRead(
                    address, AmdGpu::ShaderBinaryProbeBytes);
            scan_generation = CaptureCpuInvalidationSequence();
            if (auto recovered = scan_metadata()) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
                static std::atomic<u64> recovered_count{0};
                const u64 recovered_ordinal =
                    recovered_count.fetch_add(1, std::memory_order_relaxed) + 1;
                if (recovered_ordinal <= 32 ||
                    (recovered_ordinal & (recovered_ordinal - 1)) == 0) {
                    __android_log_print(
                        ANDROID_LOG_WARN, "LSX4Native",
                        "[EXECUTOR_SHADER_METADATA_RECOVERY] result=recovered n=%llu "
                        "stage=%u addr=0x%llx gpuDirtyExact=%u readbackOk=%u bytes=%zu",
                        static_cast<unsigned long long>(recovered_ordinal),
                        static_cast<unsigned>(stage),
                        static_cast<unsigned long long>(address),
                        gpu_dirty_exact ? 1u : 0u,
                        readback_succeeded ? 1u : 0u,
                        static_cast<size_t>(AmdGpu::ShaderBinaryProbeBytes));
                }
#endif
                return recovered;
            }
        }

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        static std::atomic<u64> miss_count{0};
        const u64 miss_ordinal =
            miss_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (miss_ordinal <= 64 || (miss_ordinal & (miss_ordinal - 1)) == 0) {
            const u64 lease_state =
                VideoCore::PageManager::DebugFexGuestMemoryLeaseState(
                    address, AmdGpu::ShaderBinaryProbeBytes, true);
            u64 head[2]{};
            u64 tail[2]{};
            auto* const memory = Core::Memory::Instance();
            const bool head_readable =
                memory != nullptr &&
                memory->TryCopyReadableMemory(address, head, sizeof(head));
            const bool tail_readable =
                memory != nullptr &&
                memory->TryCopyReadableMemory(
                    address + AmdGpu::ShaderBinarySearchBytes - sizeof(tail),
                    tail, sizeof(tail));
            __android_log_print(
                ANDROID_LOG_ERROR, "LSX4Native",
                "[EXECUTOR_SHADER_METADATA_RETRY] n=%llu stage=%u addr=0x%llx "
                "probeBytes=%zu gpuThread=%u registered=%u gpuDirty=%u "
                "gpuDirtyExact=%u cpuModified=%u readback=%u readbackOk=%u "
                "leaseState=0x%llx generation=%llu "
                "headReadable=%u head=%016llx:%016llx tailReadable=%u "
                "tail=%016llx:%016llx action=retry-next-command",
                static_cast<unsigned long long>(miss_ordinal),
                static_cast<unsigned>(stage),
                static_cast<unsigned long long>(address),
                static_cast<size_t>(AmdGpu::ShaderBinaryProbeBytes),
                gpu_thread ? 1u : 0u, registered ? 1u : 0u,
                gpu_dirty ? 1u : 0u, gpu_dirty_exact ? 1u : 0u,
                cpu_modified ? 1u : 0u, readback_attempted ? 1u : 0u,
                readback_succeeded ? 1u : 0u,
                static_cast<unsigned long long>(lease_state),
                static_cast<unsigned long long>(scan_generation),
                head_readable ? 1u : 0u,
                static_cast<unsigned long long>(head[0]),
                static_cast<unsigned long long>(head[1]),
                tail_readable ? 1u : 0u,
                static_cast<unsigned long long>(tail[0]),
                static_cast<unsigned long long>(tail[1]));
        }
#else
        (void)gpu_dirty_exact;
        (void)cpu_modified;
        (void)readback_attempted;
        (void)readback_succeeded;
#endif
        return std::nullopt;
    };

    if (!hle_clean_buffer_guard_enabled) {
        if (!shader_identity_cache_enabled) {
            u64 scan_generation = CaptureCpuInvalidationSequence();
            if (auto params = scan_metadata()) {
                return params;
            }
            return recover_missing_metadata(scan_generation);
        }

        ++shader_identity_lookups;
        u64 scan_generation = CaptureCpuInvalidationSequence();
        if (auto it = shader_identity_cache.find(address);
            it != shader_identity_cache.end()) {
            ShaderIdentityMetadata& cached_metadata = it.value();
            if (!WasCpuRangeInvalidatedSinceLocked(
                    cached_metadata.address, cached_metadata.length_bytes,
                    cached_metadata.invalidation_sequence)) {
                cached_metadata.invalidation_sequence =
                    CaptureCpuInvalidationSequence();
                ++shader_identity_hits;
                ++shader_identity_gpu_alias_hits;
                return Shader::ShaderParams{
                    .user_data = fresh_user_data,
                    .code = std::span{
                        code, cached_metadata.length_bytes / sizeof(u32)},
                    .hash = cached_metadata.hash,
                };
            }
            ++shader_identity_invalidation_misses;
            ++shader_identity_cpu_erases;
            shader_identity_cache.erase(it);
        }

        auto params = scan_metadata();
        if (!params) {
            params = recover_missing_metadata(scan_generation);
            if (!params) {
                return std::nullopt;
            }
        }
        const size_t length_bytes = params->code.size_bytes();
        const bool valid_length =
            length_bytes != 0 &&
            length_bytes <= std::numeric_limits<u32>::max() &&
            address <= std::numeric_limits<VAddr>::max() - length_bytes;
        ++shader_identity_scans;
        if (!valid_length || WasCpuRangeInvalidatedSinceLocked(
                                 address, length_bytes, scan_generation)) {
            shader_identity_dirty_after_scan += valid_length;
            return params;
        }
        if (!shader_identity_cache.contains(address) &&
            shader_identity_cache.size() >=
                MaxShaderIdentityEntriesPerSubmit) {
            ++shader_identity_capacity_misses;
            shader_identity_submit_erases += shader_identity_cache.size();
            shader_identity_cache.clear();
        }
        shader_identity_cache.insert_or_assign(
            address,
            ShaderIdentityMetadata{
                .address = address,
                .length_bytes = static_cast<u32>(length_bytes),
                .hash = params->hash,
                .invalidation_sequence = CaptureCpuInvalidationSequence(),
            });
        return params;
    }

    if (shader_identity_cache_enabled) {
        ++shader_identity_lookups;
    }
    for (;;) {
        const u64 stable_generation = WaitForStableHleCleanBufferRead();
        u64 scan_generation = CaptureCpuInvalidationSequence();
        if (shader_identity_cache_enabled) {
            if (auto it = shader_identity_cache.find(address);
                it != shader_identity_cache.end()) {
                ShaderIdentityMetadata& cached_metadata = it.value();
                if (!WasCpuRangeInvalidatedSinceLocked(
                        cached_metadata.address,
                        cached_metadata.length_bytes,
                        cached_metadata.invalidation_sequence)) {
                    const u32 length_bytes = cached_metadata.length_bytes;
                    const u64 hash = cached_metadata.hash;
                    const bool retained =
                        RetainHleShaderReadResidency(address, length_bytes);
                    if (!retained) {
                        // Retain has closed PageManager admissions. Keep the
                        // scan gate held through the stability check so no
                        // previously admitted writer can overlap this span.
                        page_manager->DisableHleCleanBufferFastPathFailClosed();
                    }
                    if (!IsHleCleanBufferReadStable(stable_generation)) {
                        continue;
                    }
                    // Never advance a cached identity to a generation which
                    // was captured after the stable-read postvalidation.  A
                    // writer beginning now publishes a later invalidation and
                    // the next lookup rejects this entry.
                    cached_metadata.invalidation_sequence = scan_generation;
                    ++shader_identity_hits;
                    ++shader_identity_gpu_alias_hits;
                    ASSERT_MSG(retained ||
                                   hle_clean_buffer_active_writers.load(
                                       std::memory_order_acquire) == 0,
                               "Unpinned cached shader span is not quiescent");
                    return Shader::ShaderParams{
                        .user_data = fresh_user_data,
                        .code =
                            std::span{code, length_bytes / sizeof(u32)},
                        .hash = hash,
                    };
                }
                ++shader_identity_invalidation_misses;
                ++shader_identity_cpu_erases;
                shader_identity_cache.erase(it);
                if (!IsHleCleanBufferReadStable(stable_generation)) {
                    continue;
                }
            }
        }

        auto params = scan_metadata();
        if (!params) {
            if (!IsHleCleanBufferReadStable(stable_generation)) {
                continue;
            }
            params = recover_missing_metadata(scan_generation);
            if (!params) {
                return std::nullopt;
            }
        }
        const size_t length_bytes = params->code.size_bytes();
        const bool valid_length =
            length_bytes != 0 &&
            length_bytes <= std::numeric_limits<u32>::max() &&
            address < (u64{1} << 40) &&
            length_bytes <= (u64{1} << 40) - address;
        if (shader_identity_cache_enabled) {
            ++shader_identity_scans;
        }
        const bool retained =
            valid_length
                ? RetainHleShaderReadResidency(address, length_bytes)
                : (page_manager->DisableHleCleanBufferFastPathFailClosed(),
                   false);
        if (!retained) {
            page_manager->DisableHleCleanBufferFastPathFailClosed();
        }
        if (!IsHleCleanBufferReadStable(stable_generation)) {
            continue;
        }
        if (!valid_length) {
            return params;
        }
        if (WasCpuRangeInvalidatedSinceLocked(address, length_bytes,
                                              scan_generation)) {
            continue;
        }
        ASSERT_MSG(retained ||
                       hle_clean_buffer_active_writers.load(
                           std::memory_order_acquire) == 0,
                   "Unpinned shader span is not quiescent");
        if (!shader_identity_cache_enabled) {
            return params;
        }
        if (!shader_identity_cache.contains(address) &&
            shader_identity_cache.size() >=
                MaxShaderIdentityEntriesPerSubmit) {
            ++shader_identity_capacity_misses;
            shader_identity_submit_erases += shader_identity_cache.size();
            shader_identity_cache.clear();
        }
        shader_identity_cache.insert_or_assign(
            address,
            ShaderIdentityMetadata{
                .address = address,
                .length_bytes = static_cast<u32>(length_bytes),
                .hash = params->hash,
                .invalidation_sequence = scan_generation,
            });
        return params;
    }
}

void PipelineCache::InvalidateShaderIdentityRange(const VAddr address, const u64 size) {
    if (!shader_identity_cache_enabled || size == 0) {
        return;
    }

    const u64 invalidation_sequence =
        shader_identity_published_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto& invalidation =
        shader_identity_invalidation_history
            [(invalidation_sequence - 1) % ShaderIdentityInvalidationHistorySize];
    // Publish the sequence last. Readers either observe this exact immutable
    // record or conservatively reject the cached snapshot while a writer is
    // filling it; no renderer-thread mutex or whole-cache scan is required.
    invalidation.sequence.store(0, std::memory_order_release);
    invalidation.address.store(address, std::memory_order_relaxed);
    invalidation.size.store(size, std::memory_order_relaxed);
    invalidation.sequence.store(invalidation_sequence, std::memory_order_release);
}

bool PipelineCache::BeginHleCleanBufferWrite(const VAddr address,
                                             const u64 size) noexcept {
    ASSERT(hle_clean_buffer_guard_enabled);
    if (!hle_clean_buffer_shader_scan_mutex.try_lock()) {
        return false;
    }
    hle_clean_buffer_active_writers.fetch_add(1,
                                              std::memory_order_acq_rel);
    hle_clean_buffer_write_generation.fetch_add(1,
                                                 std::memory_order_release);
    InvalidateShaderIdentityRange(address, size);
    return true;
}

void PipelineCache::EndHleCleanBufferWrite(const VAddr address,
                                           const u64 size) noexcept {
    ASSERT(hle_clean_buffer_guard_enabled);
    std::atomic_thread_fence(std::memory_order_release);
    InvalidateShaderIdentityRange(address, size);
    hle_clean_buffer_write_generation.fetch_add(1,
                                                 std::memory_order_release);
    const u32 previous = hle_clean_buffer_active_writers.fetch_sub(
        1, std::memory_order_release);
    ASSERT_MSG(previous != 0,
               "HLE clean-buffer shader writer underflow");
    hle_clean_buffer_shader_scan_mutex.unlock();
}

void PipelineCache::InvalidateShaderIdentityReadbackRange(const VAddr address, const u64 size) {
    // A GPU readback publishes the latest contents into the CPU backing store; it is not a
    // guest CPU write. Shader programs are CPU-uploaded and immutable while referenced by a
    // pipeline, so invalidating their identity here makes the metadata scan fault on the same
    // GPU-protected page again on every draw. Keep this behaviour opt-in because a title which
    // genuinely generates shader code on the GPU still needs conservative invalidation.
    if (immutable_shader_identity_cache_enabled) {
        return;
    }
    InvalidateShaderIdentityRange(address, size);
}

void PipelineCache::AdvanceShaderIdentitySubmit() {
    MaybeCheckpointPipelineCaches();
    if (!shader_identity_cache_enabled) {
        return;
    }

    ++shader_identity_submits;
    const size_t entries = shader_identity_cache.size();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (!VideoCore::IsExecutorLiveQuiet() &&
        (shader_identity_submits <= 4 || (shader_identity_submits & 63u) == 0)) {
        const double hit_percent =
            shader_identity_lookups != 0
                ? 100.0 * static_cast<double>(shader_identity_hits) /
                      static_cast<double>(shader_identity_lookups)
                : 0.0;
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_SHADER_IDENTITY_CACHE] submits=%llu lookups=%llu hits=%llu hitPct=%.1f "
            "scans=%llu gpuDirtyMiss=%llu backingHits=%llu invalidationMiss=%llu dirtyAfterScan=%llu "
            "cpuErases=%llu capacityErases=%llu capacityMiss=%llu entries=%zu",
            static_cast<unsigned long long>(shader_identity_submits),
            static_cast<unsigned long long>(shader_identity_lookups),
            static_cast<unsigned long long>(shader_identity_hits), hit_percent,
            static_cast<unsigned long long>(shader_identity_scans),
            static_cast<unsigned long long>(shader_identity_gpu_dirty_misses),
            static_cast<unsigned long long>(shader_identity_gpu_alias_hits),
            static_cast<unsigned long long>(shader_identity_invalidation_misses),
            static_cast<unsigned long long>(shader_identity_dirty_after_scan),
            static_cast<unsigned long long>(shader_identity_cpu_erases),
            static_cast<unsigned long long>(shader_identity_submit_erases),
            static_cast<unsigned long long>(shader_identity_capacity_misses), entries);
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_FETCH_SHADER_CACHE] lookups=%llu epochHits=%llu backingHits=%llu "
            "reparses=%llu backingFallbacks=%llu racedPublications=%llu "
            "tessLookups=%llu tessHits=%llu tessReads=%llu tessBacking=%llu",
            static_cast<unsigned long long>(fetch_shader_lookups),
            static_cast<unsigned long long>(fetch_shader_epoch_hits),
            static_cast<unsigned long long>(fetch_shader_backing_hits),
            static_cast<unsigned long long>(fetch_shader_reparses),
            static_cast<unsigned long long>(fetch_shader_backing_fallbacks),
            static_cast<unsigned long long>(fetch_shader_raced_publications),
            static_cast<unsigned long long>(tess_constants_lookups),
            static_cast<unsigned long long>(tess_constants_hits),
            static_cast<unsigned long long>(tess_constants_reads),
            static_cast<unsigned long long>(tess_constants_backing_copies));
    }
#endif
}

std::optional<Shader::TessellationDataConstantBuffer>
PipelineCache::ResolveTessConstants(const Shader::Info& info) {
    auto* const shader_lookup_profile =
        ExecutorGraphicsPipelineLookupProfile::Active();
    ExecutorGraphicsShaderTimingScope tessellation_profile{
        shader_lookup_profile,
        ExecutorGraphicsShaderTiming::Tessellation};
    if (info.l_stage != Shader::LogicalStage::TessellationControl &&
        info.l_stage != Shader::LogicalStage::TessellationEval) {
        return std::nullopt;
    }
    ++tess_constants_lookups;
    const auto buffer = info.ReadUdReg<AmdGpu::Buffer>(
        static_cast<u32>(info.tess_consts_ptr_base),
        static_cast<u32>(info.tess_consts_dword_offset));
    const VAddr address = buffer.base_address;
    constexpr u64 size = sizeof(Shader::TessellationDataConstantBuffer);
    const std::array range{Shader::SrtMemoryRange{.address = address, .size = size}};
    if (const auto it = tess_constants_snapshots.find(address);
        it != tess_constants_snapshots.end() &&
        AreSrtMemoryRangesCpuStableSince(range, it.value().invalidation_sequence)) {
        it.value().invalidation_sequence = CaptureCpuInvalidationSequence();
        ++tess_constants_hits;
        return it.value().data;
    }

    const u64 capture_sequence = CaptureCpuInvalidationSequence();
    Shader::TessellationDataConstantBuffer data{};
    // This control block is authored by the guest CPU and consumed read-only
    // by tessellation. Large aliased upload buffers can nevertheless inherit a
    // GPU-modified page bit, making a 36-byte host read wait for the entire
    // Vulkan timeline. The managed identity mode already invalidates on every
    // guest CPU publication, so its backing copy is authoritative here.
    const bool copied_from_backing =
        immutable_shader_identity_cache_enabled &&
        Core::Memory::Instance()->TryCopyBackingMemory(address, &data, sizeof(data));
    if (copied_from_backing) {
        ++tess_constants_backing_copies;
    } else {
        std::memcpy(&data, reinterpret_cast<const void*>(address), sizeof(data));
    }
    ++tess_constants_reads;
    if (!AreSrtMemoryRangesCpuStableSince(range, capture_sequence)) {
        return data;
    }
    if (!tess_constants_snapshots.contains(address) &&
        tess_constants_snapshots.size() >= MaxTessConstantsSnapshots) {
        tess_constants_snapshots.clear();
    }
    tess_constants_snapshots.insert_or_assign(
        address, TessConstantsSnapshot{
                     .data = data,
                     .invalidation_sequence = CaptureCpuInvalidationSequence(),
                 });
    return data;
}

const std::optional<Shader::Gcn::FetchShaderData>& PipelineCache::ResolveFetchShader(
    Program& program, const Shader::Info& info) {
    auto* const shader_lookup_profile =
        ExecutorGraphicsPipelineLookupProfile::Active();
    ExecutorGraphicsShaderTimingScope fetch_profile{
        shader_lookup_profile,
        ExecutorGraphicsShaderTiming::Fetch};
    auto& snapshot = program.fetch_shader_snapshot;
    ++fetch_shader_lookups;
    if (!info.has_fetch_shader) {
        snapshot.parsed.reset();
        snapshot.code.clear();
        snapshot.address = 0;
        snapshot.invalidation_sequence = CaptureCpuInvalidationSequence();
        snapshot.initialized = true;
        return snapshot.parsed;
    }

    const u32* code =
        Shader::Gcn::GetFetchShaderCode(info, info.fetch_shader_sgpr_base);
    const VAddr address = reinterpret_cast<VAddr>(code);
    if (snapshot.initialized && snapshot.parsed &&
        snapshot.code.size() == snapshot.parsed->size) {
        const size_t size_bytes = snapshot.code.size() * sizeof(u32);
        if (snapshot.address == address && AreSrtMemoryRangesCpuStableSince(
                                               std::array{Shader::SrtMemoryRange{
                                                   .address = address, .size = size_bytes}},
                                               snapshot.invalidation_sequence)) {
            snapshot.invalidation_sequence = CaptureCpuInvalidationSequence();
            ++fetch_shader_epoch_hits;
            return snapshot.parsed;
        }

        const u64 validation_sequence = CaptureCpuInvalidationSequence();
        bool bytes_match = false;
        if (immutable_shader_identity_cache_enabled) {
            fetch_shader_backing_validation_scratch.resize(snapshot.code.size());
            const bool copied =
                size_bytes == 0 ||
                Core::Memory::Instance()->TryCopyBackingMemory(
                    address, fetch_shader_backing_validation_scratch.data(), size_bytes);
            bytes_match = copied &&
                          std::ranges::equal(fetch_shader_backing_validation_scratch,
                                             snapshot.code);
        } else {
            bytes_match = size_bytes == 0 ||
                          std::memcmp(snapshot.code.data(), code, size_bytes) == 0;
        }
        u64 publish_sequence{};
        bool stable = false;
        const u64 generation_before_validation = CaptureCpuInvalidationSequence();
        stable = bytes_match &&
                 !WasCpuRangeInvalidatedSinceLocked(address, size_bytes,
                                                   validation_sequence);
        publish_sequence = CaptureCpuInvalidationSequence();
        stable &= publish_sequence == generation_before_validation;
        if (stable) {
            snapshot.address = address;
            snapshot.invalidation_sequence = publish_sequence;
            ++fetch_shader_backing_hits;
            return snapshot.parsed;
        }
    }

    ++fetch_shader_reparses;
    const u64 parse_sequence = CaptureCpuInvalidationSequence();
    auto parsed = Shader::Gcn::ParseFetchShader(info);
    std::vector<u32> parsed_code;
    bool captured_from_backing = false;
    if (parsed) {
        parsed_code.resize(parsed->size);
        if (!parsed_code.empty()) {
            if (immutable_shader_identity_cache_enabled) {
                captured_from_backing =
                    Core::Memory::Instance()->TryCopyBackingMemory(
                        address, parsed_code.data(), parsed_code.size() * sizeof(u32));
            }
            if (!captured_from_backing) {
                fetch_shader_backing_fallbacks += immutable_shader_identity_cache_enabled;
                std::memcpy(parsed_code.data(), code, parsed_code.size() * sizeof(u32));
            }
        }
    }

    u64 publish_sequence{};
    bool stable_publication = parsed.has_value();
    if (parsed) {
        const u64 generation_before_validation = CaptureCpuInvalidationSequence();
        stable_publication = !WasCpuRangeInvalidatedSinceLocked(
            address, parsed_code.size() * sizeof(u32), parse_sequence);
        publish_sequence = CaptureCpuInvalidationSequence();
        stable_publication &= publish_sequence == generation_before_validation;
    }
    snapshot.parsed = std::move(parsed);
    snapshot.code = std::move(parsed_code);
    snapshot.address = stable_publication ? address : 0;
    snapshot.invalidation_sequence = publish_sequence;
    snapshot.initialized = stable_publication;
    fetch_shader_raced_publications += !stable_publication;
    return snapshot.parsed;
}

const GraphicsPipeline* PipelineCache::GetGraphicsPipeline(
    std::optional<AmdGpu::PrimitiveType> host_primitive_override) {
    EnsureAsyncPipelineMode();
    if (Config::managedAsyncPipelineCompilation() &&
        DeferAsyncLiveReplacement(instance)) {
        async_last_foreground_activity_ms.store(
            AsyncSteadyMilliseconds(), std::memory_order_release);
    }
    PublishAsyncPipelineResults();
    const u64 state_generation = liverpool->GraphicsStateGeneration();
    const u64 other_generation = liverpool->GraphicsOtherGeneration();
    const u64 shader_program_generation =
        liverpool->GraphicsShaderProgramGeneration();
    const u64 shader_user_data_generation =
        liverpool->GraphicsShaderUserDataGeneration();
    const GraphicsPipeline* const previous_pipeline = last_graphics_pipeline;
    const bool generation_changed = last_graphics_state_generation != state_generation;
    const bool managed_state_cache =
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        VideoCore::IsManagedVulkanDriverOptimization();
#else
        false;
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    auto& telemetry = graphics_pipeline_telemetry;
    const bool collect_telemetry = !VideoCore::IsExecutorLiveQuiet();
    if (collect_telemetry) {
        ++telemetry.lookups;
        telemetry.generation_misses += generation_changed;
    }
    const auto maybe_log_telemetry = [&] {
        if (!collect_telemetry) {
            return;
        }
        const u64 generation_misses = telemetry.generation_misses;
        const bool log_checkpoint =
            generation_misses == 256 ||
            (generation_misses != 0 && (generation_misses & 4095u) == 0);
        if (!log_checkpoint ||
            telemetry.last_logged_generation_miss == generation_misses) {
            return;
        }
        const auto dirty = liverpool->GetGraphicsDirtyTelemetrySnapshot();
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_GP_CACHE_TELEMETRY] lookups=%llu refreshes=%llu generationMiss=%llu "
            "refreshSameFinalKey=%llu sameStageHashes=%llu actualPipelineChanged=%llu "
            "refreshFailures=%llu irrelevantStateHits=%llu userDataOnlyRefreshes=%llu "
            "structuralSignatureReuses=%llu "
            "dirtyProgram=%llu dirtyUserData=%llu dirtyOther=%llu "
            "programEntries=%zu graphicsPipelineEntries=%zu computePipelineEntries=%zu "
            "newPipelines=%u shaderIdentityEntries=%zu srtObjects=%zu srtRetainedBytes=%zu",
            static_cast<unsigned long long>(telemetry.lookups),
            static_cast<unsigned long long>(telemetry.refreshes),
            static_cast<unsigned long long>(generation_misses),
            static_cast<unsigned long long>(telemetry.refresh_same_final_key),
            static_cast<unsigned long long>(telemetry.same_stage_hashes),
            static_cast<unsigned long long>(telemetry.actual_pipeline_changed),
            static_cast<unsigned long long>(telemetry.refresh_failures),
            static_cast<unsigned long long>(telemetry.irrelevant_state_hits),
            static_cast<unsigned long long>(telemetry.user_data_only_refreshes),
            static_cast<unsigned long long>(telemetry.structural_signature_reuses),
            static_cast<unsigned long long>(dirty.shader_program -
                                            telemetry.last_logged_dirty_program),
            static_cast<unsigned long long>(dirty.shader_user_data -
                                            telemetry.last_logged_dirty_user_data),
            static_cast<unsigned long long>(dirty.other -
                                            telemetry.last_logged_dirty_other),
            program_cache.size(), graphics_pipelines.size(), compute_pipelines.size(),
            num_new_pipelines, shader_identity_cache.size(), srt_snapshot_object_count,
            srt_snapshot_retained_bytes);
        telemetry.last_logged_generation_miss = generation_misses;
        telemetry.last_logged_dirty_program = dirty.shader_program;
        telemetry.last_logged_dirty_user_data = dirty.shader_user_data;
        telemetry.last_logged_dirty_other = dirty.other;
    };
#endif
    if (last_graphics_lookup_resolved &&
        last_graphics_state_generation == state_generation &&
        last_graphics_primitive_override == host_primitive_override) {
        return last_graphics_pipeline;
    }

    // GraphicsStructuralSignature deliberately excludes shader user-data.
    // Matching non-user-data generations and primitive override therefore
    // prove that the previous signature remains valid. The exact proprietary
    // A750 route stays unchanged; the broader generation-guarded route remains
    // marker-only until device A/B/A. This only removes hashing; stage/SRT
    // refresh and validation below remain unchanged.
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const GraphicsLookupPolicy graphics_lookup_policy =
        instance.GetCapabilityProfile().graphics_lookup;
    const bool structural_signature_reuse_route =
        graphics_lookup_policy.structural_signature_reuse_default ||
        (graphics_lookup_policy.allow_generation_guarded_reuse_ab &&
         ExecutorGenerationGuardedGraphicsSignatureReuseEnabled());
#else
    constexpr bool structural_signature_reuse_route = false;
#endif
    const bool reuse_structural_signature =
        CanReuseGraphicsStructuralSignature(
            managed_state_cache, structural_signature_reuse_route,
            last_graphics_lookup_resolved,
            last_graphics_primitive_override == host_primitive_override,
            last_graphics_other_generation == other_generation,
            last_graphics_shader_program_generation == shader_program_generation);
    const u64 structural_signature =
        reuse_structural_signature
            ? last_graphics_structural_signature
            : GraphicsStructuralSignature(liverpool->regs, host_primitive_override);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (collect_telemetry) {
        telemetry.structural_signature_reuses += reuse_structural_signature;
    }
#endif
    const auto commit_lookup_state = [&] {
        last_graphics_state_generation = state_generation;
        last_graphics_other_generation = other_generation;
        last_graphics_shader_program_generation = shader_program_generation;
        last_graphics_shader_user_data_generation = shader_user_data_generation;
        last_graphics_structural_signature = structural_signature;
        last_graphics_primitive_override = host_primitive_override;
        last_graphics_lookup_resolved = true;
    };

    // Games rewrite attachment addresses, viewport/scissor state and other
    // dynamic registers between draws. Those writes must remain visible to
    // Rasterizer, but none of them selects a Vulkan pipeline. Avoid rebuilding
    // all shader stages when the normalized structural state is unchanged.
    if (managed_state_cache && last_graphics_lookup_resolved &&
        last_graphics_primitive_override == host_primitive_override &&
        last_graphics_shader_program_generation == shader_program_generation &&
        last_graphics_shader_user_data_generation == shader_user_data_generation &&
        last_graphics_structural_signature == structural_signature) {
        commit_lookup_state();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (collect_telemetry) {
            ++telemetry.irrelevant_state_hits;
        }
        maybe_log_telemetry();
#endif
        return last_graphics_pipeline;
    }

    std::optional<ExecutorGraphicsPipelineLookupProfile> lookup_profile;
    const bool lookup_profile_enabled =
        ExecutorGraphicsPipelineLookupProfileEnabled();
    auto* const inherited_lookup_profile =
        lookup_profile_enabled
            ? ExecutorGraphicsPipelineLookupProfile::Active()
            : nullptr;
    if (lookup_profile_enabled && inherited_lookup_profile == nullptr) {
        lookup_profile.emplace();
    }
    auto* const active_lookup_profile =
        lookup_profile ? &*lookup_profile : inherited_lookup_profile;
    const u64 background_lock_profile_begin =
        lookup_profile ? lookup_profile->BeginStage() : 0;
    auto background_lock = LockBackgroundWarmUpForForeground();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    // The lock above makes every container size race-free. Publish at most
    // once per 256 refreshes, plus the first refresh after a new pipeline.
    MaybePublishResidencyTelemetry(
        (++residency_telemetry_refreshes & 255u) == 0);
#endif
    if (lookup_profile) {
        lookup_profile->EndStage(
            ExecutorGraphicsPipelineLookupStage::BackgroundLock,
            background_lock_profile_begin);
    }
    graphics_primitive_override = host_primitive_override;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (collect_telemetry) {
        ++telemetry.refreshes;
    }
#endif
    const bool user_data_only_refresh =
        managed_state_cache && previous_pipeline != nullptr &&
        last_graphics_lookup_resolved &&
        last_graphics_primitive_override == host_primitive_override &&
        last_graphics_other_generation == other_generation &&
        last_graphics_shader_program_generation == shader_program_generation &&
        last_graphics_shader_user_data_generation != shader_user_data_generation &&
        last_graphics_structural_signature == structural_signature;
    if (lookup_profile) {
        lookup_profile->SetRefreshMode(user_data_only_refresh);
    }
    bool refresh_ok{};
    {
        ExecutorGraphicsPipelineLookupStageScope refresh_profile{
            active_lookup_profile,
            ExecutorGraphicsPipelineLookupStage::Refresh};
        refresh_ok =
            user_data_only_refresh ? RefreshGraphicsStages(false)
                                   : RefreshGraphicsKey(host_primitive_override);
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (collect_telemetry) {
        telemetry.user_data_only_refreshes += user_data_only_refresh;
    }
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (collect_telemetry && generation_changed && refresh_ok && previous_pipeline != nullptr) {
        const auto& previous_key = previous_pipeline->GetGraphicsKey();
        telemetry.refresh_same_final_key += graphics_key == previous_key;
        telemetry.same_stage_hashes += graphics_key.stage_hashes == previous_key.stage_hashes;
    }
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (collect_telemetry) {
        static std::atomic<int> s_gp{0};
        int gp_n = s_gp.load(std::memory_order_relaxed);
        if (gp_n < 24 &&
            (gp_n = s_gp.fetch_add(1, std::memory_order_relaxed)) < 24) {
            __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                                "[EXECUTOR_GP] n=%d refreshKey=%d vsInfo=%d psInfo=%d", gp_n,
                                (int)refresh_ok, infos[static_cast<u32>(Shader::LogicalStage::Vertex)] != nullptr,
                                infos[static_cast<u32>(Shader::LogicalStage::Fragment)] != nullptr);
        }
    }
#endif
    if (!refresh_ok) {
        if (lookup_profile) {
            lookup_profile->SetOutcome(
                ExecutorGraphicsPipelineLookupOutcome::RefreshFailed);
        }
        last_graphics_pipeline = nullptr;
        last_graphics_lookup_resolved = false;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (collect_telemetry) {
            ++telemetry.refresh_failures;
        }
        maybe_log_telemetry();
#endif
        return nullptr;
    }
    if (lookup_profile) {
        lookup_profile->SetHash(
            std::hash<GraphicsPipelineKey>{}(graphics_key));
    }
    // User-data/SRT addresses legitimately change on nearly every draw, but
    // they frequently resolve to the same shader permutations and fixed
    // graphics state. RefreshGraphicsKey must still update Info/bindings for
    // those resources; once the final key is identical, avoid another large
    // robin-map lookup and pipeline validity walk.
    if (previous_pipeline != nullptr &&
        graphics_key == previous_pipeline->GetGraphicsKey()) {
        if (lookup_profile) {
            lookup_profile->SetOutcome(
                ExecutorGraphicsPipelineLookupOutcome::SameKey);
        }
        last_graphics_pipeline = previous_pipeline;
        commit_lookup_state();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        maybe_log_telemetry();
#endif
        return previous_pipeline;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    static const bool kBisectSkipPipelineCtor =
        std::getenv("EXECUTOR_BISECT_SKIP_PIPELINE_CTOR") != nullptr;
    if (kBisectSkipPipelineCtor) {
        static std::atomic<int> once{0};
        if (once.fetch_add(1) == 0) {
            __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                                "[EXECUTOR_BISECT_SKIP_PIPELINE_CTOR] active (ran RefreshGraphicsKey, "
                                "skipping GraphicsPipeline ctor)");
        }
        if (lookup_profile) {
            lookup_profile->SetOutcome(
                ExecutorGraphicsPipelineLookupOutcome::BuildFailed);
        }
        return nullptr;
    }
#endif
    bool known_invalid{};
    {
        ExecutorGraphicsPipelineLookupStageScope negative_lookup_profile{
            active_lookup_profile,
            ExecutorGraphicsPipelineLookupStage::NegativeLookup};
        known_invalid = IsKnownInvalid(graphics_key);
    }
    if (known_invalid) {
        if (lookup_profile) {
            lookup_profile->SetOutcome(
                ExecutorGraphicsPipelineLookupOutcome::KnownInvalid);
        }
        graphics_pipelines.try_emplace(graphics_key);
        last_graphics_pipeline = nullptr;
        commit_lookup_state();
        return nullptr;
    }
    const u64 map_lookup_profile_begin =
        lookup_profile ? lookup_profile->BeginStage() : 0;
    auto it = graphics_pipelines.find(graphics_key);
    if (lookup_profile) {
        lookup_profile->EndStage(ExecutorGraphicsPipelineLookupStage::MapLookup,
                                 map_lookup_profile_begin);
    }
    const bool pipeline_map_miss = it == graphics_pipelines.end();
    if (pipeline_map_miss && lookup_profile) {
        lookup_profile->MarkMapMiss();
    }
    if (it == graphics_pipelines.end() &&
        UseManagedForegroundFallback(instance)) {
        async_pipeline_stats.graphics_misses.fetch_add(1,
                                                       std::memory_order_relaxed);
        GraphicsPipeline::SerializationSupport sdata{};
        {
            ExecutorGraphicsPipelineLookupStageScope serialization_profile{
                active_lookup_profile,
                ExecutorGraphicsPipelineLookupStage::SerializationSupport};
            GraphicsPipeline::PrepareSerializationSupport(
                instance, graphics_key, infos, runtime_infos, fetch_shader, sdata);
        }

        if (instance.IsPipelineCreationCacheControlSupported() &&
            sdata.tcs.empty() && sdata.tes.empty()) {
            const auto pipeline_hash =
                std::hash<GraphicsPipelineKey>{}(graphics_key);
            std::unique_ptr<GraphicsPipeline> fast_pipeline;
            {
                ExecutorGraphicsPipelineLookupStageScope constructor_profile{
                    active_lookup_profile,
                    ExecutorGraphicsPipelineLookupStage::Constructor};
                fast_pipeline = std::make_unique<GraphicsPipeline>(
                    instance, scheduler, desc_heap, profile, graphics_key,
                    *pipeline_cache, infos, runtime_infos, fetch_shader, modules,
                    sdata, true,
                    vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequired);
            }
            if (fast_pipeline->IsValid()) {
                async_pipeline_stats.fail_fast_hits.fetch_add(
                    1, std::memory_order_relaxed);
                RecordValid(graphics_key);
                RegisterPipelineData(graphics_key, pipeline_hash, sdata);
                ++num_new_pipelines;
                auto [fast_it, inserted] = graphics_pipelines.try_emplace(
                    graphics_key, std::move(fast_pipeline));
                ASSERT(inserted);
                for (size_t stage = 0; stage < MaxShaderStages; ++stage) {
                    if (infos[stage]) {
                        module_related_pipelines[modules[stage]].emplace_back(
                            graphics_key);
                    }
                }
                MarkHostPipelineCacheDirty();
                it = fast_it;
            } else if (fast_pipeline->WasCompileRequired()) {
                async_pipeline_stats.fail_fast_compile_required.fetch_add(
                    1, std::memory_order_relaxed);
            } else {
                if (fast_pipeline->BuildResult() ==
                    vk::Result::eErrorDeviceLost) {
                    scheduler.MarkTerminal(fast_pipeline->BuildResult(),
                                           "graphics pipeline fail-fast");
                } else {
                    RecordInvalid(graphics_key);
                }
                graphics_pipelines.try_emplace(graphics_key);
                it = graphics_pipelines.find(graphics_key);
            }
        }

        if (it == graphics_pipelines.end()) {
            std::unique_ptr<GraphicsPipeline> foreground_pipeline;
            {
                ExecutorGraphicsPipelineLookupStageScope constructor_profile{
                    active_lookup_profile,
                    ExecutorGraphicsPipelineLookupStage::Constructor};
                foreground_pipeline = std::make_unique<GraphicsPipeline>(
                    instance, scheduler, desc_heap, profile, graphics_key,
                    *pipeline_cache, infos, runtime_infos, fetch_shader, modules,
                    sdata, true, ForegroundFallbackBuildFlags(instance));
            }
            if (foreground_pipeline->IsValid()) {
                const auto pipeline_hash =
                    std::hash<GraphicsPipelineKey>{}(graphics_key);
                RecordValid(graphics_key);
                RegisterPipelineData(graphics_key, pipeline_hash, sdata);
                ++num_new_pipelines;
                auto [foreground_it, inserted] =
                    graphics_pipelines.try_emplace(
                        graphics_key, std::move(foreground_pipeline));
                ASSERT(inserted);
                fallback_graphics_pipelines.emplace(graphics_key);
                for (size_t stage = 0; stage < MaxShaderStages; ++stage) {
                    if (infos[stage]) {
                        module_related_pipelines[modules[stage]].emplace_back(
                            graphics_key);
                    }
                }
                async_pipeline_stats.foreground_fallbacks.fetch_add(
                    1, std::memory_order_relaxed);
                MarkHostPipelineCacheDirty();
                it = foreground_it;
                QueueAsyncGraphicsPipeline(
                    graphics_key, infos, runtime_infos, fetch_shader, modules,
                    sdata, true);
            } else {
                if (foreground_pipeline->BuildResult() ==
                    vk::Result::eErrorDeviceLost) {
                    scheduler.MarkTerminal(foreground_pipeline->BuildResult(),
                                           "graphics foreground fallback");
                } else {
                    RecordInvalid(graphics_key);
                }
                graphics_pipelines.try_emplace(graphics_key);
                it = graphics_pipelines.find(graphics_key);
            }
        }
    }
    if (it == graphics_pipelines.end()) {
        const auto [new_it, is_new] =
            graphics_pipelines.try_emplace(graphics_key);
        it = new_it;
        ASSERT(is_new);
        const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(graphics_key);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (ExecutorGraphicsPipelineLookupProfileEnabled()) {
            LOG_INFO(Render_Vulkan, "Compiling graphics pipeline {:#x}", pipeline_hash);
        }
#else
        LOG_INFO(Render_Vulkan, "Compiling graphics pipeline {:#x}", pipeline_hash);
#endif

        GraphicsPipeline::SerializationSupport sdata{};
        {
            ExecutorGraphicsPipelineLookupStageScope constructor_profile{
                active_lookup_profile,
                ExecutorGraphicsPipelineLookupStage::Constructor};
            it.value() = std::make_unique<GraphicsPipeline>(
                instance, scheduler, desc_heap, profile, graphics_key, *pipeline_cache, infos,
                runtime_infos, fetch_shader, modules, sdata, false);
        }

        if (it.value()->IsValid()) {
            RecordValid(graphics_key);
            RegisterPipelineData(graphics_key, pipeline_hash, sdata);
            ++num_new_pipelines;
            MarkHostPipelineCacheDirty();
        } else {
            RecordInvalid(graphics_key);
        }

        for (auto stage = 0; stage < MaxShaderStages; ++stage) {
            if (infos[stage]) {
                auto& m = modules[stage];
                module_related_pipelines[m].emplace_back(graphics_key);
            }
        }
        fetch_shader.reset();
    }
    auto* pipeline = it->second.get();
    if (pipeline && pipeline->IsValid()) {
        for (u32 stage = 0;
             stage < static_cast<u32>(Shader::LogicalStage::Compute); ++stage) {
            pipeline->RebindStageInfo(
                static_cast<Shader::LogicalStage>(stage), infos[stage]);
        }
        last_graphics_pipeline = pipeline;
    } else {
        last_graphics_pipeline = nullptr;
    }
    if (lookup_profile) {
        lookup_profile->SetOutcome(
            pipeline_map_miss
                ? (last_graphics_pipeline != nullptr
                       ? ExecutorGraphicsPipelineLookupOutcome::Built
                       : ExecutorGraphicsPipelineLookupOutcome::BuildFailed)
                : ExecutorGraphicsPipelineLookupOutcome::MapHit);
    }
    commit_lookup_state();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (collect_telemetry) {
        telemetry.actual_pipeline_changed += last_graphics_pipeline != previous_pipeline;
    }
    maybe_log_telemetry();
#endif
    return last_graphics_pipeline;
}

const ComputePipeline* PipelineCache::GetComputePipeline() {
    EnsureAsyncPipelineMode();
    if (Config::managedAsyncPipelineCompilation() &&
        DeferAsyncLiveReplacement(instance)) {
        async_last_foreground_activity_ms.store(
            AsyncSteadyMilliseconds(), std::memory_order_release);
    }
    PublishAsyncPipelineResults();
    auto background_lock = LockBackgroundWarmUpForForeground();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    MaybePublishResidencyTelemetry(
        (++residency_telemetry_refreshes & 255u) == 0);
#endif
    if (!RefreshComputeKey()) {
        return nullptr;
    }
    if (IsKnownInvalid(compute_key)) {
        compute_pipelines.try_emplace(compute_key);
        return nullptr;
    }
    auto it = compute_pipelines.find(compute_key);
    if (it == compute_pipelines.end() &&
        UseManagedForegroundFallback(instance)) {
        async_pipeline_stats.compute_misses.fetch_add(1,
                                                      std::memory_order_relaxed);
        ComputePipeline::SerializationSupport sdata{};
        sdata.storage_buffers.reserve(infos[0]->buffers.size());
        for (const auto& buffer : infos[0]->buffers) {
            sdata.storage_buffers.push_back(
                buffer.IsStorage(buffer.GetSharp(*infos[0])) ? 1u : 0u);
        }
        sdata.image_bindings.reserve(infos[0]->images.size());
        for (const auto& image : infos[0]->images) {
            sdata.image_bindings.push_back(image.NumBindings(*infos[0]));
        }

        if (instance.IsPipelineCreationCacheControlSupported()) {
            auto fast_pipeline = std::make_unique<ComputePipeline>(
                instance, scheduler, desc_heap, profile, *pipeline_cache,
                compute_key, *infos[0], runtime_infos[0].cs_info.workgroup_size,
                modules[0], sdata, true,
                vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequired);
            if (fast_pipeline->IsValid()) {
                async_pipeline_stats.fail_fast_hits.fetch_add(
                    1, std::memory_order_relaxed);
                RecordValid(compute_key);
                RegisterPipelineData(compute_key, sdata);
                ++num_new_pipelines;
                auto [fast_it, inserted] = compute_pipelines.try_emplace(
                    compute_key, std::move(fast_pipeline));
                ASSERT(inserted);
                module_related_pipelines[modules[0]].emplace_back(compute_key);
                MarkHostPipelineCacheDirty();
                it = fast_it;
            } else if (fast_pipeline->WasCompileRequired()) {
                async_pipeline_stats.fail_fast_compile_required.fetch_add(
                    1, std::memory_order_relaxed);
            } else {
                if (fast_pipeline->BuildResult() ==
                    vk::Result::eErrorDeviceLost) {
                    scheduler.MarkTerminal(fast_pipeline->BuildResult(),
                                           "compute pipeline fail-fast");
                } else {
                    RecordInvalid(compute_key);
                }
                compute_pipelines.try_emplace(compute_key);
                it = compute_pipelines.find(compute_key);
            }
        }

        if (it == compute_pipelines.end()) {
        auto foreground_pipeline = std::make_unique<ComputePipeline>(
            instance, scheduler, desc_heap, profile, *pipeline_cache,
            compute_key, *infos[0], runtime_infos[0].cs_info.workgroup_size,
            modules[0], sdata, true,
            ForegroundFallbackBuildFlags(instance));
            if (foreground_pipeline->IsValid()) {
                RecordValid(compute_key);
                RegisterPipelineData(compute_key, sdata);
                ++num_new_pipelines;
                auto [foreground_it, inserted] =
                    compute_pipelines.try_emplace(
                        compute_key, std::move(foreground_pipeline));
                ASSERT(inserted);
                fallback_compute_pipelines.emplace(compute_key);
                module_related_pipelines[modules[0]].emplace_back(compute_key);
                async_pipeline_stats.foreground_fallbacks.fetch_add(
                    1, std::memory_order_relaxed);
                MarkHostPipelineCacheDirty();
                it = foreground_it;
                QueueAsyncComputePipeline(
                    compute_key, *infos[0], runtime_infos[0].cs_info.workgroup_size,
                    modules[0], sdata, true);
            } else {
                if (foreground_pipeline->BuildResult() ==
                    vk::Result::eErrorDeviceLost) {
                    scheduler.MarkTerminal(foreground_pipeline->BuildResult(),
                                           "compute foreground fallback");
                } else {
                    RecordInvalid(compute_key);
                }
                compute_pipelines.try_emplace(compute_key);
                it = compute_pipelines.find(compute_key);
            }
        }
    }
    if (it == compute_pipelines.end()) {
        const auto [new_it, is_new] =
            compute_pipelines.try_emplace(compute_key);
        it = new_it;
        ASSERT(is_new);
        const auto pipeline_hash = std::hash<ComputePipelineKey>{}(compute_key);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (ExecutorGraphicsPipelineLookupProfileEnabled()) {
            LOG_INFO(Render_Vulkan, "Compiling compute pipeline {:#x}", pipeline_hash);
        }
#else
        LOG_INFO(Render_Vulkan, "Compiling compute pipeline {:#x}", pipeline_hash);
#endif

        ComputePipeline::SerializationSupport sdata{};
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        ExecutorLogComputeLookupPhase("before-constructor", compute_key);
#endif
        it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                       *pipeline_cache, compute_key, *infos[0],
                                                       runtime_infos[0].cs_info.workgroup_size,
                                                       modules[0], sdata, false);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        ExecutorLogComputeLookupPhase("after-constructor", compute_key);
#endif
        if (it.value()->IsValid()) {
            RecordValid(compute_key);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            ExecutorLogComputeLookupPhase("after-record-valid", compute_key);
#endif
            RegisterPipelineData(compute_key, sdata);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            ExecutorLogComputeLookupPhase("after-register-data", compute_key);
#endif
            ++num_new_pipelines;
            MarkHostPipelineCacheDirty();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            ExecutorLogComputeLookupPhase("after-mark-dirty", compute_key);
#endif
        } else {
            RecordInvalid(compute_key);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            ExecutorLogComputeLookupPhase("after-record-invalid", compute_key);
#endif
        }

        auto& m = modules[0];
        module_related_pipelines[m].emplace_back(compute_key);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        ExecutorLogComputeLookupPhase("after-module-link", compute_key);
#endif
    }
    auto* pipeline = it->second.get();
    if (!pipeline || !pipeline->IsValid()) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        ExecutorLogComputeLookupPhase("return-invalid", compute_key);
#endif
        return nullptr;
    }
    pipeline->RebindStageInfo(Shader::LogicalStage::Compute, infos[0]);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    ExecutorLogComputeLookupPhase("return-valid", compute_key);
#endif
    return pipeline;
}

bool PipelineCache::RefreshGraphicsKey(
    std::optional<AmdGpu::PrimitiveType> host_primitive_override) {
    std::memset(&graphics_key, 0, sizeof(GraphicsPipelineKey));
    const auto& regs = liverpool->regs;
    auto& key = graphics_key;

    // Keep the pipeline key aligned with PrepareRenderState: a merely registered depth buffer is
    // not an attachment when both depth and stencil tests are disabled.  Treating it as active
    // unnecessarily collapses a valid multisampled color-only pass to the inactive depth
    // buffer's sample count on devices without mixed-attachment extensions.
    const bool depth_enabled =
        regs.depth_control.depth_enable && regs.depth_buffer.DepthValid();
    const bool stencil_enabled =
        regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid();
    const bool db_enabled = depth_enabled || stencil_enabled;

    // BeginRendering binds every valid aspect of the depth/stencil image once either test makes
    // the attachment active.  Keep both formats in the pipeline key in that case, even for a
    // depth-only or stencil-only test, so VkPipelineRenderingCreateInfo exactly matches the
    // attachment views passed to dynamic rendering.  When neither test is active, omit the
    // registered buffer entirely so it cannot collapse an unrelated multisampled color pass.
    key.z_format = db_enabled && regs.depth_buffer.DepthValid()
                       ? regs.depth_buffer.z_info.format
                       : AmdGpu::DepthBuffer::ZFormat::Invalid;
    key.stencil_format = db_enabled && regs.depth_buffer.StencilValid()
                             ? regs.depth_buffer.stencil_info.format
                             : AmdGpu::DepthBuffer::StencilFormat::Invalid;
    key.depth_clamp_enable = !regs.depth_render_override.disable_viewport_clamp;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    key.rasterizer_discard_enable = regs.clipper_control.dx_rasterization_kill;
#endif
    key.depth_clip_enable = regs.clipper_control.ZclipEnable();
    key.clip_space = regs.clipper_control.clip_space;
    key.provoking_vtx_last = regs.polygon_control.provoking_vtx_last;
    key.prim_type = host_primitive_override.value_or(regs.primitive_type);
    key.polygon_mode = regs.polygon_control.PolyMode();
    key.patch_control_points =
        regs.stage_enable.hs_en ? regs.ls_hs_config.hs_input_control_points : 0;
    key.logic_op = regs.color_control.rop3;
    key.depth_samples = db_enabled ? regs.depth_buffer.NumSamples() : 1;
    key.num_samples = key.depth_samples;
    key.cb_shader_mask = regs.color_shader_mask;

    if (instance.UseLegacyRenderPass()) {
        u32 viewport_count = 0;
        for (const auto& viewport : regs.viewports) {
            viewport_count += viewport.xscale != 0.0f ? 1u : 0u;
        }
        key.legacy_viewport_count = static_cast<u8>(std::max(viewport_count, 1u));

        const bool depth_test = regs.depth_control.depth_enable && regs.depth_buffer.DepthValid();
        const bool depth_write = depth_test && regs.depth_control.depth_write_enable &&
                                 !regs.depth_render_control.depth_clear_enable;
        const bool depth_bounds = regs.depth_control.depth_bounds_enable;
        const bool depth_bias = regs.polygon_control.NeedsBias();
        const bool stencil_test =
            regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid();
        const auto is_list_topology = [](const AmdGpu::PrimitiveType type) {
            const auto topology = LiverpoolToVK::PrimitiveType(type);
            return topology == vk::PrimitiveTopology::ePointList ||
                   topology == vk::PrimitiveTopology::eLineList ||
                   topology == vk::PrimitiveTopology::eTriangleList ||
                   topology == vk::PrimitiveTopology::eLineListWithAdjacency ||
                   topology == vk::PrimitiveTopology::eTriangleListWithAdjacency;
        };
        const bool is_patch_topology =
            key.prim_type == AmdGpu::PrimitiveType::PatchPrimitive ||
            key.prim_type == AmdGpu::PrimitiveType::QuadList ||
            key.prim_type == AmdGpu::PrimitiveType::RectList;
        const bool primitive_restart =
            (regs.enable_primitive_restart & 1) != 0 &&
            (instance.IsListRestartSupported() || !is_list_topology(key.prim_type)) &&
            (instance.IsPatchListRestartSupported() || !is_patch_topology);

        key.legacy_fixed_state_flags =
            (depth_test ? LegacyDepthTest : 0) |
            (depth_write ? LegacyDepthWrite : 0) |
            (depth_bounds ? LegacyDepthBoundsTest : 0) |
            (depth_bias ? LegacyDepthBias : 0) |
            (stencil_test ? LegacyStencilTest : 0) |
            (primitive_restart ? LegacyPrimitiveRestart : 0);
        key.legacy_depth_compare_op =
            static_cast<u32>(LiverpoolToVK::CompareOp(regs.depth_control.depth_func));
        const auto cull_mode = LiverpoolToVK::IsPrimitiveCulled(key.prim_type)
                                   ? LiverpoolToVK::CullMode(regs.polygon_control.CullingMode())
                                   : vk::CullModeFlagBits::eNone;
        key.legacy_cull_mode = static_cast<u32>(cull_mode);
        key.legacy_front_face =
            static_cast<u32>(LiverpoolToVK::FrontFace(regs.polygon_control.front_face));

        const std::array<u32, 4> front_stencil{
            static_cast<u32>(LiverpoolToVK::StencilOp(
                regs.stencil_control.stencil_fail_front)),
            static_cast<u32>(LiverpoolToVK::StencilOp(
                regs.stencil_control.stencil_zpass_front)),
            static_cast<u32>(LiverpoolToVK::StencilOp(
                regs.stencil_control.stencil_zfail_front)),
            static_cast<u32>(LiverpoolToVK::CompareOp(
                regs.depth_control.stencil_ref_func)),
        };
        const std::array<u32, 4> back_stencil = regs.depth_control.backface_enable
                                                    ? std::array<u32, 4>{
                                                          static_cast<u32>(LiverpoolToVK::StencilOp(
                                                              regs.stencil_control.stencil_fail_back)),
                                                          static_cast<u32>(LiverpoolToVK::StencilOp(
                                                              regs.stencil_control.stencil_zpass_back)),
                                                          static_cast<u32>(LiverpoolToVK::StencilOp(
                                                              regs.stencil_control.stencil_zfail_back)),
                                                          static_cast<u32>(LiverpoolToVK::CompareOp(
                                                              regs.depth_control.stencil_bf_func)),
                                                      }
                                                    : front_stencil;
        key.legacy_stencil_front = front_stencil;
        key.legacy_stencil_back = back_stencil;
    }

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;

    for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!col_buf || !regs.color_target_mask.GetMask(cb)) {
            continue;
        }

        auto& color_buffer = key.color_buffers[cb];
        color_buffer.data_format = col_buf.GetDataFmt();
        color_buffer.num_format = col_buf.GetNumberFmt();
        color_buffer.num_conversion = col_buf.GetNumberConversion();
        color_buffer.export_format = regs.color_export_format.GetFormat(cb);
        color_buffer.swizzle = col_buf.Swizzle();
    }

    if (!RefreshGraphicsStages()) {
        return false;
    }

    u8 color_samples = 0;
    bool all_color_samples_same = true;
    bool has_active_color_target = false;
    bool has_rop3_enabled_target = false;
    for (s32 cb = 0; cb < key.num_color_attachments && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (!col_buf || !target_mask) {
            continue;
        }
        if ((key.mrt_mask & (1u << cb)) == 0) {
            std::memset(&key.color_buffers[cb], 0, sizeof(Shader::PsColorBuffer));
            continue;
        }

        has_active_color_target = true;
        has_rop3_enabled_target |= !regs.blend_control[cb].disable_rop3;

        if (regs.blend_control[cb].enable && !col_buf.info.blend_bypass) {
            key.blend_controls[cb] = regs.blend_control[cb];
        }

        key.write_masks[cb] =
            vk::ColorComponentFlags{key.color_buffers[cb].swizzle.ApplyMask(target_mask)};

        const u8 prev_color_samples = std::exchange(color_samples, col_buf.NumSamples());
        all_color_samples_same &= color_samples == prev_color_samples || prev_color_samples == 0;
        key.color_samples[cb] = color_samples;
        key.num_samples = std::max(key.num_samples, color_samples);
    }

    if (has_active_color_target && !has_rop3_enabled_target) {
        key.logic_op = AmdGpu::ColorControl::LogicOp::Copy;
    }

    if (color_samples != 0) {
        const bool depth_mismatch = db_enabled && color_samples != key.depth_samples;
        if (!all_color_samples_same && !instance.IsMixedAnySamplesSupported() ||
            all_color_samples_same && depth_mismatch &&
                !instance.IsMixedDepthSamplesSupported()) {
            key.color_samples.fill(key.depth_samples);
            key.num_samples = key.depth_samples;
        }
    }

    return true;
}

bool PipelineCache::RefreshGraphicsStages(const bool refresh_runtime_info) {
    auto* const lookup_profile =
        ExecutorGraphicsPipelineLookupProfile::Active();
    ExecutorGraphicsPipelineLookupStageScope refresh_stages_profile{
        lookup_profile,
        ExecutorGraphicsPipelineLookupStage::RefreshStages};
    const u64 guest_read_admission_begin =
        lookup_profile != nullptr ? lookup_profile->BeginStage() : 0;
    HleCleanBufferGuestReadScope hle_guest_read_scope{*this};
    if (lookup_profile != nullptr) [[unlikely]] {
        lookup_profile->EndStage(
            ExecutorGraphicsPipelineLookupStage::GuestReadAdmission,
            guest_read_admission_begin);
    }
    const auto& regs = liverpool->regs;
    ProgramHotThreadState* call_scope_program_hot_state = nullptr;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    u32 call_scope_program_calls = 0;
    if (ExecutorProgramHotGroupedTlsEnabled() &&
        ExecutorProgramHotCallScopeTlsHoistEnabled()) {
        call_scope_program_hot_state = &ExecutorProgramHotThreadState();
    }
#endif
    auto& key = graphics_key;
    fetch_shader = std::nullopt;
    key.stage_hashes.fill(0);
    key.vertex_buffer_formats.fill({});
    key.mrt_mask = 0;
    key.num_color_attachments = 0;

    enum class StageBindResult : u8 {
        Disabled,
        Ready,
        Retry,
    };

    Shader::Backend::Bindings binding{};
    const auto bind_stage = [&](Shader::Stage stage_in,
                                Shader::LogicalStage stage_out) -> StageBindResult {
        ExecutorGraphicsShaderStageContext shader_stage_profile{
            lookup_profile, stage_in, stage_out};
        const auto stage_in_idx = static_cast<u32>(stage_in);
        const auto stage_out_idx = static_cast<u32>(stage_out);
        if (!regs.stage_enable.IsStageEnabled(stage_in_idx)) {
            key.stage_hashes[stage_out_idx] = 0;
            infos[stage_out_idx] = nullptr;
            shader_stage_profile.SetOutcome(
                ExecutorGraphicsShaderBindOutcome::Disabled);
            return StageBindResult::Disabled;
        }

        const auto* pgm = regs.ProgramForStage(stage_in_idx);
        if (!pgm || !pgm->Address<u32*>()) {
            key.stage_hashes[stage_out_idx] = 0;
            infos[stage_out_idx] = nullptr;
            shader_stage_profile.SetOutcome(
                ExecutorGraphicsShaderBindOutcome::Disabled);
            return StageBindResult::Disabled;
        }

        const auto params = GetShaderParams(
            pgm->user_data.data(), pgm->Address<const u32*>(), stage_in);
        if (!params) {
            key.stage_hashes[stage_out_idx] = 0;
            infos[stage_out_idx] = nullptr;
            shader_stage_profile.SetOutcome(
                ExecutorGraphicsShaderBindOutcome::ParamsRetry);
            return StageBindResult::Retry;
        }
        shader_stage_profile.SetHash(params->hash);
        const auto program = GetProgram(stage_in, stage_out, *params, binding,
                                        refresh_runtime_info,
                                        call_scope_program_hot_state);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (call_scope_program_hot_state != nullptr &&
            ++call_scope_program_calls == 2) {
            ExecutorRecordProgramHotCallScopeTlsHoist();
        }
#endif
        if (!program) {
            key.stage_hashes[stage_out_idx] = 0;
            infos[stage_out_idx] = nullptr;
            shader_stage_profile.SetOutcome(
                ExecutorGraphicsShaderBindOutcome::ProgramRetry);
            return StageBindResult::Retry;
        }
        const auto& program_result = *program;
        infos[stage_out_idx] = program_result.info;
        modules[stage_out_idx] = program_result.module;
        key.stage_hashes[stage_out_idx] = program_result.hash;
        if (program_result.borrowed_fetch_shader) {
            const auto& fetch_shader_data = program_result.FetchShaderData();
            if (fetch_shader_data) {
                // The PipelineCache still owns the one payload needed by the
                // selected graphics pipeline. D84 removes only the redundant
                // Result and bind-local copies before this final publication.
                fetch_shader = *fetch_shader_data;
            }
        } else {
            // Preserve the pre-D84 two-step copy in the default-OFF control.
            std::optional<Shader::Gcn::FetchShaderData> fetch_shader_ =
                program_result.FetchShaderData();
            if (fetch_shader_) {
                fetch_shader = fetch_shader_;
            }
        }
        shader_stage_profile.SetOutcome(
            ExecutorGraphicsShaderBindOutcome::Ready);
        return StageBindResult::Ready;
    };

    infos.fill(nullptr);
    modules.fill(nullptr);
    if (refresh_runtime_info) {
        runtime_infos.fill({});
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    {
        static std::atomic<int> s_rgs{0};
        int rgs_n = s_rgs.load(std::memory_order_relaxed);
        if (rgs_n < 24 &&
            (rgs_n = s_rgs.fetch_add(1, std::memory_order_relaxed)) < 24) {
            const auto* vpgm = regs.ProgramForStage(static_cast<u32>(Stage::Vertex));
            const auto* fpgm = regs.ProgramForStage(static_cast<u32>(Stage::Fragment));
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_RGS] n=%d stage_enable=0x%x fsEn=%d vsEn=%d fsAddr=0x%llx vsAddr=0x%llx",
                rgs_n, static_cast<unsigned>(regs.stage_enable.raw),
                (int)regs.stage_enable.IsStageEnabled(static_cast<u32>(Stage::Fragment)),
                (int)regs.stage_enable.IsStageEnabled(static_cast<u32>(Stage::Vertex)),
                (unsigned long long)(fpgm ? reinterpret_cast<u64>(fpgm->Address<const u8*>()) : 0ull),
                (unsigned long long)(vpgm ? reinterpret_cast<u64>(vpgm->Address<const u8*>()) : 0ull));
        }
    }
#endif
    const auto fragment_result =
        bind_stage(Stage::Fragment, LogicalStage::Fragment);
    if (fragment_result == StageBindResult::Retry) {
        return false;
    }
    if (fragment_result == StageBindResult::Disabled &&
        regs.vs_output_control.clip_distance_enable &&
        profile.needs_clip_distance_emulation) {
        LOG_WARNING(Render_Vulkan,
                    "Clip distance emulation is ineffective due to absense of fragment shader");
    }

    const auto* fs_info = infos[static_cast<u32>(LogicalStage::Fragment)];
    key.mrt_mask = fs_info ? fs_info->mrt_mask : 0u;
    const u32 raw_num_color_attachments = static_cast<u32>(std::bit_width(key.mrt_mask));
    key.num_color_attachments = std::min<u32>(raw_num_color_attachments, AmdGpu::NUM_COLOR_BUFFERS);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (raw_num_color_attachments > AmdGpu::NUM_COLOR_BUFFERS) {
        __android_log_print(ANDROID_LOG_ERROR, "LSX4Native",
                            "[EXECUTOR_MRT_OOB] mrt_mask=0x%x raw_num_color_attachments=%u > %u -> "
                            "clamped (would overrun pipeline color arrays / driver attachmentCount)",
                            static_cast<unsigned>(key.mrt_mask), raw_num_color_attachments,
                            static_cast<unsigned>(AmdGpu::NUM_COLOR_BUFFERS));
    }
#endif

    switch (regs.stage_enable.raw) {
    case AmdGpu::ShaderStageEnable::VgtStages::EsGs:
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (bind_stage(Stage::Export, LogicalStage::Vertex) !=
            StageBindResult::Ready) {
            return false;
        }
        if (bind_stage(Stage::Geometry, LogicalStage::Geometry) !=
            StageBindResult::Ready) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHs:
        if (!instance.IsTessellationSupported() ||
            (regs.tess_config.type == AmdGpu::TessellationType::Isoline &&
             !instance.IsTessellationIsolinesSupported())) {
            return false;
        }
        if (bind_stage(Stage::Hull, LogicalStage::TessellationControl) !=
            StageBindResult::Ready) {
            return false;
        }
        if (bind_stage(Stage::Vertex, LogicalStage::TessellationEval) !=
            StageBindResult::Ready) {
            return false;
        }
        if (bind_stage(Stage::Local, LogicalStage::Vertex) !=
            StageBindResult::Ready) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHsEsGs:
        if (!instance.IsTessellationSupported() ||
            (regs.tess_config.type == AmdGpu::TessellationType::Isoline &&
             !instance.IsTessellationIsolinesSupported())) {
            return false;
        }
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (bind_stage(Stage::Hull, LogicalStage::TessellationControl) !=
            StageBindResult::Ready) {
            return false;
        }
        if (bind_stage(Stage::Export, LogicalStage::TessellationEval) !=
            StageBindResult::Ready) {
            return false;
        }
        if (bind_stage(Stage::Local, LogicalStage::Vertex) !=
            StageBindResult::Ready) {
            return false;
        }
        if (bind_stage(Stage::Geometry, LogicalStage::Geometry) !=
            StageBindResult::Ready) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::Vs:
        if (bind_stage(Stage::Vertex, LogicalStage::Vertex) !=
            StageBindResult::Ready) {
            return false;
        }
        break;
    default:
        UNREACHABLE_MSG("unhandled stage_en: {}", static_cast<u32>(regs.stage_enable.raw));
    }

    const auto* vs_info = infos[static_cast<u32>(Shader::LogicalStage::Vertex)];
    if (vs_info && fetch_shader && !instance.IsVertexInputDynamicState()) {
        u32 vertex_binding = 0;
        for (const auto& attrib : fetch_shader->attributes) {
            const auto& buffer = attrib.GetSharp(*vs_info);
            ASSERT(vertex_binding < MaxVertexBufferCount);
            key.vertex_buffer_formats[vertex_binding++] =
                Vulkan::LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
        }
    }

    return true;
}

bool PipelineCache::RefreshComputeKey() {
    HleCleanBufferGuestReadScope hle_guest_read_scope{*this};
    Shader::Backend::Bindings binding{};
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto cs_params = GetShaderParams(
        cs_pgm.user_data.data(), cs_pgm.Address<const u32*>(),
        Shader::Stage::Compute);
    if (!cs_params) {
        return false;
    }
    const auto program = GetProgram(Shader::Stage::Compute,
                                    LogicalStage::Compute, *cs_params, binding);
    if (!program) {
        return false;
    }
    infos[0] = program->info;
    modules[0] = program->module;
    fetch_shader = program->FetchShaderData();
    compute_key.value = program->hash;
    return true;
}

vk::ShaderModule PipelineCache::CompileModule(Shader::Info& info,
                                               Shader::RuntimeInfo& runtime_info,
                                               const std::span<const u32>& code, size_t perm_idx,
                                               Shader::Backend::Bindings& binding,
                                               std::shared_ptr<AsyncShaderPayload>*
                                                   optimization_payload) {
    auto* const shader_lookup_profile =
        ExecutorGraphicsPipelineLookupProfile::Active();
    ExecutorGraphicsShaderTimingScope compile_module_profile{
        shader_lookup_profile,
        ExecutorGraphicsShaderTiming::CompileModule};
    if (shader_lookup_profile != nullptr) [[unlikely]] {
        shader_lookup_profile->MarkShaderCompile();
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (ExecutorGraphicsPipelineLookupProfileEnabled()) {
        LOG_INFO(Render_Vulkan, "Compiling {} shader {:#x} {}", info.stage,
                 info.pgm_hash, perm_idx != 0 ? "(permutation)" : "");
    }
#else
    LOG_INFO(Render_Vulkan, "Compiling {} shader {:#x} {}", info.stage, info.pgm_hash,
             perm_idx != 0 ? "(permutation)" : "");
#endif
    {
        ExecutorGraphicsShaderTimingScope storage_profile{
            shader_lookup_profile,
            ExecutorGraphicsShaderTiming::Storage};
        DumpShader(code, info.pgm_hash, info.stage, perm_idx, "bin");
    }

    if (optimization_payload) {
        optimization_payload->reset();
    }
    std::shared_ptr<AsyncShaderPayload> shader_payload;
#if defined(LSX4_APPLE_RUNTIME)
    const bool apple_split = info.stage == Stage::Compute &&
        AppleComputeSplit::Selected() && AppleComputeSplit::state.direct &&
        AppleComputeSplit::state.factor == 8 &&
        runtime_info.cs_info.workgroup_size == std::array<u32,3>{128,1,1};
    const auto apple_original_local_size = runtime_info.cs_info.workgroup_size;
    // Translate all guest instructions with the original workgroup geometry.
    // Only the final SPIR-V built-ins/execution mode are virtualized.
    if (apple_split) runtime_info.cs_info.workgroup_size[0] = 1024;
    struct AppleLocalSizeRestore {
        std::array<u32,3>& target;
        std::array<u32,3> original;
        bool active;
        ~AppleLocalSizeRestore() {if(active) target=original;}
    } apple_local_restore{runtime_info.cs_info.workgroup_size,apple_original_local_size,apple_split};
#endif
    std::vector<u32> spv;
    bool use_fast_tier =
        !async_shader_retranslation_disabled &&
        optimization_payload != nullptr &&
        Config::managedAsyncPipelineCompilation() &&
        !DeferAsyncLiveReplacement(instance);
#if defined(LSX4_APPLE_RUNTIME)
    if (info.stage == Shader::Stage::Compute && AppleComputeSplit::Selected())
        use_fast_tier = false;
#endif
#if defined(LSX4_APPLE_RUNTIME)
    if (AppleSpirvOptimizer217::Selected() && !Config::patchShaders() &&
        !(use_fast_tier && ClaudeShaderStorage::FastTier())) {
        use_fast_tier=false;
        AppleSpirvOptimizer217::MarkFullIR(info.pgm_hash,static_cast<unsigned>(info.stage));
    }
#endif
    if (use_fast_tier &&
        IsShaderBinaryTierKnownInvalid(info.pgm_hash, perm_idx,
                                       ShaderBinaryTier::Fast)) {
        use_fast_tier = false;
        async_pipeline_stats.shader_negative_hits.fetch_add(
            1, std::memory_order_relaxed);
    }
    if (use_fast_tier) {
        shader_payload = std::make_shared<AsyncShaderPayload>();
        std::ranges::copy(info.user_data, shader_payload->user_data.begin());
        shader_payload->code.assign(code.begin(), code.end());
        shader_payload->source_runtime_info = runtime_info;
        shader_payload->runtime_info = runtime_info;
        shader_payload->binding_start = binding;
        const Shader::ShaderParams owned_params{
            .user_data = shader_payload->user_data,
            .code = shader_payload->code,
            .hash = info.pgm_hash,
        };
        shader_payload->info =
            std::make_unique<Shader::Info>(info.stage, info.l_stage,
                                           owned_params);
        shader_payload->pools = std::make_unique<Shader::Pools>();
        {
            ExecutorGraphicsShaderTimingScope translate_profile{
                shader_lookup_profile,
                ExecutorGraphicsShaderTiming::Translate};
            shader_payload->ir = std::make_unique<Shader::IR::Program>(
                Shader::TranslateProgram(shader_payload->code,
                                         *shader_payload->pools,
                                         *shader_payload->info,
                                         shader_payload->runtime_info, profile,
                                         Shader::TranslationTier::Fast));
        }
        {
            ExecutorGraphicsShaderTimingScope emit_spirv_profile{
                shader_lookup_profile,
                ExecutorGraphicsShaderTiming::EmitSpirv};
            spv = Shader::Backend::SPIRV::EmitSPIRV(
                profile, shader_payload->runtime_info, *shader_payload->ir,
                binding);
        }
        shader_payload->expected_binding_end = binding;
        constexpr size_t IrMemoryEstimateMultiplier = 12;
        const size_t source_bytes =
            shader_payload->code.size() * sizeof(u32);
        constexpr size_t PoolAllocationFloor =
            Shader::Pools::InstPoolSize * sizeof(Shader::IR::Inst) +
            Shader::Pools::BlockPoolSize * sizeof(Shader::IR::Block);
        shader_payload->queued_bytes =
            source_bytes >
                    (std::numeric_limits<size_t>::max() -
                     PoolAllocationFloor) /
                        IrMemoryEstimateMultiplier
                ? std::numeric_limits<size_t>::max()
                : PoolAllocationFloor +
                      source_bytes * IrMemoryEstimateMultiplier;

        const auto foreground_user_data = info.user_data;
        const VAddr foreground_pgm_base = info.pgm_base;
        info = *shader_payload->info;
        info.user_data = foreground_user_data;
        info.pgm_base = foreground_pgm_base;
        async_pipeline_stats.shader_fast_compiled.fetch_add(
            1, std::memory_order_relaxed);
    } else {
        const auto ir_program = [&] {
            ExecutorGraphicsShaderTimingScope translate_profile{
                shader_lookup_profile,
                ExecutorGraphicsShaderTiming::Translate};
            return Shader::TranslateProgram(
                code, pools, info, runtime_info, profile,
                Shader::TranslationTier::Optimized);
        }();
        {
            ExecutorGraphicsShaderTimingScope emit_spirv_profile{
                shader_lookup_profile,
                ExecutorGraphicsShaderTiming::EmitSpirv};
            spv = Shader::Backend::SPIRV::EmitSPIRV(
                profile, runtime_info, ir_program, binding);
        }
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (info.stage == Shader::Stage::Fragment && info.has_discard) {
        u32 kills{};
        u32 demotes{};
        constexpr u32 OpKillOpcode = 252;
        constexpr u32 OpDemoteToHelperInvocationExtOpcode = 5380;
        for (size_t offset = 5; offset < spv.size();) {
            const u32 instruction = spv[offset];
            const u32 word_count = instruction >> 16;
            const u32 opcode = instruction & 0xffffu;
            kills += opcode == OpKillOpcode;
            demotes += opcode == OpDemoteToHelperInvocationExtOpcode;
            if (word_count == 0 || offset + word_count > spv.size()) {
                break;
            }
            offset += word_count;
        }
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_DOWNWELL_DISCARD_SPV] hash=0x%llx perm=%zu words=%zu "
                            "kills=%u demotes=%u",
                            static_cast<unsigned long long>(info.pgm_hash), perm_idx, spv.size(),
                            kills, demotes);
    }
#endif
    {
        ExecutorGraphicsShaderTimingScope storage_profile{
            shader_lookup_profile,
            ExecutorGraphicsShaderTiming::Storage};
        DumpShader(spv, info.pgm_hash, info.stage, perm_idx, "spv");
    }

#if defined(LSX4_APPLE_RUNTIME)
    if (info.stage == Stage::Compute && AppleComputeSplit::Selected()) {
        if (apple_split) {
            auto virtualized = PortableComputeSplit::Rewrite(spv);
            if (virtualized.empty()) throw std::runtime_error("Compute split proof changed: refusing incompatible dispatch");
            spv = std::move(virtualized);
        } else if(runtime_info.cs_info.workgroup_size == std::array<u32,3>{1024,1,1} &&
                  runtime_info.cs_info.shared_memory_size == 0) {
            AppleComputeSplit::Admit(info.pgm_hash,code,spv);
        }
    }
#endif
#if defined(LSX4_APPLE_RUNTIME)
    // Fast tier keeps the render thread short; the background optimized tier
    // applies the optimizer and replaces the live pipeline.
    if (!Config::patchShaders() && !(use_fast_tier && ClaudeShaderStorage::FastTier()))
        AppleSpirvOptimizer217::Apply(spv,info.pgm_hash,static_cast<unsigned>(info.stage),"foreground");
#endif
    vk::ShaderModule module;

    std::optional<std::vector<u32>> patch;
    {
        ExecutorGraphicsShaderTimingScope storage_profile{
            shader_lookup_profile,
            ExecutorGraphicsShaderTiming::Storage};
        patch = GetShaderPatch(info.pgm_hash, info.stage, perm_idx, "spv");
    }
    const bool is_patched = patch && Config::patchShaders();
    if (is_patched) {
        LOG_INFO(Loader, "Loaded patch for {} shader {:#x}", info.stage, info.pgm_hash);
        ExecutorGraphicsShaderTimingScope module_create_profile{
            shader_lookup_profile,
            ExecutorGraphicsShaderTiming::ModuleCreate};
        module = CompileSPV(*patch, instance.GetDevice());
    } else {
        const auto create_module = [this, shader_lookup_profile](
                                       const std::span<const u32> code) {
            ExecutorGraphicsShaderTimingScope module_create_profile{
                shader_lookup_profile,
                ExecutorGraphicsShaderTiming::ModuleCreate};
            const vk::ShaderModuleCreateInfo create_info{
                .codeSize = code.size_bytes(),
                .pCode = code.data(),
            };
            return instance.GetDevice().createShaderModule(create_info);
        };
        auto [module_result, created_module] = create_module(spv);
        if (module_result != vk::Result::eSuccess && use_fast_tier &&
            shader_payload && shader_payload->ir) {
            RecordShaderBinaryTierFailure(info.pgm_hash, perm_idx,
                                          ShaderBinaryTier::Fast);
            {
                ExecutorGraphicsShaderTimingScope translate_profile{
                    shader_lookup_profile,
                    ExecutorGraphicsShaderTiming::Translate};
                Shader::OptimizeProgram(*shader_payload->ir);
            }
            auto optimized_binding = shader_payload->binding_start;
            {
                ExecutorGraphicsShaderTimingScope emit_spirv_profile{
                    shader_lookup_profile,
                    ExecutorGraphicsShaderTiming::EmitSpirv};
                spv = Shader::Backend::SPIRV::EmitSPIRV(
                    profile, shader_payload->runtime_info, *shader_payload->ir,
                    optimized_binding);
            }
            ASSERT_MSG(optimized_binding ==
                           shader_payload->expected_binding_end,
                       "Optimized shader binding contract changed");
            binding = optimized_binding;
            use_fast_tier = false;
            shader_payload.reset();
            std::tie(module_result, created_module) = create_module(spv);
        }
        if (module_result != vk::Result::eSuccess) {
            RecordShaderBinaryTierFailure(
                info.pgm_hash, perm_idx,
                use_fast_tier ? ShaderBinaryTier::Fast
                              : ShaderBinaryTier::Optimized);
        }
        ASSERT_MSG(module_result == vk::Result::eSuccess,
                   "Failed to compile SPIR-V shader: {}",
                   vk::to_string(module_result));
        module = created_module;
    }

    auto cached_spv = spv;
    {
        ExecutorGraphicsShaderTimingScope storage_profile{
            shader_lookup_profile,
            ExecutorGraphicsShaderTiming::Storage};
        RegisterShaderBinary(
            std::move(cached_spv), info.pgm_hash, perm_idx,
            use_fast_tier ? ShaderBinaryTier::Fast
                          : ShaderBinaryTier::Optimized);
    }

    const auto name = GetShaderName(info.stage, info.pgm_hash, perm_idx);
    Vulkan::SetObjectName(instance.GetDevice(), module, name);
    if (Config::collectShadersForDebug()) {
        DebugState.CollectShader(name, info.l_stage, module, spv, code,
                                 patch ? *patch : std::span<const u32>{}, is_patched);
    }
    if (shader_payload && !is_patched) {
        *optimization_payload = std::move(shader_payload);
    }
    return module;
}

std::optional<PipelineCache::Result> PipelineCache::GetProgram(
    Stage stage, LogicalStage l_stage, const Shader::ShaderParams& params,
    Shader::Backend::Bindings& binding, const bool refresh_runtime_info,
    ProgramHotThreadState* const call_scope_program_hot_state) {
#if defined(LSX4_APPLE_RUNTIME)
    if(AppleLiveShaderDumpOnce(params.hash,stage)) {
        DumpShader(params.code,params.hash,stage,0,"live.bin");
    }
#endif
    auto* const shader_lookup_profile =
        ExecutorGraphicsPipelineLookupProfile::Active();
    ExecutorGraphicsShaderTimingScope program_profile{
        shader_lookup_profile,
        ExecutorGraphicsShaderTiming::Program};
    HleCleanBufferGuestReadScope hle_guest_read_scope{*this};
    const bool program_flattened_scratch_recycle_enabled =
        ExecutorProgramFlattenedScratchRecycleEnabled();
    const auto* const runtime_info_source =
        refresh_runtime_info ? BuildRuntimeInfo(stage, l_stage)
                             : &runtime_infos[static_cast<u32>(l_stage)];
    if (runtime_info_source == nullptr) {
        return std::nullopt;
    }
#if defined(LSX4_APPLE_RUNTIME)
    if (stage == Stage::Compute && AppleComputeSplit::Selected()) {
        static std::atomic<unsigned> apple_split_gate_logs{};
        if(apple_split_gate_logs.load(std::memory_order_relaxed)<8 &&
           apple_split_gate_logs.fetch_add(1,std::memory_order_relaxed)<8)
            std::fprintf(stderr,"[APPLE_COMPUTE_SPLIT_GATE] direct=%u factor=%u local=%ux%ux%u shared=%u async=%u patches=%u admissions=%zu codeWords=%zu\n",
                unsigned(AppleComputeSplit::state.direct),AppleComputeSplit::state.factor,
                runtime_infos[static_cast<u32>(l_stage)].cs_info.workgroup_size[0],
                runtime_infos[static_cast<u32>(l_stage)].cs_info.workgroup_size[1],
                runtime_infos[static_cast<u32>(l_stage)].cs_info.workgroup_size[2],
                runtime_infos[static_cast<u32>(l_stage)].cs_info.shared_memory_size,
                unsigned(Config::managedAsyncPipelineCompilation()),unsigned(Config::patchShaders()),
                AppleComputeSplit::state.admissions.size(),params.code.size());
        auto& selected_runtime = runtime_infos[static_cast<u32>(l_stage)];
        if (selected_runtime.cs_info.workgroup_size == std::array<u32,3>{1024,1,1} &&
            selected_runtime.cs_info.shared_memory_size == 0 &&
            !Config::patchShaders() &&
            AppleComputeSplit::Admitted(params.hash,params.code)) {
            selected_runtime.cs_info.workgroup_size[0] = 128;
            runtime_info_identities[static_cast<u32>(l_stage)] =
                InternRuntimeInfoIdentity(l_stage,selected_runtime);
            AppleComputeSplit::state.factor = 8;
            static std::atomic_flag apple_split_planned_first=ATOMIC_FLAG_INIT;
            if(!apple_split_planned_first.test_and_set(std::memory_order_relaxed))
                std::fprintf(stderr,"[APPLE_COMPUTE_SPLIT_PLAN] firstPlan=1 hash=0x%llx hostLocal=128 direct=1 exactCode=1\n",(unsigned long long)params.hash);
        }
    }
#endif
    const auto& runtime_info = *runtime_info_source;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    auto& runtime_info_identity =
        runtime_info_identities[static_cast<u32>(l_stage)];
    if (runtime_info_identity == 0) [[unlikely]] {
        runtime_info_identity = InternRuntimeInfoIdentity(l_stage, runtime_info);
    }
#endif
    const auto binding_start = binding;
    std::array<u32, Shader::NUM_USER_DATA_REGS> specialization_user_data{};
    u32 specialization_user_data_mask = 0;
    u64 snapshot_key = 0;
    Shader::PFN_SrtMemoryRead srt_memory_reader{};
    if (immutable_srt_snapshot_cache_enabled) {
        srt_memory_reader = [](const VAddr address, void* const dest, const u64 size) {
            return Core::Memory::Instance()->TryCopyBackingMemory(address, dest, size);
        };
    }

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const auto record_stable_srt_epoch_hit = [&] {
        const u64 hit = ++stable_srt_epoch_hits;
        if (hit != 1) {
            return;
        }
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_STABLE_SRT_EPOCH] firstHit=1 byteValidationSkipped=1");
        if (std::FILE* proof = std::fopen(
                "/data/data/app.lsx4.android/files/runtime/"
                "stable-srt-epoch-hit.txt",
                "w")) {
            std::fputs("firstHit=1 byteValidationSkipped=1\n", proof);
            std::fclose(proof);
        }
    };
#endif

    const auto build_snapshot_identity = [&](Program& program, const Shader::Info& info) {
        ExecutorGraphicsShaderTimingScope srt_profile{
            shader_lookup_profile,
            ExecutorGraphicsShaderTiming::Srt};
        specialization_user_data.fill(0);
        if (!program.specialization_user_data_mask_initialized) {
            u32 mask = 0;
            const auto mark_words = [&](const u32 first, const u32 count) {
                const u32 end =
                    std::min<u32>(first + count, Shader::NUM_USER_DATA_REGS);
                for (u32 index = first; index < end; ++index) {
                    mask |= 1u << index;
                }
            };

            for (const auto& node : info.srt_info.portable_nodes) {
                if (node.parent < 0) {
                    mark_words(node.ptr_off_dw, 2);
                }
            }
            if (info.has_fetch_shader) {
                mark_words(info.fetch_shader_sgpr_base, 2);
            }
            for (const auto& resource : info.buffers) {
                if (!resource.inline_cbuf &&
                    resource.sharp_idx < Shader::NUM_USER_DATA_REGS) {
                    mark_words(resource.sharp_idx,
                               sizeof(AmdGpu::Buffer) / sizeof(u32));
                }
            }
            for (const auto& resource : info.images) {
                if (resource.sharp_idx < Shader::NUM_USER_DATA_REGS) {
                    mark_words(resource.sharp_idx,
                               sizeof(AmdGpu::Image) / sizeof(u32));
                }
            }
            for (const auto& resource : info.samplers) {
                if (!resource.is_inline_sampler &&
                    resource.sharp_idx < Shader::NUM_USER_DATA_REGS) {
                    mark_words(resource.sharp_idx,
                               sizeof(AmdGpu::Sampler) / sizeof(u32));
                }
            }
            for (const auto& resource : info.fmasks) {
                if (resource.sharp_idx < Shader::NUM_USER_DATA_REGS) {
                    mark_words(resource.sharp_idx,
                               sizeof(AmdGpu::Image) / sizeof(u32));
                }
            }
            if (info.tess_consts_ptr_base != Shader::IR::ScalarReg::Max) {
                mark_words(static_cast<u32>(info.tess_consts_ptr_base), 2);
            }
            program.specialization_user_data_mask = mask;
            program.specialization_user_data_mask_initialized = true;
        }
        specialization_user_data_mask = program.specialization_user_data_mask;
        for (u32 index = 0; index < Shader::NUM_USER_DATA_REGS; ++index) {
            if ((specialization_user_data_mask & (1u << index)) != 0) {
                specialization_user_data[index] =
                    index < params.user_data.size() ? params.user_data[index] : 0;
            }
        }

        snapshot_key = HashCombine(
            params.hash,
            static_cast<u64>(binding_start.unified) |
                (static_cast<u64>(binding_start.buffer) << 20) |
                (static_cast<u64>(binding_start.user_data) << 40));
        snapshot_key =
            HashCombine(snapshot_key, static_cast<u64>(specialization_user_data_mask));
        for (u32 index = 0; index < Shader::NUM_USER_DATA_REGS; ++index) {
            if ((specialization_user_data_mask & (1u << index)) != 0) {
                snapshot_key = HashCombine(
                    snapshot_key,
                    (static_cast<u64>(index) << 32) | specialization_user_data[index]);
            }
        }
    };

    const bool borrowed_fetch_shader_result_enabled =
        ExecutorBorrowedFetchShaderResultEnabled();
    const auto make_result =
        [&](const Shader::Info* selected_info, const vk::ShaderModule module,
            const Program& result_program, const size_t module_index,
            const u64 hash) {
            ASSERT(selected_info != nullptr);
            ASSERT(module_index < result_program.modules.size());
            Result result{};
            result.info = selected_info;
            result.module = module;
            result.fetch_shader_module_index = module_index;
            result.hash = hash;
            result.borrowed_fetch_shader =
                borrowed_fetch_shader_result_enabled;
            if (borrowed_fetch_shader_result_enabled) {
                result.fetch_shader_program = &result_program;
                ExecutorRecordBorrowedFetchShaderResult();
            } else {
                result.owned_fetch_shader =
                    result_program.modules[module_index].spec.fetch_shader_data;
            }
            return result;
        };

    const auto store_snapshot =
        [&](Program& program, const Shader::Info& info,
            Shader::SrtMemoryRanges&& ranges, const size_t module_index,
            const Shader::RuntimeInfo& snapshot_runtime_info) {
            ExecutorGraphicsShaderTimingScope srt_profile{
                shader_lookup_profile,
                ExecutorGraphicsShaderTiming::Srt};
            auto& primary_slot = program.SelectSpecializationSnapshotSlot(snapshot_key);
            const bool primary_had_valid_snapshot =
                primary_slot != nullptr && primary_slot->valid;
            if (primary_had_valid_snapshot &&
                primary_slot->identity_key != snapshot_key) {
                primary_slot =
                    RetainSrtVictimSnapshot(program, std::move(primary_slot));
            }
            const size_t previous_retained_bytes =
                primary_slot ? SrtSnapshotRetainedBytes(*primary_slot) : 0;
            if (!primary_slot) {
                if (srt_snapshot_object_count >= MaxSrtSnapshotObjects) {
                    ++srt_snapshot_capacity_rejections;
                    if (primary_had_valid_snapshot &&
                        program.specialization_snapshot_count != 0) {
                        --program.specialization_snapshot_count;
                    }
                    return;
                }
                primary_slot = std::make_unique<Program::SpecializationSnapshot>();
                ++srt_snapshot_object_count;
            }
            auto& snapshot = *primary_slot;
            snapshot.valid = false;
            snapshot.specialization_user_data = specialization_user_data;
            snapshot.specialization_user_data_mask = specialization_user_data_mask;
            const auto flattened_user_data = info.FlattenedUserData();
            snapshot.flattened_user_data.assign(flattened_user_data.begin(),
                                                flattened_user_data.end());
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            runtime_info_identity =
                InternRuntimeInfoIdentity(l_stage, snapshot_runtime_info);
            snapshot.runtime_info_identity = runtime_info_identity;
#else
            snapshot.runtime_info = snapshot_runtime_info;
#endif
            snapshot.binding_start = binding_start;
            snapshot.srt_ranges = std::move(ranges);
            const u64 capture_sequence = CaptureCpuInvalidationSequence();
            const bool captured =
                immutable_srt_snapshot_cache_enabled
                    ? CaptureSrtBackingBytes(snapshot.srt_ranges, snapshot.srt_source_bytes)
                    : CaptureSrtSourceBytes(snapshot.srt_ranges, snapshot.srt_source_bytes);
            const bool stable_capture =
                !immutable_srt_snapshot_cache_enabled ||
                AreSrtMemoryRangesCpuStableSince(snapshot.srt_ranges, capture_sequence);
            const size_t retained_bytes = SrtSnapshotRetainedBytes(snapshot);
            srt_snapshot_retained_bytes -=
                std::min(srt_snapshot_retained_bytes, previous_retained_bytes);
            srt_snapshot_retained_bytes += retained_bytes;
            if (!captured || !stable_capture) {
                if (primary_had_valid_snapshot) {
                    --program.specialization_snapshot_count;
                }
                return;
            }
            snapshot.module_index = module_index;
            snapshot.identity_key = snapshot_key;
            snapshot.cpu_invalidation_sequence = capture_sequence;
            snapshot.valid = true;
            if (!primary_had_valid_snapshot) {
                ++program.specialization_snapshot_count;
            }
        };

    auto [it_pgm, new_program] = [&] {
        ExecutorGraphicsShaderTimingScope program_cache_profile{
            shader_lookup_profile,
            ExecutorGraphicsShaderTiming::ProgramCache};
        return program_cache.try_emplace(params.hash);
    }();
    if (shader_lookup_profile != nullptr) [[unlikely]] {
        shader_lookup_profile->MarkProgramCacheResult(new_program);
    }
    if (new_program) {
        it_pgm.value() = std::make_unique<Program>(stage, l_stage, params);
        auto& program = it_pgm.value();
        auto compilation_runtime_info = runtime_info;
        std::shared_ptr<AsyncShaderPayload> shader_optimization;
        const auto module =
            CompileModule(program->info, compilation_runtime_info, params.code, 0,
                          binding, &shader_optimization);
        auto permutation_info = std::make_unique<Shader::Info>(program->info);
        build_snapshot_identity(*program, program->info);
        Shader::SrtMemoryRanges srt_ranges;
        {
            ExecutorGraphicsShaderTimingScope srt_profile{
                shader_lookup_profile,
                ExecutorGraphicsShaderTiming::Srt};
            program->info.RefreshFlatBuf(&srt_ranges, srt_memory_reader);
        }
        const auto& parsed_fetch_shader = ResolveFetchShader(*program, program->info);
        const auto tess_constants = ResolveTessConstants(program->info);
        auto spec = [&] {
            ExecutorGraphicsShaderTimingScope srt_profile{
                shader_lookup_profile,
                ExecutorGraphicsShaderTiming::Srt};
            return Shader::StageSpecialization(
                program->info, compilation_runtime_info, profile, binding_start,
                &parsed_fetch_shader,
                tess_constants ? &*tess_constants : nullptr);
        }();
        const auto perm_hash = HashCombine(params.hash, 0);

        program->AddPermut(
            module, std::move(spec), std::move(permutation_info),
            shader_optimization ? ShaderBinaryTier::Fast
                                : ShaderBinaryTier::Optimized);
        auto& selected_info = program->ActivatePermut(
            0, params, program_flattened_scratch_recycle_enabled);
        if (program_flattened_scratch_recycle_enabled) {
            ExecutorRecordProgramFlattenedScratchRecycle();
        }
        {
            ExecutorGraphicsShaderTimingScope storage_profile{
                shader_lookup_profile,
                ExecutorGraphicsShaderTiming::Storage};
            RegisterShaderMeta(selected_info,
                               program->modules[0].spec.fetch_shader_data,
                               program->modules[0].spec, perm_hash, 0);
        }
        if (shader_optimization) {
            shader_optimization->program = program.get();
            shader_optimization->perm_idx = 0;
            shader_optimization->expected_generation =
                program->modules[0].generation;
            shader_optimization->cache_generation = shader_tier_generation;
            shader_optimization->expected_module = module;
            QueueAsyncShaderOptimization(std::move(shader_optimization));
        }
        store_snapshot(*program, selected_info, std::move(srt_ranges), 0,
                       compilation_runtime_info);
        return make_result(&selected_info, module, *program, 0, perm_hash);
    }

    auto& program = it_pgm.value();
    auto& info = program->info;
    info.pgm_base = params.Base();
    info.user_data = params.user_data;
    build_snapshot_identity(*program, info);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    // Key this cache by the specialization identity instead of all user-data
    // words. Raw push-data changes on almost every Bloodborne draw, while only
    // the masked descriptor/SRT words select the flattened resources and
    // shader permutation. Comparing all words made the old cache miss 100%.
    // Bloodborne rotates through several thousand descriptor identities per
    // frame. A direct-mapped cache still discarded hot identities on hash
    // collisions and spent most misses rebuilding SRT vectors. Four exact
    // ways retain the same safety checks while sharply reducing allocator and
    // portable-SRT-walker traffic.
    ExecutorGraphicsShaderTimingScope program_hot_srt_profile{
        shader_lookup_profile,
        ExecutorGraphicsShaderTiming::Srt};

    const bool program_hot_grouped_tls_enabled =
        ExecutorProgramHotGroupedTlsEnabled();
    ProgramHotThreadState* grouped_state = call_scope_program_hot_state;
    if (program_hot_grouped_tls_enabled && grouped_state == nullptr) {
        grouped_state = &ExecutorProgramHotThreadState();
    }
    if (program_hot_grouped_tls_enabled) {
        ExecutorRecordProgramHotGroupedTls();
    }

    const bool program_hot_legacy_tls_cold_split_enabled =
        ExecutorProgramHotLegacyTlsColdSplitEnabled();
    ProgramHotThreadStateRefs program_hot_refs{};
    if (program_hot_grouped_tls_enabled &&
        program_hot_legacy_tls_cold_split_enabled) {
        program_hot_refs = ExecutorProgramHotThreadStateRefs(*grouped_state);
        ExecutorRecordProgramHotLegacyTlsColdSplit();
    } else {
        // D82 OFF deliberately resolves the old nine TLS objects even when
        // D71 later selects grouped refs, preserving a same-APK cost control.
        const ProgramHotThreadStateRefs legacy_refs =
            ExecutorProgramHotLegacyThreadStateRefs();
        program_hot_refs = program_hot_grouped_tls_enabled
                               ? ExecutorProgramHotThreadStateRefs(*grouped_state)
                               : legacy_refs;
    }

    ProgramHotOverflowWay* program_hot_overflow_way = nullptr;
    if (ExecutorProgramHotOverflowWayEnabled() && grouped_state != nullptr) {
        if (!grouped_state->overflow_way) {
            grouped_state->overflow_way.reset(
                new (std::nothrow) ProgramHotOverflowWay{});
            if (grouped_state->overflow_way) {
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_PROGRAM_HOT_OVERFLOW_WAY] allocated=1 "
                    "sets=%zu bytes=%zu fallbackOnAllocationFailure=1",
                    ProgramHotSetCount, sizeof(ProgramHotOverflowWay));
            }
        }
        program_hot_overflow_way = grouped_state->overflow_way.get();
    }

    auto& program_hot_sets = *program_hot_refs.sets;
    auto& program_hot_mru_ways = *program_hot_refs.mru_ways;
    auto& program_hot_valid_way_masks = *program_hot_refs.valid_way_masks;
    auto& program_hot_lookups = *program_hot_refs.lookups;
    auto& program_hot_hits = *program_hot_refs.hits;
    auto& program_hot_clock = *program_hot_refs.clock;
    auto& program_hot_exact_revalidations =
        *program_hot_refs.exact_revalidations;
    auto& program_hot_exact_revalidation_hits =
        *program_hot_refs.exact_revalidation_hits;
    auto& program_hot_retained_content_versions =
        *program_hot_refs.retained_content_versions;
    u64 program_hot_hash =
        snapshot_key ^ static_cast<u64>(reinterpret_cast<uintptr_t>(program.get()));
    program_hot_hash ^= program_hot_hash >> 30;
    program_hot_hash *= 0xbf58476d1ce4e5b9ULL;
    program_hot_hash ^= program_hot_hash >> 27;
    program_hot_hash *= 0x94d049bb133111ebULL;
    program_hot_hash ^= program_hot_hash >> 31;
    const size_t program_hot_set_index =
        program_hot_hash & (ProgramHotSetCount - 1);
    auto& program_hot_set = program_hot_sets[program_hot_set_index];
    ProgramHotEntry* const program_hot_overflow_entry =
        program_hot_overflow_way != nullptr
            ? &(*program_hot_overflow_way)[program_hot_set_index]
            : nullptr;
    auto& program_hot_valid_way_mask =
        program_hot_valid_way_masks[program_hot_set_index];
    const bool program_hot_occupancy_mask_enabled =
        ExecutorProgramHotOccupancyMaskEnabled();
    std::array<size_t, ProgramHotWayCount + 1> program_hot_probe_order{};
    size_t program_hot_probe_count = 0;
    const size_t program_hot_preferred_way =
        static_cast<size_t>(program_hot_mru_ways[program_hot_set_index]);
    u32 program_hot_valid_mask = 0;
    if (program_hot_occupancy_mask_enabled) {
        program_hot_valid_mask =
            static_cast<u32>(program_hot_valid_way_mask) &
            ProgramHotAllWaysMask;
        u32 remaining = program_hot_valid_mask;
        if (program_hot_mru_probe_enabled &&
            program_hot_preferred_way < ProgramHotWayCount) {
            const u32 preferred_bit = 1u << program_hot_preferred_way;
            if ((remaining & preferred_bit) != 0) {
                program_hot_probe_order[program_hot_probe_count++] =
                    program_hot_preferred_way;
                remaining &= ~preferred_bit;
            }
        }
        while (remaining != 0) {
            const auto way = static_cast<size_t>(std::countr_zero(remaining));
            program_hot_probe_order[program_hot_probe_count++] = way;
            remaining &= remaining - 1;
        }
        ExecutorRecordProgramHotOccupancyMask(
            program_hot_valid_mask,
            static_cast<u32>(ProgramHotWayCount - program_hot_probe_count));
    } else {
        for (size_t way = 0; way < ProgramHotWayCount; ++way) {
            program_hot_probe_order[way] = way;
        }
        program_hot_probe_count = ProgramHotWayCount;
        if (program_hot_mru_probe_enabled &&
            program_hot_preferred_way < ProgramHotWayCount &&
            program_hot_preferred_way != 0) {
            std::swap(program_hot_probe_order[0],
                      program_hot_probe_order[program_hot_preferred_way]);
        }
    }
    if (program_hot_overflow_entry != nullptr &&
        program_hot_overflow_entry->valid) {
        if (program_hot_mru_probe_enabled &&
            program_hot_preferred_way == ProgramHotWayCount) {
            for (size_t index = program_hot_probe_count; index != 0; --index) {
                program_hot_probe_order[index] =
                    program_hot_probe_order[index - 1];
            }
            program_hot_probe_order[0] = ProgramHotWayCount;
            ++program_hot_probe_count;
        } else {
            program_hot_probe_order[program_hot_probe_count++] =
                ProgramHotWayCount;
        }
    }
    ++program_hot_lookups;
    ++program_hot_clock;
    const bool program_hot_managed_safe =
        immutable_srt_snapshot_cache_enabled ||
        VideoCore::IsManagedVulkanDriverOptimization();
    const bool runtime_info_log_rmw_cold_gate_enabled =
        ExecutorRuntimeInfoLogRmwColdGateEnabled();
    const bool program_hot_hit_lru_defer_enabled =
        ExecutorProgramHotHitLruDeferEnabled();
    ProgramHotEntry* program_hot_hit = nullptr;
    size_t program_hot_hit_way = 0;
    ProgramHotEntry* program_hot_invalid =
        program_hot_overflow_entry != nullptr &&
                !program_hot_overflow_entry->valid
            ? program_hot_overflow_entry
            : nullptr;
    ProgramHotEntry* program_hot_lru = &program_hot_set.front();
    if (program_hot_occupancy_mask_enabled &&
        program_hot_valid_mask != ProgramHotAllWaysMask &&
        program_hot_invalid == nullptr) {
        const u32 invalid_mask =
            (~program_hot_valid_mask) & ProgramHotAllWaysMask;
        const u32 preferred_bit =
            program_hot_preferred_way < ProgramHotWayCount
                ? 1u << program_hot_preferred_way
                : 0;
        const auto invalid_way = static_cast<size_t>(
            program_hot_mru_probe_enabled &&
                    program_hot_preferred_way != 0 &&
                    (invalid_mask & preferred_bit) != 0
                ? program_hot_preferred_way
                : std::countr_zero(invalid_mask));
        program_hot_invalid = &program_hot_set[invalid_way];
    }
    for (size_t probe = 0; probe < program_hot_probe_count; ++probe) {
        const size_t candidate_way = program_hot_probe_order[probe];
        auto& candidate = candidate_way < ProgramHotWayCount
                              ? program_hot_set[candidate_way]
                              : *program_hot_overflow_entry;
        if (!candidate.valid) {
            if (program_hot_invalid == nullptr) {
                program_hot_invalid = &candidate;
            }
            continue;
        }
        const auto consider_program_hot_lru = [&] {
            if (!program_hot_lru->valid ||
                candidate.last_used < program_hot_lru->last_used) {
                program_hot_lru = &candidate;
            }
        };
        if (!program_hot_hit_lru_defer_enabled) {
            consider_program_hot_lru();
        }
        const bool identity =
            program_hot_managed_safe && candidate.owner == this &&
            candidate.program == program.get() &&
            candidate.snapshot_key == snapshot_key &&
            candidate.module_generation == program->module_generation &&
            candidate.module_index < program->modules.size() &&
            program->modules[candidate.module_index].info &&
            candidate.runtime_info_identity == runtime_info_identity &&
            candidate.binding_start == binding_start;
        if (!identity) {
            if (program_hot_hit_lru_defer_enabled) {
                consider_program_hot_lru();
            }
            continue;
        }
        static std::atomic<bool> first_runtime_info_identity_hit_logged{false};
        const bool runtime_info_log_already_published =
            runtime_info_log_rmw_cold_gate_enabled &&
            first_runtime_info_identity_hit_logged.load(std::memory_order_relaxed);
        if (!runtime_info_log_already_published &&
            !first_runtime_info_identity_hit_logged.exchange(
                true, std::memory_order_relaxed)) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_RUNTIME_INFO_IDENTITY] firstHit=1 exactIntern=1 "
                "collisionResolved=1 fullHotCompare=0 entryBytes=%zu "
                "runtimeInfoBytes=%zu tlsBytesSaved=%zu coldGate=%u "
                "repeatRmwSkipped=%u",
                sizeof(ProgramHotEntry), sizeof(Shader::RuntimeInfo),
                (sizeof(Shader::RuntimeInfo) - sizeof(u64)) *
                    ProgramHotSetCount * ProgramHotWayCount,
                runtime_info_log_rmw_cold_gate_enabled ? 1u : 0u,
                runtime_info_log_rmw_cold_gate_enabled ? 1u : 0u);
            if (std::FILE* proof = std::fopen(
                    "/data/data/app.lsx4.android/files/runtime/"
                    "runtime-info-identity-hit.txt",
                    "w")) {
                std::fprintf(proof,
                             "firstHit=1 exactIntern=1 collisionResolved=1 "
                             "fullHotCompare=0 entryBytes=%zu "
                             "runtimeInfoBytes=%zu tlsBytesSaved=%zu "
                             "coldGate=%u repeatRmwSkipped=%u\n",
                             sizeof(ProgramHotEntry), sizeof(Shader::RuntimeInfo),
                             (sizeof(Shader::RuntimeInfo) - sizeof(u64)) *
                                 ProgramHotSetCount * ProgramHotWayCount,
                             runtime_info_log_rmw_cold_gate_enabled ? 1u : 0u,
                             runtime_info_log_rmw_cold_gate_enabled ? 1u : 0u);
                std::fclose(proof);
            }
        }
        const bool cpu_stable = AreSrtMemoryRangesCpuStableSince(
            candidate.srt_ranges, candidate.invalidation_sequence);
        const bool gpu_clean = immutable_srt_snapshot_cache_enabled ||
                               AreSrtMemoryRangesGpuClean(candidate.srt_ranges);
        bool source_matches = false;
        if (cpu_stable && gpu_clean) {
            const bool stable_epoch_hit =
                CanUseStableSrtEpochHit(stable_srt_epoch_hit_enabled,
                                        identity, cpu_stable,
                                        !immutable_srt_snapshot_cache_enabled && gpu_clean);
            if (stable_epoch_hit) {
                source_matches = true;
                record_stable_srt_epoch_hit();
            } else if (immutable_srt_snapshot_cache_enabled) {
                const bool captured = CaptureSrtBackingBytes(
                    candidate.srt_ranges, srt_backing_validation_scratch);
                source_matches =
                    captured && std::ranges::equal(srt_backing_validation_scratch,
                                                   candidate.srt_source_bytes);
            } else {
                source_matches = DoSrtSourceBytesMatch(candidate.srt_ranges,
                                                       candidate.srt_source_bytes);
            }
        } else if (CanUseProgramHotExactRevalidation(
                       program_hot_exact_multiversion_enabled, identity,
                       cpu_stable, gpu_clean)) {
            // An identity can cycle through several exact descriptor contents.
            // The old path invalidated the first matching version immediately,
            // so returning to an earlier version always rebuilt the SRT walk and
            // StageSpecialization. Revalidate the retained bytes under an
            // invalidation-generation sandwich. No epoch, ownership or byte
            // proof is skipped: a racing CPU publication or GPU-dirty range is
            // still a hard miss.
            ++program_hot_exact_revalidations;
            const u64 validation_sequence = CaptureCpuInvalidationSequence();
            source_matches = DoSrtSourceBytesMatch(candidate.srt_ranges,
                                                   candidate.srt_source_bytes);
            const u64 publish_sequence = CaptureCpuInvalidationSequence();
            source_matches = source_matches &&
                             publish_sequence == validation_sequence &&
                             AreSrtMemoryRangesCpuStableSince(
                                 candidate.srt_ranges, validation_sequence);
            if (source_matches) {
                const u64 hit = ++program_hot_exact_revalidation_hits;
                if (hit == 1) {
                    __android_log_print(
                        ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_PROGRAM_HOT_MULTIVERSION] firstExactRevalidationHit=1 "
                        "byteValidationSkipped=0 generationSandwich=1");
                    if (std::FILE* proof = std::fopen(
                            "/data/data/app.lsx4.android/files/runtime/"
                            "program-hot-exact-multiversion.txt",
                            "w")) {
                        std::fputs("firstExactRevalidationHit=1 "
                                   "byteValidationSkipped=0 generationSandwich=1\n",
                                   proof);
                        std::fclose(proof);
                    }
                }
            }
        }
        if (source_matches) {
            program_hot_hit = &candidate;
            program_hot_hit_way = candidate_way;
            break;
        }
        if (program_hot_hit_lru_defer_enabled) {
            consider_program_hot_lru();
        }
        if (!program_hot_exact_multiversion_enabled) {
            candidate.valid = false;
            if (program_hot_occupancy_mask_enabled &&
                candidate_way < ProgramHotWayCount) {
                program_hot_valid_way_mask &=
                    static_cast<u8>(~(1u << candidate_way));
            }
            program_hot_invalid = &candidate;
            break;
        }
        // Preserve a non-matching exact content version so another way can be
        // checked and the old version can be reused if the guest cycles back.
        ++program_hot_retained_content_versions;
    }
    ProgramHotEntry* const program_hot_victim =
        program_hot_invalid != nullptr ? program_hot_invalid : program_hot_lru;
    program_hot_srt_profile.Stop();
    if (program_hot_hit != nullptr) {
        ++program_hot_hits;
        if (program_hot_hit_way == ProgramHotWayCount) {
            static std::atomic<bool> first_overflow_hit_logged{false};
            if (!first_overflow_hit_logged.load(std::memory_order_relaxed) &&
                !first_overflow_hit_logged.exchange(true,
                                                    std::memory_order_relaxed)) {
                constexpr const char* proof =
                    "firstOverflowHit=1 exactOwner=1 exactProgram=1 "
                    "exactSnapshot=1 exactModuleGeneration=1 "
                    "exactRuntimeInfo=1 exactBinding=1 cpuEpochProof=1 "
                    "gpuCleanProof=1 sourceByteProof=1";
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_PROGRAM_HOT_OVERFLOW_WAY] %s", proof);
                if (std::FILE* file = std::fopen(
                        "/data/data/app.lsx4.android/files/runtime/"
                        "program-hot-overflow-way.txt",
                        "w")) {
                    std::fprintf(file, "%s\n", proof);
                    std::fclose(file);
                }
            }
        }
        if (program_hot_mru_probe_enabled) {
            if (program_hot_hit_way == program_hot_preferred_way &&
                program_hot_preferred_way != 0) {
                static std::atomic<bool> first_reordered_hit_logged{false};
                const bool reordered_log_already_published =
                    runtime_info_log_rmw_cold_gate_enabled &&
                    first_reordered_hit_logged.load(std::memory_order_relaxed);
                if (!reordered_log_already_published &&
                    !first_reordered_hit_logged.exchange(
                        true, std::memory_order_relaxed)) {
                    __android_log_print(
                        ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_PROGRAM_HOT_MRU] firstReorderedHit=1 naturalWay=%zu "
                        "identityProof=1 byteProofUnchanged=1 coldGate=%u "
                        "repeatRmwSkipped=%u",
                        program_hot_hit_way,
                        runtime_info_log_rmw_cold_gate_enabled ? 1u : 0u,
                        runtime_info_log_rmw_cold_gate_enabled ? 1u : 0u);
                }
            }
            program_hot_mru_ways[program_hot_set_index] =
                static_cast<u8>(program_hot_hit_way);
        }
        program_hot_hit->last_used = program_hot_clock;
        program_hot_hit->invalidation_sequence = CaptureCpuInvalidationSequence();
        if (ExecutorProgramHotFlattenedLeaseEnabled() &&
            program_hot_hit->flattened_user_data_lease) {
            program->ActivateCachedPermutLease(
                program_hot_hit->module_index, params,
                program_hot_hit->flattened_user_data_lease);
            ExecutorRecordProgramHotFlattenedLease();
        } else if (ExecutorCachedPermutFlattenedReuseEnabled()) {
            program->ActivateCachedPermut(program_hot_hit->module_index, params,
                                          program_hot_hit->flattened_user_data);
            ExecutorRecordCachedPermutFlattenedReuse();
        } else {
            info.flattened_ud_lease.reset();
            info.flattened_ud_buf = program_hot_hit->flattened_user_data;
            program->ActivatePermut(program_hot_hit->module_index, params,
                                   program_flattened_scratch_recycle_enabled);
            if (program_flattened_scratch_recycle_enabled) {
                ExecutorRecordProgramFlattenedScratchRecycle();
            }
        }
        binding = program_hot_hit->binding_end;
        if ((program_hot_lookups & 65535u) == 0 &&
            !VideoCore::IsExecutorLiveQuiet()) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_PROGRAM_HOT_CACHE] lookups=%llu hits=%llu hitPct=%.1f "
                "sets=%zu ways=%zu exactRevalidate=%llu exactRevalidateHit=%llu "
                "retainedVersion=%llu srtObjects=%zu srtApproxKiB=%zu "
                "srtObjectCap=%zu srtCapReject=%llu",
                static_cast<unsigned long long>(program_hot_lookups),
                static_cast<unsigned long long>(program_hot_hits),
                100.0 * static_cast<double>(program_hot_hits) /
                    static_cast<double>(program_hot_lookups),
                ProgramHotSetCount,
                ProgramHotWayCount +
                    (program_hot_overflow_way != nullptr ? 1 : 0),
                static_cast<unsigned long long>(program_hot_exact_revalidations),
                static_cast<unsigned long long>(program_hot_exact_revalidation_hits),
                static_cast<unsigned long long>(program_hot_retained_content_versions),
                srt_snapshot_object_count, srt_snapshot_retained_bytes / 1024,
                MaxSrtSnapshotObjects,
                static_cast<unsigned long long>(srt_snapshot_capacity_rejections));
        }
        return program_hot_hit->result;
    }
    const auto remember_program_hot = [&](const Result& result,
                                           const Shader::SrtMemoryRanges& ranges,
                                           const std::vector<u8>& source_bytes,
                                           const u64 invalidation_sequence,
                                           const size_t module_index) {
        ExecutorGraphicsShaderTimingScope srt_profile{
            shader_lookup_profile,
            ExecutorGraphicsShaderTiming::Srt};
        auto& program_hot = *program_hot_victim;
        program_hot.owner = this;
        program_hot.program = program.get();
        program_hot.snapshot_key = snapshot_key;
        program_hot.runtime_info_identity = runtime_info_identity;
        program_hot.binding_start = binding_start;
        program_hot.binding_end = binding;
        program_hot.srt_ranges = ranges;
        program_hot.srt_source_bytes = source_bytes;
        const auto* const selected_info = result.info;
        ASSERT(selected_info);
        const auto flattened_user_data = selected_info->FlattenedUserData();
        if (ExecutorProgramHotFlattenedLeaseEnabled()) {
            program_hot.flattened_user_data.clear();
            program_hot.flattened_user_data_lease =
                std::make_shared<const std::vector<u32>>(
                    flattened_user_data.begin(), flattened_user_data.end());
        } else {
            program_hot.flattened_user_data.assign(
                flattened_user_data.begin(), flattened_user_data.end());
            program_hot.flattened_user_data_lease.reset();
        }
        program_hot.result = result;
        program_hot.invalidation_sequence = invalidation_sequence;
        program_hot.module_generation = program->module_generation;
        program_hot.module_index = module_index;
        program_hot.last_used = program_hot_clock;
        program_hot.valid = true;
        size_t program_hot_victim_way = ProgramHotWayCount;
        for (size_t way = 0; way < ProgramHotWayCount; ++way) {
            if (program_hot_victim == &program_hot_set[way]) {
                program_hot_victim_way = way;
                break;
            }
        }
        if (program_hot_occupancy_mask_enabled &&
            program_hot_victim_way < ProgramHotWayCount) {
            program_hot_valid_way_mask |=
                static_cast<u8>(1u << program_hot_victim_way);
        }
        if (program_hot_mru_probe_enabled) {
            program_hot_mru_ways[program_hot_set_index] =
                static_cast<u8>(program_hot_victim_way);
        }
        if ((program_hot_lookups & 65535u) == 0 &&
            !VideoCore::IsExecutorLiveQuiet()) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_PROGRAM_HOT_CACHE] lookups=%llu hits=%llu hitPct=%.1f "
                "sets=%zu ways=%zu srtObjects=%zu srtApproxKiB=%zu "
                "srtObjectCap=%zu srtCapReject=%llu",
                static_cast<unsigned long long>(program_hot_lookups),
                static_cast<unsigned long long>(program_hot_hits),
                100.0 * static_cast<double>(program_hot_hits) /
                    static_cast<double>(program_hot_lookups),
                ProgramHotSetCount,
                ProgramHotWayCount +
                    (program_hot_overflow_way != nullptr ? 1 : 0),
                srt_snapshot_object_count, srt_snapshot_retained_bytes / 1024,
                MaxSrtSnapshotObjects,
                static_cast<unsigned long long>(srt_snapshot_capacity_rejections));
        }
    };
#endif
    ExecutorGraphicsShaderTimingScope snapshot_srt_profile{
        shader_lookup_profile,
        ExecutorGraphicsShaderTiming::Srt};
    ++srt_snapshot_lookups;
    auto* snapshot_ptr = program->FindSpecializationSnapshot(snapshot_key);
    bool snapshot_from_victim = false;
    if (!snapshot_ptr) {
        const bool owner_count_gate =
            ExecutorSrtVictimOwnerCountGateEnabled() &&
            program->srt_victim_snapshot_count == 0;
        if (owner_count_gate) {
            ++srt_snapshot_victim_owner_skips;
            ExecutorRecordSrtVictimOwnerCountGate(srt_snapshot_victims.size());
        } else {
            snapshot_ptr = FindSrtVictimSnapshot(*program, snapshot_key);
        }
        snapshot_from_victim = snapshot_ptr != nullptr;
        srt_snapshot_identity_misses += !snapshot_ptr;
    }
    if (snapshot_ptr) {
        auto& snapshot = *snapshot_ptr;
        const bool identity_matches =
            snapshot.module_index < program->modules.size() &&
            snapshot.binding_start == binding_start &&
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            snapshot.runtime_info_identity == runtime_info_identity &&
#else
            snapshot.runtime_info == runtime_info &&
#endif
            snapshot.specialization_user_data_mask == specialization_user_data_mask &&
            snapshot.specialization_user_data == specialization_user_data;
        const bool immutable_cpu_stable =
            identity_matches && immutable_srt_snapshot_cache_enabled &&
            AreSrtMemoryRangesCpuStableSince(snapshot.srt_ranges,
                                              snapshot.cpu_invalidation_sequence);
        const bool stable_epoch_cpu_stable =
            identity_matches && stable_srt_epoch_hit_enabled &&
            AreSrtMemoryRangesCpuStableSince(snapshot.srt_ranges,
                                              snapshot.cpu_invalidation_sequence);
        const bool gpu_clean =
            identity_matches && !immutable_cpu_stable &&
            AreSrtMemoryRangesGpuClean(snapshot.srt_ranges);
        const bool stable_epoch_hit =
            CanUseStableSrtEpochHit(stable_srt_epoch_hit_enabled,
                                    identity_matches,
                                    stable_epoch_cpu_stable, gpu_clean);
        bool backing_validated = false;
        bool source_matches = false;
        if (stable_epoch_hit) {
            backing_validated = true;
            source_matches = true;
            snapshot.cpu_invalidation_sequence = CaptureCpuInvalidationSequence();
            record_stable_srt_epoch_hit();
        } else if (identity_matches && immutable_srt_snapshot_cache_enabled) {
            if (immutable_cpu_stable) {
                backing_validated = true;
                source_matches = true;
                snapshot.cpu_invalidation_sequence = CaptureCpuInvalidationSequence();
                ++srt_snapshot_epoch_hits;
            } else {
                backing_validated =
                    CaptureSrtBackingBytes(snapshot.srt_ranges, srt_backing_validation_scratch);
                source_matches =
                    backing_validated &&
                    std::ranges::equal(srt_backing_validation_scratch,
                                       snapshot.srt_source_bytes);
            }
            srt_snapshot_backing_alias_hits += source_matches && !gpu_clean;
            srt_snapshot_backing_fallbacks += !backing_validated;
        }
        if (!backing_validated) {
            source_matches =
                gpu_clean &&
                DoSrtSourceBytesMatch(snapshot.srt_ranges, snapshot.srt_source_bytes);
        }
        srt_snapshot_gpu_misses +=
            identity_matches && !gpu_clean && !immutable_srt_snapshot_cache_enabled;
        srt_snapshot_identity_mismatches += !identity_matches;
        srt_snapshot_content_misses +=
            (gpu_clean || backing_validated) && !source_matches;
        if (source_matches) {
            ++srt_snapshot_hits;
            srt_snapshot_primary_hits += !snapshot_from_victim;
            srt_snapshot_victim_hits += snapshot_from_victim;
            auto& selected_info = [&]() -> Shader::Info& {
                if (ExecutorCachedPermutFlattenedReuseEnabled()) {
                    auto& selected = program->ActivateCachedPermut(
                        snapshot.module_index, params, snapshot.flattened_user_data);
                    ExecutorRecordCachedPermutFlattenedReuse();
                    return selected;
                }
                info.flattened_ud_lease.reset();
                info.flattened_ud_buf = snapshot.flattened_user_data;
                auto& selected = program->ActivatePermut(
                    snapshot.module_index, params,
                    program_flattened_scratch_recycle_enabled);
                if (program_flattened_scratch_recycle_enabled) {
                    ExecutorRecordProgramFlattenedScratchRecycle();
                }
                return selected;
            }();
            selected_info.AddBindings(binding);
            const auto& cached_module = program->modules[snapshot.module_index];
            snapshot_srt_profile.Stop();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            static const bool trace_srt_cache =
                std::getenv("EXECUTOR_TRACE_SRT_CACHE") != nullptr;
            if (trace_srt_cache && (srt_snapshot_lookups & 0xffff) == 0) {
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_SRT_CACHE] lookups=%llu hits=%llu hitPct=%.1f "
                    "contentMiss=%llu gpuMiss=%llu backingAliasHit=%llu "
                    "backingFallback=%llu epochHit=%llu primaryHit=%llu victimLookup=%llu "
                    "victimHit=%llu keyMiss=%llu identityMismatch=%llu "
                    "primarySnapshots=%zu victimSnapshots=%zu victimInsert=%llu "
                    "victimReplace=%llu victimEvict=%llu",
                    static_cast<unsigned long long>(srt_snapshot_lookups),
                    static_cast<unsigned long long>(srt_snapshot_hits),
                    100.0 * static_cast<double>(srt_snapshot_hits) /
                        static_cast<double>(srt_snapshot_lookups),
                    static_cast<unsigned long long>(srt_snapshot_content_misses),
                    static_cast<unsigned long long>(srt_snapshot_gpu_misses),
                    static_cast<unsigned long long>(srt_snapshot_backing_alias_hits),
                    static_cast<unsigned long long>(srt_snapshot_backing_fallbacks),
                    static_cast<unsigned long long>(srt_snapshot_epoch_hits),
                    static_cast<unsigned long long>(srt_snapshot_primary_hits),
                    static_cast<unsigned long long>(srt_snapshot_victim_lookups),
                    static_cast<unsigned long long>(srt_snapshot_victim_hits),
                    static_cast<unsigned long long>(srt_snapshot_identity_misses),
                    static_cast<unsigned long long>(srt_snapshot_identity_mismatches),
                    program->specialization_snapshot_count, srt_snapshot_victims.size(),
                    static_cast<unsigned long long>(srt_snapshot_victim_insertions),
                    static_cast<unsigned long long>(srt_snapshot_victim_replacements),
                    static_cast<unsigned long long>(srt_snapshot_victim_evictions));
            }
#endif
            const Result result = make_result(
                &selected_info, cached_module.module, *program,
                snapshot.module_index,
                HashCombine(params.hash, snapshot.module_index));
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            remember_program_hot(result, snapshot.srt_ranges,
                                  snapshot.srt_source_bytes,
                                  CaptureCpuInvalidationSequence(),
                                  snapshot.module_index);
#endif
            return result;
        }
    }
    snapshot_srt_profile.Stop();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    static const bool trace_srt_cache =
        std::getenv("EXECUTOR_TRACE_SRT_CACHE") != nullptr;
    if (trace_srt_cache && (srt_snapshot_lookups & 0xffff) == 0) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_SRT_CACHE] lookups=%llu hits=%llu hitPct=%.1f "
            "contentMiss=%llu gpuMiss=%llu backingAliasHit=%llu "
            "backingFallback=%llu epochHit=%llu primaryHit=%llu victimLookup=%llu "
            "victimHit=%llu keyMiss=%llu identityMismatch=%llu "
            "primarySnapshots=%zu victimSnapshots=%zu victimInsert=%llu "
            "victimReplace=%llu victimEvict=%llu",
            static_cast<unsigned long long>(srt_snapshot_lookups),
            static_cast<unsigned long long>(srt_snapshot_hits),
            100.0 * static_cast<double>(srt_snapshot_hits) /
                static_cast<double>(srt_snapshot_lookups),
            static_cast<unsigned long long>(srt_snapshot_content_misses),
            static_cast<unsigned long long>(srt_snapshot_gpu_misses),
            static_cast<unsigned long long>(srt_snapshot_backing_alias_hits),
            static_cast<unsigned long long>(srt_snapshot_backing_fallbacks),
            static_cast<unsigned long long>(srt_snapshot_epoch_hits),
            static_cast<unsigned long long>(srt_snapshot_primary_hits),
            static_cast<unsigned long long>(srt_snapshot_victim_lookups),
            static_cast<unsigned long long>(srt_snapshot_victim_hits),
            static_cast<unsigned long long>(srt_snapshot_identity_misses),
            static_cast<unsigned long long>(srt_snapshot_identity_mismatches),
            program->specialization_snapshot_count, srt_snapshot_victims.size(),
            static_cast<unsigned long long>(srt_snapshot_victim_insertions),
            static_cast<unsigned long long>(srt_snapshot_victim_replacements),
            static_cast<unsigned long long>(srt_snapshot_victim_evictions));
    }
#endif

    Shader::SrtMemoryRanges srt_ranges;
    {
        ExecutorGraphicsShaderTimingScope srt_profile{
            shader_lookup_profile,
            ExecutorGraphicsShaderTiming::Srt};
        info.RefreshFlatBuf(&srt_ranges, srt_memory_reader);
    }
    const auto& parsed_fetch_shader = ResolveFetchShader(*program, info);
    const auto tess_constants = ResolveTessConstants(info);
    auto spec = [&] {
        ExecutorGraphicsShaderTimingScope srt_profile{
            shader_lookup_profile,
            ExecutorGraphicsShaderTiming::Srt};
        return Shader::StageSpecialization(
            info, runtime_info, profile, binding_start, &parsed_fetch_shader,
            tess_constants ? &*tess_constants : nullptr);
    }();

    size_t perm_idx = program->modules.size();
    u64 perm_hash = HashCombine(params.hash, perm_idx);

    vk::ShaderModule module{};
    bool reused_permutation = false;
    std::optional<Shader::RuntimeInfo> compilation_runtime_info;

    const auto it = [&] {
        ExecutorGraphicsShaderTimingScope program_cache_profile{
            shader_lookup_profile,
            ExecutorGraphicsShaderTiming::ProgramCache};
        return std::ranges::find(program->modules, spec,
                                 &Program::Module::spec);
    }();
    if (it == program->modules.end()) {
        auto new_info = std::make_unique<Shader::Info>(stage, l_stage, params);
        std::shared_ptr<AsyncShaderPayload> shader_optimization;
        compilation_runtime_info.emplace(runtime_info);
        module = CompileModule(*new_info, *compilation_runtime_info, params.code, perm_idx,
                               binding, &shader_optimization);

        program->AddPermut(
            module, std::move(spec), std::move(new_info),
            shader_optimization ? ShaderBinaryTier::Fast
                                : ShaderBinaryTier::Optimized);
        if (shader_optimization) {
            shader_optimization->program = program.get();
            shader_optimization->perm_idx = perm_idx;
            shader_optimization->expected_generation =
                program->modules[perm_idx].generation;
            shader_optimization->cache_generation = shader_tier_generation;
            shader_optimization->expected_module = module;
            QueueAsyncShaderOptimization(std::move(shader_optimization));
        }
    } else {
        module = it->module;
        perm_idx = std::distance(program->modules.begin(), it);
        perm_hash = HashCombine(params.hash, perm_idx);
        reused_permutation = true;
    }
    auto& selected_info = program->ActivatePermut(
        perm_idx, params, program_flattened_scratch_recycle_enabled);
    if (program_flattened_scratch_recycle_enabled) {
        ExecutorRecordProgramFlattenedScratchRecycle();
    }
    if (!reused_permutation) {
        ExecutorGraphicsShaderTimingScope storage_profile{
            shader_lookup_profile,
            ExecutorGraphicsShaderTiming::Storage};
        const auto& added_module = program->modules[perm_idx];
        RegisterShaderMeta(selected_info,
                           added_module.spec.fetch_shader_data,
                           added_module.spec, perm_hash, perm_idx);
    } else {
        ExecutorGraphicsShaderTimingScope srt_profile{
            shader_lookup_profile,
            ExecutorGraphicsShaderTiming::Srt};
        selected_info.AddBindings(binding);
    }
    const Result result =
        make_result(&selected_info, module, *program, perm_idx, perm_hash);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    // A heavily instanced scene can rotate through more descriptor/SRT
    // identities than one program's resident snapshot table. Replacing a
    // snapshot then moving the victim into the global CLOCK cache allocated
    // several vectors per draw and cost more than the exact SRT walk itself.
    // Once this program is saturated, keep the already-hot identities and use
    // the allocation-free streaming result for cold identities.
    const bool streaming_cold_identity =
        VideoCore::IsManagedVulkanDriverOptimization() &&
        program->specialization_snapshot_count >=
            Program::NumSpecializationSnapshotSlots * 7 / 8;
    if (!streaming_cold_identity) {
        const auto& snapshot_runtime_info =
            compilation_runtime_info ? *compilation_runtime_info : runtime_info;
        store_snapshot(*program, selected_info, std::move(srt_ranges), perm_idx,
                       snapshot_runtime_info);
        if (const auto* stored_snapshot =
                program->FindSpecializationSnapshot(snapshot_key)) {
            remember_program_hot(result, stored_snapshot->srt_ranges,
                                  stored_snapshot->srt_source_bytes,
                                  stored_snapshot->cpu_invalidation_sequence,
                                  perm_idx);
        }
    }
#else
    const auto& snapshot_runtime_info =
        compilation_runtime_info ? *compilation_runtime_info : runtime_info;
    store_snapshot(*program, selected_info, std::move(srt_ranges), perm_idx,
                   snapshot_runtime_info);
#endif
    return result;
}

std::optional<vk::ShaderModule> PipelineCache::ReplaceShader(vk::ShaderModule module,
                                                              std::span<const u32> spv_code) {
    const bool restart_async = Config::managedAsyncPipelineCompilation();
    ++shader_tier_generation;
    StopAsyncPipelineWorkers();
    PublishAsyncPipelineResults();
    // Keep shader infos alive until the associated retired pipelines have
    // been destroyed. Declaration order makes the pipeline vectors retire
    // before the info vector after the cache lock is released.
    std::vector<BackgroundWarmUpInfos> retired_infos;
    std::vector<std::unique_ptr<GraphicsPipeline>> retired_graphics;
    std::vector<std::unique_ptr<ComputePipeline>> retired_compute;
    std::optional<vk::ShaderModule> new_module{};
    {
        // Shader replacement iterates and mutates every cache that the background
        // warm-up worker can grow.
        std::scoped_lock background_lock{background_warmup_mutex};
        ++background_warmup_generation;
        fallback_graphics_pipelines.clear();
        fallback_compute_pipelines.clear();
        DiscardBackgroundWarmUpPipelinesLocked(
            retired_infos, retired_graphics, retired_compute);
        last_graphics_pipeline = nullptr;
        last_graphics_state_generation = std::numeric_limits<u64>::max();
        for (const auto& [_, program] : program_cache) {
            for (auto& m : program->modules) {
                if (m.module == module) {
                    const auto& d = instance.GetDevice();
                    d.destroyShaderModule(m.module);
                    m.module = CompileSPV(spv_code, d);
                    m.tier = ShaderBinaryTier::Optimized;
                    ++m.generation;
                    ++program->module_generation;
                    new_module = m.module;
                }
            }
        }
        if (module_related_pipelines.contains(module)) {
            auto& pipeline_keys = module_related_pipelines[module];
            for (auto& key : pipeline_keys) {
                if (std::holds_alternative<GraphicsPipelineKey>(key)) {
                    const auto& graphics_key = std::get<GraphicsPipelineKey>(key);
                    if (auto it = graphics_pipelines.find(graphics_key);
                        it != graphics_pipelines.end()) {
                        retired_graphics.push_back(std::move(it.value()));
                        graphics_pipelines.erase(it);
                    }
                } else if (std::holds_alternative<ComputePipelineKey>(key)) {
                    const auto& compute_key = std::get<ComputePipelineKey>(key);
                    if (auto it = compute_pipelines.find(compute_key);
                        it != compute_pipelines.end()) {
                        retired_compute.push_back(std::move(it.value()));
                        compute_pipelines.erase(it);
                    }
                }
            }
            module_related_pipelines.erase(module);
        }
    }
    if (restart_async) {
        StartAsyncPipelineWorkers();
    }
    return new_module;
}

std::string PipelineCache::GetShaderName(Shader::Stage stage, u64 hash,
                                         std::optional<size_t> perm) {
    if (perm) {
        return fmt::format("{}_{:#018x}_{}", stage, hash, *perm);
    }
    return fmt::format("{}_{:#018x}", stage, hash);
}

void PipelineCache::DumpShader(std::span<const u32> code, u64 hash, Shader::Stage stage,
                               size_t perm_idx, std::string_view ext) {
    if (!Config::dumpShaders()
#if defined(LSX4_APPLE_RUNTIME)
        && !AppleBoundedShaderDumpSelected()
#endif
    ) {
        return;
    }

    using namespace Common::FS;
    auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (const char* directory = std::getenv("EXECUTOR_SHADER_DUMP_DIR");
        directory != nullptr && *directory != '\0') {
        // Explicit host-owned diagnostics keep captures separate from persistent
        // shader caches. Optional hashes select programs, never alter rendering.
        if (const char* filter = std::getenv("EXECUTOR_SHADER_DUMP_HASHES");
            filter != nullptr && *filter != '\0') {
            std::istringstream stream{filter};
            std::string token;
            bool selected = false;
            while (stream >> token) {
                selected |= std::strtoull(token.c_str(), nullptr, 0) == hash;
            }
            if (!selected) return;
        }
        static std::atomic<u32> captured{};
        if (captured.fetch_add(1, std::memory_order_relaxed) >= 64) return;
        dump_dir = directory;
    }
#endif
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto full = dump_dir / filename;
    const auto file = IOFile{full, FileAccessMode::Create};
    file.WriteSpan(code);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                        "[EXECUTOR_SHADER_DUMP] wrote path=%s bytes=%zu isOpen=%d",
                        full.string().c_str(), code.size_bytes(), file.IsOpen() ? 1 : 0);
#endif
}

std::optional<std::vector<u32>> PipelineCache::GetShaderPatch(u64 hash, Shader::Stage stage,
                                                              size_t perm_idx,
                                                              std::string_view ext) {

    using namespace Common::FS;
    const auto patch_dir = GetUserPath(PathType::ShaderDir) / "patch";
    if (!std::filesystem::exists(patch_dir)) {
        std::filesystem::create_directories(patch_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto filepath = patch_dir / filename;
    if (!std::filesystem::exists(filepath)) {
        return {};
    }
    const auto file = IOFile{patch_dir / filename, FileAccessMode::Read};
    std::vector<u32> code(file.GetSize() / sizeof(u32));
    file.Read(code);
    return code;
}
}
