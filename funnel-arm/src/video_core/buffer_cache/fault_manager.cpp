#if defined(LSX4_APPLE_RUNTIME)
#include "staged_work_map.h"
#include "video_core/renderer_vulkan/vk_capability_profile.h"
#endif
// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/div_ceil.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

#include "video_core/host_shaders/fault_buffer_process_comp.h"

namespace VideoCore {

#if defined(LSX4_APPLE_RUNTIME)
namespace {
using StagedFault = std::pair<VAddr, u64>;
Lsx4::Portable::StagedWorkMap<FaultManager*, StagedFault> staged_faults;
}
#endif
static constexpr size_t MaxPageFaults = 1024;
static constexpr size_t PageFaultAreaSize = MaxPageFaults * sizeof(u64);

FaultManager::FaultManager(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler_,
                           BufferCache& buffer_cache_, u32 caching_pagebits, u64 caching_num_pages_)
    : scheduler{scheduler_}, buffer_cache{buffer_cache_},
      caching_pagesize{1ULL << caching_pagebits}, caching_num_pages{caching_num_pages_},
      fault_buffer_size{((caching_num_pages_ / 32) +
                         Common::DivCeil(caching_num_pages_ / 32, 32ULL)) *
                        sizeof(u32)},
      fault_buffer{instance, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, fault_buffer_size},
      download_buffer{instance, scheduler, MemoryUsage::Download,
                      0,        AllFlags,  MaxPendingFaults * PageFaultAreaSize} {
#if defined(LSX4_APPLE_RUNTIME)
    staged_faults.Reset(this, Vulkan::ExactDirtyUploadRetirementWatermark(
        instance.GetCapabilityProfile().memory_policy) != 0);
#endif
    const auto device = instance.GetDevice();
    Vulkan::SetObjectName(device, fault_buffer.Handle(), "Fault Buffer");

    const std::array<vk::DescriptorSetLayoutBinding, 2> bindings = {{
        {
            .binding = 0,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
    }};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = instance.MaxPushDescriptors() != 0
                     ? vk::DescriptorSetLayoutCreateFlags{
                           vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR}
                     : vk::DescriptorSetLayoutCreateFlags{},
        .bindingCount = 2,
        .pBindings = bindings.data(),
    };
    fault_process_desc_layout =
        Vulkan::Check(device.createDescriptorSetLayoutUnique(desc_layout_ci));

    const u64 fault_words = caching_num_pages / 32;
    const u64 summary_words = Common::DivCeil(fault_words, 32ULL);
    std::vector<std::string> defines{{
        fmt::format("CACHING_PAGEBITS={}", caching_pagebits),
        fmt::format("MAX_PAGE_FAULTS={}", MaxPageFaults),
        fmt::format("FAULT_WORDS={}", fault_words),
        fmt::format("FAULT_SUMMARY_OFFSET={}", fault_words),
        fmt::format("FAULT_SUMMARY_WORDS={}", summary_words),
    }};
    const auto module = Vulkan::Compile(HostShaders::FAULT_BUFFER_PROCESS_COMP,
                                        vk::ShaderStageFlagBits::eCompute, device, defines);
    Vulkan::SetObjectName(device, module, "Fault Buffer Parser");

    const vk::PipelineShaderStageCreateInfo shader_ci = {
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = module,
        .pName = "main",
    };

    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &(*fault_process_desc_layout),
    };
    fault_process_pipeline_layout = Vulkan::Check(device.createPipelineLayoutUnique(layout_info));

    const vk::ComputePipelineCreateInfo pipeline_info = {
        .stage = shader_ci,
        .layout = *fault_process_pipeline_layout,
    };
    fault_process_pipeline = Vulkan::Check(device.createComputePipelineUnique({}, pipeline_info));
    Vulkan::SetObjectName(device, *fault_process_pipeline, "Fault Buffer Parser Pipeline");

    device.destroyShaderModule(module);
}

void FaultManager::ProcessFaultBuffer() {
#if defined(LSX4_APPLE_RUNTIME)
    const auto apply_completed_faults = [&] {
        auto completed = staged_faults.Take(this);
        if (completed.empty()) return;
        fault_ranges.Clear();
        for (const auto& [address, size] : completed) fault_ranges.Add(address, size);
        u64 ranges = 0;
        fault_ranges.ForEach([&](VAddr start, VAddr end) {
            ASSERT_MSG((end - start) <= std::numeric_limits<u32>::max(),
                       "Buffer size is too large");
            buffer_cache.FindBuffer(start, static_cast<u32>(end - start));
            ++ranges;
        });
        static std::atomic<u64> batches{};
        const auto n = batches.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8 || (n & (n-1)) == 0)
            __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                "[APPLE_STAGED_FAULT_CACHE] batch=%llu pages=%llu ranges=%llu",
                (unsigned long long)n, (unsigned long long)completed.size(),
                (unsigned long long)ranges);
    };
    // Rasterizer's submission boundary is outside descriptor/buffer traversal
    // and outside Scheduler::SubmitExecution's lock. Never mutate there from
    // completion callbacks executed during an inner consuming upload.
    apply_completed_faults();
#endif
    if (u64 wait_tick = fault_areas[current_area]) {
        scheduler.Wait(wait_tick);
        scheduler.PopPendingOperations(true, true);
    }

#if defined(LSX4_APPLE_RUNTIME)
    // Waiting above can publish another completed fault batch.
    apply_completed_faults();
#endif
    const u32 offset = current_area * PageFaultAreaSize;
    u8* mapped = download_buffer.mapped_data.data() + offset;
    std::memset(mapped, 0, PageFaultAreaSize);

    const vk::BufferMemoryBarrier2 pre_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        // Guest shaders update the fault bitset with atomicOr and the parser
        // consumes it with atomicExchange. Both are read-modify-write accesses,
        // so advertising only one half leaves the atomic hazard unordered on
        // tile-based mobile drivers.
        .srcAccessMask = vk::AccessFlagBits2::eShaderRead |
                         vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead |
                         vk::AccessFlagBits2::eShaderWrite,
        .buffer = fault_buffer.Handle(),
        .offset = 0,
        .size = fault_buffer_size,
    };
    const vk::BufferMemoryBarrier2 post_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderRead |
                         vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead |
                         vk::AccessFlagBits2::eShaderWrite,
        .buffer = fault_buffer.Handle(),
        .offset = 0,
        .size = fault_buffer_size,
    };
    const vk::DescriptorBufferInfo fault_buffer_info = {
        .buffer = fault_buffer.Handle(),
        .offset = 0,
        .range = fault_buffer_size,
    };
    const vk::DescriptorBufferInfo download_info = {
        .buffer = download_buffer.Handle(),
        .offset = offset,
        .range = PageFaultAreaSize,
    };
    const std::array<vk::WriteDescriptorSet, 2> writes = {{
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &fault_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &download_info,
        },
    }};
    scheduler.EndRendering(Vulkan::Scheduler::EndRenderingReason::FaultCompute);
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_barrier,
    });
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *fault_process_pipeline);
    scheduler.MarkPipelineBind(vk::PipelineBindPoint::eCompute);
    scheduler.BindHostDescriptors(cmdbuf, vk::PipelineBindPoint::eCompute,
                                  *fault_process_pipeline_layout, *fault_process_desc_layout,
                                  writes);
    scheduler.MarkDescriptorBind(vk::PipelineBindPoint::eCompute);
    const u32 num_threads = caching_num_pages / 32;
    const u32 num_workgroups = Common::DivCeil(num_threads, 64u);
    cmdbuf.dispatch(num_workgroups, 1, 1);

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post_barrier,
    });

    scheduler.DeferOperation([this, mapped, area = current_area] {
#if defined(LSX4_APPLE_RUNTIME)
        const u64* completed = std::bit_cast<const u64*>(mapped);
        const u32 completed_count = std::min<u32>(completed[0], MaxPageFaults - 1);
        std::vector<StagedFault> ranges;
        ranges.reserve(completed_count);
        for (u32 i = 1; i <= completed_count; ++i)
            ranges.emplace_back(completed[i], caching_pagesize);
        if (staged_faults.Publish(this, std::move(ranges))) {
            // The CPU owns this completed download area; copying its records
            // before release permits ring reuse without retaining mapped data.
            fault_areas[area] = 0;
            return;
        }
#endif
        fault_ranges.Clear();
        const u64* fault_buf = std::bit_cast<const u64*>(mapped);
        const u32 fault_count =
            std::min<u32>(fault_buf[0], static_cast<u32>(MaxPageFaults - 1));
        for (u32 i = 1; i <= fault_count; ++i) {
            fault_ranges.Add(fault_buf[i], caching_pagesize);
            LOG_INFO(Render_Vulkan, "Accessed non-GPU cached memory at {:#x}", fault_buf[i]);
        }
        fault_ranges.ForEach([&](VAddr start, VAddr end) {
            ASSERT_MSG((end - start) <= std::numeric_limits<u32>::max(),
                       "Buffer size is too large");
            buffer_cache.FindBuffer(start, static_cast<u32>(end - start));
        });
        fault_areas[area] = 0;
    });

    fault_areas[current_area++] = scheduler.CurrentTick();
    current_area %= MaxPendingFaults;
}

}
