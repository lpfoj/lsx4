// SPDX-FileCopyrightText: Copyright 2026 LSX4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_legacy_sync.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <utility>

#include <boost/container/small_vector.hpp>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {
namespace {

struct StageMapping {
    VkPipelineStageFlags2 synchronization2;
    VkPipelineStageFlags legacy;
};

struct AccessMapping {
    VkAccessFlags2 synchronization2;
    VkAccessFlags legacy;
};

struct StageConversion {
    VkPipelineStageFlags legacy;
    VkPipelineStageFlags2 unknown;
};

struct AccessConversion {
    VkAccessFlags legacy;
    VkAccessFlags2 unknown;
};

constexpr std::array StageMappings{
    StageMapping{VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT},
    StageMapping{VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT},
    StageMapping{VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT},
    StageMapping{VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT},
    StageMapping{VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
                 VK_PIPELINE_STAGE_VERTEX_INPUT_BIT},
    StageMapping{VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT},
    StageMapping{VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT,
                 VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT},
    StageMapping{VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT,
                 VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT},
    StageMapping{VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT, VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT},
    StageMapping{VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT},
    StageMapping{VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT},
    StageMapping{VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT},
    StageMapping{VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT},
    StageMapping{VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT},
    StageMapping{VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT},
    StageMapping{VK_PIPELINE_STAGE_2_COPY_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT},
    StageMapping{VK_PIPELINE_STAGE_2_BLIT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT},
    StageMapping{VK_PIPELINE_STAGE_2_RESOLVE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT},
    StageMapping{VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT},
    StageMapping{VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT},
    StageMapping{VK_PIPELINE_STAGE_2_HOST_BIT, VK_PIPELINE_STAGE_HOST_BIT},
    StageMapping{VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT},
    StageMapping{VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT},
    StageMapping{VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT,
                 VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT},
};

constexpr std::array AccessMappings{
    AccessMapping{VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT},
    AccessMapping{VK_ACCESS_2_INDEX_READ_BIT, VK_ACCESS_INDEX_READ_BIT},
    AccessMapping{VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT},
    AccessMapping{VK_ACCESS_2_UNIFORM_READ_BIT, VK_ACCESS_UNIFORM_READ_BIT},
    AccessMapping{VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT, VK_ACCESS_INPUT_ATTACHMENT_READ_BIT},
    AccessMapping{VK_ACCESS_2_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT},
    AccessMapping{VK_ACCESS_2_SHADER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT},
    AccessMapping{VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT},
    AccessMapping{VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT},
    AccessMapping{VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT},
    AccessMapping{VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT},
    AccessMapping{VK_ACCESS_2_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT},
    AccessMapping{VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT},
    AccessMapping{VK_ACCESS_2_HOST_READ_BIT, VK_ACCESS_HOST_READ_BIT},
    AccessMapping{VK_ACCESS_2_HOST_WRITE_BIT, VK_ACCESS_HOST_WRITE_BIT},
    AccessMapping{VK_ACCESS_2_MEMORY_READ_BIT, VK_ACCESS_MEMORY_READ_BIT},
    AccessMapping{VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_WRITE_BIT},
    AccessMapping{VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_ACCESS_SHADER_READ_BIT},
    AccessMapping{VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_ACCESS_SHADER_READ_BIT},
    AccessMapping{VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT},
};

constexpr StageConversion ConvertStageFlags(const VkPipelineStageFlags2 flags) {
    StageConversion converted{};
    VkPipelineStageFlags2 known{};
    for (const auto& mapping : StageMappings) {
        if ((flags & mapping.synchronization2) != 0) {
            converted.legacy |= mapping.legacy;
            known |= mapping.synchronization2;
        }
    }
    converted.unknown = flags & ~known;
    return converted;
}

constexpr AccessConversion ConvertAccessFlags(const VkAccessFlags2 flags) {
    AccessConversion converted{};
    VkAccessFlags2 known{};
    for (const auto& mapping : AccessMappings) {
        if ((flags & mapping.synchronization2) != 0) {
            converted.legacy |= mapping.legacy;
            known |= mapping.synchronization2;
        }
    }
    converted.unknown = flags & ~known;
    return converted;
}

constexpr VkPipelineStageFlags LegacySourceStages(const VkPipelineStageFlags stages,
                                                  const bool has_barriers) {
    return stages != 0 ? stages
                       : (has_barriers ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                       : VkPipelineStageFlags{});
}

constexpr VkPipelineStageFlags LegacyDestinationStages(const VkPipelineStageFlags stages,
                                                       const bool has_barriers) {
    return stages != 0 ? stages
                       : (has_barriers ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
                                       : VkPipelineStageFlags{});
}

constexpr VkPipelineStageFlags ConservativeStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
constexpr VkAccessFlags ConservativeAccessMask =
    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

constexpr bool NeedsConservativeFallback(const VkPipelineStageFlags2 unknown_stages,
                                         const VkAccessFlags2 unknown_accesses,
                                         const bool unsupported_chain) {
    return unknown_stages != 0 || unknown_accesses != 0 || unsupported_chain;
}

static_assert(ConvertStageFlags(VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT).legacy ==
              VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
static_assert(ConvertStageFlags(VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT).legacy ==
              VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
static_assert(ConvertStageFlags(VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT |
                                VK_PIPELINE_STAGE_2_RESOLVE_BIT |
                                VK_PIPELINE_STAGE_2_CLEAR_BIT)
                      .legacy == VK_PIPELINE_STAGE_TRANSFER_BIT);
static_assert(ConvertAccessFlags(VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT).legacy ==
              VK_ACCESS_SHADER_WRITE_BIT);
static_assert(ConvertAccessFlags(VK_ACCESS_2_NONE).legacy == 0 &&
              ConvertAccessFlags(VK_ACCESS_2_NONE).unknown == 0);
static_assert(NeedsConservativeFallback(
    ConvertStageFlags(VkPipelineStageFlags2{1} << 63).unknown, 0, false));
static_assert(NeedsConservativeFallback(
    0, ConvertAccessFlags(VkAccessFlags2{1} << 63).unknown, false));
static_assert(NeedsConservativeFallback(0, 0, true));
static_assert(!NeedsConservativeFallback(0, 0, false));
static_assert((ConservativeAccessMask & VK_ACCESS_MEMORY_READ_BIT) != 0 &&
              (ConservativeAccessMask & VK_ACCESS_MEMORY_WRITE_BIT) != 0);
static_assert(LegacySourceStages(0, true) == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
static_assert(LegacyDestinationStages(0, true) == VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
static_assert(LegacySourceStages(0, false) == 0);
static_assert(LegacyDestinationStages(0, false) == 0);

void LogConservativeFallbackOnce(const VkPipelineStageFlags2 unknown_stages,
                                 const VkAccessFlags2 unknown_accesses,
                                 const bool unsupported_chain) {
    static std::atomic_flag logged = ATOMIC_FLAG_INIT;
    if (logged.test_and_set(std::memory_order_relaxed)) {
        return;
    }
    LOG_ERROR(Render_Vulkan,
              "[VK_LEGACY_SYNC_UNKNOWN] unknownStages={:#018x} unknownAccesses={:#018x} "
              "unsupportedPNext={} fallback=all_commands_full_memory",
              static_cast<std::uint64_t>(unknown_stages),
              static_cast<std::uint64_t>(unknown_accesses), unsupported_chain);
}

} // namespace

void PipelineBarrier(const Scheduler& scheduler, const vk::CommandBuffer command_buffer,
                     const vk::DependencyInfo& dependency_info) {
    if (scheduler.GetInstance().IsSynchronization2Supported()) {
        command_buffer.pipelineBarrier2(dependency_info);
        return;
    }

    bool has_barriers = dependency_info.memoryBarrierCount != 0 ||
                        dependency_info.bufferMemoryBarrierCount != 0 ||
                        dependency_info.imageMemoryBarrierCount != 0;
    const bool has_dependency_chain = dependency_info.pNext != nullptr;
    if (!has_barriers && !has_dependency_chain) {
        return;
    }

    boost::container::small_vector<vk::MemoryBarrier, 4> memory_barriers;
    boost::container::small_vector<vk::BufferMemoryBarrier, 16> buffer_barriers;
    boost::container::small_vector<vk::ImageMemoryBarrier, 32> image_barriers;
    memory_barriers.reserve(dependency_info.memoryBarrierCount + (has_dependency_chain ? 1 : 0));
    buffer_barriers.reserve(dependency_info.bufferMemoryBarrierCount);
    image_barriers.reserve(dependency_info.imageMemoryBarrierCount);

    VkPipelineStageFlags source_stages{};
    VkPipelineStageFlags destination_stages{};
    VkPipelineStageFlags2 unknown_stages{};
    VkAccessFlags2 unknown_accesses{};
    bool unsupported_chain = has_dependency_chain;

    if (!has_barriers) {
        memory_barriers.emplace_back();
        has_barriers = true;
    }

    const auto convert_masks = [&](const vk::PipelineStageFlags2 source_stage_mask,
                                   const vk::AccessFlags2 source_access_mask,
                                   const vk::PipelineStageFlags2 destination_stage_mask,
                                   const vk::AccessFlags2 destination_access_mask) {
        const auto source_stage =
            ConvertStageFlags(static_cast<VkPipelineStageFlags2>(source_stage_mask));
        const auto destination_stage =
            ConvertStageFlags(static_cast<VkPipelineStageFlags2>(destination_stage_mask));
        const auto source_access =
            ConvertAccessFlags(static_cast<VkAccessFlags2>(source_access_mask));
        const auto destination_access =
            ConvertAccessFlags(static_cast<VkAccessFlags2>(destination_access_mask));
        source_stages |= source_stage.legacy;
        destination_stages |= destination_stage.legacy;
        unknown_stages |= source_stage.unknown | destination_stage.unknown;
        unknown_accesses |= source_access.unknown | destination_access.unknown;
        return std::pair{source_access.legacy, destination_access.legacy};
    };

    for (std::uint32_t index = 0; index < dependency_info.memoryBarrierCount; ++index) {
        const auto& source = dependency_info.pMemoryBarriers[index];
        const auto [source_access, destination_access] =
            convert_masks(source.srcStageMask, source.srcAccessMask, source.dstStageMask,
                          source.dstAccessMask);
        unsupported_chain |= source.pNext != nullptr;
        memory_barriers.push_back(vk::MemoryBarrier{
            .srcAccessMask = vk::AccessFlags{source_access},
            .dstAccessMask = vk::AccessFlags{destination_access},
        });
    }

    for (std::uint32_t index = 0; index < dependency_info.bufferMemoryBarrierCount; ++index) {
        const auto& source = dependency_info.pBufferMemoryBarriers[index];
        const auto [source_access, destination_access] =
            convert_masks(source.srcStageMask, source.srcAccessMask, source.dstStageMask,
                          source.dstAccessMask);
        unsupported_chain |= source.pNext != nullptr;
        buffer_barriers.push_back(vk::BufferMemoryBarrier{
            .srcAccessMask = vk::AccessFlags{source_access},
            .dstAccessMask = vk::AccessFlags{destination_access},
            .srcQueueFamilyIndex = source.srcQueueFamilyIndex,
            .dstQueueFamilyIndex = source.dstQueueFamilyIndex,
            .buffer = source.buffer,
            .offset = source.offset,
            .size = source.size,
        });
    }

    for (std::uint32_t index = 0; index < dependency_info.imageMemoryBarrierCount; ++index) {
        const auto& source = dependency_info.pImageMemoryBarriers[index];
        const auto [source_access, destination_access] =
            convert_masks(source.srcStageMask, source.srcAccessMask, source.dstStageMask,
                          source.dstAccessMask);
        unsupported_chain |= source.pNext != nullptr;
        image_barriers.push_back(vk::ImageMemoryBarrier{
            .srcAccessMask = vk::AccessFlags{source_access},
            .dstAccessMask = vk::AccessFlags{destination_access},
            .oldLayout = source.oldLayout,
            .newLayout = source.newLayout,
            .srcQueueFamilyIndex = source.srcQueueFamilyIndex,
            .dstQueueFamilyIndex = source.dstQueueFamilyIndex,
            .image = source.image,
            .subresourceRange = source.subresourceRange,
        });
    }

    if (NeedsConservativeFallback(unknown_stages, unknown_accesses, unsupported_chain)) {
        LogConservativeFallbackOnce(unknown_stages, unknown_accesses, unsupported_chain);
        source_stages = ConservativeStageMask;
        destination_stages = ConservativeStageMask;
        constexpr vk::AccessFlags FullMemoryAccess{ConservativeAccessMask};
        for (auto& barrier : memory_barriers) {
            barrier.srcAccessMask = FullMemoryAccess;
            barrier.dstAccessMask = FullMemoryAccess;
        }
        for (auto& barrier : buffer_barriers) {
            barrier.srcAccessMask = FullMemoryAccess;
            barrier.dstAccessMask = FullMemoryAccess;
        }
        for (auto& barrier : image_barriers) {
            barrier.srcAccessMask = FullMemoryAccess;
            barrier.dstAccessMask = FullMemoryAccess;
        }
    }

    source_stages = LegacySourceStages(source_stages, has_barriers);
    destination_stages = LegacyDestinationStages(destination_stages, has_barriers);
    command_buffer.pipelineBarrier(
        vk::PipelineStageFlags{source_stages}, vk::PipelineStageFlags{destination_stages},
        dependency_info.dependencyFlags, static_cast<std::uint32_t>(memory_barriers.size()),
        memory_barriers.data(), static_cast<std::uint32_t>(buffer_barriers.size()),
        buffer_barriers.data(), static_cast<std::uint32_t>(image_barriers.size()),
        image_barriers.data());
}

} // namespace Vulkan

