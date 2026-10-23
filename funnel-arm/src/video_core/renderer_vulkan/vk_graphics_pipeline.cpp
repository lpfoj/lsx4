// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>
#include <boost/container/small_vector.hpp>
#ifdef __ANDROID__
#include <android/log.h>
#include <cstdlib>
#include <unistd.h>
#endif

#include "common/assert.h"
#include "shader_recompiler/backend/spirv/emit_spirv_quad_rect.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

using Shader::Backend::SPIRV::AuxShaderType;

static constexpr std::array LogicalStageToStageBit = {
    vk::ShaderStageFlagBits::eFragment,
    vk::ShaderStageFlagBits::eTessellationControl,
    vk::ShaderStageFlagBits::eTessellationEvaluation,
    vk::ShaderStageFlagBits::eVertex,
    vk::ShaderStageFlagBits::eGeometry,
    vk::ShaderStageFlagBits::eCompute,
};

struct ScopedAuxiliaryShaderModules {
    explicit ScopedAuxiliaryShaderModules(vk::Device device_) : device{device_} {}

    ~ScopedAuxiliaryShaderModules() {
        for (const auto module : modules) {
            if (module) {
                device.destroyShaderModule(module);
            }
        }
    }

    vk::ShaderModule Add(vk::ShaderModule module) {
        modules.push_back(module);
        return module;
    }

    vk::Device device;
    std::vector<vk::ShaderModule> modules;
};

static bool IsPrimitiveTopologyList(const vk::PrimitiveTopology topology) {
    return topology == vk::PrimitiveTopology::ePointList ||
           topology == vk::PrimitiveTopology::eLineList ||
           topology == vk::PrimitiveTopology::eTriangleList ||
           topology == vk::PrimitiveTopology::eLineListWithAdjacency ||
           topology == vk::PrimitiveTopology::eTriangleListWithAdjacency ||
           topology == vk::PrimitiveTopology::ePatchList;
}

namespace {

#ifdef __ANDROID__
bool ExecutorPipelineCreateInfoTraceEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EXECUTOR_TRACE_PIPELINE_CREATE_INFO");
        const bool environment_enabled =
            value != nullptr && value[0] != '\0' && value[0] != '0';
        return environment_enabled ||
               ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                        "run-diagnostic-pipeline-create-info",
                        F_OK) == 0;
    }();
    return enabled;
}
#endif

enum class ExecutorGraphicsPipelineBuildStage : std::size_t {
    SerializationSupport,
    DescriptorBindings,
    DescriptorSetLayout,
    PipelineLayout,
    RenderPass,
    VkCreateGraphicsPipelines,
    Count,
};

#ifdef __ANDROID__

bool ExecutorGraphicsPipelineBuildProfileEnabled() noexcept {
    // Explicit marker only, cached for the process lifetime. A normal pipeline
    // build performs no clock or log operation when the marker is absent.
    static const bool enabled =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-diagnostic-pipeline-build-profile",
                 F_OK) == 0;
    return enabled;
}

u64 ExecutorGraphicsPipelineBuildProfileNowNs() noexcept {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

class ExecutorGraphicsPipelineBuildProfile;
thread_local ExecutorGraphicsPipelineBuildProfile*
    executor_active_graphics_pipeline_build_profile{};

class ExecutorGraphicsPipelineBuildProfile {
public:
    ExecutorGraphicsPipelineBuildProfile(
        const u64 pipeline_hash, const bool preloading,
        const vk::PipelineCreateFlags build_flags, const vk::Result& build_result,
        const vk::UniquePipeline& pipeline) noexcept
        : enabled_{ExecutorGraphicsPipelineBuildProfileEnabled()},
          pipeline_hash_{pipeline_hash}, preloading_{preloading},
          build_flags_{build_flags}, build_result_{build_result}, pipeline_{pipeline} {
        if (!enabled_) {
            return;
        }
        previous_ = executor_active_graphics_pipeline_build_profile;
        executor_active_graphics_pipeline_build_profile = this;
        begin_ns_ = ExecutorGraphicsPipelineBuildProfileNowNs();
    }

    ~ExecutorGraphicsPipelineBuildProfile() {
        if (!enabled_) {
            return;
        }
        const u64 end_ns = ExecutorGraphicsPipelineBuildProfileNowNs();
        executor_active_graphics_pipeline_build_profile = previous_;
        const u64 total_ns = Delta(end_ns, begin_ns_);
        u64 accounted_ns{};
        for (const u64 stage_ns : stage_ns_) {
            accounted_ns += stage_ns;
        }
        const u64 other_ns = total_ns > accounted_ns ? total_ns - accounted_ns : 0;
        const auto us = [](const u64 ns) {
            return static_cast<unsigned long long>(ns / 1'000);
        };
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_GRAPHICS_PIPELINE_BUILD_PROFILE] schema=1 hash=0x%llx "
            "preloading=%d flags=0x%x valid=%d result=%d totalUs=%llu "
            "serializationUs=%llu descriptorBindingsUs=%llu descriptorLayoutUs=%llu "
            "pipelineLayoutUs=%llu renderPassUs=%llu vkCreateUs=%llu otherUs=%llu",
            static_cast<unsigned long long>(pipeline_hash_), preloading_ ? 1 : 0,
            static_cast<unsigned>(VkPipelineCreateFlags(build_flags_)), pipeline_ ? 1 : 0,
            static_cast<int>(build_result_), us(total_ns),
            us(Stage(ExecutorGraphicsPipelineBuildStage::SerializationSupport)),
            us(Stage(ExecutorGraphicsPipelineBuildStage::DescriptorBindings)),
            us(Stage(ExecutorGraphicsPipelineBuildStage::DescriptorSetLayout)),
            us(Stage(ExecutorGraphicsPipelineBuildStage::PipelineLayout)),
            us(Stage(ExecutorGraphicsPipelineBuildStage::RenderPass)),
            us(Stage(ExecutorGraphicsPipelineBuildStage::VkCreateGraphicsPipelines)),
            us(other_ns));
    }

    ExecutorGraphicsPipelineBuildProfile(const ExecutorGraphicsPipelineBuildProfile&) = delete;
    ExecutorGraphicsPipelineBuildProfile& operator=(
        const ExecutorGraphicsPipelineBuildProfile&) = delete;

    static ExecutorGraphicsPipelineBuildProfile* Active() noexcept {
        return executor_active_graphics_pipeline_build_profile;
    }

    [[nodiscard]] u64 BeginStage() const noexcept {
        return enabled_ ? ExecutorGraphicsPipelineBuildProfileNowNs() : 0;
    }

    void EndStage(const ExecutorGraphicsPipelineBuildStage stage,
                  const u64 begin_ns) noexcept {
        if (!enabled_) {
            return;
        }
        stage_ns_[static_cast<std::size_t>(stage)] += Delta(
            ExecutorGraphicsPipelineBuildProfileNowNs(), begin_ns);
    }

    void RecordDescriptorLayout(const u64 bindings_begin_ns,
                                const u64 layout_begin_ns,
                                const u64 layout_end_ns) noexcept {
        if (!enabled_) {
            return;
        }
        stage_ns_[static_cast<std::size_t>(
            ExecutorGraphicsPipelineBuildStage::DescriptorBindings)] +=
            Delta(layout_begin_ns, bindings_begin_ns);
        stage_ns_[static_cast<std::size_t>(
            ExecutorGraphicsPipelineBuildStage::DescriptorSetLayout)] +=
            Delta(layout_end_ns, layout_begin_ns);
    }

private:
    static u64 Delta(const u64 end_ns, const u64 begin_ns) noexcept {
        return end_ns >= begin_ns ? end_ns - begin_ns : 0;
    }

    [[nodiscard]] u64 Stage(const ExecutorGraphicsPipelineBuildStage stage) const noexcept {
        return stage_ns_[static_cast<std::size_t>(stage)];
    }

    bool enabled_{};
    u64 pipeline_hash_{};
    bool preloading_{};
    vk::PipelineCreateFlags build_flags_{};
    const vk::Result& build_result_;
    const vk::UniquePipeline& pipeline_;
    ExecutorGraphicsPipelineBuildProfile* previous_{};
    u64 begin_ns_{};
    std::array<u64, static_cast<std::size_t>(
                        ExecutorGraphicsPipelineBuildStage::Count)>
        stage_ns_{};
};

#else

class ExecutorGraphicsPipelineBuildProfile {
public:
    ExecutorGraphicsPipelineBuildProfile(u64, bool, vk::PipelineCreateFlags,
                                         const vk::Result&,
                                         const vk::UniquePipeline&) noexcept {}

    static ExecutorGraphicsPipelineBuildProfile* Active() noexcept {
        return nullptr;
    }

    [[nodiscard]] u64 BeginStage() const noexcept {
        return 0;
    }

    void EndStage(ExecutorGraphicsPipelineBuildStage, u64) noexcept {}
    void RecordDescriptorLayout(u64, u64, u64) noexcept {}
};

#endif

class ExecutorGraphicsPipelineBuildStageScope {
public:
    explicit ExecutorGraphicsPipelineBuildStageScope(
        const ExecutorGraphicsPipelineBuildStage stage) noexcept
        : profile_{ExecutorGraphicsPipelineBuildProfile::Active()}, stage_{stage} {
        if (profile_ != nullptr) {
            begin_ns_ = profile_->BeginStage();
        }
    }

    ~ExecutorGraphicsPipelineBuildStageScope() {
        if (profile_ != nullptr) {
            profile_->EndStage(stage_, begin_ns_);
        }
    }

private:
    ExecutorGraphicsPipelineBuildProfile* profile_{};
    ExecutorGraphicsPipelineBuildStage stage_{};
    u64 begin_ns_{};
};

} // namespace

GraphicsPipeline::GraphicsPipeline(
    const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
    const Shader::Profile& profile, const GraphicsPipelineKey& key_,
    vk::PipelineCache pipeline_cache, std::span<const Shader::Info*, MaxShaderStages> infos,
    std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
    std::optional<const Shader::Gcn::FetchShaderData> fetch_shader_,
    std::span<const vk::ShaderModule> modules, SerializationSupport& sdata, bool preloading,
    vk::PipelineCreateFlags build_flags)
    : Pipeline{instance, scheduler, desc_heap, profile, pipeline_cache}, key{key_},
      fetch_shader{std::move(fetch_shader_)} {
    pipeline_hash = std::hash<GraphicsPipelineKey>{}(key);
    ExecutorGraphicsPipelineBuildProfile build_profile{
        pipeline_hash, preloading, build_flags, build_result, pipeline};
    const vk::Device device = instance.GetDevice();
    ScopedAuxiliaryShaderModules auxiliary_modules{device};
    std::ranges::copy(infos, stages.begin());
    if (!preloading) {
        ExecutorGraphicsPipelineBuildStageScope serialization_profile{
            ExecutorGraphicsPipelineBuildStage::SerializationSupport};
        PrepareSerializationSupport(instance, key, infos, runtime_infos, fetch_shader, sdata);
    }
    if (!BuildDescSetLayout(sdata, preloading)) {
        return;
    }
    const auto debug_str = GetDebugString();

    const vk::PushConstantRange push_constants = {
        .stageFlags = AllGraphicsStageBits,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    };
    const u64 pipeline_layout_profile_begin = build_profile.BeginStage();
    auto [layout_result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);
    build_profile.EndStage(ExecutorGraphicsPipelineBuildStage::PipelineLayout,
                           pipeline_layout_profile_begin);
    if (layout_result != vk::Result::eSuccess) {
        build_result = layout_result;
        LOG_ERROR(Render_Vulkan, "Failed to create graphics pipeline layout: {}",
                  vk::to_string(layout_result));
        return;
    }
    pipeline_layout = std::move(layout);
    SetObjectName(device, *pipeline_layout, "Graphics PipelineLayout {}", debug_str);

    const vk::PipelineVertexInputDivisorStateCreateInfo divisor_state = {
        .vertexBindingDivisorCount = static_cast<u32>(sdata.divisors.size()),
        .pVertexBindingDivisors = sdata.divisors.data(),
    };

    const vk::PipelineVertexInputStateCreateInfo vertex_input_info = {
        .pNext = sdata.divisors.empty() ? nullptr : &divisor_state,
        .vertexBindingDescriptionCount = static_cast<u32>(sdata.vertex_bindings.size()),
        .pVertexBindingDescriptions = sdata.vertex_bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<u32>(sdata.vertex_attributes.size()),
        .pVertexAttributeDescriptions = sdata.vertex_attributes.data(),
    };

#ifdef __ANDROID__
    static const bool kBisectSkipTess = std::getenv("EXECUTOR_BISECT_SKIP_TESS") != nullptr;
#else
    constexpr bool kBisectSkipTess = false;
#endif
    const bool is_rect_or_quad = key.prim_type == AmdGpu::PrimitiveType::RectList ||
                                 key.prim_type == AmdGpu::PrimitiveType::QuadList;
    const bool can_emulate_rect_or_quad_with_tess =
        !kBisectSkipTess && instance.IsTessellationSupported();
    auto topology = LiverpoolToVK::PrimitiveType(key.prim_type);
    if (!can_emulate_rect_or_quad_with_tess && is_rect_or_quad) {
        topology = vk::PrimitiveTopology::eTriangleList;
    }
    const bool use_legacy_render_pass = instance.UseLegacyRenderPass();
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly = {
        .topology = topology,
        .primitiveRestartEnable =
            use_legacy_render_pass &&
            (key.legacy_fixed_state_flags & LegacyPrimitiveRestart) != 0,
    };

    const bool is_rect_list = can_emulate_rect_or_quad_with_tess &&
                              key.prim_type == AmdGpu::PrimitiveType::RectList;
    const bool is_quad_list = can_emulate_rect_or_quad_with_tess &&
                              key.prim_type == AmdGpu::PrimitiveType::QuadList;
    const auto ensure_auxiliary_spirv = [&](std::vector<u32>& code, AuxShaderType type) {
        constexpr u32 SpirvMagic = 0x07230203;
        const u32 supported_spirv = instance.GetCapabilityProfile().spirv_version;
        if (code.size() >= 2 && code[0] == SpirvMagic && code[1] <= supported_spirv) {
            return;
        }
        const auto& fs_info = runtime_infos[u32(Shader::LogicalStage::Fragment)].fs_info;
        code = Shader::Backend::SPIRV::EmitAuxilaryTessShader(type, fs_info,
                                                              supported_spirv);
    };
    if (is_rect_list || is_quad_list) {
        if (infos[u32(Shader::LogicalStage::TessellationControl)] == nullptr) {
            ensure_auxiliary_spirv(
                sdata.tcs, is_quad_list ? AuxShaderType::QuadListTCS : AuxShaderType::RectListTCS);
        }
        if (infos[u32(Shader::LogicalStage::TessellationEval)] == nullptr) {
            ensure_auxiliary_spirv(sdata.tes, AuxShaderType::PassthroughTES);
        }
    }
    const vk::PipelineTessellationStateCreateInfo tessellation_state = {
        .patchControlPoints = is_rect_list ? 3U : (is_quad_list ? 4U : key.patch_control_points),
    };

    vk::StructureChain raster_chain = {
        vk::PipelineRasterizationStateCreateInfo{
            .depthClampEnable = key.depth_clamp_enable &&
                                (!key.depth_clip_enable || instance.IsDepthClipEnableSupported()),
#ifdef __ANDROID__
            .rasterizerDiscardEnable = key.rasterizer_discard_enable,
#else
            .rasterizerDiscardEnable = false,
#endif
            .polygonMode = LiverpoolToVK::PolygonMode(key.polygon_mode),
            .cullMode = use_legacy_render_pass
                            ? vk::CullModeFlags{key.legacy_cull_mode}
                            : vk::CullModeFlagBits::eNone,
            .frontFace = use_legacy_render_pass
                             ? static_cast<vk::FrontFace>(key.legacy_front_face)
                             : vk::FrontFace::eCounterClockwise,
            .depthBiasEnable =
                use_legacy_render_pass &&
                (key.legacy_fixed_state_flags & LegacyDepthBias) != 0,
            .lineWidth = 1.0f,
        },
        vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT{
            .provokingVertexMode = key.provoking_vtx_last == AmdGpu::ProvokingVtxLast::First
                                       ? vk::ProvokingVertexModeEXT::eFirstVertex
                                       : vk::ProvokingVertexModeEXT::eLastVertex,
        },
        vk::PipelineRasterizationDepthClipStateCreateInfoEXT{
            .depthClipEnable = key.depth_clip_enable,
        },
    };

    if (!instance.IsProvokingVertexSupported()) {
        raster_chain.unlink<vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT>();
    }
    if (!instance.IsDepthClipEnableSupported()) {
        raster_chain.unlink<vk::PipelineRasterizationDepthClipStateCreateInfoEXT>();
    }

    const vk::PipelineViewportDepthClipControlCreateInfoEXT clip_control = {
        .negativeOneToOne = key.clip_space == AmdGpu::ClipSpace::MinusWToW,
    };

    const vk::PipelineViewportStateCreateInfo viewport_info = {
        .pNext = instance.IsDepthClipControlSupported() ? &clip_control : nullptr,
        .viewportCount = use_legacy_render_pass ? std::max<u32>(key.legacy_viewport_count, 1u) : 0u,
        .scissorCount = use_legacy_render_pass ? std::max<u32>(key.legacy_viewport_count, 1u) : 0u,
    };

    boost::container::static_vector<vk::DynamicState, 32> dynamic_states;
    if (use_legacy_render_pass) {
        dynamic_states = {
            vk::DynamicState::eViewport,           vk::DynamicState::eScissor,
            vk::DynamicState::eBlendConstants,     vk::DynamicState::eDepthBias,
            vk::DynamicState::eStencilReference,   vk::DynamicState::eStencilCompareMask,
            vk::DynamicState::eStencilWriteMask,   vk::DynamicState::eLineWidth,
        };
    } else {
        dynamic_states = {
            vk::DynamicState::eViewportWithCount,  vk::DynamicState::eScissorWithCount,
            vk::DynamicState::eBlendConstants,     vk::DynamicState::eDepthTestEnable,
            vk::DynamicState::eDepthWriteEnable,   vk::DynamicState::eDepthCompareOp,
            vk::DynamicState::eDepthBiasEnable,    vk::DynamicState::eDepthBias,
            vk::DynamicState::eStencilTestEnable,  vk::DynamicState::eStencilReference,
            vk::DynamicState::eStencilCompareMask, vk::DynamicState::eStencilWriteMask,
            vk::DynamicState::eStencilOp,          vk::DynamicState::eCullMode,
            vk::DynamicState::eFrontFace,          vk::DynamicState::eLineWidth,
            vk::DynamicState::ePrimitiveRestartEnable,
        };
    }

#ifndef __ANDROID__
    dynamic_states.push_back(vk::DynamicState::eRasterizerDiscardEnable);
#endif

    if (instance.IsDepthBoundsSupported()) {
        if (!use_legacy_render_pass) {
            dynamic_states.push_back(vk::DynamicState::eDepthBoundsTestEnable);
        }
        dynamic_states.push_back(vk::DynamicState::eDepthBounds);
    }
    if (instance.IsDynamicColorWriteMaskSupported()) {
        dynamic_states.push_back(vk::DynamicState::eColorWriteMaskEXT);
    }
    if (instance.IsCoarseFragmentShadingSupported()) {
        dynamic_states.push_back(vk::DynamicState::eFragmentShadingRateKHR);
    }
    if (!use_legacy_render_pass && instance.IsVertexInputDynamicState()) {
        dynamic_states.push_back(vk::DynamicState::eVertexInputEXT);
    } else if (!use_legacy_render_pass && !sdata.vertex_bindings.empty()) {
        dynamic_states.push_back(vk::DynamicState::eVertexInputBindingStride);
    }

    const vk::PipelineDynamicStateCreateInfo dynamic_info = {
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };

    boost::container::static_vector<vk::PipelineShaderStageCreateInfo, MaxShaderStages>
        shader_stages;
    auto stage = u32(Shader::LogicalStage::Vertex);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = modules[stage],
            .pName = "main",
        });
    }
    stage = u32(Shader::LogicalStage::Geometry);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eGeometry,
            .module = modules[stage],
            .pName = "main",
        });
    }
    stage = u32(Shader::LogicalStage::TessellationControl);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationControl,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (is_rect_list || is_quad_list) {
        const auto type = is_quad_list ? AuxShaderType::QuadListTCS : AuxShaderType::RectListTCS;
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationControl,
            .module = auxiliary_modules.Add(
                CompileSPV(sdata.tcs, instance.GetDevice())),
            .pName = "main",
        });
    }
    stage = u32(Shader::LogicalStage::TessellationEval);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationEvaluation,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (is_rect_list || is_quad_list) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationEvaluation,
            .module = auxiliary_modules.Add(
                CompileSPV(sdata.tes, instance.GetDevice())),
            .pName = "main",
        });
    }
    stage = u32(Shader::LogicalStage::Fragment);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = modules[stage],
            .pName = "main",
        });
    }

    const bool has_tessellation_stages =
        is_rect_list || is_quad_list ||
        infos[u32(Shader::LogicalStage::TessellationControl)] != nullptr ||
        infos[u32(Shader::LogicalStage::TessellationEval)] != nullptr;

    depth_format =
        instance.GetSupportedFormat(LiverpoolToVK::DepthFormat(key.z_format, key.stencil_format),
                                    vk::FormatFeatureFlagBits2::eDepthStencilAttachment);
    std::array<vk::Format, Shader::IR::NumRenderTargets> color_formats{};
    for (s32 i = 0; i < key.num_color_attachments; ++i) {
        const auto& col_buf = key.color_buffers[i];
        const auto format = LiverpoolToVK::SurfaceFormat(col_buf.data_format, col_buf.num_format);
        const auto color_format =
            instance.GetSupportedFormat(format, vk::FormatFeatureFlagBits2::eColorAttachment);
        if (!instance.IsFormatSupported(color_format,
                                        vk::FormatFeatureFlagBits2::eColorAttachment)) {
            LOG_WARNING(Render_Vulkan,
                        "color buffer format {} does not support COLOR_ATTACHMENT_BIT",
                        vk::to_string(color_format));
        }
        color_formats[i] = color_format;
    }
    if (key.num_color_attachments != 0) {
        color_format0 = color_formats[0];
    }

    u32 legacy_color_attachment_count = key.num_color_attachments;
    if (instance.GetCapabilityProfile().ShouldCompactLegacyUnusedColorAttachments()) {
        u32 defined_color_mask = 0;
        for (u32 index = 0; index < key.num_color_attachments; ++index) {
            if (color_formats[index] != vk::Format::eUndefined) {
                defined_color_mask |= 1U << index;
            }
        }
        legacy_color_attachment_count =
            CompactTrailingAttachmentCount(key.num_color_attachments, defined_color_mask);
    }
    const u32 pipeline_color_attachment_count =
        use_legacy_render_pass ? legacy_color_attachment_count : key.num_color_attachments;

    std::array<vk::SampleCountFlagBits, AmdGpu::NUM_COLOR_BUFFERS> color_samples;
    std::ranges::transform(key.color_samples, color_samples.begin(), [&instance](u8 num_samples) {
        return num_samples ? LiverpoolToVK::NumSamples(num_samples, instance.GetColorSampleCounts())
                           : vk::SampleCountFlagBits::e1;
    });
    const vk::AttachmentSampleCountInfoAMD mixed_samples = {
        .colorAttachmentCount = key.num_color_attachments,
        .pColorAttachmentSamples = color_samples.data(),
        .depthStencilAttachmentSamples =
            LiverpoolToVK::NumSamples(key.depth_samples, instance.GetDepthSampleCounts()),
    };

    const vk::PipelineRenderingCreateInfo pipeline_rendering_ci = {
        .pNext = instance.IsMixedDepthSamplesSupported() ? &mixed_samples : nullptr,
        .colorAttachmentCount = key.num_color_attachments,
        .pColorAttachmentFormats = color_formats.data(),
        .depthAttachmentFormat = key.z_format != AmdGpu::DepthBuffer::ZFormat::Invalid
                                     ? depth_format
                                     : vk::Format::eUndefined,
        .stencilAttachmentFormat = key.stencil_format != AmdGpu::DepthBuffer::StencilFormat::Invalid
                                       ? depth_format
                                       : vk::Format::eUndefined,
    };

    std::array<vk::PipelineColorBlendAttachmentState, AmdGpu::NUM_COLOR_BUFFERS> attachments;
    for (u32 i = 0; i < key.num_color_attachments; i++) {
        const auto& control = key.blend_controls[i];

        const auto src_color = LiverpoolToVK::BlendFactor(control.color_src_factor);
        const auto dst_color = LiverpoolToVK::BlendFactor(control.color_dst_factor);
        const auto color_blend = LiverpoolToVK::BlendOp(control.color_func);

        const auto src_alpha = control.separate_alpha_blend
                                   ? LiverpoolToVK::BlendFactor(control.alpha_src_factor)
                                   : src_color;
        const auto dst_alpha = control.separate_alpha_blend
                                   ? LiverpoolToVK::BlendFactor(control.alpha_dst_factor)
                                   : dst_color;
        const auto alpha_blend =
            control.separate_alpha_blend ? LiverpoolToVK::BlendOp(control.alpha_func) : color_blend;

        const auto color_scaled_min_max =
            (color_blend == vk::BlendOp::eMin || color_blend == vk::BlendOp::eMax) &&
            (src_color != vk::BlendFactor::eOne || dst_color != vk::BlendFactor::eOne);
        const auto alpha_scaled_min_max =
            (alpha_blend == vk::BlendOp::eMin || alpha_blend == vk::BlendOp::eMax) &&
            (src_alpha != vk::BlendFactor::eOne || dst_alpha != vk::BlendFactor::eOne);
        if (color_scaled_min_max || alpha_scaled_min_max) {
            LOG_WARNING(
                Render_Vulkan,
                "Unimplemented use of min/max blend op with blend factor not equal to one.");
        }

        attachments[i] = vk::PipelineColorBlendAttachmentState{
            .blendEnable = control.enable,
            .srcColorBlendFactor = src_color,
            .dstColorBlendFactor = dst_color,
            .colorBlendOp = color_blend,
            .srcAlphaBlendFactor = src_alpha,
            .dstAlphaBlendFactor = dst_alpha,
            .alphaBlendOp = alpha_blend,
            .colorWriteMask =
                instance.IsDynamicColorWriteMaskSupported()
                    ? vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
                    : key.write_masks[i],
        };

        const auto has_alpha_masked_out =
            (key.cb_shader_mask.GetMask(i) & AmdGpu::ColorBufferMask::ComponentA) == 0;
        const auto has_src_alpha_in_src_blend = src_color == vk::BlendFactor::eSrcAlpha ||
                                                src_color == vk::BlendFactor::eOneMinusSrcAlpha;
        const auto has_src_alpha_in_dst_blend = dst_color == vk::BlendFactor::eSrcAlpha ||
                                                dst_color == vk::BlendFactor::eOneMinusSrcAlpha;
        if (has_alpha_masked_out && has_src_alpha_in_src_blend) {
            attachments[i].srcColorBlendFactor = src_color == vk::BlendFactor::eSrcAlpha
                                                     ? vk::BlendFactor::eOne
                                                     : vk::BlendFactor::eZero;
        }
        if (has_alpha_masked_out && has_src_alpha_in_dst_blend) {
            attachments[i].dstColorBlendFactor = dst_color == vk::BlendFactor::eSrcAlpha
                                                     ? vk::BlendFactor::eOne
                                                     : vk::BlendFactor::eZero;
        }
    }

    const vk::PipelineColorBlendStateCreateInfo color_blending = {
        .logicOpEnable =
            instance.IsLogicOpSupported() && key.logic_op != AmdGpu::ColorControl::LogicOp::Copy,
        .logicOp = LiverpoolToVK::LogicOp(key.logic_op),
        .attachmentCount = pipeline_color_attachment_count,
        .pAttachments = pipeline_color_attachment_count != 0 ? attachments.data() : nullptr,
        .blendConstants = std::array{1.0f, 1.0f, 1.0f, 1.0f},
    };

    const vk::PipelineDepthStencilStateCreateInfo depth_stencil_info = {
        .depthTestEnable =
            use_legacy_render_pass &&
            (key.legacy_fixed_state_flags & LegacyDepthTest) != 0,
        .depthWriteEnable =
            use_legacy_render_pass &&
            (key.legacy_fixed_state_flags & LegacyDepthWrite) != 0,
        .depthCompareOp = use_legacy_render_pass
                              ? static_cast<vk::CompareOp>(key.legacy_depth_compare_op)
                              : vk::CompareOp::eNever,
        .depthBoundsTestEnable =
            use_legacy_render_pass &&
            (key.legacy_fixed_state_flags & LegacyDepthBoundsTest) != 0,
        .stencilTestEnable =
            use_legacy_render_pass &&
            (key.legacy_fixed_state_flags & LegacyStencilTest) != 0,
        .front = vk::StencilOpState{
            .failOp = static_cast<vk::StencilOp>(key.legacy_stencil_front[0]),
            .passOp = static_cast<vk::StencilOp>(key.legacy_stencil_front[1]),
            .depthFailOp = static_cast<vk::StencilOp>(key.legacy_stencil_front[2]),
            .compareOp = static_cast<vk::CompareOp>(key.legacy_stencil_front[3]),
        },
        .back = vk::StencilOpState{
            .failOp = static_cast<vk::StencilOp>(key.legacy_stencil_back[0]),
            .passOp = static_cast<vk::StencilOp>(key.legacy_stencil_back[1]),
            .depthFailOp = static_cast<vk::StencilOp>(key.legacy_stencil_back[2]),
            .compareOp = static_cast<vk::CompareOp>(key.legacy_stencil_back[3]),
        },
    };

    if (use_legacy_render_pass) {
        std::array<vk::AttachmentDescription, AmdGpu::NUM_COLOR_BUFFERS + 1> descriptions{};
        std::array<vk::AttachmentReference, AmdGpu::NUM_COLOR_BUFFERS> color_references{};
        u32 description_count = 0;
        for (u32 index = 0; index < key.num_color_attachments; ++index) {
            if (color_formats[index] == vk::Format::eUndefined) {
                color_references[index] = vk::AttachmentReference{
                    .attachment = VK_ATTACHMENT_UNUSED,
                    .layout = vk::ImageLayout::eColorAttachmentOptimal,
                };
                continue;
            }
            const u32 attachment = description_count++;
            descriptions[attachment] = vk::AttachmentDescription{
                .format = color_formats[index],
                .samples = LiverpoolToVK::NumSamples(key.color_samples[index],
                                                       instance.GetColorSampleCounts()),
                .loadOp = vk::AttachmentLoadOp::eLoad,
                .storeOp = vk::AttachmentStoreOp::eStore,
                .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
                .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
                .initialLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .finalLayout = vk::ImageLayout::eColorAttachmentOptimal,
            };
            color_references[index] = vk::AttachmentReference{
                .attachment = attachment,
                .layout = vk::ImageLayout::eColorAttachmentOptimal,
            };
        }

        vk::AttachmentReference depth_reference{
            .attachment = VK_ATTACHMENT_UNUSED,
            .layout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
        };
        if (depth_format != vk::Format::eUndefined) {
            const u32 attachment = description_count++;
            descriptions[attachment] = vk::AttachmentDescription{
                .format = depth_format,
                .samples = LiverpoolToVK::NumSamples(key.depth_samples,
                                                       instance.GetDepthSampleCounts()),
                .loadOp = vk::AttachmentLoadOp::eLoad,
                .storeOp = vk::AttachmentStoreOp::eStore,
                .stencilLoadOp = vk::AttachmentLoadOp::eLoad,
                .stencilStoreOp = vk::AttachmentStoreOp::eStore,
                .initialLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
                .finalLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
            };
            depth_reference.attachment = attachment;
        }

        const vk::SubpassDescription subpass{
            .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
            .colorAttachmentCount = legacy_color_attachment_count,
            .pColorAttachments = legacy_color_attachment_count != 0
                                     ? color_references.data()
                                     : nullptr,
            .pDepthStencilAttachment = depth_format != vk::Format::eUndefined
                                           ? &depth_reference
                                           : nullptr,
        };
        const vk::RenderPassCreateInfo render_pass_info{
            .attachmentCount = description_count,
            .pAttachments = descriptions.data(),
            .subpassCount = 1,
            .pSubpasses = &subpass,
        };
        const u64 render_pass_profile_begin = build_profile.BeginStage();
        auto [render_pass_result, render_pass] = device.createRenderPassUnique(render_pass_info);
        build_profile.EndStage(ExecutorGraphicsPipelineBuildStage::RenderPass,
                               render_pass_profile_begin);
        if (render_pass_result != vk::Result::eSuccess) {
            build_result = render_pass_result;
#ifdef __ANDROID__
            __android_log_print(ANDROID_LOG_ERROR, "LSX4Native",
                                "[EXECUTOR_GRAPHICS_PIPELINE_FAILURE] phase=renderPass hash=0x%llx result=%d",
                                static_cast<unsigned long long>(pipeline_hash),
                                static_cast<int>(render_pass_result));
#endif
            return;
        }
        legacy_render_pass = std::move(render_pass);
    }

    const vk::PipelineCreateFlags effective_build_flags =
        use_legacy_render_pass ? vk::PipelineCreateFlags{} : build_flags;
    const vk::GraphicsPipelineCreateInfo pipeline_info = {
        .pNext = use_legacy_render_pass ? nullptr : &pipeline_rendering_ci,
        .flags = effective_build_flags,
        .stageCount = static_cast<u32>(shader_stages.size()),
        .pStages = shader_stages.data(),
        .pVertexInputState = !instance.IsVertexInputDynamicState() ? &vertex_input_info : nullptr,
        .pInputAssemblyState = &input_assembly,
        .pTessellationState = has_tessellation_stages ? &tessellation_state : nullptr,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster_chain.get(),
        .pMultisampleState = &sdata.multisampling,
        .pDepthStencilState =
            use_legacy_render_pass ||
                    instance.GetCapabilityProfile().HasQuirk(
                        CapabilityQuirk::RequireStaticDepthStencil) ||
                    !instance.IsExtendedDynamicState3Supported()
                ? &depth_stencil_info
                : nullptr,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = *pipeline_layout,
        .renderPass = use_legacy_render_pass ? *legacy_render_pass : vk::RenderPass{},
    };

#ifdef __ANDROID__
    if (ExecutorPipelineCreateInfoTraceEnabled()) {
        static std::atomic<unsigned> pipeline_ci_log_budget{0};
        const unsigned pipeline_ci_log_index =
            pipeline_ci_log_budget.fetch_add(1, std::memory_order_relaxed);
        if (pipeline_ci_log_index < 16) {
            __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                        "[EXECUTOR_PIPELINE_CI_COUNTS] hash=0x%llx vs=0x%llx ps=0x%llx "
                        "vsPgm=0x%llx psPgm=0x%llx "
                        "stages=%u vtxBindings=%u vtxAttrs=%u divisors=%u colorAttach=%u/%u "
                        "dynStates=%u tessState=%u tessPoints=%u prim=%u samples=%u color0Samples=%u "
                        "colorFmt0=%u depthFmt=%u stencilFmt=%u",
                        static_cast<unsigned long long>(pipeline_hash),
                        static_cast<unsigned long long>(
                            key.stage_hashes[u32(Shader::LogicalStage::Vertex)]),
                        static_cast<unsigned long long>(
                            key.stage_hashes[u32(Shader::LogicalStage::Fragment)]),
                        static_cast<unsigned long long>(
                            infos[u32(Shader::LogicalStage::Vertex)]
                                ? infos[u32(Shader::LogicalStage::Vertex)]->pgm_hash
                                : 0),
                        static_cast<unsigned long long>(
                            infos[u32(Shader::LogicalStage::Fragment)]
                                ? infos[u32(Shader::LogicalStage::Fragment)]->pgm_hash
                                : 0),
                        static_cast<unsigned>(shader_stages.size()),
                        static_cast<unsigned>(sdata.vertex_bindings.size()),
                        static_cast<unsigned>(sdata.vertex_attributes.size()),
                        static_cast<unsigned>(sdata.divisors.size()),
                        static_cast<unsigned>(key.num_color_attachments),
                        static_cast<unsigned>(pipeline_color_attachment_count),
                        static_cast<unsigned>(dynamic_states.size()),
                        has_tessellation_stages ? 1U : 0U,
                        static_cast<unsigned>(tessellation_state.patchControlPoints),
                        static_cast<unsigned>(key.prim_type), static_cast<unsigned>(key.num_samples),
                        static_cast<unsigned>(key.color_samples[0]),
                        static_cast<unsigned>(color_format0), static_cast<unsigned>(depth_format),
                        static_cast<unsigned>(key.stencil_format));
        }
    }
    static const bool kBisectSkipVkPipelineCreate =
        std::getenv("EXECUTOR_BISECT_SKIP_VK_PIPELINE_CREATE") != nullptr;
    if (kBisectSkipVkPipelineCreate) {
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_BISECT_SKIP_VK_PIPELINE_CREATE] active (skipping "
                            "vkCreateGraphicsPipelines)");
        return;
    }
#endif
    const u64 vk_create_profile_begin = build_profile.BeginStage();
    auto [pipeline_result, pipe] =
        device.createGraphicsPipelineUnique(pipeline_cache, pipeline_info);
    build_profile.EndStage(
        ExecutorGraphicsPipelineBuildStage::VkCreateGraphicsPipelines,
        vk_create_profile_begin);
    build_result = pipeline_result;
    if (pipeline_result != vk::Result::eSuccess) {
        // ePipelineCompileRequired is the expected fail-fast signal used by
        // managed async compilation.  Logging every miss through Android's
        // logd IPC makes the cold path more expensive without reporting an
        // error.  Preserve full diagnostics for every unexpected failure.
        if (pipeline_result != vk::Result::ePipelineCompileRequired) {
#ifdef __ANDROID__
            __android_log_print(
                ANDROID_LOG_ERROR, "LSX4Native",
                "[EXECUTOR_GRAPHICS_PIPELINE_FAILURE] phase=pipeline hash=0x%llx result=%d flags=0x%x legacy=%d",
                static_cast<unsigned long long>(pipeline_hash),
                static_cast<int>(pipeline_result),
                static_cast<unsigned>(VkPipelineCreateFlags(effective_build_flags)),
                use_legacy_render_pass ? 1 : 0);
#endif
            LOG_ERROR(Render_Vulkan, "Failed to create graphics pipeline: {}",
                      vk::to_string(pipeline_result));
        }
        return;
    }
    pipeline = std::move(pipe);
    SetObjectName(device, *pipeline, "Graphics Pipeline {}", debug_str);
}

GraphicsPipeline::~GraphicsPipeline() = default;

void GraphicsPipeline::PrepareSerializationSupport(
    const Instance& instance, const GraphicsPipelineKey& key,
    const std::span<const Shader::Info*, MaxShaderStages> infos,
    const std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
    std::optional<const Shader::Gcn::FetchShaderData> fetch_shader,
    SerializationSupport& sdata) {
    sdata.vertex_attributes.clear();
    sdata.vertex_bindings.clear();
    sdata.divisors.clear();
    sdata.storage_buffers.clear();
    sdata.image_bindings.clear();
    sdata.tcs.clear();
    sdata.tes.clear();

    const auto* const vertex_info = infos[u32(Shader::LogicalStage::Vertex)];
    if (!instance.IsVertexInputDynamicState() && vertex_info && fetch_shader) {
        const auto& vs_runtime = runtime_infos[u32(Shader::LogicalStage::Vertex)].vs_info;
        for (const auto& attrib : fetch_shader->attributes) {
            if (sdata.vertex_attributes.size() >= MaxVertexBufferCount ||
                sdata.vertex_bindings.size() >= MaxVertexBufferCount) {
                break;
            }
            using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
            const auto step_rate = attrib.GetStepRate();
            const auto buffer = attrib.GetSharp(*vertex_info);
            sdata.vertex_attributes.push_back({
                .location = attrib.semantic,
                .binding = attrib.semantic,
                .format = LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(),
                                                        buffer.GetNumberFmt()),
                .offset = 0,
            });
            sdata.vertex_bindings.push_back({
                .binding = attrib.semantic,
                .stride = buffer.GetStride(),
                .inputRate = step_rate == InstanceIdType::None
                                 ? vk::VertexInputRate::eVertex
                                 : vk::VertexInputRate::eInstance,
            });
            const u32 divisor =
                step_rate == InstanceIdType::OverStepRate0
                    ? vs_runtime.step_rate_0
                    : (step_rate == InstanceIdType::OverStepRate1
                           ? vs_runtime.step_rate_1
                           : 1);
            if (step_rate != InstanceIdType::None) {
                sdata.divisors.push_back({
                    .binding = attrib.semantic,
                    .divisor = divisor,
                });
            }
        }
    }

    const auto& fs_info = runtime_infos[u32(Shader::LogicalStage::Fragment)].fs_info;
    sdata.multisampling = {
        .rasterizationSamples = LiverpoolToVK::NumSamples(
            key.num_samples,
            instance.GetColorSampleCounts() & instance.GetDepthSampleCounts()),
        .sampleShadingEnable =
            fs_info.addr_flags.persp_sample_ena || fs_info.addr_flags.linear_sample_ena,
    };

#ifdef __ANDROID__
    static const bool kBisectSkipTess = std::getenv("EXECUTOR_BISECT_SKIP_TESS") != nullptr;
#else
    constexpr bool kBisectSkipTess = false;
#endif
    const bool can_emulate_rect_or_quad_with_tess =
        !kBisectSkipTess && instance.IsTessellationSupported();
    const bool is_rect_list = can_emulate_rect_or_quad_with_tess &&
                              key.prim_type == AmdGpu::PrimitiveType::RectList;
    const bool is_quad_list = can_emulate_rect_or_quad_with_tess &&
                              key.prim_type == AmdGpu::PrimitiveType::QuadList;
    if ((is_rect_list || is_quad_list) &&
        infos[u32(Shader::LogicalStage::TessellationControl)] == nullptr) {
        const auto type =
            is_quad_list ? AuxShaderType::QuadListTCS : AuxShaderType::RectListTCS;
        sdata.tcs = Shader::Backend::SPIRV::EmitAuxilaryTessShader(
            type, fs_info, instance.GetCapabilityProfile().spirv_version);
    }
    if ((is_rect_list || is_quad_list) &&
        infos[u32(Shader::LogicalStage::TessellationEval)] == nullptr) {
        sdata.tes = Shader::Backend::SPIRV::EmitAuxilaryTessShader(
            AuxShaderType::PassthroughTES, fs_info,
            instance.GetCapabilityProfile().spirv_version);
    }

    for (const auto* stage : infos) {
        if (!stage) {
            continue;
        }
        for (const auto& buffer : stage->buffers) {
            sdata.storage_buffers.push_back(
                buffer.IsStorage(buffer.GetSharp(*stage)) ? 1u : 0u);
        }
        for (const auto& image : stage->images) {
            sdata.image_bindings.push_back(image.NumBindings(*stage));
        }
    }
}

template <typename Attribute, typename Binding>
void GraphicsPipeline::GetVertexInputs(
    VertexInputs<Attribute>& attributes, VertexInputs<Binding>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1) const {
    using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
    if (!fetch_shader || fetch_shader->attributes.empty()) {
        return;
    }
    const auto& vs_info = GetStage(Shader::LogicalStage::Vertex);
    for (const auto& attrib : fetch_shader->attributes) {
        if (attributes.size() >= MaxVertexBufferCount || bindings.size() >= MaxVertexBufferCount ||
            guest_buffers.size() >= MaxVertexBufferCount) {
            static std::atomic<u32> b{0};
            if (b.fetch_add(1, std::memory_order_relaxed) < 16) {
                LOG_ERROR(Render_Vulkan,
                          "GetVertexInputs: fetch_shader->attributes={} exceeds MaxVertexBufferCount={}"
                          " (garbage/unrelocated fetch shader); truncating to avoid static_vector "
                          "overflow",
                          fetch_shader->attributes.size(), MaxVertexBufferCount);
            }
            break;
        }
        const auto step_rate = attrib.GetStepRate();
        const auto buffer = attrib.GetSharp(vs_info);
        attributes.push_back(Attribute{
            .location = attrib.semantic,
            .binding = attrib.semantic,
            .format = LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt()),
            .offset = 0,
        });
        bindings.push_back(Binding{
            .binding = attrib.semantic,
            .stride = buffer.GetStride(),
            .inputRate = step_rate == InstanceIdType::None ? vk::VertexInputRate::eVertex
                                                           : vk::VertexInputRate::eInstance,
        });
        const u32 divisor = step_rate == InstanceIdType::OverStepRate0
                                ? step_rate_0
                                : (step_rate == InstanceIdType::OverStepRate1 ? step_rate_1 : 1);
        if constexpr (std::is_same_v<Binding, vk::VertexInputBindingDescription2EXT>) {
            bindings.back().divisor = divisor;
        } else if (step_rate != InstanceIdType::None) {
            divisors.push_back(vk::VertexInputBindingDivisorDescriptionEXT{
                .binding = attrib.semantic,
                .divisor = divisor,
            });
        }
        guest_buffers.emplace_back(buffer);
    }
}

template void GraphicsPipeline::GetVertexInputs(
    VertexInputs<vk::VertexInputAttributeDescription>& attributes,
    VertexInputs<vk::VertexInputBindingDescription>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1) const;
template void GraphicsPipeline::GetVertexInputs(
    VertexInputs<vk::VertexInputAttributeDescription2EXT>& attributes,
    VertexInputs<vk::VertexInputBindingDescription2EXT>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1) const;

bool GraphicsPipeline::BuildDescSetLayout(SerializationSupport& sdata, bool preloading) {
    auto* const build_profile = ExecutorGraphicsPipelineBuildProfile::Active();
    const u64 descriptor_bindings_profile_begin =
        build_profile != nullptr ? build_profile->BeginStage() : 0;
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    u32 binding{};
    size_t storage_buffer_index{};
    size_t image_binding_index{};

    for (const auto* stage : stages) {
        if (!stage) {
            continue;
        }
        const auto stage_bit = LogicalStageToStageBit[u32(stage->l_stage)];
        for (const auto& buffer : stage->buffers) {
            const auto sharp =
                preloading ? AmdGpu::Buffer{}
                           : buffer.GetSharp(*stage);
            const bool is_storage =
                preloading
                    ? storage_buffer_index < sdata.storage_buffers.size() &&
                          sdata.storage_buffers[storage_buffer_index] != 0
                    : buffer.IsStorage(sharp);
            ++storage_buffer_index;
            bindings.push_back({
                .binding = binding++,
                .descriptorType = is_storage ? vk::DescriptorType::eStorageBuffer
                                             : vk::DescriptorType::eUniformBuffer,
                .descriptorCount = 1,
                .stageFlags = stage_bit,
            });
        }
        for (const auto& image : stage->images) {
            const u32 num_bindings =
                preloading
                    ? (image_binding_index < sdata.image_bindings.size()
                           ? sdata.image_bindings[image_binding_index]
                           : 1u)
                    : image.NumBindings(*stage);
            ++image_binding_index;
            bindings.push_back({
                .binding = binding,
                .descriptorType = image.is_written ? vk::DescriptorType::eStorageImage
                                                   : vk::DescriptorType::eSampledImage,
                .descriptorCount = num_bindings,
                .stageFlags = stage_bit,
            });
            binding += num_bindings;
        }
        for (const auto& sampler : stage->samplers) {
            bindings.push_back({
                .binding = binding++,
                .descriptorType = vk::DescriptorType::eSampler,
                .descriptorCount = 1,
                .stageFlags = stage_bit,
            });
        }
    }
    uses_push_descriptors =
        instance.IsPushDescriptorSupported() && binding <= instance.MaxPushDescriptors();
    const auto flags = uses_push_descriptors
                           ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                           : vk::DescriptorSetLayoutCreateFlagBits{};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = flags,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    const u64 descriptor_layout_profile_begin =
        build_profile != nullptr ? build_profile->BeginStage() : 0;
    auto [layout_result, layout] =
        instance.GetDevice().createDescriptorSetLayoutUnique(desc_layout_ci);
    if (build_profile != nullptr) {
        const u64 descriptor_layout_profile_end = build_profile->BeginStage();
        build_profile->RecordDescriptorLayout(descriptor_bindings_profile_begin,
                                              descriptor_layout_profile_begin,
                                              descriptor_layout_profile_end);
    }
    if (layout_result != vk::Result::eSuccess) {
        build_result = layout_result;
        LOG_ERROR(Render_Vulkan, "Failed to create graphics descriptor set layout: {}",
                  vk::to_string(layout_result));
        return false;
    }
#ifdef __ANDROID__
    if (instance.GetCapabilityProfile().diagnostic_a750_descriptor_topology) {
        RecordPipelineDescriptorTopology(
            uses_push_descriptors, binding, instance.MaxPushDescriptors(),
            instance.MaxUpdateAfterBindDescriptors());
    }
#endif
    desc_layout = std::move(layout);
    return true;
}

}
