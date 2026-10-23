//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/host_passes/pp_pass.h"

#include "common/assert.h"
#include "common/config.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/host_shaders/post_process_frag.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_legacy_sync.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

#include <boost/container/static_vector.hpp>

namespace Vulkan::HostPasses {

static vk::UniqueRenderPass CreatePostProcessingRenderPass(vk::Device device,
                                                            vk::Format format) {
    const vk::AttachmentDescription color_attachment{
        .format = format,
        .samples = vk::SampleCountFlagBits::e1,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
        .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
        .initialLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .finalLayout = vk::ImageLayout::eColorAttachmentOptimal,
    };
    const vk::AttachmentReference color_reference{
        .attachment = 0,
        .layout = vk::ImageLayout::eColorAttachmentOptimal,
    };
    const vk::SubpassDescription subpass{
        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_reference,
    };
    return Check<"create pp legacy render pass">(device.createRenderPassUnique({
        .attachmentCount = 1,
        .pAttachments = &color_attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
    }));
}

void PostProcessingPass::Create(const Instance& instance, const vk::Format surface_format,
                                u32 num_images) {
    device = instance.GetDevice();
    uses_dynamic_rendering = instance.IsDynamicRenderingSupported();
    uses_push_descriptors = instance.IsPushDescriptorSupported();
    legacy_framebuffers.resize(num_images);
    if (!uses_dynamic_rendering) {
        legacy_render_pass = CreatePostProcessingRenderPass(device, surface_format);
    }
    static const std::array pp_shaders{
        HostShaders::FS_TRI_VERT,
        HostShaders::POST_PROCESS_FRAG,
    };

    boost::container::static_vector<vk::DescriptorSetLayoutBinding, 2> bindings{
        {
            .binding = 0,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        },
    };

    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci{
        .flags = uses_push_descriptors
                     ? vk::DescriptorSetLayoutCreateFlags{
                           vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR}
                     : vk::DescriptorSetLayoutCreateFlags{},
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };

    desc_set_layout = Check<"create pp descriptor set layout">(
        device.createDescriptorSetLayoutUnique(desc_layout_ci));

    if (!uses_push_descriptors) {
        const vk::DescriptorPoolSize pool_size{
            .type = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = num_images,
        };
        descriptor_pool = Check<"create pp descriptor pool">(device.createDescriptorPoolUnique({
            .maxSets = num_images,
            .poolSizeCount = 1,
            .pPoolSizes = &pool_size,
        }));
        const std::vector<vk::DescriptorSetLayout> layouts(num_images, *desc_set_layout);
        descriptor_sets = Check<"allocate pp descriptor sets">(device.allocateDescriptorSets({
            .descriptorPool = *descriptor_pool,
            .descriptorSetCount = num_images,
            .pSetLayouts = layouts.data(),
        }));
    }

    const vk::PushConstantRange push_constants{
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(Settings),
    };

    const auto& vs_module = Compile(pp_shaders[0], vk::ShaderStageFlagBits::eVertex, device);
    ASSERT(vs_module);
    SetObjectName(device, vs_module, "fs_tri.vert");

    const auto& fs_module = Compile(pp_shaders[1], vk::ShaderStageFlagBits::eFragment, device);
    ASSERT(fs_module);
    SetObjectName(device, fs_module, "post_process.frag");

    const std::array shaders_ci{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = vs_module,
            .pName = "main",
        },
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = fs_module,
            .pName = "main",
        },
    };

    const vk::PipelineLayoutCreateInfo layout_info{
        .setLayoutCount = 1U,
        .pSetLayouts = &*desc_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    };

    pipeline_layout =
        Check<"create pp pipeline layout">(device.createPipelineLayoutUnique(layout_info));

    const std::array pp_color_formats{
        surface_format,
    };
    const vk::PipelineRenderingCreateInfo pipeline_rendering_ci{
        .colorAttachmentCount = pp_color_formats.size(),
        .pColorAttachmentFormats = pp_color_formats.data(),
    };

    const vk::PipelineVertexInputStateCreateInfo vertex_input_info{
        .vertexBindingDescriptionCount = 0u,
        .vertexAttributeDescriptionCount = 0u,
    };

    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };

    const vk::Viewport viewport{
        .x = 0.0f,
        .y = 0.0f,
        .width = 1.0f,
        .height = 1.0f,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };

    const vk::Rect2D scissor = {
        .offset = {0, 0},
        .extent = {1, 1},
    };

    const vk::PipelineViewportStateCreateInfo viewport_info{
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor,
    };

    const vk::PipelineRasterizationStateCreateInfo raster_state{
        .depthClampEnable = false,
        .rasterizerDiscardEnable = false,
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eBack,
        .frontFace = vk::FrontFace::eClockwise,
        .depthBiasEnable = false,
        .lineWidth = 1.0f,
    };

    const vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };

    const std::array attachments{
        vk::PipelineColorBlendAttachmentState{
            .blendEnable = false,
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
        },
    };

    const vk::PipelineColorBlendStateCreateInfo color_blending{
        .logicOpEnable = false,
        .logicOp = vk::LogicOp::eCopy,
        .attachmentCount = attachments.size(),
        .pAttachments = attachments.data(),
        .blendConstants = std::array{1.0f, 1.0f, 1.0f, 1.0f},
    };

    const std::array dynamic_states{
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor,
    };

    const vk::PipelineDynamicStateCreateInfo dynamic_info{
        .dynamicStateCount = dynamic_states.size(),
        .pDynamicStates = dynamic_states.data(),
    };

    const vk::GraphicsPipelineCreateInfo pipeline_info{
        .pNext = uses_dynamic_rendering ? &pipeline_rendering_ci : nullptr,
        .stageCount = shaders_ci.size(),
        .pStages = shaders_ci.data(),
        .pVertexInputState = &vertex_input_info,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster_state,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = *pipeline_layout,
        .renderPass = uses_dynamic_rendering ? vk::RenderPass{} : *legacy_render_pass,
    };

    pipeline = Check<"create post process pipeline">(device.createGraphicsPipelineUnique(
        {}, pipeline_info));

    device.destroyShaderModule(vs_module);
    device.destroyShaderModule(fs_module);

    const vk::SamplerCreateInfo sampler_ci{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
    };
    sampler = Check<"create pp sampler">(device.createSamplerUnique(sampler_ci));
}

void PostProcessingPass::Render(Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                                vk::ImageView input, vk::Extent2D input_size, Frame& frame,
                                Settings settings) {
#ifdef __ANDROID__
    const bool presenter_journal =
        scheduler.GetInstance().GetCapabilityProfile().exact_proprietary_adreno750 &&
        ExecutorVkJournalRequested();
    const auto journal_stage = [&](const ExecutorVkJournalPresenterStage stage,
                                   const u32 flags = 0) {
        if (presenter_journal) {
            ExecutorVkJournalPresenter(
                &scheduler, scheduler.CurrentTick(), stage, frame.id,
                input_size.width, input_size.height, frame.width, frame.height,
                flags);
        }
    };
#endif
    if (Config::getVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Host/Post processing",
        });
    }

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const std::array<vk::RenderingAttachmentInfo, 1> attachments{{
        {
            .imageView = frame.image_view,
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
        },
    }};
    const vk::RenderingInfo rendering_info{
        .renderArea{
            .extent{
                .width = frame.width,
                .height = frame.height,
            },
        },
        .layerCount = 1,
        .colorAttachmentCount = attachments.size(),
        .pColorAttachments = attachments.data(),
    };

    vk::DescriptorImageInfo image_info{
        .sampler = *sampler,
        .imageView = input,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };

    std::array set_writes{
        vk::WriteDescriptorSet{
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &image_info,
        },
    };

    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
    scheduler.MarkPipelineBind(vk::PipelineBindPoint::eGraphics);

    const std::array viewports = {
        vk::Viewport{
            .width = static_cast<float>(frame.width),
            .height = static_cast<float>(frame.height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        },
    };

    cmdbuf.setViewport(0, viewports);
    cmdbuf.setScissor(0, vk::Rect2D{
                             .extent{
                                 .width = frame.width,
                                 .height = frame.height,
                             },
                         });

    if (uses_push_descriptors) {
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, *pipeline_layout, 0,
                                    set_writes);
    } else {
        const auto descriptor_set = descriptor_sets[frame.id];
        set_writes[0].dstSet = descriptor_set;
        device.updateDescriptorSets(set_writes, {});
        cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *pipeline_layout, 0,
                                  descriptor_set, {});
    }
    scheduler.MarkDescriptorBind(vk::PipelineBindPoint::eGraphics);
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(Settings),
                         &settings);
    scheduler.MarkPushConstants(vk::PipelineBindPoint::eGraphics);
    scheduler.GetDynamicState().Invalidate();

    if (uses_dynamic_rendering) {
        cmdbuf.beginRendering(rendering_info);
#ifdef __ANDROID__
        journal_stage(ExecutorVkJournalPresenterStage::PostProcessDraw, 1u);
#endif
        cmdbuf.draw(3, 1, 0, 0);
        cmdbuf.endRendering();
    } else {
        auto& legacy = legacy_framebuffers[frame.id];
        const vk::Extent2D frame_extent{frame.width, frame.height};
        if (legacy.image_view != frame.image_view || legacy.extent != frame_extent) {
            legacy.framebuffer.reset();
            legacy.framebuffer =
                Check<"create pp legacy framebuffer">(device.createFramebufferUnique({
                    .renderPass = *legacy_render_pass,
                    .attachmentCount = 1,
                    .pAttachments = &frame.image_view,
                    .width = frame.width,
                    .height = frame.height,
                    .layers = 1,
                }));
            legacy.image_view = frame.image_view;
            legacy.extent = frame_extent;
        }
        const vk::ClearValue clear_value{
            .color = vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}},
        };
        cmdbuf.beginRenderPass(
            vk::RenderPassBeginInfo{
                .renderPass = *legacy_render_pass,
                .framebuffer = *legacy.framebuffer,
                .renderArea = {.extent = frame_extent},
                .clearValueCount = 1,
                .pClearValues = &clear_value,
            },
            vk::SubpassContents::eInline);
#ifdef __ANDROID__
        journal_stage(ExecutorVkJournalPresenterStage::PostProcessDraw, 0u);
#endif
        cmdbuf.draw(3, 1, 0, 0);
        cmdbuf.endRenderPass();
    }

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame.image,
        .subresourceRange = simple_subresource,
    };

    PipelineBarrier(scheduler, cmdbuf, vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });
#ifdef __ANDROID__
    journal_stage(ExecutorVkJournalPresenterStage::PostProcessOutputBarrier,
                  uses_dynamic_rendering ? 1u : 0u);
#endif

    if (Config::getVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }
}

void PostProcessingPass::ReleaseFrame(u8 image_id) {
    if (image_id >= legacy_framebuffers.size()) {
        return;
    }
    legacy_framebuffers[image_id] = {};
}

}
