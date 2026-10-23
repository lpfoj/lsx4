// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <ranges>
#include <atomic>
#include <cstdlib>
#include "common/assert.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/host_compatibility.h"
#include "video_core/texture_cache/image.h"

#include <boost/container/small_vector.hpp>
#include <vk_mem_alloc.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace VideoCore {

using namespace Vulkan;

Common::IncrementalIdProvider<u64> Image::global_image_uid{};

static vk::ImageUsageFlags UniversalImageUsageFlags(
    const Vulkan::Instance* instance, const ImageInfo& info) {
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransferSrc |
                                vk::ImageUsageFlagBits::eTransferDst |
                                vk::ImageUsageFlagBits::eSampled;
    if (!info.props.is_block) {
        if (info.props.is_depth) {
            usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
        } else {
            usage |= vk::ImageUsageFlagBits::eColorAttachment;
            if (instance->IsAttachmentFeedbackLoopLayoutSupported()) {
                usage |= vk::ImageUsageFlagBits::eAttachmentFeedbackLoopEXT;
            }
            usage |= vk::ImageUsageFlagBits::eStorage;
        }
    } else {
        usage |= vk::ImageUsageFlagBits::eStorage;
    }

    return usage;
}

static vk::ImageUsageFlags RoleScopedImageUsageFlags(
    const Vulkan::Instance* instance, const ImageInfo& info,
    const ImageBackingRole role) {
    if (role == ImageBackingRole::Universal) {
        return UniversalImageUsageFlags(instance, info);
    }

    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransferSrc |
                                vk::ImageUsageFlagBits::eTransferDst;
    switch (role) {
    case ImageBackingRole::Texture:
    case ImageBackingRole::VideoOut:
        usage |= vk::ImageUsageFlagBits::eSampled;
        break;
    case ImageBackingRole::Storage:
        usage |= vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eStorage;
        break;
    case ImageBackingRole::RenderTarget:
        usage |= vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eColorAttachment;
        if (instance->IsAttachmentFeedbackLoopLayoutSupported()) {
            usage |= vk::ImageUsageFlagBits::eAttachmentFeedbackLoopEXT;
        }
        break;
    case ImageBackingRole::DepthTarget:
        usage |= vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eDepthStencilAttachment;
        break;
    case ImageBackingRole::Universal:
        UNREACHABLE();
    }
    return usage;
}

static vk::ImageUsageFlags ImageUsageFlags(const Vulkan::Instance* instance,
                                           const ImageInfo& info,
                                           const ImageBackingRole role) {
    if (!instance->GetCapabilityProfile().optimization_a750_role_image_usage) {
        return UniversalImageUsageFlags(instance, info);
    }
    return RoleScopedImageUsageFlags(instance, info, role);
}

static vk::ImageType ConvertImageType(AmdGpu::ImageType type) noexcept {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color1DArray:
        return vk::ImageType::e1D;
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
    case AmdGpu::ImageType::Color2DArray:
        return vk::ImageType::e2D;
    case AmdGpu::ImageType::Color3D:
        return vk::ImageType::e3D;
    default:
        UNREACHABLE();
    }
}

static vk::FormatFeatureFlags2 FormatFeatureFlags(const vk::ImageUsageFlags usage_flags) {
    vk::FormatFeatureFlags2 feature_flags{};
    if (usage_flags & vk::ImageUsageFlagBits::eTransferSrc) {
        feature_flags |= vk::FormatFeatureFlagBits2::eTransferSrc;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eTransferDst) {
        feature_flags |= vk::FormatFeatureFlagBits2::eTransferDst;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eSampled) {
        feature_flags |= vk::FormatFeatureFlagBits2::eSampledImage;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eColorAttachment) {
        feature_flags |= vk::FormatFeatureFlagBits2::eColorAttachment;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eDepthStencilAttachment) {
        feature_flags |= vk::FormatFeatureFlagBits2::eDepthStencilAttachment;
    }
    return feature_flags;
}

static vk::FormatFeatureFlags2 FullImageUsageFormatFeatures(
    const vk::ImageUsageFlags usage_flags) {
    vk::FormatFeatureFlags2 feature_flags = FormatFeatureFlags(usage_flags);
    if (usage_flags & vk::ImageUsageFlagBits::eStorage) {
        feature_flags |= vk::FormatFeatureFlagBits2::eStorageImage;
    }
    return feature_flags;
}

UniqueImage::~UniqueImage() {
    if (image) {
        vmaDestroyImage(allocator, image, allocation);
    }
}

void UniqueImage::Create(const vk::ImageCreateInfo& image_ci) {
    const vk::Result result = TryCreate(image_ci);
    ASSERT_MSG(result == vk::Result::eSuccess, "Failed allocating image with error {}",
               vk::to_string(result));
}

using BoundedImageFormatList = boost::container::small_vector<vk::Format, 32>;

static BoundedImageFormatList BoundedImageViewFormats(
    const Vulkan::Instance* instance, const vk::Format guest_base_format,
    const vk::Format host_base_format, const vk::ImageCreateFlags flags,
    const vk::FormatFeatureFlags2 format_features) {
    if (!instance->GetCapabilityProfile().optimization_a750_bounded_image_format_list ||
        !(flags & vk::ImageCreateFlagBits::eMutableFormat)) {
        return {};
    }

    BoundedImageFormatList formats;
    const auto append_guest_format = [&](const vk::Format guest_view_format) {
        if (guest_view_format == vk::Format::eUndefined ||
            !IsVulkanFormatCompatible(guest_base_format, guest_view_format)) {
            return;
        }
        // ImageView resolves through this exact device mapping. Keeping only
        // mapped guest formats is exhaustive for LSX4 while avoiding Vulkan
        // formats the Liverpool frontend can never request.
        const vk::Format host_view_format =
            instance->GetSupportedFormat(guest_view_format, format_features);
        if (!IsVulkanFormatCompatible(host_base_format, host_view_format) ||
            std::ranges::find(formats, host_view_format) != formats.end()) {
            return;
        }
        formats.push_back(host_view_format);
    };

    append_guest_format(guest_base_format);
    for (const auto& format : LiverpoolToVK::SurfaceFormats()) {
        append_guest_format(format.vk_format);
    }
    for (const auto& format : LiverpoolToVK::DepthFormats()) {
        append_guest_format(format.vk_format);
    }
    std::ranges::sort(formats, {}, [](const vk::Format format) {
        return static_cast<u32>(format);
    });
    return formats;
}

static void CreateTextureCacheImage(UniqueImage& image, vk::ImageCreateInfo image_ci,
                                    const std::span<const vk::Format> view_formats) {
    const vk::ImageFormatListCreateInfo format_list{
        .viewFormatCount = static_cast<u32>(view_formats.size()),
        .pViewFormats = view_formats.data(),
    };
    if (!view_formats.empty()) {
        image_ci.pNext = &format_list;
    }
    image.Create(image_ci);
    // pNext above is synchronous create-call storage. Backing recreation must
    // rebuild it from the immutable compatibility table, never retain it.
    image.image_ci.pNext = nullptr;
}

vk::Result UniqueImage::TryCreate(const vk::ImageCreateInfo& image_ci) noexcept {
    this->image_ci = image_ci;
    if (image || allocator == VK_NULL_HANDLE) {
        return vk::Result::eErrorInitializationFailed;
    }
    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    const VkImageCreateInfo image_ci_unsafe = static_cast<VkImageCreateInfo>(image_ci);
    VkImage unsafe_image{};
    VmaAllocation unsafe_allocation{};
    VmaAllocationInfo allocation_info{};
    const VkResult result =
        vmaCreateImage(allocator, &image_ci_unsafe, &alloc_info, &unsafe_image,
                       &unsafe_allocation, &allocation_info);
    if (result != VK_SUCCESS) {
        return vk::Result{result};
    }
    image = vk::Image{unsafe_image};
    allocation = unsafe_allocation;
    allocation_size = allocation_info.size;
    return vk::Result::eSuccess;
}

Image::Image(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
             BlitHelper& blit_helper_, Common::SlotVector<ImageView>& slot_image_views_,
             const ImageInfo& info_, const ImageBackingRole initial_role)
    : instance{&instance_}, scheduler{&scheduler_}, blit_helper{&blit_helper_},
      slot_image_views{&slot_image_views_}, info{info_} {
    if (info.pixel_format == vk::Format::eUndefined) {
        return;
    }
    image_uid = global_image_uid.Next();
    mip_hashes.resize(info.resources.levels);
    vk::ImageCreateFlags flags{vk::ImageCreateFlagBits::eMutableFormat |
                               vk::ImageCreateFlagBits::eExtendedUsage};
    if (info.props.is_volume) {
        flags |= vk::ImageCreateFlagBits::e2DArrayCompatible;
        if (instance->Is2dViewOf3dSupported()) {
            flags |= vk::ImageCreateFlagBits::e2DViewCompatibleEXT;
        }
    }
    if (info.props.is_block && instance->IsBlockTexelViewSupported()) {
        flags |= vk::ImageCreateFlagBits::eBlockTexelViewCompatible;
    }

    role_scoped_usage =
        instance->GetCapabilityProfile().optimization_a750_role_image_usage;
    // Universal color usage includes Storage.  Some mobile formats consequently advertise only
    // one sample even though the exact render-target usage supports the guest MSAA count.  Probe
    // both final usage domains and narrow only when that difference is proven for this image.
    // Limit the generic promotion to the single-subresource topology already supported by the
    // existing sample-copy path; profile-routed role usage retains its established behavior.
    if (!role_scoped_usage && initial_role == ImageBackingRole::RenderTarget &&
        info.num_samples > 1 && info.resources.levels == 1 && info.resources.layers == 1) {
        constexpr auto probe_tiling = vk::ImageTiling::eOptimal;
        const auto probe_samples = [&](const vk::ImageUsageFlags probe_usage,
                                       const vk::FormatFeatureFlags2 probe_features) {
            vk::ImageCreateFlags probe_flags = flags;
            const auto probe_format =
                instance->GetSupportedFormat(info.pixel_format, probe_features);
            if (instance->GetCapabilityProfile().optimization_a750_narrow_image_flags &&
                instance->IsOptimalImageFormatSupported(
                    probe_format, FullImageUsageFormatFeatures(probe_usage))) {
                probe_flags &= ~vk::ImageCreateFlagBits::eExtendedUsage;
            }
            const auto probe_view_formats =
                BoundedImageViewFormats(instance, info.pixel_format, probe_format,
                                        probe_flags, probe_features);
            const vk::ImageFormatListCreateInfo probe_format_list{
                .viewFormatCount = static_cast<u32>(probe_view_formats.size()),
                .pViewFormats = probe_view_formats.data(),
            };
            const vk::PhysicalDeviceImageFormatInfo2 probe_info{
                .pNext = probe_view_formats.empty() ? nullptr : &probe_format_list,
                .format = probe_format,
                .type = ConvertImageType(info.type),
                .tiling = probe_tiling,
                .usage = probe_usage,
                .flags = probe_flags,
            };
            const auto properties =
                instance->GetPhysicalDevice().getImageFormatProperties2(probe_info);
            return properties.result == vk::Result::eSuccess
                       ? properties.value.imageFormatProperties.sampleCounts
                       : vk::SampleCountFlags{vk::SampleCountFlagBits::e1};
        };
        const vk::ImageUsageFlags universal_usage =
            UniversalImageUsageFlags(instance, info);
        const vk::FormatFeatureFlags2 universal_features =
            FormatFeatureFlags(universal_usage);
        const vk::ImageUsageFlags render_target_usage =
            RoleScopedImageUsageFlags(instance, info, initial_role);
        const vk::FormatFeatureFlags2 render_target_features =
            FullImageUsageFormatFeatures(render_target_usage);
        const auto requested_samples =
            static_cast<vk::SampleCountFlagBits>(info.num_samples);
        const auto universal_samples =
            probe_samples(universal_usage, universal_features);
        const auto render_target_samples =
            probe_samples(render_target_usage, render_target_features);
        role_scoped_usage = !(universal_samples & requested_samples) &&
                            static_cast<bool>(render_target_samples & requested_samples);
    }
    usage_flags = role_scoped_usage
                      ? RoleScopedImageUsageFlags(instance, info, initial_role)
                      : ImageUsageFlags(instance, info, initial_role);
    format_features = role_scoped_usage ? FullImageUsageFormatFeatures(usage_flags)
                                        : FormatFeatureFlags(usage_flags);
    if (info.props.is_depth) {
        aspect_mask = vk::ImageAspectFlagBits::eDepth;
        if (info.props.has_stencil) {
            aspect_mask |= vk::ImageAspectFlagBits::eStencil;
        }
    }

    constexpr auto tiling = vk::ImageTiling::eOptimal;
    const auto supported_format = instance->GetSupportedFormat(info.pixel_format, format_features);
    if (instance->GetCapabilityProfile().optimization_a750_narrow_image_flags &&
        instance->IsOptimalImageFormatSupported(
            supported_format, FullImageUsageFormatFeatures(usage_flags))) {
        // Per Vulkan's image-creation rules EXTENDED_USAGE only relaxes the
        // requirement that the base format itself support every usage bit.
        // Keep MUTABLE_FORMAT for guest aliases, but avoid forcing Turnip to
        // retain the broader per-view usage domain when the base format has
        // already proven the complete optimal-tiling feature set.
        flags &= ~vk::ImageCreateFlagBits::eExtendedUsage;
    }
    const auto bounded_view_formats =
        BoundedImageViewFormats(instance, info.pixel_format, supported_format,
                                flags, format_features);
    const vk::ImageFormatListCreateInfo format_list{
        .viewFormatCount = static_cast<u32>(bounded_view_formats.size()),
        .pViewFormats = bounded_view_formats.data(),
    };
    const vk::PhysicalDeviceImageFormatInfo2 format_info{
        .pNext = bounded_view_formats.empty() ? nullptr : &format_list,
        .format = supported_format,
        .type = ConvertImageType(info.type),
        .tiling = tiling,
        .usage = usage_flags,
        .flags = flags,
    };
    const auto image_format_properties =
        instance->GetPhysicalDevice().getImageFormatProperties2(format_info);
    if (image_format_properties.result == vk::Result::eErrorFormatNotSupported) {
        LOG_ERROR(Render_Vulkan, "image format {} type {} is not supported (flags {}, usage {})",
                  vk::to_string(supported_format), vk::to_string(format_info.type),
                  vk::to_string(format_info.flags), vk::to_string(format_info.usage));
    }
    supported_samples = image_format_properties.result == vk::Result::eSuccess
                            ? image_format_properties.value.imageFormatProperties.sampleCounts
                            : vk::SampleCountFlagBits::e1;

    const vk::SampleCountFlagBits host_samples =
        LiverpoolToVK::NumSamples(info.num_samples, supported_samples);
#ifdef __ANDROID__
    if (std::getenv("EXECUTOR_TRACE_VIDEOOUT_SOURCE") != nullptr && info.num_samples > 1) {
        static std::atomic<u32> multisample_create_logs{};
        const u32 ordinal = multisample_create_logs.fetch_add(1, std::memory_order_relaxed);
        if (ordinal < 24) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_MULTISAMPLE_BACKING] phase=create ordinal=%u role=%u "
                "requested=%u supported=0x%x host=%u usage=0x%x roleScoped=%u format=%s "
                "extent=%ux%u address=0x%llx",
                ordinal + 1, static_cast<u32>(initial_role), info.num_samples,
                static_cast<u32>(supported_samples), static_cast<u32>(host_samples),
                static_cast<u32>(usage_flags), role_scoped_usage ? 1u : 0u,
                vk::to_string(supported_format).c_str(), info.size.width, info.size.height,
                static_cast<unsigned long long>(info.guest_address));
        }
    }
#endif
    const vk::ImageCreateInfo image_ci = {
        .pNext = bounded_view_formats.empty() ? nullptr : &format_list,
        .flags = flags,
        .imageType = ConvertImageType(info.type),
        .format = supported_format,
        .extent{
            .width = info.size.width,
            .height = info.size.height,
            .depth = info.size.depth,
        },
        .mipLevels = static_cast<u32>(info.resources.levels),
        .arrayLayers = static_cast<u32>(info.resources.layers),
        .samples = host_samples,
        .tiling = tiling,
        .usage = usage_flags,
        .initialLayout = vk::ImageLayout::eUndefined,
    };

    backing = &backing_images.emplace_back();
    backing->num_samples = static_cast<u32>(image_ci.samples);
    backing->image = UniqueImage{instance->GetDevice(), instance->GetAllocator()};
    CreateTextureCacheImage(backing->image, image_ci, bounded_view_formats);
#ifdef __ANDROID__
    if (instance->GetCapabilityProfile().diagnostic_a750_image_topology) {
        const bool color_storage =
            static_cast<bool>(image_ci.usage & vk::ImageUsageFlagBits::eColorAttachment) &&
            static_cast<bool>(image_ci.usage & vk::ImageUsageFlagBits::eStorage);
        RecordRendererImageCreationTopology(
            static_cast<bool>(image_ci.flags & vk::ImageCreateFlagBits::eMutableFormat),
            static_cast<bool>(image_ci.flags & vk::ImageCreateFlagBits::eExtendedUsage),
            color_storage, info.props.is_depth, info.props.is_block,
            backing->image.allocation_size,
            static_cast<u32>(bounded_view_formats.size()));
    }
#endif

    Vulkan::SetObjectName(instance->GetDevice(), GetImage(),
                          "Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{}", info.size.width,
                          info.size.height, info.size.depth, AmdGpu::NameOf(info.tile_mode),
                          vk::to_string(info.pixel_format), info.guest_address, info.guest_size,
                          info.resources.layers, info.resources.levels, info.num_samples);
}

Image::~Image() = default;

ImageView& Image::FindView(const ImageViewInfo& view_info, bool ensure_guest_samples) {
    if (ensure_guest_samples && backing->num_samples > 1 != info.num_samples > 1) {
        SetBackingSamples(info.num_samples);
    }
    const auto& view_infos = backing->image_view_infos;
    const auto it = std::ranges::find(view_infos, view_info);
    if (it != view_infos.end()) {
        const auto view_id = backing->image_view_ids[std::distance(view_infos.begin(), it)];
        return (*slot_image_views)[view_id];
    }
    const auto view_id = slot_image_views->insert(*instance, view_info, *this);
    backing->image_view_infos.emplace_back(view_info);
    backing->image_view_ids.emplace_back(view_id);
    return (*slot_image_views)[view_id];
}

Image::Barriers Image::GetBarriers(vk::ImageLayout dst_layout, vk::AccessFlags2 dst_mask,
                                   vk::PipelineStageFlags2 dst_stage,
                                   std::optional<SubresourceRange> subres_range) {
    auto& last_state = backing->state;
    auto& subresource_states = backing->subresource_states;

    const bool needs_partial_transition =
        subres_range &&
        (subres_range->base != SubresourceBase{} || subres_range->extent != info.resources);
    const bool partially_transited = !subresource_states.empty();

    Barriers barriers;
    const bool split_depth_stencil_barriers =
        instance->IsSeparateDepthStencilLayoutsSupported() &&
        instance->GetCapabilityProfile().HasQuirk(
            CapabilityQuirk::SplitDepthStencilImageBarriers);
    const auto aspect_layout = [](vk::ImageLayout layout,
                                  vk::ImageAspectFlagBits aspect) {
        const bool depth = aspect == vk::ImageAspectFlagBits::eDepth;
        switch (layout) {
        case vk::ImageLayout::eDepthStencilAttachmentOptimal:
            return depth ? vk::ImageLayout::eDepthAttachmentOptimal
                         : vk::ImageLayout::eStencilAttachmentOptimal;
        case vk::ImageLayout::eDepthStencilReadOnlyOptimal:
            return depth ? vk::ImageLayout::eDepthReadOnlyOptimal
                         : vk::ImageLayout::eStencilReadOnlyOptimal;
        case vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal:
            return depth ? vk::ImageLayout::eDepthReadOnlyOptimal
                         : vk::ImageLayout::eStencilAttachmentOptimal;
        case vk::ImageLayout::eDepthAttachmentStencilReadOnlyOptimal:
            return depth ? vk::ImageLayout::eDepthAttachmentOptimal
                         : vk::ImageLayout::eStencilReadOnlyOptimal;
        default:
            return layout;
        }
    };
    const auto append_image_barrier = [&](vk::ImageMemoryBarrier2 barrier) {
        constexpr vk::ImageAspectFlags depth_stencil =
            vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
        if (!split_depth_stencil_barriers ||
            (barrier.subresourceRange.aspectMask & depth_stencil) != depth_stencil) {
            barriers.emplace_back(barrier);
            return;
        }

        auto depth_barrier = barrier;
        depth_barrier.oldLayout =
            aspect_layout(barrier.oldLayout, vk::ImageAspectFlagBits::eDepth);
        depth_barrier.newLayout =
            aspect_layout(barrier.newLayout, vk::ImageAspectFlagBits::eDepth);
        depth_barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth;
        barriers.emplace_back(depth_barrier);

        auto stencil_barrier = barrier;
        stencil_barrier.oldLayout =
            aspect_layout(barrier.oldLayout, vk::ImageAspectFlagBits::eStencil);
        stencil_barrier.newLayout =
            aspect_layout(barrier.newLayout, vk::ImageAspectFlagBits::eStencil);
        stencil_barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eStencil;
        barriers.emplace_back(stencil_barrier);
    };
    if (needs_partial_transition || partially_transited) {
        if (!partially_transited) {
            subresource_states.resize(info.resources.levels * info.resources.layers);
            std::fill(subresource_states.begin(), subresource_states.end(), last_state);
        }

        const auto mips =
            needs_partial_transition
                ? std::ranges::views::iota(subres_range->base.level,
                                           subres_range->base.level + subres_range->extent.levels)
                : std::views::iota(0u, info.resources.levels);
        const auto layers =
            needs_partial_transition
                ? std::ranges::views::iota(subres_range->base.layer,
                                           subres_range->base.layer + subres_range->extent.layers)
                : std::views::iota(0u, info.resources.layers);

        for (u32 mip : mips) {
            for (u32 layer : layers) {
                const auto subres_idx = mip * info.resources.layers + layer;
                ASSERT(subres_idx < subresource_states.size());
                auto& state = subresource_states[subres_idx];

                constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                             vk::AccessFlagBits2::eShaderWrite |
                                             vk::AccessFlagBits2::eMemoryWrite;
                const bool is_write = static_cast<bool>(state.access_mask & write_flags);
                if (state.layout != dst_layout || state.access_mask != dst_mask || is_write) {
                    append_image_barrier(vk::ImageMemoryBarrier2{
                        .srcStageMask = state.pl_stage,
                        .srcAccessMask = state.access_mask,
                        .dstStageMask = dst_stage,
                        .dstAccessMask = dst_mask,
                        .oldLayout = state.layout,
                        .newLayout = dst_layout,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = GetImage(),
                        .subresourceRange{
                            .aspectMask = aspect_mask,
                            .baseMipLevel = mip,
                            .levelCount = 1,
                            .baseArrayLayer = layer,
                            .layerCount = 1,
                        },
                    });
                    state.layout = dst_layout;
                    state.access_mask = dst_mask;
                    state.pl_stage = dst_stage;
                }
            }
        }

        if (!needs_partial_transition) {
            subresource_states.clear();
        }
    } else {
        constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                     vk::AccessFlagBits2::eShaderWrite |
                                     vk::AccessFlagBits2::eMemoryWrite;
        const bool is_write = static_cast<bool>(last_state.access_mask & write_flags);
        if (last_state.layout == dst_layout && last_state.access_mask == dst_mask && !is_write) {
            return {};
        }

        const bool explicit_full_counts = instance->GetCapabilityProfile().HasQuirk(
            CapabilityQuirk::ExplicitFullImageSubresourceCounts);
        append_image_barrier(vk::ImageMemoryBarrier2{
            .srcStageMask = last_state.pl_stage,
            .srcAccessMask = last_state.access_mask,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_mask,
            .oldLayout = last_state.layout,
            .newLayout = dst_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = GetImage(),
            .subresourceRange{
                .aspectMask = aspect_mask,
                .baseMipLevel = 0,
                .levelCount = explicit_full_counts ? info.resources.levels
                                                   : VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = explicit_full_counts ? info.resources.layers
                                                   : VK_REMAINING_ARRAY_LAYERS,
            },
        });
    }

    last_state.layout = dst_layout;
    last_state.access_mask = dst_mask;
    last_state.pl_stage = dst_stage;

    return barriers;
}

void Image::Transit(vk::ImageLayout dst_layout, vk::AccessFlags2 dst_mask,
                    std::optional<SubresourceRange> range, vk::CommandBuffer cmdbuf) {
    const bool guard_terminal_a750 =
        instance->GetCapabilityProfile().exact_proprietary_adreno750;
    if (guard_terminal_a750 && scheduler->IsDeviceLost()) [[unlikely]] {
        return;
    }
    scheduler->FlushPendingBufferTransfersForRange(info.guest_address,
                                                   info.guest_size);
    // Queue submission can report device loss while the graphics thread is
    // already inside a draw.  The proprietary A750 ICD faults if that thread
    // subsequently records another image barrier into the terminal command
    // buffer.  Keep the established A840, Turnip, Mali and desktop paths
    // byte-for-byte unchanged while making the A750 failure report retain the
    // original queue-submit fault instead of a secondary driver SIGSEGV.
    if (guard_terminal_a750 && scheduler->IsDeviceLost()) [[unlikely]] {
        return;
    }
    vk::PipelineStageFlags2 dst_pl_stage{};
    if (instance->GetCapabilityProfile().HasQuirk(
            CapabilityQuirk::ConservativeImageTransitionStages)) {
        // The older, conservative route is intentionally retained only for an
        // exact proprietary A750 A/B.  Its ICD has repeatedly faulted in
        // vkCmdPipelineBarrier2 while consuming attachment transitions.  A840,
        // Turnip, Mali, and desktop drivers keep the precise stage masks below.
        constexpr vk::AccessFlags2 transfer_access_mask =
            vk::AccessFlagBits2::eTransferRead |
            vk::AccessFlagBits2::eTransferWrite;
        if (static_cast<bool>(dst_mask & transfer_access_mask)) {
            dst_pl_stage |= vk::PipelineStageFlagBits2::eTransfer;
        }
        // Mixed shader/transfer transitions are used after uploads and image
        // copies.  Preserve both execution domains instead of treating them
        // as graphics-only just because the access mask is not exactly one
        // transfer bit.
        if (!dst_mask || static_cast<bool>(dst_mask & ~transfer_access_mask)) {
            dst_pl_stage |= vk::PipelineStageFlagBits2::eAllGraphics |
                            vk::PipelineStageFlagBits2::eComputeShader;
        }
    } else {
        if (static_cast<bool>(dst_mask & (vk::AccessFlagBits2::eTransferRead |
                                         vk::AccessFlagBits2::eTransferWrite))) {
            dst_pl_stage |= vk::PipelineStageFlagBits2::eTransfer;
        }
        if (static_cast<bool>(dst_mask & (vk::AccessFlagBits2::eShaderRead |
                                         vk::AccessFlagBits2::eShaderWrite))) {
            dst_pl_stage |= vk::PipelineStageFlagBits2::eAllGraphics |
                            vk::PipelineStageFlagBits2::eComputeShader;
        }
        if (static_cast<bool>(dst_mask & (vk::AccessFlagBits2::eColorAttachmentRead |
                                         vk::AccessFlagBits2::eColorAttachmentWrite))) {
            dst_pl_stage |= vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        }
        if (static_cast<bool>(
                dst_mask & (vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                            vk::AccessFlagBits2::eDepthStencilAttachmentWrite))) {
            dst_pl_stage |= vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                            vk::PipelineStageFlagBits2::eLateFragmentTests;
        }
        if (!dst_pl_stage) {
            dst_pl_stage = vk::PipelineStageFlagBits2::eAllCommands;
        }
    }

    const auto barriers = GetBarriers(dst_layout, dst_mask, dst_pl_stage, range);
    if (barriers.empty()) {
        return;
    }

    if (!cmdbuf) {
        scheduler->EndRendering(Scheduler::EndRenderingReason::ImageTransit);
        if (guard_terminal_a750 && !scheduler->HasRecordingCommandBuffer()) [[unlikely]] {
            return;
        }
        cmdbuf = scheduler->CommandBuffer();
    }
    if (guard_terminal_a750 && scheduler->IsDeviceLost()) [[unlikely]] {
        return;
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
        .pImageMemoryBarriers = barriers.data(),
    });
}

void Image::Upload(std::span<const vk::BufferImageCopy> upload_copies, vk::Buffer buffer,
                   u64 offset) {
    SetBackingSamples(info.num_samples, false);
    scheduler->EndRendering(Scheduler::EndRenderingReason::ImageUpload);

    const vk::BufferMemoryBarrier2 pre_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .buffer = buffer,
        .offset = offset,
        .size = info.guest_size,
    };
    const vk::BufferMemoryBarrier2 post_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .buffer = buffer,
        .offset = offset,
        .size = info.guest_size,
    };
    const auto image_barriers =
        GetBarriers(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
                    vk::PipelineStageFlagBits2::eCopy, {});
    const auto cmdbuf = scheduler->CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_barrier,
        .imageMemoryBarrierCount = static_cast<u32>(image_barriers.size()),
        .pImageMemoryBarriers = image_barriers.data(),
    });
    cmdbuf.copyBufferToImage(buffer, GetImage(), vk::ImageLayout::eTransferDstOptimal,
                             upload_copies);
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post_barrier,
    });
    Transit(vk::ImageLayout::eGeneral,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {});
    flags &= ~ImageFlagBits::Dirty;
}

void Image::Download(std::span<const vk::BufferImageCopy> download_copies, vk::Buffer buffer,
                     u64 offset, u64 download_size) {
    SetBackingSamples(info.num_samples);
    scheduler->EndRendering(Scheduler::EndRenderingReason::ImageDownload);

    const vk::BufferMemoryBarrier2 pre_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .buffer = buffer,
        .offset = offset,
        .size = download_size,
    };
    const vk::BufferMemoryBarrier2 post_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .buffer = buffer,
        .offset = offset,
        .size = download_size,
    };
    const auto image_barriers =
        GetBarriers(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                    vk::PipelineStageFlagBits2::eCopy, {});
    auto cmdbuf = scheduler->CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_barrier,
        .imageMemoryBarrierCount = static_cast<u32>(image_barriers.size()),
        .pImageMemoryBarriers = image_barriers.data(),
    });
    cmdbuf.copyImageToBuffer(GetImage(), vk::ImageLayout::eTransferSrcOptimal, buffer,
                             download_copies);
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post_barrier,
    });
}

static std::pair<u32, u32> SanitizeCopyLayers(const ImageInfo& src_info, const ImageInfo& dst_info,
                                              const u32 depth) {
    const auto vk_src_type = ConvertImageType(src_info.type);
    const auto vk_dst_type = ConvertImageType(dst_info.type);

    u32 src_layers = src_info.resources.layers;
    u32 dst_layers = dst_info.resources.layers;

    if (vk_src_type == vk::ImageType::e3D && src_layers != 1) {
        LOG_WARNING(Render_Vulkan, "Coercing copy 3D source layers {} to 1.", src_layers);
        src_layers = 1;
    }
    if (vk_dst_type == vk::ImageType::e3D && dst_layers != 1) {
        LOG_WARNING(Render_Vulkan, "Coercing copy 3D destination layers {} to 1.", dst_layers);
        dst_layers = 1;
    }

    if (vk_src_type == vk_dst_type) {
        if (src_layers != dst_layers) {
            LOG_WARNING(Render_Vulkan,
                        "Coercing copy source layers {} and destination layers {} to minimum.",
                        src_layers, dst_layers);
            src_layers = dst_layers = std::min(src_layers, dst_layers);
        }
    } else {
        if (vk_src_type == vk::ImageType::e2D && vk_dst_type == vk::ImageType::e3D &&
            src_layers != depth) {
            LOG_WARNING(Render_Vulkan,
                        "Coercing copy 2D source layers {} to 3D destination depth {}", src_layers,
                        depth);
            src_layers = depth;
        }
        if (vk_src_type == vk::ImageType::e3D && vk_dst_type == vk::ImageType::e2D &&
            dst_layers != depth) {
            LOG_WARNING(Render_Vulkan,
                        "Coercing copy 2D destination layers {} to 3D source depth {}", dst_layers,
                        depth);
            dst_layers = depth;
        }
    }

    return std::make_pair(src_layers, dst_layers);
}

void Image::CopyImage(Image& src_image) {
    const auto& src_info = src_image.info;

    const u32 num_mips = std::min(src_info.resources.levels, info.resources.levels);

    if (src_info.pixel_format != info.pixel_format) {
        LOG_DEBUG(Render_Vulkan,
                  "Copy between different formats: src={}, dst={}. "
                  "Result may be undefined.",
                  vk::to_string(src_info.pixel_format), vk::to_string(info.pixel_format));
    }

    const u32 base_width = src_info.size.width;
    const u32 base_height = src_info.size.height;
    const u32 base_depth =
        info.type == AmdGpu::ImageType::Color3D ? info.size.depth : src_info.size.depth;

    SetBackingSamples(info.num_samples, false);
    src_image.SetBackingSamples(src_info.num_samples);

    boost::container::small_vector<vk::ImageCopy, 8> regions;

    const vk::ImageAspectFlags src_aspect =
        src_image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil;

    const vk::ImageAspectFlags dst_aspect = aspect_mask & ~vk::ImageAspectFlagBits::eStencil;

    const bool src_is_2d = ConvertImageType(src_info.type) == vk::ImageType::e2D;
    const bool src_is_3d = ConvertImageType(src_info.type) == vk::ImageType::e3D;

    const bool dst_is_2d = ConvertImageType(info.type) == vk::ImageType::e2D;
    const bool dst_is_3d = ConvertImageType(info.type) == vk::ImageType::e3D;

    const bool is_2d_to_3d = src_is_2d && dst_is_3d;
    const bool is_3d_to_2d = src_is_3d && dst_is_2d;
    const bool is_same_type = !is_2d_to_3d && !is_3d_to_2d;

    for (u32 mip = 0; mip < num_mips; ++mip) {
        const u32 mip_w = std::max(base_width >> mip, 1u);
        const u32 mip_h = std::max(base_height >> mip, 1u);
        const u32 mip_d = std::max(base_depth >> mip, 1u);

        auto [src_layers, dst_layers] = SanitizeCopyLayers(src_info, info, mip_d);

        vk::ImageCopy region{};

        region.srcSubresource.aspectMask = src_aspect;
        region.srcSubresource.mipLevel = mip;
        region.srcSubresource.baseArrayLayer = 0;

        region.dstSubresource.aspectMask = dst_aspect;
        region.dstSubresource.mipLevel = mip;
        region.dstSubresource.baseArrayLayer = 0;

        if (is_same_type) {
            if (src_is_3d) {
                region.srcSubresource.layerCount = 1;
                region.dstSubresource.layerCount = 1;
                region.extent = vk::Extent3D(mip_w, mip_h, mip_d);
            } else {
                const u32 copy_layers = std::min(src_layers, dst_layers);
                region.srcSubresource.layerCount = copy_layers;
                region.dstSubresource.layerCount = copy_layers;
                region.extent = vk::Extent3D(mip_w, mip_h, 1);
            }
        } else if (is_2d_to_3d) {
            region.srcSubresource.layerCount = src_layers;
            region.dstSubresource.layerCount = 1;
            region.extent = vk::Extent3D(mip_w, mip_h, src_layers);
        } else if (is_3d_to_2d) {
            region.srcSubresource.layerCount = 1;
            region.dstSubresource.layerCount = dst_layers;
            region.extent = vk::Extent3D(mip_w, mip_h, dst_layers);
        }

        regions.push_back(region);
    }

    scheduler->EndRendering(Scheduler::EndRenderingReason::RasterizerCopy);

    src_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});

    Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {});

    auto cmdbuf = scheduler->CommandBuffer();

    if (!regions.empty()) {
        cmdbuf.copyImage(src_image.GetImage(), src_image.backing->state.layout, GetImage(),
                         backing->state.layout, regions);
    }

    Transit(vk::ImageLayout::eGeneral,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {});
}
void Image::CopyImageWithBuffer(Image& src_image, vk::Buffer buffer, u64 offset) {
    const auto& src_info = src_image.info;
    const u32 num_mips = std::min(src_info.resources.levels, info.resources.levels);
    const u32 num_layers = std::min(src_info.resources.layers, info.resources.layers);
    ASSERT(src_info.resources.layers == info.resources.layers || num_mips == 1);

    SetBackingSamples(info.num_samples, false);
    src_image.SetBackingSamples(src_info.num_samples);

    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < num_mips; ++mip) {
        const auto mip_w = std::max(src_info.size.width >> mip, 1u);
        const auto mip_h = std::max(src_info.size.height >> mip, 1u);
        const auto mip_d = std::max(src_info.size.depth >> mip, 1u);

        buffer_copies.emplace_back(vk::BufferImageCopy{
            .bufferOffset = offset,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource{
                .aspectMask = src_image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = num_layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {mip_w, mip_h, mip_d},
        });
    }

    const vk::BufferMemoryBarrier2 pre_copy_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .buffer = buffer,
        .offset = offset,
        .size = VK_WHOLE_SIZE,
    };

    const vk::BufferMemoryBarrier2 post_copy_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .buffer = buffer,
        .offset = offset,
        .size = VK_WHOLE_SIZE,
    };

    scheduler->EndRendering(Scheduler::EndRenderingReason::RasterizerCopy);
    src_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});
    Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {});

    auto cmdbuf = scheduler->CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_copy_barrier,
    });

    cmdbuf.copyImageToBuffer(src_image.GetImage(), vk::ImageLayout::eTransferSrcOptimal, buffer,
                             buffer_copies);

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post_copy_barrier,
    });

    for (auto& copy : buffer_copies) {
        copy.imageSubresource.aspectMask = aspect_mask & ~vk::ImageAspectFlagBits::eStencil;
    }

    cmdbuf.copyBufferToImage(buffer, GetImage(), vk::ImageLayout::eTransferDstOptimal,
                             buffer_copies);
    Transit(vk::ImageLayout::eGeneral,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {});
}

void Image::CopyMip(Image& src_image, u32 mip, u32 slice) {
    const auto& src_info = src_image.info;

    const auto src_block_dim = src_info.BlockDim();
    const auto dst_block_dim = info.BlockDim();
    const auto mip_block_w = std::max(dst_block_dim.width >> mip, 1u);
    const auto mip_block_h = std::max(dst_block_dim.height >> mip, 1u);
    ASSERT(mip_block_w == src_block_dim.width);
    ASSERT(mip_block_h == src_block_dim.height);

    const auto [src_layers, dst_layers] = SanitizeCopyLayers(src_info, info, src_info.size.depth);

    const vk::ImageCopy image_copy{
        .srcSubresource{
            .aspectMask = src_image.aspect_mask,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = src_layers,
        },
        .dstSubresource{
            .aspectMask = src_image.aspect_mask,
            .mipLevel = mip,
            .baseArrayLayer = slice,
            .layerCount = dst_layers,
        },
        .extent = {src_info.size.width, src_info.size.height, src_info.size.depth},
    };

    SetBackingSamples(info.num_samples);
    src_image.SetBackingSamples(src_info.num_samples);

    scheduler->EndRendering(Scheduler::EndRenderingReason::RasterizerCopy);
    Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {});
    src_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});

    const auto cmdbuf = scheduler->CommandBuffer();
    cmdbuf.copyImage(src_image.GetImage(), src_image.backing->state.layout, GetImage(),
                     backing->state.layout, image_copy);
    Transit(vk::ImageLayout::eGeneral,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {});
}

void Image::Resolve(Image& src_image, const VideoCore::SubresourceRange& mrt0_range,
                    const VideoCore::SubresourceRange& mrt1_range) {
    SetBackingSamples(1, false);
    scheduler->EndRendering(Scheduler::EndRenderingReason::RasterizerCopy);

    src_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                      mrt0_range);
    Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, mrt1_range);

    const auto [src_layers, dst_layers] = SanitizeCopyLayers(src_image.info, info, 1);
    if (src_image.backing->num_samples == 1) {
        const vk::ImageCopy region = {
            .srcSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = mrt0_range.base.layer,
                .layerCount = src_layers,
            },
            .srcOffset = {0, 0, 0},
            .dstSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = mrt1_range.base.layer,
                .layerCount = dst_layers,
            },
            .dstOffset = {0, 0, 0},
            .extent = {info.size.width, info.size.height, 1},
        };
        scheduler->CommandBuffer().copyImage(src_image.GetImage(),
                                             vk::ImageLayout::eTransferSrcOptimal, GetImage(),
                                             vk::ImageLayout::eTransferDstOptimal, region);
    } else {
        const vk::ImageResolve region = {
            .srcSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = mrt0_range.base.layer,
                .layerCount = src_layers,
            },
            .srcOffset = {0, 0, 0},
            .dstSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = mrt1_range.base.layer,
                .layerCount = dst_layers,
            },
            .dstOffset = {0, 0, 0},
            .extent = {info.size.width, info.size.height, 1},
        };
        scheduler->CommandBuffer().resolveImage(src_image.GetImage(),
                                                vk::ImageLayout::eTransferSrcOptimal, GetImage(),
                                                vk::ImageLayout::eTransferDstOptimal, region);
    }

    flags |= VideoCore::ImageFlagBits::GpuModified;
    flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Image::Clear(const vk::ClearValue& clear_value, const VideoCore::SubresourceRange& range) {
    const vk::ImageSubresourceRange vk_range = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = range.base.level,
        .levelCount = range.extent.levels,
        .baseArrayLayer = range.base.layer,
        .layerCount = range.extent.layers,
    };
    scheduler->EndRendering(Scheduler::EndRenderingReason::RasterizerClear);
    Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {});
    const auto cmdbuf = scheduler->CommandBuffer();
    cmdbuf.clearColorImage(GetImage(), vk::ImageLayout::eTransferDstOptimal, clear_value.color,
                           vk_range);
}

void Image::EnsureBackingRole(const ImageBackingRole role) {
    if (!role_scoped_usage || role == ImageBackingRole::Universal || !backing) {
        return;
    }
    const vk::ImageUsageFlags required_usage =
        RoleScopedImageUsageFlags(instance, info, role);
    const vk::ImageUsageFlags expanded_usage = usage_flags | required_usage;
    if ((usage_flags & required_usage) == required_usage) {
        return;
    }
    // Role changes are rare (the Bloodborne census found 11 mixed-role images
    // out of 1296), but their contents can already be GPU-authored. Replace
    // the current backing with an expanded image and copy every subresource;
    // defer old views/image ownership until the recording epoch retires.
    scheduler->EndRendering(Scheduler::EndRenderingReason::RasterizerCopy);

    auto expanded_ci = backing->image.image_ci;
    expanded_ci.usage = expanded_usage;
    const vk::FormatFeatureFlags2 expanded_features =
        FullImageUsageFormatFeatures(expanded_usage);
    if (instance->IsOptimalImageFormatSupported(expanded_ci.format,
                                                expanded_features)) {
        expanded_ci.flags &= ~vk::ImageCreateFlagBits::eExtendedUsage;
    } else {
        expanded_ci.flags |= vk::ImageCreateFlagBits::eExtendedUsage;
    }
    const auto bounded_view_formats =
        BoundedImageViewFormats(instance, info.pixel_format, expanded_ci.format,
                                expanded_ci.flags, expanded_features);
    const vk::ImageFormatListCreateInfo expanded_format_list{
        .viewFormatCount = static_cast<u32>(bounded_view_formats.size()),
        .pViewFormats = bounded_view_formats.data(),
    };
    const vk::PhysicalDeviceImageFormatInfo2 expanded_format_info{
        .pNext = bounded_view_formats.empty() ? nullptr : &expanded_format_list,
        .format = expanded_ci.format,
        .type = expanded_ci.imageType,
        .tiling = expanded_ci.tiling,
        .usage = expanded_ci.usage,
        .flags = expanded_ci.flags,
    };
    const auto expanded_properties =
        instance->GetPhysicalDevice().getImageFormatProperties2(expanded_format_info);
    ASSERT_MSG(expanded_properties.result == vk::Result::eSuccess,
               "Expanded image role is unsupported for format {} usage {} flags {}",
               vk::to_string(expanded_ci.format), vk::to_string(expanded_ci.usage),
               vk::to_string(expanded_ci.flags));
    const vk::SampleCountFlags expanded_supported_samples =
        expanded_properties.value.imageFormatProperties.sampleCounts;
    const auto current_samples =
        static_cast<vk::SampleCountFlagBits>(backing->num_samples);
    if (!(expanded_supported_samples & current_samples)) {
        // A common mobile case is RT 2x being valid only without Storage.  Resolve the existing
        // single-subresource color image to a 1x role-scoped backing before expanding usage;
        // this is the same conservative sample count the former universal image selected.
        ASSERT_MSG(backing->num_samples > 1 && !info.props.is_depth &&
                       info.resources.levels == 1 && info.resources.layers == 1 &&
                       (expanded_supported_samples & vk::SampleCountFlagBits::e1),
                   "Cannot conservatively expand a {}x multisampled image role",
                   backing->num_samples);
        SetBackingSamples(1, true);
        expanded_ci = backing->image.image_ci;
        expanded_ci.usage = expanded_usage;
    }
    supported_samples = expanded_supported_samples;

    BackingImage expanded_backing{UniqueImage{}};
    expanded_backing.num_samples = backing->num_samples;
    expanded_backing.image =
        UniqueImage{instance->GetDevice(), instance->GetAllocator()};
    CreateTextureCacheImage(expanded_backing.image, expanded_ci,
                            bounded_view_formats);

    auto barriers = GetBarriers(vk::ImageLayout::eTransferSrcOptimal,
                                vk::AccessFlagBits2::eTransferRead,
                                vk::PipelineStageFlagBits2::eTransfer,
                                std::nullopt);
    barriers.push_back(vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eNone,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = expanded_backing.image,
        .subresourceRange{
            .aspectMask = aspect_mask,
            .baseMipLevel = 0,
            .levelCount = info.resources.levels,
            .baseArrayLayer = 0,
            .layerCount = info.resources.layers,
        },
    });
    const auto cmdbuf = scheduler->CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
        .pImageMemoryBarriers = barriers.data(),
    });

    boost::container::small_vector<vk::ImageCopy, 32> copies;
    const vk::ImageType host_type = ConvertImageType(info.type);
    const u32 copy_layers =
        host_type == vk::ImageType::e3D ? 1u : info.resources.layers;
    const std::array<vk::ImageAspectFlagBits, 3> aspects{
        vk::ImageAspectFlagBits::eColor, vk::ImageAspectFlagBits::eDepth,
        vk::ImageAspectFlagBits::eStencil};
    for (u32 mip = 0; mip < info.resources.levels; ++mip) {
        for (const vk::ImageAspectFlagBits aspect : aspects) {
            if (!(aspect_mask & aspect)) {
                continue;
            }
            copies.emplace_back(vk::ImageCopy{
                .srcSubresource{
                    .aspectMask = aspect,
                    .mipLevel = mip,
                    .baseArrayLayer = 0,
                    .layerCount = copy_layers,
                },
                .dstSubresource{
                    .aspectMask = aspect,
                    .mipLevel = mip,
                    .baseArrayLayer = 0,
                    .layerCount = copy_layers,
                },
                .extent{
                    .width = std::max(info.size.width >> mip, 1u),
                    .height = std::max(info.size.height >> mip, 1u),
                    .depth = std::max(info.size.depth >> mip, 1u),
                },
            });
        }
    }
    cmdbuf.copyImage(backing->image, vk::ImageLayout::eTransferSrcOptimal,
                     expanded_backing.image,
                     vk::ImageLayout::eTransferDstOptimal, copies);
    const vk::ImageMemoryBarrier2 finish_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead |
                         vk::AccessFlagBits2::eMemoryWrite,
        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = expanded_backing.image,
        .subresourceRange{
            .aspectMask = aspect_mask,
            .baseMipLevel = 0,
            .levelCount = info.resources.levels,
            .baseArrayLayer = 0,
            .layerCount = info.resources.layers,
        },
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &finish_barrier,
    });
    expanded_backing.state = State{
        .pl_stage = vk::PipelineStageFlagBits2::eAllCommands,
        .access_mask = vk::AccessFlagBits2::eMemoryRead |
                       vk::AccessFlagBits2::eMemoryWrite,
        .layout = vk::ImageLayout::eGeneral,
    };

    BackingImage retired_backing = std::move(*backing);
    *backing = std::move(expanded_backing);
    usage_flags = expanded_usage;
    format_features = expanded_features;
    binding.needs_rebind = binding.is_bound || binding.is_target;
    auto* views = slot_image_views;
    scheduler->DeferOperation(
        [retired = std::move(retired_backing), views]() mutable {
            for (const ImageViewId view_id : retired.image_view_ids) {
                views->erase(view_id);
            }
        });

    Vulkan::SetObjectName(instance->GetDevice(), GetImage(),
                          "Role-expanded image {}x{} {} {:#x}:{:#x}",
                          info.size.width, info.size.height,
                          vk::to_string(info.pixel_format), info.guest_address,
                          info.guest_size);
#ifdef __ANDROID__
    static std::atomic<u32> role_expansion_logs{};
    const u32 ordinal =
        role_expansion_logs.fetch_add(1, std::memory_order_relaxed);
    if (ordinal < 32) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_A750_IMAGE_ROLE_EXPANSION] ordinal=%u uid=%llu "
            "address=0x%llx role=%u usage=0x%x allocation=%llu",
            ordinal + 1, static_cast<unsigned long long>(image_uid),
            static_cast<unsigned long long>(info.guest_address),
            static_cast<u32>(role), static_cast<u32>(usage_flags),
            static_cast<unsigned long long>(backing->image.allocation_size));
    }
#endif
}

void Image::SetBackingSamples(u32 num_samples, bool copy_backing) {
    const vk::SampleCountFlagBits host_samples =
        LiverpoolToVK::NumSamples(num_samples, supported_samples);
    const u32 host_num_samples = static_cast<u32>(host_samples);
#ifdef __ANDROID__
    if (std::getenv("EXECUTOR_TRACE_VIDEOOUT_SOURCE") != nullptr &&
        (num_samples > 1 || (backing && backing->num_samples > 1))) {
        static std::atomic<u32> multisample_swap_logs{};
        const u32 ordinal = multisample_swap_logs.fetch_add(1, std::memory_order_relaxed);
        if (ordinal < 48) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_MULTISAMPLE_BACKING] phase=set ordinal=%u requested=%u "
                "supported=0x%x host=%u current=%u copy=%u usage=0x%x roleScoped=%u "
                "address=0x%llx",
                ordinal + 1, num_samples, static_cast<u32>(supported_samples),
                host_num_samples, backing ? backing->num_samples : 0u,
                copy_backing ? 1u : 0u, static_cast<u32>(usage_flags),
                role_scoped_usage ? 1u : 0u,
                static_cast<unsigned long long>(info.guest_address));
        }
    }
#endif
    if (!backing || backing->num_samples == host_num_samples) {
        return;
    }
    ASSERT_MSG(!info.props.is_depth, "Swapping samples is only valid for color images");
    BackingImage* new_backing;
    auto it = std::ranges::find_if(backing_images, [&](const BackingImage& candidate) {
        return candidate.num_samples == host_num_samples &&
               candidate.image.image_ci.usage == usage_flags;
    });
    if (it == backing_images.end()) {
        auto new_image_ci = backing->image.image_ci;
        new_image_ci.samples = host_samples;
        new_backing = &backing_images.emplace_back();
        new_backing->num_samples = host_num_samples;
        new_backing->image = UniqueImage{instance->GetDevice(), instance->GetAllocator()};
        const auto bounded_view_formats =
            BoundedImageViewFormats(instance, info.pixel_format,
                                    new_image_ci.format, new_image_ci.flags,
                                    format_features);
        CreateTextureCacheImage(new_backing->image, new_image_ci, bounded_view_formats);

        Vulkan::SetObjectName(instance->GetDevice(), new_backing->image.image,
                              "Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{} (backing)",
                              info.size.width, info.size.height, info.size.depth,
                              AmdGpu::NameOf(info.tile_mode), vk::to_string(info.pixel_format),
                              info.guest_address, info.guest_size, info.resources.layers,
                              info.resources.levels, host_num_samples);
    } else {
        new_backing = std::addressof(*it);
    }

    if (copy_backing) {
        scheduler->EndRendering(Scheduler::EndRenderingReason::RasterizerCopy);
        ASSERT(info.resources.levels == 1 && info.resources.layers == 1);

        auto barriers =
            GetBarriers(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead,
                        vk::PipelineStageFlagBits2::eFragmentShader, std::nullopt);

        constexpr auto dst_stage = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        constexpr auto dst_access = vk::AccessFlagBits2::eColorAttachmentWrite;
        constexpr auto dst_layout = vk::ImageLayout::eColorAttachmentOptimal;
        barriers.push_back(vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = dst_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = new_backing->image,
            .subresourceRange{
                .aspectMask = aspect_mask,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = info.resources.layers,
            },
        });
        const auto cmdbuf = scheduler->CommandBuffer();
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
            .pImageMemoryBarriers = barriers.data(),
        });

        blit_helper->CopyBetweenMsImages(
            info.size.width, info.size.height, new_backing->num_samples, info.pixel_format,
            backing->num_samples > 1, backing->image, new_backing->image);

        new_backing->state.layout = dst_layout;
        new_backing->state.access_mask = dst_access;
        new_backing->state.pl_stage = dst_stage;
    }

    backing = new_backing;
}

}
