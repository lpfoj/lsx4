// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "producer_readback_bridge.h"
#include "common/config.h"
#include "common/debug.h"
#include "common/scope_exit.h"
#include "common/thread.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/invalidation_overlap_trace.h"
#include "video_core/mobile_gpu_optimization.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_driver_optimization.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>
#include <vector>

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
#include <array>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <pthread.h>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif
#include <dlfcn.h>
#include <cstdlib>
#include <mutex>
#include <android/log.h>
#include <unistd.h>
#endif

namespace Vulkan {

Scheduler::ScopedBufferUploadBatch::ScopedBufferUploadBatch(Scheduler& scheduler_) noexcept
    : scheduler{&scheduler_} {}

Scheduler::ScopedBufferUploadBatch::ScopedBufferUploadBatch(
    ScopedBufferUploadBatch&& other) noexcept
    : scheduler{std::exchange(other.scheduler, nullptr)} {}

Scheduler::ScopedBufferUploadBatch::~ScopedBufferUploadBatch() {
    Flush();
}

void Scheduler::ScopedBufferUploadBatch::Flush() {
    Scheduler* const owner = std::exchange(scheduler, nullptr);
    if (owner == nullptr) {
        return;
    }
    ASSERT(owner->scoped_buffer_upload_batch_depth != 0);
    --owner->scoped_buffer_upload_batch_depth;
    owner->FlushPendingBufferTransfers();
}

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
namespace {
bool ExecutorAndroidRendererProbesEnabled() {
    static const bool enabled =
        std::getenv("EXECUTOR_ANDROID_RENDERER_PROBES") != nullptr ||
        ::access("/data/data/app.lsx4.android/files/lsx4-home/diag-renderer-probes", F_OK) == 0;
    return enabled;
}

bool ExecutorAndroidGpuTimelineEnabled() {
    static const bool enabled =
        std::getenv("EXECUTOR_ANDROID_GPU_TIMELINE") != nullptr ||
        ::access("/data/data/app.lsx4.android/files/lsx4-home/diag-vk-gpu-timeline", F_OK) == 0;
    return enabled;
}

bool ExecutorFinishWaitProfileEnabled() {
    static const bool enabled = ExecutorAndroidGpuTimelineEnabled() ||
                                ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                                         "run-diagnostic-vk-finish-wait",
                                         F_OK) == 0;
    return enabled;
}

bool ExecutorVulkanOptimizationStatsEnabled() {
    static const bool enabled = ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                                         "run-diagnostic-vulkan-optimization-stats",
                                         F_OK) == 0;
    return enabled;
}

bool ExecutorSubmitMutexProfileEnabled() {
    static const bool enabled = ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                                         "run-diagnostic-submit-mutex-profile",
                                         F_OK) == 0;
    return enabled;
}

bool ExecutorSubmitReasonsEnabled() {
    static const bool enabled = ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                                         "run-diagnostic-submit-reasons",
                                         F_OK) == 0;
    return enabled;
}

bool ExecutorSubmitLatencyTraceEnabled() {
    static const bool enabled = ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                                         "run-diagnostic-submit-latency-trace",
                                         F_OK) == 0;
    return enabled;
}

u64 ExecutorSubmitMutexProfileNowNs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

constexpr std::size_t SubmitMutexProfileKindCount =
    static_cast<std::size_t>(GpuTimelineSchedulerKind::RepeatFlip) + 1;
constexpr u64 SubmitMutexProfilePhysicalSubmitWindow = 120;
constexpr std::size_t SubmitExecutionProfileKindCount = 4;

enum class SubmitExecutionProfileStage : std::size_t {
    FlushPendingBufferTransfers,
    EndRenderingAndCommandBufferEnd,
    ImGuiTextureSubmit,
    QueueSubmitPath,
    AllocateWorkerCommandBuffers,
    PopPendingOperations,
    Count,
};

constexpr std::size_t SubmitExecutionProfileStageCount =
    static_cast<std::size_t>(SubmitExecutionProfileStage::Count);

struct SubmitMutexProfileCounters {
    u64 lock_acquisitions{};
    u64 physical_submits{};
    u64 work_batches{};
    u64 logical_packets{};
    u64 command_buffers{};
    u64 empty_fence_submits{};
    u64 failed_submits{};
    u64 forced_boundaries{};
    u64 wait_total_ns{};
    u64 wait_max_ns{};
    u64 hold_total_ns{};
    u64 hold_max_ns{};
    u64 vk_queue_submit_total_ns{};
    u64 vk_queue_submit_max_ns{};
    u64 submit_execution_calls{};
    std::array<u64, SubmitExecutionProfileKindCount> submit_kind_calls{};
    u64 submit_external_sync_calls{};
    u64 submit_can_defer_calls{};
    u64 submit_deferred_calls{};
    u64 submit_direct_calls{};
    u64 submit_pending_batch_calls{};
    u64 submit_forced_boundary_calls{};
    u64 submit_execution_hold_total_ns{};
    u64 submit_execution_hold_max_ns{};
    std::array<u64, SubmitExecutionProfileStageCount> submit_stage_calls{};
    std::array<u64, SubmitExecutionProfileStageCount> submit_stage_total_ns{};
    std::array<u64, SubmitExecutionProfileStageCount> submit_stage_max_ns{};
    u64 submit_remainder_total_ns{};
    u64 submit_remainder_max_ns{};
};

struct SubmitMutexProfileSnapshot {
    bool valid{};
    GpuTimelineSchedulerKind kind{};
    u64 window{};
    SubmitMutexProfileCounters counters{};
};

// QueueSubmitLocked and the lock profiler both touch these counters only while
// Scheduler::submit_mutex is held. Publication happens after that lock is
// released so diagnostic file I/O cannot lengthen the measured critical path.
std::array<SubmitMutexProfileCounters, SubmitMutexProfileKindCount>
    g_submit_mutex_profile_counters{};
std::array<u64, SubmitMutexProfileKindCount> g_submit_mutex_profile_windows{};

constexpr std::size_t SubmitMutexProfileKindIndex(const GpuTimelineSchedulerKind kind) {
    return static_cast<std::size_t>(kind);
}

constexpr std::size_t SubmitExecutionProfileStageIndex(const SubmitExecutionProfileStage stage) {
    return static_cast<std::size_t>(stage);
}

struct SubmitExecutionProfileSample {
    bool enabled{};
    u64 hold_begin_ns{};
    std::size_t submit_kind_index{};
    bool has_external_sync{};
    bool can_defer{};
    bool deferred{};
    bool direct_submit{};
    bool pending_batch_submit{};
    bool forced_boundary{};
    std::array<u64, SubmitExecutionProfileStageCount> stage_calls{};
    std::array<u64, SubmitExecutionProfileStageCount> stage_ns{};
};

class SubmitExecutionProfileStageScope {
public:
    SubmitExecutionProfileStageScope(SubmitExecutionProfileSample& sample,
                                     const SubmitExecutionProfileStage stage)
        : sample{sample}, stage{stage} {
        if (sample.enabled) {
            begin_ns = ExecutorSubmitMutexProfileNowNs();
        }
    }

    ~SubmitExecutionProfileStageScope() {
        if (!sample.enabled) {
            return;
        }
        const std::size_t index = SubmitExecutionProfileStageIndex(stage);
        ++sample.stage_calls[index];
        sample.stage_ns[index] += ExecutorSubmitMutexProfileNowNs() - begin_ns;
    }

    SubmitExecutionProfileStageScope(const SubmitExecutionProfileStageScope&) = delete;
    SubmitExecutionProfileStageScope& operator=(const SubmitExecutionProfileStageScope&) = delete;

private:
    SubmitExecutionProfileSample& sample;
    SubmitExecutionProfileStage stage{};
    u64 begin_ns{};
};

void RecordSubmitExecutionProfileLocked(const SubmitExecutionProfileSample& sample) {
    if (!sample.enabled) {
        return;
    }

    const u64 hold_ns = ExecutorSubmitMutexProfileNowNs() - sample.hold_begin_ns;
    u64 accounted_ns = 0;
    for (const u64 stage_ns : sample.stage_ns) {
        accounted_ns += stage_ns;
    }
    const u64 remainder_ns = hold_ns > accounted_ns ? hold_ns - accounted_ns : 0;

    auto& counters = g_submit_mutex_profile_counters[SubmitMutexProfileKindIndex(
        GpuTimelineSchedulerKind::GuestDraw)];
    ++counters.submit_execution_calls;
    if (sample.submit_kind_index < counters.submit_kind_calls.size()) {
        ++counters.submit_kind_calls[sample.submit_kind_index];
    }
    counters.submit_external_sync_calls += sample.has_external_sync ? 1 : 0;
    counters.submit_can_defer_calls += sample.can_defer ? 1 : 0;
    counters.submit_deferred_calls += sample.deferred ? 1 : 0;
    counters.submit_direct_calls += sample.direct_submit ? 1 : 0;
    counters.submit_pending_batch_calls += sample.pending_batch_submit ? 1 : 0;
    counters.submit_forced_boundary_calls += sample.forced_boundary ? 1 : 0;
    counters.submit_execution_hold_total_ns += hold_ns;
    counters.submit_execution_hold_max_ns =
        std::max(counters.submit_execution_hold_max_ns, hold_ns);
    for (std::size_t index = 0; index < sample.stage_ns.size(); ++index) {
        counters.submit_stage_calls[index] += sample.stage_calls[index];
        counters.submit_stage_total_ns[index] += sample.stage_ns[index];
        counters.submit_stage_max_ns[index] =
            std::max(counters.submit_stage_max_ns[index], sample.stage_ns[index]);
    }
    counters.submit_remainder_total_ns += remainder_ns;
    counters.submit_remainder_max_ns = std::max(counters.submit_remainder_max_ns, remainder_ns);
}

void RecordSubmitMutexProfileQueueSubmitLocked(const GpuTimelineSchedulerKind kind,
                                               const u64 elapsed_ns, const u64 logical_packets,
                                               const u64 command_buffers, const vk::Result result,
                                               const bool forced_boundary,
                                               const bool empty_fence_submit) {
    auto& counters = g_submit_mutex_profile_counters[SubmitMutexProfileKindIndex(kind)];
    ++counters.physical_submits;
    counters.vk_queue_submit_total_ns += elapsed_ns;
    counters.vk_queue_submit_max_ns = std::max(counters.vk_queue_submit_max_ns, elapsed_ns);
    counters.failed_submits += result == vk::Result::eSuccess ? 0 : 1;
    if (empty_fence_submit) {
        ++counters.empty_fence_submits;
        return;
    }
    ++counters.work_batches;
    counters.logical_packets += logical_packets;
    counters.command_buffers += command_buffers;
    counters.forced_boundaries += forced_boundary ? 1 : 0;
}

std::array<SubmitMutexProfileSnapshot, SubmitMutexProfileKindCount>
FinishSubmitMutexProfileLockLocked(const GpuTimelineSchedulerKind kind, const u64 wait_ns,
                                   const u64 hold_ns) {
    auto& current = g_submit_mutex_profile_counters[SubmitMutexProfileKindIndex(kind)];
    ++current.lock_acquisitions;
    current.wait_total_ns += wait_ns;
    current.wait_max_ns = std::max(current.wait_max_ns, wait_ns);
    current.hold_total_ns += hold_ns;
    current.hold_max_ns = std::max(current.hold_max_ns, hold_ns);

    std::array<SubmitMutexProfileSnapshot, SubmitMutexProfileKindCount> snapshots{};
    for (std::size_t index = 0; index < snapshots.size(); ++index) {
        auto& counters = g_submit_mutex_profile_counters[index];
        if (counters.physical_submits < SubmitMutexProfilePhysicalSubmitWindow) {
            continue;
        }
        snapshots[index] = {
            .valid = true,
            .kind = static_cast<GpuTimelineSchedulerKind>(index),
            .window = ++g_submit_mutex_profile_windows[index],
            .counters = counters,
        };
        counters = {};
    }
    return snapshots;
}

const char* GpuTimelineKindName(GpuTimelineSchedulerKind kind);

void PublishSubmitMutexProfileSnapshot(const SubmitMutexProfileSnapshot& snapshot) {
    if (!snapshot.valid) {
        return;
    }

    // This mutex, formatting, logging and file I/O exist only on an explicitly
    // marker-enabled diagnostic path and run outside Scheduler::submit_mutex.
    static std::mutex output_mutex;
    static bool first_file_write = true;
    std::scoped_lock output_lock{output_mutex};

    const auto& counters = snapshot.counters;
    const double wait_total_ms = static_cast<double>(counters.wait_total_ns) / 1'000'000.0;
    const double wait_avg_us =
        counters.lock_acquisitions == 0
            ? 0.0
            : static_cast<double>(counters.wait_total_ns) /
                  (static_cast<double>(counters.lock_acquisitions) * 1'000.0);
    const double hold_total_ms = static_cast<double>(counters.hold_total_ns) / 1'000'000.0;
    const double hold_avg_us =
        counters.lock_acquisitions == 0
            ? 0.0
            : static_cast<double>(counters.hold_total_ns) /
                  (static_cast<double>(counters.lock_acquisitions) * 1'000.0);
    const double queue_total_ms =
        static_cast<double>(counters.vk_queue_submit_total_ns) / 1'000'000.0;
    const double queue_avg_us =
        counters.physical_submits == 0
            ? 0.0
            : static_cast<double>(counters.vk_queue_submit_total_ns) /
                  (static_cast<double>(counters.physical_submits) * 1'000.0);
    const auto stage_calls = [&](const SubmitExecutionProfileStage stage) {
        return counters.submit_stage_calls[SubmitExecutionProfileStageIndex(stage)];
    };
    const auto stage_total_ms = [&](const SubmitExecutionProfileStage stage) {
        return static_cast<double>(
                   counters.submit_stage_total_ns[SubmitExecutionProfileStageIndex(stage)]) /
               1'000'000.0;
    };
    const auto stage_max_ms = [&](const SubmitExecutionProfileStage stage) {
        return static_cast<double>(
                   counters.submit_stage_max_ns[SubmitExecutionProfileStageIndex(stage)]) /
               1'000'000.0;
    };

    char line[4096]{};
    std::snprintf(
        line, sizeof(line),
        "schema=2 window=%llu kind=%s physicalSubmits=%llu "
        "lockAcquisitions=%llu workBatches=%llu logicalPackets=%llu "
        "commandBuffers=%llu emptyFenceSubmits=%llu failedSubmits=%llu "
        "forcedBoundaries=%llu waitTotalMs=%.3f waitAvgUs=%.3f "
        "waitMaxMs=%.3f holdTotalMs=%.3f holdAvgUs=%.3f "
        "holdMaxMs=%.3f vkQueueSubmitTotalMs=%.3f "
        "vkQueueSubmitAvgUs=%.3f vkQueueSubmitMaxMs=%.3f "
        "submitExec=%llu flushInfo=%llu flushPlain=%llu finish=%llu "
        "waitCurrent=%llu externalSync=%llu canDefer=%llu deferred=%llu "
        "directSubmit=%llu pendingBatchSubmit=%llu "
        "submitForcedBoundary=%llu submitExecHoldTotalMs=%.3f "
        "submitExecHoldMaxMs=%.3f "
        "flushTransfersCalls=%llu flushTransfersTotalMs=%.3f "
        "flushTransfersMaxMs=%.3f endRenderEndCalls=%llu "
        "endRenderEndTotalMs=%.3f endRenderEndMaxMs=%.3f "
        "imguiSubmitCalls=%llu imguiSubmitTotalMs=%.3f "
        "imguiSubmitMaxMs=%.3f queueSubmitPathCalls=%llu "
        "queueSubmitPathTotalMs=%.3f queueSubmitPathMaxMs=%.3f "
        "allocateWorkerCalls=%llu allocateWorkerTotalMs=%.3f "
        "allocateWorkerMaxMs=%.3f popPendingCalls=%llu "
        "popPendingTotalMs=%.3f popPendingMaxMs=%.3f "
        "remainderTotalMs=%.3f remainderMaxMs=%.3f",
        static_cast<unsigned long long>(snapshot.window), GpuTimelineKindName(snapshot.kind),
        static_cast<unsigned long long>(counters.physical_submits),
        static_cast<unsigned long long>(counters.lock_acquisitions),
        static_cast<unsigned long long>(counters.work_batches),
        static_cast<unsigned long long>(counters.logical_packets),
        static_cast<unsigned long long>(counters.command_buffers),
        static_cast<unsigned long long>(counters.empty_fence_submits),
        static_cast<unsigned long long>(counters.failed_submits),
        static_cast<unsigned long long>(counters.forced_boundaries), wait_total_ms, wait_avg_us,
        static_cast<double>(counters.wait_max_ns) / 1'000'000.0, hold_total_ms, hold_avg_us,
        static_cast<double>(counters.hold_max_ns) / 1'000'000.0, queue_total_ms, queue_avg_us,
        static_cast<double>(counters.vk_queue_submit_max_ns) / 1'000'000.0,
        static_cast<unsigned long long>(counters.submit_execution_calls),
        static_cast<unsigned long long>(counters.submit_kind_calls[0]),
        static_cast<unsigned long long>(counters.submit_kind_calls[1]),
        static_cast<unsigned long long>(counters.submit_kind_calls[2]),
        static_cast<unsigned long long>(counters.submit_kind_calls[3]),
        static_cast<unsigned long long>(counters.submit_external_sync_calls),
        static_cast<unsigned long long>(counters.submit_can_defer_calls),
        static_cast<unsigned long long>(counters.submit_deferred_calls),
        static_cast<unsigned long long>(counters.submit_direct_calls),
        static_cast<unsigned long long>(counters.submit_pending_batch_calls),
        static_cast<unsigned long long>(counters.submit_forced_boundary_calls),
        static_cast<double>(counters.submit_execution_hold_total_ns) / 1'000'000.0,
        static_cast<double>(counters.submit_execution_hold_max_ns) / 1'000'000.0,
        static_cast<unsigned long long>(
            stage_calls(SubmitExecutionProfileStage::FlushPendingBufferTransfers)),
        stage_total_ms(SubmitExecutionProfileStage::FlushPendingBufferTransfers),
        stage_max_ms(SubmitExecutionProfileStage::FlushPendingBufferTransfers),
        static_cast<unsigned long long>(
            stage_calls(SubmitExecutionProfileStage::EndRenderingAndCommandBufferEnd)),
        stage_total_ms(SubmitExecutionProfileStage::EndRenderingAndCommandBufferEnd),
        stage_max_ms(SubmitExecutionProfileStage::EndRenderingAndCommandBufferEnd),
        static_cast<unsigned long long>(
            stage_calls(SubmitExecutionProfileStage::ImGuiTextureSubmit)),
        stage_total_ms(SubmitExecutionProfileStage::ImGuiTextureSubmit),
        stage_max_ms(SubmitExecutionProfileStage::ImGuiTextureSubmit),
        static_cast<unsigned long long>(stage_calls(SubmitExecutionProfileStage::QueueSubmitPath)),
        stage_total_ms(SubmitExecutionProfileStage::QueueSubmitPath),
        stage_max_ms(SubmitExecutionProfileStage::QueueSubmitPath),
        static_cast<unsigned long long>(
            stage_calls(SubmitExecutionProfileStage::AllocateWorkerCommandBuffers)),
        stage_total_ms(SubmitExecutionProfileStage::AllocateWorkerCommandBuffers),
        stage_max_ms(SubmitExecutionProfileStage::AllocateWorkerCommandBuffers),
        static_cast<unsigned long long>(
            stage_calls(SubmitExecutionProfileStage::PopPendingOperations)),
        stage_total_ms(SubmitExecutionProfileStage::PopPendingOperations),
        stage_max_ms(SubmitExecutionProfileStage::PopPendingOperations),
        static_cast<double>(counters.submit_remainder_total_ns) / 1'000'000.0,
        static_cast<double>(counters.submit_remainder_max_ns) / 1'000'000.0);

    __android_log_print(ANDROID_LOG_INFO, "LSX4Native", "[EXECUTOR_VK_SUBMIT_MUTEX_PROFILE] %s",
                        line);
    FILE* out = std::fopen("/data/data/app.lsx4.android/files/runtime/"
                           "submit-mutex-profile.txt",
                           first_file_write ? "w" : "a");
    if (out != nullptr) {
        std::fprintf(out, "%s\n", line);
        std::fclose(out);
        first_file_write = false;
    }
}

class SubmitMutexProfileLock {
public:
    explicit SubmitMutexProfileLock(std::mutex& mutex, const GpuTimelineSchedulerKind kind)
        : enabled{ExecutorSubmitMutexProfileEnabled()}, kind{kind}, lock{mutex, std::defer_lock} {
        if (enabled) {
            wait_begin_ns = ExecutorSubmitMutexProfileNowNs();
        }
        lock.lock();
        if (enabled) {
            acquired_ns = ExecutorSubmitMutexProfileNowNs();
        }
    }

    ~SubmitMutexProfileLock() {
        if (!enabled) {
            return;
        }
        const u64 hold_end_ns = ExecutorSubmitMutexProfileNowNs();
        const auto snapshots = FinishSubmitMutexProfileLockLocked(kind, acquired_ns - wait_begin_ns,
                                                                  hold_end_ns - acquired_ns);
        lock.unlock();
        for (const auto& snapshot : snapshots) {
            PublishSubmitMutexProfileSnapshot(snapshot);
        }
    }

    SubmitMutexProfileLock(const SubmitMutexProfileLock&) = delete;
    SubmitMutexProfileLock& operator=(const SubmitMutexProfileLock&) = delete;

    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }

    [[nodiscard]] u64 AcquiredNs() const noexcept {
        return acquired_ns;
    }

private:
    bool enabled{};
    GpuTimelineSchedulerKind kind{};
    std::unique_lock<std::mutex> lock;
    u64 wait_begin_ns{};
    u64 acquired_ns{};
};

// condition_variable_any invokes BasicLockable::lock only after its kernel
// wait has ended. Timing this adapter's lock therefore measures mutex
// reacquisition contention without charging the condition-variable sleep.
class SubmitMutexProfileReacquireMutex {
public:
    explicit SubmitMutexProfileReacquireMutex(std::mutex& mutex)
        : mutex{mutex}, enabled{ExecutorSubmitMutexProfileEnabled()} {}

    void lock() {
        const u64 wait_begin_ns = enabled ? ExecutorSubmitMutexProfileNowNs() : 0;
        mutex.lock();
        if (enabled) {
            acquired_ns = ExecutorSubmitMutexProfileNowNs();
            wait_ns = acquired_ns - wait_begin_ns;
        }
    }

    void unlock() {
        mutex.unlock();
    }

    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }

    [[nodiscard]] u64 WaitNs() const noexcept {
        return wait_ns;
    }

    [[nodiscard]] u64 AcquiredNs() const noexcept {
        return acquired_ns;
    }

private:
    std::mutex& mutex;
    bool enabled{};
    u64 wait_ns{};
    u64 acquired_ns{};
};

std::atomic<u64> g_executor_gpu_frame_epoch{1};
std::atomic<u64> g_executor_gpu_frame_boundaries{};

struct GpuTimelineSampleRecord {
    const void* scheduler{};
    GpuTimelineSchedulerKind kind{};
    u32 valid_bits{};
    u64 epoch{};
    u64 submit_ordinal{};
    u64 logical_tick{};
    u64 physical_submit_sequence{};
    u32 physical_submit_index{};
    u64 begin{};
    u64 end{};
    u64 cpu_record_begin_ns{};
    u64 cpu_record_end_ns{};
    u64 cpu_submit_begin_ns{};
    u64 cpu_submit_end_ns{};
    u64 attempts{};
    u64 recorded{};
    u64 collected{};
    u64 ring_full_skips{};
    u64 not_ready_polls{};
    double period_ns{};
};

struct GpuTimelineFrameRecord {
    u64 boundary{};
    u64 closed_epoch{};
    u64 cpu_ns{};
    s64 flip_arg{};
    bool is_eop{};
};

struct GpuTimelineQueueSubmitRecord {
    u64 sequence{};
    GpuTimelineSchedulerKind kind{};
    u64 first_tick{};
    u64 last_tick{};
    u64 cpu_begin_ns{};
    u64 cpu_end_ns{};
    u32 logical_count{};
    u32 command_buffer_count{};
    s32 result{};
    bool forced_boundary{};
    bool empty_fence_submit{};
    bool queue_present{};
};

struct GpuTimelineCalibrationRecord {
    u64 sequence{};
    u64 cpu_before_ns{};
    u64 cpu_after_ns{};
    u64 device_ticks{};
    u64 host_timestamp{};
    u64 max_deviation_ns{};
};

constexpr std::size_t GpuTimelineMaxSamples = 16384;
constexpr std::size_t GpuTimelineMaxFrames = 2048;
constexpr std::size_t GpuTimelineMaxQueueSubmits = 16384;
constexpr std::size_t GpuTimelineMaxCalibrations = 4096;
constexpr std::size_t GpuTimelineQueueSubmitTailCapacity = 256;
std::array<GpuTimelineSampleRecord, GpuTimelineMaxSamples> g_gpu_timeline_samples{};
std::array<GpuTimelineFrameRecord, GpuTimelineMaxFrames> g_gpu_timeline_frames{};
std::array<GpuTimelineQueueSubmitRecord, GpuTimelineMaxQueueSubmits> g_gpu_timeline_queue_submits{};
std::array<GpuTimelineQueueSubmitRecord, GpuTimelineQueueSubmitTailCapacity>
    g_gpu_timeline_queue_submit_tail{};
std::array<GpuTimelineCalibrationRecord, GpuTimelineMaxCalibrations> g_gpu_timeline_calibrations{};
std::mutex g_gpu_timeline_mutex;
constexpr std::size_t GpuTimestampSchedulerRegistryCapacity = 8;
std::array<Scheduler*, GpuTimestampSchedulerRegistryCapacity> g_gpu_timestamp_schedulers{};
std::mutex g_gpu_timestamp_scheduler_registry_mutex;
std::size_t g_gpu_timeline_sample_count{};
std::size_t g_gpu_timeline_frame_count{};
std::size_t g_gpu_timeline_queue_submit_count{};
std::size_t g_gpu_timeline_queue_submit_tail_count{};
std::size_t g_gpu_timeline_queue_submit_tail_next{};
std::size_t g_gpu_timeline_calibration_count{};
u64 g_gpu_timeline_dropped_samples{};
u64 g_gpu_timeline_physical_submit_sequence{};

const char* GpuTimelineKindName(const GpuTimelineSchedulerKind kind) {
    switch (kind) {
    case GpuTimelineSchedulerKind::GuestDraw:
        return "draw";
    case GpuTimelineSchedulerKind::Present:
        return "present";
    case GpuTimelineSchedulerKind::RepeatFlip:
        return "repeat";
    }
    return "unknown";
}

void RecordGpuTimelineSample(const GpuTimelineSampleRecord& record) {
    std::scoped_lock lock{g_gpu_timeline_mutex};
    if (g_gpu_timeline_sample_count == g_gpu_timeline_samples.size()) {
        ++g_gpu_timeline_dropped_samples;
        return;
    }
    g_gpu_timeline_samples[g_gpu_timeline_sample_count++] = record;
}

void RecordGpuTimelineQueueSubmit(const GpuTimelineQueueSubmitRecord& record) {
    std::scoped_lock lock{g_gpu_timeline_mutex};
    g_gpu_timeline_queue_submit_tail[g_gpu_timeline_queue_submit_tail_next] = record;
    g_gpu_timeline_queue_submit_tail_next =
        (g_gpu_timeline_queue_submit_tail_next + 1) % g_gpu_timeline_queue_submit_tail.size();
    g_gpu_timeline_queue_submit_tail_count = std::min(g_gpu_timeline_queue_submit_tail_count + 1,
                                                      g_gpu_timeline_queue_submit_tail.size());
    if (g_gpu_timeline_queue_submit_count == g_gpu_timeline_queue_submits.size()) {
        return;
    }
    g_gpu_timeline_queue_submits[g_gpu_timeline_queue_submit_count++] = record;
}

void RecordGpuTimelineCalibration(const GpuTimelineCalibrationRecord& record) {
    std::scoped_lock lock{g_gpu_timeline_mutex};
    if (g_gpu_timeline_calibration_count == g_gpu_timeline_calibrations.size()) {
        return;
    }
    g_gpu_timeline_calibrations[g_gpu_timeline_calibration_count++] = record;
}

void DumpGpuTimeline() {
    std::scoped_lock lock{g_gpu_timeline_mutex};
    FILE* out = std::fopen("/data/data/app.lsx4.android/files/runtime/gpu-timeline.txt", "w");
    if (out == nullptr) {
        return;
    }
    std::fprintf(out,
                 "schema=6 samples=%zu frames=%zu queueSubmits=%zu queueTail=%zu "
                 "calibrations=%zu "
                 "droppedSamples=%llu "
                 "currentEpoch=%llu\n",
                 g_gpu_timeline_sample_count, g_gpu_timeline_frame_count,
                 g_gpu_timeline_queue_submit_count, g_gpu_timeline_queue_submit_tail_count,
                 g_gpu_timeline_calibration_count,
                 static_cast<unsigned long long>(g_gpu_timeline_dropped_samples),
                 static_cast<unsigned long long>(
                     g_executor_gpu_frame_epoch.load(std::memory_order_acquire)));
    for (std::size_t index = 0; index < g_gpu_timeline_frame_count; ++index) {
        const auto& frame = g_gpu_timeline_frames[index];
        std::fprintf(out, "F boundary=%llu epoch=%llu cpuNs=%llu flipArg=%lld isEop=%u\n",
                     static_cast<unsigned long long>(frame.boundary),
                     static_cast<unsigned long long>(frame.closed_epoch),
                     static_cast<unsigned long long>(frame.cpu_ns),
                     static_cast<long long>(frame.flip_arg), frame.is_eop ? 1u : 0u);
    }
    for (std::size_t index = 0; index < g_gpu_timeline_queue_submit_count; ++index) {
        const auto& submit = g_gpu_timeline_queue_submits[index];
        std::fprintf(out,
                     "Q sequence=%llu kind=%s firstTick=%llu lastTick=%llu cpuBeginNs=%llu "
                     "cpuEndNs=%llu logical=%u commandBuffers=%u result=%d forced=%u "
                     "emptyFence=%u queuePresent=%u\n",
                     static_cast<unsigned long long>(submit.sequence),
                     GpuTimelineKindName(submit.kind),
                     static_cast<unsigned long long>(submit.first_tick),
                     static_cast<unsigned long long>(submit.last_tick),
                     static_cast<unsigned long long>(submit.cpu_begin_ns),
                     static_cast<unsigned long long>(submit.cpu_end_ns), submit.logical_count,
                     submit.command_buffer_count, submit.result, submit.forced_boundary ? 1u : 0u,
                     submit.empty_fence_submit ? 1u : 0u, submit.queue_present ? 1u : 0u);
    }
    const std::size_t queue_tail_start =
        g_gpu_timeline_queue_submit_tail_count == g_gpu_timeline_queue_submit_tail.size()
            ? g_gpu_timeline_queue_submit_tail_next
            : 0;
    for (std::size_t index = 0; index < g_gpu_timeline_queue_submit_tail_count; ++index) {
        const auto& submit =
            g_gpu_timeline_queue_submit_tail[(queue_tail_start + index) %
                                             g_gpu_timeline_queue_submit_tail.size()];
        std::fprintf(out,
                     "QT sequence=%llu kind=%s firstTick=%llu lastTick=%llu "
                     "cpuBeginNs=%llu cpuEndNs=%llu logical=%u commandBuffers=%u "
                     "result=%d forced=%u emptyFence=%u queuePresent=%u\n",
                     static_cast<unsigned long long>(submit.sequence),
                     GpuTimelineKindName(submit.kind),
                     static_cast<unsigned long long>(submit.first_tick),
                     static_cast<unsigned long long>(submit.last_tick),
                     static_cast<unsigned long long>(submit.cpu_begin_ns),
                     static_cast<unsigned long long>(submit.cpu_end_ns), submit.logical_count,
                     submit.command_buffer_count, submit.result, submit.forced_boundary ? 1u : 0u,
                     submit.empty_fence_submit ? 1u : 0u, submit.queue_present ? 1u : 0u);
    }
    for (std::size_t index = 0; index < g_gpu_timeline_calibration_count; ++index) {
        const auto& calibration = g_gpu_timeline_calibrations[index];
        std::fprintf(out,
                     "C sequence=%llu cpuBeforeNs=%llu cpuAfterNs=%llu deviceTicks=%llu "
                     "hostTimestamp=%llu maxDeviationNs=%llu\n",
                     static_cast<unsigned long long>(calibration.sequence),
                     static_cast<unsigned long long>(calibration.cpu_before_ns),
                     static_cast<unsigned long long>(calibration.cpu_after_ns),
                     static_cast<unsigned long long>(calibration.device_ticks),
                     static_cast<unsigned long long>(calibration.host_timestamp),
                     static_cast<unsigned long long>(calibration.max_deviation_ns));
    }
    for (std::size_t index = 0; index < g_gpu_timeline_sample_count; ++index) {
        const auto& sample = g_gpu_timeline_samples[index];
        std::fprintf(out,
                     "S scheduler=%p kind=%s epoch=%llu submit=%llu logicalTick=%llu "
                     "physicalSequence=%llu physicalIndex=%u begin=%llu end=%llu validBits=%u "
                     "periodNs=%.9f cpuRecordBeginNs=%llu cpuRecordEndNs=%llu "
                     "cpuSubmitBeginNs=%llu cpuSubmitEndNs=%llu attempts=%llu "
                     "recorded=%llu collected=%llu ringFullSkips=%llu notReadyPolls=%llu\n",
                     sample.scheduler, GpuTimelineKindName(sample.kind),
                     static_cast<unsigned long long>(sample.epoch),
                     static_cast<unsigned long long>(sample.submit_ordinal),
                     static_cast<unsigned long long>(sample.logical_tick),
                     static_cast<unsigned long long>(sample.physical_submit_sequence),
                     sample.physical_submit_index, static_cast<unsigned long long>(sample.begin),
                     static_cast<unsigned long long>(sample.end), sample.valid_bits,
                     sample.period_ns, static_cast<unsigned long long>(sample.cpu_record_begin_ns),
                     static_cast<unsigned long long>(sample.cpu_record_end_ns),
                     static_cast<unsigned long long>(sample.cpu_submit_begin_ns),
                     static_cast<unsigned long long>(sample.cpu_submit_end_ns),
                     static_cast<unsigned long long>(sample.attempts),
                     static_cast<unsigned long long>(sample.recorded),
                     static_cast<unsigned long long>(sample.collected),
                     static_cast<unsigned long long>(sample.ring_full_skips),
                     static_cast<unsigned long long>(sample.not_ready_polls));
    }
    std::fclose(out);
}

// Which render scopes actually shrank under DRS and which kept the guest extent.
// If the bulk of the work sits in unscaled passes, scaling the scene never
// touched it - that is the question this answers with a count, not a guess.
struct PassExtents {
    struct Row {
        std::uint32_t w, h;
        std::uint64_t scopes;
        std::uint64_t depth_only;
    };
    Row rows[24]{};
    std::mutex lock;
    std::uint64_t total{0};
    void note(std::uint32_t w, std::uint32_t h, bool depth_only) {
        std::scoped_lock lk{lock};
        ++total;
        for (auto& r : rows) {
            if (r.scopes && r.w == w && r.h == h) {
                ++r.scopes;
                r.depth_only += depth_only;
                goto dump;
            }
            if (!r.scopes) {
                r = {w, h, 1, depth_only ? 1ull : 0ull};
                goto dump;
            }
        }
    dump:
        if ((total % 4096) != 0)
            return;
        if (FILE* out =
                std::fopen("/data/data/app.lsx4.android/files/runtime/pass-extents.txt", "w")) {
            std::fprintf(out, "scopes_total=%llu", (unsigned long long)total);
            std::fputc(10, out);
            for (const auto& r : rows) {
                if (!r.scopes)
                    continue;
                std::fprintf(out, "%ux%u  scopes=%llu  %.1f%%  depth_only=%llu (%.1f%%)", r.w, r.h,
                             (unsigned long long)r.scopes, 100.0 * r.scopes / total,
                             (unsigned long long)r.depth_only,
                             r.scopes ? 100.0 * r.depth_only / r.scopes : 0.0);
                std::fputc(10, out);
            }
            std::fclose(out);
        }
    }
};
PassExtents g_pass_extents;
} // namespace

u64 ExecutorVkGpuTimelineReserveFrameToken() noexcept {
    return VideoCore::InvalidationOverlapTrace::ReserveFrameTokenV2();
}

void ExecutorVkGpuTimelineFrameBoundary(const s64 flip_arg, const bool is_eop,
                                        const u64 frame_token) noexcept {
    const bool timeline_enabled = ExecutorAndroidGpuTimelineEnabled();
    const bool overlap_v2_enabled = VideoCore::InvalidationOverlapTrace::V2Enabled();
    if (!timeline_enabled && !overlap_v2_enabled) {
        return;
    }
    constexpr const char* ResetMarker =
        "/data/data/app.lsx4.android/files/lsx4-home/diag-vk-gpu-timeline-reset";
    if (timeline_enabled && ::access(ResetMarker, F_OK) == 0) {
        {
            std::scoped_lock lock{g_gpu_timeline_mutex};
            g_gpu_timeline_sample_count = 0;
            g_gpu_timeline_frame_count = 0;
            g_gpu_timeline_queue_submit_count = 0;
            g_gpu_timeline_queue_submit_tail_count = 0;
            g_gpu_timeline_queue_submit_tail_next = 0;
            g_gpu_timeline_calibration_count = 0;
            g_gpu_timeline_dropped_samples = 0;
        }
        ::unlink(ResetMarker);
        ::unlink("/data/data/app.lsx4.android/files/runtime/gpu-timeline.txt");
    }
    const u64 closed_epoch = g_executor_gpu_frame_epoch.fetch_add(1, std::memory_order_acq_rel);
    const u64 boundary =
        g_executor_gpu_frame_boundaries.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto boundary_time = std::chrono::steady_clock::now();
    const auto cpu_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(boundary_time.time_since_epoch())
            .count();
    if (overlap_v2_enabled) {
        VideoCore::InvalidationOverlapTrace::RecordFrameBoundaryV2(
            boundary_time, boundary, closed_epoch, frame_token, is_eop);
    }
    if (!timeline_enabled) {
        return;
    }
    {
        std::scoped_lock lock{g_gpu_timeline_mutex};
        if (g_gpu_timeline_frame_count < g_gpu_timeline_frames.size()) {
            g_gpu_timeline_frames[g_gpu_timeline_frame_count++] = {
                .boundary = boundary,
                .closed_epoch = closed_epoch,
                .cpu_ns = static_cast<u64>(cpu_ns),
                .flip_arg = flip_arg,
                .is_eop = is_eop,
            };
        }
    }
    if ((boundary % 64) == 0) {
        DumpGpuTimeline();
    }
}

u64 ExecutorVkGpuTimelineCurrentFrameEpoch() noexcept {
    return g_executor_gpu_frame_epoch.load(std::memory_order_acquire);
}

u64 ExecutorVkGpuTimelineQueuePresentBegin(u64& cpu_begin_ns) noexcept {
    if (!ExecutorAndroidGpuTimelineEnabled()) {
        cpu_begin_ns = 0;
        return 0;
    }
    cpu_begin_ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
    return ++g_gpu_timeline_physical_submit_sequence;
}

void ExecutorVkGpuTimelineQueuePresentEnd(const u64 sequence, const u64 cpu_begin_ns,
                                          const bool success) noexcept {
    if (sequence == 0) {
        return;
    }
    const u64 cpu_end_ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::chrono::steady_clock::now().time_since_epoch())
                                                .count());
    RecordGpuTimelineQueueSubmit({
        .sequence = sequence,
        .kind = GpuTimelineSchedulerKind::Present,
        .cpu_begin_ns = cpu_begin_ns,
        .cpu_end_ns = cpu_end_ns,
        .result = success ? 0 : -1,
        .forced_boundary = true,
        .queue_present = true,
    });
}
#endif

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
namespace {
// The remaining "fixed" milliseconds of a frame were only ever inferred. Time the
// actual boundaries so the plan works off measurements: submit, GPU wait, pending
// transfer flush and readback are separated instead of lumped together.
struct SchedProbe {
    std::atomic<std::uint64_t> ns[4]{};
    std::atomic<std::uint64_t> hits[4]{};
    std::atomic<std::uint64_t> ticks{0};
};
SchedProbe g_sched_probe;
const char* const kSchedProbeName[4] = {"submit", "wait", "flush_transfers", "readback"};

struct SchedSpan {
    int slot;
    bool enabled;
    std::chrono::steady_clock::time_point t0{};
    explicit SchedSpan(int s) : slot(s), enabled(ExecutorAndroidRendererProbesEnabled()) {
        if (enabled) {
            t0 = std::chrono::steady_clock::now();
        }
    }
    ~SchedSpan() {
        if (!enabled) {
            return;
        }
        const auto dt = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        g_sched_probe.ns[slot].fetch_add(dt, std::memory_order_relaxed);
        g_sched_probe.hits[slot].fetch_add(1, std::memory_order_relaxed);
        if ((g_sched_probe.ticks.fetch_add(1, std::memory_order_relaxed) % 2048) == 0) {
            if (FILE* out =
                    std::fopen("/data/data/app.lsx4.android/files/runtime/sched-probe.txt", "w")) {
                for (int i = 0; i < 4; ++i) {
                    const auto h = g_sched_probe.hits[i].load(std::memory_order_relaxed);
                    std::fprintf(out, "%-16s calls=%llu total_ms=%.1f avg_us=%.1f",
                                 kSchedProbeName[i], (unsigned long long)h,
                                 double(g_sched_probe.ns[i].load(std::memory_order_relaxed)) / 1e6,
                                 h ? double(g_sched_probe.ns[i].load(std::memory_order_relaxed)) /
                                         h / 1e3
                                   : 0.0);
                    std::fputc(10, out);
                }
                std::fclose(out);
            }
        }
    }
};
} // namespace
#endif

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
namespace {

enum class ExecutorVkJournalOp : u32 {
    GraphicsDraw = 1,
    ComputeDispatch = 2,
    SubmitBegin = 3,
    SubmitComplete = 4,
    Presenter = 5,
};

struct ExecutorVkJournalEntry {
    std::atomic<u64> sequence{};
    ExecutorVkJournalOp op{};
    u64 stream_id{};
    u64 tick{};
    u64 pipeline_key_hash{};
    u64 pipeline_handle{};
    u64 vs_hash{};
    u64 ps_hash{};
    u64 cs_hash{};
    u64 address0{};
    u64 address1{};
    u64 color_address{};
    u64 depth_address{};
    u64 attachment_signature{};
    u32 op_variant{};
    u32 primitive_type{};
    u32 element_count{};
    u32 instance_count{};
    u32 max_draw_count{};
    u32 stride{};
    u32 color_width{};
    u32 color_height{};
    u32 color_format{};
    u32 depth_format{};
    u32 color_attachment_count{};
    u32 descriptor_writes{};
    u32 buffer_descriptors{};
    u32 image_descriptors{};
    s32 result{};
};

struct ExecutorVkGraphSpan {
    ExecutorVkJournalOp op{};
    u64 stream_id{};
    u64 first_sequence{};
    u64 last_sequence{};
    u64 first_tick{};
    u64 last_tick{};
    u64 attachment_signature{};
    u64 color_address{};
    u64 depth_address{};
    u64 first_pipeline_key_hash{};
    u64 last_pipeline_key_hash{};
    u64 first_vs_hash{};
    u64 last_vs_hash{};
    u64 first_ps_hash{};
    u64 last_ps_hash{};
    u64 first_cs_hash{};
    u64 last_cs_hash{};
    u64 total_elements{};
    u32 operation_count{};
    u32 pipeline_changes{};
    u32 draw_type_mask{};
    u32 max_elements{};
    u32 max_instances{};
    u32 color_width{};
    u32 color_height{};
    u32 color_format{};
    u32 depth_format{};
    u32 color_attachment_count{};
    u32 max_descriptor_writes{};
    u32 max_buffer_descriptors{};
    u32 max_image_descriptors{};
};

constexpr u32 ExecutorVkProbePreSpanCapacity = 12;
constexpr u32 ExecutorVkProbePostPrefixCapacity = 16;
constexpr u32 ExecutorVkProbePostTailCapacity = 16;

struct ExecutorVkProbeTrace {
    u64 stream_id{};
    u64 anchor_sequence{};
    u64 selected_draw_sequence{};
    u64 fragment_hash{};
    std::array<ExecutorVkGraphSpan, ExecutorVkProbePreSpanCapacity> pre_spans{};
    u32 pre_span_count{};
    u32 pre_total_spans{};
    ExecutorVkGraphSpan post_current{};
    bool post_current_valid{};
    std::array<ExecutorVkGraphSpan, ExecutorVkProbePostPrefixCapacity> post_prefix{};
    u32 post_prefix_count{};
    std::array<ExecutorVkGraphSpan, ExecutorVkProbePostTailCapacity> post_tail{};
    u32 post_tail_count{};
    u32 post_tail_next{};
    u32 post_total_spans{};
    u64 post_operations{};
};

constexpr u64 ExecutorVkJournalCapacity = 1024;
std::array<ExecutorVkJournalEntry, ExecutorVkJournalCapacity> executor_vk_journal{};
std::atomic<u64> executor_vk_journal_sequence{0};
std::atomic<bool> executor_vk_forced_session{false};
std::atomic<bool> executor_vk_frame_capture_active{false};
std::atomic_flag executor_vk_journal_dumping = ATOMIC_FLAG_INIT;
constexpr u32 ExecutorVkProbeIdle = 0;
constexpr u32 ExecutorVkProbeInitializing = 1;
constexpr u32 ExecutorVkProbeArmed = 2;
constexpr u32 ExecutorVkProbeDumping = 3;
constexpr u32 ExecutorVkProbeDone = 4;
std::atomic<u32> executor_vk_probe_state{ExecutorVkProbeIdle};
std::atomic_flag executor_vk_probe_lock = ATOMIC_FLAG_INIT;
ExecutorVkProbeTrace executor_vk_probe_trace{};

constexpr u32 ExecutorVkFramePrefixCapacity = 64;
constexpr u32 ExecutorVkFrameTailCapacity = 64;
struct ExecutorVkFrameTrace {
    u64 epoch{};
    u64 first_sequence{};
    u64 last_sequence{};
    u64 operations{};
    ExecutorVkGraphSpan current{};
    bool current_valid{};
    std::array<ExecutorVkGraphSpan, ExecutorVkFramePrefixCapacity> prefix{};
    u32 prefix_count{};
    std::array<ExecutorVkGraphSpan, ExecutorVkFrameTailCapacity> tail{};
    u32 tail_count{};
    u32 tail_next{};
    u32 total_spans{};
};
constexpr u32 ExecutorVkFrameIdle = 0;
constexpr u32 ExecutorVkFrameArmed = 1;
constexpr u32 ExecutorVkFrameDumping = 2;
std::atomic<u32> executor_vk_frame_state{ExecutorVkFrameIdle};
std::atomic_flag executor_vk_frame_lock = ATOMIC_FLAG_INIT;
ExecutorVkFrameTrace executor_vk_frame_trace{};

void ExecutorVkProbeRecord(const ExecutorVkJournalEntry& entry, u64 sequence) noexcept;
void ExecutorVkFrameRecord(const ExecutorVkJournalEntry& entry, u64 sequence) noexcept;

bool ExecutorVkPersistentJournalRequested() {
    static const bool persistent_journal = std::getenv("EXECUTOR_VK_JOURNAL") != nullptr;
    return persistent_journal;
}

bool ExecutorVkJournalRecordEnabled(bool force_record) noexcept {
    if (force_record) {
        executor_vk_forced_session.store(true, std::memory_order_release);
    }
    return ExecutorVkJournalWouldRecord(force_record);
}

template <typename Fill>
u64 ExecutorVkJournalWrite(ExecutorVkJournalOp op, bool force_record, Fill&& fill) noexcept {
    if (!ExecutorVkJournalRecordEnabled(force_record)) [[likely]] {
        return 0;
    }
    const u64 sequence = executor_vk_journal_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    auto& entry = executor_vk_journal[(sequence - 1) % ExecutorVkJournalCapacity];
    entry.sequence.store(0, std::memory_order_relaxed);
    entry.op = op;
    fill(entry);
    entry.sequence.store(sequence, std::memory_order_release);
    ExecutorVkProbeRecord(entry, sequence);
    ExecutorVkFrameRecord(entry, sequence);
    return sequence;
}

bool ExecutorVkIsGraphOperation(const ExecutorVkJournalEntry& entry) noexcept {
    return entry.op == ExecutorVkJournalOp::GraphicsDraw ||
           entry.op == ExecutorVkJournalOp::ComputeDispatch;
}

ExecutorVkGraphSpan ExecutorVkMakeGraphSpan(const ExecutorVkJournalEntry& entry,
                                            u64 sequence) noexcept {
    ExecutorVkGraphSpan span{};
    span.op = entry.op;
    span.stream_id = entry.stream_id;
    span.first_sequence = sequence;
    span.last_sequence = sequence;
    span.first_tick = entry.tick;
    span.last_tick = entry.tick;
    span.attachment_signature = entry.attachment_signature;
    span.color_address = entry.color_address;
    span.depth_address = entry.depth_address;
    span.first_pipeline_key_hash = entry.pipeline_key_hash;
    span.last_pipeline_key_hash = entry.pipeline_key_hash;
    span.first_vs_hash = entry.vs_hash;
    span.last_vs_hash = entry.vs_hash;
    span.first_ps_hash = entry.ps_hash;
    span.last_ps_hash = entry.ps_hash;
    span.first_cs_hash = entry.cs_hash;
    span.last_cs_hash = entry.cs_hash;
    span.total_elements = entry.element_count;
    span.operation_count = 1;
    span.draw_type_mask = entry.op_variant < 32 ? 1U << entry.op_variant : 0;
    span.max_elements = entry.element_count;
    span.max_instances = entry.instance_count;
    span.color_width = entry.color_width;
    span.color_height = entry.color_height;
    span.color_format = entry.color_format;
    span.depth_format = entry.depth_format;
    span.color_attachment_count = entry.color_attachment_count;
    span.max_descriptor_writes = entry.descriptor_writes;
    span.max_buffer_descriptors = entry.buffer_descriptors;
    span.max_image_descriptors = entry.image_descriptors;
    return span;
}

bool ExecutorVkSameGraphSpan(const ExecutorVkGraphSpan& span,
                             const ExecutorVkJournalEntry& entry) noexcept {
    if (span.op != entry.op) {
        return false;
    }
    if (span.stream_id != entry.stream_id) {
        return false;
    }
    if (entry.op == ExecutorVkJournalOp::ComputeDispatch) {
        return span.last_cs_hash == entry.cs_hash;
    }
    return span.attachment_signature == entry.attachment_signature &&
           span.color_address == entry.color_address && span.depth_address == entry.depth_address &&
           span.color_width == entry.color_width && span.color_height == entry.color_height &&
           span.color_format == entry.color_format && span.depth_format == entry.depth_format &&
           span.color_attachment_count == entry.color_attachment_count;
}

void ExecutorVkExtendGraphSpan(ExecutorVkGraphSpan& span, const ExecutorVkJournalEntry& entry,
                               u64 sequence) noexcept {
    if (span.last_pipeline_key_hash != entry.pipeline_key_hash) {
        ++span.pipeline_changes;
    }
    span.last_sequence = sequence;
    span.last_tick = entry.tick;
    span.last_pipeline_key_hash = entry.pipeline_key_hash;
    span.last_vs_hash = entry.vs_hash;
    span.last_ps_hash = entry.ps_hash;
    span.last_cs_hash = entry.cs_hash;
    span.total_elements += entry.element_count;
    ++span.operation_count;
    if (entry.op_variant < 32) {
        span.draw_type_mask |= 1U << entry.op_variant;
    }
    span.max_elements = std::max(span.max_elements, entry.element_count);
    span.max_instances = std::max(span.max_instances, entry.instance_count);
    span.max_descriptor_writes = std::max(span.max_descriptor_writes, entry.descriptor_writes);
    span.max_buffer_descriptors = std::max(span.max_buffer_descriptors, entry.buffer_descriptors);
    span.max_image_descriptors = std::max(span.max_image_descriptors, entry.image_descriptors);
}

void ExecutorVkProbeLock() noexcept {
    while (executor_vk_probe_lock.test_and_set(std::memory_order_acquire)) {
    }
}

void ExecutorVkProbeUnlock() noexcept {
    executor_vk_probe_lock.clear(std::memory_order_release);
}

void ExecutorVkFrameLock() noexcept {
    while (executor_vk_frame_lock.test_and_set(std::memory_order_acquire)) {
    }
}

void ExecutorVkFrameUnlock() noexcept {
    executor_vk_frame_lock.clear(std::memory_order_release);
}

void ExecutorVkFrameStoreSpan(const ExecutorVkGraphSpan& span) noexcept {
    auto& trace = executor_vk_frame_trace;
    ++trace.total_spans;
    if (trace.prefix_count < trace.prefix.size()) {
        trace.prefix[trace.prefix_count++] = span;
        return;
    }
    trace.tail[trace.tail_next] = span;
    trace.tail_next = (trace.tail_next + 1) % trace.tail.size();
    trace.tail_count = std::min<u32>(trace.tail_count + 1, static_cast<u32>(trace.tail.size()));
}

void ExecutorVkFrameFinishCurrentSpan() noexcept {
    auto& trace = executor_vk_frame_trace;
    if (!trace.current_valid) {
        return;
    }
    ExecutorVkFrameStoreSpan(trace.current);
    trace.current = {};
    trace.current_valid = false;
}

void ExecutorVkFrameRecord(const ExecutorVkJournalEntry& entry, u64 sequence) noexcept {
    if (executor_vk_frame_state.load(std::memory_order_acquire) != ExecutorVkFrameArmed ||
        !ExecutorVkIsGraphOperation(entry)) {
        return;
    }

    ExecutorVkFrameLock();
    auto& trace = executor_vk_frame_trace;
    if (executor_vk_frame_state.load(std::memory_order_relaxed) != ExecutorVkFrameArmed) {
        ExecutorVkFrameUnlock();
        return;
    }
    if (trace.first_sequence == 0) {
        trace.first_sequence = sequence;
    }
    trace.last_sequence = sequence;
    ++trace.operations;
    if (!trace.current_valid) {
        trace.current = ExecutorVkMakeGraphSpan(entry, sequence);
        trace.current_valid = true;
    } else if (ExecutorVkSameGraphSpan(trace.current, entry)) {
        ExecutorVkExtendGraphSpan(trace.current, entry, sequence);
    } else {
        ExecutorVkFrameFinishCurrentSpan();
        trace.current = ExecutorVkMakeGraphSpan(entry, sequence);
        trace.current_valid = true;
    }
    ExecutorVkFrameUnlock();
}

void ExecutorVkProbeStorePostSpan(const ExecutorVkGraphSpan& span) noexcept {
    auto& trace = executor_vk_probe_trace;
    ++trace.post_total_spans;
    if (trace.post_prefix_count < trace.post_prefix.size()) {
        trace.post_prefix[trace.post_prefix_count++] = span;
        return;
    }
    trace.post_tail[trace.post_tail_next] = span;
    trace.post_tail_next = (trace.post_tail_next + 1) % trace.post_tail.size();
    trace.post_tail_count =
        std::min<u32>(trace.post_tail_count + 1, static_cast<u32>(trace.post_tail.size()));
}

void ExecutorVkProbeFinishCurrentPostSpan() noexcept {
    auto& trace = executor_vk_probe_trace;
    if (!trace.post_current_valid) {
        return;
    }
    ExecutorVkProbeStorePostSpan(trace.post_current);
    trace.post_current = {};
    trace.post_current_valid = false;
}

void ExecutorVkProbeRecord(const ExecutorVkJournalEntry& entry, u64 sequence) noexcept {
    if (executor_vk_probe_state.load(std::memory_order_acquire) != ExecutorVkProbeArmed ||
        !ExecutorVkIsGraphOperation(entry)) {
        return;
    }

    ExecutorVkProbeLock();
    auto& trace = executor_vk_probe_trace;
    if (executor_vk_probe_state.load(std::memory_order_relaxed) != ExecutorVkProbeArmed ||
        entry.stream_id != trace.stream_id || sequence <= trace.anchor_sequence) {
        ExecutorVkProbeUnlock();
        return;
    }

    ++trace.post_operations;
    if (!trace.post_current_valid) {
        trace.post_current = ExecutorVkMakeGraphSpan(entry, sequence);
        trace.post_current_valid = true;
    } else if (ExecutorVkSameGraphSpan(trace.post_current, entry)) {
        ExecutorVkExtendGraphSpan(trace.post_current, entry, sequence);
    } else {
        ExecutorVkProbeFinishCurrentPostSpan();
        trace.post_current = ExecutorVkMakeGraphSpan(entry, sequence);
        trace.post_current_valid = true;
    }
    ExecutorVkProbeUnlock();
}

void ExecutorVkProbePrintSpan(const char* phase, u32 index, const ExecutorVkGraphSpan& span,
                              u64 anchor_sequence) noexcept {
    const bool anchor =
        span.first_sequence <= anchor_sequence && anchor_sequence <= span.last_sequence;
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_VK_GRAPH_SPAN] phase=%s index=%u kind=%s stream=0x%llx "
        "seq=%llu-%llu ticks=%llu-%llu "
        "ops=%u anchor=%u attach=0x%llx cb0=0x%llx db=0x%llx colors=%u extent=%ux%u "
        "colorFmt=%u depthFmt=%u firstKey=0x%llx lastKey=0x%llx keyChanges=%u "
        "firstVS=0x%llx lastVS=0x%llx firstPS=0x%llx lastPS=0x%llx "
        "firstCS=0x%llx lastCS=0x%llx drawTypes=0x%x totalElements=%llu "
        "maxElements=%u maxInstances=%u writesMax=%u buffersMax=%u imagesMax=%u",
        phase, index, span.op == ExecutorVkJournalOp::GraphicsDraw ? "draw" : "dispatch",
        static_cast<unsigned long long>(span.stream_id),
        static_cast<unsigned long long>(span.first_sequence),
        static_cast<unsigned long long>(span.last_sequence),
        static_cast<unsigned long long>(span.first_tick),
        static_cast<unsigned long long>(span.last_tick), span.operation_count, anchor ? 1u : 0u,
        static_cast<unsigned long long>(span.attachment_signature),
        static_cast<unsigned long long>(span.color_address),
        static_cast<unsigned long long>(span.depth_address), span.color_attachment_count,
        span.color_width, span.color_height, span.color_format, span.depth_format,
        static_cast<unsigned long long>(span.first_pipeline_key_hash),
        static_cast<unsigned long long>(span.last_pipeline_key_hash), span.pipeline_changes,
        static_cast<unsigned long long>(span.first_vs_hash),
        static_cast<unsigned long long>(span.last_vs_hash),
        static_cast<unsigned long long>(span.first_ps_hash),
        static_cast<unsigned long long>(span.last_ps_hash),
        static_cast<unsigned long long>(span.first_cs_hash),
        static_cast<unsigned long long>(span.last_cs_hash), span.draw_type_mask,
        static_cast<unsigned long long>(span.total_elements), span.max_elements, span.max_instances,
        span.max_descriptor_writes, span.max_buffer_descriptors, span.max_image_descriptors);
}

void ExecutorVkProbeDumpAtSubmit(const void* stream, u64 tick, vk::Result result) noexcept {
    if (executor_vk_probe_state.load(std::memory_order_acquire) != ExecutorVkProbeArmed ||
        reinterpret_cast<u64>(stream) != executor_vk_probe_trace.stream_id) {
        return;
    }
    u32 expected = ExecutorVkProbeArmed;
    if (!executor_vk_probe_state.compare_exchange_strong(expected, ExecutorVkProbeDumping,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
        return;
    }

    ExecutorVkProbeLock();
    ExecutorVkProbeFinishCurrentPostSpan();
    const auto& trace = executor_vk_probe_trace;
    const u32 retained_post = trace.post_prefix_count + trace.post_tail_count;
    const u32 omitted_post =
        trace.post_total_spans > retained_post ? trace.post_total_spans - retained_post : 0;
    const u32 omitted_pre = trace.pre_total_spans > trace.pre_span_count
                                ? trace.pre_total_spans - trace.pre_span_count
                                : 0;
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_VK_GRAPH_BEGIN] stream=0x%llx selectedDraw=%llu selectedPS=0x%llx "
        "anchorSeq=%llu submitTick=%llu submitResult=%d preGroups=%u preOmitted=%u "
        "postGroups=%u postOperations=%llu postOmitted=%u",
        static_cast<unsigned long long>(trace.stream_id),
        static_cast<unsigned long long>(trace.selected_draw_sequence),
        static_cast<unsigned long long>(trace.fragment_hash),
        static_cast<unsigned long long>(trace.anchor_sequence),
        static_cast<unsigned long long>(tick), static_cast<s32>(result), trace.pre_total_spans,
        omitted_pre, trace.post_total_spans, static_cast<unsigned long long>(trace.post_operations),
        omitted_post);

    for (u32 i = 0; i < trace.pre_span_count; ++i) {
        ExecutorVkProbePrintSpan("before", i, trace.pre_spans[i], trace.anchor_sequence);
    }
    for (u32 i = 0; i < trace.post_prefix_count; ++i) {
        ExecutorVkProbePrintSpan("after_head", i, trace.post_prefix[i], trace.anchor_sequence);
    }
    const u32 tail_start =
        trace.post_tail_count == trace.post_tail.size() ? trace.post_tail_next : 0;
    for (u32 i = 0; i < trace.post_tail_count; ++i) {
        const u32 slot = (tail_start + i) % trace.post_tail.size();
        ExecutorVkProbePrintSpan("after_tail", i, trace.post_tail[slot], trace.anchor_sequence);
    }
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_VK_GRAPH_END] stream=0x%llx anchorSeq=%llu submitTick=%llu",
                        static_cast<unsigned long long>(trace.stream_id),
                        static_cast<unsigned long long>(trace.anchor_sequence),
                        static_cast<unsigned long long>(tick));
    ExecutorVkProbeUnlock();
    executor_vk_probe_state.store(ExecutorVkProbeDone, std::memory_order_release);
}

} // namespace

bool ExecutorVkJournalRequested() noexcept {
    return ExecutorVkPersistentJournalRequested();
}

bool ExecutorVkJournalWouldRecord(bool force_record) noexcept {
    if (force_record) {
        return true;
    }
    if (ExecutorVkPersistentJournalRequested()) {
        return true;
    }
    if (executor_vk_forced_session.load(std::memory_order_acquire)) {
        return true;
    }
    return executor_vk_frame_capture_active.load(std::memory_order_acquire);
}

u64 ExecutorVkJournalGraphicsDraw(
    const void* stream, u64 tick, u64 pipeline_key_hash, u64 pipeline_handle, u64 vs_hash,
    u64 ps_hash, ExecutorVkJournalDrawType draw_type, u32 primitive_type, u32 element_count,
    u32 instance_count, u32 max_draw_count, u32 stride, VAddr argument_address, VAddr count_address,
    VAddr color_address, VAddr depth_address, u64 attachment_signature, u32 color_attachment_count,
    u32 color_width, u32 color_height, u32 color_format, u32 depth_format, u32 descriptor_writes,
    u32 buffer_descriptors, u32 image_descriptors, bool force_record) noexcept {
    return ExecutorVkJournalWrite(ExecutorVkJournalOp::GraphicsDraw, force_record,
                                  [&](auto& entry) {
                                      entry.stream_id = reinterpret_cast<u64>(stream);
                                      entry.tick = tick;
                                      entry.pipeline_key_hash = pipeline_key_hash;
                                      entry.pipeline_handle = pipeline_handle;
                                      entry.vs_hash = vs_hash;
                                      entry.ps_hash = ps_hash;
                                      entry.cs_hash = 0;
                                      entry.address0 = argument_address;
                                      entry.address1 = count_address;
                                      entry.op_variant = static_cast<u32>(draw_type);
                                      entry.primitive_type = primitive_type;
                                      entry.element_count = element_count;
                                      entry.instance_count = instance_count;
                                      entry.max_draw_count = max_draw_count;
                                      entry.stride = stride;
                                      entry.color_address = color_address;
                                      entry.depth_address = depth_address;
                                      entry.attachment_signature = attachment_signature;
                                      entry.color_width = color_width;
                                      entry.color_height = color_height;
                                      entry.color_format = color_format;
                                      entry.depth_format = depth_format;
                                      entry.color_attachment_count = color_attachment_count;
                                      entry.descriptor_writes = descriptor_writes;
                                      entry.buffer_descriptors = buffer_descriptors;
                                      entry.image_descriptors = image_descriptors;
                                      entry.result = 0;
                                  });
}

void ExecutorVkJournalArmContentProbe(const void* stream, u64 journal_sequence,
                                      u64 selected_draw_sequence, u64 fragment_hash) noexcept {
    u32 expected = ExecutorVkProbeIdle;
    if (!executor_vk_probe_state.compare_exchange_strong(expected, ExecutorVkProbeInitializing,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
        return;
    }

    const u64 stream_id = reinterpret_cast<u64>(stream);
    if (journal_sequence == 0) {
        executor_vk_probe_state.store(ExecutorVkProbeIdle, std::memory_order_release);
        return;
    }
    const auto& anchor = executor_vk_journal[(journal_sequence - 1) % ExecutorVkJournalCapacity];
    if (anchor.sequence.load(std::memory_order_acquire) != journal_sequence ||
        anchor.stream_id != stream_id || anchor.op != ExecutorVkJournalOp::GraphicsDraw) {
        executor_vk_probe_state.store(ExecutorVkProbeIdle, std::memory_order_release);
        return;
    }

    executor_vk_probe_trace = {};
    auto& trace = executor_vk_probe_trace;
    trace.stream_id = stream_id;
    trace.anchor_sequence = journal_sequence;
    trace.selected_draw_sequence = selected_draw_sequence;
    trace.fragment_hash = fragment_hash;

    const u64 oldest = journal_sequence > ExecutorVkJournalCapacity
                           ? journal_sequence - ExecutorVkJournalCapacity + 1
                           : 1;
    u64 command_buffer_begin = oldest;
    for (u64 sequence = oldest; sequence < journal_sequence; ++sequence) {
        const auto& entry = executor_vk_journal[(sequence - 1) % ExecutorVkJournalCapacity];
        if (entry.sequence.load(std::memory_order_acquire) == sequence &&
            entry.stream_id == stream_id && entry.op == ExecutorVkJournalOp::SubmitComplete) {
            command_buffer_begin = sequence + 1;
        }
    }

    ExecutorVkGraphSpan current{};
    bool current_valid = false;
    const auto store_pre_span = [&](const ExecutorVkGraphSpan& span) {
        ++trace.pre_total_spans;
        if (trace.pre_span_count < trace.pre_spans.size()) {
            trace.pre_spans[trace.pre_span_count++] = span;
            return;
        }
        std::move(trace.pre_spans.begin() + 1, trace.pre_spans.end(), trace.pre_spans.begin());
        trace.pre_spans.back() = span;
    };
    for (u64 sequence = command_buffer_begin; sequence <= journal_sequence; ++sequence) {
        const auto& entry = executor_vk_journal[(sequence - 1) % ExecutorVkJournalCapacity];
        if (entry.sequence.load(std::memory_order_acquire) != sequence ||
            entry.stream_id != stream_id || !ExecutorVkIsGraphOperation(entry)) {
            continue;
        }
        if (!current_valid) {
            current = ExecutorVkMakeGraphSpan(entry, sequence);
            current_valid = true;
        } else if (ExecutorVkSameGraphSpan(current, entry)) {
            ExecutorVkExtendGraphSpan(current, entry, sequence);
        } else {
            store_pre_span(current);
            current = ExecutorVkMakeGraphSpan(entry, sequence);
        }
    }
    if (current_valid) {
        store_pre_span(current);
    }

    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_VK_GRAPH_ARM] stream=0x%llx selectedDraw=%llu selectedPS=0x%llx "
                        "anchorSeq=%llu commandBufferBegin=%llu preGroups=%u retained=%u",
                        static_cast<unsigned long long>(stream_id),
                        static_cast<unsigned long long>(selected_draw_sequence),
                        static_cast<unsigned long long>(fragment_hash),
                        static_cast<unsigned long long>(journal_sequence),
                        static_cast<unsigned long long>(command_buffer_begin),
                        trace.pre_total_spans, trace.pre_span_count);
    executor_vk_probe_state.store(ExecutorVkProbeArmed, std::memory_order_release);
}

void ExecutorVkJournalArmFrameCapture(u64 epoch) noexcept {
    executor_vk_frame_state.store(ExecutorVkFrameDumping, std::memory_order_release);
    ExecutorVkFrameLock();
    executor_vk_frame_trace = {};
    executor_vk_frame_trace.epoch = epoch;
    ExecutorVkFrameUnlock();
    executor_vk_frame_capture_active.store(true, std::memory_order_release);
    executor_vk_frame_state.store(ExecutorVkFrameArmed, std::memory_order_release);
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_VK_FRAME_GRAPH_ARM] epoch=%llu journalNext=%llu boundary=patched_flip",
        static_cast<unsigned long long>(epoch),
        static_cast<unsigned long long>(
            executor_vk_journal_sequence.load(std::memory_order_acquire) + 1));
}

void ExecutorVkJournalFinishFrameCapture(u64 epoch) noexcept {
    u32 expected = ExecutorVkFrameArmed;
    if (!executor_vk_frame_state.compare_exchange_strong(expected, ExecutorVkFrameDumping,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
        return;
    }
    executor_vk_frame_capture_active.store(false, std::memory_order_release);

    ExecutorVkFrameLock();
    ExecutorVkFrameFinishCurrentSpan();
    const auto& trace = executor_vk_frame_trace;
    if (trace.epoch != epoch) {
        ExecutorVkFrameUnlock();
        executor_vk_frame_state.store(ExecutorVkFrameIdle, std::memory_order_release);
        return;
    }
    const u32 retained = trace.prefix_count + trace.tail_count;
    const u32 omitted = trace.total_spans > retained ? trace.total_spans - retained : 0;
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_VK_FRAME_GRAPH_BEGIN] epoch=%llu firstSeq=%llu lastSeq=%llu "
                        "operations=%llu groups=%u retained=%u omitted=%u",
                        static_cast<unsigned long long>(trace.epoch),
                        static_cast<unsigned long long>(trace.first_sequence),
                        static_cast<unsigned long long>(trace.last_sequence),
                        static_cast<unsigned long long>(trace.operations), trace.total_spans,
                        retained, omitted);
    for (u32 i = 0; i < trace.prefix_count; ++i) {
        ExecutorVkProbePrintSpan("frame_head", i, trace.prefix[i], 0);
    }
    const u32 tail_start = trace.tail_count == trace.tail.size() ? trace.tail_next : 0;
    for (u32 i = 0; i < trace.tail_count; ++i) {
        const u32 slot = (tail_start + i) % trace.tail.size();
        ExecutorVkProbePrintSpan("frame_tail", i, trace.tail[slot], 0);
    }
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_VK_FRAME_GRAPH_END] epoch=%llu operations=%llu groups=%u omitted=%u",
        static_cast<unsigned long long>(trace.epoch),
        static_cast<unsigned long long>(trace.operations), trace.total_spans, omitted);
    ExecutorVkFrameUnlock();
    executor_vk_frame_state.store(ExecutorVkFrameIdle, std::memory_order_release);
}

void ExecutorVkJournalComputeDispatch(const void* stream, u64 tick, u64 pipeline_key_hash,
                                      u64 pipeline_handle, u64 cs_hash, bool indirect, u32 dim_x,
                                      u32 dim_y, u32 dim_z, VAddr argument_address,
                                      u32 descriptor_writes, u32 buffer_descriptors,
                                      u32 image_descriptors, bool force_record) noexcept {
    ExecutorVkJournalWrite(ExecutorVkJournalOp::ComputeDispatch, force_record, [&](auto& entry) {
        entry.stream_id = reinterpret_cast<u64>(stream);
        entry.tick = tick;
        entry.pipeline_key_hash = pipeline_key_hash;
        entry.pipeline_handle = pipeline_handle;
        entry.vs_hash = 0;
        entry.ps_hash = 0;
        entry.cs_hash = cs_hash;
        entry.address0 = argument_address;
        entry.address1 = 0;
        entry.op_variant = indirect ? 1U : 0U;
        entry.element_count = dim_x;
        entry.instance_count = dim_y;
        entry.max_draw_count = dim_z;
        entry.stride = 0;
        entry.color_address = 0;
        entry.depth_address = 0;
        entry.attachment_signature = 0;
        entry.color_width = 0;
        entry.color_height = 0;
        entry.color_format = 0;
        entry.depth_format = 0;
        entry.color_attachment_count = 0;
        entry.descriptor_writes = descriptor_writes;
        entry.buffer_descriptors = buffer_descriptors;
        entry.image_descriptors = image_descriptors;
        entry.result = 0;
    });
}

void ExecutorVkJournalPresenter(const void* stream, const u64 tick,
                                const ExecutorVkJournalPresenterStage stage, const u32 frame_id,
                                const u32 source_width, const u32 source_height,
                                const u32 destination_width, const u32 destination_height,
                                const u32 flags) noexcept {
    ExecutorVkJournalWrite(ExecutorVkJournalOp::Presenter, false, [&](auto& entry) {
        entry.stream_id = reinterpret_cast<u64>(stream);
        entry.tick = tick;
        entry.pipeline_key_hash = 0;
        entry.pipeline_handle = 0;
        entry.vs_hash = 0;
        entry.ps_hash = 0;
        entry.cs_hash = 0;
        entry.address0 = 0;
        entry.address1 = 0;
        entry.color_address = 0;
        entry.depth_address = 0;
        entry.attachment_signature = 0;
        entry.op_variant = static_cast<u32>(stage);
        entry.primitive_type = frame_id;
        entry.element_count = source_width;
        entry.instance_count = source_height;
        entry.max_draw_count = destination_width;
        entry.stride = destination_height;
        entry.color_width = 0;
        entry.color_height = 0;
        entry.color_format = 0;
        entry.depth_format = 0;
        entry.color_attachment_count = 0;
        entry.descriptor_writes = 0;
        entry.buffer_descriptors = 0;
        entry.image_descriptors = 0;
        entry.result = static_cast<s32>(flags);
    });
}

void ExecutorVkJournalSubmit(const void* stream, u64 tick, bool completed,
                             vk::Result result) noexcept {
    ExecutorVkJournalWrite(completed ? ExecutorVkJournalOp::SubmitComplete
                                     : ExecutorVkJournalOp::SubmitBegin,
                           false, [&](auto& entry) {
                               entry.stream_id = reinterpret_cast<u64>(stream);
                               entry.tick = tick;
                               entry.pipeline_key_hash = 0;
                               entry.pipeline_handle = 0;
                               entry.vs_hash = 0;
                               entry.ps_hash = 0;
                               entry.cs_hash = 0;
                               entry.address0 = 0;
                               entry.address1 = 0;
                               entry.op_variant = 0;
                               entry.primitive_type = 0;
                               entry.element_count = 0;
                               entry.instance_count = 0;
                               entry.max_draw_count = 0;
                               entry.stride = 0;
                               entry.color_address = 0;
                               entry.depth_address = 0;
                               entry.attachment_signature = 0;
                               entry.color_width = 0;
                               entry.color_height = 0;
                               entry.color_format = 0;
                               entry.depth_format = 0;
                               entry.color_attachment_count = 0;
                               entry.descriptor_writes = 0;
                               entry.buffer_descriptors = 0;
                               entry.image_descriptors = 0;
                               entry.result = static_cast<s32>(result);
                           });
    if (completed) {
        ExecutorVkProbeDumpAtSubmit(stream, tick, result);
    }
}

void ExecutorVkJournalDump(const char* reason, u64 failing_tick) noexcept {
    if (executor_vk_journal_dumping.test_and_set(std::memory_order_acq_rel)) {
        return;
    }

    const u64 newest = executor_vk_journal_sequence.load(std::memory_order_acquire);
    constexpr u64 DumpEntries = ExecutorVkJournalCapacity;
    const u64 oldest = newest > DumpEntries ? newest - DumpEntries + 1 : 1;
    std::FILE* journal_file{};
    if (const char* path = std::getenv("EXECUTOR_VK_JOURNAL_FILE");
        path != nullptr && path[0] != '\0') {
        journal_file = std::fopen(path, "w");
        if (journal_file != nullptr) {
            std::fprintf(journal_file, "reason=%s failingTick=%llu oldest=%llu newest=%llu\n",
                         reason ? reason : "unknown", static_cast<unsigned long long>(failing_tick),
                         static_cast<unsigned long long>(oldest),
                         static_cast<unsigned long long>(newest));
        }
    }
    __android_log_print(ANDROID_LOG_ERROR, "LSX4Native",
                        "[EXECUTOR_VK_JOURNAL_BEGIN] reason=%s failingTick=%llu oldest=%llu "
                        "newest=%llu",
                        reason ? reason : "unknown", static_cast<unsigned long long>(failing_tick),
                        static_cast<unsigned long long>(oldest),
                        static_cast<unsigned long long>(newest));

    for (u64 sequence = oldest; sequence <= newest; ++sequence) {
        const auto& entry = executor_vk_journal[(sequence - 1) % ExecutorVkJournalCapacity];
        if (entry.sequence.load(std::memory_order_acquire) != sequence) {
            continue;
        }
        if (journal_file != nullptr) {
            std::fprintf(
                journal_file,
                "seq=%llu op=%u stream=0x%llx tick=%llu result=%d variant=%u prim=%u "
                "key=0x%llx pipe=0x%llx vs=0x%llx ps=0x%llx cs=0x%llx "
                "elements=%u instances=%u maxDraws=%u stride=%u "
                "addr0=0x%llx addr1=0x%llx attach=0x%llx cb0=0x%llx db=0x%llx "
                "colors=%u extent=%ux%u colorFmt=%u depthFmt=%u "
                "writes=%u buffers=%u images=%u\n",
                static_cast<unsigned long long>(sequence), static_cast<u32>(entry.op),
                static_cast<unsigned long long>(entry.stream_id),
                static_cast<unsigned long long>(entry.tick), entry.result, entry.op_variant,
                entry.primitive_type, static_cast<unsigned long long>(entry.pipeline_key_hash),
                static_cast<unsigned long long>(entry.pipeline_handle),
                static_cast<unsigned long long>(entry.vs_hash),
                static_cast<unsigned long long>(entry.ps_hash),
                static_cast<unsigned long long>(entry.cs_hash), entry.element_count,
                entry.instance_count, entry.max_draw_count, entry.stride,
                static_cast<unsigned long long>(entry.address0),
                static_cast<unsigned long long>(entry.address1),
                static_cast<unsigned long long>(entry.attachment_signature),
                static_cast<unsigned long long>(entry.color_address),
                static_cast<unsigned long long>(entry.depth_address), entry.color_attachment_count,
                entry.color_width, entry.color_height, entry.color_format, entry.depth_format,
                entry.descriptor_writes, entry.buffer_descriptors, entry.image_descriptors);
        }
        if (entry.op == ExecutorVkJournalOp::GraphicsDraw) {
            __android_log_print(
                ANDROID_LOG_ERROR, "LSX4Native",
                "[EXECUTOR_VK_JOURNAL_DRAW] seq=%llu stream=0x%llx tick=%llu type=%u prim=%u "
                "key=0x%llx "
                "pipe=0x%llx vs=0x%llx ps=0x%llx elements=%u instances=%u "
                "maxDraws=%u stride=%u args=0x%llx countAddr=0x%llx attach=0x%llx "
                "cb0=0x%llx db=0x%llx colors=%u extent=%ux%u colorFmt=%u depthFmt=%u "
                "writes=%u buffers=%u images=%u",
                static_cast<unsigned long long>(sequence),
                static_cast<unsigned long long>(entry.stream_id),
                static_cast<unsigned long long>(entry.tick), entry.op_variant, entry.primitive_type,
                static_cast<unsigned long long>(entry.pipeline_key_hash),
                static_cast<unsigned long long>(entry.pipeline_handle),
                static_cast<unsigned long long>(entry.vs_hash),
                static_cast<unsigned long long>(entry.ps_hash), entry.element_count,
                entry.instance_count, entry.max_draw_count, entry.stride,
                static_cast<unsigned long long>(entry.address0),
                static_cast<unsigned long long>(entry.address1),
                static_cast<unsigned long long>(entry.attachment_signature),
                static_cast<unsigned long long>(entry.color_address),
                static_cast<unsigned long long>(entry.depth_address), entry.color_attachment_count,
                entry.color_width, entry.color_height, entry.color_format, entry.depth_format,
                entry.descriptor_writes, entry.buffer_descriptors, entry.image_descriptors);
        } else if (entry.op == ExecutorVkJournalOp::ComputeDispatch) {
            __android_log_print(
                ANDROID_LOG_ERROR, "LSX4Native",
                "[EXECUTOR_VK_JOURNAL_DISPATCH] seq=%llu stream=0x%llx tick=%llu indirect=%u "
                "key=0x%llx "
                "pipe=0x%llx cs=0x%llx dims=%ux%ux%u args=0x%llx writes=%u buffers=%u "
                "images=%u",
                static_cast<unsigned long long>(sequence),
                static_cast<unsigned long long>(entry.stream_id),
                static_cast<unsigned long long>(entry.tick), entry.op_variant,
                static_cast<unsigned long long>(entry.pipeline_key_hash),
                static_cast<unsigned long long>(entry.pipeline_handle),
                static_cast<unsigned long long>(entry.cs_hash), entry.element_count,
                entry.instance_count, entry.max_draw_count,
                static_cast<unsigned long long>(entry.address0), entry.descriptor_writes,
                entry.buffer_descriptors, entry.image_descriptors);
        } else if (entry.op == ExecutorVkJournalOp::Presenter) {
            __android_log_print(ANDROID_LOG_ERROR, "LSX4Native",
                                "[EXECUTOR_VK_JOURNAL_PRESENTER] seq=%llu stream=0x%llx tick=%llu "
                                "stage=%u frame=%u src=%ux%u dst=%ux%u flags=0x%x",
                                static_cast<unsigned long long>(sequence),
                                static_cast<unsigned long long>(entry.stream_id),
                                static_cast<unsigned long long>(entry.tick), entry.op_variant,
                                entry.primitive_type, entry.element_count, entry.instance_count,
                                entry.max_draw_count, entry.stride, static_cast<u32>(entry.result));
        } else {
            __android_log_print(
                ANDROID_LOG_ERROR, "LSX4Native",
                "[EXECUTOR_VK_JOURNAL_SUBMIT] seq=%llu stream=0x%llx tick=%llu phase=%s result=%d",
                static_cast<unsigned long long>(sequence),
                static_cast<unsigned long long>(entry.stream_id),
                static_cast<unsigned long long>(entry.tick),
                entry.op == ExecutorVkJournalOp::SubmitComplete ? "complete" : "begin",
                entry.result);
        }
    }
    __android_log_print(ANDROID_LOG_ERROR, "LSX4Native", "[EXECUTOR_VK_JOURNAL_END] newest=%llu",
                        static_cast<unsigned long long>(newest));
    if (journal_file != nullptr) {
        std::fclose(journal_file);
    }
}
#endif

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
namespace {

struct ExecutorRenderScopeTelemetry {
    const Scheduler* owner{};
    std::array<u64, static_cast<std::size_t>(Scheduler::EndRenderingReason::Count)>
        end_reason_counts{};
    std::array<u64, static_cast<std::size_t>(Scheduler::EndRenderingReason::Count)>
        same_state_reopen_counts{};
    RenderState last_ended_render_state{};
    Scheduler::EndRenderingReason last_end_reason{Scheduler::EndRenderingReason::Unknown};
    u64 begin_count{};
    u64 end_count{};
    u64 same_state_reopen_count{};
    u64 consumed_clear_reuse_count{};
    bool has_last_ended_render_state{};
};

bool HasRenderClear(const RenderState& state) noexcept {
    for (u32 index = 0; index < state.num_color_attachments; ++index) {
        if (state.color_attachments[index].is_clear != 0) {
            return true;
        }
    }
    return state.depth_stencil_attachment.depth_clear ||
           state.depth_stencil_attachment.stencil_clear;
}

bool CanReuseAfterConsumedClear(const RenderState& active, const RenderState& requested) noexcept {
    if (!HasRenderClear(active) || HasRenderClear(requested)) {
        return false;
    }

    RenderState normalized_active = active;
    RenderState normalized_requested = requested;
    for (u32 index = 0; index < normalized_active.color_attachments.size(); ++index) {
        normalized_active.color_attachments[index].is_clear = 0;
        normalized_active.color_attachments[index].clear_value = {};
        normalized_requested.color_attachments[index].is_clear = 0;
        normalized_requested.color_attachments[index].clear_value = {};
    }
    normalized_active.depth_stencil_attachment.depth_clear = false;
    normalized_active.depth_stencil_attachment.stencil_clear = false;
    normalized_active.depth_stencil_attachment.clear_value = {};
    normalized_requested.depth_stencil_attachment.depth_clear = false;
    normalized_requested.depth_stencil_attachment.stencil_clear = false;
    normalized_requested.depth_stencil_attachment.clear_value = {};
    return normalized_active == normalized_requested;
}

ExecutorRenderScopeTelemetry& RenderScopeTelemetryFor(const Scheduler* owner) {
    static thread_local ExecutorRenderScopeTelemetry telemetry{};
    if (telemetry.owner != owner) {
        telemetry = {};
        telemetry.owner = owner;
    }
    return telemetry;
}

} // namespace
#endif

std::mutex Scheduler::submit_mutex;

Scheduler::Scheduler(const Instance& instance, const GpuTimelineSchedulerKind timeline_kind)
    : instance{instance}, master_semaphore{instance}, command_pool{instance, &master_semaphore} {
    PortableGpu::ResetProducerTimeline(this);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    gpu_timeline_kind = timeline_kind;
#else
    static_cast<void>(timeline_kind);
#endif
    if (instance.MaxPushDescriptors() == 0) {
        static constexpr std::array descriptor_pool_sizes = {
            vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 1024},
            vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 4096},
            vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 1024},
            vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1024},
            vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 1024},
        };
        legacy_descriptor_heap =
            std::make_unique<DescriptorHeap>(instance, &master_semaphore, descriptor_pool_sizes);
    }
#if TRACY_GPU_ENABLED
    profiler_scope = reinterpret_cast<tracy::VkCtxScope*>(std::malloc(sizeof(tracy::VkCtxScope)));
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    InitializeGpuTimestampRing();
    if (gpu_timestamp_enabled) {
        std::scoped_lock lock{g_gpu_timestamp_scheduler_registry_mutex};
        const auto free_slot = std::find(g_gpu_timestamp_schedulers.begin(),
                                         g_gpu_timestamp_schedulers.end(), nullptr);
        if (free_slot != g_gpu_timestamp_schedulers.end()) {
            *free_slot = this;
        } else {
            __android_log_print(
                ANDROID_LOG_WARN, "LSX4Native",
                "[EXECUTOR_GPU_TIMESTAMP] scheduler=%p registry_full=1 capacity=%zu", this,
                g_gpu_timestamp_schedulers.size());
        }
    }
#endif
    AllocateWorkerCommandBuffers();
    priority_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::PriorityPendingOpsThread, this));
    retirement_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::RetirementPendingOpsThread, this));
}

Scheduler::~Scheduler() {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (gpu_timestamp_enabled) {
        std::scoped_lock lock{g_gpu_timestamp_scheduler_registry_mutex};
        const auto slot =
            std::find(g_gpu_timestamp_schedulers.begin(), g_gpu_timestamp_schedulers.end(), this);
        if (slot != g_gpu_timestamp_schedulers.end()) {
            *slot = nullptr;
        }
    }
    if (pending_submit_thread.joinable()) {
        pending_submit_thread.request_stop();
        pending_submit_cv.notify_all();
        pending_submit_thread.join();
    }
    {
        SubmitMutexProfileLock lk{submit_mutex, gpu_timeline_kind};
        SubmitPendingBatchLocked(nullptr, {}, 0, true);
    }
    const u64 current_tick = master_semaphore.CurrentTick();
    if (current_tick > 1 && !master_semaphore.IsDeviceLost()) {
        master_semaphore.Wait(current_tick - 1);
    }
#endif
    if (priority_pending_ops_thread.joinable()) {
        priority_pending_ops_thread.request_stop();
        priority_pending_ops_cv.notify_all();
        priority_pending_ops_thread.join();
    }
    if (retirement_pending_ops_thread.joinable()) {
        retirement_pending_ops_thread.request_stop();
        retirement_pending_ops_cv.notify_all();
        retirement_pending_ops_thread.join();
    }
#if TRACY_GPU_ENABLED
    std::free(profiler_scope);
#endif
}

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
vk::Result Scheduler::BindSparseBufferMemory(const vk::BindSparseInfo& bind_info) {
    if (master_semaphore.IsDeviceLost()) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        __android_log_print(ANDROID_LOG_ERROR, "LSX4Native",
                            "[EXECUTOR_BDA_SPARSE_FAILURE] "
                            "stage=scheduler-precheck result=%d",
                            static_cast<int>(vk::Result::eErrorDeviceLost));
#endif
        return vk::Result::eErrorDeviceLost;
    }
    vk::Result result = vk::Result::eSuccess;
    {
        SubmitMutexProfileLock lk{submit_mutex, gpu_timeline_kind};
        if (!SubmitPendingBatchLocked(nullptr, {}, 0, true)) {
            const vk::Result flush_result = master_semaphore.IsDeviceLost()
                                                ? vk::Result::eErrorDeviceLost
                                                : vk::Result::eErrorUnknown;
            __android_log_print(ANDROID_LOG_ERROR, "LSX4Native",
                                "[EXECUTOR_BDA_SPARSE_FAILURE] stage=flush-pending result=%d",
                                static_cast<int>(flush_result));
            return flush_result;
        }
        result = instance.GetGraphicsQueue().bindSparse(bind_info, {});
    }
    if (result != vk::Result::eSuccess) {
        MarkTerminal(result, "queue_bind_sparse_buffer_memory");
    }
    return result;
}
#endif

void Scheduler::DeferPriorityOperation(Common::UniqueFunction<void>&& func) {
    if (master_semaphore.IsDeviceLost()) {
        return;
    }
    const u64 gpu_tick = CurrentTick();
    if (Libraries::GnmDriver::ExecutorReplayActive()) {
        std::scoped_lock lk{pending_ops_mutex};
        if (master_semaphore.IsDeviceLost()) {
            return;
        }
        pending_ops.emplace(std::move(func), gpu_tick);
        const u64 previous_count = pending_ops_count.fetch_add(1, std::memory_order_release);
        if (previous_count == 0) {
            pending_ops_poll_sequence.store(0, std::memory_order_relaxed);
        }
        return;
    }

    {
        std::unique_lock lk(priority_pending_ops_mutex);
        if (master_semaphore.IsDeviceLost()) {
            return;
        }
        priority_pending_ops.emplace(std::move(func), gpu_tick);
    }
    priority_pending_ops_cv.notify_one();
}

void Scheduler::DeferPriorityOperationAt(u64 gpu_tick, Common::UniqueFunction<void>&& func) {
    if (master_semaphore.IsDeviceLost()) {
        return;
    }
    if (Libraries::GnmDriver::ExecutorReplayActive()) {
        std::scoped_lock lk{pending_ops_mutex};
        if (master_semaphore.IsDeviceLost()) {
            return;
        }
        pending_ops.emplace(std::move(func), gpu_tick);
        const u64 previous_count = pending_ops_count.fetch_add(1, std::memory_order_release);
        if (previous_count == 0) {
            pending_ops_poll_sequence.store(0, std::memory_order_relaxed);
        }
        return;
    }

    bool notify_retirement_worker = false;
    {
        std::unique_lock lk(retirement_pending_ops_mutex);
        if (master_semaphore.IsDeviceLost()) {
            return;
        }
        notify_retirement_worker =
            retirement_pending_ops.empty() ||
            gpu_tick <= retirement_last_published_tick.load(std::memory_order_acquire);
        retirement_pending_ops[gpu_tick].emplace(std::move(func), gpu_tick);
        ++retirement_outstanding_ops[gpu_tick];
        retirement_outstanding_count.fetch_add(1, std::memory_order_release);
    }
    if (notify_retirement_worker) {
        retirement_pending_ops_cv.notify_all();
    }
}

void Scheduler::MarkTerminal(vk::Result result, const char* where) noexcept {
    master_semaphore.MarkTerminal(result, where);
    {
        std::scoped_lock lk{retirement_pending_ops_mutex, priority_pending_ops_mutex};
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    pending_submit_cv.notify_all();
#endif
    priority_pending_ops_cv.notify_all();
    retirement_pending_ops_cv.notify_all();
    event_cv.notify_all();
}

Scheduler::LegacyRenderScope Scheduler::CreateLegacyRenderScope(const RenderState& state) {
    std::array<vk::AttachmentDescription, 9> descriptions{};
    std::array<vk::AttachmentReference, 8> color_references{};
    std::array<vk::ImageView, 9> image_views{};
    std::array<vk::ClearValue, 9> clear_values{};
    u32 attachment_count = 0;
    u32 legacy_color_attachment_count = state.num_color_attachments;
    if (instance.GetCapabilityProfile().ShouldCompactLegacyUnusedColorAttachments()) {
        u32 defined_color_mask = 0;
        for (u32 index = 0; index < state.num_color_attachments; ++index) {
            if (state.color_attachments[index].format != vk::Format::eUndefined) {
                defined_color_mask |= 1U << index;
            }
        }
        legacy_color_attachment_count =
            CompactTrailingAttachmentCount(state.num_color_attachments, defined_color_mask);
    }
    for (u32 index = 0; index < state.num_color_attachments; ++index) {
        const auto& color = state.color_attachments[index];
        if (!color.image_view || color.format == vk::Format::eUndefined) {
            color_references[index] = vk::AttachmentReference{
                .attachment = VK_ATTACHMENT_UNUSED,
                .layout = vk::ImageLayout::eUndefined,
            };
            continue;
        }
        const u32 attachment = attachment_count++;
        descriptions[attachment] = vk::AttachmentDescription{
            .format = color.format,
            .samples = color.samples == vk::SampleCountFlagBits{} ? vk::SampleCountFlagBits::e1
                                                                  : color.samples,
            .loadOp = color.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
            .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
            .initialLayout = color.image_layout,
            .finalLayout = color.image_layout,
        };
        color_references[index] = vk::AttachmentReference{
            .attachment = attachment,
            .layout = color.image_layout,
        };
        image_views[attachment] = color.image_view;
        clear_values[attachment] = vk::ClearValue{
            .color = vk::ClearColorValue{.uint32 = color.clear_value},
        };
    }

    const auto& depth = state.depth_stencil_attachment;
    vk::AttachmentReference depth_reference{
        .attachment = VK_ATTACHMENT_UNUSED,
        .layout = depth.image_layout,
    };
    if ((depth.has_depth || depth.has_stencil) && depth.image_view &&
        depth.format != vk::Format::eUndefined) {
        const u32 attachment = attachment_count++;
        descriptions[attachment] = vk::AttachmentDescription{
            .format = depth.format,
            .samples = depth.samples == vk::SampleCountFlagBits{} ? vk::SampleCountFlagBits::e1
                                                                  : depth.samples,
            .loadOp = !depth.has_depth ? vk::AttachmentLoadOp::eDontCare
                                       : (depth.depth_clear ? vk::AttachmentLoadOp::eClear
                                                            : vk::AttachmentLoadOp::eLoad),
            .storeOp =
                depth.has_depth ? vk::AttachmentStoreOp::eStore : vk::AttachmentStoreOp::eDontCare,
            .stencilLoadOp = !depth.has_stencil
                                 ? vk::AttachmentLoadOp::eDontCare
                                 : (depth.stencil_clear ? vk::AttachmentLoadOp::eClear
                                                        : vk::AttachmentLoadOp::eLoad),
            .stencilStoreOp = depth.has_stencil ? vk::AttachmentStoreOp::eStore
                                                : vk::AttachmentStoreOp::eDontCare,
            .initialLayout = depth.image_layout,
            .finalLayout = depth.image_layout,
        };
        depth_reference.attachment = attachment;
        image_views[attachment] = depth.image_view;
        clear_values[attachment] = vk::ClearValue{
            .depthStencil =
                vk::ClearDepthStencilValue{
                    .depth = std::bit_cast<float>(depth.clear_value[0]),
                    .stencil = depth.clear_value[1],
                },
        };
    }

    const vk::SubpassDescription subpass{
        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
        .colorAttachmentCount = legacy_color_attachment_count,
        .pColorAttachments = legacy_color_attachment_count != 0 ? color_references.data() : nullptr,
        .pDepthStencilAttachment =
            depth_reference.attachment != VK_ATTACHMENT_UNUSED ? &depth_reference : nullptr,
    };
    const vk::RenderPassCreateInfo render_pass_info{
        .attachmentCount = attachment_count,
        .pAttachments = descriptions.data(),
        .subpassCount = 1,
        .pSubpasses = &subpass,
    };
    auto [render_pass_result, render_pass] =
        instance.GetDevice().createRenderPass(render_pass_info);
    ASSERT_MSG(render_pass_result == vk::Result::eSuccess,
               "Failed to create legacy render pass: {}", vk::to_string(render_pass_result));

    const u32 framebuffer_layers = attachment_count == 0 ? 1u : std::max<u32>(state.num_layers, 1u);
    const vk::FramebufferCreateInfo framebuffer_info{
        .renderPass = render_pass,
        .attachmentCount = attachment_count,
        .pAttachments = image_views.data(),
        .width = std::max<u32>(state.width, 1u),
        .height = std::max<u32>(state.height, 1u),
        .layers = framebuffer_layers,
    };
    auto [framebuffer_result, framebuffer] =
        instance.GetDevice().createFramebuffer(framebuffer_info);
    ASSERT_MSG(framebuffer_result == vk::Result::eSuccess,
               "Failed to create legacy framebuffer: {}", vk::to_string(framebuffer_result));

    return LegacyRenderScope{
        .render_pass = render_pass,
        .framebuffer = framebuffer,
        .clear_values = clear_values,
        .clear_value_count = attachment_count,
    };
}

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
void Scheduler::InitializeGpuTimestampRing() {
    gpu_fault_journal_enabled =
        instance.GetCapabilityProfile().exact_proprietary_adreno750 && ExecutorVkJournalRequested();
    constexpr const char* DrawCheckpointMarker = "/data/data/app.lsx4.android/files/lsx4-home/"
                                                 "run-a750-vk-draw-checkpoints";
    gpu_fault_draw_checkpoints_enabled = gpu_fault_journal_enabled &&
                                         gpu_timeline_kind == GpuTimelineSchedulerKind::GuestDraw &&
                                         ::access(DrawCheckpointMarker, F_OK) == 0;
    constexpr const char* DisableMarker = "/data/data/app.lsx4.android/files/lsx4-home/"
                                          "run-bisect-disable-vk-gpu-timestamps";
    const bool explicitly_disabled =
        std::getenv("EXECUTOR_ANDROID_DISABLE_GPU_TIMESTAMPS") != nullptr ||
        ::access(DisableMarker, F_OK) == 0;
    const bool allowed =
        !explicitly_disabled && (ExecutorAndroidRendererProbesEnabled() ||
                                 ExecutorAndroidGpuTimelineEnabled() || gpu_fault_journal_enabled);
    if (!allowed) {
        __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                            "[EXECUTOR_GPU_TIMESTAMP] scheduler=%p enabled=0 "
                            "reason=%s exactJournal=%u",
                            this, explicitly_disabled ? "disabled" : "gate",
                            gpu_fault_journal_enabled ? 1u : 0u);
        return;
    }

    const auto properties = instance.GetPhysicalDevice().getProperties();
    const auto queue_families = instance.GetPhysicalDevice().getQueueFamilyProperties();
    const u32 queue_family = instance.GetGraphicsQueueFamilyIndex();
    if (queue_family >= queue_families.size() ||
        queue_families[queue_family].timestampValidBits == 0 ||
        properties.limits.timestampPeriod <= 0.0F ||
        properties.limits.timestampComputeAndGraphics == VK_FALSE) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_GPU_TIMESTAMP] scheduler=%p enabled=0 reason=unsupported family=%u", this,
            queue_family);
        return;
    }

    const vk::QueryPoolCreateInfo create_info{
        .queryType = vk::QueryType::eTimestamp,
        .queryCount = GpuTimestampPairCount * 2,
    };
    auto [result, query_pool] = instance.GetDevice().createQueryPoolUnique(create_info);
    if (result != vk::Result::eSuccess) {
        __android_log_print(
            ANDROID_LOG_WARN, "LSX4Native",
            "[EXECUTOR_GPU_TIMESTAMP] scheduler=%p enabled=0 reason=create result=%d", this,
            static_cast<int>(result));
        return;
    }

    gpu_timestamp_query_pool = std::move(query_pool);
    if (gpu_fault_draw_checkpoints_enabled) {
        const vk::QueryPoolCreateInfo checkpoint_create_info{
            .queryType = vk::QueryType::eTimestamp,
            .queryCount = GpuTimestampPairCount * GpuFaultCheckpointQueryCount,
        };
        auto [checkpoint_result, checkpoint_pool] =
            instance.GetDevice().createQueryPoolUnique(checkpoint_create_info);
        if (checkpoint_result == vk::Result::eSuccess) {
            gpu_fault_checkpoint_query_pool = std::move(checkpoint_pool);
        } else {
            __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                                "[EXECUTOR_GPU_FAULT_CHECKPOINT] scheduler=%p enabled=0 "
                                "reason=create result=%d",
                                this, static_cast<int>(checkpoint_result));
        }
    }
    gpu_timestamp_valid_bits = std::min<u32>(queue_families[queue_family].timestampValidBits, 64);
    gpu_timestamp_period_ns = static_cast<double>(properties.limits.timestampPeriod);
    gpu_timestamp_enabled = true;
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_GPU_TIMESTAMP] scheduler=%p enabled=1 pairs=%u validBits=%u "
                        "periodNs=%.6f exactJournal=%u drawCheckpoints=%u interval=%u",
                        this, GpuTimestampPairCount, gpu_timestamp_valid_bits,
                        gpu_timestamp_period_ns, gpu_fault_journal_enabled ? 1u : 0u,
                        gpu_fault_checkpoint_query_pool ? 1u : 0u, GpuFaultCheckpointInterval);
}

void Scheduler::CollectGpuTimestampSample() const {
    if (!gpu_timestamp_enabled || !gpu_timestamp_query_pool || gpu_timestamp_submit_ordinal < 2) {
        return;
    }

    std::scoped_lock slot_lock{gpu_timestamp_slots_mutex};
    u32 oldest_slot = InvalidGpuTimestampSlot;
    u64 oldest_ordinal = std::numeric_limits<u64>::max();
    for (u32 slot_index = 0; slot_index < GpuTimestampPairCount; ++slot_index) {
        const auto& slot = gpu_timestamp_slots[slot_index];
        if (slot.state != GpuTimestampSlotState::Pending ||
            slot.submit_ordinal + 2 > gpu_timestamp_submit_ordinal ||
            slot.submit_ordinal >= oldest_ordinal) {
            continue;
        }
        oldest_slot = slot_index;
        oldest_ordinal = slot.submit_ordinal;
    }
    if (oldest_slot == InvalidGpuTimestampSlot) {
        return;
    }

    std::array<u64, 4> values{};
    const vk::QueryResultFlags flags =
        vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability;
    const vk::Result result = instance.GetDevice().getQueryPoolResults(
        *gpu_timestamp_query_pool, oldest_slot * 2, 2, sizeof(values), values.data(),
        sizeof(u64) * 2, flags);
    auto& sampled_slot = gpu_timestamp_slots[oldest_slot];
    sampled_slot.last_query_result = static_cast<s32>(result);
    ++sampled_slot.query_poll_count;
    sampled_slot.last_query_data_valid =
        result == vk::Result::eSuccess || result == vk::Result::eNotReady;
    if (sampled_slot.last_query_data_valid) {
        sampled_slot.last_top_value = values[0];
        sampled_slot.last_top_available = values[1];
        sampled_slot.last_bottom_value = values[2];
        sampled_slot.last_bottom_available = values[3];
    }
    if (result == vk::Result::eNotReady || values[1] == 0 || values[3] == 0) {
        ++gpu_timestamp_not_ready_polls;
        return;
    }
    if (result != vk::Result::eSuccess) {
        gpu_timestamp_enabled = false;
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_GPU_TIMESTAMP] scheduler=%p enabled=0 reason=read result=%d",
                            this, static_cast<int>(result));
        return;
    }

    const u64 valid_mask = gpu_timestamp_valid_bits == 64
                               ? std::numeric_limits<u64>::max()
                               : ((u64{1} << gpu_timestamp_valid_bits) - 1);
    const u64 begin = values[0] & valid_mask;
    const u64 end = values[2] & valid_mask;
    const u64 elapsed_ticks = (end - begin) & valid_mask;
    const u64 frame_epoch = gpu_timestamp_slots[oldest_slot].frame_epoch;
    const u64 submit_ordinal = gpu_timestamp_slots[oldest_slot].submit_ordinal;
    const u64 logical_tick = gpu_timestamp_slots[oldest_slot].logical_tick;
    const u64 physical_submit_sequence = gpu_timestamp_slots[oldest_slot].physical_submit_sequence;
    const u32 physical_submit_index = gpu_timestamp_slots[oldest_slot].physical_submit_index;
    const u64 cpu_record_begin_ns = gpu_timestamp_slots[oldest_slot].cpu_record_begin_ns;
    const u64 cpu_record_end_ns = gpu_timestamp_slots[oldest_slot].cpu_record_end_ns;
    const u64 cpu_submit_begin_ns = gpu_timestamp_slots[oldest_slot].cpu_submit_begin_ns;
    const u64 cpu_submit_end_ns = gpu_timestamp_slots[oldest_slot].cpu_submit_end_ns;
    gpu_timestamp_slots[oldest_slot] = {};

    ++gpu_timestamp_samples;
    ++gpu_timestamp_window_samples;
    gpu_timestamp_window_ticks += elapsed_ticks;
    gpu_timestamp_window_min_ticks = std::min(gpu_timestamp_window_min_ticks, elapsed_ticks);
    gpu_timestamp_window_max_ticks = std::max(gpu_timestamp_window_max_ticks, elapsed_ticks);

    if (ExecutorAndroidGpuTimelineEnabled() || gpu_fault_journal_enabled) {
        RecordGpuTimelineSample({
            .scheduler = this,
            .kind = gpu_timeline_kind,
            .valid_bits = gpu_timestamp_valid_bits,
            .epoch = frame_epoch,
            .submit_ordinal = submit_ordinal,
            .logical_tick = logical_tick,
            .physical_submit_sequence = physical_submit_sequence,
            .physical_submit_index = physical_submit_index,
            .begin = begin,
            .end = end,
            .cpu_record_begin_ns = cpu_record_begin_ns,
            .cpu_record_end_ns = cpu_record_end_ns,
            .cpu_submit_begin_ns = cpu_submit_begin_ns,
            .cpu_submit_end_ns = cpu_submit_end_ns,
            .attempts = gpu_timestamp_attempts,
            .recorded = gpu_timestamp_recorded,
            .collected = gpu_timestamp_samples,
            .ring_full_skips = gpu_timestamp_ring_full_skips,
            .not_ready_polls = gpu_timestamp_not_ready_polls,
            .period_ns = gpu_timestamp_period_ns,
        });
    }

    constexpr u64 LogWindow = 64;
    if (gpu_timestamp_window_samples < LogWindow) {
        return;
    }

    const double tick_to_ms = gpu_timestamp_period_ns / 1'000'000.0;
    const double average_ms = (static_cast<double>(gpu_timestamp_window_ticks) /
                               static_cast<double>(gpu_timestamp_window_samples)) *
                              tick_to_ms;
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_GPU_TIMESTAMP] scheduler=%p samples=%llu gpuBatchMs=%.3f "
                        "minMs=%.3f maxMs=%.3f ringFullSkips=%llu notReadyPolls=%llu",
                        this, static_cast<unsigned long long>(gpu_timestamp_samples), average_ms,
                        static_cast<double>(gpu_timestamp_window_min_ticks) * tick_to_ms,
                        static_cast<double>(gpu_timestamp_window_max_ticks) * tick_to_ms,
                        static_cast<unsigned long long>(gpu_timestamp_ring_full_skips),
                        static_cast<unsigned long long>(gpu_timestamp_not_ready_polls));
    gpu_timestamp_window_samples = 0;
    gpu_timestamp_window_ticks = 0;
    gpu_timestamp_window_min_ticks = std::numeric_limits<u64>::max();
    gpu_timestamp_window_max_ticks = 0;
}

void Scheduler::BeginGpuTimestampSample() const {
    if (gpu_timestamp_attempted) {
        return;
    }
    gpu_timestamp_attempted = true;
    if (!gpu_timestamp_enabled || !gpu_timestamp_query_pool || master_semaphore.IsDeviceLost()) {
        return;
    }

    ++gpu_timestamp_attempts;

    CollectGpuTimestampSample();
    std::scoped_lock slot_lock{gpu_timestamp_slots_mutex};
    for (u32 offset = 0; offset < GpuTimestampPairCount; ++offset) {
        const u32 slot_index = (gpu_timestamp_write_cursor + offset) % GpuTimestampPairCount;
        auto& slot = gpu_timestamp_slots[slot_index];
        if (slot.state != GpuTimestampSlotState::Free) {
            continue;
        }
        const u32 first_query = slot_index * 2;
        current_cmdbuf.resetQueryPool(*gpu_timestamp_query_pool, first_query, 2);
        if (gpu_fault_checkpoint_query_pool) {
            const u32 first_checkpoint_query = slot_index * GpuFaultCheckpointQueryCount;
            current_cmdbuf.resetQueryPool(*gpu_fault_checkpoint_query_pool, first_checkpoint_query,
                                          GpuFaultCheckpointQueryCount);
        }
        current_cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe,
                                      *gpu_timestamp_query_pool, first_query);
        slot.state = GpuTimestampSlotState::Recording;
        slot.fault_draw_count = 0;
        slot.fault_checkpoint_count = 0;
        slot.fault_checkpoint_overflow = 0;
        slot.fault_last_journal_sequence = 0;
        slot.fault_pre_end_draw_count = 0;
        slot.fault_pre_end_recorded = false;
        slot.fault_checkpoints = {};
        const u64 override_epoch = gpu_timeline_epoch_override.load(std::memory_order_acquire);
        slot.frame_epoch = override_epoch != 0
                               ? override_epoch
                               : g_executor_gpu_frame_epoch.load(std::memory_order_acquire);
        if (ExecutorAndroidGpuTimelineEnabled() || gpu_fault_journal_enabled) {
            slot.cpu_record_begin_ns =
                static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
        }
        gpu_timestamp_active_slot = slot_index;
        gpu_timestamp_write_cursor = (slot_index + 1) % GpuTimestampPairCount;
        ++gpu_timestamp_recorded;
        return;
    }
    ++gpu_timestamp_ring_full_skips;
}

void Scheduler::RecordGpuFaultDrawCheckpoint(const u64 journal_sequence,
                                             const u64 pipeline_key_hash, const u64 element_count,
                                             const ExecutorVkJournalDrawType draw_type,
                                             const u32 primitive_type) const {
    if (!gpu_fault_draw_checkpoints_enabled || !gpu_fault_checkpoint_query_pool ||
        gpu_timestamp_active_slot == InvalidGpuTimestampSlot || master_semaphore.IsDeviceLost()) {
        return;
    }

    std::scoped_lock slot_lock{gpu_timestamp_slots_mutex};
    if (gpu_timestamp_active_slot == InvalidGpuTimestampSlot) {
        return;
    }
    auto& slot = gpu_timestamp_slots[gpu_timestamp_active_slot];
    if (slot.state != GpuTimestampSlotState::Recording) {
        return;
    }

    const u32 draw_ordinal = ++slot.fault_draw_count;
    slot.fault_last_journal_sequence = journal_sequence;
    if (draw_ordinal % GpuFaultCheckpointInterval != 0) {
        return;
    }
    if (slot.fault_checkpoint_count == GpuFaultDrawCheckpointCount) {
        ++slot.fault_checkpoint_overflow;
        return;
    }

    const u32 checkpoint_index = slot.fault_checkpoint_count++;
    slot.fault_checkpoints[checkpoint_index] = {
        .journal_sequence = journal_sequence,
        .pipeline_key_hash = pipeline_key_hash,
        .element_count = element_count,
        .draw_ordinal = draw_ordinal,
        .draw_type = static_cast<u32>(draw_type),
        .primitive_type = primitive_type,
    };
    const u32 query = gpu_timestamp_active_slot * GpuFaultCheckpointQueryCount + checkpoint_index;
    // Availability of this query proves that all commands through the marked
    // draw reached the end of the pipeline. It intentionally adds no barrier,
    // host wait, queue split, or cross-submit synchronization.
    current_cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,
                                  *gpu_fault_checkpoint_query_pool, query);
}

void Scheduler::RecordGpuFaultPreEndCheckpoint() const {
    if (!gpu_fault_draw_checkpoints_enabled || !gpu_fault_checkpoint_query_pool ||
        gpu_timestamp_active_slot == InvalidGpuTimestampSlot || master_semaphore.IsDeviceLost()) {
        return;
    }

    std::scoped_lock slot_lock{gpu_timestamp_slots_mutex};
    if (gpu_timestamp_active_slot == InvalidGpuTimestampSlot) {
        return;
    }
    auto& slot = gpu_timestamp_slots[gpu_timestamp_active_slot];
    if (slot.state != GpuTimestampSlotState::Recording || slot.fault_pre_end_recorded) {
        return;
    }

    slot.fault_pre_end_recorded = true;
    slot.fault_pre_end_draw_count = slot.fault_draw_count;
    const u32 query =
        gpu_timestamp_active_slot * GpuFaultCheckpointQueryCount + GpuFaultDrawCheckpointCount;
    // This timestamp is recorded immediately before EndRendering. Comparing
    // it with the submit's bottom timestamp separates guest draws from render
    // scope finalization/tile-store work without splitting the command buffer.
    current_cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,
                                  *gpu_fault_checkpoint_query_pool, query);
}

void Scheduler::FinishGpuTimestampSample(const u64 logical_tick) {
    std::scoped_lock slot_lock{gpu_timestamp_slots_mutex};
    ++gpu_timestamp_submit_ordinal;
    if (gpu_timestamp_active_slot == InvalidGpuTimestampSlot) {
        return;
    }

    auto& slot = gpu_timestamp_slots[gpu_timestamp_active_slot];
    slot.logical_tick = logical_tick;
    if (ExecutorAndroidGpuTimelineEnabled() || gpu_fault_journal_enabled) {
        slot.cpu_record_end_ns =
            static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count());
    }
    if (!master_semaphore.IsDeviceLost()) {
        current_cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,
                                      *gpu_timestamp_query_pool, gpu_timestamp_active_slot * 2 + 1);
    }
    slot.state = GpuTimestampSlotState::Pending;
    slot.submit_ordinal = gpu_timestamp_submit_ordinal;
    gpu_timestamp_active_slot = InvalidGpuTimestampSlot;
}

void Scheduler::DumpGpuTimestampFaultState(const u64 final_tick,
                                           const vk::Result submit_result) const noexcept {
    if (!instance.GetCapabilityProfile().exact_proprietary_adreno750 ||
        !ExecutorVkJournalRequested()) {
        return;
    }

    constexpr const char* FaultPath =
        "/data/data/app.lsx4.android/files/runtime/gpu-fault-timestamps.txt";
    ::unlink(FaultPath);
    FILE* out = std::fopen(FaultPath, "w");
    if (out == nullptr) {
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_GPU_FAULT_TIMESTAMPS] finalTick=%llu result=%d fileOpen=0",
                            static_cast<unsigned long long>(final_tick),
                            static_cast<int>(submit_result));
        DumpGpuTimeline();
        return;
    }

    __android_log_print(
        ANDROID_LOG_ERROR, "LSX4Native",
        "[EXECUTOR_GPU_FAULT_TIMESTAMPS_BEGIN] finalTick=%llu result=%d observer=%p",
        static_cast<unsigned long long>(final_tick), static_cast<int>(submit_result), this);

    const vk::QueryResultFlags query_flags =
        vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability;
    {
        // Keep the registry locked while dereferencing Scheduler pointers. The
        // destructor unregisters under the same mutex before member teardown.
        std::scoped_lock registry_lock{g_gpu_timestamp_scheduler_registry_mutex};
        const std::size_t registered = static_cast<std::size_t>(
            std::count_if(g_gpu_timestamp_schedulers.begin(), g_gpu_timestamp_schedulers.end(),
                          [](const Scheduler* scheduler) { return scheduler != nullptr; }));
        std::fprintf(out, "schema=3 finalTick=%llu result=%d observer=%p registered=%zu\n",
                     static_cast<unsigned long long>(final_tick), static_cast<int>(submit_result),
                     this, registered);

        for (const Scheduler* scheduler : g_gpu_timestamp_schedulers) {
            if (scheduler == nullptr) {
                continue;
            }
            std::scoped_lock slot_lock{scheduler->gpu_timestamp_slots_mutex};
            std::array<u64, GpuTimestampPairCount * 4> values{};
            std::array<u64, GpuTimestampPairCount * GpuFaultCheckpointQueryCount * 2>
                checkpoint_values{};
            bool fault_query_attempted = false;
            s32 fault_query_result = std::numeric_limits<s32>::min();
            bool fault_query_data_valid = false;
            if (scheduler->gpu_timestamp_query_pool) {
                fault_query_attempted = true;
                const vk::Result result = scheduler->instance.GetDevice().getQueryPoolResults(
                    *scheduler->gpu_timestamp_query_pool, 0, GpuTimestampPairCount * 2,
                    sizeof(values), values.data(), sizeof(u64) * 2, query_flags);
                fault_query_result = static_cast<s32>(result);
                fault_query_data_valid =
                    result == vk::Result::eSuccess || result == vk::Result::eNotReady;
            }
            bool checkpoint_query_attempted = false;
            s32 checkpoint_query_result = std::numeric_limits<s32>::min();
            bool checkpoint_query_data_valid = false;
            if (scheduler->gpu_fault_checkpoint_query_pool) {
                checkpoint_query_attempted = true;
                const vk::Result result = scheduler->instance.GetDevice().getQueryPoolResults(
                    *scheduler->gpu_fault_checkpoint_query_pool, 0,
                    GpuTimestampPairCount * GpuFaultCheckpointQueryCount, sizeof(checkpoint_values),
                    checkpoint_values.data(), sizeof(u64) * 2, query_flags);
                checkpoint_query_result = static_cast<s32>(result);
                checkpoint_query_data_valid =
                    result == vk::Result::eSuccess || result == vk::Result::eNotReady;
            }
            std::fprintf(out,
                         "scheduler=%p kind=%s enabled=%u exactJournal=%u ordinal=%llu "
                         "validBits=%u periodNs=%.9f faultQueryAttempted=%u "
                         "faultQueryResult=%d faultQueryDataValid=%u "
                         "drawCheckpoints=%u checkpointQueryAttempted=%u "
                         "checkpointQueryResult=%d checkpointQueryDataValid=%u\n",
                         scheduler, GpuTimelineKindName(scheduler->gpu_timeline_kind),
                         scheduler->gpu_timestamp_enabled ? 1u : 0u,
                         scheduler->gpu_fault_journal_enabled ? 1u : 0u,
                         static_cast<unsigned long long>(scheduler->gpu_timestamp_submit_ordinal),
                         scheduler->gpu_timestamp_valid_bits, scheduler->gpu_timestamp_period_ns,
                         fault_query_attempted ? 1u : 0u, fault_query_result,
                         fault_query_data_valid ? 1u : 0u,
                         scheduler->gpu_fault_draw_checkpoints_enabled ? 1u : 0u,
                         checkpoint_query_attempted ? 1u : 0u, checkpoint_query_result,
                         checkpoint_query_data_valid ? 1u : 0u);

            for (u32 slot_index = 0; slot_index < GpuTimestampPairCount; ++slot_index) {
                const auto& slot = scheduler->gpu_timestamp_slots[slot_index];
                if (slot.state == GpuTimestampSlotState::Free) {
                    continue;
                }
                const char* state =
                    slot.state == GpuTimestampSlotState::Recording ? "recording" : "pending";
                const std::size_t value_index = slot_index * 4;
                std::fprintf(
                    out,
                    "slot=%u state=%s submitOrdinal=%llu epoch=%llu "
                    "logicalTick=%llu physicalSequence=%llu physicalIndex=%u "
                    "faultDataValid=%u faultTopValue=%llu faultTopAvailable=%llu "
                    "faultBottomValue=%llu faultBottomAvailable=%llu "
                    "cachedPolls=%u cachedResult=%d cachedDataValid=%u "
                    "cachedTopValue=%llu cachedTopAvailable=%llu "
                    "cachedBottomValue=%llu cachedBottomAvailable=%llu "
                    "cpuRecordBeginNs=%llu cpuRecordEndNs=%llu "
                    "cpuSubmitBeginNs=%llu cpuSubmitEndNs=%llu\n",
                    slot_index, state, static_cast<unsigned long long>(slot.submit_ordinal),
                    static_cast<unsigned long long>(slot.frame_epoch),
                    static_cast<unsigned long long>(slot.logical_tick),
                    static_cast<unsigned long long>(slot.physical_submit_sequence),
                    slot.physical_submit_index, fault_query_data_valid ? 1u : 0u,
                    static_cast<unsigned long long>(fault_query_data_valid ? values[value_index]
                                                                           : 0),
                    static_cast<unsigned long long>(fault_query_data_valid ? values[value_index + 1]
                                                                           : 0),
                    static_cast<unsigned long long>(fault_query_data_valid ? values[value_index + 2]
                                                                           : 0),
                    static_cast<unsigned long long>(fault_query_data_valid ? values[value_index + 3]
                                                                           : 0),
                    slot.query_poll_count, slot.last_query_result,
                    slot.last_query_data_valid ? 1u : 0u,
                    static_cast<unsigned long long>(slot.last_top_value),
                    static_cast<unsigned long long>(slot.last_top_available),
                    static_cast<unsigned long long>(slot.last_bottom_value),
                    static_cast<unsigned long long>(slot.last_bottom_available),
                    static_cast<unsigned long long>(slot.cpu_record_begin_ns),
                    static_cast<unsigned long long>(slot.cpu_record_end_ns),
                    static_cast<unsigned long long>(slot.cpu_submit_begin_ns),
                    static_cast<unsigned long long>(slot.cpu_submit_end_ns));
                __android_log_print(ANDROID_LOG_ERROR, "LSX4Native",
                                    "[EXECUTOR_GPU_FAULT_TIMESTAMP] scheduler=%p kind=%s slot=%u "
                                    "state=%s logicalTick=%llu physicalSequence=%llu "
                                    "faultDataValid=%u faultTopAvailable=%llu "
                                    "faultBottomAvailable=%llu cachedDataValid=%u "
                                    "cachedTopAvailable=%llu cachedBottomAvailable=%llu",
                                    scheduler, GpuTimelineKindName(scheduler->gpu_timeline_kind),
                                    slot_index, state,
                                    static_cast<unsigned long long>(slot.logical_tick),
                                    static_cast<unsigned long long>(slot.physical_submit_sequence),
                                    fault_query_data_valid ? 1u : 0u,
                                    static_cast<unsigned long long>(
                                        fault_query_data_valid ? values[value_index + 1] : 0),
                                    static_cast<unsigned long long>(
                                        fault_query_data_valid ? values[value_index + 3] : 0),
                                    slot.last_query_data_valid ? 1u : 0u,
                                    static_cast<unsigned long long>(slot.last_top_available),
                                    static_cast<unsigned long long>(slot.last_bottom_available));

                if (scheduler->gpu_fault_draw_checkpoints_enabled) {
                    std::fprintf(out,
                                 "checkpointSlot=%u logicalTick=%llu drawCount=%u "
                                 "recorded=%u overflow=%u lastJournalSeq=%llu "
                                 "preEndRecorded=%u preEndDrawCount=%u\n",
                                 slot_index, static_cast<unsigned long long>(slot.logical_tick),
                                 slot.fault_draw_count, slot.fault_checkpoint_count,
                                 slot.fault_checkpoint_overflow,
                                 static_cast<unsigned long long>(slot.fault_last_journal_sequence),
                                 slot.fault_pre_end_recorded ? 1u : 0u,
                                 slot.fault_pre_end_draw_count);

                    u32 contiguous_checkpoints = 0;
                    bool saw_unavailable = false;
                    bool availability_gap = false;
                    for (u32 checkpoint_index = 0; checkpoint_index < slot.fault_checkpoint_count;
                         ++checkpoint_index) {
                        const auto& checkpoint = slot.fault_checkpoints[checkpoint_index];
                        const std::size_t checkpoint_value_index =
                            (slot_index * GpuFaultCheckpointQueryCount + checkpoint_index) * 2;
                        const u64 checkpoint_value = checkpoint_query_data_valid
                                                         ? checkpoint_values[checkpoint_value_index]
                                                         : 0;
                        const u64 checkpoint_available =
                            checkpoint_query_data_valid
                                ? checkpoint_values[checkpoint_value_index + 1]
                                : 0;
                        if (checkpoint_query_data_valid) {
                            if (checkpoint_available == 0) {
                                saw_unavailable = true;
                            } else if (!saw_unavailable) {
                                contiguous_checkpoints = checkpoint_index + 1;
                            } else {
                                availability_gap = true;
                            }
                        }
                        std::fprintf(out,
                                     "checkpoint slot=%u index=%u kind=draw ordinal=%u "
                                     "journalSeq=%llu pipeline=0x%llx elements=%llu "
                                     "drawType=%u primitive=%u faultDataValid=%u "
                                     "faultValue=%llu faultAvailable=%llu\n",
                                     slot_index, checkpoint_index, checkpoint.draw_ordinal,
                                     static_cast<unsigned long long>(checkpoint.journal_sequence),
                                     static_cast<unsigned long long>(checkpoint.pipeline_key_hash),
                                     static_cast<unsigned long long>(checkpoint.element_count),
                                     checkpoint.draw_type, checkpoint.primitive_type,
                                     checkpoint_query_data_valid ? 1u : 0u,
                                     static_cast<unsigned long long>(checkpoint_value),
                                     static_cast<unsigned long long>(checkpoint_available));
                    }

                    const std::size_t pre_end_value_index =
                        (slot_index * GpuFaultCheckpointQueryCount + GpuFaultDrawCheckpointCount) *
                        2;
                    const u64 pre_end_value =
                        checkpoint_query_data_valid && slot.fault_pre_end_recorded
                            ? checkpoint_values[pre_end_value_index]
                            : 0;
                    const u64 pre_end_available =
                        checkpoint_query_data_valid && slot.fault_pre_end_recorded
                            ? checkpoint_values[pre_end_value_index + 1]
                            : 0;
                    std::fprintf(
                        out,
                        "checkpoint slot=%u index=%u kind=preEnd ordinal=%u "
                        "journalSeq=%llu faultDataValid=%u faultValue=%llu "
                        "faultAvailable=%llu\n",
                        slot_index, GpuFaultDrawCheckpointCount, slot.fault_pre_end_draw_count,
                        static_cast<unsigned long long>(slot.fault_last_journal_sequence),
                        checkpoint_query_data_valid && slot.fault_pre_end_recorded ? 1u : 0u,
                        static_cast<unsigned long long>(pre_end_value),
                        static_cast<unsigned long long>(pre_end_available));

                    const u32 frontier_draw =
                        contiguous_checkpoints == 0
                            ? 0
                            : slot.fault_checkpoints[contiguous_checkpoints - 1].draw_ordinal;
                    const u32 candidate_begin = frontier_draw + 1;
                    const u32 candidate_end =
                        contiguous_checkpoints < slot.fault_checkpoint_count
                            ? slot.fault_checkpoints[contiguous_checkpoints].draw_ordinal
                            : slot.fault_draw_count;
                    const bool fault_top_available =
                        fault_query_data_valid && values[value_index + 1] != 0;
                    const bool fault_bottom_available =
                        fault_query_data_valid && values[value_index + 3] != 0;
                    const char* frontier_class =
                        !checkpoint_query_data_valid
                            ? "query_invalid"
                            : (pre_end_available != 0 && fault_top_available &&
                                       !fault_bottom_available
                                   ? "end_rendering_or_post_pre_end"
                                   : (pre_end_available == 0 ? "draw_or_pre_end"
                                                             : "completed_or_reset_epoch"));
                    std::fprintf(out,
                                 "checkpointFrontier slot=%u logicalTick=%llu "
                                 "contiguous=%u recorded=%u frontierDraw=%u "
                                 "candidateDraw=%u-%u preEndAvailable=%llu "
                                 "bottomAvailable=%u availabilityGap=%u class=%s\n",
                                 slot_index, static_cast<unsigned long long>(slot.logical_tick),
                                 contiguous_checkpoints, slot.fault_checkpoint_count, frontier_draw,
                                 candidate_begin, candidate_end,
                                 static_cast<unsigned long long>(pre_end_available),
                                 fault_bottom_available ? 1u : 0u, availability_gap ? 1u : 0u,
                                 frontier_class);
                    __android_log_print(
                        ANDROID_LOG_ERROR, "LSX4Native",
                        "[EXECUTOR_GPU_FAULT_DRAW_FRONTIER] scheduler=%p "
                        "kind=%s slot=%u logicalTick=%llu drawCount=%u "
                        "contiguous=%u frontierDraw=%u candidate=%u-%u "
                        "preEnd=%llu bottom=%u gap=%u class=%s",
                        scheduler, GpuTimelineKindName(scheduler->gpu_timeline_kind), slot_index,
                        static_cast<unsigned long long>(slot.logical_tick), slot.fault_draw_count,
                        contiguous_checkpoints, frontier_draw, candidate_begin, candidate_end,
                        static_cast<unsigned long long>(pre_end_available),
                        fault_bottom_available ? 1u : 0u, availability_gap ? 1u : 0u,
                        frontier_class);
                }
            }
        }
    }
    std::fclose(out);
    DumpGpuTimeline();
    __android_log_print(ANDROID_LOG_ERROR, "LSX4Native",
                        "[EXECUTOR_GPU_FAULT_TIMESTAMPS_END] finalTick=%llu path=%s",
                        static_cast<unsigned long long>(final_tick), FaultPath);
}
#endif

void Scheduler::BindHostDescriptors(vk::CommandBuffer cmdbuf, vk::PipelineBindPoint bind_point,
                                    vk::PipelineLayout pipeline_layout,
                                    vk::DescriptorSetLayout descriptor_set_layout,
                                    vk::ArrayProxy<const vk::WriteDescriptorSet> writes) {
    if (instance.MaxPushDescriptors() != 0) {
        cmdbuf.pushDescriptorSetKHR(bind_point, pipeline_layout, 0, writes);
        return;
    }

    ASSERT(legacy_descriptor_heap != nullptr);
    const vk::DescriptorSet descriptor_set = legacy_descriptor_heap->Commit(descriptor_set_layout);
    boost::container::small_vector<vk::WriteDescriptorSet, 8> set_writes{writes.begin(),
                                                                         writes.end()};
    for (auto& write : set_writes) {
        write.dstSet = descriptor_set;
    }
    instance.GetDevice().updateDescriptorSets(set_writes, {});
    cmdbuf.bindDescriptorSets(bind_point, pipeline_layout, 0, descriptor_set, {});
}

void Scheduler::BeginRendering(const RenderState& new_state) {
    if (master_semaphore.IsDeviceLost()) {
        return;
    }
    if (is_rendering && render_state == new_state) {
        return;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool managed_render_scope_reuse = VideoCore::IsManagedVulkanRenderScopeReuse();
    ExecutorRenderScopeTelemetry* const telemetry =
        managed_render_scope_reuse && ExecutorVulkanOptimizationStatsEnabled()
            ? &RenderScopeTelemetryFor(this)
            : nullptr;
    if (is_rendering && managed_render_scope_reuse &&
        CanReuseAfterConsumedClear(render_state, new_state)) {
        render_state = new_state;
        if (telemetry != nullptr) {
            ++telemetry->consumed_clear_reuse_count;
        }
        VideoCore::RecordVulkanRenderScopeReuse();
        return;
    }
    if (telemetry != nullptr) {
        ++telemetry->begin_count;
        if (!is_rendering && telemetry->has_last_ended_render_state &&
            telemetry->last_ended_render_state == new_state) {
            ++telemetry->same_state_reopen_count;
            ++telemetry->same_state_reopen_counts[
                static_cast<std::size_t>(telemetry->last_end_reason)];
        }
    }
    if (telemetry != nullptr &&
        (telemetry->begin_count == 256 || (telemetry->begin_count & 4095u) == 0)) {
        const auto reason_count = [&](const EndRenderingReason reason) {
            return telemetry->end_reason_counts[static_cast<std::size_t>(reason)];
        };
        const auto reopen_count = [&](const EndRenderingReason reason) {
            return telemetry->same_state_reopen_counts[static_cast<std::size_t>(reason)];
        };
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_RENDER_SCOPE_TELEMETRY] begins=%llu ends=%llu sameStateReopen=%llu "
            "consumedClearReuse=%llu "
            "reasonEndReopen=unknown:%llu/%llu,state:%llu/%llu,pipeline:%llu/%llu,"
            "rastBarrier:%llu/%llu,draw:%llu/%llu,clear:%llu/%llu,copy:%llu/%llu,"
            "dispatch:%llu/%llu,upload:%llu/%llu,download:%llu/%llu,transit:%llu/%llu,"
            "refresh:%llu/%llu,bufFill:%llu/%llu,bufRead:%llu/%llu,"
            "bufCopy:%llu/%llu,bufProbe:%llu/%llu,bufUpload:%llu/%llu,"
            "faultCompute:%llu/%llu,tileCompute:%llu/%llu,blit:%llu/%llu,"
            "shaderHle:%llu/%llu,presenter:%llu/%llu,flush:%llu/%llu,submit:%llu/%llu",
            static_cast<unsigned long long>(telemetry->begin_count),
            static_cast<unsigned long long>(telemetry->end_count),
            static_cast<unsigned long long>(telemetry->same_state_reopen_count),
            static_cast<unsigned long long>(telemetry->consumed_clear_reuse_count),
            static_cast<unsigned long long>(reason_count(EndRenderingReason::Unknown)),
            static_cast<unsigned long long>(reopen_count(EndRenderingReason::Unknown)),
            static_cast<unsigned long long>(reason_count(EndRenderingReason::BeginStateChange)),
            static_cast<unsigned long long>(reopen_count(EndRenderingReason::BeginStateChange)),
            static_cast<unsigned long long>(
                reason_count(EndRenderingReason::PipelineResourceBarrier)),
            static_cast<unsigned long long>(
                reopen_count(EndRenderingReason::PipelineResourceBarrier)),
            static_cast<unsigned long long>(reason_count(EndRenderingReason::RasterizerBarrier)),
            static_cast<unsigned long long>(reopen_count(EndRenderingReason::RasterizerBarrier)),
            static_cast<unsigned long long>(reason_count(EndRenderingReason::RasterizerDraw)),
            static_cast<unsigned long long>(reopen_count(EndRenderingReason::RasterizerDraw)),
            static_cast<unsigned long long>(reason_count(EndRenderingReason::RasterizerClear)),
            static_cast<unsigned long long>(reopen_count(EndRenderingReason::RasterizerClear)),
            static_cast<unsigned long long>(reason_count(EndRenderingReason::RasterizerCopy)),
            static_cast<unsigned long long>(reopen_count(EndRenderingReason::RasterizerCopy)),
            static_cast<unsigned long long>(reason_count(EndRenderingReason::RasterizerDispatch)),
            static_cast<unsigned long long>(reopen_count(EndRenderingReason::RasterizerDispatch)),
            static_cast<unsigned long long>(reason_count(EndRenderingReason::ImageUpload)),
            static_cast<unsigned long long>(reopen_count(EndRenderingReason::ImageUpload)),
            static_cast<unsigned long long>(reason_count(EndRenderingReason::ImageDownload)),
            static_cast<unsigned long long>(reopen_count(EndRenderingReason::ImageDownload)),
            static_cast<unsigned long long>(reason_count(EndRenderingReason::ImageTransit)),
            static_cast<unsigned long long>(reopen_count(EndRenderingReason::ImageTransit)),
            static_cast<unsigned long long>(reason_count(EndRenderingReason::TextureCacheRefresh)),
            static_cast<unsigned long long>(reopen_count(EndRenderingReason::TextureCacheRefresh)),
            static_cast<unsigned long long>(reason_count(EndRenderingReason::BufferFill)),
            static_cast<unsigned long long>(reopen_count(EndRenderingReason::BufferFill)),
            static_cast<unsigned long long>(reason_count(EndRenderingReason::BufferReadback)),
            static_cast<unsigned long long>(reopen_count(EndRenderingReason::BufferReadback)),
            static_cast<unsigned long long>(reason_count(EndRenderingReason::BufferCopy)),
            static_cast<unsigned long long>(reopen_count(EndRenderingReason::BufferCopy)),
            static_cast<unsigned long long>(reason_count(EndRenderingReason::BufferProbe)),
            static_cast<unsigned long long>(reopen_count(EndRenderingReason::BufferProbe)),
            static_cast<unsigned long long>(reason_count(EndRenderingReason::BufferUpload)),
            static_cast<unsigned long long>(reopen_count(EndRenderingReason::BufferUpload)),
            static_cast<unsigned long long>(reason_count(EndRenderingReason::FaultCompute)),
            static_cast<unsigned long long>(reopen_count(EndRenderingReason::FaultCompute)),
            static_cast<unsigned long long>(reason_count(EndRenderingReason::TileCompute)),
            static_cast<unsigned long long>(reopen_count(EndRenderingReason::TileCompute)),
            static_cast<unsigned long long>(reason_count(EndRenderingReason::BlitHelper)),
            static_cast<unsigned long long>(reopen_count(EndRenderingReason::BlitHelper)),
            static_cast<unsigned long long>(reason_count(EndRenderingReason::ShaderHleCopy)),
            static_cast<unsigned long long>(reopen_count(EndRenderingReason::ShaderHleCopy)),
            static_cast<unsigned long long>(reason_count(EndRenderingReason::Presenter)),
            static_cast<unsigned long long>(reopen_count(EndRenderingReason::Presenter)),
            static_cast<unsigned long long>(reason_count(EndRenderingReason::Flush)),
            static_cast<unsigned long long>(reopen_count(EndRenderingReason::Flush)),
            static_cast<unsigned long long>(reason_count(EndRenderingReason::Submit)),
            static_cast<unsigned long long>(reopen_count(EndRenderingReason::Submit)));
    }
#endif
    EndRendering(EndRenderingReason::BeginStateChange);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (telemetry != nullptr) {
        telemetry->has_last_ended_render_state = false;
    }
#endif
    is_rendering = true;
    render_state = new_state;

    std::array<vk::RenderingAttachmentInfo, 8> color_attachments;
    for (u32 i = 0; i < render_state.num_color_attachments; ++i) {
        const auto& cb = render_state.color_attachments[i];
        color_attachments[i] = vk::RenderingAttachmentInfo{
            .imageView = cb.image_view,
            .imageLayout = cb.image_layout,
            .loadOp = cb.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.uint32 = cb.clear_value}},
        };
    }

    const auto& db = render_state.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{.depth = std::bit_cast<float>(
                                                                          db.clear_value[0])}},
    };
    const vk::RenderingAttachmentInfo stencil_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil =
                                         vk::ClearDepthStencilValue{.stencil = db.clear_value[1]}},
    };

    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {render_state.width, render_state.height},
            },
        .layerCount = render_state.num_layers,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = db.has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment = db.has_stencil ? &stencil_attachment : nullptr,
    };

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (ExecutorAndroidRendererProbesEnabled()) {
        g_pass_extents.note(render_state.width, render_state.height,
                            render_state.num_color_attachments == 0);
    }
#endif
    ++rendering_epoch;
    command_recording_epoch.fetch_add(1, std::memory_order_relaxed);
    command_buffer_used = true;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (gpu_timestamp_enabled && !gpu_timestamp_attempted) {
        BeginGpuTimestampSample();
    }
#endif
    if (instance.UseLegacyRenderPass()) {
        current_legacy_render_scope = CreateLegacyRenderScope(render_state);
        const vk::RenderPassBeginInfo begin_info{
            .renderPass = current_legacy_render_scope.render_pass,
            .framebuffer = current_legacy_render_scope.framebuffer,
            .renderArea = rendering_info.renderArea,
            .clearValueCount = current_legacy_render_scope.clear_value_count,
            .pClearValues = current_legacy_render_scope.clear_values.data(),
        };
        current_cmdbuf.beginRenderPass(begin_info, vk::SubpassContents::eInline);
    } else {
        current_cmdbuf.beginRendering(rendering_info);
    }
}

void Scheduler::EndRendering(const EndRenderingReason reason) {
    if (master_semaphore.IsDeviceLost()) {
        is_rendering = false;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (ExecutorVulkanOptimizationStatsEnabled()) {
            RenderScopeTelemetryFor(this).has_last_ended_render_state = false;
        }
#endif
        return;
    }
    if (!is_rendering) {
        return;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (VideoCore::IsManagedVulkanRenderScopeReuse() &&
        ExecutorVulkanOptimizationStatsEnabled()) {
        auto& telemetry = RenderScopeTelemetryFor(this);
        ++telemetry.end_count;
        ++telemetry.end_reason_counts[static_cast<std::size_t>(reason)];
        telemetry.last_ended_render_state = render_state;
        telemetry.last_end_reason = reason;
        telemetry.has_last_ended_render_state = true;
    }
#endif
    is_rendering = false;
    if (instance.UseLegacyRenderPass()) {
        current_cmdbuf.endRenderPass();
        const auto scope = std::exchange(current_legacy_render_scope, {});
        const auto device = instance.GetDevice();
        DeferOperation([device, scope] {
            device.destroyFramebuffer(scope.framebuffer);
            device.destroyRenderPass(scope.render_pass);
        });
    } else {
        current_cmdbuf.endRendering();
    }
}

void Scheduler::PublishRetirementTick(const u64 tick) noexcept {
    {
        std::scoped_lock lk{retirement_pending_ops_mutex, priority_pending_ops_mutex};
        u64 published = retirement_last_published_tick.load(std::memory_order_relaxed);
        while (published < tick &&
               !retirement_last_published_tick.compare_exchange_weak(
                   published, tick, std::memory_order_release, std::memory_order_relaxed)) {
        }
    }
    retirement_pending_ops_cv.notify_all();
    priority_pending_ops_cv.notify_all();
}

bool Scheduler::TryCompletionBarrierInsideRendering() {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    // Adreno's dynamic-rendering local-read implementation can corrupt the
    // command-buffer state after an in-rendering pipelineBarrier2 sequence.
    // The failure surfaces later in an unrelated barrier, so keep the normal
    // end-rendering dependency path on Qualcomm.
    if (!is_rendering || !VideoCore::IsManagedVulkanDriverOptimization() ||
        instance.GetCapabilityProfile().HasQuirk(CapabilityQuirk::AvoidInRenderingBarrier) ||
        !instance.IsDynamicRenderingLocalReadSupported() || !pending_buffer_transfers.empty() ||
        master_semaphore.IsDeviceLost()) {
        return false;
    }

    // While dynamic rendering is active, all compute, transfer, layout
    // transition and attachment-replacement paths have already ended the
    // scope. The remaining producer work is therefore graphics-shader work.
    // Keep framebuffer-space stages out of this dependency so the Vulkan
    // in-rendering synchronization rules remain satisfied.
    constexpr vk::PipelineStageFlags2 ShaderStages =
        vk::PipelineStageFlagBits2::eVertexShader |
        vk::PipelineStageFlagBits2::eTessellationControlShader |
        vk::PipelineStageFlagBits2::eTessellationEvaluationShader |
        vk::PipelineStageFlagBits2::eGeometryShader | vk::PipelineStageFlagBits2::eFragmentShader;
    constexpr vk::PipelineStageFlags2 ConsumerStages = ShaderStages |
                                                       vk::PipelineStageFlagBits2::eDrawIndirect |
                                                       vk::PipelineStageFlagBits2::eVertexInput;
    constexpr vk::MemoryBarrier2 shader_barrier{
        .srcStageMask = ShaderStages,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = ConsumerStages,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite |
                         vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    current_cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &shader_barrier,
    });

    // EOP/EOS completion also covers attachment writes. Keep that dependency
    // separate because Vulkan requires a barrier whose source includes a
    // framebuffer-space stage to target framebuffer-space stages only while
    // inside a rendering instance.
    constexpr vk::PipelineStageFlags2 FramebufferStages =
        vk::PipelineStageFlagBits2::eEarlyFragmentTests |
        vk::PipelineStageFlagBits2::eLateFragmentTests |
        vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    constexpr vk::MemoryBarrier2 framebuffer_barrier{
        .srcStageMask = FramebufferStages,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite |
                         vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
        .dstStageMask = FramebufferStages,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead |
                         vk::AccessFlagBits2::eColorAttachmentWrite |
                         vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                         vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
    };
    current_cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &framebuffer_barrier,
    });
    command_buffer_used = true;
    command_recording_epoch.fetch_add(1, std::memory_order_relaxed);
    return true;
#else
    return false;
#endif
}

Scheduler::ScopedBufferUploadBatch Scheduler::BeginBufferUploadBatch() {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    static const bool fex_backend = std::getenv("EXECUTOR_FEX_OWNS_FAULT_SIGNALS") != nullptr;
    static const bool enabled =
        !fex_backend && ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                                 "run-bisect-enable-vk-scoped-upload-batch",
                                 F_OK) == 0;
    if (!enabled || !VideoCore::IsManagedVulkanTransferCoalescing()) {
        return {};
    }
#endif
    ++scoped_buffer_upload_batch_depth;
    return ScopedBufferUploadBatch{*this};
}

void Scheduler::QueueBufferCopies(const vk::Buffer source, const vk::Buffer destination,
                                  const std::span<const vk::BufferCopy> regions,
                                  const VAddr source_guest_address, const u64 source_size,
                                  const VAddr destination_guest_address, const u64 destination_size,
                                  const BufferTransferOrigin origin) {
    if (regions.empty() || master_semaphore.IsDeviceLost()) {
        return;
    }
    ASSERT(origin < BufferTransferOrigin::Count);
    VideoCore::RecordVulkanTransferRequest(regions.size());
    static constexpr u64 MaxPendingRegions = 4096;
    static constexpr std::size_t MaxPendingGroups = 256;
    if (!pending_buffer_transfers.empty() &&
        (pending_buffer_transfers.size() >= MaxPendingGroups ||
         regions.size() >
             MaxPendingRegions - std::min(pending_buffer_transfer_regions, MaxPendingRegions))) {
        FlushPendingBufferTransfers();
    }

    const auto range_end = [](const VAddr begin, const u64 size) {
        return size > std::numeric_limits<VAddr>::max() - begin ? std::numeric_limits<VAddr>::max()
                                                                : begin + size;
    };
    const VAddr source_end = range_end(source_guest_address, source_size);
    const VAddr destination_end = range_end(destination_guest_address, destination_size);
    const auto overlaps = [](const VAddr lhs_begin, const VAddr lhs_end, const VAddr rhs_begin,
                             const VAddr rhs_end) {
        return lhs_begin < lhs_end && rhs_begin < rhs_end && lhs_begin < rhs_end &&
               rhs_begin < lhs_end;
    };
    vk::DeviceSize source_offset_begin = std::numeric_limits<vk::DeviceSize>::max();
    vk::DeviceSize source_offset_end = 0;
    vk::DeviceSize destination_offset_begin = std::numeric_limits<vk::DeviceSize>::max();
    vk::DeviceSize destination_offset_end = 0;
    for (const auto& region : regions) {
        source_offset_begin = std::min(source_offset_begin, region.srcOffset);
        source_offset_end = std::max(source_offset_end, region.srcOffset + region.size);
        destination_offset_begin = std::min(destination_offset_begin, region.dstOffset);
        destination_offset_end = std::max(destination_offset_end, region.dstOffset + region.size);
    }
    const auto offset_overlaps = [](const vk::DeviceSize lhs_begin, const vk::DeviceSize lhs_end,
                                    const vk::DeviceSize rhs_begin, const vk::DeviceSize rhs_end) {
        return lhs_begin < lhs_end && rhs_begin < rhs_end && lhs_begin < rhs_end &&
               rhs_begin < lhs_end;
    };
    const bool dependency_barrier_before =
        std::ranges::any_of(pending_buffer_transfers, [&](const PendingBufferTransfer& previous) {
            // Preserve RAW, WAR and WAW ordering while still allowing disjoint
            // transfer groups to share one render-scope break and barrier pair.
            const bool guest_hazard =
                overlaps(source_guest_address, source_end, previous.destination_begin,
                         previous.destination_end) ||
                overlaps(destination_guest_address, destination_end, previous.source_begin,
                         previous.source_end) ||
                overlaps(destination_guest_address, destination_end, previous.destination_begin,
                         previous.destination_end);
            const bool buffer_hazard =
                (source == previous.destination &&
                 offset_overlaps(source_offset_begin, source_offset_end,
                                 previous.destination_offset_begin,
                                 previous.destination_offset_end)) ||
                (destination == previous.source &&
                 offset_overlaps(destination_offset_begin, destination_offset_end,
                                 previous.source_offset_begin, previous.source_offset_end)) ||
                (destination == previous.destination &&
                 offset_overlaps(destination_offset_begin, destination_offset_end,
                                 previous.destination_offset_begin,
                                 previous.destination_offset_end));
            return guest_hazard || buffer_hazard;
        });

    PendingBufferTransfer* transfer = nullptr;
    if (!dependency_barrier_before && !pending_buffer_transfers.empty()) {
        for (auto candidate = pending_buffer_transfers.rbegin();
             candidate != pending_buffer_transfers.rend(); ++candidate) {
            if (candidate->source != source || candidate->destination != destination) {
                continue;
            }
            transfer = &*candidate;
            candidate->source_begin = std::min(candidate->source_begin, source_guest_address);
            candidate->source_end = std::max(candidate->source_end, source_end);
            candidate->destination_begin =
                std::min(candidate->destination_begin, destination_guest_address);
            candidate->destination_end = std::max(candidate->destination_end, destination_end);
            candidate->source_offset_begin =
                std::min(candidate->source_offset_begin, source_offset_begin);
            candidate->source_offset_end =
                std::max(candidate->source_offset_end, source_offset_end);
            candidate->destination_offset_begin =
                std::min(candidate->destination_offset_begin, destination_offset_begin);
            candidate->destination_offset_end =
                std::max(candidate->destination_offset_end, destination_offset_end);
            break;
        }
    }
    if (transfer == nullptr) {
        transfer = &pending_buffer_transfers.emplace_back(PendingBufferTransfer{
            .source = source,
            .destination = destination,
            .source_begin = source_guest_address,
            .source_end = source_end,
            .destination_begin = destination_guest_address,
            .destination_end = destination_end,
            .source_offset_begin = source_offset_begin,
            .source_offset_end = source_offset_end,
            .destination_offset_begin = destination_offset_begin,
            .destination_offset_end = destination_offset_end,
            .dependency_barrier_before = dependency_barrier_before,
        });
    }
    for (const vk::BufferCopy& region : regions) {
        bool coalesced = false;
        for (vk::BufferCopy& pending_region : transfer->regions) {
            if (pending_region.srcOffset + pending_region.size == region.srcOffset &&
                pending_region.dstOffset + pending_region.size == region.dstOffset) {
                pending_region.size += region.size;
                coalesced = true;
                break;
            }
            if (region.srcOffset + region.size == pending_region.srcOffset &&
                region.dstOffset + region.size == pending_region.dstOffset) {
                pending_region.srcOffset = region.srcOffset;
                pending_region.dstOffset = region.dstOffset;
                pending_region.size += region.size;
                coalesced = true;
                break;
            }
        }
        if (!coalesced) {
            transfer->regions.push_back(region);
        }
    }
    ++pending_buffer_transfer_requests;
    ++pending_buffer_transfer_request_epoch;
    pending_buffer_transfer_regions += regions.size();
    pending_buffer_transfer_origins |= u32{1} << static_cast<u32>(origin);
    pending_gnm_dma_transfers += origin == BufferTransferOrigin::GnmDma;

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (ExecutorVulkanOptimizationStatsEnabled()) {
        ++executor_buffer_transfers_queued;
        executor_buffer_transfer_regions_queued += regions.size();
        executor_buffer_transfer_max_queued =
            std::max<u64>(executor_buffer_transfer_max_queued, pending_buffer_transfers.size());
        switch (origin) {
        case BufferTransferOrigin::Hle:
            ++executor_hle_buffer_transfers_queued;
            break;
        case BufferTransferOrigin::Upload:
            ++executor_buffer_uploads_queued;
            break;
        case BufferTransferOrigin::GnmDma:
            ++executor_gnm_dma_transfers_queued;
            executor_gnm_dma_regions_queued += regions.size();
            break;
        case BufferTransferOrigin::Count:
            break;
        }
    }
#endif
    if (!VideoCore::IsManagedVulkanTransferCoalescing() ||
        (origin == BufferTransferOrigin::Upload && scoped_buffer_upload_batch_depth == 0)) {
        FlushPendingBufferTransfers();
    }
}

void Scheduler::QueueBufferUploadCopies(const vk::Buffer source, const vk::Buffer destination,
                                        const std::span<const vk::BufferCopy> regions,
                                        const VAddr destination_guest_address,
                                        const u64 destination_size) {
    QueueBufferCopies(source, destination, regions, 0, 0, destination_guest_address,
                      destination_size, BufferTransferOrigin::Upload);
}

void Scheduler::FlushPendingBufferTransfersForRange(const VAddr guest_address, const u64 size) {
    if (pending_buffer_transfers.empty() || size == 0) {
        return;
    }
    const VAddr guest_end = size > std::numeric_limits<VAddr>::max() - guest_address
                                ? std::numeric_limits<VAddr>::max()
                                : guest_address + size;
    const auto overlaps = [guest_address, guest_end](const VAddr begin, const VAddr end) {
        return guest_address < end && begin < guest_end;
    };
    const bool has_conflict =
        std::ranges::any_of(pending_buffer_transfers, [&](const PendingBufferTransfer& transfer) {
            return overlaps(transfer.source_begin, transfer.source_end) ||
                   overlaps(transfer.destination_begin, transfer.destination_end);
        });
    if (has_conflict) {
        FlushPendingBufferTransfers();
    }
}

void Scheduler::FlushPendingGnmDmaTransfers() {
    if (pending_gnm_dma_transfers != 0) {
        FlushPendingBufferTransfers();
    }
}

void Scheduler::FlushPendingBufferTransfers() {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    SchedSpan _probe_flush{2};
    const bool executor_vulkan_optimization_stats_enabled =
        ExecutorVulkanOptimizationStatsEnabled();
#endif
    if (pending_buffer_transfers.empty() || flushing_buffer_transfers) {
        return;
    }
    if (master_semaphore.IsDeviceLost()) {
        pending_buffer_transfers.clear();
        pending_buffer_transfer_requests = 0;
        pending_buffer_transfer_regions = 0;
        pending_gnm_dma_transfers = 0;
        pending_buffer_transfer_origins = 0;
        return;
    }

    flushing_buffer_transfers = true;
    SCOPE_EXIT {
        flushing_buffer_transfers = false;
    };
    const bool has_gnm_dma = (pending_buffer_transfer_origins &
                              (u32{1} << static_cast<u32>(BufferTransferOrigin::GnmDma))) != 0;
    const bool has_upload = (pending_buffer_transfer_origins &
                             (u32{1} << static_cast<u32>(BufferTransferOrigin::Upload))) != 0;
    EndRendering(has_gnm_dma  ? EndRenderingReason::BufferCopy
                 : has_upload ? EndRenderingReason::BufferUpload
                              : EndRenderingReason::ShaderHleCopy);
    const bool use_buffer_local_barriers = VideoCore::IsManagedVulkanTransferCoalescing();
    static constexpr vk::MemoryBarrier ReadBarrier{
        .srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite |
                         vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask = vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite,
    };
    static constexpr vk::MemoryBarrier WriteBarrier{
        .srcAccessMask = vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
    };
    static constexpr vk::MemoryBarrier DependencyBarrier{
        .srcAccessMask = vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask = vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite,
    };
    struct BufferTransferRange {
        vk::Buffer buffer;
        vk::DeviceSize begin;
        vk::DeviceSize end;
        vk::AccessFlags2 transfer_access;
    };
    boost::container::small_vector<BufferTransferRange, 32> pre_ranges;
    boost::container::small_vector<BufferTransferRange, 32> post_ranges;
    const auto merge_range = [](auto& ranges, const vk::Buffer buffer, const vk::DeviceSize begin,
                                const vk::DeviceSize end, const vk::AccessFlags2 transfer_access) {
        if (!buffer || begin >= end) {
            return;
        }
        const auto existing = std::ranges::find(ranges, buffer, &BufferTransferRange::buffer);
        if (existing == ranges.end()) {
            ranges.emplace_back(BufferTransferRange{
                .buffer = buffer,
                .begin = begin,
                .end = end,
                .transfer_access = transfer_access,
            });
            return;
        }
        existing->begin = std::min(existing->begin, begin);
        existing->end = std::max(existing->end, end);
        existing->transfer_access |= transfer_access;
    };
    if (use_buffer_local_barriers) {
        for (const PendingBufferTransfer& transfer : pending_buffer_transfers) {
            merge_range(pre_ranges, transfer.source, transfer.source_offset_begin,
                        transfer.source_offset_end, vk::AccessFlagBits2::eTransferRead);
            merge_range(pre_ranges, transfer.destination, transfer.destination_offset_begin,
                        transfer.destination_offset_end, vk::AccessFlagBits2::eTransferWrite);
            merge_range(post_ranges, transfer.destination, transfer.destination_offset_begin,
                        transfer.destination_offset_end, vk::AccessFlagBits2::eTransferWrite);
        }
    }
    boost::container::small_vector<vk::BufferMemoryBarrier2, 32> pre_barriers;
    boost::container::small_vector<vk::BufferMemoryBarrier2, 32> post_barriers;
    if (use_buffer_local_barriers) {
        pre_barriers.reserve(pre_ranges.size());
        for (const BufferTransferRange& range : pre_ranges) {
            pre_barriers.emplace_back(vk::BufferMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask =
                    vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = range.transfer_access,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = range.buffer,
                .offset = range.begin,
                .size = range.end - range.begin,
            });
        }
        post_barriers.reserve(post_ranges.size());
        for (const BufferTransferRange& range : post_ranges) {
            post_barriers.emplace_back(vk::BufferMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask =
                    vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = range.buffer,
                .offset = range.begin,
                .size = range.end - range.begin,
            });
        }
    }

    const auto cmdbuf = CommandBuffer();
    if (use_buffer_local_barriers) {
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = static_cast<u32>(pre_barriers.size()),
            .pBufferMemoryBarriers = pre_barriers.data(),
        });
    } else {
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, ReadBarrier, {}, {});
    }
    for (const auto& transfer : pending_buffer_transfers) {
        if (transfer.dependency_barrier_before) {
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                   vk::PipelineStageFlagBits::eTransfer,
                                   vk::DependencyFlagBits::eByRegion, DependencyBarrier, {}, {});
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            if (executor_vulkan_optimization_stats_enabled) {
                ++executor_buffer_transfer_dependency_barriers;
            }
#endif
        }
        cmdbuf.copyBuffer(transfer.source, transfer.destination, transfer.regions);
    }
    if (use_buffer_local_barriers) {
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = static_cast<u32>(post_barriers.size()),
            .pBufferMemoryBarriers = post_barriers.data(),
        });
    } else {
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, WriteBarrier, {}, {});
    }

    const u64 dependency_barriers =
        std::ranges::count_if(pending_buffer_transfers, [](const PendingBufferTransfer& transfer) {
            return transfer.dependency_barrier_before;
        });
    u64 emitted_copy_regions = 0;
    for (const PendingBufferTransfer& transfer : pending_buffer_transfers) {
        emitted_copy_regions += transfer.regions.size();
    }
    const u64 requested_barriers = pending_buffer_transfer_requests * 2 + dependency_barriers;
    const u64 emitted_barriers = 2 + dependency_barriers;
    VideoCore::RecordVulkanTransferFlush(pending_buffer_transfer_regions,
                                         pending_buffer_transfers.size());
    VideoCore::RecordVulkanBarrierBatch(requested_barriers, emitted_barriers);

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (executor_vulkan_optimization_stats_enabled) {
        ++executor_buffer_transfer_flushes;
        executor_buffer_transfer_copy_calls += pending_buffer_transfers.size();
        executor_buffer_transfer_copy_regions_emitted += emitted_copy_regions;
        executor_buffer_transfer_barriers_requested += requested_barriers;
        executor_buffer_transfer_barriers_emitted += emitted_barriers;
        if (!VideoCore::IsExecutorLiveQuiet() &&
            (executor_buffer_transfer_flushes == 1 ||
             (executor_buffer_transfer_flushes & 4095u) == 0u)) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_BUFFER_TRANSFER_BATCH] queued=%llu flushes=%llu copyCalls=%llu "
                "regions=requested:%llu,emitted:%llu pendingGroups=%zu maxGroups=%llu "
                "barriers=requested:%llu,emitted:%llu dependencyBarriers=%llu "
                "origin=h:%llu,u:%llu,g:%llu gnmRegions=%llu",
                static_cast<unsigned long long>(executor_buffer_transfers_queued),
                static_cast<unsigned long long>(executor_buffer_transfer_flushes),
                static_cast<unsigned long long>(executor_buffer_transfer_copy_calls),
                static_cast<unsigned long long>(executor_buffer_transfer_regions_queued),
                static_cast<unsigned long long>(executor_buffer_transfer_copy_regions_emitted),
                pending_buffer_transfers.size(),
                static_cast<unsigned long long>(executor_buffer_transfer_max_queued),
                static_cast<unsigned long long>(executor_buffer_transfer_barriers_requested),
                static_cast<unsigned long long>(executor_buffer_transfer_barriers_emitted),
                static_cast<unsigned long long>(executor_buffer_transfer_dependency_barriers),
                static_cast<unsigned long long>(executor_hle_buffer_transfers_queued),
                static_cast<unsigned long long>(executor_buffer_uploads_queued),
                static_cast<unsigned long long>(executor_gnm_dma_transfers_queued),
                static_cast<unsigned long long>(executor_gnm_dma_regions_queued));
        }
    }
#endif
    pending_buffer_transfers.clear();
    pending_buffer_transfer_requests = 0;
    pending_buffer_transfer_regions = 0;
    pending_gnm_dma_transfers = 0;
    pending_buffer_transfer_origins = 0;
}

void Scheduler::Flush(SubmitInfo& info) {
    SubmitExecution(info, ExecutorSubmitKind::FlushInfo);
}

void Scheduler::Flush() {
    if (!command_buffer_used && pending_buffer_transfers.empty() &&
        pending_ops_count.load(std::memory_order_acquire) == 0) [[likely]] {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (pending_submit_count.load(std::memory_order_acquire) != 0) {
            PublishPendingSubmitBatch();
        }
#endif
        return;
    }
    SubmitInfo info{};
    SubmitExecution(info, ExecutorSubmitKind::FlushPlain);
}

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
void Scheduler::PublishMemoryPressureBoundary() {
    if (master_semaphore.IsDeviceLost()) {
        return;
    }

    // FlushPlain may legally join the command buffer to the submit coalescer.
    // Memory pressure needs a real publication boundary so objects evicted by
    // the cache collectors can begin retiring while the guest keeps running.
    Flush();
    if (pending_submit_count.load(std::memory_order_acquire) != 0) {
        PublishPendingSubmitBatch();
    }

    // PublishPendingSubmitBatch observes the timeline once as part of submit.
    // Refresh once more after publication and reclaim only callbacks which the
    // GPU has already completed; unlike Finish/Wait this never blocks.
    PopPendingOperations(true);
}

void Scheduler::MaybePublishExactA750D9SampledRecordingPressureBoundary(
    const char* source) {
    const auto& profile = instance.GetCapabilityProfile();
    if (!profile.exact_mesa_turnip_a750_d9_lineage ||
        profile.memory_policy.residency_retirement !=
            ResidencyRetirementMode::CoordinatedSustainedNonBlocking ||
        master_semaphore.IsDeviceLost()) {
        return;
    }

    // This function is reached for every draw/dispatch on the exact D9
    // lineage. Usually Android's 10-second process-memory sample is unchanged,
    // so reject it with one acquire instead of copying the complete HUD stats
    // (roughly fifty atomic loads) on every work item.
    const u64 pending_sample = VideoCore::AndroidProcessMemorySampleSerial();
    if (pending_sample == 0 ||
        pending_sample == exact_a750_d9_last_recording_pressure_sample) {
        return;
    }
    const auto memory = VideoCore::GetAndroidProcessMemoryStats();
    const u64 sample = memory.sample_serial;
    if (sample == 0 || sample == exact_a750_d9_last_recording_pressure_sample) {
        return;
    }

    constexpr u64 MiB = 1024ULL * 1024ULL;
    const bool pressure = memory.graphics_bytes >= 1536 * MiB ||
                          memory.total_pss_bytes >= 4608 * MiB ||
                          (memory.available_memory_bytes != 0 &&
                           memory.available_memory_bytes <= 2304 * MiB);
    if (!pressure) {
        exact_a750_d9_last_recording_pressure_sample = sample;
        return;
    }
    // Do not consume the sample before there is an actual recording to close.
    // The next top-level work item in the same sample epoch can then publish it.
    if (!HasPendingWork()) {
        return;
    }

    // Never flush from an arbitrary draw/dispatch. On tiled Adreno that can
    // split a guest render pass between tile-store decisions and make later
    // frames alternate between preserved, partial and cleared attachments.
    // Reclaim only operations whose timeline values are already complete; the
    // next VideoOut boundary performs bounded, frame-safe backpressure.
    PopPendingOperations(true);
    exact_a750_d9_last_recording_pressure_sample = sample;
    ++exact_a750_d9_recording_pressure_boundary_count;
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_A750_D9_SAMPLED_RECORDING_ADMISSION] source=%s ordinal=%llu "
        "sample=%llu pssMiB=%llu graphicsMiB=%llu swapMiB=%llu availableMiB=%llu "
        "action=defer-to-videoout waited=0",
        source != nullptr ? source : "unknown",
        static_cast<unsigned long long>(exact_a750_d9_recording_pressure_boundary_count),
        static_cast<unsigned long long>(sample),
        static_cast<unsigned long long>(memory.total_pss_bytes / MiB),
        static_cast<unsigned long long>(memory.graphics_bytes / MiB),
        static_cast<unsigned long long>(memory.total_swap_bytes / MiB),
        static_cast<unsigned long long>(memory.available_memory_bytes / MiB));
}

#endif

namespace {
struct ClaudeSchedStats {
    std::atomic<u64> finish_ns[16]{};
    std::atomic<u64> finish_n[16]{};
    std::atomic<u64> wait_ns{}, wait_n{}, retire_ns{}, retire_n{}, submit_ns{}, submit_n{};
    std::atomic<u64> other_wait_ns{}, other_retire_ns{};
    std::atomic<long long> window_start_ms{};
};
ClaudeSchedStats g_claude_sched;
struct ClaudeCallerEntry {
    std::atomic<std::uintptr_t> address{};
    std::atomic<u64> ns{};
    std::atomic<u64> n{};
};
std::array<ClaudeCallerEntry, 32> g_claude_retire_callers{};
void ClaudeNoteRetireCaller(std::uintptr_t caller, u64 ns) {
    for (auto& e : g_claude_retire_callers) {
        std::uintptr_t expected = 0;
        if (e.address.load(std::memory_order_relaxed) == caller ||
            e.address.compare_exchange_strong(expected, caller)) {
            if (e.address.load(std::memory_order_relaxed) != caller) continue;
            e.ns.fetch_add(ns, std::memory_order_relaxed);
            e.n.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}
bool ClaudeSchedEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EXECUTOR_CLAUDE_SCHED_STATS");
        return value && value[0] == '1';
    }();
    return enabled;
}
thread_local int t_claude_gpucomm = -1;
bool ClaudeIsGpuComm() {
    if (t_claude_gpucomm < 0) {
        char name[64]{};
        pthread_getname_np(pthread_self(), name, sizeof(name));
        t_claude_gpucomm = std::strstr(name, "GpuCommand") != nullptr ? 1 : 0;
    }
    return t_claude_gpucomm == 1;
}
long long ClaudeNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
double ClaudeTakeMs(std::atomic<u64>& value) {
    return static_cast<double>(value.exchange(0, std::memory_order_relaxed)) / 1e6;
}
void ClaudeSchedMaybePrint() {
    const long long now = ClaudeNowMs();
    long long start = g_claude_sched.window_start_ms.load(std::memory_order_relaxed);
    if (start == 0) {
        g_claude_sched.window_start_ms.compare_exchange_strong(start, now);
        return;
    }
    if (now - start < 2000 ||
        !g_claude_sched.window_start_ms.compare_exchange_strong(start, now)) {
        return;
    }
    const double window = static_cast<double>(now - start);
    char reasons[512]{};
    int used = 0;
    for (int i = 0; i < 9; ++i) {
        const u64 n = g_claude_sched.finish_n[i].exchange(0, std::memory_order_relaxed);
        const double ms = ClaudeTakeMs(g_claude_sched.finish_ns[i]);
        if (n != 0 && used < 480) {
            used += std::snprintf(reasons + used, sizeof(reasons) - used, " finish%d=%llux%.1fms",
                                  i, static_cast<unsigned long long>(n), ms);
        }
    }
    const u64 wn = g_claude_sched.wait_n.exchange(0);
    const u64 rn = g_claude_sched.retire_n.exchange(0);
    const u64 sn = g_claude_sched.submit_n.exchange(0);
    const double wms = ClaudeTakeMs(g_claude_sched.wait_ns);
    const double rms = ClaudeTakeMs(g_claude_sched.retire_ns);
    const double sms = ClaudeTakeMs(g_claude_sched.submit_ns);
    const double owms = ClaudeTakeMs(g_claude_sched.other_wait_ns);
    const double orms = ClaudeTakeMs(g_claude_sched.other_retire_ns);
    {
        char callers[1024]{};
        int cused = 0;
        Dl_info self_info{};
        dladdr(reinterpret_cast<const void*>(&ClaudeSchedMaybePrint), &self_info);
        const auto base = reinterpret_cast<std::uintptr_t>(self_info.dli_fbase);
        for (auto& e : g_claude_retire_callers) {
            const auto address = e.address.load(std::memory_order_relaxed);
            const u64 n = e.n.exchange(0, std::memory_order_relaxed);
            const u64 ns = e.ns.exchange(0, std::memory_order_relaxed);
            if (address != 0 && n != 0 && cused < 960) {
                cused += std::snprintf(callers + cused, sizeof(callers) - cused, " 0x%llx:%llux%.1fms",
                                       static_cast<unsigned long long>(address - base + 0x100000000ull),
                                       static_cast<unsigned long long>(n), ns / 1e6);
            }
        }
        std::fprintf(stderr, "[CLAUDE_RETIRE_CALLERS]%s\n", callers);
    }
    std::fprintf(stderr,
                 "[CLAUDE_SCHED] windowMs=%.0f gpucomm: wait=%llux%.1fms(%.1f%%) "
                 "retire=%llux%.1fms(%.1f%%) submit=%llux%.1fms(%.1f%%)%s | other: wait=%.1fms "
                 "retire=%.1fms\n",
                 window, static_cast<unsigned long long>(wn), wms, 100.0 * wms / window,
                 static_cast<unsigned long long>(rn), rms, 100.0 * rms / window,
                 static_cast<unsigned long long>(sn), sms, 100.0 * sms / window, reasons, owms,
                 orms);
}
struct ClaudeSchedScope {
    std::atomic<u64>* ns;
    std::atomic<u64>* n;
    std::atomic<u64>* other_ns;
    std::chrono::steady_clock::time_point begin{};
    bool active;
    ClaudeSchedScope(std::atomic<u64>* ns_, std::atomic<u64>* n_, std::atomic<u64>* other_)
        : ns{ns_}, n{n_}, other_ns{other_}, active{ClaudeSchedEnabled()} {
        if (active) begin = std::chrono::steady_clock::now();
    }
    ~ClaudeSchedScope() {
        if (!active) return;
        const u64 elapsed = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 std::chrono::steady_clock::now() - begin)
                                                 .count());
        if (ClaudeIsGpuComm()) {
            ns->fetch_add(elapsed, std::memory_order_relaxed);
            if (n) n->fetch_add(1, std::memory_order_relaxed);
        } else if (other_ns) {
            other_ns->fetch_add(elapsed, std::memory_order_relaxed);
        }
        ClaudeSchedMaybePrint();
    }
};
} // namespace

void Scheduler::Finish(const ExecutorFinishReason reason) {
    const auto claude_reason = std::min<u32>(static_cast<u32>(reason), 15u);
    ClaudeSchedScope claude_scope{&g_claude_sched.finish_ns[claude_reason],
                                  &g_claude_sched.finish_n[claude_reason], nullptr};
    if (master_semaphore.IsDeviceLost()) {
        return;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool executor_finish_wait_profile_enabled = ExecutorFinishWaitProfileEnabled();
    const auto executor_finish_begin = executor_finish_wait_profile_enabled
                                           ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
    SCOPE_EXIT {
        if (executor_finish_wait_profile_enabled) {
            const u64 elapsed_ns =
                static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - executor_finish_begin)
                                     .count());
            const std::size_t reason_index = static_cast<std::size_t>(reason);
            ++executor_finish_wait_counts[reason_index];
            executor_finish_wait_ns[reason_index] += elapsed_ns;
            executor_finish_wait_max_ns[reason_index] =
                std::max(executor_finish_wait_max_ns[reason_index], elapsed_ns);
            const u64 ordinal = ++executor_finish_wait_total;
            if ((ordinal & 63u) == 0u) {
                const std::size_t wave_index =
                    static_cast<std::size_t>(ExecutorFinishReason::BufferReadbackWave);
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_VK_FINISH_WAIT] total=%llu reason=%u reasonCount=%llu "
                    "reasonMs=%.3f reasonMaxMs=%.3f bufferWaveCount=%llu bufferWaveMs=%.3f "
                    "bufferWaveMaxMs=%.3f",
                    static_cast<unsigned long long>(ordinal), static_cast<unsigned>(reason),
                    static_cast<unsigned long long>(executor_finish_wait_counts[reason_index]),
                    static_cast<double>(executor_finish_wait_ns[reason_index]) / 1'000'000.0,
                    static_cast<double>(executor_finish_wait_max_ns[reason_index]) / 1'000'000.0,
                    static_cast<unsigned long long>(executor_finish_wait_counts[wave_index]),
                    static_cast<double>(executor_finish_wait_ns[wave_index]) / 1'000'000.0,
                    static_cast<double>(executor_finish_wait_max_ns[wave_index]) / 1'000'000.0);
            }
        }
    };
#endif
    if (!command_buffer_used && pending_buffer_transfers.empty() &&
        pending_ops_count.load(std::memory_order_acquire) == 0) [[likely]] {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        const u64 pending_tick = pending_submit_last_tick.load(std::memory_order_acquire);
        if (pending_tick != 0) {
            PublishPendingSubmitBatch();
            master_semaphore.Wait(pending_tick);
            return;
        }
#endif
        const u64 current_tick = CurrentTick();
        if (current_tick > 1) {
            Wait(current_tick - 1);
        }
        return;
    }
    const u64 presubmit_tick = CurrentTick();
    SubmitInfo info{};
    SubmitExecution(info, ExecutorSubmitKind::Finish, reason);
    Wait(presubmit_tick);
}

void Scheduler::Wait(u64 tick) {
    ClaudeSchedScope claude_scope{&g_claude_sched.wait_ns, &g_claude_sched.wait_n,
                                  &g_claude_sched.other_wait_ns};
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    SchedSpan _probe_wait{1};
#endif
    if (master_semaphore.IsDeviceLost()) {
        return;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (pending_submit_count.load(std::memory_order_acquire) != 0 &&
        tick <= pending_submit_last_tick.load(std::memory_order_acquire)) {
        PublishPendingSubmitBatch();
        if (master_semaphore.IsDeviceLost()) {
            return;
        }
    }
#endif
    if (tick >= master_semaphore.CurrentTick()) {
        SubmitInfo info{};
        SubmitExecution(info, ExecutorSubmitKind::WaitCurrent);
    }
    master_semaphore.Wait(tick);
}

void Scheduler::WaitRetirement(u64 tick) {
    ClaudeSchedScope claude_scope{&g_claude_sched.retire_ns, &g_claude_sched.retire_n,
                                  &g_claude_sched.other_retire_ns};
    const auto claude_caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    const auto claude_begin = std::chrono::steady_clock::now();
    SCOPE_EXIT {
        if (ClaudeSchedEnabled()) {
            ClaudeNoteRetireCaller(claude_caller,
                static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - claude_begin).count()));
        }
    };
    if (tick == 0 || master_semaphore.IsDeviceLost()) {
        return;
    }
    {
        std::scoped_lock lk{retirement_pending_ops_mutex};
        const auto oldest = retirement_outstanding_ops.begin();
        if (oldest == retirement_outstanding_ops.end() || oldest->first > tick) {
            return;
        }
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (pending_submit_count.load(std::memory_order_acquire) != 0 &&
        tick <= pending_submit_last_tick.load(std::memory_order_acquire)) {
        PublishPendingSubmitBatch();
        if (master_semaphore.IsDeviceLost()) {
            return;
        }
    }
#endif
    {
        std::scoped_lock lk{retirement_pending_ops_mutex};
        const auto oldest = retirement_outstanding_ops.begin();
        if (oldest == retirement_outstanding_ops.end() || oldest->first > tick) {
            return;
        }
    }
    if (tick > retirement_last_published_tick.load(std::memory_order_acquire) &&
        tick >= master_semaphore.CurrentTick()) {
        SubmitInfo info{};
        SubmitExecution(info, ExecutorSubmitKind::WaitCurrent);
        if (master_semaphore.IsDeviceLost()) {
            return;
        }
    }
    retirement_pending_ops_cv.notify_all();
    std::unique_lock lk(retirement_pending_ops_mutex);
    while (!master_semaphore.IsDeviceLost()) {
        const auto oldest = retirement_outstanding_ops.begin();
        if (oldest == retirement_outstanding_ops.end() || oldest->first > tick) {
            break;
        }
        retirement_pending_ops_cv.wait_for(lk, std::chrono::milliseconds{10});
    }
}

void Scheduler::PopPendingOperations(bool force, bool timeline_current) {
    if (pending_ops_count.load(std::memory_order_acquire) == 0) [[likely]] {
        return;
    }

    if (!force &&
        (pending_ops_poll_sequence.fetch_add(1, std::memory_order_relaxed) & 0xffU) != 0) {
        return;
    }

    if (!timeline_current) {
        master_semaphore.Refresh();
    }
    if (master_semaphore.IsDeviceLost()) {
        return;
    }
    while (true) {
        Common::UniqueFunction<void> callback;
        {
            std::scoped_lock lk{pending_ops_mutex};
            if (pending_ops.empty() || !master_semaphore.IsFree(pending_ops.front().gpu_tick)) {
                break;
            }
            callback = std::move(pending_ops.front().callback);
            pending_ops.pop();
            const u64 previous_count = pending_ops_count.fetch_sub(1, std::memory_order_release);
            ASSERT_MSG(previous_count != 0,
                       "pending_ops_count underflow while draining deferred operation");
        }
        callback();
    }
}

void Scheduler::AllocateWorkerCommandBuffers() {
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };

    current_cmdbuf = command_pool.Commit();
    Check(current_cmdbuf.begin(begin_info));
    command_buffer_recording = true;
    command_buffer_used = false;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    gpu_timestamp_attempted = false;
    gpu_timestamp_active_slot = InvalidGpuTimestampSlot;
#endif

    dynamic_state.Invalidate();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        static const auto scope_loc =
            GPU_SCOPE_LOCATION("Guest Frame", MarkersPalette::GpuMarkerColor);
        new (profiler_scope) tracy::VkCtxScope{profiler_ctx, &scope_loc, current_cmdbuf, true};
    }
#endif
}

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
bool Scheduler::QueueSubmitLocked(const vk::ArrayProxy<const vk::SubmitInfo> submits,
                                  const vk::Fence fence, const std::span<const u64> logical_ticks,
                                  const u64 command_buffer_count, const bool forced_boundary) {
    ASSERT(!logical_ticks.empty());

    const bool executor_journal_active = ExecutorVkJournalWouldRecord();
    if (executor_journal_active) {
        for (const u64 tick : logical_ticks) {
            ExecutorVkJournalSubmit(this, tick, false, vk::Result::eSuccess);
        }
    }
    const bool executor_submit_latency_trace =
        executor_journal_active || ExecutorSubmitLatencyTraceEnabled();
    const auto executor_submit_begin = executor_submit_latency_trace
                                           ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};

    const bool legacy_timeline = !master_semaphore.UsesTimelineSemaphore();
    const vk::Fence tracking_fence =
        legacy_timeline ? master_semaphore.PrepareSubmitFence(logical_ticks.back()) : fence;
    if (legacy_timeline && !tracking_fence) {
        return false;
    }

    const bool gpu_timeline_enabled = ExecutorAndroidGpuTimelineEnabled();
    const bool record_gpu_fault_timeline = gpu_timeline_enabled || gpu_fault_journal_enabled;
    const bool submit_mutex_profile_enabled = ExecutorSubmitMutexProfileEnabled();
    u64 physical_submit_sequence = 0;
    u64 cpu_submit_begin_ns = 0;
    if (record_gpu_fault_timeline) {
        physical_submit_sequence = ++g_gpu_timeline_physical_submit_sequence;
        cpu_submit_begin_ns =
            static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count());
        if (gpu_timeline_enabled && (physical_submit_sequence & 7u) == 1u) {
            CalibratedTimestampPair pair{};
            const u64 cpu_before_ns = cpu_submit_begin_ns;
            const bool calibrated = instance.QueryCalibratedTimestamp(pair);
            const u64 cpu_after_ns =
                static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
            if (calibrated) {
                RecordGpuTimelineCalibration({
                    .sequence = physical_submit_sequence,
                    .cpu_before_ns = cpu_before_ns,
                    .cpu_after_ns = cpu_after_ns,
                    .device_ticks = pair.device_ticks,
                    .host_timestamp = pair.host_timestamp,
                    .max_deviation_ns = pair.max_deviation_ns,
                });
            }
        }
        {
            std::scoped_lock slot_lock{gpu_timestamp_slots_mutex};
            for (u32 tick_index = 0; tick_index < logical_ticks.size(); ++tick_index) {
                const u64 tick = logical_ticks[tick_index];
                for (auto& slot : gpu_timestamp_slots) {
                    if (slot.state != GpuTimestampSlotState::Pending || slot.logical_tick != tick ||
                        slot.physical_submit_sequence != 0) {
                        continue;
                    }
                    slot.physical_submit_sequence = physical_submit_sequence;
                    slot.physical_submit_index = tick_index;
                    slot.cpu_submit_begin_ns = cpu_submit_begin_ns;
                    break;
                }
            }
        }
    }
    const u64 submit_mutex_profile_begin_ns =
        submit_mutex_profile_enabled ? ExecutorSubmitMutexProfileNowNs() : 0;
    const vk::Result submit_result = instance.GetGraphicsQueue().submit(submits, tracking_fence);
    if (submit_mutex_profile_enabled) {
        const u64 submit_mutex_profile_end_ns = ExecutorSubmitMutexProfileNowNs();
        RecordSubmitMutexProfileQueueSubmitLocked(
            gpu_timeline_kind, submit_mutex_profile_end_ns - submit_mutex_profile_begin_ns,
            logical_ticks.size(), command_buffer_count, submit_result, forced_boundary, false);
    }
    const u64 cpu_submit_end_ns =
        record_gpu_fault_timeline
            ? static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count())
            : 0;
    if (record_gpu_fault_timeline) {
        {
            std::scoped_lock slot_lock{gpu_timestamp_slots_mutex};
            for (auto& slot : gpu_timestamp_slots) {
                if (slot.physical_submit_sequence == physical_submit_sequence) {
                    slot.cpu_submit_end_ns = cpu_submit_end_ns;
                }
            }
        }
        RecordGpuTimelineQueueSubmit({
            .sequence = physical_submit_sequence,
            .kind = gpu_timeline_kind,
            .first_tick = logical_ticks.front(),
            .last_tick = logical_ticks.back(),
            .cpu_begin_ns = cpu_submit_begin_ns,
            .cpu_end_ns = cpu_submit_end_ns,
            .logical_count = static_cast<u32>(logical_ticks.size()),
            .command_buffer_count = static_cast<u32>(command_buffer_count),
            .result = static_cast<s32>(submit_result),
            .forced_boundary = forced_boundary,
            .empty_fence_submit = false,
        });
    }
    VideoCore::RecordVulkanQueueSubmit(logical_ticks.size(), command_buffer_count, forced_boundary);

    vk::Result external_fence_result = vk::Result::eSuccess;
    if (submit_result == vk::Result::eSuccess && legacy_timeline && fence) {
        // A Vulkan submission accepts one fence. Keep the master fence on the
        // real work and signal the presenter's externally-owned fence with an
        // ordered empty submit on the same queue.
        const u64 empty_sequence =
            record_gpu_fault_timeline ? ++g_gpu_timeline_physical_submit_sequence : 0;
        const u64 empty_begin_ns =
            record_gpu_fault_timeline
                ? static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count())
                : 0;
        const u64 empty_submit_profile_begin_ns =
            submit_mutex_profile_enabled ? ExecutorSubmitMutexProfileNowNs() : 0;
        external_fence_result = instance.GetGraphicsQueue().submit(vk::SubmitInfo{}, fence);
        if (submit_mutex_profile_enabled) {
            const u64 empty_submit_profile_end_ns = ExecutorSubmitMutexProfileNowNs();
            RecordSubmitMutexProfileQueueSubmitLocked(
                gpu_timeline_kind, empty_submit_profile_end_ns - empty_submit_profile_begin_ns, 0,
                0, external_fence_result, true, true);
        }
        if (record_gpu_fault_timeline) {
            const u64 empty_end_ns =
                static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
            RecordGpuTimelineQueueSubmit({
                .sequence = empty_sequence,
                .kind = gpu_timeline_kind,
                .first_tick = logical_ticks.back(),
                .last_tick = logical_ticks.back(),
                .cpu_begin_ns = empty_begin_ns,
                .cpu_end_ns = empty_end_ns,
                .logical_count = 1,
                .command_buffer_count = 0,
                .result = static_cast<s32>(external_fence_result),
                .forced_boundary = true,
                .empty_fence_submit = true,
            });
        }
        VideoCore::RecordVulkanQueueSubmit(1, 0, true);
    }

    if (ExecutorVulkanOptimizationStatsEnabled()) {
        ++executor_submit_batches;
        executor_submit_logical_packets += logical_ticks.size();
        executor_submit_command_buffers += command_buffer_count;
        executor_submit_calls_saved += logical_ticks.size() > 1 ? logical_ticks.size() - 1 : 0;
        executor_submit_forced_boundaries += forced_boundary ? 1 : 0;
        executor_submit_max_batch =
            std::max(executor_submit_max_batch, static_cast<u64>(logical_ticks.size()));
        if (!VideoCore::IsExecutorLiveQuiet() &&
            (executor_submit_batches == 1 || (executor_submit_batches & 255u) == 0u)) {
            __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                                "[EXECUTOR_VK_SUBMIT_COALESCING] physical=%llu logical=%llu "
                                "commandBuffers=%llu callsSaved=%llu forced=%llu timer=%llu "
                                "maxBatch=%llu currentBatch=%zu currentSubmitInfos=%u",
                                static_cast<unsigned long long>(executor_submit_batches),
                                static_cast<unsigned long long>(executor_submit_logical_packets),
                                static_cast<unsigned long long>(executor_submit_command_buffers),
                                static_cast<unsigned long long>(executor_submit_calls_saved),
                                static_cast<unsigned long long>(executor_submit_forced_boundaries),
                                static_cast<unsigned long long>(executor_submit_timer_flushes),
                                static_cast<unsigned long long>(executor_submit_max_batch),
                                logical_ticks.size(), static_cast<u32>(submits.size()));
        }
    }

    if (executor_submit_latency_trace) {
        const u64 executor_submit_ns =
            static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - executor_submit_begin)
                                 .count());
        ++executor_submit_latency_samples;
        executor_submit_latency_total_ns += executor_submit_ns;
        executor_submit_latency_max_ns =
            std::max(executor_submit_latency_max_ns, executor_submit_ns);
        if ((executor_submit_latency_samples & 1023u) == 0u) {
            __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                                "[EXECUTOR_VK_SUBMIT_LATENCY] samples=1024 totalMs=%.3f "
                                "avgUs=%.3f maxMs=%.3f",
                                static_cast<double>(executor_submit_latency_total_ns) / 1'000'000.0,
                                static_cast<double>(executor_submit_latency_total_ns) / 1'024'000.0,
                                static_cast<double>(executor_submit_latency_max_ns) / 1'000'000.0);
            executor_submit_latency_total_ns = 0;
            executor_submit_latency_max_ns = 0;
        }
    }
    if (executor_journal_active) {
        for (const u64 tick : logical_ticks) {
            ExecutorVkJournalSubmit(this, tick, true, submit_result);
        }
    }

    const u64 final_tick = logical_ticks.back();
    const vk::Result final_submit_result =
        submit_result != vk::Result::eSuccess ? submit_result : external_fence_result;
    if (final_submit_result != vk::Result::eSuccess) [[unlikely]] {
        if (final_submit_result == vk::Result::eErrorDeviceLost) {
            DumpGpuTimestampFaultState(final_tick, final_submit_result);
            instance.DumpDeviceFaultInfo("queue_submit", final_tick);
        }
        ExecutorVkJournalDump("queue_submit", final_tick);
        MarkTerminal(final_submit_result, "queue_submit");
        if (final_submit_result != vk::Result::eErrorDeviceLost) {
            LOG_ERROR(Render_Vulkan, "Failed to submit Vulkan command buffer batch: {}",
                      vk::to_string(final_submit_result));
        }
        return false;
    }

    PublishRetirementTick(final_tick);

    static const bool serialize_gnm_submits =
        std::getenv("EXECUTOR_ANDROID_SERIALIZE_GNM_SUBMITS") != nullptr;
    if (serialize_gnm_submits) {
        master_semaphore.Wait(final_tick);
        if (master_semaphore.IsDeviceLost()) {
            return false;
        }
    }

    master_semaphore.Refresh();
    if (master_semaphore.IsDeviceLost()) {
        return false;
    }
    return true;
}

bool Scheduler::SubmitPendingBatchLocked(const vk::SubmitInfo* trailing_submit,
                                         const vk::Fence trailing_fence, const u64 trailing_tick,
                                         const bool forced_boundary) {
    const bool has_pending = !pending_submit_command_buffers.empty();
    if (!has_pending && trailing_submit == nullptr) {
        return true;
    }
    if (master_semaphore.IsDeviceLost()) {
        pending_submit_command_buffers.clear();
        pending_submit_ticks.clear();
        pending_submit_count.store(0, std::memory_order_release);
        pending_submit_last_tick.store(0, std::memory_order_release);
        pending_submit_deadline = {};
        return false;
    }

    if (!has_pending) {
        const std::array<u64, 1> ticks{trailing_tick};
        return QueueSubmitLocked(vk::ArrayProxy<const vk::SubmitInfo>{1, trailing_submit},
                                 trailing_fence, ticks, 1, forced_boundary);
    }

    const vk::Semaphore timeline = master_semaphore.Handle();
    const std::array<vk::Semaphore, 1> signal_semas{timeline};
    const std::array<u64, 1> signal_ticks{pending_submit_ticks.back()};
    const vk::TimelineSemaphoreSubmitInfo timeline_info = {
        .signalSemaphoreValueCount = 1,
        .pSignalSemaphoreValues = signal_ticks.data(),
    };
    vk::SubmitInfo pending_submit = {
        .commandBufferCount = static_cast<u32>(pending_submit_command_buffers.size()),
        .pCommandBuffers = pending_submit_command_buffers.data(),
    };
    if (master_semaphore.UsesTimelineSemaphore()) {
        pending_submit.pNext = &timeline_info;
        pending_submit.signalSemaphoreCount = 1;
        pending_submit.pSignalSemaphores = signal_semas.data();
    }

    boost::container::small_vector<u64, 9> logical_ticks{pending_submit_ticks.begin(),
                                                         pending_submit_ticks.end()};
    const u64 pending_command_buffers = pending_submit_command_buffers.size();
    bool success = false;
    if (trailing_submit != nullptr) {
        logical_ticks.push_back(trailing_tick);
        const std::array<vk::SubmitInfo, 2> submits{pending_submit, *trailing_submit};
        success = QueueSubmitLocked(submits, trailing_fence, logical_ticks,
                                    pending_command_buffers + 1, forced_boundary);
    } else {
        success = QueueSubmitLocked(vk::ArrayProxy<const vk::SubmitInfo>{1, &pending_submit}, {},
                                    logical_ticks, pending_command_buffers, forced_boundary);
    }

    pending_submit_command_buffers.clear();
    pending_submit_ticks.clear();
    pending_submit_count.store(0, std::memory_order_release);
    pending_submit_last_tick.store(0, std::memory_order_release);
    pending_submit_deadline = {};
    pending_submit_cv.notify_all();
    return success;
}

void Scheduler::PublishPendingSubmitBatch() {
    SubmitMutexProfileLock lk{submit_mutex, gpu_timeline_kind};
    if (SubmitPendingBatchLocked(nullptr, {}, 0, true)) {
        PopPendingOperations(true, true);
    }
}

void Scheduler::PendingSubmitThread(const std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:VkSubmitCoalescer");
    SubmitMutexProfileReacquireMutex profiled_submit_mutex{submit_mutex};
    std::unique_lock lk{profiled_submit_mutex};
    while (!stoken.stop_requested() && !master_semaphore.IsDeviceLost()) {
        if (pending_submit_command_buffers.empty()) {
            pending_submit_cv.wait(lk, stoken, [this] {
                return master_semaphore.IsDeviceLost() || !pending_submit_command_buffers.empty();
            });
            continue;
        }
        const auto deadline = pending_submit_deadline;
        if (std::chrono::steady_clock::now() < deadline) {
            pending_submit_cv.wait_until(lk, deadline);
            continue;
        }
        if (ExecutorVulkanOptimizationStatsEnabled()) {
            ++executor_submit_timer_flushes;
        }
        // Deferred-operation callbacks historically run on the scheduler's
        // caller thread. The timer only publishes GPU work; the next owner
        // interaction drains callbacks after observing timeline retirement.
        SubmitPendingBatchLocked();
        if (profiled_submit_mutex.Enabled()) {
            const u64 hold_end_ns = ExecutorSubmitMutexProfileNowNs();
            const auto snapshots = FinishSubmitMutexProfileLockLocked(
                gpu_timeline_kind, profiled_submit_mutex.WaitNs(),
                hold_end_ns - profiled_submit_mutex.AcquiredNs());
            const bool should_publish = std::any_of(
                snapshots.begin(), snapshots.end(),
                [](const SubmitMutexProfileSnapshot& snapshot) { return snapshot.valid; });
            if (should_publish) {
                lk.unlock();
                for (const auto& snapshot : snapshots) {
                    PublishSubmitMutexProfileSnapshot(snapshot);
                }
                lk.lock();
            }
        }
    }
}
#endif

void Scheduler::SubmitExecution(SubmitInfo& info, ExecutorSubmitKind kind,
                                const ExecutorFinishReason finish_reason) {
    ClaudeSchedScope claude_scope{&g_claude_sched.submit_ns, &g_claude_sched.submit_n, nullptr};
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    SchedSpan _probe_submit{0};
    SubmitMutexProfileLock lk{submit_mutex, gpu_timeline_kind};
    SubmitExecutionProfileSample submit_execution_profile{
        .enabled = lk.Enabled() && gpu_timeline_kind == GpuTimelineSchedulerKind::GuestDraw,
        .hold_begin_ns = lk.AcquiredNs(),
        .submit_kind_index = static_cast<std::size_t>(kind),
    };
    SCOPE_EXIT {
        RecordSubmitExecutionProfileLocked(submit_execution_profile);
    };
#else
    std::scoped_lock lk{submit_mutex};
#endif
    if (master_semaphore.IsDeviceLost()) {
        return;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    {
        SubmitExecutionProfileStageScope stage{
            submit_execution_profile, SubmitExecutionProfileStage::FlushPendingBufferTransfers};
#endif
        FlushPendingBufferTransfers();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    }
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool executor_submit_reasons_enabled = ExecutorSubmitReasonsEnabled();
    if (executor_submit_reasons_enabled) {
        ++executor_submit_kind_counts[static_cast<std::size_t>(kind)];
        if (kind == ExecutorSubmitKind::Finish) {
            ++executor_finish_reason_counts[static_cast<std::size_t>(finish_reason)];
        }
    }
    const u64 executor_submit_kind_ordinal =
        executor_submit_reasons_enabled ? ++executor_submit_kind_total : 0;
    if (executor_submit_reasons_enabled && (executor_submit_kind_ordinal & 255u) == 0u) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_VK_SUBMIT_REASONS] scheduler=%p total=%llu flushInfo=%llu "
            "flushPlain=%llu finish=%llu waitCurrent=%llu finishUnknown=%llu "
            "finishBufWave=%llu finishBufDirect=%llu finishBufProbe=%llu "
            "finishTexRead=%llu finishTexProbe=%llu finishRasterizer=%llu "
            "finishPresenter=%llu",
            this, static_cast<unsigned long long>(executor_submit_kind_ordinal),
            static_cast<unsigned long long>(executor_submit_kind_counts[static_cast<std::size_t>(
                ExecutorSubmitKind::FlushInfo)]),
            static_cast<unsigned long long>(executor_submit_kind_counts[static_cast<std::size_t>(
                ExecutorSubmitKind::FlushPlain)]),
            static_cast<unsigned long long>(
                executor_submit_kind_counts[static_cast<std::size_t>(ExecutorSubmitKind::Finish)]),
            static_cast<unsigned long long>(executor_submit_kind_counts[static_cast<std::size_t>(
                ExecutorSubmitKind::WaitCurrent)]),
            static_cast<unsigned long long>(executor_finish_reason_counts[static_cast<std::size_t>(
                ExecutorFinishReason::Unknown)]),
            static_cast<unsigned long long>(executor_finish_reason_counts[static_cast<std::size_t>(
                ExecutorFinishReason::BufferReadbackWave)]),
            static_cast<unsigned long long>(executor_finish_reason_counts[static_cast<std::size_t>(
                ExecutorFinishReason::BufferReadbackDirect)]),
            static_cast<unsigned long long>(executor_finish_reason_counts[static_cast<std::size_t>(
                ExecutorFinishReason::BufferProbe)]),
            static_cast<unsigned long long>(executor_finish_reason_counts[static_cast<std::size_t>(
                ExecutorFinishReason::TextureReadback)]),
            static_cast<unsigned long long>(executor_finish_reason_counts[static_cast<std::size_t>(
                ExecutorFinishReason::TextureProbe)]),
            static_cast<unsigned long long>(executor_finish_reason_counts[static_cast<std::size_t>(
                ExecutorFinishReason::Rasterizer)]),
            static_cast<unsigned long long>(executor_finish_reason_counts[static_cast<std::size_t>(
                ExecutorFinishReason::PresenterShutdown)]));
    }
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool managed_submit_coalescing = VideoCore::IsManagedVulkanSubmitCoalescing();
    static const bool serialize_gnm_submits =
        std::getenv("EXECUTOR_ANDROID_SERIALIZE_GNM_SUBMITS") != nullptr;
    const bool has_external_sync = info.num_wait_semas != 0 || info.num_signal_semas != 0 ||
                                   static_cast<VkFence>(info.fence) != VK_NULL_HANDLE;
    const bool can_defer_internal_submit = managed_submit_coalescing && !serialize_gnm_submits &&
                                           kind == ExecutorSubmitKind::FlushPlain &&
                                           !has_external_sync;
    if (submit_execution_profile.enabled) {
        submit_execution_profile.has_external_sync = has_external_sync;
        submit_execution_profile.can_defer = can_defer_internal_submit;
    }
#endif
    const u64 signal_value = master_semaphore.NextTick();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        profiler_scope->~VkCtxScope();
        TracyVkCollect(profiler_ctx, current_cmdbuf);
    }
#endif

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    {
        SubmitExecutionProfileStageScope stage{
            submit_execution_profile, SubmitExecutionProfileStage::EndRenderingAndCommandBufferEnd};
        RecordGpuFaultPreEndCheckpoint();
#endif
        EndRendering(kind == ExecutorSubmitKind::FlushInfo || kind == ExecutorSubmitKind::FlushPlain
                         ? EndRenderingReason::Flush
                         : EndRenderingReason::Submit);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (gpu_timestamp_enabled) {
            FinishGpuTimestampSample(signal_value);
        }
#endif
        if (PortableGpu::ProducerReadbackEnabled() && !Config::managedGpuFastPath()) {
            // Once per producing command buffer, outside dynamic rendering.
            const vk::MemoryBarrier2 host = {
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask = vk::AccessFlagBits2::eHostRead,
            };
            current_cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                .memoryBarrierCount = 1, .pMemoryBarriers = &host});
            Check(current_cmdbuf.end());
            // This is recorded visibility evidence, not completion. Admission
            // additionally requires this exact scheduler's KnownGpuTick.
            PortableGpu::RecordProducerHostBarrier(this,signal_value);
        } else {
            Check(current_cmdbuf.end());
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    }
#endif
    command_buffer_recording = false;
    const bool uses_timeline = master_semaphore.UsesTimelineSemaphore();
    const vk::Semaphore timeline = master_semaphore.Handle();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (uses_timeline && signal_value > 1 && !managed_submit_coalescing) {
        const u64 preceding_signal = signal_value - 1;
        bool merged_wait = false;
        for (u32 index = 0; index < info.num_wait_semas; ++index) {
            if (info.wait_semas[index] != timeline) {
                continue;
            }
            info.wait_ticks[index] = std::max(info.wait_ticks[index], preceding_signal);
            info.wait_stage_masks[index] |= vk::PipelineStageFlagBits::eAllCommands;
            merged_wait = true;
            break;
        }
        if (!merged_wait) {
            info.AddWait(timeline, preceding_signal);
        }
    }
#endif
    if (uses_timeline) {
        info.AddSignal(timeline, signal_value);
    }

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = info.num_wait_semas,
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = info.num_signal_semas,
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };

    const vk::SubmitInfo submit_info = {
        .pNext = uses_timeline ? &timeline_si : nullptr,
        .waitSemaphoreCount = info.num_wait_semas,
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = info.wait_stage_masks.data(),
        .commandBufferCount = 1U,
        .pCommandBuffers = &current_cmdbuf,
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };

    if (instance.UseHostUiComposition()) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        SubmitExecutionProfileStageScope stage{submit_execution_profile,
                                               SubmitExecutionProfileStage::ImGuiTextureSubmit};
#endif
        ImGui::Core::TextureManager::Submit();
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (can_defer_internal_submit) {
        if (submit_execution_profile.enabled) {
            submit_execution_profile.deferred = true;
        }
        constexpr std::size_t MaxCoalescedCommandBuffers = 16;
        // Keep collection and vkQueueSubmit on this scheduler thread.  The
        // former timer thread could submit one command buffer while Adreno
        // was recording the next buffer from the same pool, which triggered
        // a driver race in texture detiling and normally expired at batch=1.
        // A synchronous frame/boundary batch has no concurrent queue call;
        // explicit waits, external sync and presentation publish it through
        // the existing forced-boundary path.
        pending_submit_command_buffers.push_back(current_cmdbuf);
        pending_submit_ticks.push_back(signal_value);
        pending_submit_last_tick.store(signal_value, std::memory_order_release);
        pending_submit_count.store(static_cast<u32>(pending_submit_command_buffers.size()),
                                   std::memory_order_release);

        bool success = true;
        if (pending_submit_command_buffers.size() >= MaxCoalescedCommandBuffers) {
            if (submit_execution_profile.enabled) {
                submit_execution_profile.pending_batch_submit = true;
            }
            SubmitExecutionProfileStageScope stage{submit_execution_profile,
                                                   SubmitExecutionProfileStage::QueueSubmitPath};
            success = SubmitPendingBatchLocked();
        }
        if (!success || master_semaphore.IsDeviceLost()) {
            return;
        }
        {
            SubmitExecutionProfileStageScope stage{
                submit_execution_profile,
                SubmitExecutionProfileStage::AllocateWorkerCommandBuffers};
            AllocateWorkerCommandBuffers();
        }
        if (pending_submit_command_buffers.empty()) {
            SubmitExecutionProfileStageScope stage{
                submit_execution_profile, SubmitExecutionProfileStage::PopPendingOperations};
            PopPendingOperations(true, true);
        }
        return;
    }

    const std::array<u64, 1> logical_ticks{signal_value};
    const bool has_pending_batch = !pending_submit_command_buffers.empty();
    if (submit_execution_profile.enabled) {
        submit_execution_profile.direct_submit = !has_pending_batch;
        submit_execution_profile.pending_batch_submit = has_pending_batch;
        submit_execution_profile.forced_boundary = has_pending_batch;
    }
    bool success = false;
    {
        SubmitExecutionProfileStageScope stage{submit_execution_profile,
                                               SubmitExecutionProfileStage::QueueSubmitPath};
        success = has_pending_batch
                      ? SubmitPendingBatchLocked(&submit_info, info.fence, signal_value, true)
                      : QueueSubmitLocked(vk::ArrayProxy<const vk::SubmitInfo>{1, &submit_info},
                                          info.fence, logical_ticks, 1, false);
    }
    if (!success || master_semaphore.IsDeviceLost()) {
        return;
    }
    {
        SubmitExecutionProfileStageScope stage{
            submit_execution_profile, SubmitExecutionProfileStage::AllocateWorkerCommandBuffers};
        AllocateWorkerCommandBuffers();
    }
    {
        SubmitExecutionProfileStageScope stage{submit_execution_profile,
                                               SubmitExecutionProfileStage::PopPendingOperations};
        PopPendingOperations(true, true);
    }
#else
    const vk::Result submit_result = instance.GetGraphicsQueue().submit(submit_info, info.fence);
    VideoCore::RecordVulkanQueueSubmit();
    if (submit_result != vk::Result::eSuccess) [[unlikely]] {
        MarkTerminal(submit_result, "queue_submit");
        if (submit_result != vk::Result::eErrorDeviceLost) {
            LOG_ERROR(Render_Vulkan, "Failed to submit Vulkan command buffer: {}",
                      vk::to_string(submit_result));
        }
        return;
    }
    PublishRetirementTick(signal_value);
    master_semaphore.Refresh();
    if (master_semaphore.IsDeviceLost()) {
        return;
    }
    AllocateWorkerCommandBuffers();

    PopPendingOperations(true, true);
#endif
}

void Scheduler::PriorityPendingOpsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSchedPriorityPendingOpsRunner");
#if defined(__APPLE__)
    {
        const char* claude_value = std::getenv("EXECUTOR_CLAUDE_RETIRE_QOS");
        if (claude_value && claude_value[0] == '1') {
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        }
    }
#endif

    while (!stoken.stop_requested()) {
        PendingOp op;
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops_cv.wait(lk, stoken, [this] {
                return master_semaphore.IsDeviceLost() ||
                       (!priority_pending_ops.empty() &&
                        priority_pending_ops.front().gpu_tick <=
                            retirement_last_published_tick.load(std::memory_order_acquire));
            });
            if (stoken.stop_requested() || master_semaphore.IsDeviceLost()) {
                break;
            }

            op = std::move(priority_pending_ops.front());
            priority_pending_ops.pop();
        }

        master_semaphore.Wait(op.gpu_tick);
        if (stoken.stop_requested() || master_semaphore.IsDeviceLost()) {
            break;
        }

        op.callback();
    }
}

void Scheduler::RetirementPendingOpsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSchedRetirementOpsRunner");
#if defined(__APPLE__)
    {
        const char* claude_value = std::getenv("EXECUTOR_CLAUDE_RETIRE_QOS");
        if (claude_value && claude_value[0] == '1') {
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        }
    }
#endif

    std::vector<PendingOp> batch;
    batch.reserve(256);
    while (!stoken.stop_requested()) {
        batch.clear();
        u64 oldest_tick{};
        {
            std::unique_lock lk(retirement_pending_ops_mutex);
            retirement_pending_ops_cv.wait(lk, stoken, [this] {
                return master_semaphore.IsDeviceLost() ||
                       (!retirement_pending_ops.empty() &&
                        retirement_pending_ops.begin()->first <=
                            retirement_last_published_tick.load(std::memory_order_acquire));
            });
            if (stoken.stop_requested() || master_semaphore.IsDeviceLost()) {
                break;
            }
            oldest_tick = retirement_pending_ops.begin()->first;
        }

        master_semaphore.Refresh();
        if (stoken.stop_requested() || master_semaphore.IsDeviceLost()) {
            break;
        }

        const u64 known_gpu_tick = master_semaphore.KnownGpuTick();
        if (known_gpu_tick < oldest_tick) {
            static const int claude_retire_block = [] {
                const char* value = std::getenv("EXECUTOR_CLAUDE_RETIRE_BLOCK");
                return value ? std::atoi(value) : 0;
            }();
            const bool block = claude_retire_block == 1 ||
                (claude_retire_block == 2 &&
                 (static_cast<long long>(std::time(nullptr)) / 5) % 2 == 0);
            if (block) {
                // oldest_tick is already published to the GPU: wake on its
                // completion instead of re-polling every 250 us.
                master_semaphore.Wait(oldest_tick);
                continue;
            }
            std::unique_lock lk(retirement_pending_ops_mutex);
            retirement_pending_ops_cv.wait_for(
                lk, stoken, std::chrono::microseconds{250}, [this, oldest_tick] {
                    return master_semaphore.IsDeviceLost() || retirement_pending_ops.empty() ||
                           retirement_pending_ops.begin()->first < oldest_tick;
                });
            continue;
        }

        u64 completed_tick{};
        {
            std::unique_lock lk(retirement_pending_ops_mutex);
            auto completed = retirement_pending_ops.begin();
            if (completed == retirement_pending_ops.end() || completed->first > known_gpu_tick) {
                continue;
            }
            completed_tick = completed->first;
            auto& tick_ops = completed->second;
            while (!tick_ops.empty()) {
                batch.emplace_back(std::move(tick_ops.front()));
                tick_ops.pop();
            }
            retirement_pending_ops.erase(completed);
        }

        for (auto& op : batch) {
            if (stoken.stop_requested() || master_semaphore.IsDeviceLost()) {
                break;
            }
            op.callback();
        }
        {
            std::scoped_lock lk{retirement_pending_ops_mutex};
            auto outstanding = retirement_outstanding_ops.find(completed_tick);
            ASSERT_MSG(outstanding != retirement_outstanding_ops.end() &&
                           outstanding->second >= batch.size(),
                       "Retirement callback accounting underflow at tick {}", completed_tick);
            outstanding->second -= batch.size();
            const bool tick_drained = outstanding->second == 0;
            if (tick_drained) {
                retirement_outstanding_ops.erase(outstanding);
            }
            const u64 previous_outstanding =
                retirement_outstanding_count.fetch_sub(batch.size(), std::memory_order_acq_rel);
            ASSERT_MSG(previous_outstanding >= batch.size(),
                       "Global retirement callback accounting underflow");
            if (tick_drained) {
                u64 completed_value = retirement_completed_tick.load(std::memory_order_relaxed);
                while (completed_value < completed_tick &&
                       !retirement_completed_tick.compare_exchange_weak(
                           completed_value, completed_tick, std::memory_order_release,
                           std::memory_order_relaxed)) {
                }
            }
        }
        retirement_pending_ops_cv.notify_all();
    }
    {
        std::scoped_lock lk{retirement_pending_ops_mutex};
    }
    retirement_pending_ops_cv.notify_all();
}

void DynamicState::Commit(const Instance& instance, const vk::CommandBuffer& cmdbuf) {
    const bool legacy_render_pass = instance.UseLegacyRenderPass();
    if (dirty_state.viewports) {
        dirty_state.viewports = false;
        if (legacy_render_pass) {
            cmdbuf.setViewport(0, viewports);
        } else {
            cmdbuf.setViewportWithCount(viewports);
        }
    }
    if (dirty_state.scissors) {
        dirty_state.scissors = false;
        if (legacy_render_pass) {
            cmdbuf.setScissor(0, scissors);
        } else {
            cmdbuf.setScissorWithCount(scissors);
        }
    }
    if (dirty_state.depth_test_enabled) {
        dirty_state.depth_test_enabled = false;
        if (!legacy_render_pass) {
            cmdbuf.setDepthTestEnable(depth_test_enabled);
        }
    }
    if (dirty_state.depth_write_enabled) {
        dirty_state.depth_write_enabled = false;
        if (!legacy_render_pass) {
            cmdbuf.setDepthWriteEnable(depth_write_enabled);
        }
    }
    if (depth_test_enabled && dirty_state.depth_compare_op) {
        dirty_state.depth_compare_op = false;
        if (!legacy_render_pass) {
            cmdbuf.setDepthCompareOp(depth_compare_op);
        }
    }
    if (dirty_state.depth_bounds_test_enabled) {
        dirty_state.depth_bounds_test_enabled = false;
        if (!legacy_render_pass && instance.IsDepthBoundsSupported()) {
            cmdbuf.setDepthBoundsTestEnable(depth_bounds_test_enabled);
        }
    }
    if (depth_bounds_test_enabled && dirty_state.depth_bounds) {
        dirty_state.depth_bounds = false;
        if (instance.IsDepthBoundsSupported()) {
            cmdbuf.setDepthBounds(depth_bounds_min, depth_bounds_max);
        }
    }
    if (dirty_state.depth_bias_enabled) {
        dirty_state.depth_bias_enabled = false;
        if (!legacy_render_pass) {
            cmdbuf.setDepthBiasEnable(depth_bias_enabled);
        }
    }
    if (depth_bias_enabled && dirty_state.depth_bias) {
        dirty_state.depth_bias = false;
        cmdbuf.setDepthBias(depth_bias_constant, depth_bias_clamp, depth_bias_slope);
    }
    if (dirty_state.stencil_test_enabled) {
        dirty_state.stencil_test_enabled = false;
        if (!legacy_render_pass) {
            cmdbuf.setStencilTestEnable(stencil_test_enabled);
        }
    }
    if (stencil_test_enabled) {
        if (dirty_state.stencil_front_ops && dirty_state.stencil_back_ops &&
            stencil_front_ops == stencil_back_ops) {
            dirty_state.stencil_front_ops = false;
            dirty_state.stencil_back_ops = false;
            if (!legacy_render_pass) {
                cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFrontAndBack,
                                    stencil_front_ops.fail_op, stencil_front_ops.pass_op,
                                    stencil_front_ops.depth_fail_op, stencil_front_ops.compare_op);
            }
        } else {
            if (dirty_state.stencil_front_ops) {
                dirty_state.stencil_front_ops = false;
                if (!legacy_render_pass) {
                    cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFront, stencil_front_ops.fail_op,
                                        stencil_front_ops.pass_op, stencil_front_ops.depth_fail_op,
                                        stencil_front_ops.compare_op);
                }
            }
            if (dirty_state.stencil_back_ops) {
                dirty_state.stencil_back_ops = false;
                if (!legacy_render_pass) {
                    cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eBack, stencil_back_ops.fail_op,
                                        stencil_back_ops.pass_op, stencil_back_ops.depth_fail_op,
                                        stencil_back_ops.compare_op);
                }
            }
        }
        if (dirty_state.stencil_front_reference && dirty_state.stencil_back_reference &&
            stencil_front_reference == stencil_back_reference) {
            dirty_state.stencil_front_reference = false;
            dirty_state.stencil_back_reference = false;
            cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack,
                                       stencil_front_reference);
        } else {
            if (dirty_state.stencil_front_reference) {
                dirty_state.stencil_front_reference = false;
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFront,
                                           stencil_front_reference);
            }
            if (dirty_state.stencil_back_reference) {
                dirty_state.stencil_back_reference = false;
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eBack, stencil_back_reference);
            }
        }
        if (dirty_state.stencil_front_write_mask && dirty_state.stencil_back_write_mask &&
            stencil_front_write_mask == stencil_back_write_mask) {
            dirty_state.stencil_front_write_mask = false;
            dirty_state.stencil_back_write_mask = false;
            cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                       stencil_front_write_mask);
        } else {
            if (dirty_state.stencil_front_write_mask) {
                dirty_state.stencil_front_write_mask = false;
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFront,
                                           stencil_front_write_mask);
            }
            if (dirty_state.stencil_back_write_mask) {
                dirty_state.stencil_back_write_mask = false;
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eBack, stencil_back_write_mask);
            }
        }
        if (dirty_state.stencil_front_compare_mask && dirty_state.stencil_back_compare_mask &&
            stencil_front_compare_mask == stencil_back_compare_mask) {
            dirty_state.stencil_front_compare_mask = false;
            dirty_state.stencil_back_compare_mask = false;
            cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                         stencil_front_compare_mask);
        } else {
            if (dirty_state.stencil_front_compare_mask) {
                dirty_state.stencil_front_compare_mask = false;
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFront,
                                             stencil_front_compare_mask);
            }
            if (dirty_state.stencil_back_compare_mask) {
                dirty_state.stencil_back_compare_mask = false;
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eBack,
                                             stencil_back_compare_mask);
            }
        }
    }
    if (dirty_state.primitive_restart_enable) {
        dirty_state.primitive_restart_enable = false;
        if (!legacy_render_pass) {
            cmdbuf.setPrimitiveRestartEnable(primitive_restart_enable);
        }
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    dirty_state.rasterizer_discard_enable = false;
#else
    if (dirty_state.rasterizer_discard_enable) {
        dirty_state.rasterizer_discard_enable = false;
        cmdbuf.setRasterizerDiscardEnable(rasterizer_discard_enable);
    }
#endif
    if (dirty_state.cull_mode) {
        dirty_state.cull_mode = false;
        if (!legacy_render_pass) {
            cmdbuf.setCullMode(cull_mode);
        }
    }
    if (dirty_state.front_face) {
        dirty_state.front_face = false;
        if (!legacy_render_pass) {
            cmdbuf.setFrontFace(front_face);
        }
    }
    if (dirty_state.blend_constants) {
        dirty_state.blend_constants = false;
        cmdbuf.setBlendConstants(blend_constants.data());
    }
    if (dirty_state.color_write_masks) {
        dirty_state.color_write_masks = false;
        if (instance.IsDynamicColorWriteMaskSupported()) {
            cmdbuf.setColorWriteMaskEXT(0, color_write_masks);
        }
    }
    if (dirty_state.line_width) {
        dirty_state.line_width = false;
        cmdbuf.setLineWidth(line_width);
    }
    if (dirty_state.feedback_loop_enabled && instance.IsAttachmentFeedbackLoopLayoutSupported()) {
        dirty_state.feedback_loop_enabled = false;
        cmdbuf.setAttachmentFeedbackLoopEnableEXT(feedback_loop_enabled
                                                      ? vk::ImageAspectFlagBits::eColor
                                                      : vk::ImageAspectFlagBits::eNone);
    }
    if (dirty_state.fragment_shading_rate) {
        dirty_state.fragment_shading_rate = false;
        if (instance.IsCoarseFragmentShadingSupported()) {
            const std::array combiners = {
                vk::FragmentShadingRateCombinerOpKHR::eKeep,
                vk::FragmentShadingRateCombinerOpKHR::eKeep,
            };
            cmdbuf.setFragmentShadingRateKHR(fragment_shading_rate, combiners.data());
        }
    }
}

} // namespace Vulkan
