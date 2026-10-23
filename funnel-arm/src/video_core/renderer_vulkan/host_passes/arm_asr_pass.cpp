// SPDX-FileCopyrightText: Copyright 2026 LSX4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/host_passes/arm_asr_pass.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <vector>

#ifdef __ANDROID__
#include <dlfcn.h>
#endif

#include <host/backends/vk/ffxm_vk.h>
#include <host/ffxm_fsr2.h>

#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_legacy_sync.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"

#ifdef __ANDROID__
namespace {

void* AndroidVulkanLoaderHandle() {
    static void* handle = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    return handle;
}

template <typename Function>
Function ResolveAndroidVulkanFunction(const char* core_name,
                                      const char* extension_name) {
    void* const handle = AndroidVulkanLoaderHandle();
    if (handle == nullptr) {
        return nullptr;
    }
    void* address = dlsym(handle, core_name);
    if (address == nullptr) {
        address = dlsym(handle, extension_name);
    }
    return reinterpret_cast<Function>(address);
}

} // namespace

// Android's API-26 NDK link stub exposes Vulkan 1.0 only. The device loader on
// newer Vulkan devices still provides these Vulkan 1.1/KHR-identical entry
// points. Arm ASR's official generic backend calls the core names directly, so
// resolve them at runtime while retaining a conservative Vulkan 1.0 fallback.
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(
    VkPhysicalDevice physical_device, VkPhysicalDeviceProperties2* properties) {
    using Function = PFN_vkGetPhysicalDeviceProperties2;
    static const Function function = ResolveAndroidVulkanFunction<Function>(
        "vkGetPhysicalDeviceProperties2", "vkGetPhysicalDeviceProperties2KHR");
    if (function != nullptr) {
        function(physical_device, properties);
        return;
    }
    using LegacyFunction = PFN_vkGetPhysicalDeviceProperties;
    static const LegacyFunction legacy = ResolveAndroidVulkanFunction<LegacyFunction>(
        "vkGetPhysicalDeviceProperties", "vkGetPhysicalDeviceProperties");
    if (legacy != nullptr) {
        legacy(physical_device, &properties->properties);
    }
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physical_device, VkPhysicalDeviceFeatures2* features) {
    using Function = PFN_vkGetPhysicalDeviceFeatures2;
    static const Function function = ResolveAndroidVulkanFunction<Function>(
        "vkGetPhysicalDeviceFeatures2", "vkGetPhysicalDeviceFeatures2KHR");
    if (function != nullptr) {
        function(physical_device, features);
        return;
    }
    using LegacyFunction = PFN_vkGetPhysicalDeviceFeatures;
    static const LegacyFunction legacy = ResolveAndroidVulkanFunction<LegacyFunction>(
        "vkGetPhysicalDeviceFeatures", "vkGetPhysicalDeviceFeatures");
    if (legacy != nullptr) {
        legacy(physical_device, &features->features);
    }
}
#endif

namespace Vulkan::HostPasses {
namespace {

constexpr vk::ImageSubresourceRange ColorRange{
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .levelCount = 1,
    .layerCount = 1,
};

arm::FfxmResourceDescription ImageDescription(arm::FfxmSurfaceFormat format,
                                               vk::Extent2D size,
                                               arm::FfxmResourceUsage usage) {
    return {
        .type = arm::FFXM_RESOURCE_TYPE_TEXTURE2D,
        .format = format,
        .width = size.width,
        .height = size.height,
        .depth = 1,
        .mipCount = 1,
        .flags = arm::FFXM_RESOURCE_FLAGS_NONE,
        .usage = usage,
    };
}

arm::FfxmFsr2ShaderQualityMode QualityForScale(vk::Extent2D render,
                                                vk::Extent2D display) {
    const float ratio = std::max(static_cast<float>(display.width) / render.width,
                                 static_cast<float>(display.height) / render.height);
    if (ratio <= 1.5f) {
        return arm::FFXM_FSR2_SHADER_QUALITY_MODE_QUALITY;
    }
    if (ratio <= 1.7f) {
        return arm::FFXM_FSR2_SHADER_QUALITY_MODE_BALANCED;
    }
    if (ratio <= 2.0f) {
        return arm::FFXM_FSR2_SHADER_QUALITY_MODE_PERFORMANCE;
    }
    return arm::FFXM_FSR2_SHADER_QUALITY_MODE_ULTRA_PERFORMANCE;
}

} // namespace

struct ArmAsrPass::Impl {
    struct OutputImage {
        OutputImage(vk::Device device, VmaAllocator allocator)
            : image(device, allocator) {}
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
    };

    vk::Device device{};
    vk::PhysicalDevice physical_device{};
    VmaAllocator allocator{};
    u32 num_images{};
    u32 current_image{};
    bool supported{};
    bool context_created{};
    bool dispatch_succeeded{};
    vk::Extent2D render_extent{};
    vk::Extent2D display_extent{};
    std::vector<std::byte> scratch;
    arm::FfxmFsr2Context context{};
    arm::FfxmFsr2ContextDescription description{};
    std::vector<std::unique_ptr<OutputImage>> outputs;
    std::vector<bool> output_valid;

    ~Impl() {
        DestroyContext();
    }

    void DestroyContext() {
        if (context_created) {
            const auto result = arm::ffxmFsr2ContextDestroy(&context);
            if (result != arm::FFXM_OK) {
                LOG_WARNING(Render_Vulkan, "Arm ASR context destroy failed: {}",
                            static_cast<int>(result));
            }
            context_created = false;
            context = {};
        }
        outputs.clear();
        output_valid.clear();
        render_extent = {};
        display_extent = {};
    }

    std::unique_ptr<OutputImage> CreateOutput(vk::Extent2D size) const {
        auto output = std::make_unique<OutputImage>(device, allocator);
        output->image.Create({
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .extent = {.width = size.width, .height = size.height, .depth = 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = vk::ImageUsageFlagBits::eSampled |
                     vk::ImageUsageFlagBits::eStorage |
                     vk::ImageUsageFlagBits::eTransferSrc |
                     vk::ImageUsageFlagBits::eTransferDst,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        output->view = Check<"create Arm ASR output view">(
            device.createImageViewUnique({
                .image = output->image,
                .viewType = vk::ImageViewType::e2D,
                .format = vk::Format::eR16G16B16A16Sfloat,
                .subresourceRange = ColorRange,
            }));
        return output;
    }

    bool RecreateContext(vk::Extent2D render_size, vk::Extent2D display_size,
                         bool hdr) {
        DestroyContext();
        description = {};
        description.qualityMode = QualityForScale(render_size, display_size);
        description.flags = arm::FFXM_FSR2_ENABLE_DYNAMIC_RESOLUTION;
        if (hdr) {
            description.flags |= arm::FFXM_FSR2_ENABLE_HIGH_DYNAMIC_RANGE;
        }
        // Allocate against the presentation extent so adaptive internal DRS
        // can change renderSize every frame without destroying temporal state.
        description.maxRenderSize = {display_size.width, display_size.height};
        description.displaySize = {display_size.width, display_size.height};

        arm::VkDeviceContext device_context{
            .vkDevice = static_cast<VkDevice>(device),
            .vkPhysicalDevice = static_cast<VkPhysicalDevice>(physical_device),
            .vkDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr,
        };
        const arm::FfxmDevice ffx_device = arm::ffxmGetDeviceVK(&device_context);
        const auto interface_result = arm::ffxmGetInterfaceVK(
            &description.backendInterface, ffx_device, scratch.data(), scratch.size(), 1);
        if (interface_result != arm::FFXM_OK) {
            LOG_ERROR(Render_Vulkan, "Arm ASR Vulkan interface failed: {}",
                      static_cast<int>(interface_result));
            return false;
        }
        const auto create_result = arm::ffxmFsr2ContextCreate(&context, &description);
        if (create_result != arm::FFXM_OK) {
            LOG_ERROR(Render_Vulkan, "Arm ASR context create failed: {}",
                      static_cast<int>(create_result));
            return false;
        }
        context_created = true;
        render_extent = display_size;
        display_extent = display_size;
        outputs.reserve(num_images);
        for (u32 i = 0; i < num_images; ++i) {
            outputs.push_back(CreateOutput(display_size));
        }
        output_valid.assign(num_images, false);
        current_image = 0;
        return true;
    }
};

ArmAsrPass::ArmAsrPass() : impl(std::make_unique<Impl>()) {}
ArmAsrPass::~ArmAsrPass() = default;

void ArmAsrPass::Create(const Instance& instance, VmaAllocator allocator,
                        u32 num_images) {
    impl->device = instance.GetDevice();
    impl->physical_device = instance.GetPhysicalDevice();
    impl->allocator = allocator;
    impl->num_images = num_images;
    impl->supported = instance.GetCapabilityProfile().shader_float16;
    if (!impl->supported) {
        LOG_WARNING(Render_Vulkan,
                    "Arm ASR disabled: official Vulkan permutations require shaderFloat16");
        return;
    }
    const size_t scratch_size = arm::ffxmGetScratchMemorySizeVK(
        static_cast<VkPhysicalDevice>(impl->physical_device), 1);
    impl->scratch.resize(scratch_size);
}

void ArmAsrPass::Destroy() {
    impl->DestroyContext();
    impl->scratch.clear();
}

bool ArmAsrPass::IsSupported() const noexcept {
    return impl->supported;
}

bool ArmAsrPass::LastDispatchSucceeded() const noexcept {
    return impl->dispatch_succeeded;
}

vk::ImageView ArmAsrPass::Render(
    Scheduler& scheduler, vk::CommandBuffer cmdbuf, vk::Extent2D render_size,
    vk::Extent2D display_size,
    const Sgsr2Pass::PreparedTemporalInputs& inputs, float jitter_x,
    float jitter_y, bool hdr, bool enable_sharpening, float sharpness) {
    impl->dispatch_succeeded = false;
    if (!impl->supported || !inputs.color_image || !inputs.depth_image ||
        !inputs.velocity_image) {
        return {};
    }
    if (!impl->context_created || render_size.width > impl->render_extent.width ||
        render_size.height > impl->render_extent.height ||
        display_size != impl->display_extent ||
        (hdr != ((impl->description.flags &
                  arm::FFXM_FSR2_ENABLE_HIGH_DYNAMIC_RANGE) != 0))) {
        if (!impl->RecreateContext(render_size, display_size, hdr)) {
            return {};
        }
    }

    const u32 frame_id = impl->current_image++;
    if (impl->current_image >= impl->num_images) {
        impl->current_image = 0;
    }
    auto& output = *impl->outputs[frame_id];
    const bool reused_output = impl->output_valid[frame_id];
    const vk::ImageMemoryBarrier2 output_writable{
        .srcStageMask = reused_output ? vk::PipelineStageFlagBits2::eFragmentShader
                                      : vk::PipelineStageFlagBits2::eTopOfPipe,
        .srcAccessMask = reused_output ? vk::AccessFlagBits2::eShaderRead
                                       : vk::AccessFlags2{},
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .oldLayout = reused_output ? vk::ImageLayout::eShaderReadOnlyOptimal
                                   : vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = output.image,
        .subresourceRange = ColorRange,
    };
    PipelineBarrier(scheduler, cmdbuf,
                    {.imageMemoryBarrierCount = 1,
                     .pImageMemoryBarriers = &output_writable});

    const auto color_desc = ImageDescription(
        arm::FFXM_SURFACE_FORMAT_R16G16B16A16_FLOAT, render_size,
        arm::FFXM_RESOURCE_USAGE_READ_ONLY);
    const auto depth_desc = ImageDescription(
        arm::FFXM_SURFACE_FORMAT_R32_FLOAT, render_size,
        arm::FFXM_RESOURCE_USAGE_READ_ONLY);
    const auto velocity_desc = ImageDescription(
        arm::FFXM_SURFACE_FORMAT_R16G16_FLOAT, render_size,
        arm::FFXM_RESOURCE_USAGE_READ_ONLY);
    const auto output_desc = ImageDescription(
        arm::FFXM_SURFACE_FORMAT_R16G16B16A16_FLOAT, display_size,
        arm::FFXM_RESOURCE_USAGE_UAV);

    arm::FfxmFsr2DispatchDescription dispatch{};
    dispatch.commandList = arm::ffxmGetCommandListVK(static_cast<VkCommandBuffer>(cmdbuf));
    dispatch.color = arm::ffxmGetResourceVK(
        reinterpret_cast<void*>(static_cast<VkImage>(inputs.color_image)),
        color_desc, nullptr, arm::FFXM_RESOURCE_STATE_COMPUTE_READ);
    dispatch.depth = arm::ffxmGetResourceVK(
        reinterpret_cast<void*>(static_cast<VkImage>(inputs.depth_image)),
        depth_desc, nullptr, arm::FFXM_RESOURCE_STATE_COMPUTE_READ);
    dispatch.motionVectors = arm::ffxmGetResourceVK(
        reinterpret_cast<void*>(static_cast<VkImage>(inputs.velocity_image)),
        velocity_desc, nullptr, arm::FFXM_RESOURCE_STATE_COMPUTE_READ);
    dispatch.output = arm::ffxmGetResourceVK(
        reinterpret_cast<void*>(
            static_cast<VkImage>(static_cast<vk::Image>(output.image))),
        output_desc, nullptr, arm::FFXM_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.jitterOffset = {jitter_x, jitter_y};
    dispatch.motionVectorScale = {static_cast<float>(render_size.width),
                                  static_cast<float>(render_size.height)};
    dispatch.renderSize = {render_size.width, render_size.height};
    dispatch.enableSharpening = enable_sharpening;
    dispatch.sharpness = std::clamp(sharpness, 0.0f, 1.0f);
    dispatch.frameTimeDelta = 16.6667f;
    dispatch.preExposure = 1.f;
    dispatch.reset = inputs.reset;
    dispatch.cameraNear = 0.1f;
    dispatch.cameraFar = 1000.f;
    dispatch.cameraFovAngleVertical = 1.04719755f;
    dispatch.viewSpaceToMetersFactor = 1.f;

    const auto result = arm::ffxmFsr2ContextDispatch(&impl->context, &dispatch);
    scheduler.MarkPipelineBind(vk::PipelineBindPoint::eCompute);
    scheduler.MarkDescriptorBind(vk::PipelineBindPoint::eCompute);
    scheduler.MarkPushConstants(vk::PipelineBindPoint::eCompute);
    scheduler.GetDynamicState().Invalidate();
    if (result != arm::FFXM_OK) {
        LOG_ERROR(Render_Vulkan, "Arm ASR dispatch failed: {}", static_cast<int>(result));
        return {};
    }

    const vk::ImageMemoryBarrier2 output_readable{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = output.image,
        .subresourceRange = ColorRange,
    };
    PipelineBarrier(scheduler, cmdbuf,
                    {.imageMemoryBarrierCount = 1,
                     .pImageMemoryBarriers = &output_readable});
    impl->dispatch_succeeded = true;
    impl->output_valid[frame_id] = true;
    return output.view.get();
}

} // namespace Vulkan::HostPasses
