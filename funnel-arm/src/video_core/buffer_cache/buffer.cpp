// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/alignment.h"
#include "common/assert.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#if defined(LSX4_APPLE_RUNTIME)
#include <execinfo.h>
#endif
#include <vk_mem_alloc.h>
#include "ordered_mapped_readback.h"
#ifdef __ANDROID__
#include <android/log.h>
#include <unistd.h>
#endif

#include "allocation_trace.h"

namespace VideoCore {

namespace {

bool ExecutorSuballocateBdaBuffers() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const char* value = std::getenv("EXECUTOR_SUBALLOCATE_BDA_BUFFERS");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
#else
    return false;
#endif
}

bool RequireExactA750TurnipStorageBarrier(const Vulkan::Instance& instance) {
#ifdef __ANDROID__
    static const bool requested =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-sm8650-a750-turnip-storage-buffer-barrier-ab",
                 F_OK) == 0;
    const auto& profile = instance.GetCapabilityProfile();
    return requested && profile.exact_mesa_turnip_a750_d9_lineage;
#else
    return false;
#endif
}

bool SuballocateExactA750TurnipCacheBda(const Vulkan::Instance& instance,
                                        const VAddr cpu_addr, const u64 size_bytes) {
#ifdef __ANDROID__
    static const bool requested =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-sm8650-a750-turnip-cache-bda-suballocation-ab",
                 F_OK) == 0;
    const auto& profile = instance.GetCapabilityProfile();
    constexpr u64 MaximumSuballocatedCacheBufferSize = 4ULL * 1024ULL * 1024ULL;
    return requested && profile.exact_mesa_turnip_a750_d9_lineage &&
           cpu_addr != 0 && size_bytes <= MaximumSuballocatedCacheBufferSize;
#else
    return false;
#endif
}

} // namespace

std::string_view BufferTypeName(MemoryUsage type) {
    switch (type) {
    case MemoryUsage::Upload:
        return "Upload";
    case MemoryUsage::Download:
        return "Download";
    case MemoryUsage::Stream:
        return "Stream";
    case MemoryUsage::DeviceLocal:
        return "DeviceLocal";
    default:
        return "Invalid";
    }
}

[[nodiscard]] VkMemoryPropertyFlags MemoryUsagePreferredVmaFlags(MemoryUsage usage) {
    return usage != MemoryUsage::DeviceLocal ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                             : VkMemoryPropertyFlagBits{};
}

[[nodiscard]] VmaAllocationCreateFlags MemoryUsageVmaFlags(MemoryUsage usage) {
    switch (usage) {
    case MemoryUsage::Upload:
    case MemoryUsage::Stream:
        return VMA_ALLOCATION_CREATE_MAPPED_BIT |
               VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    case MemoryUsage::Download:
        return VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    case MemoryUsage::DeviceLocal:
        if (PortableGpu::OrderedMappedReadbackEnabled()) {
            return VMA_ALLOCATION_CREATE_MAPPED_BIT |
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT;
        }
        return {};
    }
    return {};
}

[[nodiscard]] VmaMemoryUsage MemoryUsageVma(MemoryUsage usage) {
    switch (usage) {
    case MemoryUsage::DeviceLocal:
    case MemoryUsage::Stream:
        return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    case MemoryUsage::Upload:
    case MemoryUsage::Download:
        return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    }
    return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
}

UniqueBuffer::UniqueBuffer(vk::Device device_, VmaAllocator allocator_)
    : device{device_}, allocator{allocator_} {}

UniqueBuffer::~UniqueBuffer() {
    if (buffer) {
        vmaDestroyBuffer(allocator, buffer, allocation);
    }
}

void UniqueBuffer::Create(const vk::BufferCreateInfo& buffer_ci, MemoryUsage usage,
                          VmaAllocationInfo* out_alloc_info,
                          const bool allow_bda_suballocation) {
    const bool created =
        TryCreate(buffer_ci, usage, out_alloc_info, allow_bda_suballocation);
    ASSERT_MSG(created, "Failed allocating buffer");
}

bool UniqueBuffer::TryCreate(const vk::BufferCreateInfo& buffer_ci, MemoryUsage usage,
                             VmaAllocationInfo* out_alloc_info,
                             const bool allow_bda_suballocation) noexcept {

#if defined(LSX4_APPLE_RUNTIME)
    static const bool diagnostic = Lsx4::Diagnostics::AllocationTraceEnabled();
    if (diagnostic && buffer_ci.size >= (1ULL << 20)) {
        VmaTotalStatistics stats{};
        vmaCalculateStatistics(allocator, &stats);
        std::fprintf(stderr,"[APPLE_TLOU_BUFFER_ALLOC] phase=before bytes=%llu usage=%u allocations=%u allocationBytes=%llu blockBytes=%llu\n",
            (unsigned long long)buffer_ci.size,(unsigned)usage,stats.total.statistics.allocationCount,
            (unsigned long long)stats.total.statistics.allocationBytes,
            (unsigned long long)stats.total.statistics.blockBytes);
        std::fflush(stderr);
        if (buffer_ci.size >= (256ULL << 20)) {
            void* frames[16]{};
            const int count = backtrace(frames, 16);
            std::fprintf(stderr, "[APPLE_LARGE_ALLOCATION_STACK] bytes=%llu usage=%u count=%d\n",
                (unsigned long long)buffer_ci.size, (unsigned)usage, count);
            backtrace_symbols_fd(frames, count, 2);
            std::fflush(stderr);
        }
    }
#endif
    const bool with_bda = bool(buffer_ci.usage & vk::BufferUsageFlagBits::eShaderDeviceAddress);
    const bool suballocate_bda =
        ExecutorSuballocateBdaBuffers() || allow_bda_suballocation;
    const VmaAllocationCreateFlags bda_flag = with_bda && !suballocate_bda
                                                  ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT
                                                  : 0;
    VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | bda_flag | MemoryUsageVmaFlags(usage),
        .usage = MemoryUsageVma(usage),
        .requiredFlags = 0,
        .preferredFlags = MemoryUsagePreferredVmaFlags(usage),
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    const VkBufferCreateInfo buffer_ci_unsafe = static_cast<VkBufferCreateInfo>(buffer_ci);
    VkBuffer unsafe_buffer{};
    VkResult result = vmaCreateBuffer(allocator, &buffer_ci_unsafe, &alloc_ci, &unsafe_buffer,
                                      &allocation, out_alloc_info);
    if (result != VK_SUCCESS && bda_flag != 0) {
        alloc_ci.flags &= ~VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        unsafe_buffer = VK_NULL_HANDLE;
        allocation = VK_NULL_HANDLE;
        result = vmaCreateBuffer(allocator, &buffer_ci_unsafe, &alloc_ci, &unsafe_buffer,
                                 &allocation, out_alloc_info);
#ifdef __ANDROID__
        __android_log_print(result == VK_SUCCESS ? ANDROID_LOG_WARN : ANDROID_LOG_ERROR,
                            "LSX4Native", "[EXECUTOR_BDA_ALLOCATION_FALLBACK] size=%llu result=%d",
                            static_cast<unsigned long long>(buffer_ci.size),
                            static_cast<int>(result));
#endif
    }
    if (result != VK_SUCCESS) {
        allocation = VK_NULL_HANDLE;
        buffer = VK_NULL_HANDLE;
        return false;
    }
    buffer = vk::Buffer{unsafe_buffer};
#if defined(LSX4_APPLE_RUNTIME)
    if (diagnostic && buffer_ci.size >= (1ULL << 20)) {
        VmaTotalStatistics stats{};
        vmaCalculateStatistics(allocator, &stats);
        std::fprintf(stderr,"[APPLE_TLOU_BUFFER_ALLOC] phase=after bytes=%llu usage=%u allocations=%u allocationBytes=%llu blockBytes=%llu\n",
            (unsigned long long)buffer_ci.size,(unsigned)usage,stats.total.statistics.allocationCount,
            (unsigned long long)stats.total.statistics.allocationBytes,
            (unsigned long long)stats.total.statistics.blockBytes);
        std::fflush(stderr);
    }
#endif


    if (with_bda) {
        vk::BufferDeviceAddressInfo bda_info{
            .buffer = buffer,
        };
        auto bda_result = device.getBufferAddress(bda_info);
        if (bda_result == 0) {
            vmaDestroyBuffer(allocator, unsafe_buffer, allocation);
            allocation = VK_NULL_HANDLE;
            buffer = VK_NULL_HANDLE;
            return false;
        }
        bda_addr = bda_result;
    }
    return true;
}

Buffer::Buffer(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_, MemoryUsage usage_,
               VAddr cpu_addr_, vk::BufferUsageFlags flags, u64 size_bytes_)
    : cpu_addr{cpu_addr_}, size_bytes{size_bytes_},
      require_storage_buffer_read_write_barrier{instance_.GetCapabilityProfile().HasQuirk(
                                                    Vulkan::CapabilityQuirk::RequireStorageBufferReadWriteBarrier) ||
                                                RequireExactA750TurnipStorageBarrier(instance_)},
      instance{&instance_}, scheduler{&scheduler_}, usage{usage_},
      buffer{instance->GetDevice(), instance->GetAllocator()} {
    const vk::BufferCreateInfo buffer_ci = {
        .size = size_bytes,
        .usage = flags,
    };
    VmaAllocationInfo alloc_info{};
    buffer.Create(buffer_ci, usage, &alloc_info,
                  SuballocateExactA750TurnipCacheBda(instance_, cpu_addr_, size_bytes_));

    const auto device = instance->GetDevice();
    Vulkan::SetObjectName(device, Handle(), "Buffer {:#x}:{:#x}", cpu_addr, size_bytes);

    VkMemoryPropertyFlags property_flags{};
    vmaGetAllocationMemoryProperties(instance->GetAllocator(), buffer.allocation, &property_flags);
    if (alloc_info.pMappedData) {
        mapped_data = std::span<u8>{std::bit_cast<u8*>(alloc_info.pMappedData), size_bytes};
    }
    is_coherent = property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}

Buffer::Buffer(std::nothrow_t, const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
               MemoryUsage usage_, VAddr cpu_addr_, vk::BufferUsageFlags flags,
               u64 size_bytes_) noexcept
    : cpu_addr{cpu_addr_}, size_bytes{size_bytes_},
      require_storage_buffer_read_write_barrier{instance_.GetCapabilityProfile().HasQuirk(
                                                    Vulkan::CapabilityQuirk::RequireStorageBufferReadWriteBarrier) ||
                                                RequireExactA750TurnipStorageBarrier(instance_)},
      instance{&instance_}, scheduler{&scheduler_}, usage{usage_},
      buffer{instance->GetDevice(), instance->GetAllocator()} {
    const vk::BufferCreateInfo buffer_ci = {
        .size = size_bytes,
        .usage = flags,
    };
    VmaAllocationInfo alloc_info{};
    if (!buffer.TryCreate(buffer_ci, usage, &alloc_info,
                          SuballocateExactA750TurnipCacheBda(instance_, cpu_addr_, size_bytes_))) {
        size_bytes = 0;
        return;
    }

    try {
        Vulkan::SetObjectName(instance->GetDevice(), Handle(), "Best-effort buffer {:#x}:{:#x}",
                              cpu_addr, size_bytes);
    } catch (...) {
        // Debug naming is not part of a best-effort allocation contract.
    }
    VkMemoryPropertyFlags property_flags{};
    vmaGetAllocationMemoryProperties(instance->GetAllocator(), buffer.allocation, &property_flags);
    if (alloc_info.pMappedData) {
        mapped_data = std::span<u8>{std::bit_cast<u8*>(alloc_info.pMappedData), size_bytes};
    }
    is_coherent = property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}

void Buffer::InvalidateMappedRange(const u64 offset, const u64 size) const noexcept {
    if (!IsValid() || is_coherent || mapped_data.empty() || size == 0 || offset > size_bytes ||
        size > size_bytes - offset) {
        return;
    }
    vmaInvalidateAllocation(instance->GetAllocator(), buffer.allocation, offset, size);
}

void Buffer::Fill(u64 offset, u32 num_bytes, u32 value) {
    if (num_bytes == 0) {
        return;
    }
    scheduler->EndRendering(Vulkan::Scheduler::EndRenderingReason::BufferFill);
    ASSERT_MSG(offset % 4 == 0 && num_bytes % 4 == 0,
               "FillBuffer size must be a multiple of 4 bytes");
    const auto cmdbuf = scheduler->CommandBuffer();
    const vk::BufferMemoryBarrier2 pre_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .buffer = buffer,
        .offset = offset,
        .size = num_bytes,
    };
    const vk::BufferMemoryBarrier2 post_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .buffer = buffer,
        .offset = offset,
        .size = num_bytes,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_barrier,
    });
    cmdbuf.fillBuffer(buffer, offset, num_bytes, value);
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post_barrier,
    });
}

SparseBuffer::SparseBuffer(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                           const vk::BufferUsageFlags flags, const u64 size_bytes_)
    : instance{&instance_}, scheduler{&scheduler_}, size_bytes{size_bytes_} {
    const vk::BufferCreateInfo create_info{
        .flags =
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
        .size = size_bytes,
        .usage = flags,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    auto [result, created_buffer] = instance->GetDevice().createBuffer(create_info);
    ASSERT_MSG(result == vk::Result::eSuccess, "Failed creating sparse BDA page-table buffer: {}",
               vk::to_string(result));
    buffer = created_buffer;
    memory_requirements = instance->GetDevice().getBufferMemoryRequirements(buffer);
    const auto& memory_properties = instance->GetMemoryProperties();
    for (u32 index = 0; index < memory_properties.memoryTypeCount; ++index) {
        const bool allowed = (memory_requirements.memoryTypeBits & (1U << index)) != 0;
        const bool device_local =
            (memory_properties.memoryTypes[index].propertyFlags &
             vk::MemoryPropertyFlagBits::eDeviceLocal) != vk::MemoryPropertyFlags{};
        if (allowed && device_local) {
            memory_type_index = index;
            break;
        }
    }
    if (memory_type_index == std::numeric_limits<u32>::max()) {
        for (u32 index = 0; index < memory_properties.memoryTypeCount; ++index) {
            if ((memory_requirements.memoryTypeBits & (1U << index)) != 0) {
                memory_type_index = index;
                break;
            }
        }
    }
    ASSERT_MSG(memory_type_index != std::numeric_limits<u32>::max(),
               "Sparse BDA buffer exposes no compatible memory type");
    constexpr u64 PreferredPhysicalBlockSize = 4ULL * 1024ULL * 1024ULL;
    block_size =
        Common::AlignUp(std::max<u64>(PreferredPhysicalBlockSize, memory_requirements.alignment),
                        memory_requirements.alignment);
    ASSERT_MSG(size_bytes % block_size == 0,
               "Sparse BDA table must be divisible by physical block size");
    physical_blocks.resize(size_bytes / block_size);
    Vulkan::SetObjectName(instance->GetDevice(), buffer, "Sparse BDA Page Table {:#x}", size_bytes);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_BDA_SPARSE_CREATE] logicalMiB=%llu blockKiB=%llu blocks=%llu "
                        "memoryType=%u memoryTypeBits=0x%x",
                        static_cast<unsigned long long>(size_bytes / (1024ULL * 1024ULL)),
                        static_cast<unsigned long long>(block_size / 1024ULL),
                        static_cast<unsigned long long>(physical_blocks.size()), memory_type_index,
                        memory_requirements.memoryTypeBits);
#endif
}

SparseBuffer::~SparseBuffer() {
    if (buffer) {
        instance->GetDevice().destroyBuffer(buffer);
        buffer = nullptr;
    }
    for (const auto& block : physical_blocks) {
        if (block.memory) {
            instance->GetDevice().freeMemory(block.memory);
        }
    }
}

bool SparseBuffer::EnsureResident(const u64 offset, const u64 size) {
    if (size == 0) {
        return true;
    }
    ASSERT_MSG(offset <= size_bytes && size <= size_bytes - offset,
               "Sparse BDA residency request is out of bounds");
    const u64 first_block = offset / block_size;
    const u64 last_block = (offset + size - 1) / block_size;

    struct PendingBlock {
        u64 index{};
        PhysicalBlock physical{};
        vk::SparseMemoryBind bind{};
    };
    std::vector<PendingBlock> pending;
    pending.reserve(last_block - first_block + 1);
    for (u64 index = first_block; index <= last_block; ++index) {
        if (physical_blocks[index].memory) {
            continue;
        }
        const vk::MemoryAllocateInfo allocation_info{
            .allocationSize = block_size,
            .memoryTypeIndex = memory_type_index,
        };
        const auto [allocation_result, allocated_memory] =
            instance->GetDevice().allocateMemory(allocation_info);
        if (allocation_result != vk::Result::eSuccess) {
#ifdef __ANDROID__
            __android_log_print(
                ANDROID_LOG_ERROR, "LSX4Native",
                "[EXECUTOR_BDA_SPARSE_FAILURE] stage=allocate result=%d "
                "block=%llu blockKiB=%llu alignment=%llu memoryType=%u "
                "memoryTypeBits=0x%x pending=%u",
                static_cast<int>(allocation_result), static_cast<unsigned long long>(index),
                static_cast<unsigned long long>(block_size / 1024ULL),
                static_cast<unsigned long long>(memory_requirements.alignment), memory_type_index,
                memory_requirements.memoryTypeBits, static_cast<unsigned>(pending.size()));
#endif
            for (const auto& staged : pending) {
                instance->GetDevice().freeMemory(staged.physical.memory);
            }
            return false;
        }
        pending.push_back({
            .index = index,
            .physical =
                {
                    .memory = allocated_memory,
                },
            .bind =
                {
                    .resourceOffset = index * block_size,
                    .size = block_size,
                    .memory = allocated_memory,
                    .memoryOffset = 0,
                },
        });
    }
    if (pending.empty()) {
        return true;
    }

    std::vector<vk::SparseMemoryBind> binds;
    binds.reserve(pending.size());
    for (const auto& staged : pending) {
        binds.push_back(staged.bind);
    }
    const vk::SparseBufferMemoryBindInfo buffer_bind{
        .buffer = buffer,
        .bindCount = static_cast<u32>(binds.size()),
        .pBinds = binds.data(),
    };
    const vk::BindSparseInfo bind_info{
        .bufferBindCount = 1,
        .pBufferBinds = &buffer_bind,
    };
#ifdef __ANDROID__
    const vk::Result bind_result = scheduler->BindSparseBufferMemory(bind_info);
#else
    const vk::Result bind_result = vk::Result::eErrorFeatureNotPresent;
#endif
    if (bind_result != vk::Result::eSuccess) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR, "LSX4Native",
                            "[EXECUTOR_BDA_SPARSE_FAILURE] stage=bind result=%d newBlocks=%u "
                            "firstBlock=%llu lastBlock=%llu blockKiB=%llu",
                            static_cast<int>(bind_result), static_cast<unsigned>(pending.size()),
                            static_cast<unsigned long long>(first_block),
                            static_cast<unsigned long long>(last_block),
                            static_cast<unsigned long long>(block_size / 1024ULL));
#endif
        for (const auto& staged : pending) {
            instance->GetDevice().freeMemory(staged.physical.memory);
        }
        return false;
    }
    for (auto& staged : pending) {
        physical_blocks[staged.index] = staged.physical;
        resident_bytes += block_size;
    }
    ++bind_batches;
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_BDA_SPARSE_BIND] batch=%llu newBlocks=%u residentMiB=%llu "
                        "logicalMiB=%llu",
                        static_cast<unsigned long long>(bind_batches),
                        static_cast<unsigned>(pending.size()),
                        static_cast<unsigned long long>(resident_bytes / (1024ULL * 1024ULL)),
                        static_cast<unsigned long long>(size_bytes / (1024ULL * 1024ULL)));
#endif
    return true;
}

void SparseBuffer::Fill(const u64 offset, const u32 num_bytes, const u32 value) {
    if (num_bytes == 0) {
        return;
    }
    if (!EnsureResident(offset, num_bytes)) {
        ASSERT_MSG(false, "Failed committing sparse BDA page-table residency for fill");
        return;
    }
    scheduler->EndRendering(Vulkan::Scheduler::EndRenderingReason::BufferFill);
    ASSERT_MSG(offset % 4 == 0 && num_bytes % 4 == 0,
               "FillBuffer size must be a multiple of 4 bytes");
    const auto cmdbuf = scheduler->CommandBuffer();
    const vk::BufferMemoryBarrier2 pre_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .buffer = buffer,
        .offset = offset,
        .size = num_bytes,
    };
    const vk::BufferMemoryBarrier2 post_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .buffer = buffer,
        .offset = offset,
        .size = num_bytes,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_barrier,
    });
    cmdbuf.fillBuffer(buffer, offset, num_bytes, value);
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post_barrier,
    });
}

BdaPageTableBuffer::BdaPageTableBuffer(const Vulkan::Instance& instance_,
                                       Vulkan::Scheduler& scheduler_, const u64 size_bytes)
    : instance{&instance_}, scheduler{&scheduler_}, logical_size{size_bytes} {}

void BdaPageTableBuffer::EnsureStorage() const {
    if (storage_ready.load(std::memory_order_acquire)) return;
    std::call_once(storage_once, [this] {
        // Keep the capability-selected physical route and complete logical
        // address coverage. Only the allocation lifetime changes.
        const bool sparse_backing =
            instance->GetCapabilityProfile().memory_policy.bda_page_table.mode !=
            Vulkan::BdaPageTableMode::FlatResident;
        if (sparse_backing) {
            sparse = std::make_unique<SparseBuffer>(*instance, *scheduler, AllFlags, logical_size);
        } else {
            flat = std::make_unique<Buffer>(*instance, *scheduler, MemoryUsage::DeviceLocal,
                                            0, AllFlags, logical_size);
        }
        Vulkan::SetObjectName(instance->GetDevice(), sparse ? sparse->Handle() : flat->Handle(),
                              "BDA Page Table Buffer");
        storage_ready.store(true, std::memory_order_release);
    });
}

constexpr u64 WATCHES_INITIAL_RESERVE = 0x4000;
constexpr u64 WATCHES_RESERVE_CHUNK = 0x1000;

StreamBuffer::StreamBuffer(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                           MemoryUsage usage, u64 size_bytes)
    : Buffer{instance, scheduler, usage, 0, AllFlags, size_bytes} {
    ReserveWatches(current_watches, WATCHES_INITIAL_RESERVE);
    ReserveWatches(previous_watches, WATCHES_INITIAL_RESERVE);
    const auto device = instance.GetDevice();
    Vulkan::SetObjectName(device, Handle(), "StreamBuffer({}):{:#x}", BufferTypeName(usage),
                          size_bytes);
}

StreamBuffer::StreamBuffer(std::nothrow_t, const Vulkan::Instance& instance,
                           Vulkan::Scheduler& scheduler, MemoryUsage usage, u64 size_bytes) noexcept
    : Buffer{std::nothrow, instance, scheduler, usage, 0, AllFlags, size_bytes} {
    try {
        if (IsValid()) {
            lease_blocks.resize((size_bytes + LeaseBlockSize - 1) / LeaseBlockSize);
        }
    } catch (...) {
        this->size_bytes = 0;
    }
    if (IsValid() && this->size_bytes != 0) {
        try {
            Vulkan::SetObjectName(instance.GetDevice(), Handle(),
                                  "BestEffortStreamBuffer({}):{:#x}", BufferTypeName(usage),
                                  size_bytes);
        } catch (...) {
            // Keep the renderer fallback operational under host-memory pressure.
        }
    }
}

std::pair<u8*, u64> StreamBuffer::Map(u64 size, u64 alignment, bool allow_wait) {
    if (!is_coherent && usage == MemoryUsage::Stream) {
        size = Common::AlignUp(size, instance->NonCoherentAtomSize());
    }

    ++request_count;
    requested_bytes += size;
    max_request_bytes = std::max(max_request_bytes, size);
    if (size > this->size_bytes) {
        ++failed_request_count;
        return {nullptr, 0};
    }

    u64 candidate_offset = alignment > 0 ? Common::AlignUp(offset, alignment) : offset;
    const bool wraps = candidate_offset + size > this->size_bytes;
    if (wraps && !PreflightWrap(size, allow_wait)) {
        ++failed_request_count;
        return {nullptr, 0};
    }
    if (wraps) {
        ++wrap_count;
        invalidation_mark = current_watch_cursor;
        current_watch_cursor = 0;
        offset = 0;

        std::swap(previous_watches, current_watches);
        wait_cursor = 0;
        wait_bound = 0;
        candidate_offset = 0;
    }

    const u64 mapped_upper_bound = candidate_offset + size;
    if (!WaitPendingOperations(mapped_upper_bound, allow_wait)) {
        ++failed_request_count;
        return {nullptr, 0};
    }

    max_mapped_end = std::max(max_mapped_end, mapped_upper_bound);
    offset = candidate_offset;
    mapped_size = size;
    return {mapped_data.data() + offset, offset};
}

std::pair<u8*, u64> StreamBuffer::MapLeased(u64 size, u64 alignment, bool allow_wait,
                                            Lease& lease) {
    if (!is_coherent && usage == MemoryUsage::Stream) {
        size = Common::AlignUp(size, instance->NonCoherentAtomSize());
    }
    ++request_count;
    requested_bytes += size;
    max_request_bytes = std::max(max_request_bytes, size);
    if (size == 0 || size > size_bytes || lease_blocks.empty()) {
        ++failed_request_count;
        return {nullptr, 0};
    }
    if (!lease && active_lease) {
        ++failed_request_count;
        return {nullptr, 0};
    }

    const u64 effective_alignment = lease ? alignment : std::max<u64>(alignment, LeaseBlockSize);
    u64 candidate_offset =
        effective_alignment > 0 ? Common::AlignUp(offset, effective_alignment) : offset;
    bool wraps = candidate_offset + size > size_bytes;
    ActiveLease* active{};
    if (lease) {
        // One draw/dispatch owns one contiguous provisional range. A wrap in
        // the middle of that batch cannot be represented as one exact lease.
        if (wraps || lease.generation != ring_generation || candidate_offset < lease.end) {
            ++failed_request_count;
            return {nullptr, 0};
        }
        if (!active_lease || active_lease->range.id != lease.id) {
            ++failed_request_count;
            return {nullptr, 0};
        }
        active = &*active_lease;
    } else if (wraps) {
        candidate_offset = 0;
    }
    const u64 candidate_end = candidate_offset + size;
    if (!PreflightLeasedRange(candidate_offset, candidate_end, allow_wait, active)) {
        ++failed_request_count;
        return {nullptr, 0};
    }

    if (!lease) {
        if (wraps) {
            ++ring_generation;
            ++wrap_count;
        }
        lease = Lease{
            .id = next_lease_id++,
            .generation = ring_generation,
            .begin = candidate_offset,
            .end = candidate_offset,
        };
        active_lease.emplace(ActiveLease{.range = lease});
        active = &*active_lease;
    }

    const u16 first_block = static_cast<u16>(candidate_offset / LeaseBlockSize);
    const u16 last_block = static_cast<u16>((candidate_end - 1) / LeaseBlockSize);
    for (u16 block_index = first_block; block_index <= last_block; ++block_index) {
        if (LeaseOwnsBlock(*active, block_index)) {
            continue;
        }
        auto& block = lease_blocks[block_index];
        block.generation = ring_generation;
        block.retire_tick = 0;
        ++block.provisional_refs;
    }
    active->range.end = candidate_end;
    lease.end = candidate_end;

    offset = candidate_offset;
    mapped_size = size;
    max_mapped_end = std::max(max_mapped_end, candidate_end);
    return {mapped_data.data() + offset, offset};
}

void StreamBuffer::Commit() {
    if (!is_coherent) {
        if (usage == MemoryUsage::Download) {
            vmaInvalidateAllocation(instance->GetAllocator(), buffer.allocation, offset,
                                    mapped_size);
        } else {
            vmaFlushAllocation(instance->GetAllocator(), buffer.allocation, offset, mapped_size);
        }
    }

    offset += mapped_size;
    if (current_watch_cursor != 0 &&
        current_watches[current_watch_cursor - 1].tick == scheduler->CurrentTick()) {
        current_watches[current_watch_cursor - 1].upper_bound = offset;
        return;
    }

    if (current_watch_cursor + 1 >= current_watches.size()) {
        ReserveWatches(current_watches, WATCHES_RESERVE_CHUNK);
    }

    auto& watch = current_watches[current_watch_cursor++];
    watch.upper_bound = offset;
    watch.tick = scheduler->CurrentTick();
}

void StreamBuffer::CommitLeased(const Lease& lease) {
    ASSERT(active_lease && active_lease->range.id == lease.id);
    if (!is_coherent) {
        vmaFlushAllocation(instance->GetAllocator(), buffer.allocation, offset, mapped_size);
    }
    offset += mapped_size;
}

void StreamBuffer::RetireLease(const Lease& lease, const u64 consuming_tick) {
    ASSERT(active_lease && active_lease->range.id == lease.id);
    const u16 first_block = static_cast<u16>(active_lease->range.begin / LeaseBlockSize);
    const u16 last_block = static_cast<u16>((active_lease->range.end - 1) / LeaseBlockSize);
    for (u16 block_index = first_block; block_index <= last_block; ++block_index) {
        auto& block = lease_blocks[block_index];
        ASSERT(block.provisional_refs != 0);
        --block.provisional_refs;
        block.retire_tick = std::max(block.retire_tick, consuming_tick);
    }
    active_lease.reset();
}

void StreamBuffer::CancelLease(const Lease& lease) {
    if (active_lease && active_lease->range.id == lease.id) {
        const u16 first_block = static_cast<u16>(active_lease->range.begin / LeaseBlockSize);
        const u16 last_block = static_cast<u16>((active_lease->range.end - 1) / LeaseBlockSize);
        for (u16 block_index = first_block; block_index <= last_block; ++block_index) {
            auto& block = lease_blocks[block_index];
            ASSERT(block.provisional_refs != 0);
            --block.provisional_refs;
        }
        active_lease.reset();
    }
}

void StreamBuffer::RetainCurrentAllocation() {
    if (offset == 0) {
        return;
    }

    const u64 current_tick = scheduler->CurrentTick();
    if (current_watch_cursor != 0 &&
        current_watches[current_watch_cursor - 1].tick == current_tick) {
        current_watches[current_watch_cursor - 1].upper_bound =
            std::max(current_watches[current_watch_cursor - 1].upper_bound, offset);
        return;
    }

    if (current_watch_cursor + 1 >= current_watches.size()) {
        ReserveWatches(current_watches, WATCHES_RESERVE_CHUNK);
    }

    auto& watch = current_watches[current_watch_cursor++];
    watch.upper_bound = offset;
    watch.tick = current_tick;
}

void StreamBuffer::ReserveWatches(std::vector<Watch>& watches, std::size_t grow_size) {
    watches.resize(watches.size() + grow_size);
}

bool StreamBuffer::WaitPendingOperations(u64 requested_upper_bound, bool allow_wait) {
    if (!invalidation_mark) {
        return true;
    }
    while (requested_upper_bound > wait_bound && wait_cursor < *invalidation_mark) {
        auto& watch = previous_watches[wait_cursor];
        const bool pending = !scheduler->IsFree(watch.tick);
        if (pending && !allow_wait) {
            return false;
        }
        forced_wait_count += pending ? 1 : 0;
        scheduler->Wait(watch.tick);
        wait_bound = watch.upper_bound;
        ++wait_cursor;
    }
    return true;
}

bool StreamBuffer::PreflightWrap(const u64 requested_upper_bound, const bool allow_wait) {
    u64 covered = 0;
    for (std::size_t index = 0; requested_upper_bound > covered && index < current_watch_cursor;
         ++index) {
        const auto& watch = current_watches[index];
        if (!scheduler->IsFree(watch.tick)) {
            if (!allow_wait) {
                return false;
            }
            ++forced_wait_count;
            scheduler->Wait(watch.tick);
        }
        covered = watch.upper_bound;
    }
    return true;
}

bool StreamBuffer::PreflightLeasedRange(const u64 begin, const u64 end, const bool allow_wait,
                                        const ActiveLease* owner) {
    const u16 first_block = static_cast<u16>(begin / LeaseBlockSize);
    const u16 last_block = static_cast<u16>((end - 1) / LeaseBlockSize);
    for (u16 block_index = first_block; block_index <= last_block; ++block_index) {
        if (owner != nullptr && LeaseOwnsBlock(*owner, block_index)) {
            continue;
        }
        const auto& block = lease_blocks[block_index];
        if (block.provisional_refs != 0) {
            return false;
        }
        if (block.retire_tick != 0 && !scheduler->IsFree(block.retire_tick)) {
            if (!allow_wait) {
                return false;
            }
            ++forced_wait_count;
            scheduler->Wait(block.retire_tick);
        }
    }
    return true;
}

bool StreamBuffer::LeaseOwnsBlock(const ActiveLease& lease, const u16 block) const {
    if (lease.range.begin == lease.range.end) {
        return false;
    }
    const u64 block_begin = static_cast<u64>(block) * LeaseBlockSize;
    const u64 block_end = block_begin + LeaseBlockSize;
    return lease.range.begin < block_end && block_begin < lease.range.end;
}

} // namespace VideoCore
