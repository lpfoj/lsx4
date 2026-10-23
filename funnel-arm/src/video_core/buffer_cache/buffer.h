// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <utility>
#include <vector>
#include "common/types.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

VK_DEFINE_HANDLE(VmaAllocation)
VK_DEFINE_HANDLE(VmaAllocator)

struct VmaAllocationInfo;

namespace VideoCore {

enum class MemoryUsage {
    DeviceLocal,
    Upload,
    Download,
    Stream,
};

constexpr vk::BufferUsageFlags ReadFlags =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eUniformBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer;

constexpr vk::BufferUsageFlags AllFlags =
    ReadFlags | vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer;

struct UniqueBuffer {
    explicit UniqueBuffer(vk::Device device, VmaAllocator allocator);
    ~UniqueBuffer();

    UniqueBuffer(const UniqueBuffer&) = delete;
    UniqueBuffer& operator=(const UniqueBuffer&) = delete;

    UniqueBuffer(UniqueBuffer&& other)
        : allocator{std::exchange(other.allocator, VK_NULL_HANDLE)},
          allocation{std::exchange(other.allocation, VK_NULL_HANDLE)},
          buffer{std::exchange(other.buffer, VK_NULL_HANDLE)} {}
    UniqueBuffer& operator=(UniqueBuffer&& other) {
        buffer = std::exchange(other.buffer, VK_NULL_HANDLE);
        allocator = std::exchange(other.allocator, VK_NULL_HANDLE);
        allocation = std::exchange(other.allocation, VK_NULL_HANDLE);
        return *this;
    }

    void Create(const vk::BufferCreateInfo& image_ci, MemoryUsage usage,
                VmaAllocationInfo* out_alloc_info,
                bool allow_bda_suballocation = false);
    [[nodiscard]] bool TryCreate(const vk::BufferCreateInfo& image_ci, MemoryUsage usage,
                                 VmaAllocationInfo* out_alloc_info,
                                 bool allow_bda_suballocation = false) noexcept;

    operator vk::Buffer() const {
        return buffer;
    }

    vk::Device device;
    VmaAllocator allocator;
    VmaAllocation allocation;
    vk::Buffer buffer{};
    vk::DeviceAddress bda_addr = 0;
};

class Buffer {
public:
    explicit Buffer(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                    MemoryUsage usage, VAddr cpu_addr_, vk::BufferUsageFlags flags,
                    u64 size_bytes_);
    Buffer(std::nothrow_t, const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
           MemoryUsage usage, VAddr cpu_addr_, vk::BufferUsageFlags flags,
           u64 size_bytes_) noexcept;

    Buffer& operator=(const Buffer&) = delete;
    Buffer(const Buffer&) = delete;

    Buffer& operator=(Buffer&&) = default;
    Buffer(Buffer&&) = default;

    void IncreaseStreamScore(int score) noexcept {
        stream_score += score;
    }

    [[nodiscard]] int StreamScore() const noexcept {
        return stream_score;
    }

    [[nodiscard]] bool IsInBounds(VAddr addr, u64 size) const noexcept {
        return addr >= cpu_addr && addr + size <= cpu_addr + SizeBytes();
    }

    [[nodiscard]] VAddr CpuAddr() const noexcept {
        return cpu_addr;
    }

    [[nodiscard]] u64 Offset(VAddr other_cpu_addr) const noexcept {
        return other_cpu_addr - cpu_addr;
    }

    size_t SizeBytes() const {
        return size_bytes;
    }

    [[nodiscard]] bool IsValid() const noexcept {
        return static_cast<bool>(buffer.buffer);
    }

    void InvalidateMappedRange(u64 offset, u64 size) const noexcept;

    void SetLRUId(u64 id) noexcept {
        lru_id = id;
    }

    u64 LRUId() const noexcept {
        return lru_id;
    }

    vk::Buffer Handle() const noexcept {
        return buffer;
    }

    vk::DeviceAddress BufferDeviceAddress() const noexcept {
        ASSERT_MSG(buffer.bda_addr != 0, "Can't get BDA from a non BDA buffer");
        return buffer.bda_addr;
    }

    std::optional<vk::BufferMemoryBarrier2> GetBarrier(vk::AccessFlags2 dst_acess_mask,
                                                       vk::PipelineStageFlags2 dst_stage,
                                                       u32 offset = 0) {
        constexpr vk::AccessFlags2 WriteAccessMask = vk::AccessFlagBits2::eMemoryWrite |
                                                     vk::AccessFlagBits2::eTransferWrite |
                                                     vk::AccessFlagBits2::eShaderWrite;
        constexpr vk::AccessFlags2 ShaderReadWriteMask =
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
        const bool a750_same_cb_storage_hazard =
            require_storage_buffer_read_write_barrier &&
            (access_mask & vk::AccessFlagBits2::eShaderWrite) &&
            (dst_acess_mask & ShaderReadWriteMask) == ShaderReadWriteMask;
        if (dst_acess_mask == access_mask && stage == dst_stage && !a750_same_cb_storage_hazard) {
            return {};
        }

        const bool src_read_only = !(access_mask & WriteAccessMask);
        const bool dst_read_only = !(dst_acess_mask & WriteAccessMask);
        if (src_read_only && dst_read_only) {
            access_mask |= dst_acess_mask;
            stage |= dst_stage;
            return {};
        }

        DEBUG_ASSERT(offset < size_bytes);

        const auto barrier = vk::BufferMemoryBarrier2{
            .srcStageMask = stage,
            .srcAccessMask = access_mask,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_acess_mask,
            .buffer = buffer.buffer,
            .offset = offset,
            .size = size_bytes - offset,
        };
        access_mask = dst_acess_mask;
        stage = dst_stage;
        return barrier;
    }

    void Fill(u64 offset, u32 num_bytes, u32 value);

public:
    VAddr cpu_addr = 0;
    bool is_picked{};
    bool is_coherent{};
    bool is_deleted{};
    int stream_score = 0;
    size_t size_bytes = 0;
    bool require_storage_buffer_read_write_barrier{};
    u64 lru_id = 0;
    std::span<u8> mapped_data;
    const Vulkan::Instance* instance;
    Vulkan::Scheduler* scheduler;
    MemoryUsage usage;
    UniqueBuffer buffer;
    vk::Flags<vk::AccessFlagBits2> access_mask{
        vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite |
        vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite};
    vk::PipelineStageFlags2 stage{vk::PipelineStageFlagBits2::eAllCommands};
};

// A logical storage buffer whose physical pages are committed on first write.
// It is intentionally specialized instead of teaching every Buffer allocation
// about sparse residency; only the exact BDA page-table policy may construct it.
class SparseBuffer {
public:
    explicit SparseBuffer(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                          vk::BufferUsageFlags flags, u64 size_bytes);
    ~SparseBuffer();

    SparseBuffer(const SparseBuffer&) = delete;
    SparseBuffer& operator=(const SparseBuffer&) = delete;
    SparseBuffer(SparseBuffer&&) = delete;
    SparseBuffer& operator=(SparseBuffer&&) = delete;

    [[nodiscard]] vk::Buffer Handle() const noexcept {
        return buffer;
    }

    [[nodiscard]] size_t SizeBytes() const noexcept {
        return size_bytes;
    }

    [[nodiscard]] u64 Offset(const VAddr address) const noexcept {
        return address;
    }

    [[nodiscard]] u64 ResidentBytes() const noexcept {
        return resident_bytes;
    }

    [[nodiscard]] u64 BindBatchCount() const noexcept {
        return bind_batches;
    }

    bool EnsureResident(u64 offset, u64 size);
    void Fill(u64 offset, u32 num_bytes, u32 value);

private:
    struct PhysicalBlock {
        vk::DeviceMemory memory{};
    };

    const Vulkan::Instance* instance{};
    Vulkan::Scheduler* scheduler{};
    vk::Buffer buffer{};
    u64 size_bytes{};
    u64 block_size{};
    u32 memory_type_index{std::numeric_limits<u32>::max()};
    u64 resident_bytes{};
    u64 bind_batches{};
    vk::MemoryRequirements memory_requirements{};
    std::vector<PhysicalBlock> physical_blocks;
};

class BdaPageTableBuffer {
public:
    explicit BdaPageTableBuffer(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                                u64 size_bytes);

    [[nodiscard]] vk::Buffer Handle() const {
        EnsureStorage();
        return sparse ? sparse->Handle() : flat->Handle();
    }

    [[nodiscard]] size_t SizeBytes() const noexcept {
        return logical_size;
    }

    [[nodiscard]] u64 Offset(const VAddr address) const noexcept {
        return address; // BDA tables have CpuAddr()==0 in both storage routes.
    }

    [[nodiscard]] VAddr CpuAddr() const noexcept {
        return 0;
    }

    [[nodiscard]] u64 ResidentBytes() const noexcept {
        if (!storage_ready.load(std::memory_order_acquire)) return 0;
        return sparse ? sparse->ResidentBytes() : flat->SizeBytes();
    }

    bool EnsureResident(u64 offset, u64 size) {
        if (size == 0) return true;
        EnsureStorage();
        return sparse ? sparse->EnsureResident(offset, size) : true;
    }

    void Fill(u64 offset, u32 num_bytes, u32 value) {
        if (num_bytes == 0) return;
        EnsureStorage();
        if (sparse) {
            sparse->Fill(offset, num_bytes, value);
        } else {
            flat->Fill(offset, num_bytes, value);
        }
    }

private:
    // The GPU owner materializes this table only for a real upload, fill or
    // descriptor binding. Merely naming it or reading telemetry must not
    // consume 512 MiB when direct guest-memory access is disabled.
    void EnsureStorage() const;
    const Vulkan::Instance* instance;
    Vulkan::Scheduler* scheduler;
    const u64 logical_size;
    mutable std::once_flag storage_once;
    mutable std::atomic<bool> storage_ready{false};
    mutable std::unique_ptr<Buffer> flat;
    mutable std::unique_ptr<SparseBuffer> sparse;
};

class StreamBuffer : public Buffer {
public:
    struct UsageTelemetrySnapshot {
        u64 capacity_bytes{};
        u64 requested_bytes{};
        u64 request_count{};
        u64 max_request_bytes{};
        u64 max_mapped_end{};
        u64 wrap_count{};
        u64 forced_wait_count{};
        u64 failed_request_count{};
    };

    struct Lease {
        u64 id{};
        u64 generation{};
        u64 begin{};
        u64 end{};

        explicit operator bool() const noexcept {
            return id != 0;
        }
    };

    explicit StreamBuffer(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                          MemoryUsage usage, u64 size_bytes_);
    StreamBuffer(std::nothrow_t, const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                 MemoryUsage usage, u64 size_bytes_) noexcept;

    std::pair<u8*, u64> Map(u64 size, u64 alignment = 0, bool allow_wait = true);

    // Reserve or extend one exact provisional range. Extending a lease never
    // wraps; callers fail closed for the remainder of that descriptor batch.
    std::pair<u8*, u64> MapLeased(u64 size, u64 alignment, bool allow_wait, Lease& lease);

    void Commit();

    void CommitLeased(const Lease& lease);

    void RetireLease(const Lease& lease, u64 consuming_tick);

    void CancelLease(const Lease& lease);

    void RetainCurrentAllocation();

    [[nodiscard]] UsageTelemetrySnapshot GetUsageTelemetrySnapshot() const noexcept {
        return {
            .capacity_bytes = static_cast<u64>(SizeBytes()),
            .requested_bytes = requested_bytes,
            .request_count = request_count,
            .max_request_bytes = max_request_bytes,
            .max_mapped_end = max_mapped_end,
            .wrap_count = wrap_count,
            .forced_wait_count = forced_wait_count,
            .failed_request_count = failed_request_count,
        };
    }

    u64 Copy(auto src, size_t size, size_t alignment = 0) {
        const auto [data, offset] = Map(size, alignment);
        std::memcpy(data, reinterpret_cast<const void*>(src), size);
        Commit();
        return offset;
    }

private:
    // Each descriptor batch starts on its own 4-KiB block. This avoids the
    // false sharing of a coarse lease ledger while costing ~4 MiB per 1000
    // draws, so a 64-MiB arena still covers many in-flight frames.
    static constexpr u64 LeaseBlockSize = 4 * 1024;

    struct Watch {
        u64 tick{};
        u64 upper_bound{};
    };

    void ReserveWatches(std::vector<Watch>& watches, std::size_t grow_size);

    bool WaitPendingOperations(u64 requested_upper_bound, bool allow_wait);

    bool PreflightWrap(u64 requested_upper_bound, bool allow_wait);

    struct ActiveLease {
        Lease range{};
    };

    struct LeaseBlock {
        u64 generation{};
        u64 retire_tick{};
        u32 provisional_refs{};
    };

    bool PreflightLeasedRange(u64 begin, u64 end, bool allow_wait, const ActiveLease* owner);

    [[nodiscard]] bool LeaseOwnsBlock(const ActiveLease& lease, u16 block) const;

private:
    u64 offset{};
    u64 mapped_size{};
    std::vector<Watch> current_watches;
    std::size_t current_watch_cursor{};
    std::optional<size_t> invalidation_mark;
    std::vector<Watch> previous_watches;
    std::size_t wait_cursor{};
    u64 wait_bound{};
    u64 ring_generation{1};
    u64 next_lease_id{1};
    std::optional<ActiveLease> active_lease;
    std::vector<LeaseBlock> lease_blocks;
    u64 requested_bytes{};
    u64 request_count{};
    u64 max_request_bytes{};
    u64 max_mapped_end{};
    u64 wrap_count{};
    u64 forced_wait_count{};
    u64 failed_request_count{};
};

} // namespace VideoCore
