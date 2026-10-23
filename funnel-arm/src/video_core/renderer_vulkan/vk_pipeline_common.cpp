// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/static_vector.hpp>

#include <atomic>
#include <bit>
#include <cstring>
#include <optional>
#include <type_traits>

#include "common/content_fingerprint.h"
#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/mobile_gpu_optimization.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_driver_optimization.h"

#ifdef __ANDROID__
#include <android/log.h>
#include <unistd.h>
#endif

namespace Vulkan {
namespace {

std::atomic<u64> next_pipeline_identity{};

u64 AllocatePipelineIdentity() noexcept {
    return next_pipeline_identity.fetch_add(1, std::memory_order_relaxed) + 1;
}

std::optional<u64> DescriptorWritesSignature(const Pipeline::DescriptorWrites& writes) {
    Common::ContentFingerprint64 fingerprint{Common::FingerprintDomain::DescriptorWrites};
    const auto mix = [&fingerprint](const u64 value) {
        fingerprint.UpdateLittleEndian(value);
    };
    mix(writes.size());
    for (const auto& write : writes) {
        const u32 pointer_sources =
            static_cast<u32>(write.pBufferInfo != nullptr) +
            static_cast<u32>(write.pImageInfo != nullptr) +
            static_cast<u32>(write.pTexelBufferView != nullptr);
        // Extension descriptors carry their payload in pNext. Never elide a
        // bind unless all semantic descriptor bytes are represented here.
        if (write.pNext != nullptr || write.descriptorCount == 0 ||
            pointer_sources != 1) {
            return std::nullopt;
        }
        mix(write.dstBinding);
        mix(write.dstArrayElement);
        mix(write.descriptorCount);
        mix(static_cast<u32>(write.descriptorType));
        for (u32 index = 0; index < write.descriptorCount; ++index) {
            if (write.pBufferInfo != nullptr) {
                const auto& info = write.pBufferInfo[index];
                mix(std::bit_cast<u64>(static_cast<VkBuffer>(info.buffer)));
                mix(info.offset);
                mix(info.range);
            } else if (write.pImageInfo != nullptr) {
                const auto& info = write.pImageInfo[index];
                mix(std::bit_cast<u64>(static_cast<VkSampler>(info.sampler)));
                mix(std::bit_cast<u64>(static_cast<VkImageView>(info.imageView)));
                mix(static_cast<u32>(info.imageLayout));
            } else if (write.pTexelBufferView != nullptr) {
                mix(std::bit_cast<u64>(
                    static_cast<VkBufferView>(write.pTexelBufferView[index])));
            }
        }
    }
    return fingerprint.Finish();
}

struct DescriptorBindCache {
    u64 pipeline_identity{};
    u64 cmdbuf{};
    u64 recording_tick{};
    u64 state_epoch{};
    u64 signature{};
    bool valid{};
};

struct PushConstantBindCache {
    u64 pipeline_identity{};
    u64 cmdbuf{};
    u64 recording_tick{};
    u64 state_epoch{};
    Shader::PushData data{};
    bool valid{};
};

static_assert(std::is_trivially_copyable_v<Shader::PushData>);

#ifdef __ANDROID__
bool ExecutorVulkanOptimizationStatsEnabled() {
    static const bool enabled =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-diagnostic-vulkan-optimization-stats",
                 F_OK) == 0;
    return enabled;
}
#endif

}

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      identity{AllocatePipelineIdentity()}, is_compute{is_compute_} {}

Pipeline::~Pipeline() = default;

void Pipeline::BindResources(DescriptorWrites& set_writes, const BufferBarriers& buffer_barriers,
                             const Shader::PushData& push_data) const {
    const auto cmdbuf = scheduler.CommandBuffer();

    if (!buffer_barriers.empty()) {
        const auto dependencies = vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = u32(buffer_barriers.size()),
            .pBufferMemoryBarriers = buffer_barriers.data(),
        };
        scheduler.EndRendering(Scheduler::EndRenderingReason::PipelineResourceBarrier);
        cmdbuf.pipelineBarrier2(dependencies);
    }
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;

    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    const u64 raw_cmdbuf =
        std::bit_cast<u64>(static_cast<VkCommandBuffer>(cmdbuf));
    const u64 recording_tick = scheduler.CurrentTick();
    const bool descriptor_dedup_enabled =
        VideoCore::IsManagedVulkanDescriptorDeduplication(
            instance.GetCapabilityProfile()
                .descriptor_binding.allow_exact_write_dedup_ab);
    const bool push_dedup_enabled =
        VideoCore::IsManagedVulkanPushConstantDeduplication();
    static thread_local std::array<PushConstantBindCache, 2> push_caches{};
    auto& push_cache = push_caches[IsCompute() ? 1u : 0u];
    const u64 push_epoch = scheduler.PushConstantEpoch(bind_point);
    const bool push_hit =
        push_dedup_enabled &&
        push_cache.valid && push_cache.pipeline_identity == identity &&
        push_cache.cmdbuf == raw_cmdbuf &&
        push_cache.recording_tick == recording_tick &&
        push_cache.state_epoch == push_epoch &&
        std::memcmp(&push_cache.data, &push_data, sizeof(push_data)) == 0;
    if (!push_hit) {
        cmdbuf.pushConstants(*pipeline_layout, stage_flags, 0u, sizeof(push_data), &push_data);
        const u64 post_push_epoch = scheduler.MarkPushConstants(bind_point);
        push_cache = {
            .pipeline_identity = identity,
            .cmdbuf = raw_cmdbuf,
            .recording_tick = recording_tick,
            .state_epoch = post_push_epoch,
            .data = push_data,
            .valid = true,
        };
    }
    VideoCore::RecordVulkanPushConstants(push_hit);
#ifdef __ANDROID__
    if (ExecutorVulkanOptimizationStatsEnabled()) {
        static thread_local u64 push_attempts{};
        static thread_local u64 push_hits{};
        ++push_attempts;
        push_hits += static_cast<u64>(push_hit);
        if ((push_attempts & 8191u) == 0 && !VideoCore::IsExecutorLiveQuiet()) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_VK_STATE_DEDUP] kind=pushConstants attempts=%llu hits=%llu hitPct=%.1f",
                static_cast<unsigned long long>(push_attempts),
                static_cast<unsigned long long>(push_hits),
                100.0 * static_cast<double>(push_hits) /
                    static_cast<double>(push_attempts));
        }
    }
#endif

    if (set_writes.empty()) {
        return;
    }

    static thread_local std::array<DescriptorBindCache, 2> bind_caches{};
    auto& bind_cache = bind_caches[IsCompute() ? 1u : 0u];
    const std::optional<u64> descriptor_signature =
        descriptor_dedup_enabled ? DescriptorWritesSignature(set_writes) : std::nullopt;
    const u64 descriptor_epoch = scheduler.DescriptorBindEpoch(bind_point);
    const bool descriptor_hit =
        descriptor_dedup_enabled &&
        descriptor_signature.has_value() &&
        bind_cache.valid && bind_cache.pipeline_identity == identity &&
        bind_cache.cmdbuf == raw_cmdbuf &&
        bind_cache.recording_tick == recording_tick &&
        bind_cache.state_epoch == descriptor_epoch &&
        bind_cache.signature == *descriptor_signature;
    VideoCore::RecordVulkanDescriptorBind(descriptor_hit);
    if (descriptor_hit) {
        return;
    }

    if (uses_push_descriptors) {
        cmdbuf.pushDescriptorSetKHR(bind_point, *pipeline_layout, 0, set_writes);
        const u64 post_descriptor_epoch = scheduler.MarkDescriptorBind(bind_point);
        bind_cache = {
            .pipeline_identity = identity,
            .cmdbuf = raw_cmdbuf,
            .recording_tick = recording_tick,
            .state_epoch = post_descriptor_epoch,
            .signature = descriptor_signature.value_or(0),
            .valid = true,
        };
        return;
    }

    const auto desc_set = desc_heap.Commit(*desc_layout);
    for (auto& set_write : set_writes) {
        set_write.dstSet = desc_set;
    }
    instance.GetDevice().updateDescriptorSets(set_writes, {});
    cmdbuf.bindDescriptorSets(bind_point, *pipeline_layout, 0, desc_set, {});
    const u64 post_descriptor_epoch = scheduler.MarkDescriptorBind(bind_point);
    bind_cache = {
        .pipeline_identity = identity,
        .cmdbuf = raw_cmdbuf,
        .recording_tick = recording_tick,
        .state_epoch = post_descriptor_epoch,
        .signature = descriptor_signature.value_or(0),
        .valid = true,
    };
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

}
