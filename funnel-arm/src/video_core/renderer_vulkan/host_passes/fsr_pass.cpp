//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include "common/assert.h"
#include "common/config.h"
#include "video_core/host_shaders/fsr_comp.h"
#include "video_core/host_shaders/nis_comp.h"
#include "video_core/host_shaders/spatial_upscale_comp.h"
#include "video_core/mobile_gpu_optimization.h"
#include "video_core/renderer_vulkan/host_passes/fsr_pass.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_legacy_sync.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

#include <NIS_Config.h>
#include <vk_mem_alloc.h>

#define A_CPU
#include "core/debug_state.h"
#include "video_core/host_shaders/fsr/ffx_a.h"
#include "video_core/host_shaders/fsr/ffx_fsr1.h"

typedef u32 uvec4[4];

struct FSRConstants {
    uvec4 Const0;
    uvec4 Const1;
    uvec4 Const2;
    uvec4 Const3;
    uvec4 Sample;
};

struct SpatialConstants {
    u32 input_size[2];
    u32 output_size[2];
    u32 mode;
    float sharpness;
};

namespace Vulkan::HostPasses {

void FsrPass::Create(const Instance& instance, VmaAllocator allocator, u32 num_images) {
    device = instance.GetDevice();
    this->num_images = num_images;
    uses_push_descriptors = instance.IsPushDescriptorSupported();

    sampler = Check<"create upscaling sampler">(device.createSamplerUnique(vk::SamplerCreateInfo{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxAnisotropy = 1.0f,
        .minLod = -1000.0f,
        .maxLod = 1000.0f,
    }));

    std::array<vk::DescriptorSetLayoutBinding, 6> layoutBindings{{
        {
            .binding = 0,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 1,
            .descriptorType = vk::DescriptorType::eStorageImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 2,
            .descriptorType = vk::DescriptorType::eSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
            .pImmutableSamplers = &sampler.get(),
        },
        {
            .binding = 3,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 4,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 5,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
    }};

    descriptor_set_layout =
        Check<"create fsr descriptor set layout">(device.createDescriptorSetLayoutUnique({
            .flags = uses_push_descriptors
                         ? vk::DescriptorSetLayoutCreateFlags{
                               vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptor}
                         : vk::DescriptorSetLayoutCreateFlags{},
            .bindingCount = layoutBindings.size(),
            .pBindings = layoutBindings.data(),
        }));

    if (!uses_push_descriptors) {
        const std::array pool_sizes{
            vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, num_images * 6},
            vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, num_images * 2},
            vk::DescriptorPoolSize{vk::DescriptorType::eSampler, num_images * 2},
            vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, num_images * 2},
        };
        descriptor_pool = Check<"create fsr descriptor pool">(device.createDescriptorPoolUnique({
            .maxSets = num_images * 2,
            .poolSizeCount = static_cast<u32>(pool_sizes.size()),
            .pPoolSizes = pool_sizes.data(),
        }));
        const std::vector<vk::DescriptorSetLayout> layouts(num_images * 2,
                                                           *descriptor_set_layout);
        const auto sets = Check<"allocate fsr descriptor sets">(device.allocateDescriptorSets({
            .descriptorPool = *descriptor_pool,
            .descriptorSetCount = static_cast<u32>(layouts.size()),
            .pSetLayouts = layouts.data(),
        }));
        easu_descriptor_sets.assign(sets.begin(), sets.begin() + num_images);
        rcas_descriptor_sets.assign(sets.begin() + num_images, sets.end());
    }

    const vk::PushConstantRange push_constants{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(FSRConstants),
    };

    const auto& cs_easu_module =
        Compile(HostShaders::FSR_COMP, vk::ShaderStageFlagBits::eCompute, device,
                {
                    "SAMPLE_EASU=1",
                });
    ASSERT(cs_easu_module);
    SetObjectName(device, cs_easu_module, "fsr.comp [EASU]");

    const auto& cs_rcas_module =
        Compile(HostShaders::FSR_COMP, vk::ShaderStageFlagBits::eCompute, device,
                {
                    "SAMPLE_RCAS=1",
                });
    ASSERT(cs_rcas_module);
    SetObjectName(device, cs_rcas_module, "fsr.comp [RCAS]");

    const auto& cs_spatial_module =
        Compile(HostShaders::SPATIAL_UPSCALE_COMP, vk::ShaderStageFlagBits::eCompute,
                device);
    ASSERT(cs_spatial_module);
    SetObjectName(device, cs_spatial_module, "spatial_upscale.comp");

    const auto& cs_nis_module =
        Compile(HostShaders::NIS_COMP, vk::ShaderStageFlagBits::eCompute, device);
    ASSERT(cs_nis_module);
    SetObjectName(device, cs_nis_module, "nis.comp [NVIDIA Image Scaling 1.0.3]");

    pipeline_layout = Check<"fsp pipeline layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &descriptor_set_layout.get(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    }));
    SetObjectName(device, pipeline_layout.get(), "fsr pipeline layout");

    const vk::ComputePipelineCreateInfo easu_pinfo{
        .stage{
            .stage = vk::ShaderStageFlagBits::eCompute,
            .module = cs_easu_module,
            .pName = "main",
        },
        .layout = pipeline_layout.get(),
    };
    easu_pipeline =
        Check<"fsp easu compute pipelines">(device.createComputePipelineUnique({}, easu_pinfo));
    SetObjectName(device, easu_pipeline.get(), "fsr easu pipeline");

    const vk::ComputePipelineCreateInfo rcas_pinfo{
        .stage{
            .stage = vk::ShaderStageFlagBits::eCompute,
            .module = cs_rcas_module,
            .pName = "main",
        },
        .layout = pipeline_layout.get(),
    };
    rcas_pipeline =
        Check<"fsp rcas compute pipelines">(device.createComputePipelineUnique({}, rcas_pinfo));
    SetObjectName(device, rcas_pipeline.get(), "fsr rcas pipeline");

    const vk::ComputePipelineCreateInfo spatial_pinfo{
        .stage{
            .stage = vk::ShaderStageFlagBits::eCompute,
            .module = cs_spatial_module,
            .pName = "main",
        },
        .layout = pipeline_layout.get(),
    };
    spatial_pipeline = Check<"create spatial upscaler compute pipeline">(
        device.createComputePipelineUnique({}, spatial_pinfo));
    SetObjectName(device, spatial_pipeline.get(), "spatial upscaler pipeline");

    const vk::ComputePipelineCreateInfo nis_pinfo{
        .stage{
            .stage = vk::ShaderStageFlagBits::eCompute,
            .module = cs_nis_module,
            .pName = "main",
        },
        .layout = pipeline_layout.get(),
    };
    nis_pipeline = Check<"create NIS compute pipeline">(
        device.createComputePipelineUnique({}, nis_pinfo));
    SetObjectName(device, nis_pipeline.get(), "NVIDIA Image Scaling pipeline");

    device.destroyShaderModule(cs_easu_module);
    device.destroyShaderModule(cs_rcas_module);
    device.destroyShaderModule(cs_spatial_module);
    device.destroyShaderModule(cs_nis_module);

    nis_scale_coefficients =
        std::make_unique<VideoCore::UniqueImage>(device, allocator);
    nis_usm_coefficients =
        std::make_unique<VideoCore::UniqueImage>(device, allocator);
    vk::ImageCreateInfo coefficient_image_info{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR32G32B32A32Sfloat,
        .extent = {.width = 2, .height = 64, .depth = 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eSampled,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    nis_scale_coefficients->Create(coefficient_image_info);
    nis_usm_coefficients->Create(coefficient_image_info);
    vk::ImageViewCreateInfo coefficient_view_info{
        .viewType = vk::ImageViewType::e2D,
        .format = coefficient_image_info.format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    coefficient_view_info.image = static_cast<vk::Image>(*nis_scale_coefficients);
    nis_scale_coefficients_view = Check<"create NIS scale coefficient view">(
        device.createImageViewUnique(coefficient_view_info));
    coefficient_view_info.image = static_cast<vk::Image>(*nis_usm_coefficients);
    nis_usm_coefficients_view = Check<"create NIS USM coefficient view">(
        device.createImageViewUnique(coefficient_view_info));

    constexpr vk::DeviceSize coefficient_bytes =
        sizeof(coef_scale) + sizeof(coef_usm);
    nis_coefficient_upload =
        std::make_unique<VideoCore::UniqueBuffer>(device, allocator);
    VmaAllocationInfo coefficient_allocation{};
    nis_coefficient_upload->Create(
        {.size = coefficient_bytes,
         .usage = vk::BufferUsageFlagBits::eTransferSrc},
        VideoCore::MemoryUsage::Upload, &coefficient_allocation);
    ASSERT(coefficient_allocation.pMappedData != nullptr);
    std::memcpy(coefficient_allocation.pMappedData, coef_scale, sizeof(coef_scale));
    std::memcpy(static_cast<u8*>(coefficient_allocation.pMappedData) + sizeof(coef_scale),
                coef_usm, sizeof(coef_usm));
    vmaFlushAllocation(allocator, nis_coefficient_upload->allocation, 0,
                       coefficient_bytes);

    nis_uniform_buffers.reserve(num_images);
    nis_uniform_mapped.reserve(num_images);
    for (u32 i = 0; i < num_images; ++i) {
        nis_uniform_buffers.emplace_back(device, allocator);
        VmaAllocationInfo uniform_allocation{};
        nis_uniform_buffers.back().Create(
            {.size = sizeof(NISConfig),
             .usage = vk::BufferUsageFlagBits::eUniformBuffer},
            VideoCore::MemoryUsage::Upload, &uniform_allocation);
        ASSERT(uniform_allocation.pMappedData != nullptr);
        nis_uniform_mapped.push_back(uniform_allocation.pMappedData);
    }

    available_imgs.resize(num_images);
    for (int i = 0; i < num_images; ++i) {
        auto& img = available_imgs[i];
        img.id = i;
        img.intermediary_image = VideoCore::UniqueImage(device, allocator);
        img.output_image = VideoCore::UniqueImage(device, allocator);
    }
}

void FsrPass::UploadNisCoefficients(Scheduler& scheduler, vk::CommandBuffer cmdbuf) {
    if (nis_coefficients_uploaded) {
        return;
    }
    constexpr vk::ImageSubresourceRange range{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const std::array to_transfer{
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = static_cast<vk::Image>(*nis_scale_coefficients),
            .subresourceRange = range,
        },
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = static_cast<vk::Image>(*nis_usm_coefficients),
            .subresourceRange = range,
        },
    };
    PipelineBarrier(scheduler, cmdbuf, {
        .imageMemoryBarrierCount = to_transfer.size(),
        .pImageMemoryBarriers = to_transfer.data(),
    });
    vk::BufferImageCopy copy{
        .imageSubresource{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .layerCount = 1,
        },
        .imageExtent = {.width = 2, .height = 64, .depth = 1},
    };
    cmdbuf.copyBufferToImage(*nis_coefficient_upload,
                             static_cast<vk::Image>(*nis_scale_coefficients),
                             vk::ImageLayout::eTransferDstOptimal, copy);
    copy.bufferOffset = sizeof(coef_scale);
    cmdbuf.copyBufferToImage(*nis_coefficient_upload,
                             static_cast<vk::Image>(*nis_usm_coefficients),
                             vk::ImageLayout::eTransferDstOptimal, copy);
    const std::array to_sample{
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = static_cast<vk::Image>(*nis_scale_coefficients),
            .subresourceRange = range,
        },
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = static_cast<vk::Image>(*nis_usm_coefficients),
            .subresourceRange = range,
        },
    };
    PipelineBarrier(scheduler, cmdbuf, {
        .imageMemoryBarrierCount = to_sample.size(),
        .pImageMemoryBarriers = to_sample.data(),
    });
    nis_coefficients_uploaded = true;
}

vk::ImageView FsrPass::Render(Scheduler& scheduler, vk::CommandBuffer cmdbuf, vk::ImageView input,
                               vk::Extent2D input_size, vk::Extent2D output_size, Settings settings,
                               bool hdr) {
    last_effective_mode = Config::UpscalerMode::Off;
    last_fallback_reason = 0;
    if (!settings.enable || settings.mode == Config::UpscalerMode::Off) {
        DebugState.is_using_fsr = false;
#ifdef __ANDROID__
        VideoCore::RecordMobileGpuFsrDecision(false, false, input_size.width,
                                              input_size.height, output_size.width,
                                              output_size.height);
#endif
        return input;
    }
    if (input_size.width >= output_size.width && input_size.height >= output_size.height) {
        DebugState.is_using_fsr = false;
#ifdef __ANDROID__
        VideoCore::RecordMobileGpuFsrDecision(false, false, input_size.width,
                                              input_size.height, output_size.width,
                                              output_size.height);
#endif
        return input;
    }

    NISConfig nis_config{};
    bool use_nis = settings.mode == Config::UpscalerMode::Nis && !hdr &&
                   NVScalerUpdateConfig(
                       nis_config, std::clamp(settings.nis_sharpness, 0.0f, 1.0f),
                       0, 0, input_size.width,
                       input_size.height, input_size.width, input_size.height,
                       0, 0, output_size.width, output_size.height,
                       output_size.width, output_size.height, NISHDRMode::None);
    // NIS 1.0.3 officially supports 0.5..1.0 source/output scale on each
    // axis. Preserve a real upscaler outside that domain instead of dispatching
    // NIS with invalid constants.
    if (settings.mode == Config::UpscalerMode::Nis && !use_nis) {
        settings.mode = Config::UpscalerMode::Fsr1;
        last_fallback_reason = 2;
    }
    last_effective_mode = settings.mode;
    const bool fsr1 = settings.mode == Config::UpscalerMode::Fsr1;
    DebugState.is_using_fsr = fsr1;

    if (output_size != cur_size) {
        ResizeAndInvalidate(output_size.width, output_size.height);
    }
    auto [width, height] = cur_size;

    auto& img = available_imgs[cur_image];
    if (++cur_image >= available_imgs.size()) {
        cur_image = 0;
    }

#ifdef __ANDROID__
    const bool presenter_journal =
        scheduler.GetInstance().GetCapabilityProfile().exact_proprietary_adreno750 &&
        ExecutorVkJournalRequested();
    const auto journal_stage = [&](const ExecutorVkJournalPresenterStage stage,
                                   const u32 flags = 0) {
        if (presenter_journal) {
            ExecutorVkJournalPresenter(
                &scheduler, scheduler.CurrentTick(), stage, img.id,
                input_size.width, input_size.height, output_size.width,
                output_size.height, flags);
        }
    };
#endif

    if (img.dirty) {
        CreateImages(img);
    }

    if (Config::getVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = fsr1 ? "Host/FSR1" : "Host/SpatialUpscaler",
        });
    }

    static const int thread_group_work_region_dim = 16;
    int dispatch_x = (width + (thread_group_work_region_dim - 1)) / thread_group_work_region_dim;
    int dispatch_y = (height + (thread_group_work_region_dim - 1)) / thread_group_work_region_dim;

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };

    if (!fsr1) {
        const bool output_valid = img.output_valid;
        const std::array output_barrier{
            vk::ImageMemoryBarrier2{
                .srcStageMask = output_valid ? vk::PipelineStageFlagBits2::eAllCommands
                                             : vk::PipelineStageFlagBits2::eTopOfPipe,
                .srcAccessMask = output_valid ? vk::AccessFlagBits2::eShaderRead
                                              : vk::AccessFlags2{},
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                .oldLayout = output_valid ? vk::ImageLayout::eShaderReadOnlyOptimal
                                          : vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = img.output_image,
                .subresourceRange = simple_subresource,
            },
        };
        PipelineBarrier(scheduler, cmdbuf, {
            .imageMemoryBarrierCount = output_barrier.size(),
            .pImageMemoryBarriers = output_barrier.data(),
        });
        img.output_valid = true;

        if (use_nis) {
            UploadNisCoefficients(scheduler, cmdbuf);
            std::memcpy(nis_uniform_mapped[img.id], &nis_config, sizeof(nis_config));
            vmaFlushAllocation(nis_uniform_buffers[img.id].allocator,
                               nis_uniform_buffers[img.id].allocation, 0,
                               sizeof(nis_config));
            std::array<vk::DescriptorImageInfo, 5> img_info{{
                {.imageView = input,
                 .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal},
                {.imageView = img.output_image_view.get(),
                 .imageLayout = vk::ImageLayout::eGeneral},
                {.sampler = sampler.get()},
                {.imageView = nis_scale_coefficients_view.get(),
                 .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal},
                {.imageView = nis_usm_coefficients_view.get(),
                 .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal},
            }};
            const vk::DescriptorBufferInfo uniform_info{
                .buffer = nis_uniform_buffers[img.id],
                .offset = 0,
                .range = sizeof(NISConfig),
            };
            std::array<vk::WriteDescriptorSet, 6> writes{{
                {.dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eSampledImage,
                 .pImageInfo = &img_info[0]},
                {.dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage,
                 .pImageInfo = &img_info[1]},
                {.dstBinding = 2, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eSampler,
                 .pImageInfo = &img_info[2]},
                {.dstBinding = 3, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eSampledImage,
                 .pImageInfo = &img_info[3]},
                {.dstBinding = 4, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eSampledImage,
                 .pImageInfo = &img_info[4]},
                {.dstBinding = 5, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eUniformBuffer,
                 .pBufferInfo = &uniform_info},
            }};
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, nis_pipeline.get());
            scheduler.MarkPipelineBind(vk::PipelineBindPoint::eCompute);
            BindDescriptors(cmdbuf, writes, false, img.id);
            scheduler.MarkDescriptorBind(vk::PipelineBindPoint::eCompute);
            cmdbuf.dispatch((width + 31u) / 32u, (height + 23u) / 24u, 1);
        } else {
            std::array<vk::DescriptorImageInfo, 3> img_info{{
                {.imageView = input,
                 .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal},
                {.imageView = img.output_image_view.get(),
                 .imageLayout = vk::ImageLayout::eGeneral},
                {.sampler = sampler.get()},
            }};
            std::array<vk::WriteDescriptorSet, 3> writes{{
                {.dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eSampledImage,
                 .pImageInfo = &img_info[0]},
                {.dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage,
                 .pImageInfo = &img_info[1]},
                {.dstBinding = 2, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eSampler,
                 .pImageInfo = &img_info[2]},
            }};
            float spatial_parameter = 0.0f;
            if (settings.mode == Config::UpscalerMode::SharpBilinear) {
                spatial_parameter = std::clamp(
                    settings.sharp_bilinear_strength, 0.0f, 1.0f);
            } else if (settings.mode == Config::UpscalerMode::Bicubic) {
                spatial_parameter = static_cast<float>(
                    std::min(settings.bicubic_kernel, 2u));
            } else if (settings.mode == Config::UpscalerMode::Sgsr1) {
                spatial_parameter = std::clamp(
                    settings.sgsr1_edge_sharpness, 1.0f, 2.0f);
            }
            const SpatialConstants spatial_constants{
                .input_size = {input_size.width, input_size.height},
                .output_size = {output_size.width, output_size.height},
                .mode = static_cast<u32>(settings.mode),
                .sharpness = spatial_parameter,
            };
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, spatial_pipeline.get());
            scheduler.MarkPipelineBind(vk::PipelineBindPoint::eCompute);
            BindDescriptors(cmdbuf, writes, false, img.id);
            scheduler.MarkDescriptorBind(vk::PipelineBindPoint::eCompute);
            cmdbuf.pushConstants(pipeline_layout.get(), vk::ShaderStageFlagBits::eCompute, 0,
                                 sizeof(spatial_constants), &spatial_constants);
            scheduler.MarkPushConstants(vk::PipelineBindPoint::eCompute);
            cmdbuf.dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1);
        }

        const std::array read_barrier{
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .image = img.output_image,
                .subresourceRange = simple_subresource,
            },
        };
        PipelineBarrier(scheduler, cmdbuf, {
            .imageMemoryBarrierCount = read_barrier.size(),
            .pImageMemoryBarriers = read_barrier.data(),
        });
        if (Config::getVkHostMarkersEnabled()) {
            cmdbuf.endDebugUtilsLabelEXT();
        }
#ifdef __ANDROID__
        VideoCore::RecordMobileGpuFsrDecision(false, false, input_size.width,
                                              input_size.height, output_size.width,
                                              output_size.height);
#endif
        return img.output_image_view.get();
    }

    const bool enter_intermediary = settings.use_rcas;
    const bool enter_valid = enter_intermediary ? img.intermediary_valid
                                                : img.output_valid;
    const vk::Image enter_image = enter_intermediary
        ? static_cast<vk::Image>(img.intermediary_image)
        : static_cast<vk::Image>(img.output_image);
    const std::array enter_barrier{
        vk::ImageMemoryBarrier2{
            .srcStageMask = enter_valid ? vk::PipelineStageFlagBits2::eAllCommands
                                        : vk::PipelineStageFlagBits2::eTopOfPipe,
            .srcAccessMask = enter_valid ? vk::AccessFlagBits2::eShaderRead
                                         : vk::AccessFlags2{},
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .oldLayout = enter_valid ? vk::ImageLayout::eShaderReadOnlyOptimal
                                     : vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = enter_image,
            .subresourceRange = simple_subresource,
        },
    };
    PipelineBarrier(scheduler, cmdbuf, {
        .imageMemoryBarrierCount = enter_barrier.size(),
        .pImageMemoryBarriers = enter_barrier.data(),
    });
#ifdef __ANDROID__
    journal_stage(ExecutorVkJournalPresenterStage::FsrEnterBarrier,
                  settings.use_rcas ? 1u : 0u);
#endif

    FSRConstants consts{};
    FsrEasuCon(reinterpret_cast<AU1*>(&consts.Const0), reinterpret_cast<AU1*>(&consts.Const1),
               reinterpret_cast<AU1*>(&consts.Const2), reinterpret_cast<AU1*>(&consts.Const3),
               static_cast<AF1>(input_size.width), static_cast<AF1>(input_size.height),
               static_cast<AF1>(input_size.width), static_cast<AF1>(input_size.height), (AF1)width,
               (AF1)height);
    consts.Sample[0] = hdr && !settings.use_rcas ? 1 : 0;
    consts.Sample[1] = width;
    consts.Sample[2] = height;

    if (settings.use_rcas) {

        {
            std::array<vk::DescriptorImageInfo, 3> img_info{{
                {
                    .imageView = input,
                    .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                },
                {
                    .imageView = img.intermediary_image_view.get(),
                    .imageLayout = vk::ImageLayout::eGeneral,
                },
                {
                    .sampler = sampler.get(),
                },
            }};

            std::array<vk::WriteDescriptorSet, 3> set_writes{{
                {
                    .dstBinding = 0,
                    .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eSampledImage,
                    .pImageInfo = &img_info[0],
                },
                {
                    .dstBinding = 1,
                    .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eStorageImage,
                    .pImageInfo = &img_info[1],
                },
                {
                    .dstBinding = 2,
                    .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eSampler,
                    .pImageInfo = &img_info[2],
                },
            }};

            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, easu_pipeline.get());
            scheduler.MarkPipelineBind(vk::PipelineBindPoint::eCompute);
            BindDescriptors(cmdbuf, set_writes, false, img.id);
            scheduler.MarkDescriptorBind(vk::PipelineBindPoint::eCompute);
            cmdbuf.pushConstants(pipeline_layout.get(), vk::ShaderStageFlagBits::eCompute, 0,
                                 sizeof(FSRConstants), &consts);
            scheduler.MarkPushConstants(vk::PipelineBindPoint::eCompute);
#ifdef __ANDROID__
            journal_stage(ExecutorVkJournalPresenterStage::FsrEasuDispatch, 1u);
#endif
            cmdbuf.dispatch(dispatch_x, dispatch_y, 1);
        }

        std::array img_barrier{
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .image = img.intermediary_image,
                .subresourceRange = simple_subresource,
            },
            vk::ImageMemoryBarrier2{
                .srcStageMask = img.output_valid
                                    ? vk::PipelineStageFlagBits2::eAllCommands
                                    : vk::PipelineStageFlagBits2::eTopOfPipe,
                .srcAccessMask = img.output_valid
                                    ? vk::AccessFlagBits2::eShaderRead
                                    : vk::AccessFlags2{},
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                .oldLayout = img.output_valid
                                 ? vk::ImageLayout::eShaderReadOnlyOptimal
                                 : vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = img.output_image,
                .subresourceRange = simple_subresource,
            },
        };
        PipelineBarrier(scheduler, cmdbuf, vk::DependencyInfo{
            .imageMemoryBarrierCount = img_barrier.size(),
            .pImageMemoryBarriers = img_barrier.data(),
        });
        img.intermediary_valid = true;
#ifdef __ANDROID__
        journal_stage(ExecutorVkJournalPresenterStage::FsrRcasBarrier, 1u);
#endif

        {
            consts = {};
            FsrRcasCon(reinterpret_cast<AU1*>(&consts.Const0), settings.rcas_attenuation);
            consts.Sample[0] = hdr ? 1 : 0;
            consts.Sample[1] = width;
            consts.Sample[2] = height;

            std::array<vk::DescriptorImageInfo, 3> img_info{{
                {
                    .imageView = img.intermediary_image_view.get(),
                    .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                },
                {
                    .imageView = img.output_image_view.get(),
                    .imageLayout = vk::ImageLayout::eGeneral,
                },
                {
                    .sampler = sampler.get(),
                },
            }};

            std::array<vk::WriteDescriptorSet, 3> set_writes{{
                {
                    .dstBinding = 0,
                    .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eSampledImage,
                    .pImageInfo = &img_info[0],
                },
                {
                    .dstBinding = 1,
                    .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eStorageImage,
                    .pImageInfo = &img_info[1],
                },
                {
                    .dstBinding = 2,
                    .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eSampler,
                    .pImageInfo = &img_info[2],
                },
            }};

            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, rcas_pipeline.get());
            scheduler.MarkPipelineBind(vk::PipelineBindPoint::eCompute);
            BindDescriptors(cmdbuf, set_writes, true, img.id);
            scheduler.MarkDescriptorBind(vk::PipelineBindPoint::eCompute);
            cmdbuf.pushConstants(pipeline_layout.get(), vk::ShaderStageFlagBits::eCompute, 0,
                                 sizeof(FSRConstants), &consts);
            scheduler.MarkPushConstants(vk::PipelineBindPoint::eCompute);
#ifdef __ANDROID__
            journal_stage(ExecutorVkJournalPresenterStage::FsrRcasDispatch, 1u);
#endif
            cmdbuf.dispatch(dispatch_x, dispatch_y, 1);
        }

    } else {
        std::array<vk::DescriptorImageInfo, 3> img_info{{
            {
                .imageView = input,
                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            },
            {
                .imageView = img.output_image_view.get(),
                .imageLayout = vk::ImageLayout::eGeneral,
            },
            {
                .sampler = sampler.get(),
            },
        }};

        std::array<vk::WriteDescriptorSet, 3> set_writes{{
            {
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eSampledImage,
                .pImageInfo = &img_info[0],
            },
            {
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageImage,
                .pImageInfo = &img_info[1],
            },
            {
                .dstBinding = 2,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eSampler,
                .pImageInfo = &img_info[2],
            },
        }};

        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, easu_pipeline.get());
        scheduler.MarkPipelineBind(vk::PipelineBindPoint::eCompute);
        BindDescriptors(cmdbuf, set_writes, false, img.id);
        scheduler.MarkDescriptorBind(vk::PipelineBindPoint::eCompute);
        cmdbuf.pushConstants(pipeline_layout.get(), vk::ShaderStageFlagBits::eCompute, 0,
                             sizeof(FSRConstants), &consts);
        scheduler.MarkPushConstants(vk::PipelineBindPoint::eCompute);
#ifdef __ANDROID__
        journal_stage(ExecutorVkJournalPresenterStage::FsrEasuDispatch, 0u);
#endif
        cmdbuf.dispatch(dispatch_x, dispatch_y, 1);
    }

    const std::array return_barrier{
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = img.output_image,
            .subresourceRange = simple_subresource,
        },
    };
    PipelineBarrier(scheduler, cmdbuf, {
        .imageMemoryBarrierCount = return_barrier.size(),
        .pImageMemoryBarriers = return_barrier.data(),
    });
    img.output_valid = true;
#ifdef __ANDROID__
    journal_stage(ExecutorVkJournalPresenterStage::FsrOutputBarrier,
                  settings.use_rcas ? 1u : 0u);
#endif

    if (Config::getVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

#ifdef __ANDROID__
    // This is deliberately after the dispatches and their output barrier: the
    // proof counter means encoded EASU/RCAS work, not merely an enabled switch.
    VideoCore::RecordMobileGpuFsrDecision(true, settings.use_rcas, input_size.width,
                                          input_size.height, output_size.width,
                                          output_size.height);
#endif

    return img.output_image_view.get();
}

void FsrPass::BindDescriptors(vk::CommandBuffer cmdbuf,
                              std::span<vk::WriteDescriptorSet> writes, bool rcas,
                              u8 image_id) {
    if (uses_push_descriptors) {
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, pipeline_layout.get(), 0,
                                    writes);
        return;
    }
    const vk::DescriptorSet set =
        rcas ? rcas_descriptor_sets[image_id] : easu_descriptor_sets[image_id];
    for (auto& write : writes) {
        write.dstSet = set;
    }
    device.updateDescriptorSets(writes, {});
    cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipeline_layout.get(), 0, set, {});
}

void FsrPass::ResizeAndInvalidate(u32 width, u32 height) {
    this->cur_size = vk::Extent2D{
        .width = width,
        .height = height,
    };
    for (int i = 0; i < num_images; ++i) {
        available_imgs[i].dirty = true;
    }
}

void FsrPass::CreateImages(Img& img) const {
    img.dirty = false;
    img.intermediary_valid = false;
    img.output_valid = false;

    vk::ImageCreateInfo image_create_info{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent{
            .width = cur_size.width,
            .height = cur_size.height,
            .depth = 1,
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    img.intermediary_image.Create(image_create_info);
    SetObjectName(device, static_cast<vk::Image>(img.intermediary_image),
                  "FSR Intermediary Image #{}", img.id);
    image_create_info.usage = vk::ImageUsageFlagBits::eTransferSrc |
                              vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage |
                              vk::ImageUsageFlagBits::eColorAttachment;
    img.output_image.Create(image_create_info);
    SetObjectName(device, static_cast<vk::Image>(img.output_image), "FSR Output Image #{}", img.id);

    vk::ImageViewCreateInfo image_view_create_info{
        .image = img.intermediary_image,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    img.intermediary_image_view = Check<"create fsr intermediary image view">(
        device.createImageViewUnique(image_view_create_info));
    SetObjectName(device, img.intermediary_image_view.get(), "FSR Intermediary ImageView #{}",
                  img.id);

    image_view_create_info.image = img.output_image;
    img.output_image_view =
        Check<"create fsr output image view">(device.createImageViewUnique(image_view_create_info));
    SetObjectName(device, img.output_image_view.get(), "FSR Output ImageView #{}", img.id);
}

}
