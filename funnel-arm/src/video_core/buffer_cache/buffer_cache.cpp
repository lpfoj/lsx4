#include "persistent_upload_write203.h"
#include "persistent_stream_retirement.h"
// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <span>
#include <thread>
#include <execinfo.h>
#include <mutex>
#include <string>
#include <cstring>
#include <tuple>
#include <utility>
#include <vector>
#if defined(__ANDROID__)
#include <android/log.h>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#endif
#include "common/alignment.h"
#include "common/content_fingerprint.h"
#include "common/debug.h"
#include "common/scope_exit.h"
#include "core/memory.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/page_manager.h"
#include "video_core/mobile_gpu_optimization.h"
#include "video_core/readback_optimization.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/vulkan_driver_optimization.h"
#include <vk_mem_alloc.h>
#include "ordered_mapped_readback.h"
#include "producer_readback_bridge.h"
#if defined(LSX4_APPLE_RUNTIME)
#include "adaptive_range_transfer.h"
#endif

namespace VideoCore {

std::atomic<u32> claude_hot_written{0};
thread_local bool claude_guest_readback_origin = false;
thread_local bool claude_guest_readback_async_issued = false;
std::atomic<u64> claude_hot_marks{0};
std::atomic<u64> claude_hot_flushes{0};

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
namespace {

constexpr u32 ExecutorSignedZeroPositionBytes = 48;
std::atomic<bool> executor_signed_zero_vbuf_provenance_claimed{false};

bool ExecutorComputeDmaSyncDiagnosticEnabled() noexcept {
    static const bool enabled = [] {
        constexpr std::array<const char*, 2> markers{
            "/data/data/app.lsx4.android/files/lsx4-home/"
            "diag-compute-dispatch-phase",
            "/data/user/0/app.lsx4.android/files/lsx4-home/"
            "diag-compute-dispatch-phase",
        };
        for (const char* const path : markers) {
            if (std::FILE* const marker = std::fopen(path, "rb")) {
                std::fclose(marker);
                return true;
            }
        }
        return false;
    }();
    return enabled;
}

bool ExecutorSignedZeroVbufWriteProvenanceRequested() noexcept {
    static const bool requested = [] {
        constexpr std::array<const char*, 2> markers{
            "/data/data/app.lsx4.android/files/lsx4-home/"
            "diag-signed-zero-vbuf-write-provenance",
            "/data/user/0/app.lsx4.android/files/lsx4-home/"
            "diag-signed-zero-vbuf-write-provenance",
        };
        for (const char* const path : markers) {
            if (std::FILE* const marker = std::fopen(path, "rb")) {
                std::fclose(marker);
                return true;
            }
        }
        return false;
    }();
    return requested;
}

bool ExecutorIsSignedZeroPositionPayload(
    const std::span<const u8> bytes) noexcept {
    if (bytes.size() != ExecutorSignedZeroPositionBytes) {
        return false;
    }
    for (u32 index = 0; index < bytes.size() / sizeof(u32); ++index) {
        u32 word = 0;
        std::memcpy(&word, bytes.data() + index * sizeof(word), sizeof(word));
        const u32 expected = (index & 1u) != 0 ? 0x80000000u : 0u;
        if (word != expected) {
            return false;
        }
    }
    return true;
}

bool ExecutorBufferReadbackContractTraceEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EXECUTOR_READBACK_CONTRACT_TRACE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

bool ExecutorSubmitReadbackPrefetchEnabled() {
    static const bool enabled = [] {
        constexpr const char* DisableMarker =
            "/data/data/app.lsx4.android/files/lsx4-home/"
            "run-bisect-disable-submit-readback-prefetch";
        if (std::FILE* marker = std::fopen(DisableMarker, "rb")) {
            std::fclose(marker);
            return false;
        }
        return true;
    }();
    return enabled;
}

bool ExecutorReadOnlyRenameEnabled() {
    static const bool enabled = [] {
        constexpr const char* DisableMarker =
            "/data/data/app.lsx4.android/files/lsx4-home/"
            "run-bisect-disable-read-only-rename";
        if (std::FILE* marker = std::fopen(DisableMarker, "rb")) {
            std::fclose(marker);
            return false;
        }
        return true;
    }();
    return enabled;
}

bool ExecutorStableCanonicalStreamingEnabled() {
    static const bool enabled = [] {
        constexpr const char* EnableMarker =
            "/data/data/app.lsx4.android/files/lsx4-home/"
            "run-enable-stable-canonical-streaming";
        if (std::FILE* marker = std::fopen(EnableMarker, "rb")) {
            std::fclose(marker);
            return true;
        }
        return false;
    }();
    return enabled && Config::managedGpuFastPath();
}

bool ExecutorExactDemandReadbackWindowEnabled() {
    static const bool enabled = [] {
        constexpr const char* EnableMarker =
            "/data/data/app.lsx4.android/files/lsx4-home/"
            "run-enable-exact-demand-readback-window";
        if (std::FILE* marker = std::fopen(EnableMarker, "rb")) {
            std::fclose(marker);
            return true;
        }
        return false;
    }();
    return enabled;
}

bool ExecutorSnapshotDependencyResolutionEnabled() {
    static const bool enabled = [] {
        constexpr const char* DisableMarker =
            "/data/data/app.lsx4.android/files/lsx4-home/"
            "run-bisect-disable-snapshot-dependencies";
        if (std::FILE* marker = std::fopen(DisableMarker, "rb")) {
            std::fclose(marker);
            return false;
        }
        return true;
    }();
    return enabled;
}

bool ExecutorPhysicalAliasSnapshotEnabled() {
    static const bool enabled = [] {
        constexpr const char* DisableMarker =
            "/data/data/app.lsx4.android/files/lsx4-home/"
            "run-bisect-disable-physical-alias-snapshot";
        if (std::FILE* marker = std::fopen(DisableMarker, "rb")) {
            std::fclose(marker);
            return false;
        }
        return true;
    }();
    return enabled;
}

bool ExecutorUsesExactA750TurnipDedicatedBdaWindows(
    const Vulkan::Instance& instance) noexcept {
    static const bool requested = [] {
        constexpr std::array<const char*, 2> markers{
            "/data/data/app.lsx4.android/files/lsx4-home/"
            "run-sm8650-a750-turnip-dedicated-bda-window-ab",
            "/data/user/0/app.lsx4.android/files/lsx4-home/"
            "run-sm8650-a750-turnip-dedicated-bda-window-ab",
        };
        for (const char* const path : markers) {
            if (std::FILE* const marker = std::fopen(path, "rb")) {
                std::fclose(marker);
                return true;
            }
        }
        return false;
    }();
    const auto& profile = instance.GetCapabilityProfile();
    return requested && profile.exact_mesa_turnip_a750_d9_lineage;
}

bool ExecutorShouldLogDedicatedBdaWindowOrdinal(const u64 ordinal) noexcept {
    return ordinal <= 16 || (ordinal & (ordinal - 1)) == 0;
}

std::atomic<u64> executor_dedicated_bda_window_count{0};

bool ExecutorUsesExactA750TurnipBufferRetirementDrain(
    const Vulkan::Instance& instance) noexcept {
    const auto& profile = instance.GetCapabilityProfile();
    return profile.exact_mesa_turnip_a750_d9_lineage &&
           profile.memory_policy.residency_retirement ==
               Vulkan::ResidencyRetirementMode::CoordinatedSustainedNonBlocking;
}

std::atomic<u64> executor_a750_buffer_retirement_drain_count{0};

bool ExecutorReadOnlyRenameTraceEnabled() {
    static const bool enabled = [] {
        constexpr const char* TraceMarker =
            "/data/data/app.lsx4.android/files/lsx4-home/diag-vk-gpu-timeline";
        if (std::FILE* marker = std::fopen(TraceMarker, "rb")) {
            std::fclose(marker);
            return true;
        }
        return false;
    }();
    return enabled;
}

bool ExecutorDescriptorSnapshotArenaEnabled() {
    static const bool enabled = [] {
        constexpr std::array<const char*, 2> EnableMarkers{
            "/data/data/app.lsx4.android/files/lsx4-home/"
            "run-enable-descriptor-snapshot-arena",
            "/data/user/0/app.lsx4.android/files/lsx4-home/"
            "run-enable-descriptor-snapshot-arena",
        };
        for (const char* const path : EnableMarkers) {
            if (std::FILE* const marker = std::fopen(path, "rb")) {
                std::fclose(marker);
                return true;
            }
        }
        return false;
    }();
    return enabled && Config::managedGpuFastPath();
}

bool ExecutorReadOnlyRenameUsageEnabled(const BufferCache::ReadOnlyBufferUsage usage) {
    if (usage == BufferCache::ReadOnlyBufferUsage::Uniform ||
        usage == BufferCache::ReadOnlyBufferUsage::Storage) {
        // The common StreamBuffer remains forbidden for descriptor backing.
        // The experimental path is limited to the dedicated non-wrapping
        // arena: WithCpuSnapshot proves source coherence, while its exact lease
        // is retired by the consuming draw/dispatch timeline tick.
        if (!ExecutorDescriptorSnapshotArenaEnabled()) {
            return false;
        }
    }

    constexpr std::array<const char*, static_cast<std::size_t>(
                                          BufferCache::ReadOnlyBufferUsage::Count)>
        DisableMarkers{
            "/data/data/app.lsx4.android/files/lsx4-home/"
            "run-bisect-disable-read-only-rename-vertex",
            "/data/data/app.lsx4.android/files/lsx4-home/"
            "run-bisect-disable-read-only-rename-index",
            "/data/data/app.lsx4.android/files/lsx4-home/"
            "run-bisect-disable-read-only-rename-uniform",
            "/data/data/app.lsx4.android/files/lsx4-home/"
            "run-bisect-disable-read-only-rename-storage",
            "/data/data/app.lsx4.android/files/lsx4-home/"
            "run-bisect-disable-read-only-rename-indirect",
            "/data/data/app.lsx4.android/files/lsx4-home/"
            "run-bisect-disable-read-only-rename-hle",
        };
    const auto read_mask = [&] {
        u32 result = (1u << DisableMarkers.size()) - 1u;
        for (std::size_t i = 0; i < DisableMarkers.size(); ++i) {
            if (std::FILE* marker = std::fopen(DisableMarkers[i], "rb")) {
                std::fclose(marker);
                result &= ~(1u << i);
            }
        }
        return result;
    };
    const u32 usage_bit = 1u << static_cast<std::size_t>(usage);
    static const u32 startup_mask = read_mask();
    // Production runs intentionally pin the bisection policy for the process
    // lifetime.  Polling exists only so an interactive diagnostic session can
    // change markers without restarting; paying an atomic RMW on every buffer
    // resource is otherwise pure overhead.
    if (VideoCore::IsExecutorLiveQuiet()) {
        return (startup_mask & usage_bit) != 0;
    }
    static std::atomic<u32> enabled_mask{startup_mask};
    static std::atomic<u64> poll_count{0};
    // Runtime polling is intentionally sparse: it makes type-by-type GPU
    // corruption bisection possible without another multi-minute title load,
    // while four marker probes per 65536 resources are immeasurable in play.
    if ((poll_count.fetch_add(1, std::memory_order_relaxed) & 0xffffu) == 0xffffu) {
        enabled_mask.store(read_mask(), std::memory_order_relaxed);
    }
    return (enabled_mask.load(std::memory_order_relaxed) & usage_bit) != 0;
}

bool ExecutorShouldLogBufferReadbackOrdinal(const u64 ordinal) {
    return ordinal <= 64 || (ordinal & (ordinal - 1)) == 0;
}

std::atomic<u64> executor_buffer_read_request_count{0};
std::atomic<u64> executor_buffer_download_count{0};
std::atomic<u64> executor_buffer_empty_download_count{0};
std::atomic<u64> executor_buffer_download_bytes{0};

constexpr u64 ExecutorPreciseReadbackWindow =
    16 * static_cast<u64>(TRACKER_BYTES_PER_PAGE);
std::chrono::steady_clock::time_point ExecutorReadbackDiagnosticStart() {
    if (!IsReadbackOptimizationStatsEnabled() && !IsReadbackClassificationEnabled()) {
        return {};
    }
    return std::chrono::steady_clock::now();
}

u64 ExecutorElapsedNs(const std::chrono::steady_clock::time_point start) {
    if (start == std::chrono::steady_clock::time_point{}) {
        return 0;
    }
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count());
}

void ExecutorMaybeLogReadbackOptimizationStats() {
    // The aggregate is diagnostic-only. Avoid loading the complete atomic
    // counter bundle unless its explicit process-lifetime marker was present.
    if (!IsReadbackOptimizationStatsEnabled()) {
        return;
    }
    const auto stats = GetReadbackOptimizationStats();
    static std::atomic<u64> next_log{1};
    u64 expected = next_log.load(std::memory_order_relaxed);
    if (stats.snapshots < expected) {
        return;
    }
    const u64 following = expected == 1 ? 256 : expected + 256;
    if (!next_log.compare_exchange_strong(expected, following, std::memory_order_relaxed)) {
        return;
    }
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_READBACK_BATCH_STATS] enabled=%u mode=%d snapshots=%llu "
        "submit=%llu demand=%llu ranges=%llu pages=%llu copyCommands=%llu "
        "copyRegions=%llu sourceBuffers=%llu mergedRegions=%llu timelineWaits=%llu "
        "bytes=%llu cpuPlanMs=%.3f "
        "cpuPublishMs=%.3f cpuWaitMs=%.3f gpuCompletionMs=%.3f fallbacks=%llu "
        "publishFailures=%llu duplicateSubmitSkips=%llu overflowAllocations=%llu "
        "demandFlushes=%llu baselineRequests=%llu sidecarLabels=%llu sidecarWaits=%llu",
        Config::managedReadbackBatching() ? 1u : 0u, Config::getReadbacksMode(),
        static_cast<unsigned long long>(stats.snapshots),
        static_cast<unsigned long long>(stats.submit_snapshots),
        static_cast<unsigned long long>(stats.demand_snapshots),
        static_cast<unsigned long long>(stats.ranges),
        static_cast<unsigned long long>(stats.pages),
        static_cast<unsigned long long>(stats.copy_commands),
        static_cast<unsigned long long>(stats.copy_regions),
        static_cast<unsigned long long>(stats.source_buffers),
        static_cast<unsigned long long>(stats.regions_merged),
        static_cast<unsigned long long>(stats.timeline_waits),
        static_cast<unsigned long long>(stats.bytes),
        static_cast<double>(stats.cpu_plan_ns) / 1'000'000.0,
        static_cast<double>(stats.cpu_publish_ns) / 1'000'000.0,
        static_cast<double>(stats.cpu_wait_ns) / 1'000'000.0,
        static_cast<double>(stats.gpu_completion_ns) / 1'000'000.0,
        static_cast<unsigned long long>(stats.fallbacks),
        static_cast<unsigned long long>(stats.publication_failures),
        static_cast<unsigned long long>(stats.duplicate_submit_skips),
        static_cast<unsigned long long>(stats.overflow_allocations),
        static_cast<unsigned long long>(stats.demand_flushes),
        static_cast<unsigned long long>(stats.baseline_requests),
        static_cast<unsigned long long>(stats.sidecar_labels),
        static_cast<unsigned long long>(stats.sidecar_waits));
}

}
#endif

static constexpr size_t DataShareBufferSize = 64_KB;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
static constexpr size_t StagingBufferSize = 192_MB;
static constexpr size_t DownloadBufferSize = 16_MB;
static constexpr size_t UboStreamBufferSize = 32_MB;
static constexpr size_t DescriptorSnapshotBufferSize = 64_MB;
static constexpr size_t DeviceBufferSize = 64_MB;
#else
static constexpr size_t StagingBufferSize = 512_MB;
static constexpr size_t DownloadBufferSize = 32_MB;
static constexpr size_t UboStreamBufferSize = 64_MB;
static constexpr size_t DeviceBufferSize = 128_MB;
#endif

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         AmdGpu::Liverpool* liverpool_, TextureCache& texture_cache_,
                         PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      page_manager{&tracker}, memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      fault_manager{instance, scheduler, *this, CACHING_PAGEBITS, CACHING_NUMPAGES},
      staging_buffer{instance, scheduler, MemoryUsage::Upload, StagingBufferSize},
      stream_buffer{instance, scheduler, MemoryUsage::Stream, UboStreamBufferSize},
      download_buffer{instance, scheduler, MemoryUsage::Download, DownloadBufferSize},
      device_buffer{instance, scheduler, MemoryUsage::DeviceLocal, DeviceBufferSize},
      gds_buffer{instance, scheduler, MemoryUsage::Stream, 0, AllFlags, DataShareBufferSize},
      bda_pagetable_buffer{instance, scheduler, BDA_PAGETABLE_SIZE},
      page_table{instance_.GetCapabilityProfile().optimization_a750_host_metadata_compaction
                     ? 256u
                     : 8192u} {
    PortableGpu::ResetProducerCache(this);
    Vulkan::SetObjectName(instance.GetDevice(), gds_buffer.Handle(), "GDS Buffer");

    memory_tracker = std::make_unique<MemoryTracker>(tracker);

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_READBACK_BATCH_CONFIG] enabled=%u effectiveMode=%d "
        "configuredBy=managed_optimization_7",
        Config::managedReadbackBatching() ? 1u : 0u, Config::getReadbacksMode());
    if (ExecutorExactDemandReadbackWindowEnabled()) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_EXACT_DEMAND_READBACK_CONFIG] enabled=1 requestWindow=exact");
    }
#endif

    std::memset(gds_buffer.mapped_data.data(), 0, DataShareBufferSize);

    const auto null_id =
        slot_buffers.insert(instance, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, 16);
    ASSERT(null_id.index == 0);
    const vk::Buffer& null_buffer = slot_buffers[null_id].buffer;
    Vulkan::SetObjectName(instance.GetDevice(), null_buffer, "Null Buffer");

    slot_buffers[null_id].Fill(0, 16, 0);

    if (!instance.CanReportMemoryUsage()) {
        trigger_gc_memory = DEFAULT_TRIGGER_GC_MEMORY;
        critical_gc_memory = DEFAULT_CRITICAL_GC_MEMORY;
        return;
    }

    const s64 device_local_memory = static_cast<s64>(instance.GetTotalMemoryBudget());
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const auto adaptive_threshold = [device_local_memory](
                                        const s64 numerator,
                                        const s64 denominator,
                                        const s64 floor,
                                        const s64 ceiling) {
        const s64 proportional = device_local_memory * numerator / denominator;
        return static_cast<u64>(
            std::min(device_local_memory, std::clamp(proportional, floor, ceiling)));
    };
    const bool managed_mobile_gpu =
        VideoCore::IsManagedMobileGpuOptimization();
    trigger_gc_memory = managed_mobile_gpu
                            ? adaptive_threshold(7, 20, 512_MB, 1_GB + 256_MB)
                            : adaptive_threshold(9, 20, 768_MB, 1_GB + 512_MB);
    critical_gc_memory = std::min<u64>(
        device_local_memory,
        std::max<u64>(
            trigger_gc_memory + 256_MB,
            managed_mobile_gpu
                ? adaptive_threshold(7, 10, 1_GB, 2_GB + 256_MB)
                : adaptive_threshold(3, 4, 1_GB + 256_MB, 2_GB + 512_MB)));
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_BUFFER_GC_POLICY] budgetMiB=%lld triggerMiB=%llu criticalMiB=%llu "
        "accounting=cache-owned dirtyPolicy=retain",
        static_cast<long long>(device_local_memory / 1_MB),
        static_cast<unsigned long long>(trigger_gc_memory / 1_MB),
        static_cast<unsigned long long>(critical_gc_memory / 1_MB));
    return;
#endif
    const s64 min_spacing_expected = device_local_memory - 1_GB;
    const s64 min_spacing_critical = device_local_memory - 512_MB;
    const s64 mem_threshold = std::min<s64>(device_local_memory, TARGET_GC_THRESHOLD);
    const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
    const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
    trigger_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_expected, min_spacing_expected),
                      DEFAULT_TRIGGER_GC_MEMORY));
    critical_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_critical, min_spacing_critical),
                      DEFAULT_CRITICAL_GC_MEMORY));
}

BufferCache::~BufferCache() { PortableGpu::ResetProducerCache(this); }

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
StreamBuffer* BufferCache::EnsureDescriptorSnapshotBuffer() noexcept {
    if (descriptor_snapshot_buffer || descriptor_snapshot_buffer_attempted) {
        return descriptor_snapshot_buffer.get();
    }
    descriptor_snapshot_buffer_attempted = true;
    constexpr std::array<u64, 3> CandidateSizes{
        DescriptorSnapshotBufferSize, 32_MB, 16_MB};
    for (const u64 candidate_size : CandidateSizes) {
        std::unique_ptr<StreamBuffer> candidate{
            new (std::nothrow) StreamBuffer(std::nothrow, instance, scheduler,
                                           MemoryUsage::Stream, candidate_size)};
        if (!candidate || !candidate->IsValid() || candidate->SizeBytes() == 0 ||
            !candidate->is_coherent || candidate->mapped_data.empty()) {
            ++descriptor_snapshot_allocation_failures;
            continue;
        }
        descriptor_snapshot_selected_bytes = candidate_size;
        descriptor_snapshot_buffer = std::move(candidate);
        break;
    }
    return descriptor_snapshot_buffer.get();
}

void BufferCache::BeginReadOnlyDescriptorSnapshotBatch() noexcept {
    ASSERT(!descriptor_snapshot_batch_active);
    if (descriptor_snapshot_batch_active) {
        return;
    }
    ++descriptor_snapshot_batches_started;
    descriptor_snapshot_batch_active = true;
    BeginDescriptorProtectionCohortDiagnosticBatch();
}

void BufferCache::RetireReadOnlyDescriptorSnapshotBatch(const u64 consuming_tick) noexcept {
    if (!descriptor_snapshot_batch_active) {
        return;
    }
    if (descriptor_snapshot_batch_lease && descriptor_snapshot_buffer) {
        descriptor_snapshot_buffer->RetireLease(descriptor_snapshot_batch_lease,
                                                consuming_tick);
        ++descriptor_snapshot_batches_retired;
        descriptor_snapshot_max_tick_lag =
            std::max(descriptor_snapshot_max_tick_lag,
                     consuming_tick - std::min(consuming_tick,
                                                descriptor_snapshot_batch_initial_tick));
    }
    descriptor_snapshot_batch_lease = {};
    descriptor_snapshot_batch_initial_tick = 0;
    EndDescriptorProtectionCohortDiagnosticBatch();
    descriptor_snapshot_batch_active = false;
}

void BufferCache::CancelReadOnlyDescriptorSnapshotBatch() noexcept {
    const bool was_active = descriptor_snapshot_batch_active;
    if (descriptor_snapshot_batch_lease && descriptor_snapshot_buffer) {
        descriptor_snapshot_buffer->CancelLease(descriptor_snapshot_batch_lease);
        ++descriptor_snapshot_batches_canceled;
    }
    descriptor_snapshot_batch_lease = {};
    descriptor_snapshot_batch_initial_tick = 0;
    descriptor_snapshot_batch_active = false;
    if (was_active) {
        EndDescriptorProtectionCohortDiagnosticBatch();
    }
}
#endif

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
void BufferCache::EnableDmaCleanEpochDiagnostic() noexcept {
    dma_clean_epoch_diagnostic_enabled.store(true, std::memory_order_release);
}

BufferCache::DmaCleanEpochDiagnosticSerials
BufferCache::GetDmaCleanEpochDiagnosticSerials() const noexcept {
    return {
        .cpu_publication =
            dma_clean_epoch_cpu_publication_serial.load(std::memory_order_acquire),
        .topology = dma_clean_epoch_topology_serial.load(std::memory_order_acquire),
    };
}

void BufferCache::RecordDmaCleanEpochCpuPublication() noexcept {
    if (dma_clean_epoch_diagnostic_enabled.load(std::memory_order_acquire)) {
        dma_clean_epoch_cpu_publication_serial.fetch_add(1, std::memory_order_release);
    }
}

void BufferCache::RecordDmaCleanEpochTopologyChange() noexcept {
    if (dma_clean_epoch_diagnostic_enabled.load(std::memory_order_acquire)) {
        dma_clean_epoch_topology_serial.fetch_add(1, std::memory_order_release);
    }
}
#endif

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size) {
    SCOPE_EXIT { PortableGpu::InvalidateProducerRange(this,device_addr,size); };
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    RecordDmaCleanEpochCpuPublication();
#endif
    scheduler.FlushPendingBufferTransfersForRange(device_addr, size);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    (void)WaitForPendingReadback(device_addr, size);
#endif
    if (!IsRegionRegistered(device_addr, size)) {
        // The range may have been unregistered after its MemoryTracker state
        // installed the buffer-cache write watcher.  A guest write is the
        // authoritative CPU publication even when no Vulkan buffer remains;
        // reconcile that stale ownership before returning.  IteratePages<false>
        // makes this a no-op when the tracker never owned the range.
        memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        return;
    }
    liverpool->NotifyMemoryPublicationForExecutor(device_addr, size);
    memory_tracker->InvalidateRegion(
        device_addr, size, [this, device_addr, size] { ReadMemory(device_addr, size, true); });
}

CpuOwnedWritePublishResult BufferCache::TryPublishCpuOwnedWriteIfGpuClean(
    const VAddr device_addr, const u64 size) {
    // This is deliberately a MemoryTracker-only operation.  In particular,
    // do not consult pending transfers/readbacks here: those APIs are
    // GpuComm-affine and would reintroduce the synchronous wait this marker is
    // measuring.  GPU destinations publish PageManager/RegionManager dirty
    // ownership before being queued; both are rechecked by the transaction.
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    RecordDmaCleanEpochCpuPublication();
#endif
    return memory_tracker->TryPublishCpuOwnedWriteIfGpuClean(device_addr, size);
}

namespace {
struct ClaudeReadbackStats {
    bool enabled = [] {
        const char* value = std::getenv("EXECUTOR_CLAUDE_READBACK_STATS");
        return value && value[0] == '1';
    }();
    std::atomic<u64> write_n{}, read_n{}, write_bytes{}, read_bytes{};
    std::atomic<u64> write_ns{}, read_ns{}, small_write_n{};
    std::atomic<long long> window_ms{};
    std::atomic<u64> waves{}, mapped{}, producer{}, producer_done{}, lag_sum{}, lag_max{};
    std::atomic<u64> wait_plain_ns{}, wait_producer_ns{}, wait_plain_n{}, wait_producer_n{};
    std::atomic<u64> chunked{}, slow_waits{};
    std::atomic<u64> plain_pending{}, plain_unknown{}, plain_pending_ns{}, plain_unknown_ns{};
    std::atomic<u64> plain_write{}, plain_gpu_idle{};
    std::atomic<u64> split_n{}, split_flush_ns{}, split_gpu_ns{}, split_retire_ns{}, split_timeouts{};
    std::atomic<u64> async_issued{}, async_fallback{}, async_waits{}, async_wait_ns{};
    std::atomic<u64> async_timeouts{}, async_flush_cmds{};
    std::atomic<u64> origin_gpu_n{}, origin_gpu_ns{}, origin_guest_n{}, origin_guest_ns{};
};

struct ClaudeSiteEntry {
    void* frames[8]{};
    int depth = 0;
    u64 count = 0;
    u64 ns = 0;
    u64 bytes = 0;
};
struct ClaudeSites {
    bool enabled = [] {
        const char* value = std::getenv("EXECUTOR_CLAUDE_READBACK_SITES");
        return value && value[0] == '1';
    }();
    std::mutex mutex;
    std::array<ClaudeSiteEntry, 48> entries{};
    u64 dropped = 0;
    long long window_ms = 0;
};
ClaudeSites g_claude_sites;

void ClaudeRecordSite(void* const* frames, int depth, u64 ns, u64 bytes) {
    auto& s = g_claude_sites;
    std::scoped_lock lock{s.mutex};
    for (auto& e : s.entries) {
        if (e.count != 0 && e.depth == depth &&
            std::equal(frames, frames + depth, e.frames)) {
            ++e.count;
            e.ns += ns;
            e.bytes += bytes;
            return;
        }
    }
    for (auto& e : s.entries) {
        if (e.count == 0) {
            std::copy(frames, frames + depth, e.frames);
            e.depth = depth;
            e.count = 1;
            e.ns = ns;
            e.bytes = bytes;
            return;
        }
    }
    ++s.dropped;
}

void ClaudeMaybePrintSites() {
    auto& s = g_claude_sites;
    const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    std::array<ClaudeSiteEntry, 48> snapshot{};
    u64 dropped = 0;
    {
        std::scoped_lock lock{s.mutex};
        if (s.window_ms == 0) {
            s.window_ms = now;
            return;
        }
        if (now - s.window_ms < 4000) {
            return;
        }
        s.window_ms = now;
        snapshot = s.entries;
        dropped = s.dropped;
        s.entries = {};
        s.dropped = 0;
    }
    std::sort(snapshot.begin(), snapshot.end(),
              [](const ClaudeSiteEntry& a, const ClaudeSiteEntry& z) { return a.ns > z.ns; });
    for (int i = 0; i < 6 && snapshot[i].count != 0; ++i) {
        const auto& e = snapshot[i];
        char** names = backtrace_symbols(const_cast<void* const*>(e.frames), e.depth);
        std::string joined;
        for (int f = 0; f < e.depth && names != nullptr; ++f) {
            const char* name = names[f];
            const char* last_space = std::strrchr(name, ' ');
            const char* plus = last_space;
            // "<idx> <image> <addr> <symbol> + <off>": keep "<symbol>+<off>".
            int spaces = 0;
            const char* p = name;
            for (; *p != '\0' && spaces < 3; ++p) {
                if (*p == ' ' && p[1] != ' ') ++spaces;
            }
            (void)plus;
            joined += (f == 0 ? "" : " <- ");
            joined += std::string(p).substr(0, 90);
        }
        std::free(names);
        std::fprintf(stderr, "[CLAUDE_READBACK_SITE] rank=%d n=%llu ms=%.1f kb=%.1f %s\n", i,
                     static_cast<unsigned long long>(e.count), e.ns / 1e6, e.bytes / 1024.0,
                     joined.c_str());
    }
    if (dropped != 0) {
        std::fprintf(stderr, "[CLAUDE_READBACK_SITE] dropped=%llu\n",
                     static_cast<unsigned long long>(dropped));
    }
}

bool ClaudeAsyncReadbackNow() {
    static const int mode = [] {
        const char* value = std::getenv("EXECUTOR_CLAUDE_ASYNC_READBACK");
        return value ? std::atoi(value) : 0;
    }();
    return mode == 1 || (mode == 2 && (static_cast<long long>(std::time(nullptr)) / 5) % 2 == 0);
}
ClaudeReadbackStats g_claude_readback;
void ClaudeReadbackMaybePrint() {
    const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    long long start = g_claude_readback.window_ms.load(std::memory_order_relaxed);
    if (start == 0) {
        g_claude_readback.window_ms.compare_exchange_strong(start, now);
        return;
    }
    if (now - start < 2000 || !g_claude_readback.window_ms.compare_exchange_strong(start, now)) {
        return;
    }
    auto& r = g_claude_readback;
    const u64 wn = r.write_n.exchange(0), rn = r.read_n.exchange(0);
    const u64 wb = r.write_bytes.exchange(0), rb = r.read_bytes.exchange(0);
    const u64 wns = r.write_ns.exchange(0), rns = r.read_ns.exchange(0), sw = r.small_write_n.exchange(0);
    std::fprintf(stderr,
                 "[CLAUDE_READBACK] windowMs=%lld write=%llu(%.1fKB avg, small<=4K %llu) writeMs=%.1f "
                 "read=%llu(%.1fKB avg) readMs=%.1f\n",
                 now - start, static_cast<unsigned long long>(wn), wn ? wb / 1024.0 / wn : 0.0,
                 static_cast<unsigned long long>(sw), wns / 1e6, static_cast<unsigned long long>(rn),
                 rn ? rb / 1024.0 / rn : 0.0, rns / 1e6);
    const u64 waves = r.waves.exchange(0), mapped = r.mapped.exchange(0);
    const u64 producer = r.producer.exchange(0), producer_done = r.producer_done.exchange(0);
    const u64 lag_sum = r.lag_sum.exchange(0), lag_max = r.lag_max.exchange(0);
    const u64 wpn = r.wait_plain_n.exchange(0), wpns = r.wait_plain_ns.exchange(0);
    const u64 wrn = r.wait_producer_n.exchange(0), wrns = r.wait_producer_ns.exchange(0);
    const u64 chunked = r.chunked.exchange(0), slow = r.slow_waits.exchange(0);
    std::fprintf(stderr,
                 "[CLAUDE_READBACK_WAVE] waves=%llu mapped=%llu producer=%llu producerDone=%llu "
                 "lagAvg=%.2f lagMax=%llu plainWait=%llux%.3fms producerWait=%llux%.3fms "
                 "chunked=%llu slowOver2ms=%llu\n",
                 static_cast<unsigned long long>(waves), static_cast<unsigned long long>(mapped),
                 static_cast<unsigned long long>(producer),
                 static_cast<unsigned long long>(producer_done), waves ? double(lag_sum) / waves : 0.0,
                 static_cast<unsigned long long>(lag_max), static_cast<unsigned long long>(wpn),
                 wpn ? wpns / 1e6 / wpn : 0.0, static_cast<unsigned long long>(wrn),
                 wrn ? wrns / 1e6 / wrn : 0.0, static_cast<unsigned long long>(chunked),
                 static_cast<unsigned long long>(slow));
    const u64 pp = r.plain_pending.exchange(0), pu = r.plain_unknown.exchange(0);
    const u64 ppns = r.plain_pending_ns.exchange(0), puns = r.plain_unknown_ns.exchange(0);
    const u64 pw = r.plain_write.exchange(0), pidle = r.plain_gpu_idle.exchange(0);
    std::fprintf(stderr,
                 "[CLAUDE_READBACK_PLAIN] pendingProducer=%llux%.3fms unknownProducer=%llux%.3fms "
                 "isWrite=%llu gpuIdleAtFlush=%llu\n",
                 static_cast<unsigned long long>(pp), pp ? ppns / 1e6 / pp : 0.0,
                 static_cast<unsigned long long>(pu), pu ? puns / 1e6 / pu : 0.0,
                 static_cast<unsigned long long>(pw), static_cast<unsigned long long>(pidle));
    const u64 sn = r.split_n.exchange(0), sf = r.split_flush_ns.exchange(0);
    const u64 sg = r.split_gpu_ns.exchange(0), sr = r.split_retire_ns.exchange(0);
    const u64 st = r.split_timeouts.exchange(0);
    {
        const u64 ai = r.async_issued.exchange(0), af = r.async_fallback.exchange(0);
        const u64 aw = r.async_waits.exchange(0), awns = r.async_wait_ns.exchange(0);
        const u64 at = r.async_timeouts.exchange(0), afc = r.async_flush_cmds.exchange(0);
        std::fprintf(stderr,
                     "[CLAUDE_ASYNC_READBACK] issued=%llu fallback=%llu guestWaits=%llux%.3fms "
                     "timeouts=%llu flushCmds=%llu\n",
                     static_cast<unsigned long long>(ai), static_cast<unsigned long long>(af),
                     static_cast<unsigned long long>(aw), aw ? awns / 1e6 / aw : 0.0,
                     static_cast<unsigned long long>(at), static_cast<unsigned long long>(afc));
    }
    {
        const u64 gn = r.origin_gpu_n.exchange(0), gns = r.origin_gpu_ns.exchange(0);
        const u64 qn = r.origin_guest_n.exchange(0), qns = r.origin_guest_ns.exchange(0);
        std::fprintf(stderr,
                     "[CLAUDE_READBACK_ORIGIN] gpuThread=%llux%.3fms guest=%llux%.3fms\n",
                     static_cast<unsigned long long>(gn), gn ? gns / 1e6 / gn : 0.0,
                     static_cast<unsigned long long>(qn), qn ? qns / 1e6 / qn : 0.0);
    }
    std::fprintf(stderr, "[CLAUDE_HOT_FLUSH] marks=%llu flushes=%llu\n",
                 static_cast<unsigned long long>(claude_hot_marks.exchange(0)),
                 static_cast<unsigned long long>(claude_hot_flushes.exchange(0)));
    if (sn != 0) {
        std::fprintf(stderr,
                     "[CLAUDE_READBACK_SPLIT] n=%llu flush=%.3fms gpu=%.3fms retire=%.3fms "
                     "timeouts=%llu\n",
                     static_cast<unsigned long long>(sn), sf / 1e6 / sn, sg / 1e6 / sn,
                     sr / 1e6 / sn, static_cast<unsigned long long>(st));
    }
}
} // namespace

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write) {
    scheduler.FlushPendingBufferTransfersForRange(device_addr, size);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    (void)WaitForPendingReadback(device_addr, size);
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (ExecutorBufferReadbackContractTraceEnabled()) {
        const u64 ordinal =
            executor_buffer_read_request_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ExecutorShouldLogBufferReadbackOrdinal(ordinal)) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_READBACK_REQUEST] n=%llu addr=0x%llx bytes=%llu isWrite=%u",
                static_cast<unsigned long long>(ordinal),
                static_cast<unsigned long long>(device_addr),
                static_cast<unsigned long long>(size), is_write ? 1u : 0u);
        }
    }
#endif
    bool claude_async_issued = false;
    const bool claude_on_gpu_thread = liverpool->IsGpuThread();
    const bool claude_async_try =
        !is_write && claude_guest_readback_origin && ClaudeAsyncReadbackNow();
    const auto claude_origin_begin = std::chrono::steady_clock::now();
    void* claude_frames[12]{};
    int claude_depth = 0;
    if (g_claude_sites.enabled && claude_on_gpu_thread) {
        claude_depth = backtrace(claude_frames, 12);
    }
    SCOPE_EXIT {
        if (g_claude_sites.enabled && claude_depth > 5) {
            const u64 site_ns = static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - claude_origin_begin)
                    .count());
            ClaudeRecordSite(claude_frames + 5, std::min(claude_depth - 5, 6), site_ns, size);
            ClaudeMaybePrintSites();
        }
        if (g_claude_readback.enabled) {
            const u64 ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::chrono::steady_clock::now() -
                                                claude_origin_begin)
                                                .count());
            if (claude_on_gpu_thread) {
                g_claude_readback.origin_gpu_n.fetch_add(1);
                g_claude_readback.origin_gpu_ns.fetch_add(ns);
            } else {
                g_claude_readback.origin_guest_n.fetch_add(1);
                g_claude_readback.origin_guest_ns.fetch_add(ns);
            }
        }
    };
    liverpool->SendCommand<true>([this, device_addr, size, is_write, claude_async_try,
                                  &claude_async_issued] {
        const auto claude_begin = std::chrono::steady_clock::now();
        SCOPE_EXIT {
            if (g_claude_readback.enabled) {
                const u64 ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                    std::chrono::steady_clock::now() - claude_begin)
                                                    .count());
                if (is_write) {
                    g_claude_readback.write_n.fetch_add(1);
                    g_claude_readback.write_bytes.fetch_add(size);
                    g_claude_readback.write_ns.fetch_add(ns);
                    if (size <= 4096) g_claude_readback.small_write_n.fetch_add(1);
                } else {
                    g_claude_readback.read_n.fetch_add(1);
                    g_claude_readback.read_bytes.fetch_add(size);
                    g_claude_readback.read_ns.fetch_add(ns);
                }
                ClaudeReadbackMaybePrint();
            }
        };
        if (claude_async_try && size != 0 && size <= TRACKER_BYTES_PER_PAGE &&
            device_addr <= std::numeric_limits<VAddr>::max() - size &&
            ExecutorSubmitReadbackPrefetchEnabled() && !instance.UseLegacyRenderPass() &&
            Config::managedReadbackBatching() &&
            Config::getReadbacksMode() == Config::GpuReadbacksMode::Precise &&
            !ExecutorExactDemandReadbackWindowEnabled()) {
            auto* const claude_memory = Core::Memory::Instance();
            if (claude_memory != nullptr && !claude_memory->PhysicalAliasingMayExist() &&
                gpu_modified_ranges.Intersects(device_addr, size)) {
                const RecentReadbackWindow current_window{device_addr, size};
                u32 previous_index = recent_readback_window_count;
                for (u32 index = 0; index < recent_readback_window_count; ++index) {
                    const auto& recent = recent_readback_windows[index];
                    if (recent.begin == current_window.begin &&
                        recent.size == current_window.size) {
                        previous_index = index;
                        break;
                    }
                }
                const u32 shift_from =
                    previous_index < recent_readback_window_count
                        ? previous_index
                        : std::min<u32>(recent_readback_window_count,
                                        recent_readback_windows.size() - 1);
                for (u32 index = shift_from; index > 0; --index) {
                    recent_readback_windows[index] = recent_readback_windows[index - 1];
                }
                recent_readback_windows[0] = current_window;
                if (previous_index == recent_readback_window_count) {
                    recent_readback_window_count = std::min<u32>(
                        recent_readback_window_count + 1, recent_readback_windows.size());
                }
                if (scheduler.CurrentTick() == last_submit_readback_snapshot_tick) {
                    scheduler.Flush();
                }
                PrefetchReadbackWindows();
                if (!gpu_modified_ranges.Intersects(device_addr, size)) {
                    scheduler.Flush();
                    claude_async_issued = true;
                    if (g_claude_readback.enabled) g_claude_readback.async_issued.fetch_add(1);
                    return;
                }
                if (g_claude_readback.enabled) g_claude_readback.async_fallback.fetch_add(1);
            }
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        // FindBuffer accepts a u32 extent and its page-table lookup assumes a
        // non-wrapping guest range. Only valid representable requests enter
        // the experiment; malformed requests retain the established fallback
        // behavior rather than acquiring new marker-dependent semantics.
        const bool exact_demand_window =
            ExecutorExactDemandReadbackWindowEnabled() && size != 0 &&
            size <= std::numeric_limits<u32>::max() &&
            device_addr <= std::numeric_limits<VAddr>::max() - size;
#endif
        Buffer& buffer = slot_buffers[FindBuffer(device_addr, size)];
        VAddr download_addr = device_addr;
        u64 download_size = size;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        const bool managed_submit_readback =
            !instance.UseLegacyRenderPass() && Config::managedReadbackBatching() &&
            Config::getReadbacksMode() == Config::GpuReadbacksMode::Precise;
        auto* const guest_memory = Core::Memory::Instance();
        if (!exact_demand_window && managed_submit_readback &&
            size <= TRACKER_BYTES_PER_PAGE &&
            guest_memory != nullptr && !guest_memory->PhysicalAliasingMayExist()) {
            const VAddr buffer_begin = buffer.CpuAddr();
            const VAddr buffer_end = buffer_begin + buffer.SizeBytes();
            VAddr window_begin = Common::AlignDown(device_addr, ExecutorPreciseReadbackWindow);
            window_begin = std::max(window_begin, buffer_begin);
            const VAddr required_end = device_addr + size;
            VAddr window_end =
                std::min<VAddr>(buffer_end, window_begin + ExecutorPreciseReadbackWindow);
            if (window_end < required_end) {
                window_end = required_end;
                if (window_end - window_begin > ExecutorPreciseReadbackWindow) {
                    window_begin =
                        std::max<VAddr>(buffer_begin, window_end - ExecutorPreciseReadbackWindow);
                }
            }
            download_addr = window_begin;
            download_size = window_end - window_begin;
        }
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (managed_submit_readback) {
            DownloadBufferMemoryWave(buffer, download_addr, download_size,
                                     device_addr, size, is_write);
        } else
#endif
        {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            AccumulateReadbackOptimizationStats(
                ReadbackOptimizationStats{.baseline_requests = 1});
#endif
            DownloadBufferMemory<false>(buffer, download_addr, download_size, is_write);
        }
    });
    if (claude_async_issued) {
        // The guest thread that faulted waits for the publication itself.
        claude_guest_readback_async_issued = true;
    }
}

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
void BufferCache::ResolvePendingReadback(VAddr device_addr, u64 size) {
    (void)WaitForPendingReadback(device_addr, size);
}

bool BufferCache::ResolveGpuDirtyMemoryForCpuRead(const VAddr device_addr,
                                                  const u64 size) {
    if (size == 0 || size > std::numeric_limits<u32>::max() ||
        device_addr > std::numeric_limits<VAddr>::max() - size ||
        liverpool == nullptr || !liverpool->IsGpuThread()) {
        return false;
    }

    // Materialize overlapping deferred DMA first. QueueBufferCopies records
    // exact GPU ownership when the transfer is queued, so this query must be
    // made after the range flush rather than before it.
    scheduler.FlushPendingBufferTransfersForRange(device_addr, size);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    (void)WaitForPendingReadback(device_addr, size);
#endif
    if (!IsRegionRegistered(device_addr, size)) {
        return false;
    }

    const bool exact_gpu_dirty =
        gpu_modified_ranges.Intersects(device_addr, size);
    const bool tracker_gpu_dirty =
        memory_tracker->IsRegionGpuModified(device_addr, size);
    if (!exact_gpu_dirty && !tracker_gpu_dirty) {
        return true;
    }
    // Never overwrite a newer host publication. Mixed-authority ranges must
    // go through the full snapshot transaction instead of this narrow repair.
    if (memory_tracker->IsRegionCpuModified(device_addr, size)) {
        return false;
    }

    if (exact_gpu_dirty && !tracker_gpu_dirty) {
        gpu_modified_ranges.ForEachInRange(
            device_addr, size, [this](const VAddr begin, const VAddr end) {
                if (begin < end) {
                    memory_tracker->MarkRegionAsGpuModified(begin, end - begin);
                }
            });
    }
    ReadMemory(device_addr, size);
    return !gpu_modified_ranges.Intersects(device_addr, size) &&
           !memory_tracker->IsRegionGpuModified(device_addr, size);
}

bool BufferCache::HasPendingReadback(VAddr device_addr, u64 size) {
    if (size == 0 || device_addr > std::numeric_limits<VAddr>::max() - size) {
        return false;
    }
    std::scoped_lock lock{pending_readback_mutex};
    const VAddr request_end = device_addr + size;
    return std::ranges::any_of(
        pending_readback_ranges, [&](const PendingReadbackRange& pending) {
            if (pending.begin >
                std::numeric_limits<VAddr>::max() - pending.size) {
                return false;
            }
            const VAddr pending_end = pending.begin + pending.size;
            return device_addr < pending_end && pending.begin < request_end;
        });
}

bool BufferCache::WaitForPendingReadback(VAddr device_addr, u64 size) {
    if (size == 0 || device_addr > std::numeric_limits<VAddr>::max() - size) {
        return false;
    }
    u64 dependency_tick = 0;
    {
        std::scoped_lock lock{pending_readback_mutex};
        const VAddr request_end = device_addr + size;
        for (const PendingReadbackRange& pending : pending_readback_ranges) {
            if (pending.begin > std::numeric_limits<VAddr>::max() - pending.size) {
                continue;
            }
            const VAddr pending_end = pending.begin + pending.size;
            if (device_addr < pending_end && pending.begin < request_end) {
                dependency_tick = std::max(dependency_tick, pending.tick);
            }
        }
    }
    if (dependency_tick == 0) {
        return false;
    }
    if (liverpool != nullptr && !liverpool->IsGpuThread() && ClaudeAsyncReadbackNow()) {
        const auto claude_begin = std::chrono::steady_clock::now();
        if (dependency_tick >= scheduler.CurrentTick()) {
            if (g_claude_readback.enabled) g_claude_readback.async_flush_cmds.fetch_add(1);
            liverpool->SendCommand<true>([this, dependency_tick] {
                if (dependency_tick >= scheduler.CurrentTick()) {
                    scheduler.Flush();
                }
            });
        }
        const VAddr claude_request_end = device_addr + size;
        bool claude_done = false;
        for (u32 spin = 0;; ++spin) {
            bool pending_left = false;
            {
                std::scoped_lock lock{pending_readback_mutex};
                for (const PendingReadbackRange& pending : pending_readback_ranges) {
                    if (pending.tick > dependency_tick ||
                        pending.begin > std::numeric_limits<VAddr>::max() - pending.size) {
                        continue;
                    }
                    if (device_addr < pending.begin + pending.size &&
                        pending.begin < claude_request_end) {
                        pending_left = true;
                        break;
                    }
                }
            }
            if (!pending_left) {
                claude_done = true;
                break;
            }
            if (scheduler.IsDeviceLost() ||
                std::chrono::steady_clock::now() - claude_begin > std::chrono::milliseconds{250}) {
                break;
            }
            if (spin < 64) {
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds{40});
            }
        }
        if (g_claude_readback.enabled) {
            g_claude_readback.async_waits.fetch_add(1);
            g_claude_readback.async_wait_ns.fetch_add(static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - claude_begin).count()));
            if (!claude_done) g_claude_readback.async_timeouts.fetch_add(1);
        }
        if (claude_done) {
            return true;
        }
    }
    liverpool->SendCommand<true>([this, dependency_tick] {
        const auto wait_start = ExecutorReadbackDiagnosticStart();
        // A submit snapshot is normally carried by the surrounding guest
        // submit. A CPU fault can arrive before that boundary is materialized;
        // explicitly publish the current command buffer in that case so the
        // retirement worker can own the single host timeline wait.
        if (dependency_tick >= scheduler.CurrentTick()) {
            scheduler.Flush();
        }
        // The retirement worker owns the sole Vulkan timeline wait for this
        // tick. Waiting for retirement here avoids issuing a second host wait.
        scheduler.WaitRetirement(dependency_tick);
        AccumulateReadbackOptimizationStats(ReadbackOptimizationStats{
            .cpu_wait_ns = ExecutorElapsedNs(wait_start),
        });
    });
    return true;
}

void BufferCache::PrefetchReadbackWindows() {
    const auto plan_start = ExecutorReadbackDiagnosticStart();
    if (!ExecutorSubmitReadbackPrefetchEnabled() || instance.UseLegacyRenderPass() ||
        !Config::managedReadbackBatching() ||
        recent_readback_window_count == 0 ||
        Config::getReadbacksMode() != Config::GpuReadbacksMode::Precise) {
        return;
    }
    const u64 snapshot_tick = scheduler.CurrentTick();
    if (snapshot_tick != 0 && snapshot_tick == last_submit_readback_snapshot_tick) {
        AccumulateReadbackOptimizationStats(ReadbackOptimizationStats{
            .duplicate_submit_skips = 1,
        });
        return;
    }
    scheduler.FlushPendingBufferTransfers();

    struct PrefetchCopy {
        vk::Buffer source{};
        VAddr guest_addr = 0;
        u64 size = 0;
        u64 source_offset = 0;
        u64 download_offset = 0;
    };

    boost::container::small_vector<PrefetchCopy, 32> copies;
    RangeSet scheduled_ranges;
    RangeSet excluded_ranges;
    u64 total_size_bytes = 0;
    constexpr u64 CopyAlignment = 64;

    // Start with ranges already owned by an earlier submit snapshot. New
    // ranges are added below, so partially overlapping dirty spans still
    // contribute their uncovered pages instead of being skipped wholesale.
    {
        std::scoped_lock lock{pending_readback_mutex};
        for (const PendingReadbackRange& pending : pending_readback_ranges) {
            if (pending.size != 0 &&
                pending.begin <= std::numeric_limits<VAddr>::max() - pending.size) {
                excluded_ranges.Add(pending.begin, pending.size);
            }
        }
    }

    const auto queue_buffer_ranges = [&](Buffer& buffer, const VAddr range_addr,
                                         const u64 range_size) {
        if (range_size == 0) {
            return;
        }
        memory_tracker->ForEachDownloadRange<false>(
            range_addr, range_size, [&](const u64 download_addr, const u64 download_size) {
                gpu_modified_ranges.ForEachInRange(
                    download_addr, download_size, [&](const VAddr start, const VAddr end) {
                        if (start >= end || !buffer.IsInBounds(start, end - start)) {
                            return;
                        }
                        boost::container::small_vector<std::pair<VAddr, VAddr>, 4> gaps;
                        excluded_ranges.ForEachNotInRange(
                            start, end - start,
                            [&](const VAddr gap_begin, const u64 gap_size) {
                                gaps.emplace_back(gap_begin, gap_begin + gap_size);
                            });
                        for (const auto [gap_begin, gap_end] : gaps) {
                            const u64 copy_size = gap_end - gap_begin;
                            if (copy_size == 0) {
                                continue;
                            }
                            const u64 source_offset = buffer.Offset(gap_begin);
                            if (!copies.empty()) {
                                PrefetchCopy& previous = copies.back();
                                if (previous.source == buffer.Handle() &&
                                    previous.guest_addr + previous.size == gap_begin &&
                                    previous.source_offset + previous.size == source_offset &&
                                    previous.download_offset + previous.size ==
                                        total_size_bytes) {
                                    previous.size += copy_size;
                                } else {
                                    copies.push_back(PrefetchCopy{
                                        .source = buffer.Handle(),
                                        .guest_addr = gap_begin,
                                        .size = copy_size,
                                        .source_offset = source_offset,
                                        .download_offset = total_size_bytes,
                                    });
                                }
                            } else {
                                copies.push_back(PrefetchCopy{
                                    .source = buffer.Handle(),
                                    .guest_addr = gap_begin,
                                    .size = copy_size,
                                    .source_offset = source_offset,
                                    .download_offset = total_size_bytes,
                                });
                            }
                            scheduled_ranges.Add(gap_begin, copy_size);
                            excluded_ranges.Add(gap_begin, copy_size);
                            total_size_bytes =
                                Common::AlignUp(total_size_bytes + copy_size, CopyAlignment);
                        }
                    });
            });
    };

    for (u32 index = 0; index < recent_readback_window_count; ++index) {
        const RecentReadbackWindow& window = recent_readback_windows[index];
        buffer_ranges.ForEachInRange(
            window.begin, window.size,
            [&](const VAddr range_begin, const VAddr range_end, const BufferId id) {
                if (IsBufferInvalid(id)) {
                    return;
                }
                Buffer& buffer = slot_buffers[id];
                const VAddr begin = std::max(range_begin, buffer.CpuAddr());
                const VAddr end =
                    std::min<VAddr>(range_end, buffer.CpuAddr() + buffer.SizeBytes());
                if (begin < end) {
                    queue_buffer_ranges(buffer, begin, end - begin);
                }
            });
    }
    if (copies.empty()) {
        return;
    }

    const u64 planned_regions = copies.size();
    std::stable_sort(copies.begin(), copies.end(),
                     [](const PrefetchCopy& lhs, const PrefetchCopy& rhs) {
                         const VkBuffer lhs_handle = static_cast<VkBuffer>(lhs.source);
                         const VkBuffer rhs_handle = static_cast<VkBuffer>(rhs.source);
                         if (lhs_handle != rhs_handle) {
                             return std::less<VkBuffer>{}(lhs_handle, rhs_handle);
                         }
                         if (lhs.source_offset != rhs.source_offset) {
                             return lhs.source_offset < rhs.source_offset;
                         }
                         return lhs.guest_addr < rhs.guest_addr;
                     });
    boost::container::small_vector<PrefetchCopy, 32> coalesced_copies;
    for (const PrefetchCopy& copy : copies) {
        if (!coalesced_copies.empty()) {
            PrefetchCopy& previous = coalesced_copies.back();
            if (previous.source == copy.source &&
                previous.guest_addr + previous.size == copy.guest_addr &&
                previous.source_offset + previous.size == copy.source_offset) {
                previous.size += copy.size;
                continue;
            }
        }
        coalesced_copies.push_back(copy);
    }
    copies = std::move(coalesced_copies);
    total_size_bytes = 0;
    for (PrefetchCopy& copy : copies) {
        copy.download_offset = total_size_bytes;
        total_size_bytes =
            Common::AlignUp(total_size_bytes + copy.size, CopyAlignment);
    }

    std::unique_ptr<Buffer> overflow_download;
    u8* download = nullptr;
    u64 base_offset = 0;
    if (total_size_bytes <= download_buffer.SizeBytes()) {
        std::tie(download, base_offset) =
            download_buffer.Map(total_size_bytes, CopyAlignment, false);
    }
    if (download == nullptr) {
        overflow_download = std::make_unique<Buffer>(
            std::nothrow, instance, scheduler, MemoryUsage::Download, 0,
            vk::BufferUsageFlagBits::eTransferDst, total_size_bytes);
        if (overflow_download->IsValid() &&
            overflow_download->mapped_data.size() >= total_size_bytes) {
            download = overflow_download->mapped_data.data();
            base_offset = 0;
            AccumulateReadbackOptimizationStats(
                ReadbackOptimizationStats{.overflow_allocations = 1});
        }
    }
    if (download == nullptr &&
        total_size_bytes <= download_buffer.SizeBytes()) {
        // Dedicated allocation can fail under transient heap pressure. The
        // existing ring remains a correctness fallback; waiting here only
        // retires an older ring slice and does not create another submit.
        overflow_download.reset();
        std::tie(download, base_offset) =
            download_buffer.Map(total_size_bytes, CopyAlignment, true);
    }
    if (download == nullptr) {
        AccumulateReadbackOptimizationStats(ReadbackOptimizationStats{
            .cpu_plan_ns = ExecutorElapsedNs(plan_start),
            .fallbacks = 1,
        });
        return;
    }
    Buffer* const download_target =
        overflow_download ? overflow_download.get() : &download_buffer;
    if (!overflow_download) {
        download_buffer.Commit();
    }
    scheduler.EndRendering(Vulkan::Scheduler::EndRenderingReason::BufferReadback);
    const auto cmdbuf = scheduler.CommandBuffer();
    boost::container::small_vector<vk::BufferMemoryBarrier2, 32> pre_barriers;
    vk::Buffer previous_source{};
    for (const PrefetchCopy& copy : copies) {
        if (copy.source == previous_source) {
            continue;
        }
        previous_source = copy.source;
        pre_barriers.push_back(vk::BufferMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite |
                             vk::AccessFlagBits2::eShaderWrite |
                             vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .buffer = copy.source,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        });
    }
    pre_barriers.push_back(vk::BufferMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eHost,
        .srcAccessMask = vk::AccessFlagBits2::eHostRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .buffer = download_target->Handle(),
        .offset = base_offset,
        .size = total_size_bytes,
    });
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = static_cast<u32>(pre_barriers.size()),
        .pBufferMemoryBarriers = pre_barriers.data(),
    });
    std::vector<bool> copy_recorded(copies.size(), false);
    u64 copy_commands = 0;
    u64 copy_regions = 0;
    for (size_t first = 0; first < copies.size(); ++first) {
        if (copy_recorded[first]) {
            continue;
        }
        const vk::Buffer source = copies[first].source;
        boost::container::small_vector<vk::BufferCopy, 32> regions;
        for (size_t index = first; index < copies.size(); ++index) {
            if (!copy_recorded[index] && copies[index].source == source) {
                const PrefetchCopy& copy = copies[index];
                regions.push_back(vk::BufferCopy{
                    .srcOffset = copy.source_offset,
                    .dstOffset = base_offset + copy.download_offset,
                    .size = copy.size,
                });
                copy_recorded[index] = true;
            }
        }
        cmdbuf.copyBuffer(source, download_target->Handle(), regions);
        ++copy_commands;
        copy_regions += regions.size();
    }
    const vk::BufferMemoryBarrier2 host_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        .buffer = download_target->Handle(),
        .offset = base_offset,
        .size = total_size_bytes,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &host_barrier,
    });

    std::vector<PendingReadbackRange> tracked_ranges;
    tracked_ranges.reserve(copies.size());
    const u64 completion_tick = snapshot_tick;
    u64 tracked_pages = 0;
    u64 copied_bytes = 0;
    for (const PrefetchCopy& copy : copies) {
        copied_bytes += copy.size;
    }
    scheduled_ranges.ForEach([&](const VAddr begin, const VAddr end) {
        const u64 size = end - begin;
        tracked_pages +=
            (size + static_cast<u64>(TRACKER_BYTES_PER_PAGE) - 1) /
            static_cast<u64>(TRACKER_BYTES_PER_PAGE);
        page_manager->UpdatePageWatchers<true>(begin, size);
        page_manager->TrackPendingCompletionRead(begin, size);
        tracked_ranges.push_back(PendingReadbackRange{begin, size, completion_tick});
    });
    {
        std::scoped_lock lock{pending_readback_mutex};
        pending_readback_ranges.insert(pending_readback_ranges.end(), tracked_ranges.begin(),
                                       tracked_ranges.end());
    }
    for (const PendingReadbackRange& range : tracked_ranges) {
        memory_tracker->UnmarkRegionAsGpuModified(range.begin, range.size);
        gpu_modified_ranges.Subtract(range.begin, range.size);
    }
    last_submit_readback_snapshot_tick = snapshot_tick;

    if (IsReadbackOptimizationStatsEnabled()) {
        static u64 prefetch_batches = 0;
        static u64 prefetch_bytes = 0;
        prefetch_bytes += total_size_bytes;
        if (((++prefetch_batches) & 255u) == 0u) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_READBACK_PREFETCH] batches=256 bytesMiB=%.3f avgKiB=%.3f",
                static_cast<double>(prefetch_bytes) / static_cast<double>(1_MB),
                static_cast<double>(prefetch_bytes) / (256.0 * 1024.0));
            prefetch_bytes = 0;
        }
    }

    // Publish prefetched readbacks on the retirement runner for this exact
    // timeline tick. This avoids leaving completed copies in the ordinary
    // pending queue until another submit and lets faulting guest threads wait
    // for one batched retirement point instead of repeatedly round-tripping
    // through the GPU command queue.
    const auto queued_at = ExecutorReadbackDiagnosticStart();
    AccumulateReadbackOptimizationStats(ReadbackOptimizationStats{
        .snapshots = 1,
        .submit_snapshots = 1,
        .ranges = static_cast<u64>(tracked_ranges.size()),
        .pages = tracked_pages,
        .copy_commands = copy_commands,
        .copy_regions = copy_regions,
        .source_buffers = copy_commands,
        .regions_merged = planned_regions - copies.size(),
        .bytes = copied_bytes,
        .cpu_plan_ns = ExecutorElapsedNs(plan_start),
    });
    ExecutorMaybeLogReadbackOptimizationStats();
    scheduler.DeferPriorityOperationAt(
        completion_tick,
        [this, copies = std::move(copies), tracked_ranges = std::move(tracked_ranges),
         download, base_offset, total_size_bytes, completion_tick, queued_at,
         overflow_download = std::move(overflow_download)]() mutable {
            const auto publish_start = ExecutorReadbackDiagnosticStart();
            RecordReadbackTimelineRetirement(completion_tick);
            if (overflow_download) {
                overflow_download->InvalidateMappedRange(0, total_size_bytes);
            } else {
                download_buffer.InvalidateMappedRange(base_offset, total_size_bytes);
            }
            auto* guest_memory = Core::Memory::Instance();
            bool wrote_all = true;
            for (const PrefetchCopy& copy : copies) {
                if (!guest_memory->TryWriteBacking(
                        std::bit_cast<u8*>(copy.guest_addr),
                        download + copy.download_offset, copy.size)) {
                    wrote_all = false;
                }
            }
            if (!wrote_all) {
                AccumulateReadbackOptimizationStats(ReadbackOptimizationStats{
                    .fallbacks = 1,
                    .publication_failures = 1,
                });
                for (const PendingReadbackRange& range : tracked_ranges) {
                    gpu_modified_ranges.Add(range.begin, range.size);
                    memory_tracker->MarkRegionAsGpuModified(range.begin, range.size);
                }
            } else {
                NotifyGuestReadback();
            }
            AccumulateReadbackOptimizationStats(ReadbackOptimizationStats{
                .cpu_publish_ns = ExecutorElapsedNs(publish_start),
                .gpu_completion_ns = ExecutorElapsedNs(queued_at),
            });
            // Keep the retired dependency discoverable until its page
            // protections are actually open.  A host read can fault on
            // GpuComm while this retirement callback runs.  If the range is
            // erased first, WaitForPendingReadback observes no dependency and
            // the signal handler can report no progress while the PTE is
            // still protected.  Publishing the open PTE before removing the
            // dependency gives the faulting thread only two valid states: it
            // either waits for this retirement, or it observes an accessible
            // page after the range disappears.
            for (const PendingReadbackRange& range : tracked_ranges) {
                page_manager->UntrackPendingCompletionRead(range.begin, range.size);
                page_manager->UpdatePageWatchers<false>(range.begin, range.size);
            }
            {
                std::scoped_lock lock{pending_readback_mutex};
                std::erase_if(pending_readback_ranges, [&](const auto& pending) {
                    return pending.tick == completion_tick &&
                           std::ranges::any_of(tracked_ranges, [&](const auto& completed) {
                               return pending.begin == completed.begin &&
                                      pending.size == completed.size;
                           });
                });
            }
            liverpool->NotifyMemoryPublicationForExecutor(0, 1);
        });
}

void BufferCache::DownloadBufferMemoryWave(Buffer& primary_buffer, const VAddr window_addr,
                                           const u64 window_size, const VAddr access_addr,
                                           const u64 access_size, const bool is_write) {
    const auto plan_start = ExecutorReadbackDiagnosticStart();
    // Demand publication is an explicit consumer boundary. Materialize only
    // transfers which can affect this wave before dirty-range planning; the
    // exact primary access was already range-flushed by ReadMemory, while the
    // expanded window and recent prefetch windows can add dependencies here.
    // Physical aliases and executor replay do not provide a complete guest-VA
    // range proof, so they retain the established global drain.
    const bool incomplete_range_proof =
        memory->PhysicalAliasingMayExist() ||
        Libraries::GnmDriver::ExecutorReplayActive();
    if (incomplete_range_proof) {
        scheduler.FlushPendingBufferTransfers();
    } else {
        scheduler.FlushPendingBufferTransfersForRange(window_addr, window_size);
        for (u32 index = 0; index < recent_readback_window_count; ++index) {
            const RecentReadbackWindow& window = recent_readback_windows[index];
            scheduler.FlushPendingBufferTransfersForRange(window.begin, window.size);
        }
    }
    struct WaveCopy {
        Buffer* buffer{};
        VAddr guest_addr{};
        u64 size{};
        u64 source_offset{};
        u64 download_offset{};
        bool primary{};
        const u8* retired_mapped{};
    };

    boost::container::small_vector<WaveCopy, 32> copies;
    RangeSet scheduled_ranges;
    constexpr u64 CopyAlignment = 64;
    const bool exact_valid = access_size != 0 &&
                             access_addr <= std::numeric_limits<VAddr>::max() - access_size;
    const VAddr access_end = exact_valid ? access_addr + access_size : access_addr;

    const auto queue_buffer_ranges = [&](Buffer& buffer, const VAddr range_addr,
                                         const u64 range_size, const bool primary) {
        if (range_size == 0) {
            return;
        }
        memory_tracker->ForEachDownloadRange<false>(
            range_addr, range_size,
            [&](const u64 download_addr, const u64 download_size) {
                gpu_modified_ranges.ForEachInRange(
                    download_addr, download_size,
                    [&](const VAddr start, const VAddr end) {
                        if (start >= end ||
                            !buffer.IsInBounds(start, end - start)) {
                            return;
                        }
                        boost::container::small_vector<std::pair<VAddr, VAddr>, 4>
                            gaps;
                        scheduled_ranges.ForEachNotInRange(
                            start, end - start,
                            [&](const VAddr gap_begin, const u64 gap_size) {
                                gaps.emplace_back(gap_begin,
                                                  gap_begin + gap_size);
                            });
                        for (const auto [gap_begin, gap_end] : gaps) {
                            const u64 copy_size = gap_end - gap_begin;
                            if (copy_size == 0) {
                                continue;
                            }
                            const bool exact_overlap =
                                exact_valid && gap_begin < access_end && access_addr < gap_end;
                            copies.push_back(WaveCopy{
                                .buffer = &buffer,
                                .guest_addr = gap_begin,
                                .size = copy_size,
                                .source_offset = buffer.Offset(gap_begin),
                                .primary = primary && exact_overlap,
                            });
                            scheduled_ranges.Add(gap_begin, copy_size);
                        }
                    });
            });
    };

    queue_buffer_ranges(primary_buffer, window_addr, window_size, true);
    const auto queue_window = [&](const RecentReadbackWindow& window) {
        if (window.size == 0) {
            return;
        }
        buffer_ranges.ForEachInRange(
            window.begin, window.size,
            [&](const VAddr range_begin, const VAddr range_end,
                const BufferId id) {
                if (IsBufferInvalid(id)) {
                    return;
                }
                Buffer& buffer = slot_buffers[id];
                const VAddr begin = std::max(range_begin, buffer.CpuAddr());
                const VAddr end = std::min<VAddr>(
                    range_end, buffer.CpuAddr() + buffer.SizeBytes());
                if (begin < end) {
                    queue_buffer_ranges(buffer, begin, end - begin, false);
                }
            });
    };
    for (u32 index = 0; index < recent_readback_window_count; ++index) {
        queue_window(recent_readback_windows[index]);
    }

    // Page-sized CPU writes are the dominant Bloodborne readback fault too:
    // before a partial overwrite the current GPU contents must be published.
    // Keep both read and write windows as submit-prefetch hints so the next
    // ordinary submit retires recurring ranges in one wave instead of forcing
    // one Vulkan submit/timeline wait for every guest fault.
    if (access_size != 0 && access_size <= TRACKER_BYTES_PER_PAGE) {
        const RecentReadbackWindow current_window{access_addr, access_size};
        u32 previous_index = recent_readback_window_count;
        for (u32 index = 0; index < recent_readback_window_count; ++index) {
            const auto& recent = recent_readback_windows[index];
            if (recent.begin == current_window.begin &&
                recent.size == current_window.size) {
                previous_index = index;
                break;
            }
        }
        const u32 shift_from =
            previous_index < recent_readback_window_count
                ? previous_index
                : std::min<u32>(recent_readback_window_count,
                                recent_readback_windows.size() - 1);
        for (u32 index = shift_from; index > 0; --index) {
            recent_readback_windows[index] =
                recent_readback_windows[index - 1];
        }
        recent_readback_windows[0] = current_window;
        if (previous_index == recent_readback_window_count) {
            recent_readback_window_count =
                std::min<u32>(recent_readback_window_count + 1,
                              recent_readback_windows.size());
        }
    }

    // A demand access can arrive after the exact primary range has already
    // been published while a recently adjacent GPU-owned range is still the
    // backing-memory dependency needed by the guest.  Discover those windows
    // before deciding that the wave is empty.  The primary request itself is
    // excluded from snapshot dependency aliases in ResolveSnapshotDependencies,
    // so this does not reintroduce the former self-alias readback storm.
    if (copies.empty()) {
        if (is_write && exact_valid) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            RecordDmaCleanEpochCpuPublication();
#endif
            memory_tracker->MarkRegionAsCpuModified(access_addr, access_size);
        }
        return;
    }

    const u64 planned_regions = copies.size();
    std::stable_sort(
        copies.begin(), copies.end(),
        [](const WaveCopy& lhs, const WaveCopy& rhs) {
            const VkBuffer lhs_handle =
                static_cast<VkBuffer>(lhs.buffer->Handle());
            const VkBuffer rhs_handle =
                static_cast<VkBuffer>(rhs.buffer->Handle());
            if (lhs_handle != rhs_handle) {
                return std::less<VkBuffer>{}(lhs_handle, rhs_handle);
            }
            if (lhs.source_offset != rhs.source_offset) {
                return lhs.source_offset < rhs.source_offset;
            }
            return lhs.guest_addr < rhs.guest_addr;
        });
    boost::container::small_vector<WaveCopy, 32> coalesced_copies;
    for (const WaveCopy& copy : copies) {
        if (!coalesced_copies.empty()) {
            WaveCopy& previous = coalesced_copies.back();
            if (previous.buffer == copy.buffer &&
                previous.guest_addr + previous.size == copy.guest_addr &&
                previous.source_offset + previous.size ==
                    copy.source_offset) {
                previous.size += copy.size;
                previous.primary |= copy.primary;
                continue;
            }
        }
        coalesced_copies.push_back(copy);
    }
    copies = std::move(coalesced_copies);

    static const bool claude_mapped_ab = [] {
        const char* value = std::getenv("EXECUTOR_CLAUDE_MAPPED_AB");
        return value && value[0] == '1';
    }();
    const bool claude_mapped_phase = !claude_mapped_ab ||
        (static_cast<long long>(std::time(nullptr)) / 5) % 2 == 0;
    bool mapped_wave = PortableGpu::OrderedMappedReadbackEnabled() && claude_mapped_phase &&
        liverpool->IsGpuThread() && !incomplete_range_proof && !copies.empty();
    if (mapped_wave) {
        for (WaveCopy& copy : copies) {
            const auto mapped = copy.buffer->mapped_data;
            if (!PortableGpu::OrderedMappedRangeEligible(true,incomplete_range_proof,
                    copy.buffer->IsValid(),copy.buffer->is_deleted,copy.buffer->is_coherent,
                    mapped.size(),copy.source_offset,copy.size)) {
                mapped_wave = false;
                break;
            }
            copy.retired_mapped = mapped.data() + copy.source_offset;
        }
    }
    bool producer_wave = mapped_wave && PortableGpu::ProducerReadbackEnabled() &&
        !Config::managedGpuFastPath() && !scheduler.IsDeviceLost();
    u64 producer_tick = 0;
    if (producer_wave) {
        for (const WaveCopy& copy : copies) {
            const auto tick = PortableGpu::FindSealedProducer(this,&scheduler,
                PortableGpu::ProducerAllocation{reinterpret_cast<std::uintptr_t>(copy.buffer),
                    std::bit_cast<std::uint64_t>(static_cast<VkBuffer>(copy.buffer->Handle()))},
                copy.guest_addr,copy.size);
            if (!tick) { producer_wave = false; break; }
            producer_tick = std::max(producer_tick,*tick);
        }
    }
    // All-or-nothing: any incomplete allocation proof preserves the original
    // immutable staging path. Active buffers cannot enter DeleteBuffer while
    // their GPU owner is blocked in the unchanged synchronous retirement wait.
    u64 total_size_bytes = 0;
    u64 copied_bytes = 0;
    for (WaveCopy& copy : copies) {
        copy.download_offset = total_size_bytes;
        copied_bytes += copy.size;
        total_size_bytes =
            Common::AlignUp(total_size_bytes + copy.size, CopyAlignment);
    }

    std::unique_ptr<Buffer> overflow_download;
    u8* download = nullptr;
    u64 base_offset = 0;
    Buffer* download_target = nullptr;
    if (!mapped_wave) {
    if (total_size_bytes <= download_buffer.SizeBytes()) {
        std::tie(download, base_offset) =
            download_buffer.Map(total_size_bytes, CopyAlignment, false);
    }
    if (download == nullptr) {
        overflow_download = std::make_unique<Buffer>(
            std::nothrow, instance, scheduler, MemoryUsage::Download, 0,
            vk::BufferUsageFlagBits::eTransferDst, total_size_bytes);
        if (overflow_download->IsValid() &&
            overflow_download->mapped_data.size() >= total_size_bytes) {
            download = overflow_download->mapped_data.data();
            base_offset = 0;
            AccumulateReadbackOptimizationStats(
                ReadbackOptimizationStats{.overflow_allocations = 1});
        }
    }
    if (download == nullptr &&
        total_size_bytes <= download_buffer.SizeBytes()) {
        overflow_download.reset();
        std::tie(download, base_offset) =
            download_buffer.Map(total_size_bytes, CopyAlignment, true);
    }
    if (download == nullptr) {
        AccumulateReadbackOptimizationStats(ReadbackOptimizationStats{
            .cpu_plan_ns = ExecutorElapsedNs(plan_start),
            .fallbacks = 1,
        });
        // A full-size dedicated allocation can exceed a device allocation
        // limit. Preserve correctness with the same managed retirement path
        // in bounded chunks: no scheduler::Finish loop is reintroduced.
        if (g_claude_readback.enabled) g_claude_readback.chunked.fetch_add(1);
        const u64 chunk_size =
            std::max<u64>(TRACKER_BYTES_PER_PAGE,
                          download_buffer.SizeBytes() / 2);
        const auto saved_windows = recent_readback_windows;
        const u32 saved_window_count = recent_readback_window_count;
        recent_readback_window_count = 0;
        for (u64 offset = 0; offset < window_size;) {
            const u64 bytes = std::min(chunk_size, window_size - offset);
            const VAddr chunk_begin = window_addr + offset;
            const VAddr chunk_end = chunk_begin + bytes;
            const VAddr exact_begin = exact_valid ? std::max(chunk_begin, access_addr)
                                                  : chunk_begin;
            const VAddr exact_end = exact_valid ? std::min(chunk_end, access_end)
                                                : chunk_begin;
            const u64 exact_size = exact_end > exact_begin ? exact_end - exact_begin : 0;
            DownloadBufferMemoryWave(primary_buffer, chunk_begin, bytes, exact_begin,
                                     exact_size, is_write && exact_size != 0);
            offset += bytes;
        }
        recent_readback_windows = saved_windows;
        recent_readback_window_count = saved_window_count;
        return;
    }

    download_target = overflow_download ? overflow_download.get() : &download_buffer;
    if (!overflow_download) {
        download_buffer.Commit();
    }
    }
    if (!producer_wave) scheduler.EndRendering(
        Vulkan::Scheduler::EndRenderingReason::BufferReadback);
    const auto cmdbuf = scheduler.CommandBuffer();
    u64 copy_commands = 0;
    u64 copy_regions = 0;
    if (producer_wave) {
        // Producer command buffer already contains its HOST barrier; original callback retires it.
    } else if (mapped_wave) {
        // Coherency is insufficient: publish every selected producer range to
        // HOST and retire this tail tick before the existing callback reads it.
        boost::container::small_vector<vk::BufferMemoryBarrier2, 32> host_barriers;
        for (const WaveCopy& copy : copies) {
            host_barriers.push_back(vk::BufferMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask = vk::AccessFlagBits2::eHostRead,
                .buffer = copy.buffer->Handle(),
                .offset = copy.source_offset,
                .size = copy.size,
            });
        }
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .bufferMemoryBarrierCount = static_cast<u32>(host_barriers.size()),
            .pBufferMemoryBarriers = host_barriers.data(),
        });
    } else {
    boost::container::small_vector<vk::BufferMemoryBarrier2, 32> pre_barriers;
    for (const WaveCopy& copy : copies) {
        pre_barriers.push_back(vk::BufferMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite |
                             vk::AccessFlagBits2::eShaderWrite |
                             vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .buffer = copy.buffer->Handle(),
            .offset = copy.source_offset,
            .size = copy.size,
        });
    }
    pre_barriers.push_back(vk::BufferMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eHost,
        .srcAccessMask = vk::AccessFlagBits2::eHostRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .buffer = download_target->Handle(),
        .offset = base_offset,
        .size = total_size_bytes,
    });
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount =
            static_cast<u32>(pre_barriers.size()),
        .pBufferMemoryBarriers = pre_barriers.data(),
    });

    for (size_t first = 0; first < copies.size();) {
        const vk::Buffer source = copies[first].buffer->Handle();
        boost::container::small_vector<vk::BufferCopy, 32> regions;
        size_t end = first;
        while (end < copies.size() &&
               copies[end].buffer->Handle() == source) {
            const WaveCopy& copy = copies[end++];
            regions.push_back(vk::BufferCopy{
                .srcOffset = copy.source_offset,
                .dstOffset = base_offset + copy.download_offset,
                .size = copy.size,
            });
        }
        cmdbuf.copyBuffer(source, download_target->Handle(), regions);
        ++copy_commands;
        copy_regions += regions.size();
        first = end;
    }
    const vk::BufferMemoryBarrier2 host_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        .buffer = download_target->Handle(),
        .offset = base_offset,
        .size = total_size_bytes,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &host_barrier,
    });

    }

    u64 tracked_ranges = 0;
    u64 tracked_pages = 0;
    scheduled_ranges.ForEach([&](const VAddr begin, const VAddr end) {
        ++tracked_ranges;
        const u64 range_size = end - begin;
        tracked_pages +=
            (range_size + static_cast<u64>(TRACKER_BYTES_PER_PAGE) - 1) /
            static_cast<u64>(TRACKER_BYTES_PER_PAGE);
    });
    std::vector<std::tuple<Buffer*, VAddr, u64>> claude_copy_keys;
    if (g_claude_readback.enabled && !producer_wave && mapped_wave) {
        for (const WaveCopy& copy : copies) {
            claude_copy_keys.emplace_back(copy.buffer, copy.guest_addr, copy.size);
        }
    }
    const u64 completion_tick = producer_wave ? producer_tick : scheduler.CurrentTick();
    const auto queued_at = ExecutorReadbackDiagnosticStart();
    scheduler.DeferPriorityOperationAt(
        completion_tick,
        [this, copies = std::move(copies), mapped_wave, download, base_offset,
         total_size_bytes, completion_tick, queued_at, access_addr, access_size,
         is_write,
         overflow_download = std::move(overflow_download)]() mutable {
            const auto publish_start = ExecutorReadbackDiagnosticStart();
            RecordReadbackTimelineRetirement(completion_tick);
            if (mapped_wave) {
                // Only proven HOST_COHERENT active allocations enter this route.
            } else if (overflow_download) {
                overflow_download->InvalidateMappedRange(0, total_size_bytes);
            } else {
                download_buffer.InvalidateMappedRange(base_offset,
                                                      total_size_bytes);
            }
            auto* guest_memory = Core::Memory::Instance();
            RangeSet published_ranges;
            bool published_any = false;
            bool primary_failed = false;
            for (const WaveCopy& copy : copies) {
                if (!guest_memory->TryWriteBacking(
                        std::bit_cast<u8*>(copy.guest_addr),
                        (mapped_wave ? copy.retired_mapped : download + copy.download_offset), copy.size)) {
                    primary_failed |= copy.primary;
                    AccumulateReadbackOptimizationStats(
                        ReadbackOptimizationStats{
                            .fallbacks = 1,
                            .publication_failures = 1,
                        });
                    continue;
                }
                published_ranges.Add(copy.guest_addr, copy.size);
                published_any = true;
            }
            published_ranges.ForEach(
                [&](const VAddr begin, const VAddr end) {
                    gpu_modified_ranges.Subtract(begin, end - begin);
                    memory_tracker->UnmarkRegionAsGpuModified(begin,
                                                              end - begin);
                });
            if (is_write && access_size != 0 && !primary_failed) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
                RecordDmaCleanEpochCpuPublication();
#endif
                memory_tracker->MarkRegionAsCpuModified(access_addr, access_size);
            }
            if (published_any) {
                NotifyGuestReadback();
                liverpool->NotifyMemoryPublicationForExecutor(
                    access_size != 0 ? access_addr : 0,
                    access_size != 0 ? access_size : 1);
            }
            AccumulateReadbackOptimizationStats(ReadbackOptimizationStats{
                .cpu_publish_ns = ExecutorElapsedNs(publish_start),
                .gpu_completion_ns = ExecutorElapsedNs(queued_at),
            });
        });

    AccumulateReadbackOptimizationStats(ReadbackOptimizationStats{
        .snapshots = 1,
        .demand_snapshots = 1,
        .ranges = tracked_ranges,
        .pages = tracked_pages,
        .copy_commands = copy_commands,
        .copy_regions = copy_regions,
        .source_buffers = copy_commands,
        .regions_merged = planned_regions - copy_regions,
        .bytes = copied_bytes,
        .cpu_plan_ns = ExecutorElapsedNs(plan_start),
        .demand_flushes = 1,
    });
    if (mapped_wave) {
        static u64 served = 0;
        const u64 ordinal = ++served;
        if ((ordinal & (ordinal-1)) == 0) {
            __android_log_print(ANDROID_LOG_INFO,"LSX4Native",
                "[PORTABLE_ORDERED_MAPPED_READBACK] served=%llu bytes=%llu tick=%llu",
                static_cast<unsigned long long>(ordinal),
                static_cast<unsigned long long>(copied_bytes),
                static_cast<unsigned long long>(completion_tick));
        }
    }
    if (producer_wave) PortableGpu::RecordProducerReadbackHit(
        copied_bytes,producer_tick,scheduler.CurrentTick());
    ExecutorMaybeLogReadbackOptimizationStats();

    const auto wait_start = ExecutorReadbackDiagnosticStart();
    const auto claude_wait_begin = std::chrono::steady_clock::now();
    if (g_claude_readback.enabled) {
        auto& r = g_claude_readback;
        r.waves.fetch_add(1);
        if (mapped_wave) r.mapped.fetch_add(1);
        const u64 current = scheduler.CurrentTick();
        u64 lag = 64;
        for (u64 d = 1; d <= 32; d *= 2) {
            if (current <= d || scheduler.IsFree(current - d)) {
                lag = d;
                break;
            }
        }
        r.lag_sum.fetch_add(lag);
        u64 prev = r.lag_max.load();
        while (lag > prev && !r.lag_max.compare_exchange_weak(prev, lag)) {
        }
        if (producer_wave) {
            r.producer.fetch_add(1);
            if (scheduler.IsFree(producer_tick)) r.producer_done.fetch_add(1);
        }
    }
    int claude_plain_kind = 0;
    if (g_claude_readback.enabled && !producer_wave) {
        const u64 before = scheduler.CurrentTick();
        if (before > 1 && scheduler.IsFree(before - 1)) g_claude_readback.plain_gpu_idle.fetch_add(1);
        if (is_write) g_claude_readback.plain_write.fetch_add(1);
    }
    static const bool claude_split = [] {
        const char* value = std::getenv("EXECUTOR_CLAUDE_READBACK_TIMING");
        return value && value[0] == '1';
    }();
    const auto claude_t0 = std::chrono::steady_clock::now();
    if (!producer_wave) scheduler.Flush();
    if (claude_split && !producer_wave) {
        const auto t1 = std::chrono::steady_clock::now();
        bool timed_out = false;
        while (!scheduler.IsFree(completion_tick)) {
            if (std::chrono::steady_clock::now() - t1 > std::chrono::milliseconds{20}) {
                timed_out = true;
                break;
            }
            std::this_thread::yield();
        }
        const auto t2 = std::chrono::steady_clock::now();
        scheduler.WaitRetirement(completion_tick);
        const auto t3 = std::chrono::steady_clock::now();
        auto ns = [](auto a, auto z) {
            return static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(z - a).count());
        };
        auto& r = g_claude_readback;
        r.split_n.fetch_add(1);
        r.split_flush_ns.fetch_add(ns(claude_t0, t1));
        r.split_gpu_ns.fetch_add(ns(t1, t2));
        r.split_retire_ns.fetch_add(ns(t2, t3));
        if (timed_out) r.split_timeouts.fetch_add(1);
    }
    if (g_claude_readback.enabled && !producer_wave && mapped_wave &&
        PortableGpu::ProducerReadbackEnabled()) {
        bool all = true;
        for (const auto& [claude_buffer, claude_addr, claude_size] : claude_copy_keys) {
            const auto tick = PortableGpu::FindSealedProducer(this, &scheduler,
                PortableGpu::ProducerAllocation{reinterpret_cast<std::uintptr_t>(claude_buffer),
                    std::bit_cast<std::uint64_t>(static_cast<VkBuffer>(claude_buffer->Handle()))},
                claude_addr, claude_size);
            if (!tick) {
                all = false;
                break;
            }
        }
        claude_plain_kind = all ? 1 : 2;
    }
    // Publication callback and outstanding-operation accounting stay original.
    scheduler.WaitRetirement(completion_tick);
    if (g_claude_readback.enabled && claude_plain_kind != 0) {
        const u64 ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now() - claude_wait_begin)
                                            .count());
        if (claude_plain_kind == 1) {
            g_claude_readback.plain_pending.fetch_add(1);
            g_claude_readback.plain_pending_ns.fetch_add(ns);
        } else {
            g_claude_readback.plain_unknown.fetch_add(1);
            g_claude_readback.plain_unknown_ns.fetch_add(ns);
        }
    }
    if (g_claude_readback.enabled) {
        const u64 ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now() - claude_wait_begin)
                                            .count());
        auto& r = g_claude_readback;
        if (producer_wave) {
            r.wait_producer_n.fetch_add(1);
            r.wait_producer_ns.fetch_add(ns);
        } else {
            r.wait_plain_n.fetch_add(1);
            r.wait_plain_ns.fetch_add(ns);
        }
        if (ns > 2000000) r.slow_waits.fetch_add(1);
    }
    const u64 wait_ns = ExecutorElapsedNs(wait_start);
    AccumulateReadbackOptimizationStats(ReadbackOptimizationStats{
        .cpu_wait_ns = wait_ns,
    });
    if (IsReadbackClassificationEnabled()) {
        // access_size is this submitted wave's exact guest-request portion.
        // Recursive overflow splitting passes disjoint portions, so the
        // original request is not multiplied across child waves.
        RecordBufferDemandWaveClassification(access_size, copied_bytes, wait_ns,
                                             ExecutorElapsedNs(plan_start));
    }
}
#endif

template <bool async>
void BufferCache::DownloadBufferMemory(
    Buffer& buffer, VAddr device_addr, u64 size, bool is_write,
    const Core::MemoryManager::SnapshotReadGuard* snapshot_guard) {
    static_assert(!async,
                  "asynchronous buffer readback is not guest-VA generation safe");
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const auto plan_start = ExecutorReadbackDiagnosticStart();
#endif
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    memory_tracker->ForEachDownloadRange<false>(
        device_addr, size, [&](u64 device_addr_out, u64 range_size) {
            const VAddr buffer_addr = buffer.CpuAddr();
            const auto add_download = [&](VAddr start, VAddr end) {
                const u64 new_offset = start - buffer_addr;
                const u64 new_size = end - start;
                copies.push_back(vk::BufferCopy{
                    .srcOffset = new_offset,
                    .dstOffset = total_size_bytes,
                    .size = new_size,
                });
                constexpr u64 align = 64ULL;
                constexpr u64 mask = ~(align - 1ULL);
                total_size_bytes += (new_size + align - 1) & mask;
            };
            gpu_modified_ranges.ForEachInRange(device_addr_out, range_size, add_download);
        });
    if (total_size_bytes == 0) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (ExecutorBufferReadbackContractTraceEnabled()) {
            const u64 ordinal =
                executor_buffer_empty_download_count.fetch_add(1, std::memory_order_relaxed) + 1;
            if (ExecutorShouldLogBufferReadbackOrdinal(ordinal)) {
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_READBACK_DOWNLOAD_EMPTY] n=%llu addr=0x%llx bytes=%llu "
                    "bufferBase=0x%llx isWrite=%u",
                    static_cast<unsigned long long>(ordinal),
                    static_cast<unsigned long long>(device_addr),
                    static_cast<unsigned long long>(size),
                    static_cast<unsigned long long>(buffer.CpuAddr()), is_write ? 1u : 0u);
            }
        }
#endif
        return;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (ExecutorBufferReadbackContractTraceEnabled()) {
        const u64 ordinal =
            executor_buffer_download_count.fetch_add(1, std::memory_order_relaxed) + 1;
        const u64 aggregate_bytes =
            executor_buffer_download_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed) +
            total_size_bytes;
        if (ExecutorShouldLogBufferReadbackOrdinal(ordinal)) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_READBACK_DOWNLOAD] n=%llu addr=0x%llx requestBytes=%llu "
                "downloadBytes=%llu aggregateBytes=%llu copies=%zu bufferBase=0x%llx isWrite=%u "
                "async=%u",
                static_cast<unsigned long long>(ordinal),
                static_cast<unsigned long long>(device_addr),
                static_cast<unsigned long long>(size),
                static_cast<unsigned long long>(total_size_bytes),
                static_cast<unsigned long long>(aggregate_bytes), copies.size(),
                static_cast<unsigned long long>(buffer.CpuAddr()), is_write ? 1u : 0u,
                async ? 1u : 0u);
        }
    }
#endif
    const auto [download, offset] = download_buffer.Map(total_size_bytes);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (download == nullptr) {
        AccumulateReadbackOptimizationStats(ReadbackOptimizationStats{
            .cpu_plan_ns = ExecutorElapsedNs(plan_start),
            .fallbacks = 1,
            .publication_failures = 1,
        });
        return;
    }
#endif
    for (auto& copy : copies) {
        copy.dstOffset += offset;
    }
    download_buffer.Commit();
    scheduler.EndRendering(Vulkan::Scheduler::EndRenderingReason::BufferReadback);
    const auto cmdbuf = scheduler.CommandBuffer();
    // A completion wait after the copy does not make earlier shader writes
    // visible to that copy.  Establish the required device-side dependency
    // before reading the source buffer.  This stays entirely inside the
    // already-synchronous demand-readback path and adds no new CPU wait or
    // submission on the normal draw path.
    const vk::BufferMemoryBarrier2 pre_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead |
                         vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .buffer = buffer.buffer,
        .offset = 0,
        .size = buffer.SizeBytes(),
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_barrier,
    });
    cmdbuf.copyBuffer(buffer.buffer, download_buffer.Handle(), copies);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    u64 copied_bytes = 0;
    u64 copied_pages = 0;
    for (const auto& copy : copies) {
        copied_bytes += copy.size;
        copied_pages +=
            (copy.size + static_cast<u64>(TRACKER_BYTES_PER_PAGE) - 1) /
            static_cast<u64>(TRACKER_BYTES_PER_PAGE);
    }
    const auto queued_at = ExecutorReadbackDiagnosticStart();
    AccumulateReadbackOptimizationStats(ReadbackOptimizationStats{
        .snapshots = 1,
        .demand_snapshots = 1,
        .ranges = static_cast<u64>(copies.size()),
        .pages = copied_pages,
        .copy_commands = 1,
        .copy_regions = static_cast<u64>(copies.size()),
        .bytes = copied_bytes,
        .cpu_plan_ns = ExecutorElapsedNs(plan_start),
    });
    ExecutorMaybeLogReadbackOptimizationStats();
#endif
    auto write_data =
        [this, buffer_cpu_addr = buffer.CpuAddr(), copies, download, offset, device_addr, size,
         is_write, snapshot_guard
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
         ,
         queued_at
#endif
    ]() {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        const auto publish_start = ExecutorReadbackDiagnosticStart();
#endif
        auto* memory = Core::Memory::Instance();
        bool wrote_all = true;
        for (const auto& copy : copies) {
            const VAddr copy_device_addr = buffer_cpu_addr + copy.srcOffset;
            const u64 dst_offset = copy.dstOffset - offset;
            // The synchronous caller can already pin topology while refreshing
            // a texture. Reuse that lease instead of upgrading its shared lock.
            const bool wrote = snapshot_guard != nullptr
                ? memory->TryWriteBacking(*snapshot_guard,
                      std::bit_cast<u8*>(copy_device_addr), download + dst_offset, copy.size)
                : memory->TryWriteBacking(std::bit_cast<u8*>(copy_device_addr),
                      download + dst_offset, copy.size);
            if (!wrote) {
                wrote_all = false;
                static std::atomic<u32> failure_count{0};
                const u32 failure = failure_count.fetch_add(1, std::memory_order_relaxed);
                if (failure < 16) {
                    LOG_ERROR(Render_Vulkan,
                              "GPU buffer readback could not write guest backing: addr={:#x}, "
                              "size={:#x}, occurrence={}",
                              copy_device_addr, copy.size, failure + 1);
                }
            }
        }
        if (!wrote_all) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            AccumulateReadbackOptimizationStats(ReadbackOptimizationStats{
                .cpu_publish_ns = ExecutorElapsedNs(publish_start),
                .fallbacks = 1,
                .publication_failures = 1,
            });
#endif
            return;
        }
        NotifyGuestReadback();
        for (const auto& copy : copies) {
            gpu_modified_ranges.Subtract(buffer_cpu_addr + copy.srcOffset, copy.size);
        }
        memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
        if (is_write) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            RecordDmaCleanEpochCpuPublication();
#endif
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
        liverpool->NotifyMemoryPublicationForExecutor(device_addr, size);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        AccumulateReadbackOptimizationStats(ReadbackOptimizationStats{
            .timeline_waits = async ? 1u : 0u,
            .cpu_publish_ns = ExecutorElapsedNs(publish_start),
            .gpu_completion_ns = async ? ExecutorElapsedNs(queued_at) : 0u,
        });
#endif
    };
    if constexpr (async) {
        scheduler.DeferOperation(std::move(write_data));
    } else {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        const auto wait_start = ExecutorReadbackDiagnosticStart();
#endif
        scheduler.Finish(Vulkan::Scheduler::ExecutorFinishReason::BufferReadbackDirect);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        const u64 wait_ns = ExecutorElapsedNs(wait_start);
        AccumulateReadbackOptimizationStats(ReadbackOptimizationStats{
            .timeline_waits = 1,
            .cpu_wait_ns = wait_ns,
            .gpu_completion_ns = wait_ns,
        });
#endif
        write_data();
    }
}

BufferCache::PreparedVertexBuffers BufferCache::PrepareVertexBuffers(
    const Vulkan::GraphicsPipeline& pipeline,
    boost::container::small_vector<vk::BufferMemoryBarrier2, 16>& barriers,
    const bool executor_probe_bound, const u64 executor_probe_sequence) {
    const auto& regs = liverpool->regs;
    Vulkan::VertexInputs<vk::VertexInputAttributeDescription2EXT> attributes;
    Vulkan::VertexInputs<vk::VertexInputBindingDescription2EXT> bindings;
    Vulkan::VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
    Vulkan::VertexInputs<AmdGpu::Buffer> guest_buffers;
    pipeline.GetVertexInputs(attributes, bindings, divisors, guest_buffers,
                             regs.vgt_instance_step_rate_0, regs.vgt_instance_step_rate_1);

    if (Libraries::GnmDriver::ExecutorReplayActive()) {
        for (auto& buffer : guest_buffers) {
            if (buffer.base_address != 0) {
                const u64 nb = Libraries::GnmDriver::ExecutorReplayRelocate(buffer.base_address);
                if (nb) {
                    buffer.base_address = nb;
                }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
                u32 v0 = 0, v1 = 0;
                if (Libraries::GnmDriver::ExecutorReplayIsRelocated(buffer.base_address)) {
                    const u32* p = reinterpret_cast<const u32*>(buffer.base_address);
                    v0 = p[0];
                    v1 = p[1];
                }
                __android_log_print(ANDROID_LOG_INFO, "EXECUTOR",
                                    "[EXECUTOR_VBUF] base=0x%llx stride=%u size=%u reloc=%d v=%08x %08x",
                                    (unsigned long long)buffer.base_address, (unsigned)buffer.GetStride(),
                                    (unsigned)buffer.GetSize(), nb ? 1 : 0, v0, v1);
#endif
            }
        }
    }

    struct BufferRange {
        VAddr base_address;
        VAddr end_address;
        vk::Buffer vk_buffer;
        u64 offset;

        [[nodiscard]] size_t GetSize() const {
            return end_address - base_address;
        }
    };

    Vulkan::VertexInputs<BufferRange> ranges{};
#if defined(LSX4_APPLE_RUNTIME)
    Vulkan::VertexInputs<u64> admitted_vertex_sizes{};
    bool invalid_vertex_range = false;
#endif
    for (const auto& buffer : guest_buffers) {
#if defined(LSX4_APPLE_RUNTIME)
        // Clamp individual descriptors before unioning: a sentinel-sized range
        // must not absorb another heap across an unmapped virtual-address hole.
        const u64 admitted_size = memory->ClampRangeSize(buffer.base_address, buffer.GetSize());
        admitted_vertex_sizes.push_back(admitted_size);
        invalid_vertex_range |= buffer.GetSize() != 0 && admitted_size == 0;
        if (admitted_size > 0) {
            ranges.emplace_back(buffer.base_address, buffer.base_address + admitted_size);
        }
#else
        if (buffer.GetSize() > 0) {
            ranges.emplace_back(buffer.base_address, buffer.base_address + buffer.GetSize());
        }
#endif
    }

    Vulkan::VertexInputs<BufferRange> ranges_merged{};
    if (!ranges.empty()) {
        std::ranges::sort(ranges, [](const BufferRange& lhv, const BufferRange& rhv) {
            return lhv.base_address < rhv.base_address;
        });
        ranges_merged.emplace_back(ranges[0]);
        for (auto range : ranges) {
            auto& prev_range = ranges_merged.back();
            if (prev_range.end_address < range.base_address) {
                ranges_merged.emplace_back(range);
            } else {
                prev_range.end_address = std::max(prev_range.end_address, range.end_address);
            }
        }
    }

    PreparedVertexBuffers prepared{};
#if defined(LSX4_APPLE_RUNTIME)
    if (invalid_vertex_range) {
        prepared.valid = false;
        return prepared;
    }
#endif
    prepared.dynamic_vertex_input = instance.IsVertexInputDynamicState();
    prepared.attributes.assign(attributes.begin(), attributes.end());
    prepared.bindings.assign(bindings.begin(), bindings.end());

    const auto null_buffer =
        instance.IsNullDescriptorSupported() ? VK_NULL_HANDLE : GetBuffer(NULL_BUFFER_ID).Handle();
    for (auto& range : ranges_merged) {
        const u64 size = memory->ClampRangeSize(range.base_address, range.GetSize());
        if (size == 0) {
            // A tiny null buffer is not a safe replacement for a malformed
            // gigabyte-sized vertex descriptor: Qualcomm may still fetch past
            // it and terminate the device.  Propagate rejection to the draw
            // caller, which drops this one invalid packet before recording any
            // Vulkan draw. Valid ranges and the ordinary hot path are unchanged.
            prepared.valid = false;
            return prepared;
        }
#if defined(LSX4_APPLE_RUNTIME)
        // Keep range lookup, diagnostics and binding limits consistent with the
        // bytes actually allocated, even if a mapping changed during admission.
        range.end_address = range.base_address + size;
#endif
        const auto [buffer, offset] =
            ObtainReadOnlyBuffer(range.base_address, static_cast<u32>(size),
                                 ReadOnlyBufferUsage::Vertex);
        range.vk_buffer = buffer->buffer;
        range.offset = offset;
        if (IsRegionGpuModified(range.base_address, size)) {
            if (auto barrier =
                    buffer->GetBarrier(vk::AccessFlagBits2::eVertexAttributeRead,
                                       vk::PipelineStageFlagBits2::eVertexAttributeInput)) {
                barriers.emplace_back(*barrier);
            }
        }
    }

#if defined(LSX4_APPLE_RUNTIME)
    u32 vertex_binding_index = 0;
#endif
    for (const auto& buffer : guest_buffers) {
        if (buffer.GetSize() > 0) {
            const auto host_buffer_info =
                std::ranges::find_if(ranges_merged, [&](const BufferRange& range) {
                    return buffer.base_address >= range.base_address &&
                           buffer.base_address < range.end_address;
                });
#if defined(LSX4_APPLE_RUNTIME)
            if (host_buffer_info == ranges_merged.cend() ||
                admitted_vertex_sizes[vertex_binding_index] >
                    host_buffer_info->end_address - buffer.base_address) {
                prepared.valid = false;
                return prepared;
            }
#endif
            ASSERT(host_buffer_info != ranges_merged.cend());
            prepared.buffers.emplace_back(host_buffer_info->vk_buffer);
            prepared.offsets.push_back(host_buffer_info->offset + buffer.base_address -
                                       host_buffer_info->base_address);
        } else {
            prepared.buffers.emplace_back(null_buffer);
            prepared.offsets.push_back(0);
        }
#if defined(LSX4_APPLE_RUNTIME)
        prepared.sizes.push_back(admitted_vertex_sizes[vertex_binding_index++]);
#else
        prepared.sizes.push_back(buffer.GetSize());
#endif
        prepared.strides.push_back(buffer.GetStride());
    }

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (executor_probe_bound) {
        for (u32 i = 0; i < ranges_merged.size(); ++i) {
            const auto& range = ranges_merged[i];
            if (range.base_address != 0 && range.GetSize() != 0 &&
                range.vk_buffer != VK_NULL_HANDLE) {
                ExecutorProbeBoundGpuBuffer(range.base_address, static_cast<u32>(range.GetSize()),
                                            range.vk_buffer, range.offset, "vertex_range",
                                            executor_probe_sequence, i);
            }
        }
    }
#else
    (void)executor_probe_bound;
    (void)executor_probe_sequence;
#endif

    return prepared;
}

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
void BufferCache::ExecutorMaybeLogBindDedup() {
    if (VideoCore::IsExecutorLiveQuiet()) {
        return;
    }
    const u64 total_attempts = executor_vertex_input_bind_attempts +
                               executor_vertex_buffer_bind_attempts +
                               executor_index_buffer_bind_attempts;
    if (total_attempts < executor_bind_dedup_next_log) {
        return;
    }
    executor_bind_dedup_next_log =
        (total_attempts / 16384u + 1u) * 16384u;
    const auto hit_percent = [](const u64 attempts, const u64 hits) {
        return attempts != 0
                   ? 100.0 * static_cast<double>(hits) /
                         static_cast<double>(attempts)
                   : 0.0;
    };
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_VK_BIND_DEDUP] kind=vertexIndex total=%llu "
        "input=%llu/%llu(%.1f%%) vertex=%llu/%llu(%.1f%%) "
        "index=%llu/%llu(%.1f%%)",
        static_cast<unsigned long long>(total_attempts),
        static_cast<unsigned long long>(executor_vertex_input_bind_hits),
        static_cast<unsigned long long>(executor_vertex_input_bind_attempts),
        hit_percent(executor_vertex_input_bind_attempts,
                    executor_vertex_input_bind_hits),
        static_cast<unsigned long long>(executor_vertex_buffer_bind_hits),
        static_cast<unsigned long long>(executor_vertex_buffer_bind_attempts),
        hit_percent(executor_vertex_buffer_bind_attempts,
                    executor_vertex_buffer_bind_hits),
        static_cast<unsigned long long>(executor_index_buffer_bind_hits),
        static_cast<unsigned long long>(executor_index_buffer_bind_attempts),
        hit_percent(executor_index_buffer_bind_attempts,
                    executor_index_buffer_bind_hits));
}
#endif

void BufferCache::BindPreparedVertexBuffers(const PreparedVertexBuffers& prepared) {
    const auto cmdbuf = scheduler.CommandBuffer();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool dedup_enabled = IsManagedVulkanVertexBindDeduplication();
    const u64 recording_tick = scheduler.CurrentTick();
    const u64 rendering_epoch = scheduler.RenderingEpoch();
    const u64 graphics_pipeline_epoch =
        scheduler.PipelineBindEpoch(vk::PipelineBindPoint::eGraphics);
    if (executor_vertex_bind_cmdbuf != cmdbuf ||
        executor_vertex_bind_recording_tick != recording_tick ||
        executor_vertex_bind_rendering_epoch != rendering_epoch ||
        executor_vertex_bind_graphics_pipeline_epoch != graphics_pipeline_epoch) {
        executor_vertex_bind_cmdbuf = cmdbuf;
        executor_vertex_bind_recording_tick = recording_tick;
        executor_vertex_bind_rendering_epoch = rendering_epoch;
        executor_vertex_bind_graphics_pipeline_epoch = graphics_pipeline_epoch;
        // Qualcomm's proprietary driver can consume stale vertex input/buffer
        // state after a graphics-pipeline transition. Preserve deduplication
        // within the pipeline epoch, but replay both binds at its boundary.
        executor_vertex_input_bind_valid = false;
        executor_vertex_buffer_bind_valid = false;
    }

    if (prepared.dynamic_vertex_input) {
        ++executor_vertex_input_bind_attempts;
        const u64 input_epoch = scheduler.VertexInputEpoch();
        const bool input_hit =
            dedup_enabled &&
            executor_vertex_input_bind_valid &&
            executor_vertex_input_bind_epoch == input_epoch &&
            std::ranges::equal(executor_bound_vertex_state.bindings,
                               prepared.bindings) &&
            std::ranges::equal(executor_bound_vertex_state.attributes,
                               prepared.attributes);
        executor_vertex_input_bind_hits += static_cast<u64>(input_hit);
        if (!input_hit) {
            cmdbuf.setVertexInputEXT(prepared.bindings, prepared.attributes);
            executor_vertex_input_bind_epoch = scheduler.MarkVertexInput();
            executor_bound_vertex_state.bindings = prepared.bindings;
            executor_bound_vertex_state.attributes = prepared.attributes;
            executor_vertex_input_bind_valid = true;
        }
    }
#else
    if (prepared.dynamic_vertex_input) {
        cmdbuf.setVertexInputEXT(prepared.bindings, prepared.attributes);
        scheduler.MarkVertexInput();
    }
#endif
    if (prepared.buffers.empty()) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        ExecutorMaybeLogBindDedup();
#endif
        return;
    }

    const auto num_buffers = static_cast<u32>(prepared.buffers.size());
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    ++executor_vertex_buffer_bind_attempts;
    const u64 vertex_buffer_epoch = scheduler.VertexBufferBindEpoch();
    constexpr bool raw_buffer_bind_dedup_enabled = false;
    const bool buffer_hit =
        raw_buffer_bind_dedup_enabled && dedup_enabled &&
        executor_vertex_buffer_bind_valid &&
        executor_vertex_buffer_bind_epoch == vertex_buffer_epoch &&
        executor_bound_vertex_state.dynamic_vertex_input ==
            prepared.dynamic_vertex_input &&
        std::ranges::equal(executor_bound_vertex_state.buffers,
                           prepared.buffers) &&
        std::ranges::equal(executor_bound_vertex_state.offsets,
                           prepared.offsets) &&
        (prepared.dynamic_vertex_input ||
         (std::ranges::equal(executor_bound_vertex_state.sizes,
                             prepared.sizes) &&
          std::ranges::equal(executor_bound_vertex_state.strides,
                             prepared.strides)));
    executor_vertex_buffer_bind_hits += static_cast<u64>(buffer_hit);
    if (!buffer_hit) {
#endif
        [[maybe_unused]] u64 post_vertex_buffer_epoch{};
        if (prepared.dynamic_vertex_input || instance.UseLegacyRenderPass()) {
            cmdbuf.bindVertexBuffers(0, num_buffers, prepared.buffers.data(),
                                     prepared.offsets.data());
            post_vertex_buffer_epoch = scheduler.MarkVertexBufferBind();
        } else {
            cmdbuf.bindVertexBuffers2(0, num_buffers, prepared.buffers.data(),
                                      prepared.offsets.data(), prepared.sizes.data(),
                                      prepared.strides.data());
            post_vertex_buffer_epoch = scheduler.MarkVertexBufferBind();
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        executor_vertex_buffer_bind_epoch = post_vertex_buffer_epoch;
        executor_bound_vertex_state.buffers = prepared.buffers;
        executor_bound_vertex_state.offsets = prepared.offsets;
        executor_bound_vertex_state.sizes = prepared.sizes;
        executor_bound_vertex_state.strides = prepared.strides;
        executor_bound_vertex_state.dynamic_vertex_input =
            prepared.dynamic_vertex_input;
        executor_vertex_buffer_bind_valid = true;
    }
    ExecutorMaybeLogBindDedup();
#endif
}

BufferCache::PreparedIndexBuffer BufferCache::PrepareIndexBuffer(
    u32 index_offset, boost::container::small_vector<vk::BufferMemoryBarrier2, 16>& barriers,
    const bool executor_probe_bound, const u64 executor_probe_sequence) {
    const auto& regs = liverpool->regs;

    const bool is_index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
    const vk::IndexType index_type = is_index16 ? vk::IndexType::eUint16 : vk::IndexType::eUint32;
    const u32 index_size = is_index16 ? sizeof(u16) : sizeof(u32);
    const VAddr index_address =
        regs.index_base_address.Address<VAddr>() + index_offset * index_size;

    const u32 index_buffer_size = regs.num_indices * index_size;
    const auto [vk_buffer, offset] =
        ObtainReadOnlyBuffer(index_address, index_buffer_size, ReadOnlyBufferUsage::Index);
    if (IsRegionGpuModified(index_address, index_buffer_size)) {
        if (auto barrier = vk_buffer->GetBarrier(vk::AccessFlagBits2::eIndexRead,
                                                 vk::PipelineStageFlagBits2::eIndexInput)) {
            barriers.emplace_back(*barrier);
        }
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (executor_probe_bound) {
        ExecutorProbeBoundGpuBuffer(index_address, index_buffer_size, vk_buffer->Handle(), offset,
                                    "index_input", executor_probe_sequence, 0);
    }
#else
    (void)executor_probe_bound;
    (void)executor_probe_sequence;
#endif
    return PreparedIndexBuffer{
        .buffer = vk_buffer->Handle(),
        .offset = offset,
        .type = index_type,
    };
}

void BufferCache::BindPreparedIndexBuffer(const PreparedIndexBuffer& prepared) {
    const auto cmdbuf = scheduler.CommandBuffer();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool dedup_enabled = IsManagedVulkanVertexBindDeduplication();
    const u64 recording_tick = scheduler.CurrentTick();
    const u64 rendering_epoch = scheduler.RenderingEpoch();
    const u64 graphics_pipeline_epoch =
        scheduler.PipelineBindEpoch(vk::PipelineBindPoint::eGraphics);
    if (executor_index_bind_cmdbuf != cmdbuf ||
        executor_index_bind_recording_tick != recording_tick ||
        executor_index_bind_rendering_epoch != rendering_epoch ||
        executor_index_bind_graphics_pipeline_epoch != graphics_pipeline_epoch) {
        executor_index_bind_cmdbuf = cmdbuf;
        executor_index_bind_recording_tick = recording_tick;
        executor_index_bind_rendering_epoch = rendering_epoch;
        executor_index_bind_graphics_pipeline_epoch = graphics_pipeline_epoch;
        executor_index_buffer_bind_valid = false;
    }
    ++executor_index_buffer_bind_attempts;
    const u64 index_buffer_epoch = scheduler.IndexBufferBindEpoch();
    constexpr bool raw_buffer_bind_dedup_enabled = false;
    const bool index_hit =
        raw_buffer_bind_dedup_enabled && dedup_enabled &&
        executor_index_buffer_bind_valid &&
        executor_index_buffer_bind_epoch == index_buffer_epoch &&
        executor_bound_index_state.buffer == prepared.buffer &&
        executor_bound_index_state.offset == prepared.offset &&
        executor_bound_index_state.type == prepared.type;
    executor_index_buffer_bind_hits += static_cast<u64>(index_hit);
    if (!index_hit) {
#endif
        cmdbuf.bindIndexBuffer(prepared.buffer, prepared.offset, prepared.type);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        executor_index_buffer_bind_epoch = scheduler.MarkIndexBufferBind();
        executor_bound_index_state = prepared;
        executor_index_buffer_bind_valid = true;
    }
    ExecutorMaybeLogBindDedup();
#else
        scheduler.MarkIndexBufferBind();
#endif
}

void BufferCache::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    SCOPE_EXIT { if (!is_gds) PortableGpu::InvalidateProducerRange(this,address,num_bytes); };
    if (num_bytes == 0) {
        return;
    }
    ASSERT_MSG(address % 4 == 0, "GDS offset must be dword aligned");
    if (!is_gds) {
        texture_cache.ClearMeta(address);
        if (!IsRegionGpuModified(address, num_bytes)) {
            u32* buffer = std::bit_cast<u32*>(address);
            std::fill(buffer, buffer + num_bytes / sizeof(u32), value);
            return;
        }
    }
    if (is_gds) {
        gds_buffer.Fill(gds_buffer.Offset(address), num_bytes, value);
        return;
    }
    const auto [buffer, offset] = ObtainBuffer(address, num_bytes, true);
    if (buffer == &GetBuffer(NULL_BUFFER_ID)) {
        return;
    }
    buffer->Fill(offset, num_bytes, value);
}

void BufferCache::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds,
                             bool defer_gnm_dma) {
    SCOPE_EXIT { if (!dst_gds) PortableGpu::InvalidateProducerRange(this,dst,num_bytes); };
    if (num_bytes == 0) {
        return;
    }
    if (!dst_gds && !IsRegionGpuModified(dst, num_bytes)) {
        if (!src_gds && !IsRegionGpuModified(src, num_bytes) &&
            !texture_cache.FindImageFromRange(src, num_bytes)) {
            memcpy(std::bit_cast<void*>(dst), std::bit_cast<void*>(src), num_bytes);
            return;
        }
    }
    texture_cache.InvalidateMemoryFromGPU(dst, num_bytes);
    const Buffer* src_buffer = [&]() -> const Buffer* {
        if (src_gds) {
            return &gds_buffer;
        }
        const auto buffer_id = FindBuffer(src, num_bytes);
        auto& buffer = slot_buffers[buffer_id];
        return SynchronizeBuffer(buffer, src, num_bytes, false, true) ==
                       BufferSyncResult::Ready
                   ? &buffer
                   : nullptr;
    }();
    const Buffer* dst_buffer = [&]() -> const Buffer* {
        if (dst_gds) {
            return &gds_buffer;
        }
        const auto buffer_id = FindBuffer(dst, num_bytes);
        auto& buffer = slot_buffers[buffer_id];
        if (SynchronizeBuffer(buffer, dst, num_bytes, true, true) ==
            BufferSyncResult::Failed) {
            return nullptr;
        }
        gpu_modified_ranges.Add(dst, num_bytes);
        return &buffer;
    }();
    if (src_buffer == nullptr || dst_buffer == nullptr) {
        return;
    }
    const vk::BufferCopy region = {
        .srcOffset = src_buffer->Offset(src),
        .dstOffset = dst_buffer->Offset(dst),
        .size = num_bytes,
    };
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (defer_gnm_dma && IsManagedVulkanTransferCoalescing()) {
        scheduler.QueueBufferCopies(
            src_buffer->Handle(), dst_buffer->Handle(),
            std::span<const vk::BufferCopy>{&region, 1},
            src_gds ? VAddr{0} : src, src_gds ? u64{0} : num_bytes,
            dst_gds ? VAddr{0} : dst, dst_gds ? u64{0} : num_bytes,
            Vulkan::Scheduler::BufferTransferOrigin::GnmDma);
        return;
    }
#else
    (void)defer_gnm_dma;
#endif
    const vk::BufferMemoryBarrier2 buf_barriers_before[2] = {
        {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .buffer = dst_buffer->Handle(),
            .offset = dst_buffer->Offset(dst),
            .size = num_bytes,
        },
        {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .buffer = src_buffer->Handle(),
            .offset = src_buffer->Offset(src),
            .size = num_bytes,
        },
    };
    scheduler.EndRendering(Vulkan::Scheduler::EndRenderingReason::BufferCopy);
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 2,
        .pBufferMemoryBarriers = buf_barriers_before,
    });
    cmdbuf.copyBuffer(src_buffer->Handle(), dst_buffer->Handle(), region);
    const vk::BufferMemoryBarrier2 buf_barriers_after[2] = {
        {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
            .buffer = dst_buffer->Handle(),
            .offset = dst_buffer->Offset(dst),
            .size = num_bytes,
        },
        {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .buffer = src_buffer->Handle(),
            .offset = src_buffer->Offset(src),
            .size = num_bytes,
        },
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 2,
        .pBufferMemoryBarriers = buf_barriers_after,
    });
}

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
static void ExecutorFlagSuspectGpuDescriptor(const char* site, VAddr addr, u64 size) {
    // PS4 GPU virtual addresses occupy a 40-bit guest range. The generic Android guest
    // layout legitimately places resources above the old 64-GiB diagnostic threshold.
    // Keep this diagnostic aligned with MemoryManager::IsValidGpuMapping and reject
    // overflow without performing addr + size.
    constexpr VAddr GuestGpuAddressLimit = 0x10000000000ull;
    const bool bad_addr = (addr != 0 && addr < 0x10000ull) || addr >= GuestGpuAddressLimit;
    const bool bad_size = size > GuestGpuAddressLimit ||
                          (addr < GuestGpuAddressLimit && size > GuestGpuAddressLimit - addr);
    if (!bad_addr && !bad_size) {
        return;
    }
    static std::atomic<u32> budget{64};
    u32 remaining = budget.load(std::memory_order_relaxed);
    while (remaining != 0 &&
           !budget.compare_exchange_weak(remaining, remaining - 1, std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
    }
    if (remaining == 0) {
        return;
    }
    __android_log_print(ANDROID_LOG_ERROR, "LSX4Native",
                        "[EXECUTOR_SUSPECT_GPU_DESCRIPTOR] site=%s addr=0x%llx size=0x%llx "
                        "badAddr=%d badSize=%d (JIT garbage sharp)",
                        site, static_cast<unsigned long long>(addr),
                        static_cast<unsigned long long>(size), bad_addr ? 1 : 0, bad_size ? 1 : 0);
    if (std::FILE* cf = std::fopen(
            "/data/data/app.lsx4.android/files/lsx4-home/jit-checkfail.txt", "a")) {
        std::fprintf(cf, "SUSPECT_GPU_DESCRIPTOR site=%s addr=0x%llx size=0x%llx\n", site,
                     static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size));
        std::fclose(cf);
    }
}
#endif

std::pair<Buffer*, u32> BufferCache::ObtainBuffer(VAddr device_addr, u32 size, bool is_written,
                                                   bool is_texel_buffer, BufferId buffer_id,
                                                   const bool transfer_range_checked,
                                                   const bool stable_read_only_candidate,
                                                   const bool fex_texture_cache_lock_held,
                                                   Core::MemoryManager::SnapshotReadGuard*
                                                       external_snapshot_guard) {
    if (!transfer_range_checked) {
        scheduler.FlushPendingBufferTransfersForRange(device_addr, size);
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    ExecutorFlagSuspectGpuDescriptor("ObtainBuffer", device_addr, size);
#endif
    if (!is_written && Libraries::GnmDriver::ExecutorGnmCaptureActive()) {
        Libraries::GnmDriver::ExecutorGnmCaptureRecordResource(3u, device_addr, size);
    }
    Libraries::GnmDriver::ExecutorReplayCheckAddr(3u, device_addr, size);
    if (IsBufferInvalid(buffer_id)) {
        buffer_id = FindBuffer(device_addr, size, true);
    }
    Buffer& buffer = slot_buffers[buffer_id];
    if (SynchronizeBuffer(buffer, device_addr, size, is_written,
                          is_texel_buffer, stable_read_only_candidate,
                          fex_texture_cache_lock_held,
                          external_snapshot_guard) ==
        BufferSyncResult::Failed) {
        return {&GetBuffer(NULL_BUFFER_ID), 0};
    }
    if (is_written) {
        if (liverpool->IsGpuThread() && !Config::managedGpuFastPath()) {
            PortableGpu::RecordPendingProducer(this,&scheduler,
                PortableGpu::ProducerAllocation{reinterpret_cast<std::uintptr_t>(&buffer),
                    std::bit_cast<std::uint64_t>(static_cast<VkBuffer>(buffer.Handle()))},device_addr,size);
        } else {
            PortableGpu::InvalidateProducerRange(this,device_addr,size);
        }
        gpu_modified_ranges.Add(device_addr, size);
        static const bool claude_hot = [] {
            const char* value = std::getenv("EXECUTOR_CLAUDE_HOT_FLUSH");
            return value && (value[0] == '1' || value[0] == '2');
        }();
        if (claude_hot && size != 0) {
            const VAddr end = device_addr + size;
            for (u32 index = 0; index < recent_readback_window_count; ++index) {
                const RecentReadbackWindow& window = recent_readback_windows[index];
                if (window.size != 0 && window.begin < end &&
                    device_addr < window.begin + window.size) {
                    claude_hot_written.store(1, std::memory_order_relaxed);
                    claude_hot_marks.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
        }
    }
    return {&buffer, buffer.Offset(device_addr)};
}

std::pair<Buffer*, u32> BufferCache::ObtainReadOnlyBuffer(
    const VAddr device_addr, const u32 size, const ReadOnlyBufferUsage usage,
    const BufferId fallback_buffer_id, const bool transfer_range_checked) {
#if !defined(__ANDROID__) && !defined(LSX4_APPLE_RUNTIME)
    (void)usage;
    return ObtainBuffer(device_addr, size, false, false, fallback_buffer_id,
                        transfer_range_checked);
#else
    ASSERT(usage < ReadOnlyBufferUsage::Count);
    const bool descriptor_request = usage == ReadOnlyBufferUsage::Uniform ||
                                    usage == ReadOnlyBufferUsage::Storage;
    if (!transfer_range_checked) {
        scheduler.FlushPendingBufferTransfersForRange(device_addr, size);
    }
    if (!ExecutorReadOnlyRenameEnabled()) {
        return ObtainBuffer(device_addr, size, false, false,
                            fallback_buffer_id, true);
    }

    ++read_only_rename_attempts;
    ++read_only_rename_usage_counts[static_cast<std::size_t>(usage)];
    const auto maybe_log = [&] {
        const bool trace_enabled = ExecutorReadOnlyRenameTraceEnabled();
        if (!trace_enabled) {
            return;
        }
        constexpr u64 log_mask = 16383u;
        if (read_only_rename_attempts != 256 &&
            (read_only_rename_attempts & log_mask) != 0u) {
            return;
        }
        const auto fallback_count = [&](const ReadOnlyRenameFallback reason) {
            return read_only_rename_fallback_counts[static_cast<std::size_t>(reason)];
        };
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_READ_ONLY_RENAME] attempts=%llu renamed=%llu bytes=%llu "
            "usage=vtx:%llu,idx:%llu,ubo:%llu,ssbo:%llu,ind:%llu,hle:%llu "
            "renamedUsage=vtx:%llu,idx:%llu,ubo:%llu,ssbo:%llu,ind:%llu,hle:%llu "
            "fallback=gpu:%llu,image:%llu,clean:%llu,unreg:%llu,read:%llu,large:%llu,"
            "coherent:%llu,budget:%llu,ring:%llu,copy:%llu "
            "snapshot=arena:%llu,batch:%llu/%llu/%llu,allocFail:%llu,"
            "exhaust:%llu,fallback:%llu/%llu,high:%llu,maxBatch:%llu,maxLag:%llu",
            static_cast<unsigned long long>(read_only_rename_attempts),
            static_cast<unsigned long long>(read_only_rename_ranges),
            static_cast<unsigned long long>(read_only_rename_bytes),
            static_cast<unsigned long long>(
                read_only_rename_usage_counts[static_cast<std::size_t>(
                    ReadOnlyBufferUsage::Vertex)]),
            static_cast<unsigned long long>(
                read_only_rename_usage_counts[static_cast<std::size_t>(
                    ReadOnlyBufferUsage::Index)]),
            static_cast<unsigned long long>(
                read_only_rename_usage_counts[static_cast<std::size_t>(
                    ReadOnlyBufferUsage::Uniform)]),
            static_cast<unsigned long long>(
                read_only_rename_usage_counts[static_cast<std::size_t>(
                    ReadOnlyBufferUsage::Storage)]),
            static_cast<unsigned long long>(
                read_only_rename_usage_counts[static_cast<std::size_t>(
                    ReadOnlyBufferUsage::Indirect)]),
            static_cast<unsigned long long>(
                read_only_rename_usage_counts[static_cast<std::size_t>(
                    ReadOnlyBufferUsage::HleTransfer)]),
            static_cast<unsigned long long>(
                read_only_rename_success_usage_counts[static_cast<std::size_t>(
                    ReadOnlyBufferUsage::Vertex)]),
            static_cast<unsigned long long>(
                read_only_rename_success_usage_counts[static_cast<std::size_t>(
                    ReadOnlyBufferUsage::Index)]),
            static_cast<unsigned long long>(
                read_only_rename_success_usage_counts[static_cast<std::size_t>(
                    ReadOnlyBufferUsage::Uniform)]),
            static_cast<unsigned long long>(
                read_only_rename_success_usage_counts[static_cast<std::size_t>(
                    ReadOnlyBufferUsage::Storage)]),
            static_cast<unsigned long long>(
                read_only_rename_success_usage_counts[static_cast<std::size_t>(
                    ReadOnlyBufferUsage::Indirect)]),
            static_cast<unsigned long long>(
                read_only_rename_success_usage_counts[static_cast<std::size_t>(
                    ReadOnlyBufferUsage::HleTransfer)]),
            static_cast<unsigned long long>(
                fallback_count(ReadOnlyRenameFallback::GpuModified)),
            static_cast<unsigned long long>(
                fallback_count(ReadOnlyRenameFallback::ImageAlias)),
            static_cast<unsigned long long>(
                fallback_count(ReadOnlyRenameFallback::NotCpuDirty)),
            static_cast<unsigned long long>(
                fallback_count(ReadOnlyRenameFallback::Unregistered)),
            static_cast<unsigned long long>(
                fallback_count(ReadOnlyRenameFallback::NotReadable)),
            static_cast<unsigned long long>(
                fallback_count(ReadOnlyRenameFallback::TooLarge)),
            static_cast<unsigned long long>(
                fallback_count(ReadOnlyRenameFallback::StreamNotCoherent)),
            static_cast<unsigned long long>(
                fallback_count(ReadOnlyRenameFallback::TickBudget)),
            static_cast<unsigned long long>(
                fallback_count(ReadOnlyRenameFallback::RingBusy)),
            static_cast<unsigned long long>(
                fallback_count(ReadOnlyRenameFallback::CopyFailed)),
            static_cast<unsigned long long>(descriptor_snapshot_selected_bytes),
            static_cast<unsigned long long>(descriptor_snapshot_batches_started),
            static_cast<unsigned long long>(descriptor_snapshot_batches_retired),
            static_cast<unsigned long long>(descriptor_snapshot_batches_canceled),
            static_cast<unsigned long long>(descriptor_snapshot_allocation_failures),
            static_cast<unsigned long long>(descriptor_snapshot_exhaustions),
            static_cast<unsigned long long>(descriptor_snapshot_fallback_attempts),
            static_cast<unsigned long long>(descriptor_snapshot_fallback_successes),
            static_cast<unsigned long long>(descriptor_snapshot_high_water_bytes),
            static_cast<unsigned long long>(descriptor_snapshot_max_batch_bytes),
            static_cast<unsigned long long>(descriptor_snapshot_max_tick_lag));
    };
    const auto account_fallback = [&](const ReadOnlyRenameFallback reason,
                                      auto&& obtain_buffer) {
        ++read_only_rename_fallback_counts[static_cast<std::size_t>(reason)];
        maybe_log();
        descriptor_snapshot_fallback_attempts += descriptor_request;
        auto result = obtain_buffer();
        descriptor_snapshot_fallback_successes +=
            descriptor_request && result.first != nullptr &&
            result.first != &GetBuffer(NULL_BUFFER_ID);
        return result;
    };
    const auto fallback = [&](const ReadOnlyRenameFallback reason) {
        return account_fallback(reason, [&] {
            return ObtainBuffer(device_addr, size, false, false,
                                fallback_buffer_id, true);
        });
    };
    if (!ExecutorReadOnlyRenameUsageEnabled(usage)) {
        const bool stable_read_only_candidate =
            descriptor_request && ExecutorStableCanonicalStreamingEnabled() &&
            device_addr != 0 && size != 0 && size <= 64_KB &&
            !texture_cache.FindImageFromRange(device_addr, size);
        return ObtainBuffer(device_addr, size, false, false,
                            fallback_buffer_id, true,
                            stable_read_only_candidate);
    }

    if (device_addr == 0 || size == 0) {
        ++read_only_rename_fallback_counts[static_cast<std::size_t>(
            ReadOnlyRenameFallback::NotReadable)];
        maybe_log();
        return {&GetBuffer(NULL_BUFFER_ID), 0};
    }
    if (IsRegionGpuModified(device_addr, size) ||
        gpu_modified_ranges.Intersects(device_addr, size)) {
        return fallback(ReadOnlyRenameFallback::GpuModified);
    }
    const bool replay_memory = Libraries::GnmDriver::ExecutorReplayActive();
    const bool valid_range =
        device_addr <= std::numeric_limits<VAddr>::max() - size;
    const bool fully_registered =
        valid_range && buffer_ranges.Contains(device_addr, size);
    if (!fully_registered && !replay_memory) {
        return fallback(ReadOnlyRenameFallback::Unregistered);
    }
    if (!replay_memory) {
        bool snapshot_dirty =
            IsRegionCpuModified(device_addr, size) ||
            memory_tracker->IsRegionSnapshotStale(device_addr, size);
        if (!snapshot_dirty && !memory->PhysicalAliasingMayExist()) {
            return fallback(ReadOnlyRenameFallback::NotCpuDirty);
        }
        if (!snapshot_dirty) {
            Core::MemoryManager::SnapshotWritePlan snapshot_plan;
            bool snapshot_plan_built = false;
            bool alias_snapshot_dirty = false;
            {
                auto memory_guard = memory->AcquireSnapshotReadGuard();
                snapshot_plan_built = memory->BuildSnapshotWritePlan(
                    memory_guard, device_addr, size, snapshot_plan);
                if (snapshot_plan_built) {
                    alias_snapshot_dirty =
                        memory_tracker->AreSnapshotRangesDirty(
                            snapshot_plan.AliasRanges());
                }
            }
            if (!snapshot_plan_built) {
                return fallback(ReadOnlyRenameFallback::CopyFailed);
            }
            snapshot_dirty = alias_snapshot_dirty;
        }
        if (!snapshot_dirty) {
            return fallback(ReadOnlyRenameFallback::NotCpuDirty);
        }
    }
    if (texture_cache.FindImageFromRange(device_addr, size)) {
        return fallback(ReadOnlyRenameFallback::ImageAlias);
    }
    if (!memory->IsReadableMapping(device_addr, size) &&
        !replay_memory) {
        return fallback(ReadOnlyRenameFallback::NotReadable);
    }

    // A host-visible ring is a win for small per-draw constants, but putting
    // multi-megabyte vertex/index ranges in it causes frequent wraparound and
    // lets Qualcomm observe a recycled range while an older draw is still in
    // flight. Keep the original one-cache-page safety contract for every
    // read-only usage; large geometry remains in the tracked buffer cache.
    static constexpr u64 MaxRenameRangeBytes = CACHING_PAGESIZE;
    static constexpr u64 MaxRenameBytesPerTick = 16_MB;
    const bool descriptor_usage = descriptor_request;
    StreamBuffer* snapshot_buffer = &stream_buffer;
    if (descriptor_usage) {
        if (!descriptor_snapshot_batch_active ||
            (snapshot_buffer = EnsureDescriptorSnapshotBuffer()) == nullptr) {
            return fallback(ReadOnlyRenameFallback::RingBusy);
        }
    }
    if (size > MaxRenameRangeBytes || size > snapshot_buffer->SizeBytes()) {
        return fallback(ReadOnlyRenameFallback::TooLarge);
    }
    if (!snapshot_buffer->is_coherent || snapshot_buffer->mapped_data.empty()) {
        return fallback(ReadOnlyRenameFallback::StreamNotCoherent);
    }

    const u64 current_tick = scheduler.CurrentTick();
    if (read_only_rename_tick != current_tick) {
        read_only_rename_tick = current_tick;
        read_only_rename_tick_bytes = 0;
    }
    if (size > MaxRenameBytesPerTick -
                   std::min(read_only_rename_tick_bytes, MaxRenameBytesPerTick)) {
        return fallback(ReadOnlyRenameFallback::TickBudget);
    }

    const u64 alignment = [&] {
        switch (usage) {
        case ReadOnlyBufferUsage::Vertex:
            return u64{16};
        case ReadOnlyBufferUsage::Index:
            return u64{4};
        case ReadOnlyBufferUsage::Uniform:
            return static_cast<u64>(instance.UniformMinAlignment());
        case ReadOnlyBufferUsage::Storage:
            return static_cast<u64>(instance.StorageMinAlignment());
        case ReadOnlyBufferUsage::Indirect:
        case ReadOnlyBufferUsage::HleTransfer:
            return u64{4};
        case ReadOnlyBufferUsage::Count:
            break;
        }
        UNREACHABLE();
        return u64{16};
    }();
    const bool descriptor_batch_was_empty =
        descriptor_usage && !descriptor_snapshot_batch_lease;
    const auto [stream_data, stream_offset] =
        descriptor_usage
            ? snapshot_buffer->MapLeased(size, alignment, false,
                                         descriptor_snapshot_batch_lease)
            : snapshot_buffer->Map(size, alignment, false);
    if (stream_data == nullptr) {
        descriptor_snapshot_exhaustions += descriptor_usage;
        return fallback(ReadOnlyRenameFallback::RingBusy);
    }
    if (descriptor_batch_was_empty) {
        descriptor_snapshot_batch_initial_tick = current_tick;
    }
    if (descriptor_usage) {
        descriptor_snapshot_high_water_bytes =
            std::max(descriptor_snapshot_high_water_bytes, stream_offset + size);
        descriptor_snapshot_max_batch_bytes =
            std::max(descriptor_snapshot_max_batch_bytes,
                     descriptor_snapshot_batch_lease.end -
                         descriptor_snapshot_batch_lease.begin);
    }
    bool copy_succeeded = false;
    if (replay_memory) {
        copy_succeeded = memory->TryCopyReadableMemory(
            device_addr, stream_data, size, false);
    } else {
        SnapshotTransactionResult snapshot_result =
            SnapshotTransactionResult::Invalid;
        static constexpr u32 MaxSnapshotTransactionAttempts = 64;
        for (u32 attempt = 0; attempt < MaxSnapshotTransactionAttempts; ++attempt) {
            Core::MemoryManager::SnapshotWritePlan preflight_plan;
            {
                auto memory_guard = memory->AcquireSnapshotReadGuard();
                if (!memory->BuildSnapshotWritePlan(
                        memory_guard, device_addr, size, preflight_plan)) {
                    snapshot_result = SnapshotTransactionResult::Invalid;
                    break;
                }
            }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            bool backing_was_published = false;
            for (const auto& [range_addr, range_size] :
                 preflight_plan.AliasRanges()) {
                backing_was_published |=
                    WaitForPendingReadback(range_addr, range_size);
            }
            if (backing_was_published) {
                memory_tracker->MarkSnapshotRangesStale(
                    preflight_plan.AliasRanges());
            }
#endif
            {
                auto memory_guard = memory->AcquireSnapshotReadGuard();
                Core::MemoryManager::SnapshotWritePlan snapshot_plan;
                if (!memory->BuildSnapshotWritePlan(
                        memory_guard, device_addr, size, snapshot_plan)) {
                    snapshot_result = SnapshotTransactionResult::Invalid;
                } else {
                    snapshot_result = memory_tracker->WithCpuSnapshot(
                        device_addr, size, snapshot_plan.WriteRanges(),
                        snapshot_plan.AliasRanges(),
                        [this, &memory_guard, device_addr, stream_data, size] {
                            return memory->TryCopyBackingMemory(
                                memory_guard, device_addr, stream_data, size);
                        });
                }
            }
            if (DecideSnapshotTransactionAction(snapshot_result) !=
                SnapshotTransactionAction::Retry) {
                break;
            }
            std::this_thread::yield();
            if (attempt >= 8) {
                std::this_thread::sleep_for(std::chrono::microseconds{10});
            }
        }
        copy_succeeded = snapshot_result == SnapshotTransactionResult::Complete;
    }
    if (!copy_succeeded) {
        std::memset(stream_data, 0, size);
        if (descriptor_usage) {
            snapshot_buffer->CommitLeased(descriptor_snapshot_batch_lease);
        } else {
            snapshot_buffer->Commit();
            snapshot_buffer->RetainCurrentAllocation();
        }
        return fallback(ReadOnlyRenameFallback::CopyFailed);
    }
    if (descriptor_usage) {
        snapshot_buffer->CommitLeased(descriptor_snapshot_batch_lease);
    } else {
        snapshot_buffer->Commit();
        snapshot_buffer->RetainCurrentAllocation();
    }
    read_only_rename_tick_bytes += size;
    ++read_only_rename_ranges;
    ++read_only_rename_success_usage_counts[static_cast<std::size_t>(usage)];
    read_only_rename_bytes += size;
    maybe_log();
    if (descriptor_usage && ExecutorDescriptorSnapshotArenaEnabled()) {
        static std::atomic<bool> first_success_logged{false};
        if (!first_success_logged.exchange(true, std::memory_order_relaxed)) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_DESCRIPTOR_SNAPSHOT_ARENA] firstSuccess=1 usage=%s bytes=%u "
                "offset=%llu lease=%llu-%llu tick=%llu dedicated=1 exactCpuSnapshot=1 "
                "nonWrappingBatch=1 consumingTickRetirement=1",
                usage == ReadOnlyBufferUsage::Storage ? "storage" : "uniform", size,
                static_cast<unsigned long long>(stream_offset),
                static_cast<unsigned long long>(descriptor_snapshot_batch_lease.begin),
                static_cast<unsigned long long>(descriptor_snapshot_batch_lease.end),
                static_cast<unsigned long long>(current_tick));
        }
    }
    ASSERT(stream_offset <= std::numeric_limits<u32>::max());
    return {snapshot_buffer, static_cast<u32>(stream_offset)};
#endif
}

std::pair<Buffer*, u32> BufferCache::ObtainBufferForImage(
    VAddr gpu_addr, u32 size,
    Core::MemoryManager::SnapshotReadGuard* external_snapshot_guard) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    ExecutorFlagSuspectGpuDescriptor("ObtainBufferForImage", gpu_addr, size);
#endif
    const auto safe_obtain = [&] {
        auto result = ObtainBuffer(gpu_addr, size, false, false, {}, false,
                                   false, true, external_snapshot_guard);
        if (result.first == &GetBuffer(NULL_BUFFER_ID)) {
            result = {nullptr, 0};
        }
        return result;
    };
    const BufferId buffer_id = page_table[gpu_addr >> CACHING_PAGEBITS].buffer_id;
    if (buffer_id) {
        if (Buffer& buffer = slot_buffers[buffer_id]; buffer.IsInBounds(gpu_addr, size)) {
            if (SynchronizeBuffer(buffer, gpu_addr, size, false, false,
                                  false, true, external_snapshot_guard) ==
                BufferSyncResult::Failed) {
                return {nullptr, 0};
            }
            return {&buffer, buffer.Offset(gpu_addr)};
        }
    }
    if (IsRegionGpuModified(gpu_addr, size)) {
        return safe_obtain();
    }
    // Do not bypass the snapshot transaction for a first-use image buffer.
    // ObtainBuffer provides the same dependency resolve, alias watchers,
    // bounded retry and fail-closed behavior as every other guest buffer.
    return safe_obtain();
}

bool BufferCache::IsRegionRegistered(VAddr addr, size_t size) {
    return buffer_ranges.Intersects(addr, size);
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
void BufferCache::ExecutorProbeGpuBuffer(VAddr device_addr, u32 size, const char* role,
                                         u64 sequence) {
    if (device_addr == 0 || size == 0) {
        return;
    }

    constexpr u32 MaxProbeBytes = 64_KB;
    const u32 probe_size = std::min(size, MaxProbeBytes);
    const auto [source, source_offset] = ObtainBuffer(device_addr, probe_size, false);
    if (source == nullptr || !source->IsInBounds(device_addr, probe_size)) {
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_GPU_BUFFER_CONTENT] seq=%llu role=%s guest=0x%llx "
                            "bytes=%u status=unavailable",
                            static_cast<unsigned long long>(sequence), role,
                            static_cast<unsigned long long>(device_addr), probe_size);
        return;
    }

    const auto [download, download_offset] = download_buffer.Map(probe_size, 64);
    if (download == nullptr) {
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_GPU_BUFFER_CONTENT] seq=%llu role=%s guest=0x%llx "
                            "bytes=%u status=download_full",
                            static_cast<unsigned long long>(sequence), role,
                            static_cast<unsigned long long>(device_addr), probe_size);
        return;
    }
    download_buffer.Commit();

    scheduler.EndRendering(Vulkan::Scheduler::EndRenderingReason::BufferProbe);
    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::BufferMemoryBarrier2 pre_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .buffer = source->Handle(),
        .offset = source_offset,
        .size = probe_size,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_barrier,
    });
    const vk::BufferCopy copy = {
        .srcOffset = source_offset,
        .dstOffset = download_offset,
        .size = probe_size,
    };
    cmdbuf.copyBuffer(source->Handle(), download_buffer.Handle(), copy);
    const vk::BufferMemoryBarrier2 post_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .buffer = source->Handle(),
        .offset = source_offset,
        .size = probe_size,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post_barrier,
    });
    scheduler.Finish(Vulkan::Scheduler::ExecutorFinishReason::BufferProbe);

    const u64 digest = Common::FingerprintBytes(
        std::span<const std::uint8_t>{download, probe_size},
        Common::FingerprintDomain::BufferProbe);
    u32 nonzero = 0;
    u32 nan32 = 0;
    u32 inf32 = 0;
    float min_finite = std::numeric_limits<float>::infinity();
    float max_finite = -std::numeric_limits<float>::infinity();
    for (u32 i = 0; i < probe_size; ++i) {
        nonzero += download[i] != 0;
    }
    for (u32 i = 0; i + sizeof(u32) <= probe_size; i += sizeof(u32)) {
        u32 word{};
        std::memcpy(&word, download + i, sizeof(word));
        const float value = std::bit_cast<float>(word);
        nan32 += std::isnan(value);
        inf32 += std::isinf(value);
        if (std::isfinite(value)) {
            min_finite = std::min(min_finite, value);
            max_finite = std::max(max_finite, value);
        }
    }
    const auto word_at = [&](u32 index) {
        u32 value{};
        const u32 offset = index * sizeof(u32);
        if (offset + sizeof(u32) <= probe_size) {
            std::memcpy(&value, download + offset, sizeof(value));
        }
        return value;
    };
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_GPU_BUFFER_CONTENT] seq=%llu role=%s guest=0x%llx requested=%u bytes=%u "
        "digest=0x%llx nonzero=%u nan32=%u inf32=%u finiteMin=%g finiteMax=%g "
        "w0_15=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
        static_cast<unsigned long long>(sequence), role,
        static_cast<unsigned long long>(device_addr), size, probe_size,
        static_cast<unsigned long long>(digest), nonzero, nan32, inf32,
        std::isfinite(min_finite) ? min_finite : 0.0f,
        std::isfinite(max_finite) ? max_finite : 0.0f, word_at(0), word_at(1), word_at(2),
        word_at(3), word_at(4), word_at(5), word_at(6), word_at(7), word_at(8), word_at(9),
        word_at(10), word_at(11), word_at(12), word_at(13), word_at(14), word_at(15));
}

void BufferCache::ExecutorProbeBoundGpuBuffer(VAddr guest_addr, u32 size, vk::Buffer bound_buffer,
                                              u64 bound_offset, const char* role, u64 sequence,
                                              u32 slot) {
    if (guest_addr == 0 || size == 0 || bound_buffer == VK_NULL_HANDLE) {
        return;
    }

    constexpr u32 MaxProbeBytes = 512_KB;
    const u64 readable_size = memory->ClampRangeSize(guest_addr, size);
    const u32 probe_size = static_cast<u32>(std::min<u64>({size, MaxProbeBytes, readable_size}));
    if (probe_size == 0) {
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_BOUND_BUFFER_ORACLE] seq=%llu role=%s slot=%u "
                            "guest=0x%llx requested=%u status=guest_unreadable",
                            static_cast<unsigned long long>(sequence), role, slot,
                            static_cast<unsigned long long>(guest_addr), size);
        return;
    }

    std::vector<u8> guest(probe_size);
    memory->CopySparseMemory(guest_addr, guest.data(), probe_size);

    const auto [download, download_offset] = download_buffer.Map(probe_size, 64);
    if (download == nullptr) {
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_BOUND_BUFFER_ORACLE] seq=%llu role=%s slot=%u "
                            "guest=0x%llx bytes=%u status=download_full",
                            static_cast<unsigned long long>(sequence), role, slot,
                            static_cast<unsigned long long>(guest_addr), probe_size);
        return;
    }
    download_buffer.Commit();

    scheduler.EndRendering(Vulkan::Scheduler::EndRenderingReason::BufferProbe);
    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::BufferMemoryBarrier2 pre_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .buffer = bound_buffer,
        .offset = bound_offset,
        .size = probe_size,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_barrier,
    });
    cmdbuf.copyBuffer(bound_buffer, download_buffer.Handle(), vk::BufferCopy{
        .srcOffset = bound_offset,
        .dstOffset = download_offset,
        .size = probe_size,
    });
    const vk::BufferMemoryBarrier2 post_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .buffer = bound_buffer,
        .offset = bound_offset,
        .size = probe_size,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post_barrier,
    });
    scheduler.Finish(Vulkan::Scheduler::ExecutorFinishReason::BufferProbe);

    const auto digest = [](const u8* bytes, const u32 count) {
        return Common::FingerprintBytes(
            std::span<const std::uint8_t>{bytes, count},
            Common::FingerprintDomain::BufferProbe);
    };
    u32 first_diff = probe_size;
    u32 guest_nonzero = 0;
    u32 gpu_nonzero = 0;
    for (u32 i = 0; i < probe_size; ++i) {
        guest_nonzero += guest[i] != 0;
        gpu_nonzero += download[i] != 0;
        if (first_diff == probe_size && guest[i] != download[i]) {
            first_diff = i;
        }
    }
    const bool exact_signed_zero_vertex_payload =
        size == ExecutorSignedZeroPositionBytes &&
        probe_size == ExecutorSignedZeroPositionBytes &&
        role != nullptr && std::strcmp(role, "vertex_range") == 0 && slot == 0 &&
        first_diff == probe_size &&
        ExecutorIsSignedZeroPositionPayload(
            std::span<const u8>{guest.data(), guest.size()}) &&
        ExecutorIsSignedZeroPositionPayload(
            std::span<const u8>{download, probe_size});
    if (exact_signed_zero_vertex_payload &&
        ExecutorSignedZeroVbufWriteProvenanceRequested()) {
        bool expected = false;
        if (executor_signed_zero_vbuf_provenance_claimed.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            if (PageManager::TryArmDiagnosticWriteProvenance(
                    guest_addr, ExecutorSignedZeroPositionBytes)) {
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_SIGNED_ZERO_VBUF_PROVENANCE_SELECT] seq=%llu "
                    "guest=0x%llx bytes=%u",
                    static_cast<unsigned long long>(sequence),
                    static_cast<unsigned long long>(guest_addr),
                    ExecutorSignedZeroPositionBytes);
            } else {
                executor_signed_zero_vbuf_provenance_claimed.store(
                    false, std::memory_order_release);
            }
        }
    }
    const auto word_at = [probe_size](const u8* bytes, const u32 index) {
        u32 value{};
        const u32 offset = index * sizeof(u32);
        if (offset + sizeof(u32) <= probe_size) {
            std::memcpy(&value, bytes + offset, sizeof(value));
        }
        return value;
    };
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_BOUND_BUFFER_ORACLE] seq=%llu role=%s slot=%u guest=0x%llx "
        "boundOffset=0x%llx requested=%u bytes=%u truncated=%u match=%u firstDiff=%u "
        "guestDigest=0x%llx gpuDigest=0x%llx guestNonzero=%u gpuNonzero=%u "
        "guestW0_7=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x "
        "gpuW0_7=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
        static_cast<unsigned long long>(sequence), role, slot,
        static_cast<unsigned long long>(guest_addr),
        static_cast<unsigned long long>(bound_offset), size, probe_size,
        probe_size != size ? 1u : 0u, first_diff == probe_size ? 1u : 0u, first_diff,
        static_cast<unsigned long long>(digest(guest.data(), probe_size)),
        static_cast<unsigned long long>(digest(download, probe_size)), guest_nonzero, gpu_nonzero,
        word_at(guest.data(), 0), word_at(guest.data(), 1), word_at(guest.data(), 2),
        word_at(guest.data(), 3), word_at(guest.data(), 4), word_at(guest.data(), 5),
        word_at(guest.data(), 6), word_at(guest.data(), 7), word_at(download, 0),
        word_at(download, 1), word_at(download, 2), word_at(download, 3), word_at(download, 4),
        word_at(download, 5), word_at(download, 6), word_at(download, 7));

    const bool is_shader_buffer = role != nullptr && std::strstr(role, "_buffer") != nullptr;
    const u32 dump_bytes = std::min(probe_size, is_shader_buffer ? u32{4_KB} : u32{512});
    const u32 dump_words = dump_bytes / sizeof(u32);
    for (u32 first_word = 0; first_word < dump_words; first_word += 8) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_BOUND_BUFFER_WORDS] seq=%llu role=%s slot=%u first=%u count=%u "
            "w=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
            static_cast<unsigned long long>(sequence), role, slot, first_word,
            std::min(8u, dump_words - first_word), word_at(guest.data(), first_word + 0),
            word_at(guest.data(), first_word + 1), word_at(guest.data(), first_word + 2),
            word_at(guest.data(), first_word + 3), word_at(guest.data(), first_word + 4),
            word_at(guest.data(), first_word + 5), word_at(guest.data(), first_word + 6),
            word_at(guest.data(), first_word + 7));
    }
}
#endif

BufferId BufferCache::FindBuffer(VAddr device_addr, u32 size,
                                 const bool transfer_range_checked) {
    if (device_addr == 0) {
        return NULL_BUFFER_ID;
    }
    if (!transfer_range_checked) {
        scheduler.FlushPendingBufferTransfersForRange(device_addr, size);
    }
    const u64 page = device_addr >> CACHING_PAGEBITS;
    const BufferId buffer_id = page_table[page].buffer_id;
    if (!buffer_id) {
        return CreateBuffer(device_addr, size);
    }
    const Buffer& buffer = slot_buffers[buffer_id];
    if (buffer.IsInBounds(device_addr, size)) {
        return buffer_id;
    }
    return CreateBuffer(device_addr, size);
}

BufferCache::OverlapResult BufferCache::ResolveOverlaps(VAddr device_addr, u32 wanted_size) {
    static constexpr int STREAM_LEAP_THRESHOLD = 16;
    boost::container::small_vector<BufferId, 16> overlap_ids;
    VAddr begin = device_addr;
    VAddr end = device_addr + wanted_size;
    int stream_score = 0;
    bool has_stream_leap = false;
    const auto expand_begin = [&](VAddr add_value) {
        static constexpr VAddr min_page = CACHING_PAGESIZE + DEVICE_PAGESIZE;
        if (add_value > begin - min_page) {
            begin = min_page;
            device_addr = DEVICE_PAGESIZE;
            return;
        }
        begin -= add_value;
        device_addr = begin - CACHING_PAGESIZE;
    };
    const auto expand_end = [&](VAddr add_value) {
        static constexpr VAddr max_page = 1ULL << MemoryTracker::MAX_CPU_PAGE_BITS;
        if (add_value > max_page - end) {
            end = max_page;
            return;
        }
        end += add_value;
    };
    if (begin == 0) {
        return OverlapResult{
            .ids = std::move(overlap_ids),
            .begin = begin,
            .end = end,
            .has_stream_leap = has_stream_leap,
        };
    }
    for (; device_addr >> CACHING_PAGEBITS < Common::DivCeil(end, CACHING_PAGESIZE);
         device_addr += CACHING_PAGESIZE) {
        const BufferId overlap_id = page_table[device_addr >> CACHING_PAGEBITS].buffer_id;
        if (!overlap_id) {
            continue;
        }
        Buffer& overlap = slot_buffers[overlap_id];
        if (overlap.is_picked) {
            continue;
        }
        overlap_ids.push_back(overlap_id);
        overlap.is_picked = true;
        const VAddr overlap_device_addr = overlap.CpuAddr();
        const bool expands_left = overlap_device_addr < begin;
        if (expands_left) {
            begin = overlap_device_addr;
        }
        const VAddr overlap_end = overlap_device_addr + overlap.SizeBytes();
        const bool expands_right = overlap_end > end;
        if (overlap_end > end) {
            end = overlap_end;
        }
        stream_score += overlap.StreamScore();
        if (stream_score > STREAM_LEAP_THRESHOLD && !has_stream_leap) {
            has_stream_leap = true;
            if (expands_right) {
                expand_end(CACHING_PAGESIZE * 128);
            }
            if (expands_left) {
                expand_begin(CACHING_PAGESIZE * 128);
            }
        }
    }
    return OverlapResult{
        .ids = std::move(overlap_ids),
        .begin = begin,
        .end = end,
        .has_stream_leap = has_stream_leap,
    };
}

void BufferCache::JoinOverlap(BufferId new_buffer_id, BufferId overlap_id,
                              bool accumulate_stream_score) {
    Buffer& new_buffer = slot_buffers[new_buffer_id];
    Buffer& overlap = slot_buffers[overlap_id];
    PortableGpu::InvalidateProducerRange(this,new_buffer.CpuAddr(),new_buffer.SizeBytes());
    if (accumulate_stream_score) {
        new_buffer.IncreaseStreamScore(overlap.StreamScore() + 1);
    }
    const size_t dst_base_offset = overlap.CpuAddr() - new_buffer.CpuAddr();
    const vk::BufferCopy copy = {
        .srcOffset = 0,
        .dstOffset = dst_base_offset,
        .size = overlap.SizeBytes(),
    };
    scheduler.EndRendering(Vulkan::Scheduler::EndRenderingReason::BufferCopy);
    const auto cmdbuf = scheduler.CommandBuffer();

    boost::container::static_vector<vk::BufferMemoryBarrier2, 2> pre_barriers{};
    if (auto src_barrier = overlap.GetBarrier(vk::AccessFlagBits2::eTransferRead,
                                              vk::PipelineStageFlagBits2::eTransfer)) {
        pre_barriers.push_back(*src_barrier);
    }
    if (auto dst_barrier =
            new_buffer.GetBarrier(vk::AccessFlagBits2::eTransferWrite,
                                  vk::PipelineStageFlagBits2::eTransfer, dst_base_offset)) {
        pre_barriers.push_back(*dst_barrier);
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = static_cast<u32>(pre_barriers.size()),
        .pBufferMemoryBarriers = pre_barriers.data(),
    });

    cmdbuf.copyBuffer(overlap.Handle(), new_buffer.Handle(), copy);

    boost::container::static_vector<vk::BufferMemoryBarrier2, 2> post_barriers{};
    if (auto src_barrier =
            overlap.GetBarrier(vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                               vk::PipelineStageFlagBits2::eAllCommands)) {
        post_barriers.push_back(*src_barrier);
    }
    if (auto dst_barrier = new_buffer.GetBarrier(
            vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            vk::PipelineStageFlagBits2::eAllCommands, dst_base_offset)) {
        post_barriers.push_back(*dst_barrier);
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = static_cast<u32>(post_barriers.size()),
        .pBufferMemoryBarriers = post_barriers.data(),
    });
    DeleteBuffer(overlap_id);
}

BufferId BufferCache::CreateBuffer(VAddr device_addr, u32 wanted_size) {
    PortableGpu::InvalidateProducerRange(this,device_addr,wanted_size);
    // Creating a wider cache buffer can absorb and retire buffers outside the
    // requested range. Materialize queued copies before changing that topology.
    scheduler.FlushPendingBufferTransfers();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
#if defined(LSX4_APPLE_RUNTIME)
    const bool exact_a750_retirement_drain =
        Vulkan::ExactDirtyUploadRetirementWatermark(
            instance.GetCapabilityProfile().memory_policy) != 0;
#else
    const bool exact_a750_retirement_drain =
        ExecutorUsesExactA750TurnipBufferRetirementDrain(instance);
#endif
    const auto drain_retired_generations = [&](const char* phase) {
        // Bloodborne creates a dense sequence of progressively wider cache
        // buffers while entering gameplay.  Each merge correctly defers the
        // old VkBuffer until its copy completes, but without a submit boundary
        // several GiB of superseded generations coexist and HyperOS LMKD kills
        // the foreground process.  Bound only that transient set.  The wait is
        // outside dynamic rendering (JoinOverlap ended it before recording a
        // copy), and the exact A750/Turnip D9 policy keeps A840 and every other
        // renderer route unchanged.
#if defined(LSX4_APPLE_RUNTIME)
        const u64 RetiringDrainThreshold = Vulkan::ExactDirtyUploadRetirementWatermark(
            instance.GetCapabilityProfile().memory_policy);
#else
        constexpr u64 RetiringDrainThreshold = 192_MB;
#endif
        const u64 before = retiring_buffer_bytes.load(std::memory_order_acquire);
        if (!exact_a750_retirement_drain || before < RetiringDrainThreshold) {
            return;
        }
        const auto begin = std::chrono::steady_clock::now();
        const u64 pending_before = scheduler.PendingOperationCount();
        scheduler.Finish(
            Vulkan::Scheduler::ExecutorFinishReason::BufferCacheRetirement);
        scheduler.PopPendingOperations(true);
        const u64 after = retiring_buffer_bytes.load(std::memory_order_acquire);
        const u64 elapsed_ns = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - begin)
                .count());
        const u64 ordinal = executor_a750_buffer_retirement_drain_count.fetch_add(
                                1, std::memory_order_relaxed) +
                            1;
        __android_log_print(
            ANDROID_LOG_WARN, "LSX4Native",
#if defined(LSX4_APPLE_RUNTIME)
            "[APPLE_BUFFER_RETIREMENT_DRAIN] ordinal=%llu phase=%s "
#else
            "[EXECUTOR_A750_BUFFER_RETIREMENT_DRAIN] ordinal=%llu phase=%s "
#endif
            "retiringMiB=%llu->%llu residentMiB=%llu pending=%llu->%llu "
            "elapsedMs=%.3f",
            static_cast<unsigned long long>(ordinal), phase,
            static_cast<unsigned long long>(before / 1_MB),
            static_cast<unsigned long long>(after / 1_MB),
            static_cast<unsigned long long>(resident_buffer_bytes / 1_MB),
            static_cast<unsigned long long>(pending_before),
            static_cast<unsigned long long>(scheduler.PendingOperationCount()),
            static_cast<double>(elapsed_ns) / 1'000'000.0);
    };
    // A preceding merge may already have crossed the watermark.  Retire it
    // before allocating the next replacement so the watermark is a real peak
    // bound rather than merely post-fact telemetry.
    drain_retired_generations("pre-allocate");
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    // Turnip D9 needs cache buffers carrying shader device addresses to stay on
    // dedicated allocations bound at offset zero. A separate allocation for
    // every small PS4 resource, however, creates hundreds of KGSL objects and
    // exhausts Android graphics memory long before their useful bytes do. Map
    // small requests through a stable 1 MiB guest-address window instead. This
    // reduces object count without using the BDA suballocation path that is
    // known to corrupt A750 output. The exact driver/build and an explicit
    // marker keep the experiment isolated from every other route, including
    // the protected A840/8gen5 route.
    if (ExecutorUsesExactA750TurnipDedicatedBdaWindows(instance) && wanted_size != 0 &&
        wanted_size <= 1_MB) {
        constexpr VAddr WindowSize = 1_MB;
        constexpr VAddr GuestAddressLimit = 1ULL << MemoryTracker::MAX_CPU_PAGE_BITS;
        const VAddr requested_begin = device_addr;
        const u32 requested_bytes = wanted_size;
        const VAddr requested_end =
            requested_begin < GuestAddressLimit &&
                    wanted_size <= GuestAddressLimit - requested_begin
                ? requested_begin + static_cast<VAddr>(wanted_size)
                : GuestAddressLimit + 1;
        if (requested_begin != 0 && requested_end <= GuestAddressLimit) {
            const VAddr window_begin = Common::AlignDown(requested_begin, WindowSize);
            const VAddr window_end = Common::AlignUp(requested_end, WindowSize);
            if (window_begin != 0 && window_end <= GuestAddressLimit &&
                window_end > window_begin &&
                window_end - window_begin <= std::numeric_limits<u32>::max()) {
                device_addr = window_begin;
                wanted_size = static_cast<u32>(window_end - window_begin);
                const u64 ordinal = executor_dedicated_bda_window_count.fetch_add(
                                        1, std::memory_order_relaxed) +
                                    1;
                if (ExecutorShouldLogDedicatedBdaWindowOrdinal(ordinal)) {
                    __android_log_print(
                        ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_A750_D9_DEDICATED_BDA_WINDOW] ordinal=%llu "
                        "requestedBase=0x%llx requestedBytes=%u windowBase=0x%llx "
                        "windowBytes=%u",
                        static_cast<unsigned long long>(ordinal),
                        static_cast<unsigned long long>(requested_begin), requested_bytes,
                        static_cast<unsigned long long>(window_begin),
                        static_cast<u32>(window_end - window_begin));
                }
            }
        }
    }
#endif
    const VAddr device_addr_end = Common::AlignUp(device_addr + wanted_size, CACHING_PAGESIZE);
    device_addr = Common::AlignDown(device_addr, CACHING_PAGESIZE);
    wanted_size = static_cast<u32>(device_addr_end - device_addr);
    const OverlapResult overlap = ResolveOverlaps(device_addr, wanted_size);
    const u32 size = static_cast<u32>(overlap.end - overlap.begin);
    const vk::BufferUsageFlags usage =
        instance.IsBufferDeviceAddressSupported()
            ? AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress
            : AllFlags;
    const BufferId new_buffer_id = slot_buffers.insert(
        instance, scheduler, MemoryUsage::DeviceLocal, overlap.begin, usage, size);
    auto& new_buffer = slot_buffers[new_buffer_id];
    for (const BufferId overlap_id : overlap.ids) {
        JoinOverlap(new_buffer_id, overlap_id, !overlap.has_stream_leap);
    }
    Register(new_buffer_id);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    // Also close a single unusually large merge immediately; otherwise the
    // burst might end before another CreateBuffer reaches the pre-allocation
    // check and leave its old generations resident until a much later frame.
    drain_retired_generations("post-merge");
#endif
    return new_buffer_id;
}

void BufferCache::ProcessFaultBuffer() {
    fault_manager.ProcessFaultBuffer();
}

void BufferCache::Register(BufferId buffer_id) {
    ChangeRegister<true>(buffer_id);
}

void BufferCache::Unregister(BufferId buffer_id) {
    auto& retiring = slot_buffers[buffer_id];
    PortableGpu::InvalidateProducerRange(this,retiring.CpuAddr(),retiring.SizeBytes());
    ChangeRegister<false>(buffer_id);
}

template <bool insert>
void BufferCache::ChangeRegister(BufferId buffer_id) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    RecordDmaCleanEpochTopologyChange();
#endif
    Buffer& buffer = slot_buffers[buffer_id];
    const auto size = buffer.SizeBytes();
    const VAddr device_addr_begin = buffer.CpuAddr();
    const VAddr device_addr_end = device_addr_begin + size;
    const u64 page_begin = device_addr_begin / CACHING_PAGESIZE;
    const u64 page_end = Common::DivCeil(device_addr_end, CACHING_PAGESIZE);
    const u64 size_pages = page_end - page_begin;
    for (u64 page = page_begin; page != page_end; ++page) {
        if constexpr (insert) {
            page_table[page].buffer_id = buffer_id;
        } else {
            page_table[page].buffer_id = BufferId{};
        }
    }
    if constexpr (insert) {
        resident_buffer_bytes += Common::AlignUp(size, CACHING_PAGESIZE);
        buffer.SetLRUId(lru_cache.Insert(buffer_id, gc_tick));
        if (Config::directMemoryAccess() && instance.IsBufferDeviceAddressSupported()) {
            boost::container::small_vector<vk::DeviceAddress, 128> bda_addrs;
            bda_addrs.reserve(size_pages);
            for (u64 i = 0; i < size_pages; ++i) {
                vk::DeviceAddress addr = buffer.BufferDeviceAddress() + (i << CACHING_PAGEBITS);
                bda_addrs.push_back(addr);
            }
            WriteDataBuffer(bda_pagetable_buffer, page_begin * sizeof(vk::DeviceAddress),
                            bda_addrs.data(), bda_addrs.size() * sizeof(vk::DeviceAddress));
        }
        buffer_ranges.Add(buffer.CpuAddr(), buffer.SizeBytes(), buffer_id);
    } else {
        const u64 resident_size = Common::AlignUp(size, CACHING_PAGESIZE);
        ASSERT(resident_buffer_bytes >= resident_size);
        resident_buffer_bytes -= resident_size;
        lru_cache.Free(buffer.LRUId());
        if (Config::directMemoryAccess() && instance.IsBufferDeviceAddressSupported()) {
            const u64 offset = bda_pagetable_buffer.Offset(page_begin * sizeof(vk::DeviceAddress));
            bda_pagetable_buffer.Fill(offset, size_pages * sizeof(vk::DeviceAddress), 0);
        }
        buffer_ranges.Subtract(buffer.CpuAddr(), buffer.SizeBytes());
    }
}

bool BufferCache::ResolveSnapshotDependencies(
    const VAddr device_addr, const u64 size,
    Core::MemoryManager::SnapshotReadGuard* external_snapshot_guard) {
    // This flag is monotonic. While it is false, no concurrently mapped
    // Direct physical GPU sibling VA can publish newer bytes for the
    // requested Vulkan buffer, so production guest memory has no dependency
    // graph to build. Replay ranges deliberately retain the full proof path:
    // a replay session can reuse a clean VA without publishing a VMA change.
    // Ordinary synchronization still validates and uploads dirty primaries.
    if (!memory->PhysicalAliasingMayExist() &&
        !Libraries::GnmDriver::ExecutorReplayActive()) {
        return true;
    }
    static constexpr u32 MaxResolveAttempts = 4;
    for (u32 attempt = 0; attempt < MaxResolveAttempts; ++attempt) {
        Core::MemoryManager::SnapshotWritePlan snapshot_plan;
        std::optional<Core::MemoryManager::SnapshotReadGuard> owned_guard;
        auto* memory_guard = external_snapshot_guard;
        if (memory_guard == nullptr) {
            owned_guard.emplace(memory->AcquireSnapshotReadGuard());
            memory_guard = &*owned_guard;
        }
        const auto sibling_plan_result = memory->BuildSnapshotSiblingPlan(
            *memory_guard, device_addr, size, snapshot_plan);
        if (sibling_plan_result ==
            Core::MemoryManager::SnapshotSiblingPlanResult::Failed) {
            return false;
        }
        if (sibling_plan_result ==
            Core::MemoryManager::SnapshotSiblingPlanResult::AliasFree) {
            return true;
        }

        // BuildSnapshotWriteRanges includes the requested VA itself alongside
        // true sibling aliases of the same physical span.  The current Vulkan
        // buffer is already authoritative for an ordinary GPU-to-GPU reuse;
        // downloading it as its own dependency turns every draw into a
        // synchronous GPU -> CPU -> GPU round trip.  Only sibling VA ranges
        // participate in dependency publication.  Keep the complete alias set
        // below so a real sibling publication still invalidates every copy.
        const auto dependency_alias_ranges =
            snapshot_plan.SiblingAliasRanges();

        bool backing_was_published = false;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        for (const auto& [range_addr, range_size] : dependency_alias_ranges) {
            backing_was_published |= WaitForPendingReadback(range_addr, range_size);
        }
#endif

        std::optional<std::pair<VAddr, u64>> gpu_dirty_range;
        for (const auto& [range_addr, range_size] : dependency_alias_ranges) {
            if (memory_tracker->IsRegionGpuModified(range_addr, range_size) ||
                gpu_modified_ranges.Intersects(range_addr, range_size)) {
                // Two distinct dirty Vulkan buffers that alias one physical
                // span need command-order metadata to choose an authority.
                // Never publish them in arbitrary virtual-address order.
                if (gpu_dirty_range) {
                    return false;
                }
                gpu_dirty_range = std::pair{range_addr, range_size};
            }
        }
        const bool resolved_gpu_data = gpu_dirty_range.has_value();
        if (gpu_dirty_range) {
            // The alias plan pins topology for this entire attempt. Demand
            // ReadMemory would discard that lease and attempt an exclusive
            // backing write while we still own its shared lock. Publish the
            // exact sibling range using the existing snapshot-owned route.
            if (!ResolveSnapshotGpuRange(gpu_dirty_range->first,
                                         gpu_dirty_range->second, memory_guard)) {
                return false;
            }
        }
        if (backing_was_published || resolved_gpu_data) {
            // A physical alias can have a distinct Vulkan buffer. Publishing
            // one alias updates backing memory, not those sibling buffers, so
            // every alias remains stale until it is uploaded independently.
            memory_tracker->MarkSnapshotRangesStale(
                snapshot_plan.AliasRanges());
        }
        const bool gpu_data_remains = std::ranges::any_of(
            dependency_alias_ranges, [&](const auto& range) {
                return memory_tracker->IsRegionGpuModified(range.first, range.second) ||
                       gpu_modified_ranges.Intersects(range.first, range.second);
            });
        if (!resolved_gpu_data && !gpu_data_remains) {
            return true;
        }
    }
    return false;
}

bool BufferCache::ResolveSnapshotGpuRange(
    const VAddr device_addr, const u64 size,
    const Core::MemoryManager::SnapshotReadGuard* snapshot_guard) {
    if (size == 0 || size > std::numeric_limits<u32>::max() ||
        device_addr > std::numeric_limits<VAddr>::max() - size ||
        !IsRegionRegistered(device_addr, size)) {
        return false;
    }
    scheduler.FlushPendingBufferTransfersForRange(device_addr, size);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    (void)WaitForPendingReadback(device_addr, size);
#endif
    bool resolved = false;
    liverpool->SendCommand<true>([this, device_addr, size, &resolved, snapshot_guard] {
        Buffer& source = slot_buffers[FindBuffer(
            device_addr, static_cast<u32>(size), true)];
        if (!source.IsInBounds(device_addr, size)) {
            return;
        }
        // Snapshot ownership resolution must never inherit demand-prefetch
        // windows: an adjacent page can have a newer host authority.  The
        // direct path copies only the page ranges proven GPU-authoritative by
        // the ownership classifier below.
        DownloadBufferMemory<false>(source, device_addr, size, false, snapshot_guard);
        resolved = !memory_tracker->IsRegionGpuModified(device_addr, size) &&
                   !gpu_modified_ranges.Intersects(device_addr, size);
    });
    return resolved;
}

BufferCache::BufferSyncResult BufferCache::SynchronizeBuffer(
    Buffer& buffer, VAddr device_addr, u32 size, bool is_written,
    bool is_texel_buffer, bool stable_read_only_candidate,
    bool fex_texture_cache_lock_held,
    Core::MemoryManager::SnapshotReadGuard* external_snapshot_guard,
    const u32 compute_diagnostic_id, const u32 compute_diagnostic_range,
    const u32 compute_diagnostic_buffer) {
#if defined(LSX4_APPLE_RUNTIME)
    static thread_local bool bounded_chunk_child = false;
    static thread_local SnapshotTransactionResult bounded_child_failure = SnapshotTransactionResult::Invalid;
    const u64 chunk_limit = Vulkan::ExactDirtyUploadChunkLimit(
        instance.GetCapabilityProfile().memory_policy);
    if (chunk_limit != 0 && size > chunk_limit && !is_texel_buffer &&
        !fex_texture_cache_lock_held &&
        device_addr <= std::numeric_limits<VAddr>::max() - size) {
        const bool previous_chunk_scope = bounded_chunk_child;
        bounded_chunk_child = true;
        SCOPE_EXIT { bounded_chunk_child = previous_chunk_scope; };
        // Each child uses the original immutable snapshot transaction. A failed
        // child fails the whole request; no draw can consume a partial upload.
        // Finish only after that child has released its snapshot/capture locks.
        static std::atomic<u64> diagnostic_upload_count{};
        const u64 diagnostic_ordinal = diagnostic_upload_count.fetch_add(1, std::memory_order_relaxed) + 1;
        const bool report_upload = diagnostic_ordinal <= 8 || (diagnostic_ordinal & (diagnostic_ordinal - 1)) == 0;
        if (report_upload) std::fprintf(stderr, "[APPLE_BOUNDED_BUFFER_UPLOAD] phase=begin address=0x%llx bytes=%u chunk=%llu write=%d\n",
            (unsigned long long)device_addr, size, (unsigned long long)chunk_limit,
            is_written ? 1 : 0);
        Lsx4::Portable::RangeTransferStats stats;
        const bool complete = Lsx4::Portable::TransferAdaptiveRange(
            size, chunk_limit, 16 * 1024,
            [&](const u64 offset, const u64 bytes) {
                bounded_child_failure = SnapshotTransactionResult::Invalid;
                const auto result = SynchronizeBuffer(buffer, device_addr + offset,
                    static_cast<u32>(bytes), is_written, false,
                    stable_read_only_candidate, false, external_snapshot_guard,
                    compute_diagnostic_id, compute_diagnostic_range,
                    compute_diagnostic_buffer);
                if (result != BufferSyncResult::Failed)
                    return Lsx4::Portable::RangeTransferResult::Ready;
                // The child returned after releasing its transaction leases.
                // Retry only verified contention, retaining all ownership and
                // dirty-page rules of the original snapshot transaction.
                const bool retryable =
                    bounded_child_failure == SnapshotTransactionResult::Busy ||
                    bounded_child_failure == SnapshotTransactionResult::Overlap;
                return retryable ? Lsx4::Portable::RangeTransferResult::Retryable
                                 : Lsx4::Portable::RangeTransferResult::Failed;
            }, stats);
        if (stats.refinements != 0 || !complete) {
            std::fprintf(stderr,
                "[APPLE_ADAPTIVE_BUFFER_UPLOAD] ready=%d bytes=%u committed=%llu attempts=%llu refinements=%llu\n",
                complete ? 1 : 0, size, (unsigned long long)stats.committedBytes,
                (unsigned long long)stats.attempts,
                (unsigned long long)stats.refinements);
        }
        if (!complete) return BufferSyncResult::Failed;
        if (report_upload) std::fprintf(stderr, "[APPLE_BOUNDED_BUFFER_UPLOAD] phase=complete bytes=%u\n", size);
        return BufferSyncResult::Ready;
    }
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool diagnose_compute_dma = compute_diagnostic_id != 0 &&
        compute_diagnostic_id <= 256 && compute_diagnostic_range == 65 &&
        ExecutorComputeDmaSyncDiagnosticEnabled();
    const auto log_compute_dma_phase = [&](const char* phase,
                                           const u32 step = 0) {
        if (!diagnose_compute_dma) {
            return;
        }
        __android_log_print(
            ANDROID_LOG_WARN, "LSX4Native",
            "[EXECUTOR_COMPUTE_DMA_SYNC_PHASE] id=%u range=%u buffer=%u "
            "phase=%s step=%u addr=0x%llx size=%u bufferAddr=0x%llx "
            "bufferSize=%llu",
            compute_diagnostic_id, compute_diagnostic_range,
            compute_diagnostic_buffer, phase, step,
            static_cast<unsigned long long>(device_addr), size,
            static_cast<unsigned long long>(buffer.CpuAddr()),
            static_cast<unsigned long long>(buffer.SizeBytes()));
    };
    log_compute_dma_phase("sync-entry");
#else
    (void)compute_diagnostic_id;
    (void)compute_diagnostic_range;
    (void)compute_diagnostic_buffer;
#endif
    const auto fail = [&](const char* reason,
                          const SnapshotTransactionResult transaction =
                              SnapshotTransactionResult::Invalid) {
#if defined(LSX4_APPLE_RUNTIME)
        bounded_child_failure = transaction;
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        static std::atomic<u64> ordinal{0};
        const u64 current = ordinal.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ExecutorShouldLogBufferReadbackOrdinal(current)) {
            __android_log_print(
                ANDROID_LOG_ERROR, "LSX4Native",
                "[EXECUTOR_BUFFER_SYNC_FAILURE] ordinal=%llu reason=%s "
                "addr=0x%llx size=%u write=%d texel=%d transaction=%u",
                static_cast<unsigned long long>(current), reason,
                static_cast<unsigned long long>(device_addr), size,
                is_written ? 1 : 0, is_texel_buffer ? 1 : 0,
                static_cast<unsigned>(transaction));
        }
#else
        (void)reason;
        (void)transaction;
#endif
        return BufferSyncResult::Failed;
    };
    bool resolve_snapshot_dependencies = true;
    bool physical_alias_snapshot = true;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    resolve_snapshot_dependencies = ExecutorSnapshotDependencyResolutionEnabled();
    physical_alias_snapshot = ExecutorPhysicalAliasSnapshotEnabled();
#endif
    if (device_addr > std::numeric_limits<VAddr>::max() - size) {
        return fail("dependency-or-overflow");
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_compute_dma_phase("before-snapshot-dependencies");
#endif
    if (resolve_snapshot_dependencies &&
        !ResolveSnapshotDependencies(device_addr, size,
                                     external_snapshot_guard)) {
        return fail("dependency-or-overflow");
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_compute_dma_phase("after-snapshot-dependencies");
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    using SnapshotReadGuard =
        decltype(memory->AcquireSnapshotReadGuard());
    std::optional<SnapshotReadGuard> fex_snapshot_guard;
    bool fex_buffer_capture = false;
    bool fex_snapshot_boundary = false;
    const bool fex_capture_hazard = !fex_texture_cache_lock_held &&
        !Libraries::GnmDriver::ExecutorReplayActive() &&
        PageManager::MayHaveFexDeferredBufferCapture(device_addr, size);
    log_compute_dma_phase(fex_capture_hazard ? "fex-capture-hazard" :
                                               "no-fex-capture-hazard");
    if (fex_capture_hazard) {
        if (external_snapshot_guard == nullptr) {
            log_compute_dma_phase("before-fex-snapshot-guard");
            fex_snapshot_guard.emplace(memory->AcquireSnapshotReadGuard());
            log_compute_dma_phase("after-fex-snapshot-guard");
        }
        if (!memory->PhysicalAliasingMayExist()) {
            fex_buffer_capture =
                PageManager::BeginFexDeferredBufferCapture(
                    device_addr, size, fex_texture_cache_lock_held);
        }
        if (!fex_buffer_capture) {
            fex_snapshot_boundary =
                PageManager::BeginFexBufferSnapshotBoundary(device_addr,
                                                            size);
        }
        if (!fex_buffer_capture && !fex_snapshot_boundary) {
            fex_snapshot_guard.reset();
            return fail("fex-capture-boundary-unavailable");
        }
        log_compute_dma_phase("after-fex-capture-boundary");
    }
    const auto release_fex_buffer_capture = [&] {
        if (fex_buffer_capture) {
            PageManager::EndFexDeferredBufferCapture(
                true, fex_texture_cache_lock_held);
            fex_buffer_capture = false;
        }
        if (fex_snapshot_boundary) {
            PageManager::EndFexBufferSnapshotBoundary(true);
            fex_snapshot_boundary = false;
        }
        fex_snapshot_guard.reset();
    };
    SCOPE_EXIT {
        release_fex_buffer_capture();
    };
    if (fex_buffer_capture) {
        for (const VAddr page :
             PageManager::CurrentFexDeferredBufferCaptureDirtyPages()) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            RecordDmaCleanEpochCpuPublication();
#endif
            memory_tracker->MarkRegionAsCpuModified(
                page, TRACKER_BYTES_PER_PAGE);
        }
    }
#endif
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes = 0;
    VAddr buffer_start = buffer.CpuAddr();
    vk::Buffer src_buffer = VK_NULL_HANDLE;
    std::unique_ptr<Buffer> transient_upload_buffer;
    const bool source_upload_needed = physical_alias_snapshot
        ? memory_tracker->IsRegionCpuModifiedOrSnapshotStale(device_addr, size)
        : memory_tracker->IsRegionCpuModified(device_addr, size);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_compute_dma_phase(source_upload_needed ? "source-upload-needed" :
                                                 "source-upload-clean");
#endif
    bool alias_upload_needed = false;
    if (physical_alias_snapshot && !source_upload_needed &&
        memory->PhysicalAliasingMayExist()) {
        Core::MemoryManager::SnapshotWritePlan initial_snapshot_plan;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        std::optional<SnapshotReadGuard> initial_guard;
        auto* memory_guard = external_snapshot_guard;
        if (memory_guard == nullptr) {
            log_compute_dma_phase("before-initial-snapshot-guard");
            initial_guard.emplace(memory->AcquireSnapshotReadGuard());
            log_compute_dma_phase("after-initial-snapshot-guard");
            memory_guard = &*initial_guard;
        }
        log_compute_dma_phase("before-initial-sibling-plan");
        const auto sibling_plan_result = memory->BuildSnapshotSiblingPlan(
            *memory_guard, device_addr, size, initial_snapshot_plan);
        log_compute_dma_phase("after-initial-sibling-plan",
                              static_cast<u32>(sibling_plan_result));
        if (sibling_plan_result ==
            Core::MemoryManager::SnapshotSiblingPlanResult::Failed) {
#else
        auto memory_guard = memory->AcquireSnapshotReadGuard();
        const auto sibling_plan_result = memory->BuildSnapshotSiblingPlan(
            memory_guard, device_addr, size, initial_snapshot_plan);
        if (sibling_plan_result ==
            Core::MemoryManager::SnapshotSiblingPlanResult::Failed) {
#endif
            return fail("initial-snapshot-ranges");
        }
        if (sibling_plan_result ==
            Core::MemoryManager::SnapshotSiblingPlanResult::HasSiblingAliases) {
            alias_upload_needed = memory_tracker->AreSnapshotRangesDirty(
                initial_snapshot_plan.SiblingAliasRanges());
        }
    }
    const bool upload_needed = source_upload_needed || alias_upload_needed;
    const VAddr upload_page_begin =
        Common::AlignDown(device_addr, TRACKER_BYTES_PER_PAGE);
    const VAddr upload_page_end =
        Common::AlignUp(device_addr + size, TRACKER_BYTES_PER_PAGE);
    const u64 upload_capacity = upload_page_end - upload_page_begin;
    u8* upload_data = nullptr;
    u64 upload_base_offset = 0;
    u64 reserved_upload_capacity = 0;
    vk::Buffer reserved_upload_buffer = VK_NULL_HANDLE;
    bool staging_reserved = false;
    const auto reserve_upload_storage = [&](const u64 required_capacity) {
        if (required_capacity == 0 || required_capacity > upload_capacity) {
            return false;
        }
        if (upload_data != nullptr && required_capacity <= reserved_upload_capacity) {
            return true;
        }
        // An exact sparse plan can grow only if ownership changed after its
        // read-only measurement. StreamBuffer::Map has not been committed yet,
        // so remapping the same provisional ring position with the larger size
        // safely replaces that reservation.
        if (transient_upload_buffer != nullptr) {
            return false;
        }
        const auto [staging, offset] = staging_buffer.Map(
            required_capacity, 0,
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            !fex_buffer_capture && !fex_snapshot_boundary
#else
            true
#endif
        );
        if (staging != nullptr) {
            upload_data = staging;
            upload_base_offset = offset;
            reserved_upload_buffer = staging_buffer.Handle();
            staging_reserved = true;
        } else {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            if (fex_buffer_capture || fex_snapshot_boundary) {
                return false;
            }
#endif
            transient_upload_buffer =
                std::make_unique<Buffer>(std::nothrow, instance, scheduler,
                                         MemoryUsage::Upload, 0,
                                         vk::BufferUsageFlagBits::eTransferSrc,
                                         required_capacity);
            if (!transient_upload_buffer->IsValid() ||
                transient_upload_buffer->mapped_data.size() < required_capacity) {
                transient_upload_buffer.reset();
                return false;
            }
            upload_data = transient_upload_buffer->mapped_data.data();
            reserved_upload_buffer = transient_upload_buffer->Handle();
        }
        reserved_upload_capacity = required_capacity;
        return upload_data != nullptr && reserved_upload_buffer != VK_NULL_HANDLE;
    };
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    // Full-span reservation on exact proprietary A750 dropped Bloodborne to
    // 4-6 FPS and amplified transient floor/wall loss in a moving 16-frame
    // control. The compatibility router may also opt an exact measured ICD
    // build into the same immutable dirty-range transaction. Every unmeasured
    // A840, Turnip, Mali, and unknown route retains the full-span default.
    const auto& upload_policy =
        instance.GetCapabilityProfile().memory_policy;
    const bool exact_dirty_upload_reservation =
        upload_policy.upload_reservation ==
            Vulkan::UploadReservationMode::ExactDirtyRanges &&
        (upload_policy.upload_reservation_scope ==
             Vulkan::UploadReservationScope::AllCpuDirtyUploads ||
         fex_buffer_capture || fex_snapshot_boundary);
#else
    constexpr bool exact_dirty_upload_reservation = false;
#endif
    if (upload_needed && !exact_dirty_upload_reservation &&
        !reserve_upload_storage(upload_capacity)) {
        return fail("upload-reserve");
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_compute_dma_phase(upload_needed ? "after-upload-reserve" :
                                          "no-upload-reserve");
#endif

    const bool ownership_transaction_needed =
        upload_needed ||
        (is_written &&
         !memory_tracker->IsRegionGpuModified(device_addr, size));
    SnapshotTransactionResult transaction_result =
        ownership_transaction_needed ? SnapshotTransactionResult::Invalid
                                     : SnapshotTransactionResult::NoUpload;
    u64 stable_promotions = 0;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    stable_canonical_requests += stable_read_only_candidate;
    u64 sparse_upload_measurements = 0;
    u64 sparse_upload_retries = 0;
#endif
    const char* transaction_failure_reason = "snapshot-transaction";
    std::vector<std::pair<VAddr, u64>> transaction_alias_ranges;
    static constexpr u32 MaxSnapshotTransactionAttempts = 64;
    for (u32 attempt = 0; attempt < MaxSnapshotTransactionAttempts; ++attempt) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        log_compute_dma_phase("transaction-attempt", attempt);
#endif
        if (!ownership_transaction_needed) {
            break;
        }
        copies.clear();
        total_size_bytes = 0;
        bool source_copy_succeeded = upload_data != nullptr;
        bool sparse_upload_plan_changed = false;
        bool upload_range_overflow = false;
        {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            std::optional<SnapshotReadGuard> attempt_guard;
            auto* active_guard = external_snapshot_guard;
            if (active_guard == nullptr && fex_snapshot_guard) {
                active_guard = &*fex_snapshot_guard;
            }
            if (active_guard == nullptr) {
                log_compute_dma_phase("before-attempt-snapshot-guard", attempt);
                attempt_guard.emplace(memory->AcquireSnapshotReadGuard());
                log_compute_dma_phase("after-attempt-snapshot-guard", attempt);
                active_guard = &*attempt_guard;
            }
            auto& memory_guard = *active_guard;
#else
            auto memory_guard = memory->AcquireSnapshotReadGuard();
#endif
            Core::MemoryManager::SnapshotWritePlan snapshot_plan;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            log_compute_dma_phase("before-snapshot-write-plan", attempt);
#endif
            if (physical_alias_snapshot &&
                !memory->BuildSnapshotWritePlan(
                    memory_guard, device_addr, size, snapshot_plan)) {
                transaction_result = SnapshotTransactionResult::Invalid;
            } else {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
                log_compute_dma_phase("after-snapshot-write-plan", attempt);
#endif
                const auto snapshot_write_ranges =
                    snapshot_plan.WriteRanges();
                const auto snapshot_alias_ranges =
                    snapshot_plan.SiblingAliasRanges();
                transaction_alias_ranges.assign(snapshot_alias_ranges.begin(),
                                                snapshot_alias_ranges.end());
                bool execute_upload_transaction = true;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
                if (exact_dirty_upload_reservation) {
                    ++sparse_upload_measurements;
                    u64 measured_upload_bytes = 0;
                    bool measurement_overflow = false;
                    const auto measurement_result =
                        memory_tracker->MeasureUploadRanges(
                            device_addr, size, snapshot_alias_ranges,
                            [&](const u64, const u64 range_size) {
                                if (range_size >
                                    upload_capacity -
                                        std::min(measured_upload_bytes,
                                                 upload_capacity)) {
                                    measurement_overflow = true;
                                    return;
                                }
                                measured_upload_bytes += range_size;
                            });
                    if (measurement_overflow) {
                        transaction_failure_reason =
                            "sparse-upload-measure-overflow";
                        transaction_result = SnapshotTransactionResult::Invalid;
                        execute_upload_transaction = false;
                    } else if (measurement_result ==
                               SnapshotTransactionResult::GpuDirty) {
                        transaction_result = measurement_result;
                        execute_upload_transaction = false;
                    } else if (measurement_result !=
                                   SnapshotTransactionResult::Complete &&
                               measurement_result !=
                                   SnapshotTransactionResult::NoUpload) {
                        transaction_failure_reason =
                            "sparse-upload-measure-invalid";
                        transaction_result = measurement_result;
                        execute_upload_transaction = false;
                    } else if (measurement_result ==
                                   SnapshotTransactionResult::Complete &&
                               (measured_upload_bytes == 0 ||
                                !reserve_upload_storage(
                                    measured_upload_bytes))) {
                        transaction_failure_reason =
                            "upload-reserve-sparse-fex";
                        transaction_result = SnapshotTransactionResult::CopyFailed;
                        execute_upload_transaction = false;
                    }
                }
#endif
                if (execute_upload_transaction) {
                    source_copy_succeeded = upload_data != nullptr;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
                    log_compute_dma_phase("before-upload-ranges", attempt);
#endif
                    transaction_result = memory_tracker->ForEachUploadRange(
                        device_addr, size, is_written,
                        stable_read_only_candidate, &stable_promotions,
                        snapshot_write_ranges, snapshot_alias_ranges,
                        [&](u64 device_addr_out, u64 range_size) {
                            if (range_size >
                                upload_capacity -
                                    std::min<u64>(total_size_bytes,
                                                  upload_capacity)) {
                                upload_range_overflow = true;
                                source_copy_succeeded = false;
                                return;
                            }
                            const u64 copy_offset = total_size_bytes;
                            total_size_bytes += range_size;
                            if (upload_data == nullptr ||
                                total_size_bytes > reserved_upload_capacity) {
                                sparse_upload_plan_changed =
                                    exact_dirty_upload_reservation;
                                source_copy_succeeded = false;
                                return;
                            }
                            if (!source_copy_succeeded) {
                                return;
                            }
                            copies.emplace_back(
                                copy_offset, device_addr_out - buffer_start,
                                range_size);
                            if (!memory->TryCopyBackingMemory(
                                    memory_guard, device_addr_out,
                                    upload_data + copy_offset, range_size)) {
                                if (!memory->TryCopyReadableMemory(
                                        memory_guard, device_addr_out,
                                        upload_data + copy_offset, range_size,
                                        false)) {
                                    source_copy_succeeded = false;
                                    copies.pop_back();
                                    return;
                                }
                            }
                        },
                        [&] {
                            return source_copy_succeeded && !copies.empty();
                        });
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
                    log_compute_dma_phase("after-upload-ranges", attempt);
#endif
                }
            }
        }

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (transaction_result == SnapshotTransactionResult::CopyFailed &&
            exact_dirty_upload_reservation && sparse_upload_plan_changed &&
            !upload_range_overflow) {
            // The read-only plan raced a new dirty page. The failed commit
            // publishes no GPU ownership and retains the touched pages as
            // snapshot-stale, so remeasure and retry while the same FEX
            // capture remains closed.
            ++sparse_upload_retries;
            continue;
        }
#endif
        const auto action = DecideSnapshotTransactionAction(transaction_result);
        if (action == SnapshotTransactionAction::Complete) {
            break;
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        const bool ended_fex_capture =
            fex_buffer_capture || fex_snapshot_boundary;
        release_fex_buffer_capture();
        if (ended_fex_capture) {
            return fail("fex-capture-retry", transaction_result);
        }
#endif
        if (action == SnapshotTransactionAction::Retry) {
            std::this_thread::yield();
            if (attempt >= 8) {
                std::this_thread::sleep_for(std::chrono::microseconds{10});
            }
            continue;
        }
        if (action == SnapshotTransactionAction::ResolveGpu) {
            std::vector<std::pair<VAddr, u64>> snapshot_domains;
            snapshot_domains.reserve(transaction_alias_ranges.size() + 1);
            snapshot_domains.emplace_back(device_addr, size);
            snapshot_domains.insert(snapshot_domains.end(),
                                    transaction_alias_ranges.begin(),
                                    transaction_alias_ranges.end());

            std::optional<std::pair<VAddr, u64>> gpu_domain;
            for (const auto& range : snapshot_domains) {
                if (!memory_tracker->IsRegionGpuModified(range.first,
                                                         range.second) &&
                    !gpu_modified_ranges.Intersects(range.first,
                                                    range.second)) {
                    continue;
                }
                if (gpu_domain) {
                    transaction_failure_reason =
                        "snapshot-multiple-gpu-authorities";
                    transaction_result = SnapshotTransactionResult::Invalid;
                    break;
                }
                gpu_domain = range;
            }
            if (transaction_result == SnapshotTransactionResult::Invalid) {
                break;
            }
            if (!gpu_domain) {
                // A pending readback can retire between classification and
                // resolution. Rebuild the transaction from current state.
                transaction_result = SnapshotTransactionResult::Busy;
                continue;
            }

            std::vector<std::pair<VAddr, u64>> gpu_readback_ranges;
            std::vector<std::pair<VAddr, u64>> host_authority_ranges;
            bool conflict_unknown = false;
            const u64 classified_bytes =
                memory_tracker->ForEachSnapshotGpuConflictRange(
                    gpu_domain->first, gpu_domain->second,
                    [&](const VAddr range_addr, const u64 range_size,
                        const SnapshotGpuConflictDisposition disposition) {
                        switch (disposition) {
                        case SnapshotGpuConflictDisposition::ResolveGpu:
                            gpu_readback_ranges.emplace_back(range_addr,
                                                              range_size);
                            break;
                        case SnapshotGpuConflictDisposition::PreferHost:
                            host_authority_ranges.emplace_back(range_addr,
                                                                range_size);
                            break;
                        case SnapshotGpuConflictDisposition::CpuConflict:
                        case SnapshotGpuConflictDisposition::Unknown:
                            conflict_unknown = true;
                            break;
                        }
                    });
            if (classified_bytes == 0 || conflict_unknown) {
                transaction_failure_reason =
                    "snapshot-gpu-authority-unknown";
                transaction_result = SnapshotTransactionResult::Invalid;
                break;
            }
            // The exact VA ledger is a readback planning index, while the
            // MemoryTracker bits are the ownership authority. A deferred
            // publication can retire the index before a later alias query
            // observes the tracker bit. Once the classifier has selected
            // ResolveGpu, repair only that proven range and only when a
            // registered Vulkan buffer covers it. Unknown, host-newer and
            // unregistered ranges remain fail-closed.
            const bool ledger_reconciled = std::ranges::all_of(
                gpu_readback_ranges, [&](const auto& range) {
                    const bool valid_range =
                        range.second != 0 &&
                        range.second <= std::numeric_limits<VAddr>::max() &&
                        range.first <= std::numeric_limits<VAddr>::max() -
                                       range.second;
                    return valid_range &&
                           buffer_ranges.Contains(range.first, range.second);
                });
            if (!ledger_reconciled) {
                transaction_failure_reason =
                    "snapshot-gpu-range-ledger-mismatch";
                transaction_result = SnapshotTransactionResult::Invalid;
                break;
            }
            for (const auto& [range_addr, range_size] : gpu_readback_ranges) {
                if (!gpu_modified_ranges.Contains(range_addr, range_size)) {
                    gpu_modified_ranges.Add(range_addr, range_size);
                }
            }

            // Host-newer stale pages discard only their obsolete Vulkan
            // ownership. GPU-newer pages use exact readback without adjacent
            // prefetch. Mixed page dispositions are handled independently.
            for (const auto& [range_addr, range_size] :
                 host_authority_ranges) {
                memory_tracker->UnmarkRegionAsGpuModified(range_addr,
                                                          range_size);
                gpu_modified_ranges.Subtract(range_addr, range_size);
            }
            bool readback_succeeded = true;
            for (const auto& [range_addr, range_size] :
                 gpu_readback_ranges) {
                readback_succeeded &=
                    ResolveSnapshotGpuRange(range_addr, range_size, external_snapshot_guard);
                if (!readback_succeeded) {
                    break;
                }
            }
            if (!readback_succeeded) {
                transaction_failure_reason = "snapshot-gpu-readback-failed";
                transaction_result = SnapshotTransactionResult::Invalid;
                break;
            }
            if (!gpu_readback_ranges.empty()) {
                // Backing memory now carries the proven GPU authority. Every
                // sibling Vulkan buffer is stale, while the readback source
                // itself is already synchronized and must not be re-uploaded.
                memory_tracker->MarkSnapshotRangesStale(snapshot_domains);
                memory_tracker->ClearSnapshotRangesStale(
                    gpu_readback_ranges);
            }
            transaction_result = SnapshotTransactionResult::Busy;
            continue;
        }
        if (transaction_result == SnapshotTransactionResult::CopyFailed &&
            upload_data == nullptr && reserve_upload_storage(upload_capacity)) {
            continue;
        }
        break;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (exact_dirty_upload_reservation && upload_needed) {
        ++upload_reservation_transaction_count;
        upload_reservation_measurement_count += sparse_upload_measurements;
        upload_reservation_retry_count += sparse_upload_retries;
        if (transaction_result == SnapshotTransactionResult::Complete ||
            transaction_result == SnapshotTransactionResult::NoUpload) {
            ++upload_reservation_completed_transaction_count;
            const u64 committed_dirty_bytes =
                transaction_result == SnapshotTransactionResult::Complete
                    ? static_cast<u64>(total_size_bytes)
                    : 0;
            upload_reservation_full_span_candidate_bytes += upload_capacity;
            upload_reservation_dirty_bytes += committed_dirty_bytes;
            upload_reservation_avoided_bytes +=
                upload_capacity - std::min(upload_capacity, committed_dirty_bytes);
            upload_reservation_max_full_span_candidate_bytes =
                std::max(upload_reservation_max_full_span_candidate_bytes,
                         upload_capacity);
            upload_reservation_max_dirty_bytes =
                std::max(upload_reservation_max_dirty_bytes,
                         committed_dirty_bytes);
        } else {
            ++upload_reservation_failed_transaction_count;
        }
    }
#endif
    if (staging_reserved) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        log_compute_dma_phase("before-staging-commit");
#endif
        staging_buffer.Commit();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        log_compute_dma_phase("after-staging-commit");
#endif
    }
    const bool upload_succeeded =
        transaction_result == SnapshotTransactionResult::Complete;
    if (transaction_result != SnapshotTransactionResult::Complete &&
        transaction_result != SnapshotTransactionResult::NoUpload) {
        return fail(transaction_failure_reason, transaction_result);
    }
    if (upload_succeeded) {
        src_buffer = reserved_upload_buffer;
        for (auto& copy : copies) {
            copy.srcOffset += upload_base_offset;
        }
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    release_fex_buffer_capture();
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (stable_read_only_candidate && upload_succeeded) {
        ++stable_canonical_uploads;
        stable_canonical_bytes += total_size_bytes;
        stable_canonical_promotions += stable_promotions;
        if (stable_promotions != 0 &&
            (stable_canonical_promotions <= 8 ||
             (stable_canonical_promotions & (stable_canonical_promotions - 1)) == 0)) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_STABLE_CANONICAL_STREAM] requests=%llu uploads=%llu "
                "bytes=%llu promotedPages=%llu",
                static_cast<unsigned long long>(stable_canonical_requests),
                static_cast<unsigned long long>(stable_canonical_uploads),
                static_cast<unsigned long long>(stable_canonical_bytes),
                static_cast<unsigned long long>(stable_canonical_promotions));
        }
    }
#endif

    if (src_buffer) {
        ASSERT(!copies.empty());
        for (const auto& copy : copies) {
            PortableGpu::InvalidateProducerRange(this,buffer.CpuAddr()+copy.dstOffset,copy.size);
        }
        vk::DeviceSize upload_begin = copies.front().dstOffset;
        vk::DeviceSize upload_end = upload_begin + copies.front().size;
        for (const auto& copy : copies) {
            upload_begin = std::min(upload_begin, copy.dstOffset);
            upload_end = std::max(upload_end, copy.dstOffset + copy.size);
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (IsManagedVulkanTransferCoalescing()) {
            scheduler.QueueBufferUploadCopies(
                src_buffer, buffer.buffer, copies,
                buffer_start + upload_begin, upload_end - upload_begin);
            if (transient_upload_buffer) {
                scheduler.FlushPendingBufferTransfers();
            }
        } else
#endif
        {
            scheduler.EndRendering(Vulkan::Scheduler::EndRenderingReason::BufferUpload);
            const auto cmdbuf = scheduler.CommandBuffer();
            const vk::BufferMemoryBarrier2 pre_barrier = {
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask =
                    vk::AccessFlagBits2::eMemoryRead |
                    vk::AccessFlagBits2::eMemoryWrite |
                    vk::AccessFlagBits2::eTransferRead |
                    vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .buffer = buffer.Handle(),
                .offset = upload_begin,
                .size = upload_end - upload_begin,
            };
            const vk::BufferMemoryBarrier2 post_barrier = {
                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask =
                    vk::AccessFlagBits2::eMemoryRead |
                    vk::AccessFlagBits2::eMemoryWrite,
                .buffer = buffer.Handle(),
                .offset = upload_begin,
                .size = upload_end - upload_begin,
            };
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                .dependencyFlags = vk::DependencyFlagBits::eByRegion,
                .bufferMemoryBarrierCount = 1,
                .pBufferMemoryBarriers = &pre_barrier,
            });
            cmdbuf.copyBuffer(src_buffer, buffer.buffer, copies);
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                .dependencyFlags = vk::DependencyFlagBits::eByRegion,
                .bufferMemoryBarrierCount = 1,
                .pBufferMemoryBarriers = &post_barrier,
            });
        }
        if (transient_upload_buffer) {
            scheduler.DeferOperation(
                [upload = std::move(transient_upload_buffer)]() mutable {
                    upload.reset();
                });
        }
#if defined(LSX4_APPLE_RUNTIME)
        if (bounded_chunk_child) {
            // Only actual uploads need a consuming submission and retirement.
            // Clean child transactions record no copies and create no waits.
            scheduler.FlushPendingBufferTransfers();
            static const bool retain_stream = [] {
                const auto* v=std::getenv("EXECUTOR_RETAIN_PERSISTENT_UPLOADS");
                return v && std::strcmp(v,"1")==0;
            }();
            const bool read_only_retained = Lsx4::Portable::RetainPersistentUpload(
                retain_stream, staging_reserved, exact_dirty_upload_reservation,
                is_written, is_texel_buffer, external_snapshot_guard!=nullptr,
                transient_upload_buffer!=nullptr,
                reserved_upload_buffer==staging_buffer.Handle(), reserved_upload_capacity,
                total_size_bytes, chunk_limit, staging_buffer.SizeBytes());
            static const bool retain_writable = [] {
                const auto* value=std::getenv("EXECUTOR_RETAIN_WRITABLE_UPLOADS");
                return value && std::strcmp(value,"1")==0;
            }();
            const bool written_retained = Lsx4::Portable::RetainWritableUploadSource(
                retain_stream && retain_writable, is_written,
                reserved_upload_buffer != buffer.Handle(), staging_reserved,
                exact_dirty_upload_reservation, is_texel_buffer,
                external_snapshot_guard != nullptr, transient_upload_buffer != nullptr,
                reserved_upload_buffer == staging_buffer.Handle(), reserved_upload_capacity,
                total_size_bytes, chunk_limit, staging_buffer.SizeBytes());
            const bool retained = read_only_retained || written_retained;
            if (written_retained) {
                static std::atomic<u64> writes{};
                const u64 n=writes.fetch_add(1,std::memory_order_relaxed)+1;
                if(n==1 || (n&255u)==0) std::fprintf(stderr,
                    "[EXECUTOR_WRITABLE_UPLOAD_RETIREMENT] uploads=%llu distinctSource=1 committedTickProtection=1 destinationOwnershipUnchanged=1 copiesBarriersReadbackUnchanged=1\n",
                    static_cast<unsigned long long>(n));
            }
            if (retained) {
                static std::atomic<u64> retained_uploads{},retained_bytes{};
                const u64 count=retained_uploads.fetch_add(1,std::memory_order_relaxed)+1;
                const u64 bytes=retained_bytes.fetch_add(total_size_bytes,std::memory_order_relaxed)+total_size_bytes;
                if (count==1 || (count&255u)==0)
                    std::fprintf(stderr,"[EXECUTOR_PERSISTENT_UPLOAD_RETIREMENT] uploads=%llu copiedBytes=%llu reservedBytes=%llu streamCapacity=%llu committedTickProtection=1 queueCopiesUnchanged=1 transientFallback=1 destinationWrite=%u\n",
                        static_cast<unsigned long long>(count),static_cast<unsigned long long>(bytes),
                        static_cast<unsigned long long>(reserved_upload_capacity),
                        static_cast<unsigned long long>(staging_buffer.SizeBytes()), is_written ? 1u : 0u);
            } else {
                scheduler.Finish(Vulkan::Scheduler::ExecutorFinishReason::BufferCacheRetirement);
                scheduler.PopPendingOperations(true);
            }
        }
#endif
        TouchBuffer(buffer);
    }
    if (is_texel_buffer && !is_written) {
        const bool image_alias =
            static_cast<bool>(texture_cache.FindImageFromRange(device_addr, size));
        if (image_alias &&
            !SynchronizeBufferFromImage(buffer, device_addr, size)) {
            return fail("texel-image-copy");
        }
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_compute_dma_phase("sync-complete");
#endif
    return BufferSyncResult::Ready;
}

vk::Buffer BufferCache::UploadCopies(
    Buffer& buffer, std::span<vk::BufferCopy> copies,
    size_t total_size_bytes, std::unique_ptr<Buffer>& transient_buffer) {
    if (copies.empty()) {
        return VK_NULL_HANDLE;
    }
    const auto [staging, offset] = staging_buffer.Map(total_size_bytes);
    if (staging) {
        for (auto& copy : copies) {
            u8* const src_pointer = staging + copy.srcOffset;
            const VAddr device_addr = buffer.CpuAddr() + copy.dstOffset;
            // These ranges were selected as CPU-modified by MemoryTracker, so
            // the physical backing is authoritative. Read it through the
            // unprotected alias: the guest VA page can remain read-protected
            // solely because a disjoint range on the same 4-KiB page is still
            // GPU-modified.
            if (!memory->TryCopyBackingMemory(device_addr, src_pointer, copy.size)) {
                memory->CopySparseMemory(device_addr, src_pointer, copy.size);
            }
            copy.srcOffset += offset;
        }
        staging_buffer.Commit();
        return staging_buffer.Handle();
    } else {
        transient_buffer =
            std::make_unique<Buffer>(instance, scheduler, MemoryUsage::Upload, 0,
                                     vk::BufferUsageFlagBits::eTransferSrc, total_size_bytes);
        const vk::Buffer src_buffer = transient_buffer->Handle();
        u8* const staging = transient_buffer->mapped_data.data();
        for (const auto& copy : copies) {
            u8* const src_pointer = staging + copy.srcOffset;
            const VAddr device_addr = buffer.CpuAddr() + copy.dstOffset;
            if (!memory->TryCopyBackingMemory(device_addr, src_pointer, copy.size)) {
                memory->CopySparseMemory(device_addr, src_pointer, copy.size);
            }
        }
        return src_buffer;
    }
}

bool BufferCache::SynchronizeBufferFromImage(Buffer& buffer, VAddr device_addr, u32 size) {
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        return false;
    }
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u32 buf_offset = buffer.Offset(image.info.guest_address);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    u32 copy_size = 0;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (buf_offset + mip_info.offset + mip_info.size > buffer.SizeBytes()) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
        copy_size += mip_info.size;
    }
    if (copy_size == 0) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    PortableGpu::InvalidateProducerRange(this,buffer.CpuAddr(),buffer.SizeBytes());
    tile_manager.TileImage(image, buffer_copies, buffer.Handle(), buf_offset, copy_size);
    return true;
}

bool BufferCache::SynchronizeBuffersInRange(
    VAddr device_addr, u64 size,
    Core::MemoryManager::SnapshotReadGuard* external_snapshot_guard,
    const u32 compute_diagnostic_id,
    const u32 compute_diagnostic_range) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool diagnose_compute_dma = compute_diagnostic_id != 0 &&
        compute_diagnostic_id <= 256 &&
        (compute_diagnostic_range < 4 ||
         (compute_diagnostic_range & 15u) == 0u ||
         compute_diagnostic_range == 65) &&
        ExecutorComputeDmaSyncDiagnosticEnabled();
    const auto log_compute_dma_range =
        [&](const char* phase, const u32 buffer_ordinal = 0xffffffffu,
            const VAddr address = 0, const u64 range_size = 0) {
            if (!diagnose_compute_dma) {
                return;
            }
            __android_log_print(
                ANDROID_LOG_WARN, "LSX4Native",
                "[EXECUTOR_COMPUTE_DMA_RANGE_PHASE] id=%u range=%u buffer=%u "
                "phase=%s mappedAddr=0x%llx mappedSize=%llu addr=0x%llx "
                "size=%llu",
                compute_diagnostic_id, compute_diagnostic_range,
                buffer_ordinal, phase,
                static_cast<unsigned long long>(device_addr),
                static_cast<unsigned long long>(size),
                static_cast<unsigned long long>(address),
                static_cast<unsigned long long>(range_size));
        };
    log_compute_dma_range("range-entry");
#else
    (void)compute_diagnostic_id;
    (void)compute_diagnostic_range;
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_compute_dma_range("before-transfer-flush");
#endif
    scheduler.FlushPendingBufferTransfersForRange(device_addr, size);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_compute_dma_range("after-transfer-flush");
#endif
    const VAddr device_addr_end = device_addr + size;
    bool synchronized = true;
    u32 buffer_ordinal = 0;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_compute_dma_range("before-buffer-walk");
#endif
    ForEachBufferInRange(device_addr, size, [&](BufferId buffer_id, Buffer& buffer) {
        RENDERER_TRACE;
        VAddr start = std::max(buffer.CpuAddr(), device_addr);
        VAddr end = std::min(buffer.CpuAddr() + buffer.SizeBytes(), device_addr_end);
        u32 buffer_size = static_cast<u32>(end - start);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        log_compute_dma_range("before-buffer-sync", buffer_ordinal, start,
                              buffer_size);
#endif
        const bool buffer_synchronized =
            SynchronizeBuffer(buffer, start, buffer_size, false, false, false,
                              false, external_snapshot_guard,
                              compute_diagnostic_id,
                              compute_diagnostic_range, buffer_ordinal) ==
            BufferSyncResult::Ready;
        synchronized &= buffer_synchronized;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        log_compute_dma_range(buffer_synchronized ? "after-buffer-sync-ready" :
                                                    "after-buffer-sync-failed",
                              buffer_ordinal, start, buffer_size);
#endif
        ++buffer_ordinal;
    });
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_compute_dma_range(synchronized ? "range-complete-ready" :
                                             "range-complete-failed",
                          buffer_ordinal);
#endif
    return synchronized;
}

void BufferCache::WriteDataBuffer(BdaPageTableBuffer& buffer, VAddr address,
                                  const void* value, u32 num_bytes) {
    if (num_bytes == 0) {
        return;
    }
    PortableGpu::InvalidateProducerRange(this,address,num_bytes);
    const u64 destination_offset = buffer.Offset(address);
    if (!buffer.EnsureResident(destination_offset, num_bytes)) {
        ASSERT_MSG(false,
                   "Failed committing sparse BDA page-table residency for upload");
        return;
    }
    vk::BufferCopy copy = {
        .srcOffset = 0,
        .dstOffset = destination_offset,
        .size = num_bytes,
    };
    vk::Buffer src_buffer = staging_buffer.Handle();
    std::unique_ptr<Buffer> transient_upload_buffer;
    if (num_bytes < StagingBufferSize) {
        const auto [staging, offset] = staging_buffer.Map(num_bytes);
        std::memcpy(staging, value, num_bytes);
        copy.srcOffset = offset;
        staging_buffer.Commit();
    } else {
        transient_upload_buffer = std::make_unique<Buffer>(
            instance, scheduler, MemoryUsage::Upload, 0, vk::BufferUsageFlagBits::eTransferSrc,
            num_bytes);
        src_buffer = transient_upload_buffer->Handle();
        u8* const staging = transient_upload_buffer->mapped_data.data();
        std::memcpy(staging, value, num_bytes);
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (IsManagedVulkanTransferCoalescing()) {
        const bool has_guest_range = buffer.CpuAddr() != 0;
        scheduler.QueueBufferUploadCopies(
            src_buffer, buffer.Handle(), std::span{&copy, 1},
            has_guest_range ? address : 0, has_guest_range ? num_bytes : 0);
        if (transient_upload_buffer) {
            scheduler.FlushPendingBufferTransfers();
        }
    } else
#endif
    {
        scheduler.EndRendering(Vulkan::Scheduler::EndRenderingReason::BufferUpload);
        const auto cmdbuf = scheduler.CommandBuffer();
        const vk::BufferMemoryBarrier2 pre_barrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .buffer = buffer.Handle(),
            .offset = destination_offset,
            .size = num_bytes,
        };
        const vk::BufferMemoryBarrier2 post_barrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask =
                vk::AccessFlagBits2::eMemoryRead |
                vk::AccessFlagBits2::eMemoryWrite,
            .buffer = buffer.Handle(),
            .offset = destination_offset,
            .size = num_bytes,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &pre_barrier,
        });
        cmdbuf.copyBuffer(src_buffer, buffer.Handle(), copy);
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &post_barrier,
        });
    }
    if (transient_upload_buffer) {
        scheduler.DeferOperation(
            [upload = std::move(transient_upload_buffer)]() mutable {
                upload.reset();
            });
    }
}

void BufferCache::RunGarbageCollector(
    const std::optional<size_t> sampled_device_memory,
    const bool force_pressure_scan) {
    SCOPE_EXIT {
        ++gc_tick;
    };
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    // The proprietary A750 ICD does not expose VK_EXT_memory_budget.  Cache-owned
    // byte counters therefore miss the 512 MiB BDA table, fixed rings, VMA block
    // slack, driver-private allocations and superseded image backings.  The
    // rasterizer supplies VMA heap usage (the allocator's tracked block bytes)
    // for this exact device so both caches react before KGSL pushes Android
    // into swap.
    const auto& aggregate_pressure_policy =
        instance.GetCapabilityProfile().aggregate_memory_pressure;
    const bool exact_a750_aggregate_pressure =
        aggregate_pressure_policy.cache_pressure_active;
    const bool a750_combined_pressure_gc =
        aggregate_pressure_policy.combined_gc;
    const u64 device_budget = instance.GetMemoryPressureUsageLimit();
    const u64 aggregate_pressure_limit =
        a750_combined_pressure_gc
            ? device_budget * 55 / 100
            : device_budget - device_budget / 4;
    const u64 aggregate_critical_limit =
        a750_combined_pressure_gc
            ? device_budget * 65 / 100
            : device_budget - device_budget / 8;
    const bool aggregate_pressure =
        exact_a750_aggregate_pressure && sampled_device_memory &&
        device_budget != 0 &&
        *sampled_device_memory >= aggregate_pressure_limit;
    const bool aggregate_emergency =
        exact_a750_aggregate_pressure && sampled_device_memory &&
        device_budget != 0 &&
        *sampled_device_memory >= aggregate_critical_limit;
#else
    constexpr bool aggregate_pressure = false;
    constexpr bool aggregate_emergency = false;
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool trace_exact_a750_residency =
        instance.GetCapabilityProfile().diagnostic_a750_residency_telemetry;
    const bool trace_residency_sample =
        trace_exact_a750_residency &&
        (gc_tick < 16 || (gc_tick & 31u) == 0u);
    if (trace_residency_sample) {
        VmaTotalStatistics vma_stats{};
        vmaCalculateStatistics(instance.GetAllocator(), &vma_stats);

        std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> heap_budgets{};
        vmaGetHeapBudgets(instance.GetAllocator(), heap_budgets.data());
        u64 reported_usage = 0;
        u64 reported_budget = 0;
        for (const auto& heap : heap_budgets) {
            reported_usage += heap.usage;
            reported_budget += heap.budget;
        }

        const auto& total = vma_stats.total.statistics;
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_A750_VMA_RESIDENCY] tick=%llu blocks=%u allocations=%u "
            "blockMiB=%llu allocationMiB=%llu unusedMiB=%llu maxAllocationMiB=%llu "
            "budgetUsageMiB=%llu budgetMiB=%llu",
            static_cast<unsigned long long>(gc_tick), total.blockCount,
            total.allocationCount,
            static_cast<unsigned long long>(total.blockBytes / 1_MB),
            static_cast<unsigned long long>(total.allocationBytes / 1_MB),
            static_cast<unsigned long long>((total.blockBytes - total.allocationBytes) / 1_MB),
            static_cast<unsigned long long>(vma_stats.total.allocationSizeMax / 1_MB),
            static_cast<unsigned long long>(reported_usage / 1_MB),
            static_cast<unsigned long long>(reported_budget / 1_MB));
    }
#endif
    if (resident_buffer_bytes < trigger_gc_memory && !aggregate_pressure) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (trace_residency_sample) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_A750_BUFFER_RESIDENCY] tick=%llu state=below-trigger "
                "residentMiB=%llu triggerMiB=%llu criticalMiB=%llu buffers=%zu",
                static_cast<unsigned long long>(gc_tick),
                static_cast<unsigned long long>(resident_buffer_bytes / 1_MB),
                static_cast<unsigned long long>(trigger_gc_memory / 1_MB),
                static_cast<unsigned long long>(critical_gc_memory / 1_MB),
                slot_buffers.size());
        }
#endif
        return;
    }
    scheduler.FlushPendingBufferTransfers();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool protect_a750_bda_identity =
        ExecutorUsesExactA750TurnipBufferRetirementDrain(instance) &&
        Config::directMemoryAccess() && instance.IsBufferDeviceAddressSupported();
    // Transfer materialization and all emergency behavior stay unchanged.
    // Only avoid the LRU walk whose exact-route predicate protects every
    // otherwise clean buffer candidate.
    const bool protected_gc_candidate =
        instance.GetCapabilityProfile()
                .optimization_a750_protected_gc_fast_noop_ab &&
        protect_a750_bda_identity && !force_pressure_scan &&
        !aggregate_emergency && resident_buffer_bytes < critical_gc_memory;
    const bool protected_gc_audit_scan =
        protected_gc_candidate && (gc_tick & 127u) == 0u;
    const bool protected_gc_fast_noop =
        protected_gc_candidate && !protected_gc_audit_scan;
    if (protected_gc_fast_noop) {
        VideoCore::RecordMobileGpuBufferResidencyGcNoVictimSkip();
        if (trace_residency_sample) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_A750_BUFFER_RESIDENCY] tick=%llu "
                "state=no-victim-skip residentMiB=%llu buffers=%zu emergency=0",
                static_cast<unsigned long long>(gc_tick),
                static_cast<unsigned long long>(resident_buffer_bytes / 1_MB),
                slot_buffers.size());
        }
        return;
    }
#endif
    const bool aggressive =
        aggregate_emergency || resident_buffer_bytes >= critical_gc_memory;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const u64 ticks_to_destroy = std::min<u64>(aggressive ? 16 : 64, gc_tick);
    constexpr int CandidateScanMultiplier = 4;
    const int max_deletions = aggressive ? 48 : 16;
#else
    const u64 ticks_to_destroy = std::min<u64>(aggressive ? 80 : 160, gc_tick);
    constexpr int CandidateScanMultiplier = 2;
    const int max_deletions = aggressive ? 64 : 32;
#endif
    int remaining_deletions = max_deletions;
    int remaining_candidates = max_deletions * CandidateScanMultiplier;
    u64 clean_evicted = 0;
    u64 dirty_retained = 0;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    u64 bda_identity_retained = 0;
#endif
    const auto clean_up = [&](BufferId buffer_id) {
        if (remaining_deletions == 0 || remaining_candidates == 0) {
            return true;
        }
        --remaining_candidates;
        Buffer& buffer = slot_buffers[buffer_id];
        const VAddr buffer_addr = buffer.CpuAddr();
        const u64 buffer_size = buffer.SizeBytes();
        const bool gpu_dirty =
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            HasPendingReadback(buffer_addr, buffer_size) ||
#endif
            gpu_modified_ranges.Intersects(buffer_addr, buffer_size) ||
            memory_tracker->IsRegionGpuModified(buffer_addr, buffer_size);
        if (gpu_dirty) {
            // A deferred readback callback is keyed only by guest VA. Deleting
            // and unregistering this generation before publication lets that
            // callback overwrite a newer buffer mapped at the same VA. Keep
            // dirty buffers resident; clean eviction remains wait-free.
            TouchBuffer(buffer);
            ++dirty_retained;
            return false;
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (protect_a750_bda_identity) {
            // Direct-memory shaders may dereference this guest range through
            // the BDA page table without another host-side FindBuffer call.
            // Consequently LRU age and CPU/GPU dirty tracking cannot prove the
            // range dead.  Removing a supposedly clean entry zeroes its BDA
            // pages; Bloodborne then loses vertex/index resources a few
            // seconds after gameplay starts.  Stable identity is affordable
            // here because overlap replacement still retires generations via
            // JoinOverlap, bounded by the exact-route retirement drain.
            TouchBuffer(buffer);
            ++bda_identity_retained;
            return false;
        }
#endif
        --remaining_deletions;
        ++clean_evicted;
        DeleteBuffer(buffer_id);
        return false;
    };
    lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (clean_evicted != 0 && IsReadbackClassificationEnabled()) {
        RecordBufferCleanEvictionsClassification(clean_evicted);
    }
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    ASSERT_MSG(!protected_gc_audit_scan || clean_evicted == 0,
               "A750 protected buffer GC audit found an evictable identity");
    VideoCore::RecordMobileGpuBufferResidencyGc(
        clean_evicted, force_pressure_scan, protected_gc_audit_scan);
#else
    VideoCore::RecordMobileGpuBufferResidencyGc(clean_evicted, false, false);
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (trace_residency_sample) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_A750_BUFFER_RESIDENCY] tick=%llu state=collect "
            "residentMiB=%llu triggerMiB=%llu criticalMiB=%llu buffers=%zu "
            "scanned=%d dirtyRetained=%llu bdaRetained=%llu cleanEvicted=%llu "
            "aggressive=%u",
            static_cast<unsigned long long>(gc_tick),
            static_cast<unsigned long long>(resident_buffer_bytes / 1_MB),
            static_cast<unsigned long long>(trigger_gc_memory / 1_MB),
            static_cast<unsigned long long>(critical_gc_memory / 1_MB),
            slot_buffers.size(), max_deletions * CandidateScanMultiplier -
                                     remaining_candidates,
            static_cast<unsigned long long>(dirty_retained),
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            static_cast<unsigned long long>(bda_identity_retained),
#else
            0ULL,
#endif
            static_cast<unsigned long long>(clean_evicted),
            aggressive ? 1u : 0u);
    }
#endif
}

void BufferCache::TouchBuffer(const Buffer& buffer) {
    lru_cache.Touch(buffer.LRUId(), gc_tick);
}

void BufferCache::DeleteBuffer(BufferId buffer_id) {
    // Pending buffer-transfer batches retain raw Vulkan handles until their commands are
    // materialized. Never let a cache slot enter deferred destruction first.
    scheduler.FlushPendingBufferTransfers();
    Buffer& buffer = slot_buffers[buffer_id];
    const u64 retiring_bytes = Common::AlignUp(buffer.SizeBytes(), CACHING_PAGESIZE);
    Unregister(buffer_id);
    retiring_buffer_bytes.fetch_add(retiring_bytes, std::memory_order_release);
    scheduler.DeferOperation([this, buffer_id, retiring_bytes] {
        slot_buffers.erase(buffer_id);
        const u64 previous =
            retiring_buffer_bytes.fetch_sub(retiring_bytes, std::memory_order_acq_rel);
        ASSERT_MSG(previous >= retiring_bytes,
                   "retiring_buffer_bytes underflow while deleting cache buffer");
    });
    buffer.is_deleted = true;
}

}
