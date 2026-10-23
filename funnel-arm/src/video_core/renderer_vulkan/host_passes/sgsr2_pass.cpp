// SPDX-FileCopyrightText: Copyright 2026 LSX4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/host_passes/sgsr2_pass.h"

#include <algorithm>
#include <cstring>

#include "common/assert.h"
#include "video_core/host_shaders/sgsr2_convert_comp.h"
#include "video_core/host_shaders/sgsr2_upscale_comp.h"
#include "video_core/host_shaders/temporal_inputs_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_legacy_sync.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan::HostPasses {
namespace {

struct TemporalInputConstants {
    u32 render_size[2];
    u32 reset;
    u32 reserved;
};

struct ConvertConstants {
    u32 render_size[2];
    float render_size_rcp[2];
    float clip_to_previous[16];
    float pre_exposure;
    float camera_fov_horizontal;
};
static_assert(sizeof(ConvertConstants) == 88);

struct UpscaleConstants {
    u32 render_size[2];
    u32 display_size[2];
    float render_size_rcp[2];
    float display_size_rcp[2];
    float jitter_offset[2];
    float pre_exposure;
    float minimum_lerp_contribution;
    u32 same_camera_frame_count;
    u32 reset;
};
static_assert(sizeof(UpscaleConstants) == 56);

constexpr vk::ImageSubresourceRange ColorRange{
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .levelCount = 1,
    .layerCount = 1,
};

} // namespace

void Sgsr2Pass::Create(const Instance& instance, VmaAllocator allocator_, u32 num_images_) {
    device = instance.GetDevice();
    allocator = allocator_;
    num_images = num_images_;
    uses_push_descriptors = instance.IsPushDescriptorSupported();

    sampler = Check<"create SGSR2 sampler">(device.createSamplerUnique({
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxAnisotropy = 1.0f,
        .maxLod = 0.0f,
    }));

    const auto layout_flags = uses_push_descriptors
        ? vk::DescriptorSetLayoutCreateFlags{
              vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptor}
        : vk::DescriptorSetLayoutCreateFlags{};
    const std::array input_bindings{
        vk::DescriptorSetLayoutBinding{0, vk::DescriptorType::eCombinedImageSampler, 1,
                                       vk::ShaderStageFlagBits::eCompute},
        vk::DescriptorSetLayoutBinding{1, vk::DescriptorType::eCombinedImageSampler, 1,
                                       vk::ShaderStageFlagBits::eCompute},
        vk::DescriptorSetLayoutBinding{2, vk::DescriptorType::eStorageImage, 1,
                                       vk::ShaderStageFlagBits::eCompute},
        vk::DescriptorSetLayoutBinding{3, vk::DescriptorType::eStorageImage, 1,
                                       vk::ShaderStageFlagBits::eCompute},
        vk::DescriptorSetLayoutBinding{4, vk::DescriptorType::eStorageImage, 1,
                                       vk::ShaderStageFlagBits::eCompute},
        vk::DescriptorSetLayoutBinding{5, vk::DescriptorType::eStorageImage, 1,
                                       vk::ShaderStageFlagBits::eCompute},
    };
    input_layout = Check<"create temporal-input descriptor layout">(
        device.createDescriptorSetLayoutUnique({
            .flags = layout_flags,
            .bindingCount = static_cast<u32>(input_bindings.size()),
            .pBindings = input_bindings.data(),
        }));
    const std::array sgsr_bindings{
        vk::DescriptorSetLayoutBinding{1, vk::DescriptorType::eCombinedImageSampler, 1,
                                       vk::ShaderStageFlagBits::eCompute},
        vk::DescriptorSetLayoutBinding{2, vk::DescriptorType::eCombinedImageSampler, 1,
                                       vk::ShaderStageFlagBits::eCompute},
        vk::DescriptorSetLayoutBinding{3, vk::DescriptorType::eCombinedImageSampler, 1,
                                       vk::ShaderStageFlagBits::eCompute},
        vk::DescriptorSetLayoutBinding{4, vk::DescriptorType::eStorageImage, 1,
                                       vk::ShaderStageFlagBits::eCompute},
        vk::DescriptorSetLayoutBinding{5, vk::DescriptorType::eStorageImage, 1,
                                       vk::ShaderStageFlagBits::eCompute},
    };
    sgsr_layout = Check<"create SGSR2 descriptor layout">(
        device.createDescriptorSetLayoutUnique({
            .flags = layout_flags,
            .bindingCount = static_cast<u32>(sgsr_bindings.size()),
            .pBindings = sgsr_bindings.data(),
        }));

    if (!uses_push_descriptors) {
        const std::array pool_sizes{
            vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler,
                                   num_images * 8},
            vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage,
                                   num_images * 8},
        };
        descriptor_pool = Check<"create SGSR2 descriptor pool">(
            device.createDescriptorPoolUnique({
                .maxSets = num_images * 3,
                .poolSizeCount = static_cast<u32>(pool_sizes.size()),
                .pPoolSizes = pool_sizes.data(),
            }));
        const std::vector<vk::DescriptorSetLayout> input_layouts(num_images,
                                                                  input_layout.get());
        input_sets = Check<"allocate temporal-input descriptor sets">(
            device.allocateDescriptorSets({
                .descriptorPool = descriptor_pool.get(),
                .descriptorSetCount = num_images,
                .pSetLayouts = input_layouts.data(),
            }));
        const std::vector<vk::DescriptorSetLayout> sgsr_layouts(num_images * 2,
                                                                sgsr_layout.get());
        auto sgsr_sets = Check<"allocate SGSR2 descriptor sets">(
            device.allocateDescriptorSets({
                .descriptorPool = descriptor_pool.get(),
                .descriptorSetCount = num_images * 2,
                .pSetLayouts = sgsr_layouts.data(),
            }));
        convert_sets.assign(sgsr_sets.begin(), sgsr_sets.begin() + num_images);
        upscale_sets.assign(sgsr_sets.begin() + num_images, sgsr_sets.end());
    }

    const vk::PushConstantRange input_push{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .size = sizeof(TemporalInputConstants),
    };
    input_pipeline_layout = Check<"create temporal-input pipeline layout">(
        device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &input_layout.get(),
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &input_push,
        }));
    const vk::PushConstantRange sgsr_push{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .size = sizeof(ConvertConstants),
    };
    sgsr_pipeline_layout = Check<"create SGSR2 pipeline layout">(
        device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &sgsr_layout.get(),
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &sgsr_push,
        }));

    const auto input_module = Compile(HostShaders::TEMPORAL_INPUTS_COMP,
                                      vk::ShaderStageFlagBits::eCompute, device);
    const auto convert_module = Compile(HostShaders::SGSR2_CONVERT_COMP,
                                        vk::ShaderStageFlagBits::eCompute, device,
                                        {"REQUEST_NDC_Y_UP=1"});
    const auto upscale_module = Compile(HostShaders::SGSR2_UPSCALE_COMP,
                                        vk::ShaderStageFlagBits::eCompute, device,
                                        {"REQUEST_NDC_Y_UP=1"});
    ASSERT(input_module && convert_module && upscale_module);
    const auto make_pipeline = [&](vk::ShaderModule module, vk::PipelineLayout layout,
                                   const char* name) {
        auto pipeline = Check<"create temporal compute pipeline">(
            device.createComputePipelineUnique({}, {
                .stage{.stage = vk::ShaderStageFlagBits::eCompute,
                       .module = module, .pName = "main"},
                .layout = layout,
            }));
        SetObjectName(device, pipeline.get(), name);
        return pipeline;
    };
    input_pipeline = make_pipeline(input_module, input_pipeline_layout.get(),
                                   "temporal input reconstruction");
    convert_pipeline = make_pipeline(convert_module, sgsr_pipeline_layout.get(),
                                     "SGSR2 convert");
    upscale_pipeline = make_pipeline(upscale_module, sgsr_pipeline_layout.get(),
                                     "SGSR2 upscale");
    device.destroyShaderModule(input_module);
    device.destroyShaderModule(convert_module);
    device.destroyShaderModule(upscale_module);
}

std::unique_ptr<Sgsr2Pass::ImageResource> Sgsr2Pass::CreateImage(
    vk::Extent2D size, vk::Format format, vk::ImageUsageFlags extra_usage) const {
    auto resource = std::make_unique<ImageResource>(device, allocator);
    resource->image.Create({
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {.width = size.width, .height = size.height, .depth = 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eStorage |
                 vk::ImageUsageFlagBits::eTransferDst | extra_usage,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    resource->view = Check<"create SGSR2 image view">(device.createImageViewUnique({
        .image = resource->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = ColorRange,
    }));
    return resource;
}

void Sgsr2Pass::RecreateResources(vk::Extent2D render_size,
                                  vk::Extent2D display_size) {
    render_extent = render_size;
    display_extent = display_size;
    for (auto& image : low_color_history) {
        image = CreateImage(render_size, vk::Format::eR16G16B16A16Sfloat);
    }
    for (auto& image : display_history) {
        image = CreateImage(display_size, vk::Format::eR16G16B16A16Sfloat);
    }
    reconstructed_depth = CreateImage(render_size, vk::Format::eR32Sfloat);
    reconstructed_velocity = CreateImage(render_size, vk::Format::eR16G16B16A16Sfloat);
    reconstructed_raw_velocity = CreateImage(render_size,
                                              vk::Format::eR16G16Sfloat);
    motion_depth_clip = CreateImage(render_size, vk::Format::eR16G16B16A16Sfloat);
    ycocg_color = CreateImage(render_size, vk::Format::eR32Uint);
    scene_outputs.clear();
    scene_outputs.reserve(num_images);
    for (u32 i = 0; i < num_images; ++i) {
        scene_outputs.push_back(CreateImage(display_size,
                                            vk::Format::eR16G16B16A16Sfloat,
                                            vk::ImageUsageFlagBits::eTransferSrc |
                                            vk::ImageUsageFlagBits::eColorAttachment));
    }
    current_image = 0;
    history_index = 0;
    history_valid = false;
    sgsr_history_valid = false;
}

void Sgsr2Pass::Bind(Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                     vk::PipelineLayout pipeline_layout,
                     vk::DescriptorSetLayout descriptor_layout,
                     std::span<vk::WriteDescriptorSet> writes) {
    scheduler.BindHostDescriptors(cmdbuf, vk::PipelineBindPoint::eCompute,
                                  pipeline_layout, descriptor_layout, writes);
}

Sgsr2Pass::PreparedTemporalInputs Sgsr2Pass::PrepareForExternalTemporal(
    Scheduler& scheduler, vk::CommandBuffer cmdbuf, vk::ImageView color,
    vk::Extent2D render_size, vk::Extent2D display_size,
    const TemporalInputs& inputs) {
    if (render_size != render_extent || display_size != display_extent) {
        RecreateResources(render_size, display_size);
    }
    const u32 frame_id = current_image++;
    if (current_image >= num_images) {
        current_image = 0;
    }
    const u32 previous_index = history_index;
    const u32 next_index = history_index ^ 1u;
    const bool reset = inputs.reset || !history_valid;

    if (reset) {
        const vk::ImageMemoryBarrier2 to_clear{
            .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = low_color_history[previous_index]->image,
            .subresourceRange = ColorRange,
        };
        PipelineBarrier(scheduler, cmdbuf,
                        {.imageMemoryBarrierCount = 1,
                         .pImageMemoryBarriers = &to_clear});
        const vk::ClearColorValue zero{std::array<float, 4>{0.f, 0.f, 0.f, 0.f}};
        cmdbuf.clearColorImage(low_color_history[previous_index]->image,
                              vk::ImageLayout::eTransferDstOptimal, zero, ColorRange);
        const vk::ImageMemoryBarrier2 readable{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = low_color_history[previous_index]->image,
            .subresourceRange = ColorRange,
        };
        PipelineBarrier(scheduler, cmdbuf,
                        {.imageMemoryBarrierCount = 1,
                         .pImageMemoryBarriers = &readable});
    }

    const vk::ImageLayout old_layout = history_valid
        ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined;
    const vk::PipelineStageFlags2 old_stage = history_valid
        ? vk::PipelineStageFlagBits2::eComputeShader
        : vk::PipelineStageFlagBits2::eTopOfPipe;
    const vk::AccessFlags2 old_access = history_valid
        ? vk::AccessFlagBits2::eShaderRead : vk::AccessFlags2{};
    const std::array writable_images{
        static_cast<vk::Image>(low_color_history[next_index]->image),
        static_cast<vk::Image>(reconstructed_depth->image),
        static_cast<vk::Image>(reconstructed_velocity->image),
        static_cast<vk::Image>(reconstructed_raw_velocity->image),
    };
    std::array<vk::ImageMemoryBarrier2, 4> to_writable{};
    for (u32 i = 0; i < writable_images.size(); ++i) {
        to_writable[i] = {
            .srcStageMask = old_stage,
            .srcAccessMask = old_access,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .oldLayout = old_layout,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = writable_images[i],
            .subresourceRange = ColorRange,
        };
    }
    PipelineBarrier(scheduler, cmdbuf,
                    {.imageMemoryBarrierCount = to_writable.size(),
                     .pImageMemoryBarriers = to_writable.data()});

    const std::array infos{
        vk::DescriptorImageInfo{sampler.get(), color,
                                vk::ImageLayout::eShaderReadOnlyOptimal},
        vk::DescriptorImageInfo{sampler.get(), low_color_history[previous_index]->view.get(),
                                vk::ImageLayout::eShaderReadOnlyOptimal},
        vk::DescriptorImageInfo{{}, reconstructed_depth->view.get(),
                                vk::ImageLayout::eGeneral},
        vk::DescriptorImageInfo{{}, reconstructed_velocity->view.get(),
                                vk::ImageLayout::eGeneral},
        vk::DescriptorImageInfo{{}, low_color_history[next_index]->view.get(),
                                vk::ImageLayout::eGeneral},
        vk::DescriptorImageInfo{{}, reconstructed_raw_velocity->view.get(),
                                vk::ImageLayout::eGeneral},
    };
    std::array<vk::WriteDescriptorSet, 6> writes{};
    for (u32 i = 0; i < writes.size(); ++i) {
        writes[i] = {
            .dstBinding = i,
            .descriptorCount = 1,
            .descriptorType = i < 2 ? vk::DescriptorType::eCombinedImageSampler
                                    : vk::DescriptorType::eStorageImage,
            .pImageInfo = &infos[i],
        };
    }
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, input_pipeline.get());
    scheduler.MarkPipelineBind(vk::PipelineBindPoint::eCompute);
    Bind(scheduler, cmdbuf, input_pipeline_layout.get(), input_layout.get(), writes);
    scheduler.MarkDescriptorBind(vk::PipelineBindPoint::eCompute);
    const TemporalInputConstants constants{
        .render_size = {render_size.width, render_size.height},
        .reset = reset ? 1u : 0u,
    };
    cmdbuf.pushConstants(input_pipeline_layout.get(), vk::ShaderStageFlagBits::eCompute,
                         0, sizeof(constants), &constants);
    scheduler.MarkPushConstants(vk::PipelineBindPoint::eCompute);
    cmdbuf.dispatch((render_size.width + 7u) / 8u,
                    (render_size.height + 7u) / 8u, 1);

    std::array<vk::ImageMemoryBarrier2, 4> readable{};
    for (u32 i = 0; i < writable_images.size(); ++i) {
        readable[i] = {
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = writable_images[i],
            .subresourceRange = ColorRange,
        };
    }
    PipelineBarrier(scheduler, cmdbuf,
                    {.imageMemoryBarrierCount = readable.size(),
                     .pImageMemoryBarriers = readable.data()});

    history_index = next_index;
    history_valid = true;
    // The external temporal path advances the shared low-resolution history,
    // but not SGSR2's display/YCoCg history. Force a clean SGSR2 restart if
    // the user switches back without recreating the presenter.
    sgsr_history_valid = false;
    const bool native = inputs.depth_valid && inputs.velocity_valid &&
                        inputs.depth_image && inputs.velocity_image;
    last_native_inputs = native;
    return {
        .color_image = static_cast<vk::Image>(low_color_history[next_index]->image),
        .depth_image = native ? inputs.depth_image
                              : static_cast<vk::Image>(reconstructed_depth->image),
        .velocity_image = native ? inputs.velocity_image
                                 : static_cast<vk::Image>(reconstructed_raw_velocity->image),
        .color = low_color_history[next_index]->view.get(),
        .depth = native ? inputs.depth : reconstructed_depth->view.get(),
        .velocity = native ? inputs.velocity : reconstructed_raw_velocity->view.get(),
        .native = native,
        .reset = reset,
    };
}

vk::ImageView Sgsr2Pass::Render(Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                                vk::ImageView color, vk::Extent2D render_size,
                                vk::Extent2D display_size,
                                const TemporalInputs& inputs,
                                float minimum_lerp_contribution) {
    if (render_size != render_extent || display_size != display_extent) {
        RecreateResources(render_size, display_size);
    }
    const u32 frame_id = current_image++;
    if (current_image >= num_images) current_image = 0;
    const u32 previous_index = history_index;
    const u32 next_index = history_index ^ 1u;
    const bool reset = inputs.reset || !sgsr_history_valid;
    last_native_inputs = inputs.depth_valid && inputs.velocity_valid;

    if (reset) {
        const std::array initialize_barriers{
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
                .srcAccessMask = vk::AccessFlagBits2::eNone,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .image = low_color_history[previous_index]->image,
                .subresourceRange = ColorRange,
            },
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
                .srcAccessMask = vk::AccessFlagBits2::eNone,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .image = display_history[previous_index]->image,
                .subresourceRange = ColorRange,
            },
        };
        PipelineBarrier(scheduler, cmdbuf, {
            .imageMemoryBarrierCount = initialize_barriers.size(),
            .pImageMemoryBarriers = initialize_barriers.data(),
        });
        const vk::ClearColorValue zero{std::array<float, 4>{0.f, 0.f, 0.f, 0.f}};
        cmdbuf.clearColorImage(low_color_history[previous_index]->image,
                              vk::ImageLayout::eTransferDstOptimal, zero, ColorRange);
        cmdbuf.clearColorImage(display_history[previous_index]->image,
                              vk::ImageLayout::eTransferDstOptimal, zero, ColorRange);
        const std::array initialized_barriers{
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .image = low_color_history[previous_index]->image,
                .subresourceRange = ColorRange,
            },
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .image = display_history[previous_index]->image,
                .subresourceRange = ColorRange,
            },
        };
        PipelineBarrier(scheduler, cmdbuf, {
            .imageMemoryBarrierCount = initialized_barriers.size(),
            .pImageMemoryBarriers = initialized_barriers.data(),
        });
    }

    const vk::ImageLayout reusable_old_layout =
        sgsr_history_valid ? vk::ImageLayout::eShaderReadOnlyOptimal
                           : vk::ImageLayout::eUndefined;
    const vk::PipelineStageFlags2 reusable_src_stage =
        sgsr_history_valid ? vk::PipelineStageFlagBits2::eComputeShader
                           : vk::PipelineStageFlagBits2::eTopOfPipe;
    const vk::AccessFlags2 reusable_src_access =
        sgsr_history_valid ? vk::AccessFlagBits2::eShaderRead : vk::AccessFlags2{};
    std::array write_images{
        static_cast<vk::Image>(low_color_history[next_index]->image),
        static_cast<vk::Image>(reconstructed_depth->image),
        static_cast<vk::Image>(reconstructed_velocity->image),
        static_cast<vk::Image>(reconstructed_raw_velocity->image),
        static_cast<vk::Image>(motion_depth_clip->image),
        static_cast<vk::Image>(ycocg_color->image),
        static_cast<vk::Image>(display_history[next_index]->image),
    };
    std::vector<vk::ImageMemoryBarrier2> write_barriers;
    write_barriers.reserve(write_images.size() + 1);
    for (const vk::Image image : write_images) {
        write_barriers.push_back({
            .srcStageMask = reusable_src_stage,
            .srcAccessMask = reusable_src_access,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .oldLayout = reusable_old_layout,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = image,
            .subresourceRange = ColorRange,
        });
    }
    write_barriers.push_back({
        .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = scene_outputs[frame_id]->image,
        .subresourceRange = ColorRange,
    });
    PipelineBarrier(scheduler, cmdbuf, {
        .imageMemoryBarrierCount = static_cast<u32>(write_barriers.size()),
        .pImageMemoryBarriers = write_barriers.data(),
    });

    const std::array input_infos{
        vk::DescriptorImageInfo{sampler.get(), color,
                                vk::ImageLayout::eShaderReadOnlyOptimal},
        vk::DescriptorImageInfo{sampler.get(), low_color_history[previous_index]->view.get(),
                                vk::ImageLayout::eShaderReadOnlyOptimal},
        vk::DescriptorImageInfo{{}, reconstructed_depth->view.get(),
                                vk::ImageLayout::eGeneral},
        vk::DescriptorImageInfo{{}, reconstructed_velocity->view.get(),
                                vk::ImageLayout::eGeneral},
        vk::DescriptorImageInfo{{}, low_color_history[next_index]->view.get(),
                                vk::ImageLayout::eGeneral},
        vk::DescriptorImageInfo{{}, reconstructed_raw_velocity->view.get(),
                                vk::ImageLayout::eGeneral},
    };
    std::array<vk::WriteDescriptorSet, 6> input_writes{};
    for (u32 i = 0; i < input_writes.size(); ++i) {
        input_writes[i] = {
            .dstBinding = i,
            .descriptorCount = 1,
            .descriptorType = i < 2 ? vk::DescriptorType::eCombinedImageSampler
                                    : vk::DescriptorType::eStorageImage,
            .pImageInfo = &input_infos[i],
        };
    }
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, input_pipeline.get());
    scheduler.MarkPipelineBind(vk::PipelineBindPoint::eCompute);
    Bind(scheduler, cmdbuf, input_pipeline_layout.get(), input_layout.get(), input_writes);
    scheduler.MarkDescriptorBind(vk::PipelineBindPoint::eCompute);
    const TemporalInputConstants input_constants{
        .render_size = {render_size.width, render_size.height},
        .reset = reset ? 1u : 0u,
    };
    cmdbuf.pushConstants(input_pipeline_layout.get(), vk::ShaderStageFlagBits::eCompute,
                         0, sizeof(input_constants), &input_constants);
    scheduler.MarkPushConstants(vk::PipelineBindPoint::eCompute);
    cmdbuf.dispatch((render_size.width + 7u) / 8u,
                    (render_size.height + 7u) / 8u, 1);

    const std::array reconstructed_barriers{
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = reconstructed_depth->image,
            .subresourceRange = ColorRange,
        },
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = reconstructed_velocity->image,
            .subresourceRange = ColorRange,
        },
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = reconstructed_raw_velocity->image,
            .subresourceRange = ColorRange,
        },
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = low_color_history[next_index]->image,
            .subresourceRange = ColorRange,
        },
    };
    PipelineBarrier(scheduler, cmdbuf, {
        .imageMemoryBarrierCount = reconstructed_barriers.size(),
        .pImageMemoryBarriers = reconstructed_barriers.data(),
    });

    const vk::ImageView depth = inputs.depth_valid
                                    ? inputs.depth
                                    : reconstructed_depth->view.get();
    const vk::ImageView velocity = inputs.velocity_valid
                                       ? inputs.velocity
                                       : reconstructed_velocity->view.get();
    const std::array convert_infos{
        vk::DescriptorImageInfo{sampler.get(), color,
                                vk::ImageLayout::eShaderReadOnlyOptimal},
        vk::DescriptorImageInfo{sampler.get(), depth,
                                vk::ImageLayout::eShaderReadOnlyOptimal},
        vk::DescriptorImageInfo{sampler.get(), velocity,
                                vk::ImageLayout::eShaderReadOnlyOptimal},
        vk::DescriptorImageInfo{{}, motion_depth_clip->view.get(),
                                vk::ImageLayout::eGeneral},
        vk::DescriptorImageInfo{{}, ycocg_color->view.get(),
                                vk::ImageLayout::eGeneral},
    };
    std::array<vk::WriteDescriptorSet, 5> convert_writes{};
    for (u32 i = 0; i < convert_writes.size(); ++i) {
        convert_writes[i] = {
            .dstBinding = i + 1,
            .descriptorCount = 1,
            .descriptorType = i < 3 ? vk::DescriptorType::eCombinedImageSampler
                                    : vk::DescriptorType::eStorageImage,
            .pImageInfo = &convert_infos[i],
        };
    }
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, convert_pipeline.get());
    scheduler.MarkPipelineBind(vk::PipelineBindPoint::eCompute);
    Bind(scheduler, cmdbuf, sgsr_pipeline_layout.get(), sgsr_layout.get(), convert_writes);
    scheduler.MarkDescriptorBind(vk::PipelineBindPoint::eCompute);
    ConvertConstants convert_constants{
        .render_size = {render_size.width, render_size.height},
        .render_size_rcp = {1.f / render_size.width, 1.f / render_size.height},
        // SGSR2 defines this as previous pre-exposure / current pre-exposure.
        // The presenter supplies display-referred color without an exposure
        // change, therefore the neutral ratio is exactly one.
        .pre_exposure = 1.f,
        .camera_fov_horizontal = 1.f,
    };
    std::memcpy(convert_constants.clip_to_previous, inputs.clip_to_previous.data(),
                sizeof(convert_constants.clip_to_previous));
    cmdbuf.pushConstants(sgsr_pipeline_layout.get(), vk::ShaderStageFlagBits::eCompute,
                         0, sizeof(convert_constants), &convert_constants);
    scheduler.MarkPushConstants(vk::PipelineBindPoint::eCompute);
    cmdbuf.dispatch((render_size.width + 7u) / 8u,
                    (render_size.height + 7u) / 8u, 1);

    const std::array converted_barriers{
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = motion_depth_clip->image,
            .subresourceRange = ColorRange,
        },
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = ycocg_color->image,
            .subresourceRange = ColorRange,
        },
    };
    PipelineBarrier(scheduler, cmdbuf, {
        .imageMemoryBarrierCount = converted_barriers.size(),
        .pImageMemoryBarriers = converted_barriers.data(),
    });

    const std::array upscale_infos{
        vk::DescriptorImageInfo{sampler.get(), display_history[previous_index]->view.get(),
                                vk::ImageLayout::eShaderReadOnlyOptimal},
        vk::DescriptorImageInfo{sampler.get(), motion_depth_clip->view.get(),
                                vk::ImageLayout::eShaderReadOnlyOptimal},
        vk::DescriptorImageInfo{sampler.get(), ycocg_color->view.get(),
                                vk::ImageLayout::eShaderReadOnlyOptimal},
        vk::DescriptorImageInfo{{}, scene_outputs[frame_id]->view.get(),
                                vk::ImageLayout::eGeneral},
        vk::DescriptorImageInfo{{}, display_history[next_index]->view.get(),
                                vk::ImageLayout::eGeneral},
    };
    std::array<vk::WriteDescriptorSet, 5> upscale_writes{};
    for (u32 i = 0; i < upscale_writes.size(); ++i) {
        upscale_writes[i] = {
            .dstBinding = i + 1,
            .descriptorCount = 1,
            .descriptorType = i < 3 ? vk::DescriptorType::eCombinedImageSampler
                                    : vk::DescriptorType::eStorageImage,
            .pImageInfo = &upscale_infos[i],
        };
    }
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, upscale_pipeline.get());
    scheduler.MarkPipelineBind(vk::PipelineBindPoint::eCompute);
    Bind(scheduler, cmdbuf, sgsr_pipeline_layout.get(), sgsr_layout.get(), upscale_writes);
    scheduler.MarkDescriptorBind(vk::PipelineBindPoint::eCompute);
    const UpscaleConstants upscale_constants{
        .render_size = {render_size.width, render_size.height},
        .display_size = {display_size.width, display_size.height},
        .render_size_rcp = {1.f / render_size.width, 1.f / render_size.height},
        .display_size_rcp = {1.f / display_size.width, 1.f / display_size.height},
        .jitter_offset = {inputs.jitter_x, inputs.jitter_y},
        .pre_exposure = 1.f,
        .minimum_lerp_contribution =
            std::clamp(minimum_lerp_contribution, 0.05f, 1.0f),
        .same_camera_frame_count = inputs.camera_unchanged ? 1u : 0u,
        .reset = reset ? 1u : 0u,
    };
    cmdbuf.pushConstants(sgsr_pipeline_layout.get(), vk::ShaderStageFlagBits::eCompute,
                         0, sizeof(upscale_constants), &upscale_constants);
    scheduler.MarkPushConstants(vk::PipelineBindPoint::eCompute);
    cmdbuf.dispatch((display_size.width + 7u) / 8u,
                    (display_size.height + 7u) / 8u, 1);

    const std::array output_barriers{
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = scene_outputs[frame_id]->image,
            .subresourceRange = ColorRange,
        },
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = display_history[next_index]->image,
            .subresourceRange = ColorRange,
        },
    };
    PipelineBarrier(scheduler, cmdbuf, {
        .imageMemoryBarrierCount = output_barriers.size(),
        .pImageMemoryBarriers = output_barriers.data(),
    });
    history_index = next_index;
    history_valid = true;
    sgsr_history_valid = true;
    return scene_outputs[frame_id]->view.get();
}

} // namespace Vulkan::HostPasses
