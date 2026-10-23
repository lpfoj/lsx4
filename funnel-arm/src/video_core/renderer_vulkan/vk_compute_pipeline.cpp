// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/small_vector.hpp>

#ifdef __ANDROID__
#include <android/log.h>
#include <unistd.h>
#endif

#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

#ifdef __ANDROID__
namespace {

bool ComputePipelineCreateDiagnosticRequested() noexcept {
    static const bool requested =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "diag-compute-pipeline-create",
                 F_OK) == 0;
    return requested;
}

} // namespace
#endif

ComputePipeline::ComputePipeline(const Instance& instance, Scheduler& scheduler,
                                 DescriptorHeap& desc_heap, const Shader::Profile& profile,
                                 vk::PipelineCache pipeline_cache, ComputePipelineKey compute_key_,
                                 const Shader::Info& info_,
                                 const std::array<u32, 3>& workgroup_size,
                                 vk::ShaderModule module, SerializationSupport& sdata,
                                 bool preloading,
                                 vk::PipelineCreateFlags build_flags)
    : Pipeline{instance, scheduler, desc_heap, profile, pipeline_cache, true},
      compute_key{compute_key_} {
    auto& info = stages[int(Shader::LogicalStage::Compute)];
    info = &info_;
    const auto debug_str = GetDebugString();

    const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_size_ci = {
        .requiredSubgroupSize = 64,
    };
    const bool avoid_redundant_required_subgroup_size =
        instance.GetCapabilityProfile().HasQuirk(
            CapabilityQuirk::AvoidRequiredSubgroupSizeForSubgroupIndependentCompute) &&
        !info->uses_subgroup_ops;
    const bool require_subgroup_size_64 =
        !avoid_redundant_required_subgroup_size &&
        instance.IsSubgroupSize64Supported(workgroup_size);
    const vk::PipelineShaderStageCreateInfo shader_ci = {
        .pNext = require_subgroup_size_64 ? &subgroup_size_ci : nullptr,
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = module,
        .pName = "main",
    };

    u32 binding{};
    size_t storage_buffer_index{};
    if (!preloading) {
        sdata.storage_buffers.clear();
        sdata.storage_buffers.reserve(info->buffers.size());
        sdata.image_bindings.clear();
        sdata.image_bindings.reserve(info->images.size());
    }
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    for (const auto& buffer : info->buffers) {
        const auto sharp = preloading ? AmdGpu::Buffer{} : buffer.GetSharp(*info);
        const bool is_storage =
            preloading
                ? storage_buffer_index < sdata.storage_buffers.size() &&
                      sdata.storage_buffers[storage_buffer_index] != 0
                : buffer.IsStorage(sharp);
        if (!preloading) {
            sdata.storage_buffers.push_back(is_storage ? 1u : 0u);
        }
        ++storage_buffer_index;
        bindings.push_back({
            .binding = binding++,
            .descriptorType = is_storage ? vk::DescriptorType::eStorageBuffer
                                         : vk::DescriptorType::eUniformBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }
    size_t image_binding_index{};
    for (const auto& image : info->images) {
        const u32 num_bindings =
            preloading
                ? (image_binding_index < sdata.image_bindings.size()
                       ? sdata.image_bindings[image_binding_index]
                       : 1u)
                : image.NumBindings(*info);
        if (!preloading) {
            sdata.image_bindings.push_back(num_bindings);
        }
        ++image_binding_index;
        bindings.push_back({
            .binding = binding,
            .descriptorType = image.is_written ? vk::DescriptorType::eStorageImage
                                               : vk::DescriptorType::eSampledImage,
            .descriptorCount = num_bindings,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
        binding += num_bindings;
    }
    for (const auto& sampler : info->samplers) {
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }

    const vk::PushConstantRange push_constants = {
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };

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
    const auto device = instance.GetDevice();
    auto [descriptor_set_result, descriptor_set] =
        device.createDescriptorSetLayoutUnique(desc_layout_ci);
    if (descriptor_set_result != vk::Result::eSuccess) {
        build_result = descriptor_set_result;
        LOG_ERROR(Render_Vulkan, "Failed to create compute descriptor set layout: {}",
                  vk::to_string(descriptor_set_result));
        return;
    }
#ifdef __ANDROID__
    if (instance.GetCapabilityProfile().diagnostic_a750_descriptor_topology) {
        RecordPipelineDescriptorTopology(
            uses_push_descriptors, binding, instance.MaxPushDescriptors(),
            instance.MaxUpdateAfterBindDescriptors());
    }
#endif
    desc_layout = std::move(descriptor_set);

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1U,
        .pPushConstantRanges = &push_constants,
    };
    auto [layout_result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);
    if (layout_result != vk::Result::eSuccess) {
        build_result = layout_result;
        LOG_ERROR(Render_Vulkan, "Failed to create compute pipeline layout: {}",
                  vk::to_string(layout_result));
        return;
    }
    pipeline_layout = std::move(layout);
    SetObjectName(device, *pipeline_layout, "Compute PipelineLayout {}", debug_str);

    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .flags = build_flags,
        .stage = shader_ci,
        .layout = *pipeline_layout,
    };
#ifdef __ANDROID__
    // This is an explicit, marker-gated observation point.  Pipeline creation
    // can stall on any driver family for reasons unrelated to the float-control
    // quirk, so the diagnostic must bracket the same call on Turnip as well.
    const bool diagnose_pipeline_create =
        ComputePipelineCreateDiagnosticRequested();
    if (diagnose_pipeline_create) {
        __android_log_print(
            ANDROID_LOG_WARN, "LSX4Native",
            "[EXECUTOR_COMPUTE_PIPELINE_CREATE] event=begin key=0x%llx "
            "pgmHash=0x%llx pgmBase=0x%llx workgroup=%ux%ux%u "
            "subgroupOps=%u requireSubgroup64=%u buffers=%zu images=%zu "
            "samplers=%zu storageImages=%u preloading=%u flags=0x%x",
            static_cast<unsigned long long>(compute_key.value),
            static_cast<unsigned long long>(info->pgm_hash),
            static_cast<unsigned long long>(info->pgm_base),
            workgroup_size[0], workgroup_size[1], workgroup_size[2],
            info->uses_subgroup_ops ? 1u : 0u,
            require_subgroup_size_64 ? 1u : 0u, info->buffers.size(),
            info->images.size(), info->samplers.size(),
            info->has_storage_images ? 1u : 0u, preloading ? 1u : 0u,
            static_cast<unsigned>(static_cast<VkPipelineCreateFlags>(build_flags)));
    }
#endif
    auto [pipeline_result, pipe] =
        instance.GetDevice().createComputePipelineUnique(pipeline_cache, compute_pipeline_ci);
#ifdef __ANDROID__
    if (diagnose_pipeline_create) {
        __android_log_print(
            ANDROID_LOG_WARN, "LSX4Native",
            "[EXECUTOR_COMPUTE_PIPELINE_CREATE] event=end key=0x%llx "
            "pgmHash=0x%llx result=%d",
            static_cast<unsigned long long>(compute_key.value),
            static_cast<unsigned long long>(info->pgm_hash),
            static_cast<int>(pipeline_result));
    }
#endif
    build_result = pipeline_result;
    if (pipeline_result != vk::Result::eSuccess) {
        if (pipeline_result != vk::Result::ePipelineCompileRequired) {
            LOG_ERROR(Render_Vulkan, "Failed to create compute pipeline: {}",
                      vk::to_string(pipeline_result));
        }
        return;
    }
    pipeline = std::move(pipe);
    SetObjectName(device, *pipeline, "Compute Pipeline {}", debug_str);
}

ComputePipeline::~ComputePipeline() = default;

}
