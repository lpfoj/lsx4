// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstddef>
#include <optional>
#include "common/assert.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace Vulkan {

ResourcePool::ResourcePool(MasterSemaphore* master_semaphore_, std::size_t grow_step_)
    : master_semaphore{master_semaphore_}, grow_step{grow_step_} {}

std::size_t ResourcePool::CommitResource() {
    u64 gpu_tick = master_semaphore->KnownGpuTick();
    const auto search = [this, gpu_tick](std::size_t begin,
                                         std::size_t end) -> std::optional<std::size_t> {
        for (std::size_t iterator = begin; iterator < end; ++iterator) {
            if (gpu_tick >= ticks[iterator]) {
                ticks[iterator] = master_semaphore->CurrentTick();
                return iterator;
            }
        }
        return std::nullopt;
    };

    auto found = search(hint_iterator, ticks.size());
    if (!found) {
        master_semaphore->Refresh();
        gpu_tick = master_semaphore->KnownGpuTick();
        found = search(hint_iterator, ticks.size());
    }
    if (!found) {
        found = search(0, hint_iterator);
        if (!found) {
            const std::size_t free_resource = ManageOverflow();

            ticks[free_resource] = master_semaphore->CurrentTick();
            found = free_resource;
        }
    }

    hint_iterator = (*found + 1) % ticks.size();
    return *found;
}

std::size_t ResourcePool::ManageOverflow() {
    const std::size_t old_capacity = ticks.size();
    ticks.resize(old_capacity + grow_step);
    Allocate(old_capacity, old_capacity + grow_step);
    return old_capacity;
}

constexpr std::size_t COMMAND_BUFFER_POOL_SIZE = 4;

CommandPool::CommandPool(const Instance& instance, MasterSemaphore* master_semaphore,
                         const vk::CommandBufferLevel level_)
    : ResourcePool{master_semaphore, COMMAND_BUFFER_POOL_SIZE}, instance{instance},
      level{level_},
      trace_exact_turnip_a750{
          instance.GetCapabilityProfile().exact_mesa_turnip_adreno750} {
    const vk::CommandPoolCreateInfo pool_create_info = {
        .flags = vk::CommandPoolCreateFlagBits::eTransient |
                 vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex(),
    };
    const vk::Device device = instance.GetDevice();
    auto [pool_result, pool] = device.createCommandPoolUnique(pool_create_info);
    ASSERT_MSG(pool_result == vk::Result::eSuccess, "Failed to create command pool: {}",
               vk::to_string(pool_result));
    cmd_pool = std::move(pool);
    SetObjectName(device, *cmd_pool, "CommandPool");
}

CommandPool::~CommandPool() = default;

void CommandPool::Allocate(std::size_t begin, std::size_t end) {
    cmd_buffers.resize(end);
    ++allocation_batches;

    const vk::CommandBufferAllocateInfo buffer_alloc_info = {
        .commandPool = *cmd_pool,
        .level = level,
        .commandBufferCount = COMMAND_BUFFER_POOL_SIZE,
    };

    const vk::Device device = instance.GetDevice();
    const auto result =
        device.allocateCommandBuffers(&buffer_alloc_info, cmd_buffers.data() + begin);
    ASSERT(result == vk::Result::eSuccess);

    for (std::size_t i = begin; i < end; ++i) {
        SetObjectName(device, cmd_buffers[i], "CommandPool: Command Buffer {}", i);
    }
#ifdef __ANDROID__
    // Diagnostic-only O(1) locator for the A750 Turnip residency frontier.
    // Log initial growth and then a sparse geometric sample: no command
    // ordering, reuse rule or completion boundary changes here. A840 and Mali
    // never enter this exact resolved route.
    const bool sparse_sample = allocation_batches <= 16 ||
        (allocation_batches & (allocation_batches - 1)) == 0;
    if (trace_exact_turnip_a750 && sparse_sample) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_A750_COMMAND_POOL] pool=%p level=%u batches=%llu "
            "buffers=%zu begin=%zu end=%zu knownGpuTick=%llu currentTick=%llu",
            static_cast<const void*>(this), static_cast<unsigned>(level),
            static_cast<unsigned long long>(allocation_batches),
            cmd_buffers.size(), begin, end,
            static_cast<unsigned long long>(master_semaphore->KnownGpuTick()),
            static_cast<unsigned long long>(master_semaphore->CurrentTick()));
    }
#endif
}

vk::CommandBuffer CommandPool::Commit() {
    const std::size_t index = CommitResource();
    return cmd_buffers[index];
}

DescriptorHeap::DescriptorHeap(const Instance& instance, MasterSemaphore* master_semaphore_,
                               std::span<const vk::DescriptorPoolSize> pool_sizes_,
                               u32 descriptor_heap_count_)
    : device{instance.GetDevice()}, master_semaphore{master_semaphore_},
      descriptor_heap_count{descriptor_heap_count_},
      descriptor_set_batch{instance.GetCapabilityProfile()
                               .descriptor_binding.legacy_allocation_batch},
      trace_exact_qti_a750{instance.GetCapabilityProfile().exact_proprietary_adreno750},
      pool_sizes{pool_sizes_} {
    ASSERT(descriptor_set_batch != 0 &&
           descriptor_set_batch <= DescriptorSetBatchCapacity);
    CreateDescriptorPool();
#ifdef __ANDROID__
    if (trace_exact_qti_a750) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_A750_DESCRIPTOR_HEAP] action=config batch=%u maxSets=%u",
            descriptor_set_batch, descriptor_heap_count);
    }
#endif
}

DescriptorHeap::~DescriptorHeap() {
    device.destroyDescriptorPool(curr_pool);
    for (const auto [pool, tick] : pending_pools) {
        master_semaphore->Wait(tick);
        device.destroyDescriptorPool(pool);
    }
}

vk::DescriptorSet DescriptorHeap::Commit(vk::DescriptorSetLayout set_layout) {
    ++commit_count;
    const u64 set_key = std::bit_cast<u64>(set_layout);
    const auto [it, _] = descriptor_sets.try_emplace(set_key);

    if (!it->second.empty()) {
        ++cached_set_hits;
        const auto desc_set = it->second.back();
        it.value().pop_back();
        return desc_set;
    }

    DescSetBatch desc_sets(descriptor_set_batch);
    std::array<vk::DescriptorSetLayout, DescriptorSetBatchCapacity> layouts;
    layouts.fill(set_layout);

    vk::DescriptorSetAllocateInfo alloc_info = {
        .descriptorPool = curr_pool,
        .descriptorSetCount = descriptor_set_batch,
        .pSetLayouts = layouts.data(),
    };

    auto result = device.allocateDescriptorSets(&alloc_info, desc_sets.data());
    if (result == vk::Result::eSuccess) {
        ++allocation_batches;
        const auto desc_set = desc_sets.back();
        desc_sets.pop_back();
        it.value() = std::move(desc_sets);
        return desc_set;
    }

    ASSERT_MSG(result == vk::Result::eErrorOutOfPoolMemory ||
                   result == vk::Result::eErrorFragmentedPool,
               "Unexpected error during descriptor set allocation: {}", vk::to_string(result));
    pending_pools.emplace_back(curr_pool, master_semaphore->CurrentTick());
    ++pool_rollovers;
    peak_pending_pools = std::max<u64>(peak_pending_pools, pending_pools.size());
    bool recycled_pool = false;
    if (const auto [pool, tick] = pending_pools.front(); master_semaphore->IsFree(tick)) {
        curr_pool = pool;
        pending_pools.pop_front();

        const auto reset_result = device.resetDescriptorPool(curr_pool);
        ASSERT_MSG(reset_result == vk::Result::eSuccess,
                   "Unexpected error resetting descriptor pool: {}", vk::to_string(reset_result));
        ++pool_resets;
        recycled_pool = true;
    } else {
        CreateDescriptorPool();
    }
#ifdef __ANDROID__
    if (trace_exact_qti_a750) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_A750_DESCRIPTOR_HEAP] action=rollover batch=%u commits=%llu "
            "cachedHits=%llu allocationBatches=%llu rollovers=%llu resets=%llu "
            "created=%llu pending=%zu peakPending=%llu recycled=%u",
            descriptor_set_batch, static_cast<unsigned long long>(commit_count),
            static_cast<unsigned long long>(cached_set_hits),
            static_cast<unsigned long long>(allocation_batches),
            static_cast<unsigned long long>(pool_rollovers),
            static_cast<unsigned long long>(pool_resets),
            static_cast<unsigned long long>(pools_created), pending_pools.size(),
            static_cast<unsigned long long>(peak_pending_pools), recycled_pool ? 1u : 0u);
    }
#endif

    alloc_info.descriptorPool = curr_pool;
    result = device.allocateDescriptorSets(&alloc_info, desc_sets.data());
    ASSERT_MSG(result == vk::Result::eSuccess,
               "Unexpected error during descriptor set allocation {}", vk::to_string(result));
    ++allocation_batches;

    descriptor_sets.clear();
    const auto desc_set = desc_sets.back();
    desc_sets.pop_back();
    descriptor_sets[set_key] = std::move(desc_sets);
    return desc_set;
}

void DescriptorHeap::CreateDescriptorPool() {
    const vk::DescriptorPoolCreateInfo pool_info = {
        .flags = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind,
        .maxSets = descriptor_heap_count,
        .poolSizeCount = static_cast<u32>(pool_sizes.size()),
        .pPoolSizes = pool_sizes.data(),
    };
    auto [pool_result, pool] = device.createDescriptorPool(pool_info);
    ASSERT_MSG(pool_result == vk::Result::eSuccess, "Failed to create descriptor pool: {}",
               vk::to_string(pool_result));
    curr_pool = pool;
    ++pools_created;
}

}
