// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <boost/container/small_vector.hpp>
#include <mutex>
#include <optional>
#include <vector>
#include "common/lru_cache.h"
#include "common/slot_vector.h"
#include "common/types.h"
#include "core/memory.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/multi_level_page_table.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {
class GraphicsPipeline;
}

namespace VideoCore {

using BufferId = Common::SlotId;

static constexpr BufferId NULL_BUFFER_ID{0};

class TextureCache;
class MemoryTracker;
class PageManager;

class BufferCache {
public:
    struct ResidencyTelemetrySnapshot {
        u64 resident_bytes{};
        u64 utility_bytes{};
        u64 buffer_entries{};
    };

    struct UtilityRingTelemetrySnapshot {
        struct UploadReservation {
            u64 transaction_count{};
            u64 completed_transaction_count{};
            u64 failed_transaction_count{};
            u64 measurement_count{};
            u64 retry_count{};
            u64 full_span_candidate_bytes{};
            u64 dirty_bytes{};
            u64 avoided_bytes{};
            u64 max_full_span_candidate_bytes{};
            u64 max_dirty_bytes{};
        };

        StreamBuffer::UsageTelemetrySnapshot staging;
        StreamBuffer::UsageTelemetrySnapshot stream;
        StreamBuffer::UsageTelemetrySnapshot download;
        StreamBuffer::UsageTelemetrySnapshot device;
        UploadReservation upload_reservation;
    };

    enum class ReadOnlyBufferUsage : u8 {
        Vertex,
        Index,
        Uniform,
        Storage,
        Indirect,
        HleTransfer,
        Count,
    };

    static constexpr u32 CACHING_PAGEBITS = 14;
    static constexpr u64 CACHING_PAGESIZE = u64{1} << CACHING_PAGEBITS;
    static constexpr u64 DEVICE_PAGESIZE = 16_KB;
    static constexpr u64 CACHING_NUMPAGES = u64{1} << (40 - CACHING_PAGEBITS);
    static constexpr u64 BDA_PAGETABLE_SIZE = CACHING_NUMPAGES * sizeof(vk::DeviceAddress);
    static constexpr u64 FAULT_WORDS = CACHING_NUMPAGES / 32;
    static constexpr u64 FAULT_SUMMARY_WORDS = (FAULT_WORDS + 31) / 32;
    static constexpr u64 FAULT_SUMMARY_OFFSET = FAULT_WORDS;
    static constexpr u64 FAULT_BUFFER_WORDS = FAULT_WORDS + FAULT_SUMMARY_WORDS;

    static constexpr s64 DEFAULT_TRIGGER_GC_MEMORY = 1_GB;
    static constexpr s64 DEFAULT_CRITICAL_GC_MEMORY = 2_GB;
    static constexpr s64 TARGET_GC_THRESHOLD = 8_GB;

    struct PageData {
        BufferId buffer_id{};
    };

    struct Traits {
        using Entry = PageData;
        static constexpr size_t AddressSpaceBits = 40;
        static constexpr size_t FirstLevelBits = 16;
        static constexpr size_t PageBits = CACHING_PAGEBITS;
    };
    using PageTable = MultiLevelPageTable<Traits>;

    struct OverlapResult {
        boost::container::small_vector<BufferId, 16> ids;
        VAddr begin;
        VAddr end;
        bool has_stream_leap = false;
    };

    struct PreparedVertexBuffers {
        boost::container::small_vector<vk::VertexInputAttributeDescription2EXT, 32> attributes;
        boost::container::small_vector<vk::VertexInputBindingDescription2EXT, 32> bindings;
        boost::container::small_vector<vk::Buffer, 32> buffers;
        boost::container::small_vector<vk::DeviceSize, 32> offsets;
        boost::container::small_vector<vk::DeviceSize, 32> sizes;
        boost::container::small_vector<vk::DeviceSize, 32> strides;
        bool dynamic_vertex_input{};
        bool valid{true};
    };

    struct PreparedIndexBuffer {
        vk::Buffer buffer{};
        vk::DeviceSize offset{};
        vk::IndexType type{};
    };

public:
    explicit BufferCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         AmdGpu::Liverpool* liverpool, TextureCache& texture_cache,
                         PageManager& tracker);
    ~BufferCache();

    [[nodiscard]] const Buffer* GetGdsBuffer() const noexcept {
        return &gds_buffer;
    }

    [[nodiscard]] BdaPageTableBuffer* GetBdaPageTableBuffer() noexcept {
        return &bda_pagetable_buffer;
    }

    [[nodiscard]] Buffer* GetFaultBuffer() noexcept {
        return fault_manager.GetFaultBuffer();
    }

    [[nodiscard]] Buffer& GetBuffer(BufferId id) {
        return slot_buffers[id];
    }

    StreamBuffer& GetUtilityBuffer(MemoryUsage usage) noexcept {
        switch (usage) {
        case MemoryUsage::Stream:
            return stream_buffer;
        case MemoryUsage::Download:
            return download_buffer;
        case MemoryUsage::DeviceLocal:
            return device_buffer;
        case MemoryUsage::Upload:
            return staging_buffer;
        }
        return staging_buffer;
    }

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    [[nodiscard]] bool IsReadOnlySnapshotBuffer(const Buffer* buffer) const noexcept {
        return buffer == &stream_buffer ||
               (descriptor_snapshot_buffer && buffer == descriptor_snapshot_buffer.get());
    }

    void BeginReadOnlyDescriptorSnapshotBatch() noexcept;

    void RetireReadOnlyDescriptorSnapshotBatch(u64 consuming_tick) noexcept;

    void CancelReadOnlyDescriptorSnapshotBatch() noexcept;
#endif

    void InvalidateMemory(VAddr device_addr, u64 size);

    // Guest-thread-safe ownership publication used only by the default-off
    // HLE memset transaction.  It never touches Scheduler, Vulkan resources,
    // pending transfers, TextureCache, or slot_buffers.
    [[nodiscard]] CpuOwnedWritePublishResult
    TryPublishCpuOwnedWriteIfGpuClean(VAddr device_addr, u64 size);

    void ReadMemory(VAddr device_addr, u64 size, bool is_write = false);

    // Resolve a GPU-authoritative exact-ledger range for a synchronous CPU
    // consumer.  Deferred GNM DMA publishes gpu_modified_ranges before the
    // coarse MemoryTracker can occasionally be observed; this reconciles the
    // two ledgers before the ordinary readback planner selects copy ranges.
    [[nodiscard]] bool ResolveGpuDirtyMemoryForCpuRead(VAddr device_addr,
                                                       u64 size);

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    void ResolvePendingReadback(VAddr device_addr, u64 size);
#endif

    [[nodiscard]] PreparedVertexBuffers PrepareVertexBuffers(
        const Vulkan::GraphicsPipeline& pipeline,
        boost::container::small_vector<vk::BufferMemoryBarrier2, 16>& barriers,
        bool executor_probe_bound = false, u64 executor_probe_sequence = 0);

    void BindPreparedVertexBuffers(const PreparedVertexBuffers& prepared);

    [[nodiscard]] PreparedIndexBuffer PrepareIndexBuffer(
        u32 index_offset, boost::container::small_vector<vk::BufferMemoryBarrier2, 16>& barriers,
        bool executor_probe_bound = false, u64 executor_probe_sequence = 0);

    void BindPreparedIndexBuffer(const PreparedIndexBuffer& prepared);

    void FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds);

    void CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds,
                    bool defer_gnm_dma);

    [[nodiscard]] std::pair<Buffer*, u32> ObtainBuffer(VAddr gpu_addr, u32 size, bool is_written,
                                                       bool is_texel_buffer = false,
                                                       BufferId buffer_id = {},
                                                       bool transfer_range_checked = false,
                                                       bool stable_read_only_candidate = false,
                                                       bool fex_texture_cache_lock_held = false,
                                                       Core::MemoryManager::SnapshotReadGuard*
                                                           external_snapshot_guard = nullptr);

    [[nodiscard]] std::pair<Buffer*, u32> ObtainReadOnlyBuffer(
        VAddr gpu_addr, u32 size, ReadOnlyBufferUsage usage, BufferId fallback_buffer_id = {},
        bool transfer_range_checked = false);

    [[nodiscard]] std::pair<Buffer*, u32> ObtainBufferForImage(
        VAddr gpu_addr, u32 size,
        Core::MemoryManager::SnapshotReadGuard* external_snapshot_guard = nullptr);

    [[nodiscard]] bool IsRegionRegistered(VAddr addr, size_t size);

    [[nodiscard]] bool IsRegionCpuModified(VAddr addr, size_t size);

    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size);

    [[nodiscard]] bool IsRegionGpuModifiedExact(VAddr addr, size_t size) const {
        return gpu_modified_ranges.Intersects(addr, size);
    }

    void EnableGuestReadbackGenerationTracking() noexcept {
        guest_readback_generation_tracking.store(true, std::memory_order_release);
    }

    [[nodiscard]] u64 GetGuestReadbackGeneration() const noexcept {
        return guest_readback_generation.load(std::memory_order_acquire);
    }

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    void PrefetchReadbackWindows();

    void ExecutorProbeGpuBuffer(VAddr device_addr, u32 size, const char* role, u64 sequence);

    void ExecutorProbeBoundGpuBuffer(VAddr guest_addr, u32 size, vk::Buffer bound_buffer,
                                     u64 bound_offset, const char* role, u64 sequence, u32 slot);
#endif

    BufferId FindBuffer(VAddr device_addr, u32 size,
                        bool transfer_range_checked = false);

    void ProcessFaultBuffer();

    [[nodiscard]] bool SynchronizeBuffersInRange(
        VAddr device_addr, u64 size,
        Core::MemoryManager::SnapshotReadGuard* external_snapshot_guard = nullptr,
        u32 compute_diagnostic_id = 0,
        u32 compute_diagnostic_range = 0);

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    struct DmaCleanEpochDiagnosticSerials {
        u64 cpu_publication{};
        u64 topology{};
    };

    // Diagnostic-only conservative monotonic witnesses. Publication attempts
    // are recorded before ownership changes so an overlapping DMA sweep can
    // never observe a false clean epoch. Enabling them does not alter dirty
    // ownership, synchronization, transfer flushing, or buffer topology.
    void EnableDmaCleanEpochDiagnostic() noexcept;
    void RecordDmaCleanEpochCpuPublication() noexcept;
    [[nodiscard]] DmaCleanEpochDiagnosticSerials
    GetDmaCleanEpochDiagnosticSerials() const noexcept;
#endif

    void SynchronizeDmaBuffers();

    void RunGarbageCollector(std::optional<size_t> sampled_device_memory = std::nullopt,
                             bool force_pressure_scan = false);

    [[nodiscard]] ResidencyTelemetrySnapshot
    GetResidencyTelemetrySnapshot() const noexcept {
        const u64 descriptor_bytes =
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            descriptor_snapshot_buffer
                ? static_cast<u64>(descriptor_snapshot_buffer->SizeBytes())
                : 0;
#else
            0;
#endif
        return {
            .resident_bytes = resident_buffer_bytes,
            .utility_bytes = static_cast<u64>(staging_buffer.SizeBytes()) +
                             static_cast<u64>(stream_buffer.SizeBytes()) +
                             static_cast<u64>(download_buffer.SizeBytes()) +
                             static_cast<u64>(device_buffer.SizeBytes()) +
                             static_cast<u64>(gds_buffer.SizeBytes()) +
                             bda_pagetable_buffer.ResidentBytes() +
                             descriptor_bytes,
            .buffer_entries = static_cast<u64>(slot_buffers.size()),
        };
    }

    [[nodiscard]] UtilityRingTelemetrySnapshot
    GetUtilityRingTelemetrySnapshot() const noexcept {
        return {
            .staging = staging_buffer.GetUsageTelemetrySnapshot(),
            .stream = stream_buffer.GetUsageTelemetrySnapshot(),
            .download = download_buffer.GetUsageTelemetrySnapshot(),
            .device = device_buffer.GetUsageTelemetrySnapshot(),
            .upload_reservation = {
                .transaction_count = upload_reservation_transaction_count,
                .completed_transaction_count =
                    upload_reservation_completed_transaction_count,
                .failed_transaction_count =
                    upload_reservation_failed_transaction_count,
                .measurement_count = upload_reservation_measurement_count,
                .retry_count = upload_reservation_retry_count,
                .full_span_candidate_bytes =
                    upload_reservation_full_span_candidate_bytes,
                .dirty_bytes = upload_reservation_dirty_bytes,
                .avoided_bytes = upload_reservation_avoided_bytes,
                .max_full_span_candidate_bytes =
                    upload_reservation_max_full_span_candidate_bytes,
                .max_dirty_bytes = upload_reservation_max_dirty_bytes,
            },
        };
    }

private:
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    void ExecutorMaybeLogBindDedup();

    [[nodiscard]] StreamBuffer* EnsureDescriptorSnapshotBuffer() noexcept;

    void RecordDmaCleanEpochTopologyChange() noexcept;
#endif

    enum class ReadOnlyRenameFallback : u8 {
        GpuModified,
        ImageAlias,
        NotCpuDirty,
        Unregistered,
        NotReadable,
        TooLarge,
        StreamNotCoherent,
        TickBudget,
        RingBusy,
        CopyFailed,
        Count,
    };

    template <typename Func>
    void ForEachBufferInRange(VAddr device_addr, u64 size, Func&& func) {
        buffer_ranges.ForEachInRange(device_addr, size,
                                     [&](u64 page_start, u64 page_end, BufferId id) {
                                         Buffer& buffer = slot_buffers[id];
                                         func(id, buffer);
                                     });
    }

    inline bool IsBufferInvalid(BufferId buffer_id) const {
        return !buffer_id || slot_buffers[buffer_id].is_deleted;
    }

    template <bool async>
    void DownloadBufferMemory(Buffer& buffer, VAddr device_addr, u64 size, bool is_write,
                              const Core::MemoryManager::SnapshotReadGuard* snapshot_guard = nullptr);

    void NotifyGuestReadback() noexcept {
        if (guest_readback_generation_tracking.load(std::memory_order_relaxed)) {
            guest_readback_generation.fetch_add(1, std::memory_order_release);
        }
    }

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    struct RecentReadbackWindow {
        VAddr begin = 0;
        u64 size = 0;
    };

    struct PendingReadbackRange {
        VAddr begin = 0;
        u64 size = 0;
        u64 tick = 0;
    };

    void DownloadBufferMemoryWave(Buffer& primary_buffer, VAddr window_addr, u64 window_size,
                                  VAddr access_addr, u64 access_size, bool is_write);
    [[nodiscard]] bool HasPendingReadback(VAddr device_addr, u64 size);
    [[nodiscard]] bool WaitForPendingReadback(VAddr device_addr, u64 size);

    std::array<RecentReadbackWindow, 64> recent_readback_windows{};
    u32 recent_readback_window_count = 0;
    u64 last_submit_readback_snapshot_tick = 0;
    std::mutex pending_readback_mutex;
    std::vector<PendingReadbackRange> pending_readback_ranges;
#endif

    [[nodiscard]] OverlapResult ResolveOverlaps(VAddr device_addr, u32 wanted_size);

    void JoinOverlap(BufferId new_buffer_id, BufferId overlap_id, bool accumulate_stream_score);

    BufferId CreateBuffer(VAddr device_addr, u32 wanted_size);

    void Register(BufferId buffer_id);

    void Unregister(BufferId buffer_id);

    template <bool insert>
    void ChangeRegister(BufferId buffer_id);

    enum class BufferSyncResult : u8 {
        Ready,
        Failed,
    };

    [[nodiscard]] bool ResolveSnapshotDependencies(
        VAddr device_addr, u64 size,
        Core::MemoryManager::SnapshotReadGuard* external_snapshot_guard = nullptr);

    [[nodiscard]] bool ResolveSnapshotGpuRange(
        VAddr device_addr, u64 size,
        const Core::MemoryManager::SnapshotReadGuard* snapshot_guard = nullptr);

    BufferSyncResult SynchronizeBuffer(Buffer& buffer, VAddr device_addr, u32 size,
                                       bool is_written, bool is_texel_buffer,
                                       bool stable_read_only_candidate = false,
                                       bool fex_texture_cache_lock_held = false,
                                       Core::MemoryManager::SnapshotReadGuard*
                                           external_snapshot_guard = nullptr,
                                       u32 compute_diagnostic_id = 0,
                                       u32 compute_diagnostic_range = 0,
                                       u32 compute_diagnostic_buffer = 0);

    vk::Buffer UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
                            size_t total_size_bytes,
                            std::unique_ptr<Buffer>& transient_buffer);

    bool SynchronizeBufferFromImage(Buffer& buffer, VAddr device_addr, u32 size);

    void WriteDataBuffer(BdaPageTableBuffer& buffer, VAddr address,
                         const void* value, u32 num_bytes);

    void TouchBuffer(const Buffer& buffer);

    void DeleteBuffer(BufferId buffer_id);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    PageManager* page_manager;
    Core::MemoryManager* memory;
    TextureCache& texture_cache;
    FaultManager fault_manager;
    std::unique_ptr<MemoryTracker> memory_tracker;
    StreamBuffer staging_buffer;
    StreamBuffer stream_buffer;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    std::unique_ptr<StreamBuffer> descriptor_snapshot_buffer;
#endif
    StreamBuffer download_buffer;
    StreamBuffer device_buffer;
    Buffer gds_buffer;
    BdaPageTableBuffer bda_pagetable_buffer;
    Common::SlotVector<Buffer> slot_buffers;
    // Cache-owned device-local bytes only. Whole-device residency includes
    // textures, staging rings and driver allocations and must not be charged
    // independently to every cache.
    u64 resident_buffer_bytes = 0;
    // Bytes from cache-buffer generations which have been unregistered but
    // whose Vulkan allocations must remain alive until their recorded uses and
    // overlap copies complete.  This is separate from resident_buffer_bytes:
    // both generations are charged by KGSL during a merge burst even though
    // only the replacement is visible through the cache page table.
    std::atomic<u64> retiring_buffer_bytes{0};
    u64 trigger_gc_memory = 0;
    u64 critical_gc_memory = 0;
    u64 gc_tick = 0;
    // Render-thread-owned O(1) counters. They remain zero unless a typed
    // ExactDirtyRanges policy reaches an upload transaction.
    u64 upload_reservation_transaction_count{};
    u64 upload_reservation_completed_transaction_count{};
    u64 upload_reservation_failed_transaction_count{};
    u64 upload_reservation_measurement_count{};
    u64 upload_reservation_retry_count{};
    u64 upload_reservation_full_span_candidate_bytes{};
    u64 upload_reservation_dirty_bytes{};
    u64 upload_reservation_avoided_bytes{};
    u64 upload_reservation_max_full_span_candidate_bytes{};
    u64 upload_reservation_max_dirty_bytes{};
    std::atomic<bool> guest_readback_generation_tracking{false};
    std::atomic<u64> guest_readback_generation{0};
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    std::atomic<bool> dma_clean_epoch_diagnostic_enabled{false};
    std::atomic<u64> dma_clean_epoch_cpu_publication_serial{1};
    std::atomic<u64> dma_clean_epoch_topology_serial{1};
    vk::CommandBuffer executor_vertex_bind_cmdbuf{};
    u64 executor_vertex_bind_recording_tick{};
    u64 executor_vertex_bind_rendering_epoch{};
    u64 executor_vertex_bind_graphics_pipeline_epoch{};
    u64 executor_vertex_input_bind_epoch{};
    u64 executor_vertex_buffer_bind_epoch{};
    PreparedVertexBuffers executor_bound_vertex_state{};
    bool executor_vertex_input_bind_valid{};
    bool executor_vertex_buffer_bind_valid{};
    vk::CommandBuffer executor_index_bind_cmdbuf{};
    u64 executor_index_bind_recording_tick{};
    u64 executor_index_bind_rendering_epoch{};
    u64 executor_index_bind_graphics_pipeline_epoch{};
    u64 executor_index_buffer_bind_epoch{};
    PreparedIndexBuffer executor_bound_index_state{};
    bool executor_index_buffer_bind_valid{};
    u64 executor_vertex_input_bind_attempts{};
    u64 executor_vertex_input_bind_hits{};
    u64 executor_vertex_buffer_bind_attempts{};
    u64 executor_vertex_buffer_bind_hits{};
    u64 executor_index_buffer_bind_attempts{};
    u64 executor_index_buffer_bind_hits{};
    u64 executor_bind_dedup_next_log{16384};
    std::array<u64, static_cast<std::size_t>(ReadOnlyBufferUsage::Count)>
        read_only_rename_usage_counts{};
    std::array<u64, static_cast<std::size_t>(ReadOnlyBufferUsage::Count)>
        read_only_rename_success_usage_counts{};
    std::array<u64, static_cast<std::size_t>(ReadOnlyRenameFallback::Count)>
        read_only_rename_fallback_counts{};
    u64 read_only_rename_attempts{};
    u64 read_only_rename_ranges{};
    u64 read_only_rename_bytes{};
    u64 read_only_rename_tick{};
    u64 read_only_rename_tick_bytes{};
    StreamBuffer::Lease descriptor_snapshot_batch_lease{};
    bool descriptor_snapshot_batch_active{};
    bool descriptor_snapshot_buffer_attempted{};
    u64 descriptor_snapshot_selected_bytes{};
    u64 descriptor_snapshot_batches_started{};
    u64 descriptor_snapshot_batches_retired{};
    u64 descriptor_snapshot_batches_canceled{};
    u64 descriptor_snapshot_allocation_failures{};
    u64 descriptor_snapshot_exhaustions{};
    u64 descriptor_snapshot_fallback_attempts{};
    u64 descriptor_snapshot_fallback_successes{};
    u64 descriptor_snapshot_high_water_bytes{};
    u64 descriptor_snapshot_max_batch_bytes{};
    u64 descriptor_snapshot_batch_initial_tick{};
    u64 descriptor_snapshot_max_tick_lag{};
    u64 stable_canonical_requests{};
    u64 stable_canonical_uploads{};
    u64 stable_canonical_bytes{};
    u64 stable_canonical_promotions{};
#endif
    Common::LeastRecentlyUsedCache<BufferId, u64> lru_cache;
    RangeSet gpu_modified_ranges;
    SplitRangeMap<BufferId> buffer_ranges;
    PageTable page_table;
};

}
