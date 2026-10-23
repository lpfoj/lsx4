//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "common/config.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {
class Instance;
class Scheduler;
}

namespace Vulkan::HostPasses {

class FsrPass {
public:
    struct Settings {
        bool enable{true};
        Config::UpscalerMode mode{Config::UpscalerMode::Auto};
        bool use_rcas{true};
        float rcas_attenuation{0.25f};
        float sharp_bilinear_strength{1.0f};
        u32 bicubic_kernel{0};
        float sgsr1_edge_sharpness{2.0f};
        float nis_sharpness{0.5f};
    };

    void Create(const Instance& instance, VmaAllocator allocator, u32 num_images);

    vk::ImageView Render(Scheduler& scheduler, vk::CommandBuffer cmdbuf, vk::ImageView input,
                         vk::Extent2D input_size, vk::Extent2D output_size, Settings settings,
                         bool hdr);

    [[nodiscard]] Config::UpscalerMode LastEffectiveMode() const noexcept {
        return last_effective_mode;
    }
    [[nodiscard]] u32 LastFallbackReason() const noexcept {
        return last_fallback_reason;
    }

private:
    struct Img {
        u8 id{};
        bool dirty{true};
        bool intermediary_valid{};
        bool output_valid{};

        VideoCore::UniqueImage intermediary_image;
        vk::UniqueImageView intermediary_image_view;

        VideoCore::UniqueImage output_image;
        vk::UniqueImageView output_image_view;
    };

    void ResizeAndInvalidate(u32 width, u32 height);
    void CreateImages(Img& img) const;
    void UploadNisCoefficients(Scheduler& scheduler, vk::CommandBuffer cmdbuf);
    void BindDescriptors(vk::CommandBuffer cmdbuf, std::span<vk::WriteDescriptorSet> writes,
                         bool rcas, u8 image_id);

    vk::Device device{};
    u32 num_images{};
    bool uses_push_descriptors{};

    vk::UniqueDescriptorSetLayout descriptor_set_layout{};
    vk::UniqueDescriptorPool descriptor_pool{};
    std::vector<vk::DescriptorSet> easu_descriptor_sets;
    std::vector<vk::DescriptorSet> rcas_descriptor_sets;
    vk::UniqueSampler sampler{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniquePipeline easu_pipeline{};
    vk::UniquePipeline rcas_pipeline{};
    vk::UniquePipeline spatial_pipeline{};
    vk::UniquePipeline nis_pipeline{};

    std::unique_ptr<VideoCore::UniqueImage> nis_scale_coefficients;
    std::unique_ptr<VideoCore::UniqueImage> nis_usm_coefficients;
    vk::UniqueImageView nis_scale_coefficients_view{};
    vk::UniqueImageView nis_usm_coefficients_view{};
    std::unique_ptr<VideoCore::UniqueBuffer> nis_coefficient_upload;
    std::vector<VideoCore::UniqueBuffer> nis_uniform_buffers;
    std::vector<void*> nis_uniform_mapped;
    bool nis_coefficients_uploaded{};

    vk::Extent2D cur_size{};
    u32 cur_image{};
    std::vector<Img> available_imgs;
    Config::UpscalerMode last_effective_mode{Config::UpscalerMode::Off};
    u32 last_fallback_reason{};
};

}
