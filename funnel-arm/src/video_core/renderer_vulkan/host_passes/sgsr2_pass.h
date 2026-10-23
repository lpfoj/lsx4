// SPDX-FileCopyrightText: Copyright 2026 LSX4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>
#include <span>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {
class Instance;
class Scheduler;
}

namespace Vulkan::HostPasses {

class Sgsr2Pass {
public:
    struct TemporalInputs {
        vk::Image depth_image{};
        vk::Image velocity_image{};
        vk::ImageView depth{};
        vk::ImageView velocity{};
        std::array<float, 16> clip_to_previous{
            1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
            0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
        float jitter_x{};
        float jitter_y{};
        bool depth_valid{};
        bool velocity_valid{};
        bool camera_unchanged{};
        bool reset{};
    };

    struct PreparedTemporalInputs {
        vk::Image color_image{};
        vk::Image depth_image{};
        vk::Image velocity_image{};
        vk::ImageView color{};
        vk::ImageView depth{};
        vk::ImageView velocity{};
        bool native{};
        bool reset{};
    };

    void Create(const Instance& instance, VmaAllocator allocator, u32 num_images);
    vk::ImageView Render(Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                         vk::ImageView color, vk::Extent2D render_size,
                         vk::Extent2D display_size, const TemporalInputs& inputs,
                         float minimum_lerp_contribution = 0.3f);
    PreparedTemporalInputs PrepareForExternalTemporal(
        Scheduler& scheduler, vk::CommandBuffer cmdbuf, vk::ImageView color,
        vk::Extent2D render_size, vk::Extent2D display_size,
        const TemporalInputs& inputs);

    [[nodiscard]] bool HasNativeTemporalInputs() const noexcept {
        return last_native_inputs;
    }

private:
    struct ImageResource {
        ImageResource(vk::Device device, VmaAllocator allocator)
            : image(device, allocator) {}
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
    };

    void RecreateResources(vk::Extent2D render_size, vk::Extent2D display_size);
    std::unique_ptr<ImageResource> CreateImage(vk::Extent2D size, vk::Format format,
                                               vk::ImageUsageFlags extra_usage = {}) const;
    void Bind(Scheduler& scheduler, vk::CommandBuffer cmdbuf,
              vk::PipelineLayout pipeline_layout,
              vk::DescriptorSetLayout descriptor_layout,
              std::span<vk::WriteDescriptorSet> writes);

    vk::Device device{};
    VmaAllocator allocator{};
    u32 num_images{};
    u32 current_image{};
    u32 history_index{};
    bool uses_push_descriptors{};
    bool history_valid{};
    bool sgsr_history_valid{};
    bool last_native_inputs{};
    vk::Extent2D render_extent{};
    vk::Extent2D display_extent{};

    vk::UniqueSampler sampler;
    vk::UniqueDescriptorPool descriptor_pool;
    vk::UniqueDescriptorSetLayout input_layout;
    vk::UniqueDescriptorSetLayout sgsr_layout;
    vk::UniquePipelineLayout input_pipeline_layout;
    vk::UniquePipelineLayout sgsr_pipeline_layout;
    vk::UniquePipeline input_pipeline;
    vk::UniquePipeline convert_pipeline;
    vk::UniquePipeline upscale_pipeline;
    std::vector<vk::DescriptorSet> input_sets;
    std::vector<vk::DescriptorSet> convert_sets;
    std::vector<vk::DescriptorSet> upscale_sets;

    std::array<std::unique_ptr<ImageResource>, 2> low_color_history;
    std::array<std::unique_ptr<ImageResource>, 2> display_history;
    std::unique_ptr<ImageResource> reconstructed_depth;
    std::unique_ptr<ImageResource> reconstructed_velocity;
    std::unique_ptr<ImageResource> reconstructed_raw_velocity;
    std::unique_ptr<ImageResource> motion_depth_clip;
    std::unique_ptr<ImageResource> ycocg_color;
    std::vector<std::unique_ptr<ImageResource>> scene_outputs;
};

} // namespace Vulkan::HostPasses
