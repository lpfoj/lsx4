// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include "common/config.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
#if defined(__ANDROID__)
#include <android/log.h>
#endif
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/texture_cache/sampler.h"

namespace VideoCore {

Sampler::Sampler(const Vulkan::Instance& instance, const AmdGpu::Sampler& sampler,
                 const AmdGpu::BorderColorBuffer border_color_base)
    : Sampler(instance, sampler, border_color_base,
              sampler.depth_compare_func != AmdGpu::DepthCompare::Never) {}

Sampler::Sampler(const Vulkan::Instance& instance, const AmdGpu::Sampler& sampler,
                 const AmdGpu::BorderColorBuffer border_color_base, const bool is_depth) {
    using namespace Vulkan;
    const bool anisotropy_requested = instance.IsAnisotropicFilteringSupported() &&
                                      (AmdGpu::IsAnisoFilter(sampler.xy_mag_filter) ||
                                       AmdGpu::IsAnisoFilter(sampler.xy_min_filter));
    const u32 managed_anisotropy = Config::managedMaxAnisotropy();
    const bool anisotropy_enable =
        anisotropy_requested && (managed_anisotropy == 0 || managed_anisotropy > 1);
    float max_anisotropy =
        anisotropy_enable ? std::clamp(sampler.MaxAniso(), 1.0f, instance.MaxSamplerAnisotropy())
                          : 1.0f;
    if (managed_anisotropy > 1) {
        max_anisotropy = std::min(max_anisotropy, static_cast<float>(managed_anisotropy));
    }
#ifdef __ANDROID__
    if (managed_anisotropy != 0) {
        static std::atomic_bool logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed)) {
            __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                                "[EXECUTOR_SAMPLER_ANISOTROPY] requested=%d cap=%u effective=%.1f",
                                anisotropy_requested ? 1 : 0, managed_anisotropy,
                                max_anisotropy);
        }
    }
#endif
    auto border_color = LiverpoolToVK::BorderColor(sampler.border_color_type);
    if (border_color == vk::BorderColor::eFloatCustomEXT &&
        !instance.IsCustomBorderColorSupported()) {
        LOG_WARNING(Render_Vulkan, "Custom border color is not supported, falling back to black");
        border_color = vk::BorderColor::eFloatOpaqueBlack;
    }

    const auto custom_color = [&]() -> std::optional<vk::SamplerCustomBorderColorCreateInfoEXT> {
        if (border_color == vk::BorderColor::eFloatCustomEXT) {
            const auto border_color_index = sampler.border_color_ptr.Value();
            const auto border_color_buffer = border_color_base.Address<std::array<float, 4>*>();
            const auto custom_border_color_array = border_color_buffer[border_color_index];

            const vk::SamplerCustomBorderColorCreateInfoEXT ret{
                .customBorderColor =
                    vk::ClearColorValue{
                        .float32 = custom_border_color_array,
                    },
                .format = vk::Format::eR32G32B32A32Sfloat,
            };
            return ret;
        } else {
            return std::nullopt;
        }
    }();

    // GCN's MIP_FILTER_NONE disables mip selection; it is not merely nearest mip
    // filtering. Vulkan has no equivalent sampler mode, so constrain the sampler
    // to level zero of the image view (the guest resource's base level) when doing
    // so cannot change within-level filtering. Keep the legacy range for differing
    // min/mag or anisotropic filters because clamping Vulkan LOD to zero would force
    // the magnification path and would not be semantically equivalent there.
    const auto min_filter = sampler.xy_min_filter.Value();
    const auto mag_filter = sampler.xy_mag_filter.Value();
    const bool force_base_level =
        sampler.mip_filter.Value() == AmdGpu::MipFilter::None && min_filter == mag_filter &&
        !AmdGpu::IsAnisoFilter(min_filter) && !AmdGpu::IsAnisoFilter(mag_filter);
    const float effective_min_lod = force_base_level ? 0.0f : sampler.MinLod();
    const float effective_max_lod = force_base_level ? 0.0f : sampler.MaxLod();

    const bool exec_probe = Libraries::GnmDriver::ExecutorReplayActive();
#ifdef __ANDROID__
    if (exec_probe) {
        __android_log_print(ANDROID_LOG_INFO, "EXECUTOR",
                            "[EXECUTOR_SAMPLER] clampX=%u clampY=%u clampZ=%u minF=%u magF=%u mip=%u "
                            "minLod=%.2f maxLod=%.2f border=%u depthCmp=%u",
                            (unsigned)sampler.clamp_x.Value(), (unsigned)sampler.clamp_y.Value(),
                            (unsigned)sampler.clamp_z.Value(), (unsigned)sampler.xy_min_filter.Value(),
                            (unsigned)sampler.xy_mag_filter.Value(), (unsigned)sampler.mip_filter.Value(),
                            sampler.MinLod(), sampler.MaxLod(),
                            (unsigned)sampler.border_color_type.Value(),
                            (unsigned)sampler.depth_compare_func.Value());
    }
#endif
    const vk::SamplerCreateInfo sampler_ci = {
        .pNext = custom_color ? &*custom_color : nullptr,
        .magFilter = LiverpoolToVK::Filter(sampler.xy_mag_filter),
        .minFilter = LiverpoolToVK::Filter(sampler.xy_min_filter),
        .mipmapMode = LiverpoolToVK::MipFilter(sampler.mip_filter),
        .addressModeU = LiverpoolToVK::ClampMode(sampler.clamp_x),
        .addressModeV = LiverpoolToVK::ClampMode(sampler.clamp_y),
        .addressModeW = LiverpoolToVK::ClampMode(sampler.clamp_z),
        .mipLodBias = std::min(sampler.LodBias(), instance.MaxSamplerLodBias()),
        .anisotropyEnable = anisotropy_enable,
        .maxAnisotropy = max_anisotropy,
        .compareEnable = is_depth, // GCN comparison is selected by the instruction.
        .compareOp = LiverpoolToVK::DepthCompare(sampler.depth_compare_func),
        .minLod = effective_min_lod,
        .maxLod = effective_max_lod,
        .borderColor = border_color,
        .unnormalizedCoordinates = false,
    };
    auto [sampler_result, smplr] = instance.GetDevice().createSamplerUnique(sampler_ci);
    ASSERT_MSG(sampler_result == vk::Result::eSuccess, "Failed to create sampler: {}",
               vk::to_string(sampler_result));
    handle = std::move(smplr);
}

Sampler::~Sampler() = default;

}
