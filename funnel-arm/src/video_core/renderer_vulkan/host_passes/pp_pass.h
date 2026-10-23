//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Frame;
class Instance;
class Scheduler;
}

namespace Vulkan::HostPasses {

class PostProcessingPass {
public:
    struct Settings {
        float gamma = 1.0f;
        u32 hdr = 0;
    };

    void Create(const Instance& instance, vk::Format surface_format, u32 num_images);

    void Render(Scheduler& scheduler, vk::CommandBuffer cmdbuf, vk::ImageView input,
                vk::Extent2D input_size, Frame& output, Settings settings);

    void ReleaseFrame(u8 image_id);

private:
    struct LegacyFramebuffer {
        vk::ImageView image_view{};
        vk::Extent2D extent{};
        vk::UniqueFramebuffer framebuffer{};
    };

    vk::Device device{};
    bool uses_dynamic_rendering{};
    bool uses_push_descriptors{};
    vk::UniquePipeline pipeline{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniqueDescriptorPool descriptor_pool{};
    std::vector<vk::DescriptorSet> descriptor_sets;
    vk::UniqueRenderPass legacy_render_pass{};
    std::vector<LegacyFramebuffer> legacy_framebuffers;
    vk::UniqueSampler sampler{};
};

}
