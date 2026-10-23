// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/tile_manager.h"
#include "video_core/mobile_gpu_optimization.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
#include "core/memory.h"
#if defined(__ANDROID__)
#include <android/log.h>
#include <unistd.h>
#endif

#include "video_core/host_shaders/tiling_comp.h"

#include <magic_enum/magic_enum.hpp>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <iterator>
#include <list>
#include <memory>
#include <map>
#include <cstdio>
#include <cstdlib>
#include "tiling_pipeline_identity.h"
#include <cmath>
#include <tuple>
#include "video_core/texture_cache/tile.h"
#include "mixed_mip_layout.h"
#include "mixed_mip_shader.h"
#include <mutex>
#include <unordered_map>
#include <vector>

namespace VideoCore {

struct TilingInfo {
    u32 bank_swizzle;
    u32 num_slices;
    u32 num_mips;
    std::array<ImageInfo::MipInfo, 16> mips;
    u32 buffer_elements;
    u32 micro_tiled_mips;
};
static_assert(offsetof(TilingInfo, mips) == 12);
static_assert(offsetof(TilingInfo, buffer_elements) == 268);
static_assert(offsetof(TilingInfo, micro_tiled_mips) == 272);
static_assert(sizeof(TilingInfo) == 276);

namespace {

u32 MixedMipMask(const ImageInfo& info) {
    if (info.array_mode != AmdGpu::ArrayMode::Array2DTiledThin1 &&
        info.array_mode != AmdGpu::ArrayMode::Array2DTiledThick) return 0;
    const auto [w,h] = GetMacroTileExtents(info.tile_mode,info.num_bits,
                                        info.num_samples,info.alt_tile);
    return PortableGpu::MixedMacroMipMask(info.pitch,info.size.height,
        info.resources.levels,w,h,info.props.is_block);
}

constexpr u64 ScratchPoolMaximumBudgetBytes = 128ull * 1024ull * 1024ull;
constexpr u64 ScratchPoolMinimumBucketBytes = 4ull * 1024ull;

bool ExecutorTileScratchStatsEnabled() {
#ifdef __ANDROID__
    static const bool enabled =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-diagnostic-tile-scratch-stats",
                 F_OK) == 0;
    return enabled;
#else
    return false;
#endif
}

u64 ScratchBucketSize(const u32 requested_size) noexcept {
    if (requested_size == 0) {
        return 0;
    }
    const u64 normalized =
        std::max<u64>(requested_size, ScratchPoolMinimumBucketBytes);
    return std::bit_ceil(normalized);
}

} // namespace

struct TileManager::ScratchPoolState {
    // Opaque heap state: public TileManager/ImageInfo layouts stay unchanged.
    std::map<PortableGpu::TilingPipelineIdentity, vk::UniquePipeline> tiling_pipelines_exact;
    struct IdleEntry {
        VkBuffer buffer{VK_NULL_HANDLE};
        VmaAllocation allocation{};
        u64 bucket{};
        u64 capacity{};
        size_t bucket_slot{};
    };

    using IdleIterator = std::list<IdleEntry>::iterator;

    struct BucketState {
        std::vector<IdleIterator> entries;
        u32 miss_pressure{};
    };

    struct ReleasedEntry {
        VkBuffer buffer{VK_NULL_HANDLE};
        VmaAllocation allocation{};
    };

    struct RecordingEpochState {
        std::unordered_map<u64, std::vector<ScratchBuffer>> buckets;
        bool retirement_scheduled{};
    };

    ScratchPoolState(const VmaAllocator allocator_, const u64 budget_bytes_,
                     const bool allow_oversize_recording_reuse_)
        : allocator{allocator_}, budget_bytes{budget_bytes_},
          collect_stats{ExecutorTileScratchStatsEnabled()},
          allow_oversize_recording_reuse{allow_oversize_recording_reuse_} {}

    u64 BucketSize(const u32 requested_size) const noexcept {
        const u64 bucket = ScratchBucketSize(requested_size);
        return allow_oversize_recording_reuse || bucket <= budget_bytes ? bucket : 0;
    }

    u64 AllocationSize(const u32 requested_size, const u64 bucket) const noexcept {
        return bucket != 0 && bucket <= budget_bytes ? bucket : requested_size;
    }

    bool TryAcquire(const u64 bucket, ScratchBuffer& scratch) {
        if (bucket == 0 || bucket > budget_bytes) {
            return false;
        }
        {
            std::scoped_lock lock{mutex};
            if (stopping) {
                return false;
            }
            auto& state = buckets[bucket];
            if (state.entries.empty()) {
                state.miss_pressure =
                    std::min(state.miss_pressure + 1, MaximumMissPressure);
                if (collect_stats) {
                    misses.fetch_add(1, std::memory_order_relaxed);
                }
                return false;
            }

            const IdleIterator idle = state.entries.back();
            state.entries.pop_back();
            scratch = {
                .buffer = vk::Buffer{idle->buffer},
                .allocation = idle->allocation,
                .bucket = bucket,
                .capacity = idle->capacity,
                .reused = true,
                .recording_reused = false,
            };
            idle_buffers.erase(idle);
            pooled_bytes.fetch_sub(bucket, std::memory_order_relaxed);
            pooled_buffers.fetch_sub(1, std::memory_order_relaxed);
            if (state.miss_pressure != 0) {
                --state.miss_pressure;
            }
        }
        if (collect_stats) {
            reused.fetch_add(1, std::memory_order_relaxed);
        }
        MaybeLog("reuse");
        return true;
    }

    bool TryAcquireRecording(const u64 recording_tick, const u64 bucket,
                             const u64 required_capacity, ScratchBuffer& scratch) {
        if (recording_tick == 0 || bucket == 0) {
            return false;
        }

        bool cross_bucket_reuse = false;
        {
            std::scoped_lock lock{mutex};
            if (stopping) {
                return false;
            }
            const auto epoch = recording_epochs.find(recording_tick);
            if (epoch == recording_epochs.end()) {
                return false;
            }
            auto selected_bucket = epoch->second.buckets.end();
            size_t selected_slot = 0;
            u64 selected_capacity = std::numeric_limits<u64>::max();

            const auto exact_bucket = epoch->second.buckets.find(bucket);
            if (exact_bucket != epoch->second.buckets.end()) {
                const auto& entries = exact_bucket->second;
                const auto fit = std::ranges::find_if(
                    entries, [required_capacity](const ScratchBuffer& candidate) {
                        return candidate.capacity >= required_capacity;
                    });
                if (fit != entries.end()) {
                    selected_bucket = exact_bucket;
                    selected_slot = static_cast<size_t>(fit - entries.begin());
                    selected_capacity = fit->capacity;
                }
            }

            // A750 can produce several oversized detile requests in one
            // recording epoch. On an exact-bucket miss, reuse the smallest
            // already ordered buffer from the next size class instead of
            // allocating another standalone VkBuffer. This path is disabled
            // for every other device and never crosses a submit boundary.
            if (selected_bucket == epoch->second.buckets.end() &&
                allow_oversize_recording_reuse) {
                const u64 maximum_bucket =
                    bucket > std::numeric_limits<u64>::max() / 2
                        ? std::numeric_limits<u64>::max()
                        : bucket * 2;
                for (auto candidate_bucket = epoch->second.buckets.begin();
                     candidate_bucket != epoch->second.buckets.end();
                     ++candidate_bucket) {
                    if (candidate_bucket->first <= bucket ||
                        candidate_bucket->first > maximum_bucket) {
                        continue;
                    }
                    const auto& entries = candidate_bucket->second;
                    for (size_t slot = 0; slot < entries.size(); ++slot) {
                        const ScratchBuffer& candidate = entries[slot];
                        if (candidate.capacity >= required_capacity &&
                            candidate.capacity < selected_capacity) {
                            selected_bucket = candidate_bucket;
                            selected_slot = slot;
                            selected_capacity = candidate.capacity;
                        }
                    }
                }
                cross_bucket_reuse =
                    selected_bucket != epoch->second.buckets.end();
            }
            if (selected_bucket == epoch->second.buckets.end()) {
                return false;
            }

            auto& entries = selected_bucket->second;
            scratch = entries[selected_slot];
            entries[selected_slot] = entries.back();
            entries.pop_back();
            if (entries.empty()) {
                epoch->second.buckets.erase(selected_bucket);
            }
            scratch.recording_reused = true;
        }
        if (collect_stats) {
            recording_reused.fetch_add(1, std::memory_order_relaxed);
            if (cross_bucket_reuse) {
                recording_cross_reused.fetch_add(1, std::memory_order_relaxed);
            }
        }
        MaybeLog(cross_bucket_reuse ? "recording_cross_reuse" : "recording_reuse");
        return true;
    }

    bool RetireForRecording(const u64 recording_tick, const ScratchBuffer& scratch,
                            bool& schedule_retirement) {
        schedule_retirement = false;
        if (recording_tick == 0 || scratch.bucket == 0) {
            return false;
        }

        try {
            std::scoped_lock lock{mutex};
            if (stopping) {
                return false;
            }
            auto& epoch = recording_epochs[recording_tick];
            epoch.buckets[scratch.bucket].push_back(scratch);
            if (!epoch.retirement_scheduled) {
                epoch.retirement_scheduled = true;
                schedule_retirement = true;
            }
        } catch (...) {
            return false;
        }
        if (collect_stats) {
            recording_retired.fetch_add(1, std::memory_order_relaxed);
        }
        MaybeLog(schedule_retirement ? "recording_retire_schedule" : "recording_retire");
        return true;
    }

    void ReleaseRecordingEpoch(const u64 recording_tick) {
        std::vector<ScratchBuffer> released;
        {
            std::scoped_lock lock{mutex};
            const auto epoch = recording_epochs.find(recording_tick);
            if (epoch == recording_epochs.end()) {
                return;
            }
            size_t entry_count = 0;
            for (const auto& [bucket, entries] : epoch->second.buckets) {
                (void)bucket;
                entry_count += entries.size();
            }
            released.reserve(entry_count);
            for (auto& [bucket, entries] : epoch->second.buckets) {
                (void)bucket;
                released.insert(released.end(),
                                std::make_move_iterator(entries.begin()),
                                std::make_move_iterator(entries.end()));
            }
            recording_epochs.erase(epoch);
        }

        for (const ScratchBuffer& scratch : released) {
            if (!TryPool(scratch)) {
                vmaDestroyBuffer(allocator, static_cast<VkBuffer>(scratch.buffer),
                                 scratch.allocation);
            }
        }
        MaybeLog("recording_epoch_release");
    }

    bool TryPool(const ScratchBuffer& scratch) {
        const u64 bucket = scratch.bucket;
        if (bucket == 0 || bucket > budget_bytes) {
            if (collect_stats) {
                rejected.fetch_add(1, std::memory_order_relaxed);
            }
            MaybeLog("destroy_unpooled");
            return false;
        }

        std::vector<ReleasedEntry> evicted_entries;
        bool admitted = false;
        try {
            std::scoped_lock lock{mutex};
            if (!stopping) {
                auto& target = buckets[bucket];
                u64 current_bytes = pooled_bytes.load(std::memory_order_relaxed);
                bool may_admit = true;

                if (current_bytes > budget_bytes - bucket) {
                    const u64 bytes_needed = current_bytes - (budget_bytes - bucket);
                    const u64 same_bucket_bytes =
                        static_cast<u64>(target.entries.size()) * bucket;
                    const u64 evictable_bytes = current_bytes - same_bucket_bytes;
                    const u32 admission_misses = RequiredAdmissionMisses(bucket);

                    // Do not let a one-shot large texture flush a warmed cache. A
                    // buffer acquired from this pool always gets its slot back;
                    // newly created size classes must demonstrate repeated misses.
                    may_admit =
                        evictable_bytes >= bytes_needed &&
                        (scratch.reused || target.miss_pressure >= admission_misses);

                    if (may_admit) {
                        evicted_entries.reserve(8);
                        while (current_bytes > budget_bytes - bucket) {
                            const auto oldest = std::ranges::find_if(
                                idle_buffers,
                                [bucket](const IdleEntry& entry) {
                                    return entry.bucket != bucket;
                                });
                            if (oldest == idle_buffers.end()) {
                                may_admit = false;
                                break;
                            }

                            const u64 evicted_bucket = oldest->bucket;
                            evicted_entries.push_back(
                                {oldest->buffer, oldest->allocation});
                            RemoveIdle(oldest);
                            current_bytes -= evicted_bucket;
                            pooled_bytes.store(current_bytes,
                                               std::memory_order_relaxed);
                            pooled_buffers.fetch_sub(1,
                                                     std::memory_order_relaxed);
                        }
                        if (may_admit && !scratch.reused) {
                            target.miss_pressure -= admission_misses;
                        }
                    }
                }

                if (may_admit) {
                    idle_buffers.push_back({
                        .buffer = static_cast<VkBuffer>(scratch.buffer),
                        .allocation = scratch.allocation,
                        .bucket = bucket,
                        .capacity = scratch.capacity,
                        .bucket_slot = target.entries.size(),
                    });
                    auto idle = idle_buffers.end();
                    --idle;
                    try {
                        target.entries.push_back(idle);
                    } catch (...) {
                        idle_buffers.erase(idle);
                        throw;
                    }
                    pooled_bytes.store(current_bytes + bucket,
                                       std::memory_order_relaxed);
                    pooled_buffers.fetch_add(1, std::memory_order_relaxed);
                    admitted = true;
                }
            }
        } catch (...) {
        }

        for (const ReleasedEntry& entry : evicted_entries) {
            vmaDestroyBuffer(allocator, entry.buffer, entry.allocation);
        }
        if (collect_stats && !evicted_entries.empty()) {
            evicted.fetch_add(evicted_entries.size(), std::memory_order_relaxed);
        }
        if (collect_stats && !admitted) {
            rejected.fetch_add(1, std::memory_order_relaxed);
        }
        MaybeLog(admitted ? (evicted_entries.empty() ? "retire_to_pool"
                                                     : "retire_with_evict")
                          : "destroy_rejected");
        return admitted;
    }

    void NoteCreated() {
        if (collect_stats) {
            created.fetch_add(1, std::memory_order_relaxed);
        }
        MaybeLog("create");
    }

    void StopAndDestroy() {
        std::list<IdleEntry> retired_entries;
        {
            std::scoped_lock lock{mutex};
            stopping = true;
            retired_entries.splice(retired_entries.end(), idle_buffers);
            buckets.clear();
            pooled_bytes.store(0, std::memory_order_relaxed);
            pooled_buffers.store(0, std::memory_order_relaxed);

            // Recording-epoch entries can still be referenced by an
            // in-flight command buffer. Their already scheduled timeline
            // callback retains this state and destroys them after retirement.
            // On a terminal device loss, leaking them until allocator teardown
            // is safer than destroying storage still owned by the queue.
        }
        for (const IdleEntry& entry : retired_entries) {
            vmaDestroyBuffer(allocator, entry.buffer, entry.allocation);
        }
    }

    void MaybeLog(const char* event) {
#ifdef __ANDROID__
        if (!collect_stats) {
            return;
        }
        const u64 sequence = log_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        if (sequence != 1 && (sequence & 255u) != 0) {
            return;
        }
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_TILE_SCRATCH_POOL] event=%s created=%llu reused=%llu "
            "pooledBytes=%llu budgetBytes=%llu misses=%llu evicted=%llu "
            "rejected=%llu pooledBuffers=%llu recordingReused=%llu "
            "recordingCrossReused=%llu recordingRetired=%llu",
            event,
            static_cast<unsigned long long>(created.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(reused.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(pooled_bytes.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(budget_bytes),
            static_cast<unsigned long long>(misses.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(evicted.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(rejected.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                pooled_buffers.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                recording_reused.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                recording_cross_reused.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                recording_retired.load(std::memory_order_relaxed)));
#else
        (void)event;
#endif
    }

    u32 RequiredAdmissionMisses(const u64 bucket) const noexcept {
        const u64 admission_unit =
            std::max(budget_bytes / 8, ScratchPoolMinimumBucketBytes);
        const u64 proportional_misses =
            (bucket + admission_unit - 1) / admission_unit;
        return static_cast<u32>(std::clamp<u64>(proportional_misses, 2, 8));
    }

    void RemoveIdle(const IdleIterator idle) {
        auto& state = buckets.at(idle->bucket);
        const size_t slot = idle->bucket_slot;
        const size_t last_slot = state.entries.size() - 1;
        if (slot != last_slot) {
            state.entries[slot] = state.entries[last_slot];
            state.entries[slot]->bucket_slot = slot;
        }
        state.entries.pop_back();
        idle_buffers.erase(idle);
    }

    static constexpr u32 MaximumMissPressure = 255;

    VmaAllocator allocator{};
    u64 budget_bytes{};
    std::mutex mutex;
    std::list<IdleEntry> idle_buffers;
    std::unordered_map<u64, BucketState> buckets;
    std::unordered_map<u64, RecordingEpochState> recording_epochs;
    std::atomic<u64> created{};
    std::atomic<u64> reused{};
    std::atomic<u64> misses{};
    std::atomic<u64> evicted{};
    std::atomic<u64> rejected{};
    std::atomic<u64> pooled_buffers{};
    std::atomic<u64> pooled_bytes{};
    std::atomic<u64> recording_reused{};
    std::atomic<u64> recording_cross_reused{};
    std::atomic<u64> recording_retired{};
    std::atomic<u64> log_sequence{};
    const bool collect_stats{};
    bool allow_oversize_recording_reuse{};
    bool stopping{};
};

TileManager::TileManager(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         StreamBuffer& stream_buffer_)
    : instance{instance}, scheduler{scheduler}, stream_buffer{stream_buffer_},
      scratch_pool{std::make_shared<ScratchPoolState>(
          instance.GetAllocator(),
          std::min(ScratchPoolMaximumBudgetBytes,
                   std::max(ScratchPoolMinimumBucketBytes,
                            instance.GetTotalMemoryBudget() / 32)),
          instance.GetCapabilityProfile().exact_proprietary_adreno750)}
#ifdef __ANDROID__
      ,
      detile_command_pool{std::make_unique<Vulkan::CommandPool>(
          instance, scheduler.GetMasterSemaphore(),
          vk::CommandBufferLevel::eSecondary)}
#endif
{
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings = {{
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
        {
            .binding = 2,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
    }};

    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = instance.MaxPushDescriptors() != 0
                     ? vk::DescriptorSetLayoutCreateFlags{
                           vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR}
                     : vk::DescriptorSetLayoutCreateFlags{},
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    auto desc_layout_result = device.createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(desc_layout_result.result == vk::Result::eSuccess,
               "Failed to create descriptor set layout: {}",
               vk::to_string(desc_layout_result.result));
    desc_layout = std::move(desc_layout_result.value);

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 0U,
        .pPushConstantRanges = nullptr,
    };
    auto [layout_result, layout] = device.createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess, "Failed to create pipeline layout: {}",
               vk::to_string(layout_result));
    pl_layout = std::move(layout);
}

TileManager::~TileManager() {
    scratch_pool->StopAndDestroy();
    scratch_pool->tiling_pipelines_exact.clear();
}

TileManager::ScratchBuffer TileManager::GetScratchBuffer(u32 size) {
    const u64 bucket = scratch_pool->BucketSize(size);
    ScratchBuffer scratch{};
    if (bucket != 0 && scratch_pool->TryAcquire(bucket, scratch)) {
        return scratch;
    }

    constexpr auto usage =
        vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst;

    const u64 allocation_size = scratch_pool->AllocationSize(size, bucket);
    const vk::BufferCreateInfo buffer_ci = {
        .size = allocation_size,
        .usage = usage,
    };

    const VmaAllocationCreateInfo alloc_info{
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };

    VkBuffer buffer;
    VmaAllocation allocation;
    const auto buffer_ci_unsafe = static_cast<VkBufferCreateInfo>(buffer_ci);
    const auto result = vmaCreateBuffer(instance.GetAllocator(), &buffer_ci_unsafe, &alloc_info,
                                        &buffer, &allocation, nullptr);
    ASSERT(result == VK_SUCCESS);
    scratch_pool->NoteCreated();
    return {
        .buffer = vk::Buffer{buffer},
        .allocation = allocation,
        .bucket = bucket,
        .capacity = allocation_size,
        .reused = false,
        .recording_reused = false,
    };
}

TileManager::ScratchBuffer TileManager::GetDetileScratchBuffer(
    const u32 size, const u64 recording_tick) {
    const u64 bucket = scratch_pool->BucketSize(size);
    ScratchBuffer scratch{};
    if (scheduler.HasRecordingCommandBuffer() &&
        scratch_pool->TryAcquireRecording(recording_tick, bucket, size, scratch)) {
        return scratch;
    }
    return GetScratchBuffer(size);
}

void TileManager::RetireScratchBuffer(ScratchBuffer scratch) {
    const std::shared_ptr<ScratchPoolState> pool = scratch_pool;
    const VmaAllocator allocator = instance.GetAllocator();
    // Return the allocation only after its command buffer retires. Pool hits
    // avoid both VMA allocation and Adreno object destruction in the hot
    // detile path; rejected entries retain the original deferred-destruction
    // lifetime.
    scheduler.DeferOperation([pool, allocator, scratch] {
        if (!pool->TryPool(scratch)) {
            vmaDestroyBuffer(allocator, static_cast<VkBuffer>(scratch.buffer),
                             scratch.allocation);
        }
    });
}

void TileManager::RetireDetileScratchBuffer(ScratchBuffer scratch,
                                            const u64 recording_tick) {
    if (recording_tick == scheduler.CurrentTick() &&
        scheduler.HasRecordingCommandBuffer()) {
        bool schedule_retirement = false;
        if (scratch_pool->RetireForRecording(recording_tick, scratch,
                                             schedule_retirement)) {
            if (schedule_retirement) {
                const std::shared_ptr<ScratchPoolState> pool = scratch_pool;
                scheduler.DeferOperation([pool, recording_tick] {
                    pool->ReleaseRecordingEpoch(recording_tick);
                });
            }
            return;
        }
    }

    // A submit between detile and the caller's final copy retirement changes
    // the recording tick. In that case the buffer must take the normal
    // timeline path and cannot be reused by the new command buffer yet.
    RetireScratchBuffer(scratch);
}

vk::Pipeline TileManager::GetTilingPipeline(const ImageInfo& info, bool is_tiler) {
    const u32 pl_id = u32(info.tile_mode) * NUM_BPPS + std::bit_width(info.num_bits) - 4;
    const PortableGpu::TilingPipelineIdentity key{
        u32(info.tile_mode),info.num_bits,info.num_samples,u32(info.array_mode),
        MixedMipMask(info)!=0,is_tiler,instance.GetCapabilityProfile().HasQuirk(Vulkan::CapabilityQuirk::ExplicitTilingBufferBounds)};
    auto& tiling_pipelines = scratch_pool->tiling_pipelines_exact;
    auto& exact_pipeline = tiling_pipelines[key];
#ifdef __ANDROID__
    if (Libraries::GnmDriver::ExecutorReplayActive())
        __android_log_print(ANDROID_LOG_INFO, "EXECUTOR",
                            "[EXECUTOR_DETILE_PL] pl_id=%u arraySize=%zu tile_mode=%u num_bits=%u",
                            pl_id, tiling_pipelines.size(), (unsigned)info.tile_mode, info.num_bits);
#endif
    if (auto pipeline = *exact_pipeline; pipeline != VK_NULL_HANDLE) {
        return pipeline;
    }
#ifdef __ANDROID__
    if (Libraries::GnmDriver::ExecutorReplayActive())
        __android_log_print(ANDROID_LOG_INFO, "EXECUTOR", "[EXECUTOR_TILE] post_deref array_mode=%u",
                            (unsigned)info.array_mode);
#endif

    // Compile-miss diagnostics only; cache hits perform no I/O. The old
    // array key ignored sample/array-mode recipe changes at the same tile/BPP.
    if (const char* trace=std::getenv("EXECUTOR_TILING_KEY_TRACE"); trace && trace[0]=='1') {
        for(const auto& [prior,pipeline] : tiling_pipelines) {
            if(*pipeline && prior.tile_mode==key.tile_mode &&
               prior.bits_per_pixel==key.bits_per_pixel && prior.tiler==key.tiler &&
               prior.micro_tiled_mips==key.micro_tiled_mips && prior!=key) {
                std::fprintf(stderr,"[TILING_RECIPE_ALIAS_PREVENTED] tile=%u bits=%u samples=%u->%u array=%u->%u\n",
                    key.tile_mode,key.bits_per_pixel,prior.samples,key.samples,prior.array_mode,key.array_mode);
                break;
            }
        }
    }
    const auto device = instance.GetDevice();
    const auto micro_tile_mode = AmdGpu::GetMicroTileMode(info.tile_mode);
    std::vector<std::string> defines = {
        fmt::format("BITS_PER_PIXEL={}", info.num_bits),
        fmt::format("NUM_SAMPLES={}", info.num_samples),
        fmt::format("ARRAY_MODE={}", u32(info.array_mode)),
        fmt::format("MICRO_TILE_MODE={}", u32(micro_tile_mode)),
        fmt::format("MICRO_TILE_THICKNESS={}", AmdGpu::GetMicroTileThickness(info.array_mode)),
    };
#ifdef __ANDROID__
#define EXEC_TL_MARK(s) do { if (Libraries::GnmDriver::ExecutorReplayActive()) \
    __android_log_print(ANDROID_LOG_INFO, "EXECUTOR", "[EXECUTOR_TILE] " s); } while(0)
#else
#define EXEC_TL_MARK(s) ((void)0)
#endif
    EXEC_TL_MARK("after_microtile");
    if (AmdGpu::IsMacroTiled(info.array_mode)) {
        EXEC_TL_MARK("macro:before_calcmode");
        const auto macro_tile_mode =
            AmdGpu::CalculateMacrotileMode(info.tile_mode, info.num_bits, info.num_samples);
        EXEC_TL_MARK("macro:before_numbanks");
        const u32 num_banks = AmdGpu::GetNumBanks(macro_tile_mode);
        defines.emplace_back(
            fmt::format("PIPE_CONFIG={}", u32(AmdGpu::GetPipeConfig(info.tile_mode))));
        defines.emplace_back(fmt::format("BANK_WIDTH={}", AmdGpu::GetBankWidth(macro_tile_mode)));
        defines.emplace_back(fmt::format("BANK_HEIGHT={}", AmdGpu::GetBankHeight(macro_tile_mode)));
        defines.emplace_back(fmt::format("NUM_BANKS={}", num_banks));
        defines.emplace_back(fmt::format("NUM_BANK_BITS={}", std::bit_width(num_banks) - 1));
        defines.emplace_back(fmt::format(
            "TILE_SPLIT_BYTES={}", AmdGpu::CalculateTileSplit(info.tile_mode, info.array_mode,
                                                              micro_tile_mode, info.num_bits)));
        defines.emplace_back(
            fmt::format("MACRO_TILE_ASPECT={}", AmdGpu::GetMacrotileAspect(macro_tile_mode)));
    }
    if (instance.GetCapabilityProfile().HasQuirk(
            Vulkan::CapabilityQuirk::ExplicitTilingBufferBounds)) {
        defines.emplace_back("EXPLICIT_TILING_BUFFER_BOUNDS=1");
    }
    if (key.micro_tiled_mips) {
        defines.emplace_back("HAS_MICRO_TILED_MIPS=1");
        std::fprintf(stderr,"[MIXED_MIP_LAYOUT] mask=%x tile=%u bpp=%u samples=%u tiler=%u\n",
            MixedMipMask(info),u32(info.tile_mode),info.num_bits,info.num_samples,is_tiler);
    }
    if (is_tiler) {
        defines.emplace_back(fmt::format("IS_TILER=1"));
    }
    EXEC_TL_MARK("before_vkcompile");
    const auto& module = Vulkan::Compile(PortableGpu::MixedMipShader,
                                         vk::ShaderStageFlagBits::eCompute, device, defines);
    EXEC_TL_MARK("after_vkcompile");
    const auto module_name = fmt::format("{}_{} {}", magic_enum::enum_name(info.tile_mode),
                                         info.num_bits, is_tiler ? "tiler" : "detiler");
    EXEC_TL_MARK("after_modulename");
    LOG_INFO(Render_Vulkan, "Compiling shader {}", module_name);
    for (const auto& def : defines) {
        LOG_INFO(Render_Vulkan, "#define {}", def);
    }
    Vulkan::SetObjectName(device, module, module_name);
    EXEC_TL_MARK("after_setname before_createpipeline");
    const vk::PipelineShaderStageCreateInfo shader_ci = {
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = module,
        .pName = "main",
    };
    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .stage = shader_ci,
        .layout = *pl_layout,
    };
    auto [result, pipeline] =
        device.createComputePipelineUnique(VK_NULL_HANDLE, compute_pipeline_ci);
#ifdef __ANDROID__
    if (Libraries::GnmDriver::ExecutorReplayActive())
        __android_log_print(ANDROID_LOG_INFO, "EXECUTOR",
                            "[EXECUTOR_TILE] createpipeline result=%d handle=%p",
                            (int)result, (void*)(VkPipeline)*pipeline);
#endif
    ASSERT_MSG(result == vk::Result::eSuccess, "Detiler pipeline creation failed {}",
               vk::to_string(result));
    exact_pipeline = std::move(pipeline);
    device.destroyShaderModule(module);
    EXEC_TL_MARK("getpipeline_done");
    return *exact_pipeline;
}

TileManager::Result TileManager::DetileImage(vk::Buffer in_buffer, u32 in_offset,
                                             const ImageInfo& info) {
    if (!info.props.is_tiled) {
        return {.buffer = in_buffer, .offset = in_offset};
    }

    TilingInfo params{};
    params.bank_swizzle = info.bank_swizzle;
    params.micro_tiled_mips = MixedMipMask(info);
    params.buffer_elements = info.guest_size / (info.num_bits / 8);
    params.num_slices = info.props.is_volume ? info.size.depth : info.resources.layers;
    params.num_mips = info.resources.levels;
    for (u32 mip = 0; mip < params.num_mips; ++mip) {
        auto& mip_info = params.mips[mip];
        mip_info = info.mips_layout[mip];
        if (info.props.is_block) {
            mip_info.pitch = std::max((mip_info.pitch + 3) / 4, 1U);
            mip_info.height = std::max((mip_info.height + 3) / 4, 1U);
        }
    }

    const vk::DescriptorBufferInfo params_buffer_info{
        .buffer = stream_buffer.Handle(),
        .offset = stream_buffer.Copy(&params, sizeof(params), instance.UniformMinAlignment()),
        .range = sizeof(params),
    };

    const u64 recording_tick = scheduler.CurrentTick();
    const ScratchBuffer scratch =
        GetDetileScratchBuffer(info.guest_size, recording_tick);
    const vk::Buffer out_buffer = scratch.buffer;

    scheduler.EndRendering(Vulkan::Scheduler::EndRenderingReason::TileCompute);
    const auto prepare_detile_write = [&](const vk::CommandBuffer barrier_cmdbuf) {
        if (!scratch.recording_reused) {
            return;
        }
        const vk::BufferMemoryBarrier2 upload_to_detile{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = out_buffer,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
        barrier_cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &upload_to_detile,
        });
    };
#ifdef __ANDROID__
    // Keep Qualcomm's compute state isolated without splitting the queue
    // epoch. The primary retains all barriers and ordering, while the
    // secondary starts from clean Vulkan command state.
    const auto primary_cmdbuf = scheduler.CommandBuffer();
    prepare_detile_write(primary_cmdbuf);
    const auto cmdbuf = detile_command_pool->Commit();
    const vk::CommandBufferInheritanceInfo inheritance_info{};
    const vk::CommandBufferBeginInfo begin_info{
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
        .pInheritanceInfo = &inheritance_info,
    };
    const vk::Result begin_result = cmdbuf.begin(begin_info);
    ASSERT_MSG(begin_result == vk::Result::eSuccess,
               "Failed to begin isolated detile command buffer: {}",
               vk::to_string(begin_result));
#else
    const auto cmdbuf = scheduler.CommandBuffer();
    prepare_detile_write(cmdbuf);
#endif

    const vk::Pipeline detile_pipeline = GetTilingPipeline(info, false);
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, detile_pipeline);
    scheduler.MarkPipelineBind(vk::PipelineBindPoint::eCompute);

    const vk::DescriptorBufferInfo tiled_buffer_info{
        .buffer = in_buffer,
        .offset = in_offset,
        .range = info.guest_size,
    };

    const vk::DescriptorBufferInfo linear_buffer_info{
        .buffer = out_buffer,
        .offset = 0,
        .range = info.guest_size,
    };

    const std::array<vk::WriteDescriptorSet, 3> set_writes = {{
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &tiled_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &linear_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 2,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .pBufferInfo = &params_buffer_info,
        },
    }};
    EXEC_TL_MARK("before_pushdesc");
    scheduler.BindHostDescriptors(cmdbuf, vk::PipelineBindPoint::eCompute, *pl_layout,
                                  *desc_layout, set_writes);
    scheduler.MarkDescriptorBind(vk::PipelineBindPoint::eCompute);
    EXEC_TL_MARK("after_pushdesc");

    const auto dim_x = (info.guest_size / (info.num_bits / 8)) / 64;
#ifdef __ANDROID__
    if (ExecutorTileScratchStatsEnabled()) {
        static std::atomic<u32> detile_dispatch_sequence{};
        const u32 dispatch_sequence =
            detile_dispatch_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        if (dispatch_sequence <= 8) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_DETILE_DISPATCH] n=%u scheduler=%p recording=%d "
                "tick=%llu gpuTick=%llu cmdbuf=%p pipeline=%p layout=%p "
                "in=%p inOffset=%u inOffsetMod=%llu out=%p params=%p paramsOffset=%llu "
                "guestSize=%u bits=%u dimX=%u bank=%u slices=%u mips=%u "
                "mip0Size=%u mip0Pitch=%u mip0Height=%u mip0Offset=%u "
                "width=%u height=%u pitch=%u tileMode=%u arrayMode=%u format=%u",
                dispatch_sequence, &scheduler,
                scheduler.HasRecordingCommandBuffer() ? 1 : 0,
                static_cast<unsigned long long>(scheduler.CurrentTick()),
                static_cast<unsigned long long>(scheduler.KnownGpuTick()),
                static_cast<void*>(static_cast<VkCommandBuffer>(cmdbuf)),
                static_cast<void*>(static_cast<VkPipeline>(detile_pipeline)),
                static_cast<void*>(static_cast<VkPipelineLayout>(*pl_layout)),
                static_cast<void*>(static_cast<VkBuffer>(in_buffer)),
                in_offset,
                static_cast<unsigned long long>(
                    in_offset % std::max<vk::DeviceSize>(instance.StorageMinAlignment(), 1)),
                static_cast<void*>(static_cast<VkBuffer>(out_buffer)),
                static_cast<void*>(static_cast<VkBuffer>(params_buffer_info.buffer)),
                static_cast<unsigned long long>(params_buffer_info.offset),
                info.guest_size, info.num_bits, dim_x, params.bank_swizzle,
                params.num_slices, params.num_mips, params.mips[0].size,
                params.mips[0].pitch, params.mips[0].height, params.mips[0].offset,
                info.size.width, info.size.height, info.pitch,
                static_cast<unsigned>(info.tile_mode), static_cast<unsigned>(info.array_mode),
                static_cast<unsigned>(info.pixel_format));
        }
    }
    if (Libraries::GnmDriver::ExecutorReplayActive()) {
        Core::Memory::Instance()->VerifyReplayCanaries("before_detile_dispatch");
        __android_log_print(ANDROID_LOG_INFO, "EXECUTOR",
                            "[EXECUTOR_TILE] before_dispatch dim_x=%u guest_size=%u num_bits=%u cmdbuf=%p",
                            (unsigned)dim_x, info.guest_size, info.num_bits, (void*)(VkCommandBuffer)cmdbuf);
    }
#endif
    cmdbuf.dispatch(dim_x, 1, 1);
    const vk::BufferMemoryBarrier2 detile_to_upload{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = out_buffer,
        .offset = 0,
        .size = info.guest_size,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &detile_to_upload,
    });
#ifdef __ANDROID__
    const vk::Result end_result = cmdbuf.end();
    ASSERT_MSG(end_result == vk::Result::eSuccess,
               "Failed to end isolated detile command buffer: {}",
               vk::to_string(end_result));
    primary_cmdbuf.executeCommands(cmdbuf);
#endif
    EXEC_TL_MARK("after_dispatch");
    // The caller still records copyBufferToImage from this buffer. Hand
    // ownership back with the result and retire it only after that final use
    // has been recorded, so its timeline tick cannot precede its last Vulkan
    // command.
    return {
        .buffer = out_buffer,
        .offset = 0,
        .scratch = scratch,
        .recording_tick = recording_tick,
    };
}

void TileManager::RetireDetileResult(Result result) {
    if (result.scratch.buffer) {
        RetireDetileScratchBuffer(result.scratch, result.recording_tick);
    }
}

void TileManager::TileImage(Image& in_image, std::span<vk::BufferImageCopy> buffer_copies,
                            vk::Buffer out_buffer, u32 out_offset, u32 copy_size) {
    const auto& info = in_image.info;
    if (!info.props.is_tiled) {
        for (auto& copy : buffer_copies) {
            copy.bufferOffset += out_offset;
        }
        in_image.Download(buffer_copies, out_buffer, out_offset, copy_size);
        return;
    }

    TilingInfo params{};
    params.bank_swizzle = info.bank_swizzle;
    params.micro_tiled_mips = MixedMipMask(info);
    params.buffer_elements = info.guest_size / (info.num_bits / 8);
    params.num_slices = info.props.is_volume ? info.size.depth : info.resources.layers;
    params.num_mips = static_cast<u32>(buffer_copies.size());
    for (u32 mip = 0; mip < params.num_mips; ++mip) {
        auto& mip_info = params.mips[mip];
        mip_info = info.mips_layout[mip];
        if (info.props.is_block) {
            mip_info.pitch = std::max((mip_info.pitch + 3) / 4, 1U);
            mip_info.height = std::max((mip_info.height + 3) / 4, 1U);
        }
    }

    const vk::DescriptorBufferInfo params_buffer_info{
        .buffer = stream_buffer.Handle(),
        .offset = stream_buffer.Copy(&params, sizeof(params), instance.UniformMinAlignment()),
        .range = sizeof(params),
    };

    const ScratchBuffer scratch = GetScratchBuffer(info.guest_size);
    const vk::Buffer temp_buffer = scratch.buffer;

    const auto cmdbuf = scheduler.CommandBuffer();
    in_image.Download(buffer_copies, temp_buffer, 0, copy_size);

    const vk::BufferMemoryBarrier2 download_to_tiler{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = temp_buffer,
        .offset = 0,
        .size = info.guest_size,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &download_to_tiler,
    });

    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, GetTilingPipeline(info, true));
    scheduler.MarkPipelineBind(vk::PipelineBindPoint::eCompute);

    const vk::DescriptorBufferInfo tiled_buffer_info{
        .buffer = out_buffer,
        .offset = out_offset,
        .range = info.guest_size,
    };

    const vk::DescriptorBufferInfo linear_buffer_info{
        .buffer = temp_buffer,
        .offset = 0,
        .range = info.guest_size,
    };

    const std::array<vk::WriteDescriptorSet, 3> set_writes = {{
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &tiled_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &linear_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 2,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .pBufferInfo = &params_buffer_info,
        },
    }};
    scheduler.BindHostDescriptors(cmdbuf, vk::PipelineBindPoint::eCompute, *pl_layout,
                                  *desc_layout, set_writes);
    scheduler.MarkDescriptorBind(vk::PipelineBindPoint::eCompute);

    const auto dim_x = (info.guest_size / (info.num_bits / 8)) / 64;
    cmdbuf.dispatch(dim_x, 1, 1);

    const vk::BufferMemoryBarrier2 tiler_to_consumers{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = out_buffer,
        .offset = out_offset,
        .size = info.guest_size,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &tiler_to_consumers,
    });
    RetireScratchBuffer(scratch);
}

}
