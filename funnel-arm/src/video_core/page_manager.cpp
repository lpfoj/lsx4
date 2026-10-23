#if defined(LSX4_APPLE_RUNTIME)
#include "AppleHostServices.h"
#if defined(LSX4_APPLE_FEX)
#define LSX4_PAGE_HOST_TID() pthread_mach_thread_np(pthread_self())
#else
#define LSX4_PAGE_HOST_TID() Lsx4::Apple::HostThreadId()
#endif
#else
#define LSX4_PAGE_HOST_TID() gettid()
#endif
// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <thread>
#include <boost/container/small_vector.hpp>
#include "common/assert.h"
#include "common/config.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/range_lock.h"
#include "common/signal_context.h"
#include "common/scope_exit.h"
#include "common/thread.h"
#include "common/title_scoped_optimization_marker.h"
#include "core/memory.h"
#include "core/signals.h"
#include "video_core/page_manager.h"
#include "video_core/mobile_gpu_optimization.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

#ifndef _WIN64
#include <sys/mman.h>
#include <unistd.h>
#include "common/adaptive_mutex.h"
#ifdef ENABLE_USERFAULTFD
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include "common/error.h"
#endif
#else
#include <windows.h>
#include "common/spin_lock.h"
#endif

#ifdef __linux__
#include "common/adaptive_mutex.h"
#endif

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
#include <android/log.h>

extern "C" std::uint64_t executor_jit_current_fault_guest_rip();
extern "C" std::uint64_t executor_jit_signal_context_fault_provenance(
    const void* raw_context, std::uint64_t* instruction_low,
    std::uint64_t* instruction_high,
    std::uint32_t* instruction_meta) noexcept __attribute__((weak));
extern "C" std::uint32_t
executor_jit_signal_context_fault_memory_read_size(
    const void* raw_context) noexcept;
extern "C" int executor_jit_signal_context_pc_is_generated_code(
    const void* raw_context) noexcept __attribute__((weak));
extern "C" int executor_lsx4_android_fex_signal_context_pc_is_generated_code(
    const void* raw_context) noexcept __attribute__((weak));
extern "C" std::uint64_t executor_lsx4_android_fex_signal_context_guest_rip(
    const void* raw_context) noexcept __attribute__((weak));
extern "C" int executor_lsx4_android_fex_deferred_write_epoch_arm_current(
    std::uint64_t token) noexcept __attribute__((weak));
extern "C" int executor_lsx4_android_fex_deferred_write_epoch_kick_owner(
    int owner_tid) noexcept __attribute__((weak));

using SignalContextPcClassifier = int (*)(const void*) noexcept;
#endif
#if !defined(__linux__) || !defined(PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP)
#include "common/spin_lock.h"
#endif

namespace VideoCore {

namespace {

bool ExecutorPreserveBloodborneSnapdragonStrongBundle() noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    static const bool preserve =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-preserve-bloodborne-snapdragon-strong-bundle",
                 F_OK) == 0;
    return preserve;
#else
    return false;
#endif
}

bool ExecutorPreserveBloodborneSnapdragonRenderBundle() noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    return ExecutorPreserveBloodborneSnapdragonStrongBundle() ||
           ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                    "run-preserve-bloodborne-snapdragon-render-bundle",
                    F_OK) == 0;
#else
    return false;
#endif
}

} // namespace

bool ExecutorDirectRegionModifiedRangeEnabled() noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    static const bool enabled = [] {
        const bool requested =
            Common::TitleScopedOptimizationMarkerEnabled(
                "run-enable-direct-region-modified-range");
        if (ExecutorPreserveBloodborneSnapdragonRenderBundle()) {
            return false;
        }
        const bool snapdragon_default = Common::IsSnapdragon8Gen3Host() ||
                                        Common::IsSnapdragon8Gen5Host();
        if (!requested && !snapdragon_default) {
            return false;
        }
        constexpr const char* witness =
            "/data/data/app.lsx4.android/files/lsx4-home/"
            "direct-region-modified-range.txt";
        if (FILE* file = std::fopen(witness, "w")) {
            std::fprintf(
                file,
                "selected=1 exactAnyInRange=1 legacyFallbackPreserved=1 "
                "dirtyBitsUnchanged=1 lockUnchanged=1 requested=%u "
                "snapdragonDefault=%u\n",
                requested ? 1u : 0u, snapdragon_default ? 1u : 0u);
            std::fclose(file);
        }
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_DIRECT_REGION_MODIFIED_RANGE] selected=1 "
            "exactAnyInRange=1 legacyFallbackPreserved=1 "
            "dirtyBitsUnchanged=1 lockUnchanged=1 requested=%u "
            "snapdragonDefault=%u",
            requested ? 1u : 0u, snapdragon_default ? 1u : 0u);
        return true;
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorSm8650CombinedCpuSnapshotQueryEnabled() noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    static const bool enabled = [] {
        const bool selected = Common::IsSnapdragon8Gen3Host();
        if (selected) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_SM8650_COMBINED_CPU_SNAPSHOT_QUERY] enabled=1 "
                "proof=same-region-lock+exact-cpu-bits+exact-stale-bits "
                "legacyFallbackPreserved=1 mutations=0");
        }
        return selected;
    }();
    return enabled;
#else
    return false;
#endif
}

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
namespace {

bool ExecutorReadbackContractTraceEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EXECUTOR_READBACK_CONTRACT_TRACE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

bool ExecutorFastBulkGuestWriteEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EXECUTOR_FAST_BULK_GUEST_WRITE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

bool ExecutorFusedFastBulkGuestWriteLeaseEnabled() {
    static const bool enabled = [] {
        const char* value =
            std::getenv("EXECUTOR_FUSED_FAST_BULK_GUEST_WRITE_LEASE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

bool ExecutorSm8650ForceWatcherPteTransitionsEnabled() {
    static const bool enabled = [] {
        if (!Common::IsSnapdragon8Gen3Host()) {
            return false;
        }
        const char* value =
            std::getenv("EXECUTOR_SM8650_FORCE_WATCHER_PTE_TRANSITIONS");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

bool ExecutorHleCleanBufferFastPathEnabled() {
    static const bool requested = [] {
        const char* value =
            std::getenv("EXECUTOR_HLE_CLEAN_BUFFER_FASTPATH");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return requested;
}

bool ExecutorHleCleanBufferStatsEnabled() {
    static const bool enabled =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-diagnostic-hle-clean-buffer-stats",
                 F_OK) == 0;
    return enabled;
}

bool ExecutorGuestInvalidationSourceProfileEnabled() {
    static const bool enabled = [] {
        const char* value =
            std::getenv("EXECUTOR_PROFILE_GUEST_INVALIDATION_SOURCES");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

bool ExecutorExactProtectedBulkInvalidationRunsEnabled() {
    static const bool enabled =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-enable-exact-protected-bulk-invalidation-runs",
                 F_OK) == 0;
    return enabled;
}

bool ExecutorSinglePageDescriptorSnapshotFastPathEnabled() {
    static const bool enabled = [] {
        const bool requested = Common::TitleScopedOptimizationMarkerEnabled(
            "run-enable-single-page-descriptor-snapshot-fast-path");
        if (ExecutorPreserveBloodborneSnapdragonRenderBundle()) {
            return false;
        }
        const bool snapdragon_default = Common::IsSnapdragon8Gen3Host() ||
                                        Common::IsSnapdragon8Gen5Host();
        const bool selected = requested || snapdragon_default;
        if (selected) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_SINGLE_PAGE_DESCRIPTOR_SNAPSHOT] enabled=1 "
                "requested=%u snapdragonDefault=%u semantics=exact "
                "chunkLookup=single conflictRead=single",
                requested ? 1u : 0u, snapdragon_default ? 1u : 0u);
        }
        return selected;
    }();
    return enabled;
}

bool ExecutorSingleChunkDescriptorSnapshotFastPathEnabled() {
    static const bool enabled = [] {
        const bool requested =
            ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                     "run-enable-single-chunk-descriptor-snapshot-fast-path",
                     F_OK) == 0;
        const bool parent_enabled =
            ExecutorSinglePageDescriptorSnapshotFastPathEnabled();
        if (requested) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_SINGLE_CHUNK_DESCRIPTOR_SNAPSHOT] requested=1 "
                "enabled=%u parentSinglePage=%u "
                "proof=range-lock+stable-chunk+exact-pages "
                "fallback=cross-chunk",
                parent_enabled ? 1u : 0u, parent_enabled ? 1u : 0u);
        }
        return requested && parent_enabled;
    }();
    return enabled;
}

bool ExecutorDescriptorSnapshotLocalChunkHoistEnabled() {
    static const bool enabled = [] {
        const bool requested = Common::TitleScopedOptimizationMarkerEnabled(
            "run-enable-descriptor-snapshot-local-chunk-hoist");
        if (ExecutorPreserveBloodborneSnapdragonRenderBundle()) {
            return false;
        }
        const bool snapdragon_default = Common::IsSnapdragon8Gen3Host() ||
                                        Common::IsSnapdragon8Gen5Host();
        const bool selected = requested || snapdragon_default;
        if (selected) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_DESCRIPTOR_SNAPSHOT_LOCAL_CHUNK_HOIST] enabled=1 "
                "requested=%u snapdragonDefault=%u "
                "proof=range-lock+page-local-chunk+exact-state "
                "semantics=watcher+conflict+generation+protection",
                requested ? 1u : 0u, snapdragon_default ? 1u : 0u);
        }
        return selected;
    }();
    return enabled;
}

bool ExecutorSm8650DescriptorHazardWordPublishEnabled() {
    static const bool enabled = [] {
        const bool selected = Common::IsSnapdragon8Gen3Host();
        if (selected) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_SM8650_DESCRIPTOR_HAZARD_WORD_PUBLISH] enabled=1 "
                "proof=unwatched-cutoff+range-lock+release-before-protect "
                "granularity=64-pages");
        }
        return selected;
    }();
    return enabled;
}

bool ExecutorDescriptorProtectionCohortDiagnosticEnabled() noexcept {
    static const bool enabled = [] {
        const bool requested =
            ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                     "run-diagnostic-descriptor-protection-cohort",
                     F_OK) == 0;
        const bool selected = requested && Common::IsSnapdragon8Gen3Host();
        if (requested) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_DESCRIPTOR_PROTECTION_COHORT_DIAGNOSTIC] "
                "enabled=%u requested=1 exactA750=%u behavior=observe-only",
                selected ? 1u : 0u,
                Common::IsSnapdragon8Gen3Host() ? 1u : 0u);
        }
        return selected;
    }();
    return enabled;
}

struct DescriptorProtectionDiagnosticInterval {
    size_t first_page{};
    size_t page_count{};
    Core::MemoryPermission permissions{};
};

struct DescriptorProtectionDiagnosticBatch {
    bool active{};
    u64 ranges{};
    boost::container::small_vector<DescriptorProtectionDiagnosticInterval, 64>
        untrack_runs;
};

struct DescriptorProtectionDiagnosticCounters {
    std::atomic<u64> start_present{};
    std::atomic<u64> batches{};
    std::atomic<u64> transactions{};
    std::atomic<u64> write_ranges{};
    std::atomic<u64> pages{};
    std::atomic<u64> track_calls{};
    std::atomic<u64> tracked{};
    std::atomic<u64> busy{};
    std::atomic<u64> overlap{};
    std::atomic<u64> unsupported{};
    std::atomic<u64> invalid{};
    std::atomic<u64> untrack_calls{};
    std::atomic<u64> clean_untracks{};
    std::atomic<u64> unclean_untracks{};
    std::atomic<u64> write_faults{};
    std::atomic<u64> legacy_track_protect_runs{};
    std::atomic<u64> legacy_track_protect_pages{};
    std::atomic<u64> legacy_untrack_protect_runs{};
    std::atomic<u64> legacy_untrack_protect_pages{};
    std::atomic<u64> cohort_untrack_runs{};
    std::atomic<u64> cohort_untrack_unique_pages{};
    std::atomic<u64> track_cpu_ns{};
    std::atomic<u64> untrack_cpu_ns{};
    std::atomic<u64> track_protect_cpu_ns{};
    std::atomic<u64> untrack_protect_cpu_ns{};
    std::atomic<u64> max_ranges_per_batch{};
    std::atomic<u64> last_publish_ns{};
};

DescriptorProtectionDiagnosticCounters descriptor_protection_diagnostic{};
thread_local DescriptorProtectionDiagnosticBatch descriptor_protection_batch{};

u64 ExecutorDescriptorDiagnosticThreadCpuNow() noexcept {
    if (!ExecutorDescriptorProtectionCohortDiagnosticEnabled()) {
        return 0;
    }
    timespec value{};
    return ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0
               ? static_cast<u64>(value.tv_sec) * 1'000'000'000ull +
                     static_cast<u64>(value.tv_nsec)
               : 0;
}

void ExecutorDescriptorDiagnosticUpdateMax(std::atomic<u64>& target,
                                           const u64 value) noexcept {
    u64 current = target.load(std::memory_order_relaxed);
    while (current < value &&
           !target.compare_exchange_weak(current, value,
                                         std::memory_order_relaxed)) {
    }
}

void ExecutorRecordDescriptorProtectRun(
    const bool track, const size_t first_page, const size_t page_count,
    const Core::MemoryPermission permissions, const u64 cpu_ns) noexcept {
    if (!ExecutorDescriptorProtectionCohortDiagnosticEnabled()) {
        return;
    }
    auto& counters = descriptor_protection_diagnostic;
    if (track) {
        counters.legacy_track_protect_runs.fetch_add(1,
                                                     std::memory_order_relaxed);
        counters.legacy_track_protect_pages.fetch_add(page_count,
                                                      std::memory_order_relaxed);
        counters.track_protect_cpu_ns.fetch_add(cpu_ns,
                                                std::memory_order_relaxed);
        return;
    }
    counters.legacy_untrack_protect_runs.fetch_add(1,
                                                   std::memory_order_relaxed);
    counters.legacy_untrack_protect_pages.fetch_add(page_count,
                                                    std::memory_order_relaxed);
    counters.untrack_protect_cpu_ns.fetch_add(cpu_ns,
                                              std::memory_order_relaxed);
    if (descriptor_protection_batch.active) {
        descriptor_protection_batch.untrack_runs.push_back(
            {first_page, page_count, permissions});
    }
}

void ExecutorRecordDescriptorTrackCall(
    const VAddr addr, const u64 size,
    const DescriptorSnapshotTrackResult result, const u64 cpu_ns) noexcept {
    if (!ExecutorDescriptorProtectionCohortDiagnosticEnabled()) {
        return;
    }
    auto& counters = descriptor_protection_diagnostic;
    constexpr u64 GuestPageSize = 4_KB;
    const u64 pages = size == 0
                          ? 0
                          : Common::DivCeil((addr & (GuestPageSize - 1)) + size,
                                            GuestPageSize);
    counters.track_calls.fetch_add(1, std::memory_order_relaxed);
    counters.write_ranges.fetch_add(1, std::memory_order_relaxed);
    counters.pages.fetch_add(pages, std::memory_order_relaxed);
    counters.track_cpu_ns.fetch_add(cpu_ns, std::memory_order_relaxed);
    if (descriptor_protection_batch.active) {
        ++descriptor_protection_batch.ranges;
    }
    switch (result) {
    case DescriptorSnapshotTrackResult::Tracked:
        counters.tracked.fetch_add(1, std::memory_order_relaxed);
        break;
    case DescriptorSnapshotTrackResult::Busy:
        counters.busy.fetch_add(1, std::memory_order_relaxed);
        break;
    case DescriptorSnapshotTrackResult::Overlap:
        counters.overlap.fetch_add(1, std::memory_order_relaxed);
        break;
    case DescriptorSnapshotTrackResult::Unsupported:
        counters.unsupported.fetch_add(1, std::memory_order_relaxed);
        break;
    case DescriptorSnapshotTrackResult::Invalid:
        counters.invalid.fetch_add(1, std::memory_order_relaxed);
        break;
    }
}

void ExecutorRecordDescriptorUntrackCall(const bool clean,
                                         const u64 cpu_ns) noexcept {
    if (!ExecutorDescriptorProtectionCohortDiagnosticEnabled()) {
        return;
    }
    auto& counters = descriptor_protection_diagnostic;
    counters.untrack_calls.fetch_add(1, std::memory_order_relaxed);
    counters.untrack_cpu_ns.fetch_add(cpu_ns, std::memory_order_relaxed);
    (clean ? counters.clean_untracks : counters.unclean_untracks)
        .fetch_add(1, std::memory_order_relaxed);
}

void ExecutorPublishDescriptorProtectionDiagnostic() noexcept {
    auto& counters = descriptor_protection_diagnostic;
    const u64 now_ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    u64 last = counters.last_publish_ns.load(std::memory_order_relaxed);
    if (last != 0 && now_ns - last < 1'000'000'000ull) {
        return;
    }
    if (!counters.last_publish_ns.compare_exchange_strong(
            last, now_ns, std::memory_order_relaxed)) {
        return;
    }
    const u64 present = Vulkan::ExecutorGetLivePresentCount();
    u64 start_present = counters.start_present.load(std::memory_order_relaxed);
    if (start_present == 0) {
        counters.start_present.compare_exchange_strong(
            start_present, present, std::memory_order_relaxed);
        start_present = present;
    }
    const u64 frames = present >= start_present ? present - start_present : 0;
    const auto load = [](const std::atomic<u64>& value) {
        return value.load(std::memory_order_relaxed);
    };
    const u64 legacy_untrack_runs =
        load(counters.legacy_untrack_protect_runs);
    const u64 cohort_untrack_runs = load(counters.cohort_untrack_runs);
    const double removed_fraction = legacy_untrack_runs == 0
                                        ? 0.0
                                        : static_cast<double>(
                                              legacy_untrack_runs -
                                              std::min(legacy_untrack_runs,
                                                       cohort_untrack_runs)) /
                                              legacy_untrack_runs;
    if (FILE* out = std::fopen(
            "/data/data/app.lsx4.android/files/runtime/"
            "descriptor-protection-cohort.txt",
            "w")) {
        std::fprintf(
            out,
            "schema=1 frames=%llu batches=%llu transactions=%llu "
            "writeRanges=%llu pages=%llu trackCalls=%llu tracked=%llu "
            "busy=%llu overlap=%llu unsupported=%llu invalid=%llu "
            "untrackCalls=%llu cleanUntracks=%llu uncleanUntracks=%llu "
            "writeFaults=%llu legacyTrackProtectRuns=%llu "
            "legacyTrackProtectPages=%llu legacyUntrackProtectRuns=%llu "
            "legacyUntrackProtectPages=%llu cohortUntrackRuns=%llu "
            "cohortUntrackUniquePages=%llu modelRemovedRunFraction=%.6f "
            "trackCpuNs=%llu untrackCpuNs=%llu trackProtectCpuNs=%llu "
            "untrackProtectCpuNs=%llu maxRangesPerBatch=%llu\n",
            static_cast<unsigned long long>(frames),
            static_cast<unsigned long long>(load(counters.batches)),
            static_cast<unsigned long long>(load(counters.transactions)),
            static_cast<unsigned long long>(load(counters.write_ranges)),
            static_cast<unsigned long long>(load(counters.pages)),
            static_cast<unsigned long long>(load(counters.track_calls)),
            static_cast<unsigned long long>(load(counters.tracked)),
            static_cast<unsigned long long>(load(counters.busy)),
            static_cast<unsigned long long>(load(counters.overlap)),
            static_cast<unsigned long long>(load(counters.unsupported)),
            static_cast<unsigned long long>(load(counters.invalid)),
            static_cast<unsigned long long>(load(counters.untrack_calls)),
            static_cast<unsigned long long>(load(counters.clean_untracks)),
            static_cast<unsigned long long>(load(counters.unclean_untracks)),
            static_cast<unsigned long long>(load(counters.write_faults)),
            static_cast<unsigned long long>(
                load(counters.legacy_track_protect_runs)),
            static_cast<unsigned long long>(
                load(counters.legacy_track_protect_pages)),
            static_cast<unsigned long long>(legacy_untrack_runs),
            static_cast<unsigned long long>(
                load(counters.legacy_untrack_protect_pages)),
            static_cast<unsigned long long>(cohort_untrack_runs),
            static_cast<unsigned long long>(
                load(counters.cohort_untrack_unique_pages)),
            removed_fraction,
            static_cast<unsigned long long>(load(counters.track_cpu_ns)),
            static_cast<unsigned long long>(load(counters.untrack_cpu_ns)),
            static_cast<unsigned long long>(
                load(counters.track_protect_cpu_ns)),
            static_cast<unsigned long long>(
                load(counters.untrack_protect_cpu_ns)),
            static_cast<unsigned long long>(
                load(counters.max_ranges_per_batch)));
        std::fclose(out);
    }
}

bool ExecutorShouldSampleGuestInvalidationSources() {
    if (!ExecutorGuestInvalidationSourceProfileEnabled()) {
        return false;
    }
    // This hook sits on libc bulk writes.  Sampling every call measurably
    // changes the workload being diagnosed, so keep the diagnostic sparse and
    // thread-local (no contended global sampling counter).
    thread_local u32 ordinal = 0;
    return ((++ordinal) & 1023u) == 1u;
}

bool ExecutorUnwatchedBulkGuestWriteFastPathEnabled() {
    static const bool enabled = [] {
        const char* value =
            std::getenv("EXECUTOR_ENABLE_UNWATCHED_BULK_GUEST_WRITE_FAST_PATH");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

bool ExecutorFexBufferCaptureAckEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EXECUTOR_FEX_BUFFER_CAPTURE_ACK");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

bool ExecutorExactFexEpochPageIndexEnabled() {
    static const bool enabled =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-enable-exact-fex-epoch-page-index",
                 F_OK) == 0;
    return enabled;
}

bool ExecutorFexDeferredBulkGuestWritesEnabled() {
    static const bool enabled = [] {
        const char* value =
            std::getenv("EXECUTOR_FEX_DEFERRED_BULK_GUEST_WRITES");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled && ExecutorFexBufferCaptureAckEnabled();
}

bool ExecutorShouldLogReadbackOrdinal(const u64 ordinal) {
    return ordinal <= 64 || (ordinal & (ordinal - 1)) == 0;
}

std::atomic<u64> executor_readback_protect_count{0};
std::atomic<u64> executor_readback_protect_os_count{0};
std::atomic<u64> executor_readback_protect_noop_count{0};
std::atomic<u64> executor_readback_fault_count{0};
std::atomic<u64> executor_readback_read_fault_count{0};
std::atomic<u64> executor_readback_write_fault_count{0};
std::atomic<u64> executor_owned_generated_fault_count{0};
std::atomic<u64> executor_owned_host_fault_count{0};
std::atomic<u64> executor_owned_unclassified_fault_count{0};
std::atomic<u64> executor_generated_buffer_write_fault_count{0};
std::atomic<u64> executor_generated_texture_write_fault_count{0};
std::atomic<u64> executor_generated_other_write_fault_count{0};
std::atomic<u64> executor_generated_special_write_fault_count{0};
std::atomic<u64> executor_sequential_buffer_fault_window_count{0};
std::atomic<u64> executor_sequential_buffer_fault_window_pages{0};
std::atomic<u64> executor_gpu_thread_host_fault_count{0};
std::atomic<u32> executor_unowned_fault_diag_count{0};
std::atomic<u32> executor_unresolved_fault_diag_count{0};
std::atomic<u64> executor_write_provenance_exact_begin{0};
std::atomic<u64> executor_write_provenance_exact_end{0};
std::atomic<u64> executor_write_provenance_page_begin{0};
std::atomic<u64> executor_write_provenance_page_end{0};
std::atomic<u64> executor_unwatched_bulk_write_fast_calls{0};
std::atomic<u64> executor_unwatched_bulk_write_fast_bytes{0};
std::atomic<u64> executor_unwatched_bulk_write_hazard_fallbacks{0};
std::atomic<u64> executor_unwatched_bulk_write_gate_fallbacks{0};
std::atomic<u64> executor_bulk_invalidation_profile_calls{0};
std::atomic<u64> executor_bulk_invalidation_profile_invalidations{0};
std::atomic<u64> executor_bulk_invalidation_profile_noops{0};
std::atomic<u64> executor_bulk_invalidation_profile_requested_pages{0};
std::atomic<u64> executor_bulk_invalidation_profile_protected_pages{0};
std::atomic<u64> executor_bulk_invalidation_profile_invalidated_pages{0};
std::atomic<u64> executor_bulk_invalidation_profile_potential_span_pages{0};
std::atomic<u64> executor_bulk_invalidation_profile_span_savings_pages{0};
std::atomic<u64> executor_bulk_invalidation_profile_protected_runs{0};
std::atomic<u64> executor_bulk_invalidation_profile_single_page_calls{0};
std::atomic<u64> executor_bulk_invalidation_profile_two_to_four_page_calls{0};
std::atomic<u64> executor_bulk_invalidation_profile_five_to_sixteen_page_calls{0};
std::atomic<u64> executor_bulk_invalidation_profile_large_page_calls{0};
std::atomic<u64> executor_bulk_invalidation_profile_all_protected_calls{0};
std::atomic<u64> executor_bulk_invalidation_profile_sparse_calls{0};
std::atomic<u64> executor_bulk_invalidation_profile_sync_wait_ns{0};
std::atomic<u64> executor_bulk_invalidation_profile_max_sync_wait_ns{0};

enum class ExecutorHleCleanBufferFallback : u8 {
    Size,
    Alias,
    Mapping,
    Page,
    Texture,
    Shader,
    RegionMissing,
    RegionBusy,
    RegionGpuDirty,
    Postvalidate,
    Nested,
    Count,
};

struct ExecutorHleCleanBufferCounters {
    std::atomic<u64> attempts{};
    std::atomic<u64> eligible{};
    std::atomic<u64> committed{};
    std::atomic<u64> avoided_sync{};
    std::atomic<u64> pages{};
    std::atomic<u64> bytes{};
    std::atomic<u64> active{};
    std::atomic<u64> active_max{};
    std::array<std::atomic<u64>,
               static_cast<size_t>(ExecutorHleCleanBufferFallback::Count)>
        fallbacks{};
};

ExecutorHleCleanBufferCounters executor_hle_clean_buffer_counters{};

void ExecutorHleCleanBufferUpdateMax(std::atomic<u64>& maximum,
                                     const u64 value) noexcept {
    u64 observed = maximum.load(std::memory_order_relaxed);
    while (observed < value &&
           !maximum.compare_exchange_weak(observed, value,
                                          std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {
    }
}

void ExecutorHleCleanBufferMaybeLog(const u64 ordinal) noexcept {
    // Keep startup diagnostics dense, then provide a bounded periodic sample
    // inside a short A/B window without adding per-call log traffic.
    if (ordinal > 64 && (ordinal & 4095u) != 0) {
        return;
    }
    const auto& counters = executor_hle_clean_buffer_counters;
    const auto fallback = [&](const ExecutorHleCleanBufferFallback reason) {
        return counters.fallbacks[static_cast<size_t>(reason)].load(
            std::memory_order_relaxed);
    };
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_HLE_CLEAN_BUFFER_FASTPATH] attempts=%llu eligible=%llu "
        "committed=%llu avoidedSync=%llu fallbackSize=%llu "
        "fallbackAlias=%llu fallbackMapping=%llu fallbackPage=%llu "
        "fallbackTexture=%llu fallbackShader=%llu "
        "fallbackRegionMissing=%llu "
        "fallbackRegionBusy=%llu fallbackRegionGpuDirty=%llu "
        "fallbackPostvalidate=%llu fallbackNested=%llu pages=%llu "
        "bytes=%llu active=%llu activeMax=%llu",
        static_cast<unsigned long long>(
            counters.attempts.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            counters.eligible.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            counters.committed.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            counters.avoided_sync.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            fallback(ExecutorHleCleanBufferFallback::Size)),
        static_cast<unsigned long long>(
            fallback(ExecutorHleCleanBufferFallback::Alias)),
        static_cast<unsigned long long>(
            fallback(ExecutorHleCleanBufferFallback::Mapping)),
        static_cast<unsigned long long>(
            fallback(ExecutorHleCleanBufferFallback::Page)),
        static_cast<unsigned long long>(
            fallback(ExecutorHleCleanBufferFallback::Texture)),
        static_cast<unsigned long long>(
            fallback(ExecutorHleCleanBufferFallback::Shader)),
        static_cast<unsigned long long>(
            fallback(ExecutorHleCleanBufferFallback::RegionMissing)),
        static_cast<unsigned long long>(
            fallback(ExecutorHleCleanBufferFallback::RegionBusy)),
        static_cast<unsigned long long>(
            fallback(ExecutorHleCleanBufferFallback::RegionGpuDirty)),
        static_cast<unsigned long long>(
            fallback(ExecutorHleCleanBufferFallback::Postvalidate)),
        static_cast<unsigned long long>(
            fallback(ExecutorHleCleanBufferFallback::Nested)),
        static_cast<unsigned long long>(
            counters.pages.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            counters.bytes.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            counters.active.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            counters.active_max.load(std::memory_order_relaxed)));
}

void ExecutorHleCleanBufferRecordFallback(
    const ExecutorHleCleanBufferFallback reason,
    const u64 ordinal) noexcept {
    if (ordinal == 0) {
        return;
    }
    executor_hle_clean_buffer_counters
        .fallbacks[static_cast<size_t>(reason)]
        .fetch_add(1, std::memory_order_relaxed);
    ExecutorHleCleanBufferMaybeLog(ordinal);
}

enum class ExecutorBulkInvalidationFirstBlocker : u8 {
    None,
    Incomplete,
    Alias,
    Topology,
    Read,
    Special,
    GpuDirty,
    Source,
    Mapping,
    Busy,
    Eligible,
};

struct ExecutorBulkInvalidationEligibilitySnapshot {
    u64 source_none_pages{};
    u64 source_buffer_pages{};
    u64 source_texture_pages{};
    u64 source_mixed_pages{};
    u64 source_mismatch_pages{};
    u64 candidate_pages{};
    u64 read_watcher_pages{};
    u64 actual_read_protected_pages{};
    u64 memory_wait_pages{};
    u64 jit_poll_pages{};
    u64 descriptor_watcher_pages{};
    u64 descriptor_conflict_pages{};
    u64 gpu_dirty_pages{};
    u64 unmapped_pages{};
    u64 host_write_denied_pages{};
    u64 guest_write_denied_pages{};
    u64 active_bulk_writer_pages{};
    u64 prepare_reservation_pages{};
    u64 write_fault_bypass_pages{};
    u64 deferred_write_bypass_pages{};
    u64 buffer_capture_blocked_pages{};
    bool alias_observed{};
    bool topology_changed{};
    bool incomplete_sample{};
    bool read_blocker{};
    bool special_blocker{};
    bool gpu_dirty_blocker{};
    bool source_blocker{};
    bool mapping_blocker{};
    bool busy_blocker{};
    ExecutorBulkInvalidationFirstBlocker first_blocker{
        ExecutorBulkInvalidationFirstBlocker::None};
};

struct ExecutorBulkInvalidationProfileSample {
    ExecutorBulkInvalidationEligibilitySnapshot eligibility{};
    u64 protected_pages{};
    u64 first_protected_page{};
    u64 last_protected_page{};
    u64 protected_runs{};
};

struct ExecutorBulkInvalidationEligibilityCounters {
    std::atomic<u64> source_none_pages{};
    std::atomic<u64> source_buffer_pages{};
    std::atomic<u64> source_texture_pages{};
    std::atomic<u64> source_mixed_pages{};
    std::atomic<u64> source_mismatch_pages{};
    std::atomic<u64> candidate_pages{};
    std::atomic<u64> read_watcher_pages{};
    std::atomic<u64> actual_read_protected_pages{};
    std::atomic<u64> memory_wait_pages{};
    std::atomic<u64> jit_poll_pages{};
    std::atomic<u64> descriptor_watcher_pages{};
    std::atomic<u64> descriptor_conflict_pages{};
    std::atomic<u64> gpu_dirty_pages{};
    std::atomic<u64> unmapped_pages{};
    std::atomic<u64> host_write_denied_pages{};
    std::atomic<u64> guest_write_denied_pages{};
    std::atomic<u64> active_bulk_writer_pages{};
    std::atomic<u64> prepare_reservation_pages{};
    std::atomic<u64> write_fault_bypass_pages{};
    std::atomic<u64> deferred_write_bypass_pages{};
    std::atomic<u64> buffer_capture_blocked_pages{};
    std::atomic<u64> alias_observed_calls{};
    std::atomic<u64> topology_changed_calls{};
    std::atomic<u64> incomplete_sample_calls{};
    std::atomic<u64> alias_blocked_calls{};
    std::atomic<u64> topology_blocked_calls{};
    std::atomic<u64> read_blocked_calls{};
    std::atomic<u64> special_blocked_calls{};
    std::atomic<u64> gpu_dirty_blocked_calls{};
    std::atomic<u64> source_blocked_calls{};
    std::atomic<u64> mapping_blocked_calls{};
    std::atomic<u64> busy_blocked_calls{};
    std::atomic<u64> eligible_calls{};
    std::atomic<u64> eligible_sync_wait_ns{};
    std::atomic<u64> blocked_sync_wait_ns{};
};

ExecutorBulkInvalidationEligibilityCounters
    executor_bulk_invalidation_eligibility{};

void ExecutorRecordBulkInvalidationProfile(const u64 requested_pages,
                                           const u64 protected_pages,
                                           const u64 invalidated_pages,
                                           const u64 potential_span_pages,
                                           const u64 protected_runs,
                                           const u64 sync_wait_ns,
                                           const ExecutorBulkInvalidationEligibilitySnapshot&
                                               eligibility) {
    if (!ExecutorGuestInvalidationSourceProfileEnabled()) {
        return;
    }
    auto& eligibility_counters = executor_bulk_invalidation_eligibility;
    const auto add_eligibility_counter = [](std::atomic<u64>& counter,
                                            const u64 value) {
        counter.fetch_add(value, std::memory_order_relaxed);
    };
    add_eligibility_counter(eligibility_counters.source_none_pages,
                            eligibility.source_none_pages);
    add_eligibility_counter(eligibility_counters.source_buffer_pages,
                            eligibility.source_buffer_pages);
    add_eligibility_counter(eligibility_counters.source_texture_pages,
                            eligibility.source_texture_pages);
    add_eligibility_counter(eligibility_counters.source_mixed_pages,
                            eligibility.source_mixed_pages);
    add_eligibility_counter(eligibility_counters.source_mismatch_pages,
                            eligibility.source_mismatch_pages);
    add_eligibility_counter(eligibility_counters.candidate_pages,
                            eligibility.candidate_pages);
    add_eligibility_counter(eligibility_counters.read_watcher_pages,
                            eligibility.read_watcher_pages);
    add_eligibility_counter(eligibility_counters.actual_read_protected_pages,
                            eligibility.actual_read_protected_pages);
    add_eligibility_counter(eligibility_counters.memory_wait_pages,
                            eligibility.memory_wait_pages);
    add_eligibility_counter(eligibility_counters.jit_poll_pages,
                            eligibility.jit_poll_pages);
    add_eligibility_counter(eligibility_counters.descriptor_watcher_pages,
                            eligibility.descriptor_watcher_pages);
    add_eligibility_counter(eligibility_counters.descriptor_conflict_pages,
                            eligibility.descriptor_conflict_pages);
    add_eligibility_counter(eligibility_counters.gpu_dirty_pages,
                            eligibility.gpu_dirty_pages);
    add_eligibility_counter(eligibility_counters.unmapped_pages,
                            eligibility.unmapped_pages);
    add_eligibility_counter(eligibility_counters.host_write_denied_pages,
                            eligibility.host_write_denied_pages);
    add_eligibility_counter(eligibility_counters.guest_write_denied_pages,
                            eligibility.guest_write_denied_pages);
    add_eligibility_counter(eligibility_counters.active_bulk_writer_pages,
                            eligibility.active_bulk_writer_pages);
    add_eligibility_counter(eligibility_counters.prepare_reservation_pages,
                            eligibility.prepare_reservation_pages);
    add_eligibility_counter(eligibility_counters.write_fault_bypass_pages,
                            eligibility.write_fault_bypass_pages);
    add_eligibility_counter(eligibility_counters.deferred_write_bypass_pages,
                            eligibility.deferred_write_bypass_pages);
    add_eligibility_counter(eligibility_counters.buffer_capture_blocked_pages,
                            eligibility.buffer_capture_blocked_pages);
    if (eligibility.alias_observed) {
        eligibility_counters.alias_observed_calls.fetch_add(
            1, std::memory_order_relaxed);
    }
    if (eligibility.topology_changed) {
        eligibility_counters.topology_changed_calls.fetch_add(
            1, std::memory_order_relaxed);
    }
    switch (eligibility.first_blocker) {
    case ExecutorBulkInvalidationFirstBlocker::Incomplete:
        eligibility_counters.incomplete_sample_calls.fetch_add(
            1, std::memory_order_relaxed);
        break;
    case ExecutorBulkInvalidationFirstBlocker::Alias:
        eligibility_counters.alias_blocked_calls.fetch_add(
            1, std::memory_order_relaxed);
        break;
    case ExecutorBulkInvalidationFirstBlocker::Topology:
        eligibility_counters.topology_blocked_calls.fetch_add(
            1, std::memory_order_relaxed);
        break;
    case ExecutorBulkInvalidationFirstBlocker::Read:
        eligibility_counters.read_blocked_calls.fetch_add(
            1, std::memory_order_relaxed);
        break;
    case ExecutorBulkInvalidationFirstBlocker::Special:
        eligibility_counters.special_blocked_calls.fetch_add(
            1, std::memory_order_relaxed);
        break;
    case ExecutorBulkInvalidationFirstBlocker::GpuDirty:
        eligibility_counters.gpu_dirty_blocked_calls.fetch_add(
            1, std::memory_order_relaxed);
        break;
    case ExecutorBulkInvalidationFirstBlocker::Source:
        eligibility_counters.source_blocked_calls.fetch_add(
            1, std::memory_order_relaxed);
        break;
    case ExecutorBulkInvalidationFirstBlocker::Mapping:
        eligibility_counters.mapping_blocked_calls.fetch_add(
            1, std::memory_order_relaxed);
        break;
    case ExecutorBulkInvalidationFirstBlocker::Busy:
        eligibility_counters.busy_blocked_calls.fetch_add(
            1, std::memory_order_relaxed);
        break;
    case ExecutorBulkInvalidationFirstBlocker::Eligible:
        eligibility_counters.eligible_calls.fetch_add(
            1, std::memory_order_relaxed);
        break;
    case ExecutorBulkInvalidationFirstBlocker::None:
        break;
    }
    if (eligibility.first_blocker ==
        ExecutorBulkInvalidationFirstBlocker::Eligible) {
        eligibility_counters.eligible_sync_wait_ns.fetch_add(
            sync_wait_ns, std::memory_order_relaxed);
    } else if (eligibility.first_blocker !=
               ExecutorBulkInvalidationFirstBlocker::None) {
        eligibility_counters.blocked_sync_wait_ns.fetch_add(
            sync_wait_ns, std::memory_order_relaxed);
    }
    const u64 calls = executor_bulk_invalidation_profile_calls.fetch_add(
                          1, std::memory_order_relaxed) +
                      1;
    executor_bulk_invalidation_profile_requested_pages.fetch_add(
        requested_pages, std::memory_order_relaxed);
    executor_bulk_invalidation_profile_protected_pages.fetch_add(
        protected_pages, std::memory_order_relaxed);
    executor_bulk_invalidation_profile_invalidated_pages.fetch_add(
        invalidated_pages, std::memory_order_relaxed);
    executor_bulk_invalidation_profile_potential_span_pages.fetch_add(
        potential_span_pages, std::memory_order_relaxed);
    if (!eligibility.incomplete_sample && invalidated_pages != 0) {
        executor_bulk_invalidation_profile_span_savings_pages.fetch_add(
            requested_pages - potential_span_pages, std::memory_order_relaxed);
    }
    executor_bulk_invalidation_profile_protected_runs.fetch_add(
        protected_runs, std::memory_order_relaxed);
    executor_bulk_invalidation_profile_sync_wait_ns.fetch_add(
        sync_wait_ns, std::memory_order_relaxed);
    auto previous_max = executor_bulk_invalidation_profile_max_sync_wait_ns.load(
        std::memory_order_relaxed);
    while (previous_max < sync_wait_ns &&
           !executor_bulk_invalidation_profile_max_sync_wait_ns.compare_exchange_weak(
               previous_max, sync_wait_ns, std::memory_order_relaxed)) {
    }
    if (requested_pages == 1) {
        executor_bulk_invalidation_profile_single_page_calls.fetch_add(
            1, std::memory_order_relaxed);
    } else if (requested_pages <= 4) {
        executor_bulk_invalidation_profile_two_to_four_page_calls.fetch_add(
            1, std::memory_order_relaxed);
    } else if (requested_pages <= 16) {
        executor_bulk_invalidation_profile_five_to_sixteen_page_calls.fetch_add(
            1, std::memory_order_relaxed);
    } else {
        executor_bulk_invalidation_profile_large_page_calls.fetch_add(
            1, std::memory_order_relaxed);
    }
    if (invalidated_pages == 0) {
        executor_bulk_invalidation_profile_noops.fetch_add(
            1, std::memory_order_relaxed);
    } else {
        executor_bulk_invalidation_profile_invalidations.fetch_add(
            1, std::memory_order_relaxed);
        if (!eligibility.incomplete_sample &&
            protected_pages == requested_pages) {
            executor_bulk_invalidation_profile_all_protected_calls.fetch_add(
                1, std::memory_order_relaxed);
        }
        if (!eligibility.incomplete_sample &&
            protected_pages < potential_span_pages) {
            executor_bulk_invalidation_profile_sparse_calls.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    // Diagnostic-only cumulative snapshots at powers of two converge quickly
    // enough for a short gameplay reachability run. Sampling itself remains
    // sparse (1/1024 bulk calls), so this adds only logarithmic log traffic.
    if ((calls & (calls - 1)) != 0) {
        return;
    }
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_GUEST_INVALIDATION_SOURCES] bulkCalls=%llu "
        "bulkInvalidations=%llu bulkNoops=%llu requestedPages=%llu "
        "protectedPages=%llu invalidatedPages=%llu potentialSpanPages=%llu "
        "spanSavingsPages=%llu protectedRuns=%llu singlePageCalls=%llu "
        "twoToFourPageCalls=%llu fiveToSixteenPageCalls=%llu largePageCalls=%llu "
        "allProtectedCalls=%llu sparseCalls=%llu syncWaitNs=%llu "
        "maxSyncWaitNs=%llu generatedBufferFaults=%llu "
        "generatedTextureFaults=%llu generatedOtherFaults=%llu",
        static_cast<unsigned long long>(calls),
        static_cast<unsigned long long>(
            executor_bulk_invalidation_profile_invalidations.load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            executor_bulk_invalidation_profile_noops.load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            executor_bulk_invalidation_profile_requested_pages.load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            executor_bulk_invalidation_profile_protected_pages.load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            executor_bulk_invalidation_profile_invalidated_pages.load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            executor_bulk_invalidation_profile_potential_span_pages.load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            executor_bulk_invalidation_profile_span_savings_pages.load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            executor_bulk_invalidation_profile_protected_runs.load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            executor_bulk_invalidation_profile_single_page_calls.load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            executor_bulk_invalidation_profile_two_to_four_page_calls.load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            executor_bulk_invalidation_profile_five_to_sixteen_page_calls.load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            executor_bulk_invalidation_profile_large_page_calls.load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            executor_bulk_invalidation_profile_all_protected_calls.load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            executor_bulk_invalidation_profile_sparse_calls.load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            executor_bulk_invalidation_profile_sync_wait_ns.load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            executor_bulk_invalidation_profile_max_sync_wait_ns.load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            executor_generated_buffer_write_fault_count.load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            executor_generated_texture_write_fault_count.load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            executor_generated_other_write_fault_count.load(
                std::memory_order_relaxed)));
    const auto load_eligibility_counter = [](const std::atomic<u64>& counter) {
        return counter.load(std::memory_order_relaxed);
    };
    const u64 alias_blocked = load_eligibility_counter(
        eligibility_counters.alias_blocked_calls);
    const u64 incomplete_samples = load_eligibility_counter(
        eligibility_counters.incomplete_sample_calls);
    const u64 topology_blocked = load_eligibility_counter(
        eligibility_counters.topology_blocked_calls);
    const u64 read_blocked = load_eligibility_counter(
        eligibility_counters.read_blocked_calls);
    const u64 special_blocked = load_eligibility_counter(
        eligibility_counters.special_blocked_calls);
    const u64 gpu_dirty_blocked = load_eligibility_counter(
        eligibility_counters.gpu_dirty_blocked_calls);
    const u64 source_blocked = load_eligibility_counter(
        eligibility_counters.source_blocked_calls);
    const u64 mapping_blocked = load_eligibility_counter(
        eligibility_counters.mapping_blocked_calls);
    const u64 busy_blocked = load_eligibility_counter(
        eligibility_counters.busy_blocked_calls);
    const u64 eligible_calls = load_eligibility_counter(
        eligibility_counters.eligible_calls);
    const u64 classified_invalidations =
        incomplete_samples + alias_blocked + topology_blocked + read_blocked + special_blocked +
        gpu_dirty_blocked + source_blocked + mapping_blocked + busy_blocked +
        eligible_calls;
    const u64 source_none = load_eligibility_counter(
        eligibility_counters.source_none_pages);
    const u64 source_buffer = load_eligibility_counter(
        eligibility_counters.source_buffer_pages);
    const u64 source_texture = load_eligibility_counter(
        eligibility_counters.source_texture_pages);
    const u64 source_mixed = load_eligibility_counter(
        eligibility_counters.source_mixed_pages);
    const u64 source_mismatch = load_eligibility_counter(
        eligibility_counters.source_mismatch_pages);
    const u64 source_classified_pages = source_none + source_buffer +
                                        source_texture + source_mixed +
                                        source_mismatch;
    const u64 eligible_wait = load_eligibility_counter(
        eligibility_counters.eligible_sync_wait_ns);
    const u64 blocked_wait = load_eligibility_counter(
        eligibility_counters.blocked_sync_wait_ns);
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_GUEST_INVALIDATION_ELIGIBILITY] "
        "aliasObservedCalls=%llu topologyChangedCalls=%llu "
        "incompleteSampleCalls=%llu "
        "aliasBlockedCalls=%llu topologyBlockedCalls=%llu "
        "readBlockedCalls=%llu specialBlockedCalls=%llu "
        "gpuDirtyBlockedCalls=%llu sourceBlockedCalls=%llu "
        "mappingBlockedCalls=%llu busyBlockedCalls=%llu eligibleCalls=%llu "
        "classifiedInvalidations=%llu eligibleSyncWaitNs=%llu "
        "blockedSyncWaitNs=%llu classifiedSyncWaitNs=%llu",
        static_cast<unsigned long long>(load_eligibility_counter(
            eligibility_counters.alias_observed_calls)),
        static_cast<unsigned long long>(load_eligibility_counter(
            eligibility_counters.topology_changed_calls)),
        static_cast<unsigned long long>(incomplete_samples),
        static_cast<unsigned long long>(alias_blocked),
        static_cast<unsigned long long>(topology_blocked),
        static_cast<unsigned long long>(read_blocked),
        static_cast<unsigned long long>(special_blocked),
        static_cast<unsigned long long>(gpu_dirty_blocked),
        static_cast<unsigned long long>(source_blocked),
        static_cast<unsigned long long>(mapping_blocked),
        static_cast<unsigned long long>(busy_blocked),
        static_cast<unsigned long long>(eligible_calls),
        static_cast<unsigned long long>(classified_invalidations),
        static_cast<unsigned long long>(eligible_wait),
        static_cast<unsigned long long>(blocked_wait),
        static_cast<unsigned long long>(eligible_wait + blocked_wait));
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_GUEST_INVALIDATION_ELIGIBILITY_PAGES] "
        "sourceNonePages=%llu sourceBufferPages=%llu sourceTexturePages=%llu "
        "sourceMixedPages=%llu sourceMismatchPages=%llu "
        "sourceClassifiedPages=%llu candidatePages=%llu "
        "readWatcherPages=%llu actualReadProtectedPages=%llu "
        "memoryWaitPages=%llu jitPollPages=%llu descriptorWatcherPages=%llu "
        "descriptorConflictPages=%llu gpuDirtyPages=%llu",
        static_cast<unsigned long long>(source_none),
        static_cast<unsigned long long>(source_buffer),
        static_cast<unsigned long long>(source_texture),
        static_cast<unsigned long long>(source_mixed),
        static_cast<unsigned long long>(source_mismatch),
        static_cast<unsigned long long>(source_classified_pages),
        static_cast<unsigned long long>(load_eligibility_counter(
            eligibility_counters.candidate_pages)),
        static_cast<unsigned long long>(load_eligibility_counter(
            eligibility_counters.read_watcher_pages)),
        static_cast<unsigned long long>(load_eligibility_counter(
            eligibility_counters.actual_read_protected_pages)),
        static_cast<unsigned long long>(load_eligibility_counter(
            eligibility_counters.memory_wait_pages)),
        static_cast<unsigned long long>(load_eligibility_counter(
            eligibility_counters.jit_poll_pages)),
        static_cast<unsigned long long>(load_eligibility_counter(
            eligibility_counters.descriptor_watcher_pages)),
        static_cast<unsigned long long>(load_eligibility_counter(
            eligibility_counters.descriptor_conflict_pages)),
        static_cast<unsigned long long>(load_eligibility_counter(
            eligibility_counters.gpu_dirty_pages)));
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_GUEST_INVALIDATION_ELIGIBILITY_STATE] "
        "unmappedPages=%llu hostWriteDeniedPages=%llu "
        "guestWriteDeniedPages=%llu activeBulkWriterPages=%llu "
        "prepareReservationPages=%llu writeFaultBypassPages=%llu "
        "deferredWriteBypassPages=%llu bufferCaptureBlockedPages=%llu",
        static_cast<unsigned long long>(load_eligibility_counter(
            eligibility_counters.unmapped_pages)),
        static_cast<unsigned long long>(load_eligibility_counter(
            eligibility_counters.host_write_denied_pages)),
        static_cast<unsigned long long>(load_eligibility_counter(
            eligibility_counters.guest_write_denied_pages)),
        static_cast<unsigned long long>(load_eligibility_counter(
            eligibility_counters.active_bulk_writer_pages)),
        static_cast<unsigned long long>(load_eligibility_counter(
            eligibility_counters.prepare_reservation_pages)),
        static_cast<unsigned long long>(load_eligibility_counter(
            eligibility_counters.write_fault_bypass_pages)),
        static_cast<unsigned long long>(load_eligibility_counter(
            eligibility_counters.deferred_write_bypass_pages)),
        static_cast<unsigned long long>(load_eligibility_counter(
            eligibility_counters.buffer_capture_blocked_pages)));
}
enum class FexWriteEpochState : u32 {
    Free,
    Reserved,
    Faulted,
    Ready,
    Capturing,
    Draining,
    BulkOpen,
    BulkDraining,
};

constexpr size_t FexWriteEpochSlotCount = 4096;
constexpr u64 FexWriteEpochSlotMask = FexWriteEpochSlotCount - 1;
static_assert((FexWriteEpochSlotCount & FexWriteEpochSlotMask) == 0);
constexpr u32 FexWriteEpochPressureThreshold =
    (FexWriteEpochSlotCount * 3) / 4;
constexpr size_t FexBulkPageTokenCacheSize = 8192;
constexpr size_t FexBulkPageTokenCacheMask = FexBulkPageTokenCacheSize - 1;
static_assert((FexBulkPageTokenCacheSize & FexBulkPageTokenCacheMask) == 0);
constexpr size_t FexPendingPageBucketCount = 16384;
constexpr size_t FexPendingPageBucketMask = FexPendingPageBucketCount - 1;
static_assert((FexPendingPageBucketCount & FexPendingPageBucketMask) == 0);
constexpr u32 FexBufferCaptureFaultedRetryLimit = 32;

struct FexWriteEpochSlot {
    std::atomic<u32> state{static_cast<u32>(FexWriteEpochState::Free)};
    std::atomic<u64> token{};
    std::atomic<u32> bulk_writers{};
    VAddr address{};
    int owner_tid{};
};

struct FexWriteEpochCoordinator {
    std::array<FexWriteEpochSlot, FexWriteEpochSlotCount> slots{};
    std::array<std::atomic<u64>, FexBulkPageTokenCacheSize>
        bulk_page_tokens{};
    std::array<std::atomic<u32>, FexPendingPageBucketCount>
        pending_page_refs{};
    // A token is published only while its hash bucket has exactly one live
    // page reference.  Zero means that the bucket is empty, collided, or in a
    // transition and callers must retain the full slot scan.
    std::array<std::atomic<u64>, FexPendingPageBucketCount>
        pending_page_tokens{};
    std::atomic<u64> next_sequence{1};
    std::atomic<u32> active_armers{};
    std::atomic<u32> active_bulk_armers{};
    std::atomic<u32> active_bulk_writers{};
    std::atomic<u32> active_untracked_bulk_writers{};
    std::atomic<u32> active_buffer_captures{};
    std::atomic<u32> faulted{};
    std::atomic<u32> pending{};
    std::atomic<bool> gate_open{false};
    std::atomic<bool> bulk_gate_open{false};
    std::atomic<bool> enabled{};
    std::mutex drain_mutex;
    std::atomic<u64> armed{};
    std::atomic<u64> bulk_write_calls{};
    std::atomic<u64> bulk_write_pages{};
    std::atomic<u64> bulk_page_token_hits{};
    std::atomic<u64> drained{};
    std::atomic<u64> fallbacks{};
    std::atomic<u64> deferred_faulted_submits{};
    std::atomic<u64> forced_overlap_drains{};
    std::atomic<u64> buffer_captures{};
    std::atomic<u64> buffer_capture_pages{};
    std::atomic<u64> buffer_capture_waits{};
    std::atomic<u64> buffer_capture_faulted_fallbacks{};
    std::atomic<u64> buffer_capture_owner_kicks{};
    std::atomic<u64> buffer_capture_owner_kick_failures{};
};

FexWriteEpochCoordinator& FexWriteEpochs() {
    static FexWriteEpochCoordinator coordinator;
    return coordinator;
}

size_t FexPendingPageBucket(const VAddr address) noexcept {
    return static_cast<size_t>(
        (address / TRACKER_BYTES_PER_PAGE) & FexPendingPageBucketMask);
}

void AddFexPendingPageRef(const VAddr address, const u64 token) noexcept {
    auto& epochs = FexWriteEpochs();
    const size_t bucket = FexPendingPageBucket(address);
    const u32 previous = epochs.pending_page_refs[bucket].fetch_add(
        1, std::memory_order_acq_rel);
    epochs.pending_page_tokens[bucket].store(previous == 0 ? token : 0,
                                             std::memory_order_release);
}

void RemoveFexPendingPageRef(const VAddr address) noexcept {
    const u32 previous =
        FexWriteEpochs().pending_page_refs[FexPendingPageBucket(address)]
            .fetch_sub(1, std::memory_order_release);
    ASSERT_MSG(previous != 0, "FEX pending-page ref underflow");
    if (previous <= 2) {
        // previous==1 makes the bucket empty.  previous==2 leaves one token,
        // but its identity is intentionally not guessed; the safe full scan
        // remains active until the bucket is rebuilt from an empty state.
        FexWriteEpochs()
            .pending_page_tokens[FexPendingPageBucket(address)]
            .store(0, std::memory_order_release);
    }
}

bool EnterFexWriteEpochArming() noexcept {
    auto& epochs = FexWriteEpochs();
    if (!epochs.enabled.load(std::memory_order_acquire) ||
        !epochs.gate_open.load(std::memory_order_acquire)) {
        return false;
    }
    epochs.active_armers.fetch_add(1, std::memory_order_acq_rel);
    if (!epochs.enabled.load(std::memory_order_acquire) ||
        !epochs.gate_open.load(std::memory_order_acquire)) {
        epochs.active_armers.fetch_sub(1, std::memory_order_release);
        return false;
    }
    return true;
}

void LeaveFexWriteEpochArming() noexcept {
    FexWriteEpochs().active_armers.fetch_sub(1, std::memory_order_release);
}

bool TryEnterFexBulkEpochArming() noexcept {
    auto& epochs = FexWriteEpochs();
    if (!epochs.enabled.load(std::memory_order_acquire) ||
        !epochs.bulk_gate_open.load(std::memory_order_acquire)) {
        return false;
    }
    epochs.active_bulk_armers.fetch_add(1, std::memory_order_acq_rel);
    if (epochs.enabled.load(std::memory_order_acquire) &&
        epochs.bulk_gate_open.load(std::memory_order_acquire)) {
        return true;
    }
    const u32 previous = epochs.active_bulk_armers.fetch_sub(
        1, std::memory_order_release);
    ASSERT_MSG(previous != 0, "FEX bulk armer admission underflow");
    return false;
}

void LeaveFexBulkEpochArming() noexcept {
    FexWriteEpochs().active_bulk_armers.fetch_sub(
        1, std::memory_order_release);
}

FexWriteEpochSlot* ReserveFexWriteEpoch(const VAddr address,
                                        u64& token) noexcept {
    auto& epochs = FexWriteEpochs();
    if (ExecutorFexBufferCaptureAckEnabled() &&
        epochs.pending.load(std::memory_order_acquire) >=
            FexWriteEpochPressureThreshold) {
        epochs.fallbacks.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    for (u32 attempt = 0; attempt < 32; ++attempt) {
        const u64 sequence =
            epochs.next_sequence.fetch_add(1, std::memory_order_relaxed);
        const size_t index = static_cast<size_t>(sequence & FexWriteEpochSlotMask);
        FexWriteEpochSlot& slot = epochs.slots[index];
        u32 expected = static_cast<u32>(FexWriteEpochState::Free);
        if (!slot.state.compare_exchange_strong(
                expected, static_cast<u32>(FexWriteEpochState::Reserved),
                std::memory_order_acquire, std::memory_order_relaxed)) {
            continue;
        }
        token = (sequence << 12) | index;
        slot.address = Common::AlignDown(address, TRACKER_BYTES_PER_PAGE);
        slot.owner_tid = static_cast<int>(LSX4_PAGE_HOST_TID());
        slot.token.store(token, std::memory_order_relaxed);
        slot.state.store(static_cast<u32>(FexWriteEpochState::Faulted),
                         std::memory_order_release);
        epochs.faulted.fetch_add(1, std::memory_order_release);
        epochs.pending.fetch_add(1, std::memory_order_release);
        AddFexPendingPageRef(slot.address, token);
        return &slot;
    }
    epochs.fallbacks.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

FexWriteEpochSlot* AcquireFexBulkPageEpoch(const VAddr address, u64& token,
                                           bool& owned) noexcept {
    auto& epochs = FexWriteEpochs();
    const VAddr page_address =
        Common::AlignDown(address, TRACKER_BYTES_PER_PAGE);
    const size_t cache_index = static_cast<size_t>(
        (page_address / TRACKER_BYTES_PER_PAGE) & FexBulkPageTokenCacheMask);
    const u64 cached_token =
        epochs.bulk_page_tokens[cache_index].load(std::memory_order_acquire);
    if (cached_token != 0) {
        FexWriteEpochSlot& cached_slot = epochs.slots[static_cast<size_t>(
            cached_token & FexWriteEpochSlotMask)];
        if (cached_slot.token.load(std::memory_order_acquire) == cached_token &&
            cached_slot.state.load(std::memory_order_acquire) ==
                static_cast<u32>(FexWriteEpochState::BulkOpen) &&
            cached_slot.address == page_address) {
            cached_slot.bulk_writers.fetch_add(1, std::memory_order_acq_rel);
            if (cached_slot.token.load(std::memory_order_acquire) == cached_token &&
                cached_slot.state.load(std::memory_order_acquire) ==
                    static_cast<u32>(FexWriteEpochState::BulkOpen)) {
                token = cached_token;
                owned = false;
                epochs.bulk_page_token_hits.fetch_add(1,
                                                       std::memory_order_relaxed);
                return &cached_slot;
            }
            const u32 previous = cached_slot.bulk_writers.fetch_sub(
                1, std::memory_order_release);
            ASSERT_MSG(previous != 0, "FEX bulk page writer underflow");
        }
    }

    if (ExecutorFexBufferCaptureAckEnabled() &&
        epochs.pending.load(std::memory_order_acquire) >=
            FexWriteEpochPressureThreshold) {
        epochs.fallbacks.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    for (u32 attempt = 0; attempt < 32; ++attempt) {
        const u64 sequence =
            epochs.next_sequence.fetch_add(1, std::memory_order_relaxed);
        const size_t index =
            static_cast<size_t>(sequence & FexWriteEpochSlotMask);
        FexWriteEpochSlot& slot = epochs.slots[index];
        u32 expected = static_cast<u32>(FexWriteEpochState::Free);
        if (!slot.state.compare_exchange_strong(
                expected, static_cast<u32>(FexWriteEpochState::Reserved),
                std::memory_order_acquire, std::memory_order_relaxed)) {
            continue;
        }
        token = (sequence << 12) | index;
        slot.address = page_address;
        slot.bulk_writers.store(1, std::memory_order_relaxed);
        slot.token.store(token, std::memory_order_relaxed);
        slot.state.store(static_cast<u32>(FexWriteEpochState::BulkOpen),
                         std::memory_order_release);
        epochs.pending.fetch_add(1, std::memory_order_release);
        AddFexPendingPageRef(slot.address, token);
        owned = true;
        epochs.bulk_page_tokens[cache_index].store(token,
                                                   std::memory_order_release);
        return &slot;
    }
    epochs.fallbacks.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

void ReleaseFexBulkPageEpoch(const u64 token) noexcept {
    if (token == 0) {
        return;
    }
    FexWriteEpochSlot& slot = FexWriteEpochs().slots[
        static_cast<size_t>(token & FexWriteEpochSlotMask)];
    ASSERT_MSG(slot.token.load(std::memory_order_acquire) == token,
               "Stale FEX bulk page token");
    const u32 previous =
        slot.bulk_writers.fetch_sub(1, std::memory_order_release);
    ASSERT_MSG(previous != 0, "FEX bulk page writer underflow");
}

void CancelFexWriteEpoch(FexWriteEpochSlot& slot,
                         const u64 token) noexcept {
    if (slot.token.load(std::memory_order_acquire) != token) {
        return;
    }
    const VAddr slot_address = slot.address;
    u32 expected = static_cast<u32>(FexWriteEpochState::Faulted);
    if (slot.state.compare_exchange_strong(
            expected, static_cast<u32>(FexWriteEpochState::Free),
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        auto& epochs = FexWriteEpochs();
        epochs.faulted.fetch_sub(1, std::memory_order_release);
        epochs.pending.fetch_sub(1, std::memory_order_release);
        RemoveFexPendingPageRef(slot_address);
        epochs.fallbacks.fetch_add(1, std::memory_order_relaxed);
    }
}

static_assert(std::atomic<u64>::is_always_lock_free,
              "Owned-page fault counters must remain signal-safe");

}
#endif

#if defined(LSX4_APPLE_RUNTIME)
constexpr size_t PAGE_SIZE = TRACKER_BYTES_PER_PAGE;
constexpr size_t PAGE_BITS = TRACKER_PAGE_BITS;
#else
constexpr size_t PAGE_SIZE = 4_KB;
constexpr size_t PAGE_BITS = 12;
#endif

enum class ExecutorFexBulkLeasePhase : u8 {
    Idle,
    Prepared,
    Entered,
    Writing,
    Closing,
};

struct BulkGuestWriteThreadState {
    VAddr begin{};
    VAddr end{};
    bool consumed{};
    ExecutorFexBulkLeasePhase fex_lease_phase{
        ExecutorFexBulkLeasePhase::Idle};
};

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
thread_local bool executor_handling_guest_fault = false;
thread_local VAddr executor_bulk_write_begin = 0;
thread_local VAddr executor_bulk_write_end = 0;
thread_local bool executor_bulk_write_consumed = false;
thread_local BulkGuestWriteThreadState executor_bulk_write_hoisted_state{};
struct ExecutorSequentialBufferFaultState {
    VAddr expected_page{};
    u8 streak{};
};
thread_local ExecutorSequentialBufferFaultState
    executor_sequential_buffer_fault_state{};
thread_local bool executor_unwatched_bulk_write_active = false;
thread_local VAddr executor_unwatched_bulk_write_address = 0;
thread_local u64 executor_unwatched_bulk_write_size = 0;
thread_local bool executor_tracked_bulk_write_gate_active = false;
thread_local bool executor_tracked_bulk_write_activity_entered = false;
thread_local VAddr executor_tracked_bulk_write_address = 0;
thread_local u64 executor_tracked_bulk_write_size = 0;
constexpr size_t ExecutorHleCleanBufferMaxPages = 16;
struct ExecutorHleCleanBufferWriteTicket {
    std::optional<Core::MemoryManager::SnapshotReadGuard> topology_guard;
    VAddr address{};
    u64 size{};
    VAddr page_begin{};
    u64 page_bytes{};
    size_t first_page{};
    size_t page_end{};
    u64 attempt_ordinal{};
    bool active{};
    bool entered{};
    bool pipeline_active{};
    bool counted_active{};
};
thread_local ExecutorHleCleanBufferWriteTicket
    executor_hle_clean_buffer_write_ticket{};
constexpr size_t ExecutorFexDeferredBulkWritePageCapacity = 256;
struct ExecutorFexDeferredBulkWriteTicket {
    std::array<u64, ExecutorFexDeferredBulkWritePageCapacity> tokens{};
    std::array<bool, ExecutorFexDeferredBulkWritePageCapacity> owned{};
    u32 count{};
    bool active{};
};
thread_local ExecutorFexDeferredBulkWriteTicket
    executor_fex_deferred_bulk_write_ticket{};
struct ExecutorFexBufferCaptureTicket {
    std::array<u64, FexWriteEpochSlotCount> tokens{};
    static constexpr size_t PageCapacity = 16384;
    std::array<VAddr, PageCapacity> guarded_pages{};
    std::array<VAddr, FexWriteEpochSlotCount> dirty_pages{};
    u32 token_count{};
    u32 guarded_page_count{};
    u32 dirty_page_count{};
    bool active{};
};
thread_local ExecutorFexBufferCaptureTicket
    executor_fex_buffer_capture_ticket{};
thread_local bool executor_fex_bulk_writer_admitted = false;
thread_local bool executor_fex_bulk_writer_untracked = false;
thread_local bool executor_fex_bulk_activity_deferred = false;
std::atomic<u64> executor_bulk_prepare_token_counter{1};
enum class ExecutorBulkPreparePhase : u8 {
    Idle,
    Preparing,
    Prepared,
};
struct ExecutorBulkPrepareTicket {
    boost::container::small_vector<u32, 16> generations;
    VAddr address{};
    u64 size{};
    u64 reservation_token{};
    ExecutorBulkPreparePhase phase{ExecutorBulkPreparePhase::Idle};
};
thread_local ExecutorBulkPrepareTicket executor_bulk_prepare_ticket{};
thread_local bool executor_guest_protection_change_active = false;
thread_local ExecutorFexBulkLeasePhase executor_fex_bulk_lease_phase =
    ExecutorFexBulkLeasePhase::Idle;
thread_local u64 executor_fex_bulk_lease_epoch = 0;
thread_local VAddr executor_fex_bulk_lease_address = 0;
thread_local u64 executor_fex_bulk_lease_size = 0;

bool ExecutorHleBulkWriteTlsHoistEnabled() noexcept {
    static const bool enabled = [] {
        constexpr const char* preserve_marker =
            "/data/data/app.lsx4.android/files/lsx4-home/"
            "run-preserve-hle-bulk-write-tls-lookups";
        const bool sm8650 = Common::IsSnapdragon8Gen3Host();
        const bool sm8850 = Common::IsSnapdragon8Gen5Host();
        const bool preserve = ::access(preserve_marker, F_OK) == 0;
        const bool selected = (sm8650 || sm8850) && !preserve;
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_HLE_BULK_WRITE_TLS_HOIST] enabled=%u sm8650=%u "
            "sm8850=%u preserve=%u legacyFallback=1 fields=3 "
            "fexLeasePhaseGrouped=1 marker=%s",
            selected ? 1u : 0u, sm8650 ? 1u : 0u, sm8850 ? 1u : 0u,
            preserve ? 1u : 0u, preserve_marker);
        return selected;
    }();
    return enabled;
}

ExecutorFexBulkLeasePhase& CurrentFexBulkLeasePhase() noexcept {
    if (BulkGuestWriteThreadState* const state =
            ResolveBulkGuestWriteThreadState();
        state != nullptr) {
        return state->fex_lease_phase;
    }
    // Keep the legacy emulated-TLS resolver below the null branch.  Without
    // this compiler barrier, Clang speculates both __emutls_get_address calls
    // and selects the result, turning a one-lookup route into two.
    asm volatile("" ::: "memory");
    return executor_fex_bulk_lease_phase;
}

void BeginCurrentFexBulkLease(const VAddr address, const u64 size) noexcept {
    auto& phase = CurrentFexBulkLeasePhase();
    ASSERT_MSG(phase == ExecutorFexBulkLeasePhase::Idle,
               "Nested FEX bulk lease");
    ASSERT_MSG(!executor_fex_bulk_activity_deferred,
               "Stale FEX bulk activity mode");
    ++executor_fex_bulk_lease_epoch;
    if (executor_fex_bulk_lease_epoch == 0) {
        ++executor_fex_bulk_lease_epoch;
    }
    executor_fex_bulk_lease_address = address;
    executor_fex_bulk_lease_size = size;
    phase = ExecutorFexBulkLeasePhase::Prepared;
}

bool AdmitCurrentUntrackedFexBulkWriter(const VAddr address,
                                        const u64 size) noexcept {
    auto& epochs = FexWriteEpochs();
    auto& phase = CurrentFexBulkLeasePhase();
    if (phase == ExecutorFexBulkLeasePhase::Idle) {
        BeginCurrentFexBulkLease(address, size);
    }
    ASSERT_MSG(phase == ExecutorFexBulkLeasePhase::Prepared &&
                   executor_fex_bulk_lease_address == address &&
                   executor_fex_bulk_lease_size == size,
               "Mismatched untracked FEX bulk lease admission");
    if (executor_fex_bulk_writer_admitted) {
        return true;
    }

    u32 waits = 0;
    while (epochs.enabled.load(std::memory_order_acquire)) {
        if (!epochs.bulk_gate_open.load(std::memory_order_acquire)) {
            if (++waits < 128) {
#if defined(__aarch64__)
                asm volatile("yield" ::: "memory");
#else
                std::this_thread::yield();
#endif
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
            continue;
        }
        epochs.active_untracked_bulk_writers.fetch_add(
            1, std::memory_order_acq_rel);
        epochs.active_bulk_writers.fetch_add(1, std::memory_order_acq_rel);
        if (epochs.enabled.load(std::memory_order_acquire) &&
            epochs.bulk_gate_open.load(std::memory_order_acquire)) {
            executor_fex_bulk_writer_admitted = true;
            executor_fex_bulk_writer_untracked = true;
            return true;
        }
        epochs.active_bulk_writers.fetch_sub(1, std::memory_order_release);
        epochs.active_untracked_bulk_writers.fetch_sub(
            1, std::memory_order_release);
    }
    return false;
}

bool TryAdmitCurrentUntrackedFexBulkWriterOnce(
    const VAddr address, const u64 size) noexcept {
    auto& epochs = FexWriteEpochs();
    auto& phase = CurrentFexBulkLeasePhase();
    if (phase == ExecutorFexBulkLeasePhase::Idle) {
        BeginCurrentFexBulkLease(address, size);
    }
    ASSERT_MSG(phase == ExecutorFexBulkLeasePhase::Prepared &&
                   executor_fex_bulk_lease_address == address &&
                   executor_fex_bulk_lease_size == size,
               "Mismatched one-shot FEX bulk lease admission");
    if (executor_fex_bulk_writer_admitted) {
        return true;
    }
    if (!epochs.enabled.load(std::memory_order_acquire) ||
        !epochs.bulk_gate_open.load(std::memory_order_acquire)) {
        return false;
    }

    epochs.active_untracked_bulk_writers.fetch_add(
        1, std::memory_order_acq_rel);
    epochs.active_bulk_writers.fetch_add(1, std::memory_order_acq_rel);
    if (epochs.enabled.load(std::memory_order_acquire) &&
        epochs.bulk_gate_open.load(std::memory_order_acquire)) {
        executor_fex_bulk_writer_admitted = true;
        executor_fex_bulk_writer_untracked = true;
        return true;
    }

    const u32 previous_bulk = epochs.active_bulk_writers.fetch_sub(
        1, std::memory_order_release);
    const u32 previous_untracked =
        epochs.active_untracked_bulk_writers.fetch_sub(
            1, std::memory_order_release);
    ASSERT_MSG(previous_bulk != 0,
               "FEX one-shot bulk writer admission underflow");
    ASSERT_MSG(previous_untracked != 0,
               "FEX one-shot untracked writer admission underflow");
    return false;
}

void ReleaseCurrentFexBulkWriterAdmission() noexcept {
    if (!executor_fex_bulk_writer_admitted) {
        return;
    }
    executor_fex_bulk_writer_admitted = false;
    auto& epochs = FexWriteEpochs();
    if (executor_fex_bulk_writer_untracked) {
        executor_fex_bulk_writer_untracked = false;
        const u32 previous_untracked =
            epochs.active_untracked_bulk_writers.fetch_sub(
                1, std::memory_order_release);
        ASSERT_MSG(previous_untracked != 0,
                   "FEX untracked bulk writer admission underflow");
    }
    const u32 previous = epochs.active_bulk_writers.fetch_sub(
        1, std::memory_order_release);
    ASSERT_MSG(previous != 0, "FEX bulk writer admission underflow");
}

void ClearCurrentBulkPrepareTicket() noexcept {
    auto& ticket = executor_bulk_prepare_ticket;
    ticket.phase = ExecutorBulkPreparePhase::Idle;
    ticket.address = 0;
    ticket.size = 0;
    ticket.reservation_token = 0;
    ticket.generations.clear();
}

u64 NextBulkPrepareReservationToken() noexcept {
    for (;;) {
        const u64 token = executor_bulk_prepare_token_counter.fetch_add(
            1, std::memory_order_relaxed);
        if (token != 0) {
            return token;
        }
    }
}

void PublishAndClearCurrentFexDeferredBulkTicket() noexcept {
    auto& ticket = executor_fex_deferred_bulk_write_ticket;
    if (!ticket.active) {
        return;
    }
    const u32 count = ticket.count;
    for (u32 index = 0; index < count; ++index) {
        ReleaseFexBulkPageEpoch(ticket.tokens[index]);
        ticket.tokens[index] = 0;
        ticket.owned[index] = false;
    }
    ticket.count = 0;
    ticket.active = false;
}

bool CurrentFexDeferredBulkTicketOwnsPage(
    const VAddr page_address) noexcept {
    const auto& ticket = executor_fex_deferred_bulk_write_ticket;
    if (!ticket.active) {
        return false;
    }
    auto& epochs = FexWriteEpochs();
    for (u32 index = 0; index < ticket.count; ++index) {
        const u64 token = ticket.tokens[index];
        if (token == 0) {
            continue;
        }
        const FexWriteEpochSlot& slot = epochs.slots[static_cast<size_t>(
            token & FexWriteEpochSlotMask)];
        if (slot.token.load(std::memory_order_acquire) != token ||
            slot.address != page_address) {
            continue;
        }
        const u32 state = slot.state.load(std::memory_order_acquire);
        if (state == static_cast<u32>(FexWriteEpochState::BulkOpen) ||
            state == static_cast<u32>(FexWriteEpochState::BulkDraining)) {
            return true;
        }
    }
    return false;
}
#endif

SHAD_NO_INLINE BulkGuestWriteThreadState*
ResolveBulkGuestWriteThreadState() noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (!ExecutorHleBulkWriteTlsHoistEnabled()) {
        return nullptr;
    }
    // The barrier keeps __emutls_get_address in the selected branch.  This is
    // a hot ABI seam and must not resolve candidate TLS in the legacy control.
    asm volatile("" ::: "memory");
    return &executor_bulk_write_hoisted_state;
#else
    return nullptr;
#endif
}

void BeginBulkGuestWrite(BulkGuestWriteThreadState* const state,
                         const VAddr address, const u64 size) noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (state != nullptr) {
        state->begin = address;
        state->end = size <= std::numeric_limits<VAddr>::max() - address
                         ? address + size
                         : address;
        state->consumed = false;
        if (state->fex_lease_phase == ExecutorFexBulkLeasePhase::Entered) {
            state->fex_lease_phase = ExecutorFexBulkLeasePhase::Writing;
        }
        return;
    }
    executor_bulk_write_begin = address;
    executor_bulk_write_end =
        size <= std::numeric_limits<VAddr>::max() - address ? address + size : address;
    executor_bulk_write_consumed = false;
    if (executor_fex_bulk_lease_phase == ExecutorFexBulkLeasePhase::Entered) {
        executor_fex_bulk_lease_phase = ExecutorFexBulkLeasePhase::Writing;
    }
#else
    (void)state;
    (void)address;
    (void)size;
#endif
}

void EndBulkGuestWrite(BulkGuestWriteThreadState* const state) noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (state != nullptr) {
        state->begin = 0;
        state->end = 0;
        state->consumed = false;
    } else {
        executor_bulk_write_begin = 0;
        executor_bulk_write_end = 0;
        executor_bulk_write_consumed = false;
    }
#else
    (void)state;
#endif
}

void BeginBulkGuestWrite(const VAddr address, const u64 size) noexcept {
    BeginBulkGuestWrite(ResolveBulkGuestWriteThreadState(), address, size);
}

void EndBulkGuestWrite() noexcept {
    EndBulkGuestWrite(ResolveBulkGuestWriteThreadState());
}

struct PageManager::Impl {
    struct PageState {
        // Submit-wide readback ranges and completion-label sidecars can
        // legitimately overlap the same guest page more than three times.
        // The old two-bit read counter wrapped to zero, then a matching
        // retirement looked like an underflow and also dropped protection too
        // early. Keep exact watcher parity; this table is still compact enough
        // for the 40-bit guest address space.
        u32 num_write_watchers : 7;
        u32 num_read_watchers : 8;
        u32 memory_wait_write_watcher : 1;
        u32 jit_poll_write_watcher : 1;
        u32 descriptor_snapshot_write_watcher : 1;
        u32 num_bulk_writers : 13;
        u32 bulk_writer_overflow : 1;

        Core::MemoryPermission WritePerm() const noexcept {
            return num_write_watchers == 0 &&
                           memory_wait_write_watcher == 0 &&
                           jit_poll_write_watcher == 0 &&
                           descriptor_snapshot_write_watcher == 0
                       ? Core::MemoryPermission::Write
                       : Core::MemoryPermission::None;
        }

        Core::MemoryPermission ReadPerm() const noexcept {
            return num_read_watchers == 0 ? Core::MemoryPermission::Read
                                          : Core::MemoryPermission::None;
        }

        Core::MemoryPermission WatcherPerms() const noexcept {
            return ReadPerm() | WritePerm();
        }

        template <s32 delta, bool is_read>
        u8 AddDelta() {
            if constexpr (is_read) {
                if constexpr (delta == 1) {
                    ASSERT_MSG(num_read_watchers < 255, "Too many read watchers");
                    return ++num_read_watchers;
                } else if (delta == -1) {
                    ASSERT_MSG(num_read_watchers > 0, "Not enough watchers");
                    return --num_read_watchers;
                } else {
                    return num_read_watchers;
                }
            } else {
                if constexpr (delta == 1) {
                    ASSERT_MSG(num_write_watchers < 127, "Too many write watchers");
                    return ++num_write_watchers;
                } else if (delta == -1) {
                    ASSERT_MSG(num_write_watchers > 0, "Not enough watchers");
                    return --num_write_watchers;
                } else {
                    return num_write_watchers;
                }
            }
        }
    };
    static_assert(sizeof(PageState) == 4,
                  "The 40-bit page-state table must remain compact");

    static constexpr size_t ADDRESS_BITS = 40;
    static constexpr size_t NUM_ADDRESS_PAGES = 1ULL << (40 - PAGE_BITS);
    static constexpr size_t NUM_ADDRESS_LOCKS = NUM_ADDRESS_PAGES / PAGES_PER_LOCK;
    static constexpr size_t NUM_ADDRESS_CHUNKS =
        NUM_ADDRESS_PAGES / PAGES_PER_LOCK;
    static constexpr size_t WORDS_PER_CHUNK = PAGES_PER_LOCK / 64;

    struct HleResidencyCounts {
        std::array<u32, PAGES_PER_LOCK> texture{};
        std::array<u32, PAGES_PER_LOCK> shader_read{};
    };

    struct PageChunk {
        std::array<PageState, PAGES_PER_LOCK> pages{};
        // Low/high halves are independent read/write watcher generations.
        // They only need to distinguish transitions during one signal-handler
        // invocation, so 16 bits per access type avoids doubling PageState.
        std::array<u32, PAGES_PER_LOCK> fault_generation{};
        std::array<u32, PAGES_PER_LOCK> bulk_contract_generation{};
        std::array<u64, PAGES_PER_LOCK> bulk_prepare_reservation{};
        std::array<u8, PAGES_PER_LOCK> buffer_write_watchers{};
        std::array<u8, PAGES_PER_LOCK> texture_write_watchers{};
        // Marker-off chunks pay only one null pointer. The two 4-KiB count
        // arrays are allocated lazily on the first experimental residency.
        std::unique_ptr<HleResidencyCounts> hle_residency_counts{};
        std::array<u64, WORDS_PER_CHUNK> gpu_mapped{};
        std::array<u64, WORDS_PER_CHUNK> base_host_read{};
        std::array<u64, WORDS_PER_CHUNK> base_host_write{};
        std::array<u64, WORDS_PER_CHUNK> base_host_execute{};
        std::array<u64, WORDS_PER_CHUNK> guest_cpu_read{};
        std::array<u64, WORDS_PER_CHUNK> guest_cpu_write{};
        std::array<u64, WORDS_PER_CHUNK> actual_read_protection{};
        std::array<std::atomic<u64>, WORDS_PER_CHUNK> actual_write_protection{};
        std::array<std::atomic<u64>, WORDS_PER_CHUNK> write_fault_bypass{};
        std::array<std::atomic<u64>, WORDS_PER_CHUNK> fex_deferred_write_bypass{};
        std::array<std::atomic<u64>, WORDS_PER_CHUNK> fex_buffer_capture_blocked{};
        std::array<std::atomic<u64>, WORDS_PER_CHUNK> write_watcher_hazard{};
        // Once residency accounting is observed inconsistent, keep the
        // corresponding lock-free hazard sticky even if later balanced
        // Track/Untrack traffic brings the ordinary count back to zero.
        std::array<std::atomic<u64>, WORDS_PER_CHUNK> hle_residency_poison{};
        std::array<std::atomic<u64>, WORDS_PER_CHUNK> hle_residency_hazard{};
        std::array<u64, WORDS_PER_CHUNK> gpu_dirty{};
        std::array<u64, WORDS_PER_CHUNK> descriptor_snapshot_conflict{};
    };
    static_assert(std::atomic<u64>::is_always_lock_free,
                  "Bulk-write hazard bits must remain lock-free");

    inline static Vulkan::Rasterizer* rasterizer;
    inline static Impl* owner;
#ifdef ENABLE_USERFAULTFD
    Impl(Vulkan::Rasterizer* rasterizer_) {
        rasterizer = rasterizer_;
        owner = this;
        uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
        ASSERT_MSG(uffd != -1, "{}", Common::GetLastErrorMsg());

        uffdio_api api;
        api.api = UFFD_API;
        api.features = UFFD_FEATURE_THREAD_ID;
        const int ret = ioctl(uffd, UFFDIO_API, &api);
        ASSERT(ret == 0 && api.api == UFFD_API);

        ufd_thread = std::jthread([&](std::stop_token token) { UffdHandler(token); });
    }

    void OnMap(VAddr address, size_t size,
               Core::MemoryPermission host_permissions,
               bool guest_cpu_read_allowed,
               bool guest_cpu_write_allowed) {
        (void)host_permissions;
        (void)guest_cpu_read_allowed;
        (void)guest_cpu_write_allowed;
        uffdio_register reg;
        reg.range.start = address;
        reg.range.len = size;
        reg.mode = UFFDIO_REGISTER_MODE_WP;
        const int ret = ioctl(uffd, UFFDIO_REGISTER, &reg);
        ASSERT_MSG(ret != -1, "Uffdio register failed");
    }

    void OnUnmap(VAddr address, size_t size) {
        uffdio_range range;
        range.start = address;
        range.len = size;
        const int ret = ioctl(uffd, UFFDIO_UNREGISTER, &range);
        ASSERT_MSG(ret != -1, "Uffdio unregister failed");
    }

    bool OnGuestProtectionChanged(
        VAddr address, size_t size,
        Core::MemoryPermission host_permissions,
        bool guest_cpu_read_allowed,
        bool guest_cpu_write_allowed) {
        (void)address;
        (void)size;
        (void)host_permissions;
        (void)guest_cpu_read_allowed;
        (void)guest_cpu_write_allowed;
        return false;
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) {
        bool allow_write = True(perms & Core::MemoryPermission::Write);
        uffdio_writeprotect wp;
        wp.range.start = address;
        wp.range.len = size;
        wp.mode = allow_write ? 0 : UFFDIO_WRITEPROTECT_MODE_WP;
        const int ret = ioctl(uffd, UFFDIO_WRITEPROTECT, &wp);
        ASSERT_MSG(ret != -1, "Uffdio writeprotect failed with error: {}",
                   Common::GetLastErrorMsg());
    }

    void UffdHandler(std::stop_token token) {
        while (!token.stop_requested()) {
            pollfd pollfd;
            pollfd.fd = uffd;
            pollfd.events = POLLIN;

            const int pollres = poll(&pollfd, 1, -1);
            switch (pollres) {
            case -1:
                perror("Poll userfaultfd");
                continue;
                break;
            case 0:
                continue;
            case 1:
                break;
            default:
                UNREACHABLE_MSG("Unexpected number of descriptors {} out of poll", pollres);
            }

            ASSERT_MSG(!(pollfd.revents & POLLERR), "POLLERR on userfaultfd");

            if (!(pollfd.revents & POLLIN)) {
                continue;
            }

            uffd_msg msg;
            const int readret = read(uffd, &msg, sizeof(msg));
            ASSERT_MSG(readret != -1 || errno == EAGAIN, "Unexpected result of uffd read");
            if (errno == EAGAIN) {
                continue;
            }
            ASSERT_MSG(readret == sizeof(msg), "Unexpected short read, exiting");
            ASSERT(msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WP);

            const VAddr addr = msg.arg.pagefault.address;
            const bool wait_event = rasterizer->ConsumeMemoryWaitWriteFault(addr);
            const bool jit_poll_event = rasterizer->ConsumeJitPollWriteFault(addr);
            const bool invalidated = rasterizer->InvalidateMemory(addr, 1);
            ASSERT_MSG(wait_event || jit_poll_event || invalidated,
                       "Unhandled userfaultfd write protection at {:#x}", addr);
        }
    }

    std::jthread ufd_thread;
    int uffd;
#else
    Impl(Vulkan::Rasterizer* rasterizer_) {
        rasterizer = rasterizer_;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        // Resolve the weak bridge before registering the signal handler. The
        // handler then performs only an indirect call through this immutable
        // pointer, never first-use PLT resolution from signal context.
        signal_context_pc_classifier =
            executor_jit_signal_context_pc_is_generated_code;
        fex_signal_context_pc_classifier =
            executor_lsx4_android_fex_signal_context_pc_is_generated_code;
#endif
        owner = this;
        for (auto& chunk : page_chunks) {
            chunk.store(nullptr, std::memory_order_relaxed);
        }

        constexpr auto priority = std::numeric_limits<u32>::min();
        Core::Signals::Instance()->RegisterAccessViolationHandler(GuestFaultSignalHandler,
                                                                  priority);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (ExecutorReadbackContractTraceEnabled()) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_READBACK_CONTRACT_INIT] mode=%d hostPageSize=%zu trackerPageSize=%zu",
                Config::getReadbacksMode(), static_cast<size_t>(::sysconf(_SC_PAGESIZE)),
                PAGE_SIZE);
        }
#endif
    }

    void OnMap(VAddr address, size_t size,
               Core::MemoryPermission host_permissions,
               bool guest_cpu_read_allowed,
               bool guest_cpu_write_allowed) {
        if (size == 0) {
            return;
        }
        ASSERT_MSG((address & (PAGE_SIZE - 1)) == 0 &&
                       (size & (PAGE_SIZE - 1)) == 0,
                   "GPU mapping must be page aligned");
        EnsureChunks(address, size);
        const size_t first_page = address >> PAGE_BITS;
        const size_t page_end = first_page + (size >> PAGE_BITS);
        WithQuiescedBulkWriters(first_page, page_end, [&] {
            FillMappedProtection(address, size, host_permissions,
                                 guest_cpu_read_allowed,
                                 guest_cpu_write_allowed);
            for (size_t page = first_page; page < page_end; ++page) {
                AdvanceBulkContractGeneration(page);
            }
            ReconcileProtectionRange(address, size, host_permissions, true);
        });
    }

    void OnUnmap(VAddr address, size_t size) {
        if (size == 0) {
            return;
        }
        const size_t first_page = address >> PAGE_BITS;
        const size_t page_end = first_page + (size >> PAGE_BITS);
        WithQuiescedBulkWriters(first_page, page_end, [&] {
            ClearMappedProtection(address, size);
            FillActualWriteProtection(address, size, false);
            FillActualReadProtection(address, size, false);
            for (size_t page = first_page; page < page_end; ++page) {
                AdvanceBulkContractGeneration(page);
                SetFexDeferredWriteBypass(page, false);
                SetGpuDirty(page, false);
            }
        });
    }

    void UpdateGpuDirtyPages(const VAddr address, const u64 size,
                             const bool dirty) {
        if (size == 0 || address >= (u64{1} << ADDRESS_BITS) ||
            size > (u64{1} << ADDRESS_BITS) - address) {
            return;
        }
        const size_t first_page = address >> PAGE_BITS;
        const size_t page_end = Common::DivCeil(address + size, PAGE_SIZE);
        const auto update = [&] {
            for (size_t page = first_page; page < page_end; ++page) {
                if (GpuDirty(page) != dirty) {
                    SetGpuDirty(page, dirty);
                    AdvanceFaultGeneration(page, true);
                }
                if (dirty) {
                    CloseFexDeferredWriteBypass(page);
                }
            }
        };
        if (dirty) {
            WithQuiescedBulkWriters(first_page, page_end, update);
        } else {
            const auto lock_start =
                locks.begin() + (first_page / PAGES_PER_LOCK);
            const auto lock_end =
                locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
            Common::RangeLockGuard lk(lock_start, lock_end);
            update();
        }
    }

    bool OnGuestProtectionChanged(
        VAddr address, size_t size,
        Core::MemoryPermission host_permissions,
        bool guest_cpu_read_allowed,
        bool guest_cpu_write_allowed) {
        if (size == 0 || address >= (u64{1} << ADDRESS_BITS) ||
            size > (u64{1} << ADDRESS_BITS) - address ||
            (address & (PAGE_SIZE - 1)) != 0 ||
            (size & (PAGE_SIZE - 1)) != 0) {
            return false;
        }
        const size_t first_page = address >> PAGE_BITS;
        const size_t page_end = first_page + (size >> PAGE_BITS);
        bool mapped = true;
        WithQuiescedBulkWriters(first_page, page_end, [&] {
            for (size_t page = first_page; page < page_end; ++page) {
                if (!GpuMapped(page)) {
                    mapped = false;
                    return;
                }
            }
            for (size_t page = first_page; page < page_end; ++page) {
                CloseFexDeferredWriteBypass(page);
                SetPageBaseProtection(page, host_permissions,
                                      guest_cpu_read_allowed,
                                      guest_cpu_write_allowed);
            }
            ReconcileProtectionRange(address, size, host_permissions, true);
        });
        return mapped;
    }

    PageChunk* FindChunk(const size_t page) const noexcept {
        if (page >= NUM_ADDRESS_PAGES) {
            return nullptr;
        }
        return page_chunks[page / PAGES_PER_LOCK].load(
            std::memory_order_acquire);
    }

    PageChunk& EnsureChunk(const size_t page) {
        ASSERT(page < NUM_ADDRESS_PAGES);
        auto& slot = page_chunks[page / PAGES_PER_LOCK];
        PageChunk* chunk = slot.load(std::memory_order_acquire);
        if (chunk != nullptr) {
            return *chunk;
        }
        auto candidate = std::make_unique<PageChunk>();
        PageChunk* expected = nullptr;
        if (slot.compare_exchange_strong(expected, candidate.get(),
                                         std::memory_order_release,
                                         std::memory_order_acquire)) {
            chunk = candidate.release();
        } else {
            chunk = expected;
        }
        return *chunk;
    }

    void EnsureChunks(const VAddr address, const u64 size) {
        if (size == 0 || address >= (u64{1} << ADDRESS_BITS)) {
            return;
        }
        const u64 bounded_size =
            std::min(size, (u64{1} << ADDRESS_BITS) - address);
        const size_t first_page = address >> PAGE_BITS;
        const size_t end_page =
            Common::DivCeil(address + bounded_size, PAGE_SIZE);
        for (size_t page = first_page; page < end_page;
             page = std::min(end_page,
                             Common::AlignUp(page + 1, PAGES_PER_LOCK))) {
            (void)EnsureChunk(page);
        }
    }

    PageState& State(const size_t page) {
        return EnsureChunk(page).pages[page % PAGES_PER_LOCK];
    }

    const PageState* FindState(const size_t page) const noexcept {
        const PageChunk* chunk = FindChunk(page);
        return chunk != nullptr ? &chunk->pages[page % PAGES_PER_LOCK]
                                : nullptr;
    }

    template <auto Member>
    bool TestPageBit(const size_t page) const noexcept {
        const PageChunk* chunk = FindChunk(page);
        if (chunk == nullptr) {
            return false;
        }
        const size_t local_page = page % PAGES_PER_LOCK;
        const u64 word = (chunk->*Member)[local_page >> 6];
        return ((word >> (local_page & 63)) & 1u) != 0;
    }

    bool GpuMapped(const size_t page) const noexcept {
        return TestPageBit<&PageChunk::gpu_mapped>(page);
    }

    bool GuestCpuAccessAllowed(const size_t page, const bool is_write) const noexcept {
        return is_write ? TestPageBit<&PageChunk::guest_cpu_write>(page)
                        : TestPageBit<&PageChunk::guest_cpu_read>(page);
    }

    Core::MemoryPermission BaseHostPerms(const size_t page) const noexcept {
        Core::MemoryPermission perms{};
        if (TestPageBit<&PageChunk::base_host_read>(page)) {
            perms |= Core::MemoryPermission::Read;
        }
        if (TestPageBit<&PageChunk::base_host_write>(page)) {
            perms |= Core::MemoryPermission::Write;
        }
        if (TestPageBit<&PageChunk::base_host_execute>(page)) {
            perms |= Core::MemoryPermission::Execute;
        }
        return perms;
    }

    u16 FaultGeneration(const size_t page, const bool is_write) const noexcept {
        const PageChunk* chunk = FindChunk(page);
        if (chunk == nullptr) {
            return 0;
        }
        const u32 packed = chunk->fault_generation[page % PAGES_PER_LOCK];
        return static_cast<u16>(is_write ? packed >> 16 : packed);
    }

    static void AdvanceFaultGeneration(PageChunk& chunk,
                                       const size_t local_page,
                                       const bool is_write) noexcept {
        u32& packed = chunk.fault_generation[local_page];
        if (is_write) {
            const u32 next = ((packed >> 16) + 1u) & 0xffffu;
            packed = (packed & 0xffffu) | (next << 16);
            ++chunk.bulk_contract_generation[local_page];
        } else {
            packed = (packed & 0xffff0000u) |
                     ((packed + 1u) & 0xffffu);
        }
    }

    void AdvanceFaultGeneration(const size_t page,
                                const bool is_write) noexcept {
        PageChunk& chunk = EnsureChunk(page);
        AdvanceFaultGeneration(chunk, page % PAGES_PER_LOCK, is_write);
    }

    u8 WriteWatcherSourceMask(const size_t page) const noexcept {
        const PageChunk* chunk = FindChunk(page);
        if (chunk == nullptr) {
            return 0;
        }
        const size_t local_page = page % PAGES_PER_LOCK;
        return static_cast<u8>((chunk->buffer_write_watchers[local_page] != 0 ? 1 : 0) |
                               (chunk->texture_write_watchers[local_page] != 0 ? 2 : 0));
    }

    bool WriteWatcherHazard(const size_t page) const noexcept {
        const PageChunk* chunk = FindChunk(page);
        if (chunk == nullptr) {
            return false;
        }
        const size_t local_page = page % PAGES_PER_LOCK;
        const u64 word = chunk->write_watcher_hazard[local_page >> 6].load(
            std::memory_order_acquire);
        return ((word >> (local_page & 63)) & 1u) != 0;
    }

    static void PublishWriteWatcherHazard(PageChunk& chunk,
                                          const size_t local_page,
                                          const PageState& state) noexcept {
        const u64 mask = u64{1} << (local_page & 63);
        auto& word = chunk.write_watcher_hazard[local_page >> 6];
        if (!True(state.WritePerm() & Core::MemoryPermission::Write)) {
            word.fetch_or(mask, std::memory_order_release);
        } else {
            word.fetch_and(~mask, std::memory_order_release);
        }
    }

    void PublishWriteWatcherHazard(const size_t page,
                                   const PageState& state) noexcept {
        PageChunk& chunk = EnsureChunk(page);
        PublishWriteWatcherHazard(chunk, page % PAGES_PER_LOCK, state);
    }

    void UpdateWriteWatcherSource(const size_t page, const u8 source,
                                  const bool track) noexcept {
        PageChunk& chunk = EnsureChunk(page);
        u8& count = source == 2
                        ? chunk.texture_write_watchers[page % PAGES_PER_LOCK]
                        : chunk.buffer_write_watchers[page % PAGES_PER_LOCK];
        if (track) {
            ASSERT_MSG(count != 0xff, "Write watcher source count overflow");
            ++count;
        } else {
            ASSERT_MSG(count != 0, "Write watcher source count underflow");
            --count;
        }
    }

    void PublishHleResidencyHazard(PageChunk& chunk,
                                   const size_t local_page) noexcept {
        const u64 mask = u64{1} << (local_page & 63);
        auto& word = chunk.hle_residency_hazard[local_page >> 6];
        const bool poisoned =
            (chunk.hle_residency_poison[local_page >> 6].load(
                 std::memory_order_relaxed) &
             mask) != 0;
        const HleResidencyCounts* const counts =
            chunk.hle_residency_counts.get();
        if (poisoned ||
            (counts != nullptr &&
             (counts->texture[local_page] != 0 ||
              counts->shader_read[local_page] != 0))) {
            word.fetch_or(mask, std::memory_order_release);
        } else {
            word.fetch_and(~mask, std::memory_order_release);
        }
    }

    void PoisonHleResidency(PageChunk& chunk,
                            const size_t local_page) noexcept {
        hle_clean_buffer_fail_closed.store(true, std::memory_order_release);
        const u64 mask = u64{1} << (local_page & 63);
        chunk.hle_residency_poison[local_page >> 6].fetch_or(
            mask, std::memory_order_release);
        chunk.hle_residency_hazard[local_page >> 6].fetch_or(
            mask, std::memory_order_release);
    }

    bool HleResidencyHazard(const size_t page) const noexcept {
        const PageChunk* const chunk = FindChunk(page);
        if (chunk == nullptr) {
            return false;
        }
        const size_t local_page = page % PAGES_PER_LOCK;
        return ((chunk->hle_residency_hazard[local_page >> 6].load(
                     std::memory_order_acquire) >>
                 (local_page & 63)) &
                1u) != 0;
    }

    u32 TextureResidentCount(const size_t page) const noexcept {
        const PageChunk* const chunk = FindChunk(page);
        const HleResidencyCounts* const counts =
            chunk != nullptr ? chunk->hle_residency_counts.get() : nullptr;
        return counts != nullptr ? counts->texture[page % PAGES_PER_LOCK] : 0;
    }

    bool UpdateTextureResidentCount(const size_t page,
                                    const bool track) {
        PageChunk& chunk = EnsureChunk(page);
        if (chunk.hle_residency_counts == nullptr) {
            if (!track) {
                PoisonHleResidency(chunk, page % PAGES_PER_LOCK);
                return false;
            }
            chunk.hle_residency_counts =
                std::make_unique<HleResidencyCounts>();
        }
        u32& count = chunk.hle_residency_counts
                         ->texture[page % PAGES_PER_LOCK];
        if (track) {
            ASSERT_MSG(count != std::numeric_limits<u32>::max(),
                       "Texture residency counter overflow");
            if (count == std::numeric_limits<u32>::max()) {
                PoisonHleResidency(chunk, page % PAGES_PER_LOCK);
                return false;
            }
            ++count;
        } else {
            ASSERT_MSG(count != 0, "Texture residency counter underflow");
            if (count == 0) {
                PoisonHleResidency(chunk, page % PAGES_PER_LOCK);
                return false;
            }
            --count;
        }
        PublishHleResidencyHazard(chunk, page % PAGES_PER_LOCK);
        AdvanceBulkContractGeneration(page);
        return true;
    }

    u32 ShaderReadResidentCount(const size_t page) const noexcept {
        const PageChunk* const chunk = FindChunk(page);
        const HleResidencyCounts* const counts =
            chunk != nullptr ? chunk->hle_residency_counts.get() : nullptr;
        return counts != nullptr
                   ? counts->shader_read[page % PAGES_PER_LOCK]
                   : 0;
    }

    bool UpdateShaderReadResidentCount(const size_t page,
                                       const bool track) {
        PageChunk& chunk = EnsureChunk(page);
        if (chunk.hle_residency_counts == nullptr) {
            if (!track) {
                PoisonHleResidency(chunk, page % PAGES_PER_LOCK);
                return false;
            }
            chunk.hle_residency_counts =
                std::make_unique<HleResidencyCounts>();
        }
        u32& count = chunk.hle_residency_counts
                         ->shader_read[page % PAGES_PER_LOCK];
        if (track) {
            ASSERT_MSG(count != std::numeric_limits<u32>::max(),
                       "Shader read residency counter overflow");
            if (count == std::numeric_limits<u32>::max()) {
                PoisonHleResidency(chunk, page % PAGES_PER_LOCK);
                return false;
            }
            ++count;
        } else {
            ASSERT_MSG(count != 0,
                       "Shader read residency counter underflow");
            if (count == 0) {
                PoisonHleResidency(chunk, page % PAGES_PER_LOCK);
                return false;
            }
            --count;
        }
        PublishHleResidencyHazard(chunk, page % PAGES_PER_LOCK);
        AdvanceBulkContractGeneration(page);
        return true;
    }

    bool GpuDirty(const size_t page) const noexcept {
        const PageChunk* chunk = FindChunk(page);
        if (chunk == nullptr) {
            return false;
        }
        const size_t local_page = page % PAGES_PER_LOCK;
        return ((chunk->gpu_dirty[local_page >> 6] >> (local_page & 63)) & 1u) != 0;
    }

    void SetGpuDirty(const size_t page, const bool dirty) noexcept {
        PageChunk* chunk = dirty ? &EnsureChunk(page) : FindChunk(page);
        if (chunk == nullptr) {
            return;
        }
        const size_t local_page = page % PAGES_PER_LOCK;
        const u64 mask = u64{1} << (local_page & 63);
        if (dirty) {
            chunk->gpu_dirty[local_page >> 6] |= mask;
        } else {
            chunk->gpu_dirty[local_page >> 6] &= ~mask;
        }
    }

    Core::MemoryPermission EffectivePerms(
        const size_t page, const Core::MemoryPermission watcher_perms) const noexcept {
        if (!GpuMapped(page)) {
            return Core::MemoryPermission::None;
        }
        const Core::MemoryPermission base = BaseHostPerms(page);
        Core::MemoryPermission effective =
            base & Core::MemoryPermission::Execute;
        if (True(base & Core::MemoryPermission::Read) &&
            True(watcher_perms & Core::MemoryPermission::Read)) {
            effective |= Core::MemoryPermission::Read;
        }
        if (!FexBufferCaptureBlocked(page) &&
            True(base & Core::MemoryPermission::Write) &&
            (True(watcher_perms & Core::MemoryPermission::Write) ||
             WriteFaultBypassed(page) || FexDeferredWriteBypassed(page))) {
            effective |= Core::MemoryPermission::Write;
        }
        return effective;
    }

    bool ActualReadProtected(const size_t page) const noexcept {
        const PageChunk* chunk = FindChunk(page);
        if (chunk == nullptr) {
            return false;
        }
        const size_t local_page = page % PAGES_PER_LOCK;
        const u64 word = chunk->actual_read_protection[local_page >> 6];
        return ((word >> (local_page & 63)) & 1u) != 0;
    }

    bool ActualWriteProtected(const size_t page) const noexcept {
        const PageChunk* chunk = FindChunk(page);
        if (chunk == nullptr) {
            return false;
        }
        const size_t local_page = page % PAGES_PER_LOCK;
        const u64 word = chunk->actual_write_protection[local_page >> 6].load(
            std::memory_order_acquire);
        return ((word >> (local_page & 63)) & 1u) != 0;
    }

    bool WriteFaultBypassed(const size_t page) const noexcept {
        const PageChunk* chunk = FindChunk(page);
        if (chunk == nullptr) {
            return false;
        }
        const size_t local_page = page % PAGES_PER_LOCK;
        const u64 word = chunk->write_fault_bypass[local_page >> 6].load(
            std::memory_order_acquire);
        return ((word >> (local_page & 63)) & 1u) != 0;
    }

    bool FexDeferredWriteBypassed(const size_t page) const noexcept {
        const PageChunk* chunk = FindChunk(page);
        if (chunk == nullptr) {
            return false;
        }
        const size_t local_page = page % PAGES_PER_LOCK;
        const u64 word = chunk->fex_deferred_write_bypass[local_page >> 6].load(
            std::memory_order_acquire);
        return ((word >> (local_page & 63)) & 1u) != 0;
    }

    bool FexBufferCaptureBlocked(const size_t page) const noexcept {
        const PageChunk* chunk = FindChunk(page);
        if (chunk == nullptr) {
            return false;
        }
        const size_t local_page = page % PAGES_PER_LOCK;
        const u64 word = chunk->fex_buffer_capture_blocked[local_page >> 6].load(
            std::memory_order_acquire);
        return ((word >> (local_page & 63)) & 1u) != 0;
    }

    bool DescriptorSnapshotConflicted(const size_t page) const noexcept {
        const PageChunk* chunk = FindChunk(page);
        if (chunk == nullptr) {
            return false;
        }
        const size_t local_page = page % PAGES_PER_LOCK;
        const u64 word = chunk->descriptor_snapshot_conflict[local_page >> 6];
        return ((word >> (local_page & 63)) & 1u) != 0;
    }

    void SetDescriptorSnapshotConflict(const size_t page,
                                       const bool enabled) noexcept {
        PageChunk* chunk = enabled ? &EnsureChunk(page) : FindChunk(page);
        if (chunk == nullptr) {
            return;
        }
        const size_t local_page = page % PAGES_PER_LOCK;
        const u64 mask = u64{1} << (local_page & 63);
        if (enabled) {
            chunk->descriptor_snapshot_conflict[local_page >> 6] |= mask;
        } else {
            chunk->descriptor_snapshot_conflict[local_page >> 6] &= ~mask;
        }
    }

    bool BulkWriteActive(const size_t page) const noexcept {
        const PageState* state = FindState(page);
        return state != nullptr &&
               (state->num_bulk_writers != 0 || state->bulk_writer_overflow != 0);
    }

    u32 BulkContractGeneration(const size_t page) const noexcept {
        const PageChunk* chunk = FindChunk(page);
        if (chunk == nullptr) {
            return 0;
        }
        return chunk->bulk_contract_generation[page % PAGES_PER_LOCK];
    }

    void AdvanceBulkContractGeneration(const size_t page) noexcept {
        PageChunk& chunk = EnsureChunk(page);
        ++chunk.bulk_contract_generation[page % PAGES_PER_LOCK];
    }

    u64 BulkPrepareReservation(const size_t page) const noexcept {
        const PageChunk* chunk = FindChunk(page);
        if (chunk == nullptr) {
            return 0;
        }
        return chunk->bulk_prepare_reservation[page % PAGES_PER_LOCK];
    }

    void SetBulkPrepareReservation(const size_t page,
                                   const u64 token) noexcept {
        PageChunk& chunk = EnsureChunk(page);
        chunk.bulk_prepare_reservation[page % PAGES_PER_LOCK] = token;
    }

    u8 UpdateBulkWriterCount(const size_t page, const bool enabled,
                             const char* const owner) noexcept {
        PageState& state = State(page);
        if (enabled) {
            if (state.bulk_writer_overflow != 0) {
                return 0xff;
            }
            if (state.num_bulk_writers == 8191) {
                state.bulk_writer_overflow = 1;
                bulk_writer_overflows.fetch_add(1, std::memory_order_relaxed);
                return 0xff;
            }
            AdvanceBulkContractGeneration(page);
            return static_cast<u8>(std::min<u32>(++state.num_bulk_writers, 0xff));
        } else {
            if (state.bulk_writer_overflow != 0) {
                // Exact cardinality is unknowable after saturation.  Keep the
                // page permanently snapshot-busy rather than clearing early.
                return 0xff;
            }
            if (state.num_bulk_writers == 0) {
                bulk_writer_underflows.fetch_add(1, std::memory_order_relaxed);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
                __android_log_print(
                    ANDROID_LOG_ERROR, "LSX4Native",
                    "[EXECUTOR_BULK_WRITER_UNDERFLOW] owner=%s page=0x%zx "
                    "phase=%u leaseAddress=0x%llx leaseSize=0x%llx "
                    "trackedGate=%u trackedActivity=%u unwatched=%u "
                    "deferredTicket=%u admitted=%u",
                    owner, page,
                    static_cast<unsigned>(CurrentFexBulkLeasePhase()),
                    static_cast<unsigned long long>(
                        executor_fex_bulk_lease_address),
                    static_cast<unsigned long long>(executor_fex_bulk_lease_size),
                    executor_tracked_bulk_write_gate_active ? 1u : 0u,
                    executor_tracked_bulk_write_activity_entered ? 1u : 0u,
                    executor_unwatched_bulk_write_active ? 1u : 0u,
                    executor_fex_deferred_bulk_write_ticket.active ? 1u : 0u,
                    executor_fex_bulk_writer_admitted ? 1u : 0u);
#else
                (void)owner;
#endif
                ASSERT_MSG(false, "Bulk writer count underflow");
                return 0;
            }
            return static_cast<u8>(std::min<u32>(--state.num_bulk_writers, 0xff));
        }
    }

    static void BackoffBulkWriterWait(u32& waits) {
        if (++waits < 128) {
#if defined(__aarch64__)
            asm volatile("yield" ::: "memory");
#else
            std::this_thread::yield();
#endif
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    }

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    struct UnwatchedBulkWriteCutoff {
        explicit UnwatchedBulkWriteCutoff(Impl& owner_) : owner{owner_}, lock{
            owner.unwatched_bulk_write_gate_mutex} {
            const u64 previous = owner.unwatched_bulk_write_gate.fetch_add(
                1, std::memory_order_acq_rel);
            ASSERT_MSG((previous & 1u) == 0,
                       "Nested unwatched bulk-write cutoff");
            u32 waits = 0;
            while (owner.active_unwatched_bulk_writers.load(
                       std::memory_order_acquire) != 0 ||
                   owner.tracked_bulk_writers.load(
                       std::memory_order_acquire) != 0) {
                BackoffBulkWriterWait(waits);
            }
        }

        ~UnwatchedBulkWriteCutoff() {
            const u64 previous = owner.unwatched_bulk_write_gate.fetch_add(
                1, std::memory_order_release);
            ASSERT_MSG((previous & 1u) != 0,
                       "Unwatched bulk-write cutoff was not active");
        }

        Impl& owner;
        std::unique_lock<std::mutex> lock;
    };

    void CompleteInitialGpuMappingRegistration() noexcept {
        if (initial_gpu_mapping_registration_complete.load(
                std::memory_order_acquire)) {
            return;
        }
        UnwatchedBulkWriteCutoff cutoff{*this};
        initial_gpu_mapping_registration_complete.store(
            true, std::memory_order_release);
    }

    bool InitialGpuMappingRegistrationComplete() const noexcept {
        return initial_gpu_mapping_registration_complete.load(
            std::memory_order_acquire);
    }

    void EnterTrackedBulkWriteGate(const VAddr address,
                                   const u64 size) noexcept {
        ASSERT_MSG(!executor_tracked_bulk_write_gate_active,
                   "Nested tracked bulk write");
        u32 waits = 0;
        for (;;) {
            const u64 gate =
                unwatched_bulk_write_gate.load(std::memory_order_acquire);
            if ((gate & 1u) != 0) {
                BackoffBulkWriterWait(waits);
                continue;
            }
            tracked_bulk_writers.fetch_add(1, std::memory_order_acq_rel);
            if (unwatched_bulk_write_gate.load(std::memory_order_acquire) ==
                gate) {
                break;
            }
            const u32 previous =
                tracked_bulk_writers.fetch_sub(1, std::memory_order_release);
            ASSERT_MSG(previous != 0, "Tracked bulk writer underflow");
            BackoffBulkWriterWait(waits);
        }
        executor_tracked_bulk_write_gate_active = true;
        executor_tracked_bulk_write_activity_entered = false;
        executor_tracked_bulk_write_address = address;
        executor_tracked_bulk_write_size = size;
        while (active_unwatched_bulk_writers.load(
                   std::memory_order_acquire) != 0) {
            BackoffBulkWriterWait(waits);
        }
    }

    void LeaveTrackedBulkWriteGate() noexcept {
        if (!executor_tracked_bulk_write_gate_active) {
            return;
        }
        executor_tracked_bulk_write_gate_active = false;
        executor_tracked_bulk_write_activity_entered = false;
        executor_tracked_bulk_write_address = 0;
        executor_tracked_bulk_write_size = 0;
        const u32 previous =
            tracked_bulk_writers.fetch_sub(1, std::memory_order_release);
        ASSERT_MSG(previous != 0, "Tracked bulk writer underflow");
    }

    bool TryEnterUnwatchedBulkGuestWrite(const VAddr address,
                                         const u64 size) noexcept {
        if (ExecutorHleCleanBufferFastPathEnabled() &&
            hle_clean_buffer_fail_closed.load(std::memory_order_acquire)) {
            return false;
        }
        const bool registration_complete =
            initial_gpu_mapping_registration_complete.load(
                std::memory_order_acquire);
        const bool steady_state_unwatched_disabled =
            registration_complete &&
            (ExecutorFexBufferCaptureAckEnabled() ||
             !ExecutorUnwatchedBulkGuestWriteFastPathEnabled());
        if (steady_state_unwatched_disabled || size == 0 ||
            address >= (u64{1} << ADDRESS_BITS) ||
            size > (u64{1} << ADDRESS_BITS) - address ||
            executor_unwatched_bulk_write_active ||
            executor_tracked_bulk_write_gate_active) {
            return false;
        }
        const u64 gate =
            unwatched_bulk_write_gate.load(std::memory_order_acquire);
        if ((gate & 1u) != 0) {
            if (ExecutorReadbackContractTraceEnabled()) {
                executor_unwatched_bulk_write_gate_fallbacks.fetch_add(
                    1, std::memory_order_relaxed);
            }
            return false;
        }
        active_unwatched_bulk_writers.fetch_add(1,
                                                std::memory_order_acq_rel);
        const auto release_admission = [&] {
            const u32 previous = active_unwatched_bulk_writers.fetch_sub(
                1, std::memory_order_release);
            ASSERT_MSG(previous != 0, "Unwatched bulk writer underflow");
        };
        const bool completed_during_admission =
            initial_gpu_mapping_registration_complete.load(
                std::memory_order_acquire);
        if (unwatched_bulk_write_gate.load(std::memory_order_acquire) != gate ||
            (completed_during_admission &&
             (ExecutorFexBufferCaptureAckEnabled() ||
              !ExecutorUnwatchedBulkGuestWriteFastPathEnabled())) ||
            tracked_bulk_writers.load(std::memory_order_acquire) != 0) {
            release_admission();
            if (ExecutorReadbackContractTraceEnabled()) {
                executor_unwatched_bulk_write_gate_fallbacks.fetch_add(
                    1, std::memory_order_relaxed);
            }
            return false;
        }

        const size_t first_page = address >> PAGE_BITS;
        const size_t page_end = Common::DivCeil(address + size, PAGE_SIZE);
        const bool check_hle_residency =
            ExecutorHleCleanBufferFastPathEnabled();
        bool hazard = false;
        for (size_t page = first_page; page < page_end; ++page) {
            if (WriteWatcherHazard(page) || ActualWriteProtected(page) ||
                (check_hle_residency && HleResidencyHazard(page)) ||
                WriteFaultBypassed(page) ||
                FexDeferredWriteBypassed(page)) {
                hazard = true;
                break;
            }
        }
        if (hazard ||
            tracked_bulk_writers.load(std::memory_order_acquire) != 0 ||
            unwatched_bulk_write_gate.load(std::memory_order_acquire) != gate) {
            release_admission();
            if (ExecutorReadbackContractTraceEnabled()) {
                (hazard ? executor_unwatched_bulk_write_hazard_fallbacks
                        : executor_unwatched_bulk_write_gate_fallbacks)
                    .fetch_add(1, std::memory_order_relaxed);
            }
            return false;
        }

        executor_unwatched_bulk_write_active = true;
        executor_unwatched_bulk_write_address = address;
        executor_unwatched_bulk_write_size = size;
        if (ExecutorReadbackContractTraceEnabled()) {
            const u64 ordinal =
                executor_unwatched_bulk_write_fast_calls.fetch_add(
                    1, std::memory_order_relaxed) +
                1;
            const u64 bytes =
                executor_unwatched_bulk_write_fast_bytes.fetch_add(
                    size, std::memory_order_relaxed) +
                size;
            if (ExecutorShouldLogReadbackOrdinal(ordinal)) {
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_UNWATCHED_BULK_WRITE] calls=%llu bytes=%llu "
                    "hazardFallbacks=%llu gateFallbacks=%llu",
                    static_cast<unsigned long long>(ordinal),
                    static_cast<unsigned long long>(bytes),
                    static_cast<unsigned long long>(
                        executor_unwatched_bulk_write_hazard_fallbacks.load(
                            std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        executor_unwatched_bulk_write_gate_fallbacks.load(
                            std::memory_order_relaxed)));
            }
        }
        return true;
    }

    bool LeaveUnwatchedBulkGuestWrite(const VAddr address,
                                      const u64 size) noexcept {
        if (!executor_unwatched_bulk_write_active) {
            return false;
        }
        ASSERT_MSG(executor_unwatched_bulk_write_address == address &&
                       executor_unwatched_bulk_write_size == size,
                   "Mismatched unwatched bulk write leave");
        executor_unwatched_bulk_write_active = false;
        executor_unwatched_bulk_write_address = 0;
        executor_unwatched_bulk_write_size = 0;
        const u32 previous = active_unwatched_bulk_writers.fetch_sub(
            1, std::memory_order_release);
        ASSERT_MSG(previous != 0, "Unwatched bulk writer underflow");
        return true;
    }
#endif

    template <typename Callback>
    void WithQuiescedBulkWriters(const size_t first_page,
                                 const size_t page_end,
                                 Callback&& callback) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        UnwatchedBulkWriteCutoff unwatched_cutoff{*this};
#endif
        const auto lock_start = locks.begin() + (first_page / PAGES_PER_LOCK);
        const auto lock_end =
            locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        u32 waits = 0;
        for (;;) {
            {
                Common::RangeLockGuard lk(lock_start, lock_end);
                bool busy = false;
                for (size_t page = first_page; page < page_end; ++page) {
                    if (BulkWriteActive(page)) {
                        busy = true;
                        break;
                    }
                }
                if (!busy) {
                    callback();
                    return;
                }
            }
            BackoffBulkWriterWait(waits);
        }
    }

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    void ReleaseBulkPrepareReservation(const VAddr address, const u64 size,
                                       const u64 token) noexcept {
        if (token == 0 || size == 0 || address >= (u64{1} << ADDRESS_BITS)) {
            return;
        }
        const u64 bounded_size =
            std::min(size, (u64{1} << ADDRESS_BITS) - address);
        const size_t first_page = address >> PAGE_BITS;
        const size_t page_end =
            Common::DivCeil(address + bounded_size, PAGE_SIZE);
        const auto lock_start = locks.begin() + (first_page / PAGES_PER_LOCK);
        const auto lock_end =
            locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        Common::RangeLockGuard lk(lock_start, lock_end);
        for (size_t page = first_page; page < page_end; ++page) {
            if (BulkPrepareReservation(page) == token) {
                SetBulkPrepareReservation(page, 0);
            }
        }
    }

    bool ValidateBulkPrepareReservation(const VAddr address, const u64 size,
                                        const u64 token) noexcept {
        if (token == 0 || size == 0 || address >= (u64{1} << ADDRESS_BITS) ||
            size > (u64{1} << ADDRESS_BITS) - address) {
            return false;
        }
        const size_t first_page = address >> PAGE_BITS;
        const size_t page_end =
            Common::DivCeil(address + size, PAGE_SIZE);
        const auto lock_start = locks.begin() + (first_page / PAGES_PER_LOCK);
        const auto lock_end =
            locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        Common::RangeLockGuard lk(lock_start, lock_end);
        for (size_t page = first_page; page < page_end; ++page) {
            if (BulkPrepareReservation(page) != token) {
                return false;
            }
        }
        return true;
    }

    static bool ValidateBulkPrepareReservationCallback(
        void* context, const VAddr address, const u64 size,
        const u64 token) noexcept {
        return static_cast<Impl*>(context)->ValidateBulkPrepareReservation(
            address, size, token);
    }

    bool ReserveBulkGuestWrite(const VAddr address, const u64 size,
                               const u64 token,
                               boost::container::small_vector<u32, 16>&
                                   generations) {
        const u64 bounded_size =
            std::min(size, (u64{1} << ADDRESS_BITS) - address);
        const size_t first_page = address >> PAGE_BITS;
        const size_t page_end =
            Common::DivCeil(address + bounded_size, PAGE_SIZE);
        const auto lock_start = locks.begin() + (first_page / PAGES_PER_LOCK);
        const auto lock_end =
            locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        ASSERT_MSG(generations.size() == page_end - first_page,
                   "Bulk reservation generation range mismatch");
        const bool gpu_thread = IsGpuThread();
        u32 waits = 0;
        for (;;) {
            {
                Common::RangeLockGuard lk(lock_start, lock_end);
                bool busy = false;
                for (size_t page = first_page; page < page_end; ++page) {
                    const u64 owner = BulkPrepareReservation(page);
                    if (BulkWriteActive(page) ||
                        (!gpu_thread && owner != 0 && owner != token)) {
                        busy = true;
                        break;
                    }
                }
                if (!busy) {
                    bool protected_page = false;
                    for (size_t page = first_page; page < page_end; ++page) {
                        const u64 previous = BulkPrepareReservation(page);
                        if (gpu_thread && previous != 0 && previous != token) {
                            SetBulkPrepareReservation(page, 0);
                            AdvanceBulkContractGeneration(page);
                        }
                        CloseFexDeferredWriteBypass(page);
                        protected_page = protected_page ||
                                         ActualWriteProtected(page);
                        SetBulkPrepareReservation(page, token);
                    }
                    for (size_t page = first_page; page < page_end; ++page) {
                        generations[page - first_page] =
                            BulkContractGeneration(page);
                    }
                    return protected_page;
                }
            }
            BackoffBulkWriterWait(waits);
        }
    }
#endif

    void SetWriteFaultBypass(const size_t page, const bool enabled) noexcept {
        PageChunk* chunk = enabled ? &EnsureChunk(page) : FindChunk(page);
        if (chunk == nullptr) {
            return;
        }
        const size_t local_page = page % PAGES_PER_LOCK;
        const u64 mask = u64{1} << (local_page & 63);
        auto& word = chunk->write_fault_bypass[local_page >> 6];
        const bool was_enabled =
            (word.load(std::memory_order_relaxed) & mask) != 0;
        if (enabled) {
            word.fetch_or(mask, std::memory_order_release);
        } else {
            word.fetch_and(~mask, std::memory_order_release);
        }
        if (was_enabled != enabled) {
            AdvanceFaultGeneration(page, true);
        }
    }

    void SetFexDeferredWriteBypass(const size_t page, const bool enabled) noexcept {
        PageChunk* chunk = enabled ? &EnsureChunk(page) : FindChunk(page);
        if (chunk == nullptr) {
            return;
        }
        const size_t local_page = page % PAGES_PER_LOCK;
        const u64 mask = u64{1} << (local_page & 63);
        auto& word = chunk->fex_deferred_write_bypass[local_page >> 6];
        const bool was_enabled =
            (word.load(std::memory_order_relaxed) & mask) != 0;
        if (enabled) {
            word.fetch_or(mask, std::memory_order_release);
        } else {
            word.fetch_and(~mask, std::memory_order_release);
        }
        if (was_enabled != enabled) {
            AdvanceFaultGeneration(page, true);
        }
    }

    void SetFexBufferCaptureBlocked(const size_t page,
                                    const bool enabled) noexcept {
        PageChunk* chunk = enabled ? &EnsureChunk(page) : FindChunk(page);
        if (chunk == nullptr) {
            return;
        }
        const size_t local_page = page % PAGES_PER_LOCK;
        const u64 mask = u64{1} << (local_page & 63);
        auto& word = chunk->fex_buffer_capture_blocked[local_page >> 6];
        if (enabled) {
            word.fetch_or(mask, std::memory_order_release);
        } else {
            word.fetch_and(~mask, std::memory_order_release);
        }
    }

    bool WaitForFexBufferCapture(const VAddr address) const noexcept {
        if (!ExecutorFexBufferCaptureAckEnabled() ||
            address >= (u64{1} << ADDRESS_BITS)) {
            return false;
        }
        const size_t page = address >> PAGE_BITS;
        if (!FexBufferCaptureBlocked(page)) {
            return false;
        }
        FexWriteEpochs().buffer_capture_waits.fetch_add(
            1, std::memory_order_relaxed);
        while (FexBufferCaptureBlocked(page)) {
#if defined(__aarch64__)
            asm volatile("yield" ::: "memory");
#else
            std::this_thread::yield();
#endif
        }
        return true;
    }

    bool CloseFexDeferredWriteBypass(const size_t page) {
        if (!FexDeferredWriteBypassed(page)) {
            return false;
        }
        ASSERT_MSG(!BulkWriteActive(page),
                   "FEX deferred bypass closed under an active bulk writer");
        SetFexDeferredWriteBypass(page, false);
        Protect(static_cast<VAddr>(page) << PAGE_BITS, PAGE_SIZE,
                State(page).WatcherPerms(), true);
        return true;
    }

    bool FexDeferredBulkPageContractValid(const size_t page) {
        const bool deferred_open = FexDeferredWriteBypassed(page);
        const PageState* const state = FindState(page);
        if (state == nullptr || !GpuMapped(page) ||
            !True(BaseHostPerms(page) & Core::MemoryPermission::Write) ||
            BulkPrepareReservation(page) != 0 ||
            WriteFaultBypassed(page) || state->num_write_watchers == 0 ||
            state->num_read_watchers != 0 || WriteWatcherSourceMask(page) != 1 ||
            state->memory_wait_write_watcher != 0 ||
            state->jit_poll_write_watcher != 0 ||
            state->descriptor_snapshot_write_watcher != 0 || GpuDirty(page) ||
            (!deferred_open && BulkWriteActive(page)) ||
            (!deferred_open && !ActualWriteProtected(page))) {
            return false;
        }
        return true;
    }

    template <auto Member>
    void FillProtection(const VAddr address, const u64 size,
                        const bool protected_state) noexcept {
        if (size == 0 || address >= (u64{1} << ADDRESS_BITS)) {
            return;
        }
        const u64 bounded_size =
            std::min(size, (u64{1} << ADDRESS_BITS) - address);
        size_t page = address >> PAGE_BITS;
        const size_t end_page =
            Common::DivCeil(address + bounded_size, PAGE_SIZE);
        while (page < end_page) {
            const size_t chunk_end =
                std::min(end_page, Common::AlignUp(page + 1, PAGES_PER_LOCK));
            PageChunk* chunk =
                protected_state ? &EnsureChunk(page) : FindChunk(page);
            if (chunk == nullptr) {
                page = chunk_end;
                continue;
            }
            auto& bits = chunk->*Member;
            const size_t local_begin = page % PAGES_PER_LOCK;
            const size_t local_end = local_begin + (chunk_end - page);
            const size_t first_word = local_begin >> 6;
            const size_t last_word = (local_end - 1) >> 6;
            const u64 first_mask = ~u64{0} << (local_begin & 63);
            const u64 last_mask =
                (local_end & 63) == 0
                    ? ~u64{0}
                    : (u64{1} << (local_end & 63)) - 1;
            if (first_word == last_word) {
                const u64 mask = first_mask & last_mask;
                if (protected_state) {
                    bits[first_word] |= mask;
                } else {
                    bits[first_word] &= ~mask;
                }
            } else if (protected_state) {
                bits[first_word] |= first_mask;
                std::fill(bits.begin() + first_word + 1,
                          bits.begin() + last_word, ~u64{0});
                bits[last_word] |= last_mask;
            } else {
                bits[first_word] &= ~first_mask;
                std::fill(bits.begin() + first_word + 1,
                          bits.begin() + last_word, u64{0});
                bits[last_word] &= ~last_mask;
            }
            page = chunk_end;
        }
    }

    void FillActualReadProtection(const VAddr address, const u64 size,
                                  const bool protected_state) noexcept {
        FillProtection<&PageChunk::actual_read_protection>(
            address, size, protected_state);
    }

    void FillActualWriteProtection(const VAddr address, const u64 size,
                                   const bool protected_state) noexcept {
        if (size == 0 || address >= (u64{1} << ADDRESS_BITS)) {
            return;
        }
        const u64 bounded_size =
            std::min(size, (u64{1} << ADDRESS_BITS) - address);
        size_t page = address >> PAGE_BITS;
        const size_t end_page =
            Common::DivCeil(address + bounded_size, PAGE_SIZE);
        while (page < end_page) {
            const size_t chunk_end =
                std::min(end_page, Common::AlignUp(page + 1, PAGES_PER_LOCK));
            PageChunk* chunk =
                protected_state ? &EnsureChunk(page) : FindChunk(page);
            if (chunk == nullptr) {
                page = chunk_end;
                continue;
            }
            auto& bits = chunk->actual_write_protection;
            const size_t local_begin = page % PAGES_PER_LOCK;
            const size_t local_end = local_begin + (chunk_end - page);
            const size_t first_word = local_begin >> 6;
            const size_t last_word = (local_end - 1) >> 6;
            const u64 first_mask = ~u64{0} << (local_begin & 63);
            const u64 last_mask =
                (local_end & 63) == 0
                    ? ~u64{0}
                    : (u64{1} << (local_end & 63)) - 1;
            const auto update_word = [&](const size_t word, const u64 mask) {
                if (protected_state) {
                    bits[word].fetch_or(mask, std::memory_order_release);
                } else {
                    bits[word].fetch_and(~mask, std::memory_order_release);
                }
            };
            if (first_word == last_word) {
                update_word(first_word, first_mask & last_mask);
            } else {
                update_word(first_word, first_mask);
                for (size_t word = first_word + 1; word < last_word; ++word) {
                    bits[word].store(protected_state ? ~u64{0} : u64{0},
                                     std::memory_order_release);
                }
                update_word(last_word, last_mask);
            }
            page = chunk_end;
        }
    }

    void FillMappedProtection(const VAddr address, const u64 size,
                              const Core::MemoryPermission host_permissions,
                              const bool guest_cpu_read_allowed,
                              const bool guest_cpu_write_allowed) noexcept {
        FillProtection<&PageChunk::gpu_mapped>(address, size, true);
        FillProtection<&PageChunk::base_host_read>(
            address, size,
            True(host_permissions & Core::MemoryPermission::Read));
        FillProtection<&PageChunk::base_host_write>(
            address, size,
            True(host_permissions & Core::MemoryPermission::Write));
        FillProtection<&PageChunk::base_host_execute>(
            address, size,
            True(host_permissions & Core::MemoryPermission::Execute));
        FillProtection<&PageChunk::guest_cpu_read>(
            address, size, guest_cpu_read_allowed);
        FillProtection<&PageChunk::guest_cpu_write>(
            address, size, guest_cpu_write_allowed);
    }

    void ClearMappedProtection(const VAddr address, const u64 size) noexcept {
        FillProtection<&PageChunk::gpu_mapped>(address, size, false);
        FillProtection<&PageChunk::base_host_read>(address, size, false);
        FillProtection<&PageChunk::base_host_write>(address, size, false);
        FillProtection<&PageChunk::base_host_execute>(address, size, false);
        FillProtection<&PageChunk::guest_cpu_read>(address, size, false);
        FillProtection<&PageChunk::guest_cpu_write>(address, size, false);
    }

    template <auto Member>
    void SetPageBit(const size_t page, const bool enabled) noexcept {
        PageChunk* chunk = enabled ? &EnsureChunk(page) : FindChunk(page);
        if (chunk == nullptr) {
            return;
        }
        const size_t local_page = page % PAGES_PER_LOCK;
        const u64 mask = u64{1} << (local_page & 63);
        auto& word = (chunk->*Member)[local_page >> 6];
        if (enabled) {
            word |= mask;
        } else {
            word &= ~mask;
        }
    }

    void SetPageBaseProtection(
        const size_t page, const Core::MemoryPermission host_permissions,
        const bool guest_cpu_read_allowed,
        const bool guest_cpu_write_allowed) noexcept {
        SetPageBit<&PageChunk::base_host_read>(
            page, True(host_permissions & Core::MemoryPermission::Read));
        SetPageBit<&PageChunk::base_host_write>(
            page, True(host_permissions & Core::MemoryPermission::Write));
        SetPageBit<&PageChunk::base_host_execute>(
            page, True(host_permissions & Core::MemoryPermission::Execute));
        SetPageBit<&PageChunk::guest_cpu_read>(page,
                                               guest_cpu_read_allowed);
        SetPageBit<&PageChunk::guest_cpu_write>(page,
                                                guest_cpu_write_allowed);
        AdvanceFaultGeneration(page, false);
        AdvanceFaultGeneration(page, true);
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms,
                 const bool force_host_transition = false) {
        RENDERER_TRACE;
        auto* memory = Core::Memory::Instance();
        auto& impl = memory->GetAddressSpace();
        const auto apply = [&](const VAddr begin, const u64 bytes,
                               const Core::MemoryPermission actual_perms) {
            // Fault and bulk-write recovery often converge on a permission
            // state that is already installed.  Repeating mprotect there
            // performs a process-wide TLB shootdown and can stall every guest
            // thread.  Actual*Protected is authoritative for those internal
            // transitions.  Watcher 0<->1 changes deliberately bypass this
            // cache: an external guest protection update may have changed the
            // host PTE without passing through PageManager.
            bool permission_change = force_host_transition;
            if (!permission_change &&
                (begin & (PAGE_SIZE - 1)) == 0 &&
                (bytes & (PAGE_SIZE - 1)) == 0 &&
                begin < (u64{1} << ADDRESS_BITS) &&
                bytes <= (u64{1} << ADDRESS_BITS) - begin) {
                const bool desired_read_protected =
                    !True(actual_perms & Core::MemoryPermission::Read);
                const bool desired_write_protected =
                    !True(actual_perms & Core::MemoryPermission::Write);
                const size_t first_page = begin >> PAGE_BITS;
                const size_t page_end = first_page + (bytes >> PAGE_BITS);
                for (size_t page = first_page; page < page_end; ++page) {
                    if (ActualReadProtected(page) != desired_read_protected ||
                        ActualWriteProtected(page) != desired_write_protected) {
                        permission_change = true;
                        break;
                    }
                }
            } else if (!permission_change) {
                permission_change = true;
            }
            if (!permission_change) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
                executor_readback_protect_noop_count.fetch_add(
                    1, std::memory_order_relaxed);
#endif
                return;
            }
            impl.Protect(begin, bytes, actual_perms);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            executor_readback_protect_os_count.fetch_add(
                1, std::memory_order_relaxed);
#endif
            FillActualReadProtection(
                begin, bytes, !True(actual_perms & Core::MemoryPermission::Read));
            FillActualWriteProtection(
                begin, bytes, !True(actual_perms & Core::MemoryPermission::Write));
        };
        if (size != 0) {
            ASSERT_MSG((address & (PAGE_SIZE - 1)) == 0 &&
                           (size & (PAGE_SIZE - 1)) == 0,
                       "Page protection range must be page aligned");
            size_t page = address >> PAGE_BITS;
            const size_t page_end = page + (size >> PAGE_BITS);
            while (page < page_end) {
                while (page < page_end && !GpuMapped(page)) {
                    ++page;
                }
                if (page == page_end) {
                    break;
                }
                const Core::MemoryPermission actual_perms =
                    EffectivePerms(page, perms);
                const size_t first = page++;
                while (page < page_end && GpuMapped(page) &&
                       EffectivePerms(page, perms) == actual_perms) {
                    ++page;
                }
                apply(static_cast<VAddr>(first) << PAGE_BITS,
                      (page - first) << PAGE_BITS, actual_perms);
            }
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (ExecutorReadbackContractTraceEnabled()) {
            const u64 ordinal =
                executor_readback_protect_count.fetch_add(1, std::memory_order_relaxed) + 1;
            if (ExecutorShouldLogReadbackOrdinal(ordinal)) {
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_READBACK_PROTECT] n=%llu addr=0x%llx bytes=%zu read=%u write=%u "
                    "execute=%u mode=%d osCalls=%llu noops=%llu",
                    static_cast<unsigned long long>(ordinal),
                    static_cast<unsigned long long>(address), size,
                    True(perms & Core::MemoryPermission::Read) ? 1u : 0u,
                    True(perms & Core::MemoryPermission::Write) ? 1u : 0u,
                    True(perms & Core::MemoryPermission::Execute) ? 1u : 0u,
                    Config::getReadbacksMode(),
                    static_cast<unsigned long long>(
                        executor_readback_protect_os_count.load(
                            std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        executor_readback_protect_noop_count.load(
                            std::memory_order_relaxed)));
            }
        }
#endif
    }

    void ReconcileProtectionRange(
        const VAddr address, const u64 size,
        const Core::MemoryPermission fallback_permissions,
        const bool force_host_transition) {
        size_t page = address >> PAGE_BITS;
        const size_t page_end = page + (size >> PAGE_BITS);
        while (page < page_end) {
            const Core::MemoryPermission requested =
                GpuMapped(page) ? State(page).WatcherPerms()
                                : fallback_permissions;
            const size_t first = page++;
            while (page < page_end) {
                const Core::MemoryPermission next =
                    GpuMapped(page) ? State(page).WatcherPerms()
                                    : fallback_permissions;
                if (next != requested) {
                    break;
                }
                ++page;
            }
            Protect(static_cast<VAddr>(first) << PAGE_BITS,
                    (page - first) << PAGE_BITS, requested,
                    force_host_transition);
        }
    }

    template <bool track, bool is_read>
    void ApplyWatcherProtection(const VAddr address, const u64 size,
                                const Core::MemoryPermission perms) {
        // Mapped guest protection changes are reconciled through
        // OnGuestProtectionChanged, so Actual*Protection is authoritative here.
        // Publish every logical watcher transition synchronously through
        // Protect.  Protect performs the host PTE transition whenever the
        // authoritative sidecar differs, but must not reinstall an identical
        // PTE: doing so can revoke a FEX write lease which already represents
        // the same PS4-visible permission state. This is an ABI correctness
        // boundary, not merely a redundant host syscall. Keep unconditional host
        // transitions available only as an explicit same-binary SM8650 A/B.
        Protect(address, size, perms,
                ExecutorSm8650ForceWatcherPteTransitionsEnabled());
    }

    bool ReleaseDeferredWriteProtection(const VAddr address) {
        if (address >= (u64{1} << ADDRESS_BITS)) {
            return false;
        }
        const size_t page = address >> PAGE_BITS;
        std::scoped_lock lk{locks[page / PAGES_PER_LOCK]};
        PageState& state = State(page);
        if (!True(state.WritePerm() & Core::MemoryPermission::Write)) {
            return false;
        }
        Protect(static_cast<VAddr>(page) << PAGE_BITS, PAGE_SIZE,
                state.WatcherPerms());
        return true;
    }

    bool CommitWriteFault(const VAddr address, const bool persistent) {
        if (address >= (u64{1} << ADDRESS_BITS)) {
            return false;
        }
        (void)persistent;
        const size_t page = address >> PAGE_BITS;
        std::scoped_lock lk{locks[page / PAGES_PER_LOCK]};
        if (!ActualWriteProtected(page)) {
            return true;
        }
        const PageState* state = FindState(page);
        if (state == nullptr) {
            return false;
        }
        const Core::MemoryPermission perms = state->WatcherPerms();
        if (!True(perms & Core::MemoryPermission::Write)) {
            return false;
        }
        Protect(static_cast<VAddr>(page) << PAGE_BITS, PAGE_SIZE, perms);
        return true;
    }

    bool TryOpenFexDeferredWrite(const VAddr address) {
        if (address >= (u64{1} << ADDRESS_BITS)) {
            return false;
        }
        const size_t page = address >> PAGE_BITS;
        std::scoped_lock lk{locks[page / PAGES_PER_LOCK]};
        if (!GpuMapped(page)) {
            return false;
        }
        PageState& state = State(page);
        if (state.num_write_watchers == 0 || state.num_read_watchers != 0 ||
            WriteWatcherSourceMask(page) != 1 ||
            state.memory_wait_write_watcher != 0 ||
            state.jit_poll_write_watcher != 0 ||
            state.descriptor_snapshot_write_watcher != 0 ||
            BulkPrepareReservation(page) != 0 ||
            BulkWriteActive(page) || GpuDirty(page) ||
            (!ActualWriteProtected(page) && !FexDeferredWriteBypassed(page))) {
            return false;
        }
        const auto* memory = Core::Memory::Instance();
        if (memory == nullptr || memory->PhysicalAliasingMayExist()) {
            return false;
        }
        SetFexDeferredWriteBypass(page, true);
        Protect(static_cast<VAddr>(page) << PAGE_BITS, PAGE_SIZE,
                state.WatcherPerms());
        return !ActualWriteProtected(page);
    }

    void ReprotectFexDeferredWrite(const VAddr address) {
        if (address >= (u64{1} << ADDRESS_BITS)) {
            return;
        }
        const size_t page = address >> PAGE_BITS;
        std::scoped_lock lk{locks[page / PAGES_PER_LOCK]};
        if (!GpuMapped(page) || !FexDeferredWriteBypassed(page)) {
            return;
        }
        SetFexDeferredWriteBypass(page, false);
        Protect(static_cast<VAddr>(page) << PAGE_BITS, PAGE_SIZE,
                State(page).WatcherPerms());
    }

    bool GuardFexBufferCapturePage(const VAddr address,
                                   const bool requires_buffer_epoch,
                                   const bool texture_cache_lock_held) {
        if (address >= (u64{1} << ADDRESS_BITS)) {
            return false;
        }
        const size_t page = address >> PAGE_BITS;
        std::scoped_lock lk{locks[page / PAGES_PER_LOCK]};
        if (!GpuMapped(page) || FexBufferCaptureBlocked(page) ||
            !True(BaseHostPerms(page) & Core::MemoryPermission::Write) ||
            !GuestCpuAccessAllowed(page, true)) {
            return false;
        }
        PageState& state = State(page);
        const u8 watcher_sources = WriteWatcherSourceMask(page);
        const u8 allowed_sources = texture_cache_lock_held ? u8{3} : u8{1};
        const bool has_buffer_watcher = (watcher_sources & u8{1}) != 0;
        if ((requires_buffer_epoch &&
             (!has_buffer_watcher ||
              (watcher_sources & ~allowed_sources) != 0)) ||
            (!requires_buffer_epoch &&
             (watcher_sources & ~allowed_sources) != 0) ||
            (requires_buffer_epoch && state.num_read_watchers != 0) ||
            state.memory_wait_write_watcher != 0 ||
            state.jit_poll_write_watcher != 0 ||
            state.descriptor_snapshot_write_watcher != 0 ||
            BulkPrepareReservation(page) != 0 || BulkWriteActive(page) ||
            GpuDirty(page)) {
            return false;
        }
        SetFexBufferCaptureBlocked(page, true);
        if (FexDeferredWriteBypassed(page)) {
            SetFexDeferredWriteBypass(page, false);
        }
        AdvanceFaultGeneration(page, true);
        const Core::MemoryPermission capture_perms = state.WatcherPerms();
        const Core::MemoryPermission effective_capture_perms =
            EffectivePerms(page, capture_perms);
        const bool desired_read_protected =
            !True(effective_capture_perms & Core::MemoryPermission::Read);
        const bool desired_write_protected =
            !True(effective_capture_perms & Core::MemoryPermission::Write);
        if (ActualReadProtected(page) != desired_read_protected ||
            ActualWriteProtected(page) != desired_write_protected) {
            Protect(static_cast<VAddr>(page) << PAGE_BITS, PAGE_SIZE,
                    capture_perms);
        }
        return ActualWriteProtected(page);
    }

    void EndFexBufferCapture(const VAddr address) noexcept {
        if (address >= (u64{1} << ADDRESS_BITS)) {
            return;
        }
        const size_t page = address >> PAGE_BITS;
        std::scoped_lock lk{locks[page / PAGES_PER_LOCK]};
        if (!FexBufferCaptureBlocked(page)) {
            return;
        }
        SetFexBufferCaptureBlocked(page, false);
        AdvanceFaultGeneration(page, true);
        if (GpuMapped(page)) {
            const Core::MemoryPermission restored_perms =
                State(page).WatcherPerms();
            const Core::MemoryPermission effective_restored_perms =
                EffectivePerms(page, restored_perms);
            const bool desired_read_protected =
                !True(effective_restored_perms & Core::MemoryPermission::Read);
            const bool desired_write_protected =
                !True(effective_restored_perms & Core::MemoryPermission::Write);
            if (ActualReadProtected(page) != desired_read_protected ||
                ActualWriteProtected(page) != desired_write_protected) {
                Protect(static_cast<VAddr>(page) << PAGE_BITS, PAGE_SIZE,
                        restored_perms);
            }
        }
    }

    bool FaultProtectionActive(const VAddr address, const bool is_write) {
        if (address >= (u64{1} << ADDRESS_BITS)) {
            return false;
        }
        const size_t page = address >> PAGE_BITS;
        std::scoped_lock lk{locks[page / PAGES_PER_LOCK]};
        return is_write ? ActualWriteProtected(page)
                        : ActualReadProtected(page);
    }

    enum class FaultOwnership : u8 {
        Unowned,
        Tracked,
        Repaired,
    };

    FaultOwnership ClassifyAndRepairFault(const VAddr address,
                                          const bool is_write,
                                          const bool is_execute,
                                          const bool guest_cpu_access,
                                          u16& generation,
                                          u8& write_watcher_source,
                                          u8& special_write_watchers) {
        write_watcher_source = 0;
        special_write_watchers = 0;
        if (address >= (u64{1} << ADDRESS_BITS)) {
            return FaultOwnership::Unowned;
        }
        const size_t page = address >> PAGE_BITS;
        std::scoped_lock lk{locks[page / PAGES_PER_LOCK]};
        if (!GpuMapped(page)) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            const u32 ordinal = executor_unowned_fault_diag_count.fetch_add(
                1, std::memory_order_relaxed);
            if (ordinal < 16) {
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_PAGE_FAULT_CLASS] result=unowned reason=not-mapped "
                    "addr=0x%llx write=%u execute=%u guest=%u",
                    static_cast<unsigned long long>(address), is_write ? 1u : 0u,
                    is_execute ? 1u : 0u, guest_cpu_access ? 1u : 0u);
            }
#endif
            return FaultOwnership::Unowned;
        }

        generation = FaultGeneration(page, is_write);

        const Core::MemoryPermission base = BaseHostPerms(page);
        const PageState& state = State(page);
        if (is_write) {
            write_watcher_source = WriteWatcherSourceMask(page);
            special_write_watchers =
                static_cast<u8>(state.memory_wait_write_watcher != 0) |
                static_cast<u8>((state.jit_poll_write_watcher != 0) << 1) |
                static_cast<u8>((state.descriptor_snapshot_write_watcher != 0) << 2);
        }
        const Core::MemoryPermission effective =
            EffectivePerms(page, state.WatcherPerms());
        const Core::MemoryPermission requested_permission =
            is_execute ? Core::MemoryPermission::Execute
                       : (is_write ? Core::MemoryPermission::Write
                                   : Core::MemoryPermission::Read);
        const bool baseline_allowed =
            is_execute
                ? True(base & Core::MemoryPermission::Execute)
                : (guest_cpu_access
                       ? (GuestCpuAccessAllowed(page, is_write) ||
                          (is_write &&
                           True(base & Core::MemoryPermission::Write)))
                       : True(base & requested_permission));
        if (!baseline_allowed) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            const u32 ordinal = executor_unowned_fault_diag_count.fetch_add(
                1, std::memory_order_relaxed);
            if (ordinal < 16) {
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_PAGE_FAULT_CLASS] result=unowned reason=baseline "
                    "addr=0x%llx write=%u execute=%u guest=%u base=%u cpuRead=%u "
                    "cpuWrite=%u watcher=%u effective=%u actualRead=%u actualWrite=%u",
                    static_cast<unsigned long long>(address), is_write ? 1u : 0u,
                    is_execute ? 1u : 0u, guest_cpu_access ? 1u : 0u,
                    static_cast<unsigned>(base),
                    GuestCpuAccessAllowed(page, false) ? 1u : 0u,
                    GuestCpuAccessAllowed(page, true) ? 1u : 0u,
                    static_cast<unsigned>(state.WatcherPerms()),
                    static_cast<unsigned>(effective),
                    ActualReadProtected(page) ? 1u : 0u,
                    ActualWriteProtected(page) ? 1u : 0u);
            }
#endif
            return FaultOwnership::Unowned;
        }

        const bool effective_allows = True(effective & requested_permission);
        if (effective_allows) {
            if (is_write &&
                (WriteFaultBypassed(page) || FexDeferredWriteBypassed(page))) {
                return FaultOwnership::Tracked;
            }
            Protect(static_cast<VAddr>(page) << PAGE_BITS, PAGE_SIZE,
                    state.WatcherPerms(), true);
            return (is_execute ||
                    (is_write ? !ActualWriteProtected(page)
                              : !ActualReadProtected(page)))
                       ? FaultOwnership::Repaired
                       : FaultOwnership::Unowned;
        }
        if (is_execute) {
            return FaultOwnership::Unowned;
        }
        // Do not publish the protection observed by this signal as current
        // state. Several guest workers can trap on the same PTE before the
        // first handler removes its watcher. A late handler would then write
        // that stale observation back after the PTE had already been opened,
        // making the first handler report false progress and chain a guest
        // SIGSEGV. Every PageManager-owned mprotect updates Actual*Protected
        // while holding this page lock, so that sidecar is the current state;
        // the signal is only evidence that this access belongs to a watcher.
        return FaultOwnership::Tracked;
    }

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    u64 SequentialGeneratedBufferWriteFaultSpan(
        const VAddr address, const u8 write_watcher_source,
        const u8 special_write_watchers) {
        constexpr size_t MaxWindowPages = 4;
        const VAddr page_address = Common::AlignDown(address, PAGE_SIZE);
        auto& sequence = executor_sequential_buffer_fault_state;

        if (write_watcher_source != 1 || special_write_watchers != 0) {
            sequence = {};
            return PAGE_SIZE;
        }

        // Do not widen an isolated fault.  One ordinary next-page fault on
        // this same guest thread must first prove the access direction.  Once
        // a window is opened, expected_page advances to its end so a linear
        // writer can continue in bounded four-page transactions.
        const bool sequential = page_address == sequence.expected_page;
        sequence.streak = sequential
                              ? static_cast<u8>(
                                    std::min<unsigned>(sequence.streak + 1, 0xffu))
                              : 0;
        sequence.expected_page = page_address + PAGE_SIZE;
        if (!sequential) {
            return PAGE_SIZE;
        }

        const auto* const memory = Core::Memory::TryInstance();
        if (memory == nullptr || memory->PhysicalAliasingMayExist()) {
            sequence = {};
            return PAGE_SIZE;
        }

        const size_t first_page = page_address >> PAGE_BITS;
        const size_t lock_end = Common::AlignUp(first_page + 1,
                                                PAGES_PER_LOCK);
        const size_t candidate_end = std::min(
            {first_page + MaxWindowPages, lock_end, NUM_ADDRESS_PAGES});
        size_t page_end = first_page;
        {
            // Stay inside one PageManager chunk/lock.  Besides making the
            // signal path bounded, this makes the complete eligibility proof
            // one atomic PageState snapshot relative to watcher changes.
            std::unique_lock<LockType> lk{
                locks[first_page / PAGES_PER_LOCK], std::try_to_lock};
            if (!lk.owns_lock()) {
                sequence.expected_page = page_address + PAGE_SIZE;
                return PAGE_SIZE;
            }
            const PageChunk* const chunk = FindChunk(first_page);
            if (chunk == nullptr) {
                sequence = {};
                return PAGE_SIZE;
            }
            for (size_t page = first_page; page < candidate_end; ++page) {
                const size_t local_page = page % PAGES_PER_LOCK;
                const PageState& state = chunk->pages[local_page];
                const bool exact_buffer_watchers =
                    chunk->buffer_write_watchers[local_page] != 0 &&
                    chunk->texture_write_watchers[local_page] == 0 &&
                    chunk->buffer_write_watchers[local_page] ==
                        state.num_write_watchers;
                if (!GpuMapped(page) || !ActualWriteProtected(page) ||
                    ActualReadProtected(page) || state.num_read_watchers != 0 ||
                    state.num_write_watchers == 0 || !exact_buffer_watchers ||
                    WriteWatcherSourceMask(page) != 1 ||
                    state.memory_wait_write_watcher != 0 ||
                    state.jit_poll_write_watcher != 0 ||
                    state.descriptor_snapshot_write_watcher != 0 ||
                    GpuDirty(page) || DescriptorSnapshotConflicted(page) ||
                    BulkPrepareReservation(page) != 0 || BulkWriteActive(page) ||
                    WriteFaultBypassed(page) || FexDeferredWriteBypassed(page) ||
                    FexBufferCaptureBlocked(page) || HleResidencyHazard(page) ||
                    !True(BaseHostPerms(page) &
                          Core::MemoryPermission::Write) ||
                    !GuestCpuAccessAllowed(page, true)) {
                    break;
                }
                ++page_end;
            }
        }

        const size_t page_count = page_end - first_page;
        if (page_count <= 1) {
            return PAGE_SIZE;
        }
        sequence.expected_page =
            page_address + static_cast<u64>(page_count) * PAGE_SIZE;
        executor_sequential_buffer_fault_window_count.fetch_add(
            1, std::memory_order_relaxed);
        executor_sequential_buffer_fault_window_pages.fetch_add(
            page_count, std::memory_order_relaxed);
        return static_cast<u64>(page_count) * PAGE_SIZE;
    }
#endif

    bool FaultAccessResolved(const VAddr address, const bool is_write,
                             const bool is_execute,
                             const bool guest_cpu_access,
                             const u16 captured_generation) {
        if (address >= (u64{1} << ADDRESS_BITS)) {
            return false;
        }
        const size_t page = address >> PAGE_BITS;
        std::scoped_lock lk{locks[page / PAGES_PER_LOCK]};
        if (!GpuMapped(page)) {
            return false;
        }
        const Core::MemoryPermission base = BaseHostPerms(page);
        const Core::MemoryPermission requested_permission =
            is_execute ? Core::MemoryPermission::Execute
                       : (is_write ? Core::MemoryPermission::Write
                                   : Core::MemoryPermission::Read);
        const bool baseline_allowed =
            is_execute
                ? True(base & Core::MemoryPermission::Execute)
                : (guest_cpu_access
                       ? (GuestCpuAccessAllowed(page, is_write) ||
                          (is_write &&
                           True(base & Core::MemoryPermission::Write)))
                       : True(base & requested_permission));
        if (!baseline_allowed) {
            return false;
        }
        const bool completed_generation =
            !is_execute &&
            FaultGeneration(page, is_write) != captured_generation;
        const Core::MemoryPermission effective =
            EffectivePerms(page, State(page).WatcherPerms());
        const bool effective_allows = True(effective & requested_permission);
        const bool still_protected =
            is_execute ? false
                       : (is_write ? ActualWriteProtected(page)
                                   : ActualReadProtected(page));
        const bool resolved = completed_generation ||
                              (effective_allows && !still_protected);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (!resolved && ExecutorReadbackContractTraceEnabled()) {
            const u32 ordinal = executor_unresolved_fault_diag_count.fetch_add(
                1, std::memory_order_relaxed);
            if (ordinal < 32) {
                const PageState& state = State(page);
                const char* reason =
                    !effective_allows ? "watcher_denies_access"
                                      : (still_protected ? "pte_still_protected"
                                                         : "generation_unchanged");
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_PAGE_FAULT_UNRESOLVED] reason=%s owner=tracked "
                    "addr=0x%llx page=0x%llx write=%u "
                    "execute=%u guest=%u base=%u cpuRead=%u cpuWrite=%u "
                    "watcher=%u effective=%u actualRead=%u actualWrite=%u "
                    "readWatchers=%u writeWatchers=%u wait=%u jit=%u snapshot=%u "
                    "bypass=%u source=%u capturedGeneration=%u currentGeneration=%u",
                    reason, static_cast<unsigned long long>(address),
                    static_cast<unsigned long long>(page << PAGE_BITS),
                    is_write ? 1u : 0u,
                    is_execute ? 1u : 0u, guest_cpu_access ? 1u : 0u,
                    static_cast<unsigned>(base),
                    GuestCpuAccessAllowed(page, false) ? 1u : 0u,
                    GuestCpuAccessAllowed(page, true) ? 1u : 0u,
                    static_cast<unsigned>(state.WatcherPerms()),
                    static_cast<unsigned>(effective),
                    ActualReadProtected(page) ? 1u : 0u,
                    ActualWriteProtected(page) ? 1u : 0u,
                    static_cast<unsigned>(state.num_read_watchers),
                    static_cast<unsigned>(state.num_write_watchers),
                    static_cast<unsigned>(state.memory_wait_write_watcher),
                    static_cast<unsigned>(state.jit_poll_write_watcher),
                    static_cast<unsigned>(state.descriptor_snapshot_write_watcher),
                    WriteFaultBypassed(page) ? 1u : 0u,
                    static_cast<unsigned>(WriteWatcherSourceMask(page)),
                    static_cast<unsigned>(captured_generation),
                    static_cast<unsigned>(FaultGeneration(page, is_write)));
            }
        }
#endif
        return resolved;
    }

    void ReleaseDeferredWriteProtectionRange(const VAddr address, const u64 size) {
        if (size == 0 || address >= (u64{1} << ADDRESS_BITS)) {
            return;
        }
        const u64 bounded_size = std::min(size, (u64{1} << ADDRESS_BITS) - address);
        size_t page = address >> PAGE_BITS;
        const size_t page_end = Common::DivCeil(address + bounded_size, PAGE_SIZE);
        while (page < page_end) {
            const size_t lock_end =
                std::min(page_end, Common::AlignUp(page + 1, PAGES_PER_LOCK));
            std::scoped_lock lk{locks[page / PAGES_PER_LOCK]};
            while (page < lock_end) {
                while (page < lock_end &&
                       (!ActualWriteProtected(page) ||
                        !True(State(page).WritePerm() &
                              Core::MemoryPermission::Write))) {
                    ++page;
                }
                const size_t first = page;
                const Core::MemoryPermission perms =
                    first < lock_end ? State(first).WatcherPerms()
                                     : Core::MemoryPermission::None;
                while (page < lock_end && ActualWriteProtected(page) &&
                       True(State(page).WritePerm() &
                            Core::MemoryPermission::Write) &&
                       State(page).WatcherPerms() == perms) {
                    ++page;
                }
                if (first != page) {
                    Protect(static_cast<VAddr>(first) << PAGE_BITS,
                            (page - first) << PAGE_BITS, perms);
                }
            }
        }
    }

    [[gnu::noinline]] const ExecutorBulkInvalidationProfileSample*
    SampleBulkGuestInvalidation(const size_t first_page,
                                const size_t page_end,
                                const bool invalidation_required) {
        static thread_local ExecutorBulkInvalidationProfileSample sample;
        sample = {};
        sample.first_protected_page = page_end;
        sample.last_protected_page = first_page;
        auto& eligibility = sample.eligibility;

        Core::MemoryManager* const memory = Core::Memory::TryInstance();
        const bool memory_available = memory != nullptr;
        const bool alias_before =
            memory_available && memory->PhysicalAliasingMayExist();
        const u64 topology_before =
            memory_available ? memory->DebugVmaGeneration() : 0;
        constexpr size_t MaxSampledPages = 256;
        const size_t sampled_page_end =
            std::min(page_end, first_page + MaxSampledPages);
        eligibility.incomplete_sample = sampled_page_end != page_end;
        bool previous_page_protected = false;
        for (size_t chunk_page = first_page; chunk_page < sampled_page_end;) {
            const size_t chunk_end = std::min(
                sampled_page_end,
                Common::AlignUp(chunk_page + 1, PAGES_PER_LOCK));
            std::unique_lock<LockType> chunk_lock{
                locks[chunk_page / PAGES_PER_LOCK], std::try_to_lock};
            if (!chunk_lock.owns_lock()) {
                eligibility.incomplete_sample = true;
                previous_page_protected = false;
                chunk_page = chunk_end;
                continue;
            }
            for (size_t page = chunk_page; page < chunk_end; ++page) {
                const bool page_protected = ActualWriteProtected(page);
                if (page_protected) {
                    sample.first_protected_page =
                        std::min<u64>(sample.first_protected_page, page);
                    sample.last_protected_page = page;
                    ++sample.protected_pages;
                    sample.protected_runs +=
                        previous_page_protected ? 0u : 1u;
                }
                previous_page_protected = page_protected;

                const PageChunk* const chunk = FindChunk(page);
                const PageState* const state = FindState(page);
                const size_t local_page = page % PAGES_PER_LOCK;
                const u8 buffer_watchers =
                    chunk != nullptr ? chunk->buffer_write_watchers[local_page]
                                     : 0;
                const u8 texture_watchers =
                    chunk != nullptr ? chunk->texture_write_watchers[local_page]
                                     : 0;
                const u8 source_mask = WriteWatcherSourceMask(page);
                const u8 expected_source_mask = static_cast<u8>(
                    (buffer_watchers != 0 ? 1u : 0u) |
                    (texture_watchers != 0 ? 2u : 0u));
                const u16 source_count = static_cast<u16>(buffer_watchers) +
                                         static_cast<u16>(texture_watchers);
                const bool source_mismatch =
                    page_protected &&
                    (state == nullptr ||
                     source_mask != expected_source_mask ||
                     source_count != state->num_write_watchers);
                const bool source_none =
                    page_protected && !source_mismatch &&
                    expected_source_mask == 0;
                const bool source_buffer =
                    page_protected && !source_mismatch &&
                    expected_source_mask == 1;
                const bool source_texture =
                    page_protected && !source_mismatch &&
                    expected_source_mask == 2;
                const bool source_mixed =
                    page_protected && !source_mismatch &&
                    expected_source_mask == 3;
                eligibility.source_none_pages += source_none ? 1u : 0u;
                eligibility.source_buffer_pages += source_buffer ? 1u : 0u;
                eligibility.source_texture_pages += source_texture ? 1u : 0u;
                eligibility.source_mixed_pages += source_mixed ? 1u : 0u;
                eligibility.source_mismatch_pages += source_mismatch ? 1u : 0u;

                const bool read_watcher =
                    state != nullptr && state->num_read_watchers != 0;
                const bool actual_read_protected = ActualReadProtected(page);
                const bool memory_wait =
                    state != nullptr && state->memory_wait_write_watcher != 0;
                const bool jit_poll =
                    state != nullptr && state->jit_poll_write_watcher != 0;
                const bool descriptor_watcher =
                    state != nullptr &&
                    state->descriptor_snapshot_write_watcher != 0;
                const bool descriptor_conflict =
                    DescriptorSnapshotConflicted(page);
                const bool gpu_dirty = GpuDirty(page);
                const bool mapped = GpuMapped(page);
                const bool host_write = True(
                    BaseHostPerms(page) & Core::MemoryPermission::Write);
                const bool guest_write = GuestCpuAccessAllowed(page, true);
                const bool active_bulk_writer = BulkWriteActive(page);
                const bool prepare_reservation =
                    BulkPrepareReservation(page) != 0;
                const bool write_fault_bypass = WriteFaultBypassed(page);
                const bool deferred_write_bypass =
                    FexDeferredWriteBypassed(page);
                const bool buffer_capture_blocked =
                    FexBufferCaptureBlocked(page);

                eligibility.read_watcher_pages += read_watcher ? 1u : 0u;
                eligibility.actual_read_protected_pages +=
                    actual_read_protected ? 1u : 0u;
                eligibility.memory_wait_pages += memory_wait ? 1u : 0u;
                eligibility.jit_poll_pages += jit_poll ? 1u : 0u;
                eligibility.descriptor_watcher_pages +=
                    descriptor_watcher ? 1u : 0u;
                eligibility.descriptor_conflict_pages +=
                    descriptor_conflict ? 1u : 0u;
                eligibility.gpu_dirty_pages += gpu_dirty ? 1u : 0u;
                eligibility.unmapped_pages += mapped ? 0u : 1u;
                eligibility.host_write_denied_pages += host_write ? 0u : 1u;
                eligibility.guest_write_denied_pages += guest_write ? 0u : 1u;
                eligibility.active_bulk_writer_pages +=
                    active_bulk_writer ? 1u : 0u;
                eligibility.prepare_reservation_pages +=
                    prepare_reservation ? 1u : 0u;
                eligibility.write_fault_bypass_pages +=
                    write_fault_bypass ? 1u : 0u;
                eligibility.deferred_write_bypass_pages +=
                    deferred_write_bypass ? 1u : 0u;
                eligibility.buffer_capture_blocked_pages +=
                    buffer_capture_blocked ? 1u : 0u;

                const bool page_read_blocked =
                    read_watcher || actual_read_protected;
                const bool page_special_blocked =
                    memory_wait || jit_poll || descriptor_watcher;
                const bool page_source_blocked =
                    page_protected && !source_buffer;
                const bool page_mapping_blocked =
                    !memory_available || !mapped || !host_write || !guest_write;
                const bool page_busy_blocked =
                    active_bulk_writer || prepare_reservation ||
                    descriptor_conflict || write_fault_bypass ||
                    deferred_write_bypass || buffer_capture_blocked;
                eligibility.read_blocker |= page_read_blocked;
                eligibility.special_blocker |= page_special_blocked;
                eligibility.gpu_dirty_blocker |= gpu_dirty;
                eligibility.source_blocker |= page_source_blocked;
                eligibility.mapping_blocker |= page_mapping_blocked;
                eligibility.busy_blocker |= page_busy_blocked;
                if (page_protected && !page_read_blocked &&
                    !page_special_blocked && !gpu_dirty &&
                    !page_source_blocked && !page_mapping_blocked &&
                    !page_busy_blocked) {
                    ++eligibility.candidate_pages;
                }
            }
            chunk_page = chunk_end;
        }

        if (invalidation_required) {
            const bool alias_after =
                memory_available && memory->PhysicalAliasingMayExist();
            const u64 topology_after =
                memory_available ? memory->DebugVmaGeneration() : 0;
            eligibility.alias_observed = alias_before || alias_after;
            eligibility.topology_changed = topology_before != topology_after;
            if (eligibility.incomplete_sample) {
                eligibility.first_blocker =
                    ExecutorBulkInvalidationFirstBlocker::Incomplete;
            } else if (eligibility.alias_observed) {
                eligibility.first_blocker =
                    ExecutorBulkInvalidationFirstBlocker::Alias;
            } else if (eligibility.topology_changed) {
                eligibility.first_blocker =
                    ExecutorBulkInvalidationFirstBlocker::Topology;
            } else if (eligibility.read_blocker) {
                eligibility.first_blocker =
                    ExecutorBulkInvalidationFirstBlocker::Read;
            } else if (eligibility.special_blocker) {
                eligibility.first_blocker =
                    ExecutorBulkInvalidationFirstBlocker::Special;
            } else if (eligibility.gpu_dirty_blocker) {
                eligibility.first_blocker =
                    ExecutorBulkInvalidationFirstBlocker::GpuDirty;
            } else if (eligibility.source_blocker) {
                eligibility.first_blocker =
                    ExecutorBulkInvalidationFirstBlocker::Source;
            } else if (eligibility.mapping_blocker) {
                eligibility.first_blocker =
                    ExecutorBulkInvalidationFirstBlocker::Mapping;
            } else if (eligibility.busy_blocker) {
                eligibility.first_blocker =
                    ExecutorBulkInvalidationFirstBlocker::Busy;
            } else {
                eligibility.first_blocker =
                    ExecutorBulkInvalidationFirstBlocker::Eligible;
            }
        }
        return &sample;
    }

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    void ClearHleCleanBufferWriteTicket() noexcept {
        auto& ticket = executor_hle_clean_buffer_write_ticket;
        ticket.topology_guard.reset();
        ticket.address = 0;
        ticket.size = 0;
        ticket.page_begin = 0;
        ticket.page_bytes = 0;
        ticket.first_page = 0;
        ticket.page_end = 0;
        ticket.attempt_ordinal = 0;
        ticket.active = false;
        ticket.entered = false;
        ticket.pipeline_active = false;
        ticket.counted_active = false;
    }

    void ReleaseHleCleanBufferWriteTicket(const bool committed) noexcept {
        auto& ticket = executor_hle_clean_buffer_write_ticket;
        if (!ticket.active) {
            return;
        }
        const VAddr address = ticket.address;
        const u64 size = ticket.size;
        const u64 ordinal = ticket.attempt_ordinal;
        const size_t pages = ticket.page_end - ticket.first_page;
        const bool counted_active = ticket.counted_active;
        if (ticket.pipeline_active) {
            rasterizer->EndHleCleanBufferWrite(ticket.page_begin,
                                               ticket.page_bytes);
            ticket.pipeline_active = false;
        }
        {
            const auto lock_start =
                locks.begin() + (ticket.first_page / PAGES_PER_LOCK);
            const auto lock_end =
                locks.begin() +
                Common::DivCeil(ticket.page_end, PAGES_PER_LOCK);
            Common::RangeLockGuard lk(lock_start, lock_end);
            for (size_t page = ticket.first_page; page < ticket.page_end;
                 ++page) {
                ASSERT_MSG(BulkWriteActive(page),
                           "Clean HLE bulk lease lost page activity");
                (void)UpdateBulkWriterCount(page, false, "hle-clean-release");
            }
        }
        LeaveTrackedBulkWriteGate();
        ClearHleCleanBufferWriteTicket();

        auto& counters = executor_hle_clean_buffer_counters;
        if (counted_active) {
            const u64 previous_active =
                counters.active.fetch_sub(1, std::memory_order_release);
            ASSERT_MSG(previous_active != 0,
                       "Clean HLE active counter underflow");
        }
        if (committed && ordinal != 0) {
            counters.committed.fetch_add(1, std::memory_order_relaxed);
            counters.pages.fetch_add(pages, std::memory_order_relaxed);
            counters.bytes.fetch_add(size, std::memory_order_relaxed);
            ExecutorHleCleanBufferMaybeLog(ordinal);
        }
        (void)address;
    }

    bool TryEnterHleCleanBufferWrite(const VAddr address,
                                     const u64 size) noexcept {
        auto& ticket = executor_hle_clean_buffer_write_ticket;
        if (!ticket.active) {
            return false;
        }
        if (ticket.address != address || ticket.size != size ||
            ticket.entered) {
            ASSERT_MSG(false, "Mismatched clean HLE bulk write enter");
            hle_clean_buffer_fail_closed.store(true,
                                               std::memory_order_release);
            ReleaseHleCleanBufferWriteTicket(false);
            // Prepare the ordinary transaction which EnterBulkGuestWrite will
            // consume after this returns false. Never leave a stale fast
            // ticket merely because Android assertions are nonfatal.
            PrepareBulkGuestWrite(address, size);
            return false;
        }
        ticket.entered = true;
        return true;
    }

    bool TryLeaveHleCleanBufferWrite(const VAddr address,
                                     const u64 size) noexcept {
        auto& ticket = executor_hle_clean_buffer_write_ticket;
        if (!ticket.active) {
            return false;
        }
        if (ticket.address != address || ticket.size != size ||
            !ticket.entered) {
            ASSERT_MSG(false, "Mismatched clean HLE bulk write leave");
            hle_clean_buffer_fail_closed.store(true,
                                               std::memory_order_release);
            const VAddr original_begin = ticket.page_begin;
            const u64 original_bytes = ticket.page_bytes;
            ReleaseHleCleanBufferWriteTicket(false);
            // The write may already have happened. Publish it synchronously
            // after releasing every PM/Region/shader lease, then consume this
            // leave so generic bookkeeping is not decremented spuriously.
            (void)rasterizer->InvalidateMemoryFromGuestThread(original_begin,
                                                               original_bytes);
            ReleaseDeferredWriteProtectionRange(original_begin,
                                                original_bytes);
            if (size != 0 && address < (u64{1} << ADDRESS_BITS) &&
                size <= (u64{1} << ADDRESS_BITS) - address &&
                (address != original_begin || size != original_bytes)) {
                (void)rasterizer->InvalidateMemoryFromGuestThread(address,
                                                                   size);
                ReleaseDeferredWriteProtectionRange(address, size);
            }
            return true;
        }
        std::atomic_thread_fence(std::memory_order_release);
        ReleaseHleCleanBufferWriteTicket(true);
        return true;
    }

    bool TryPrepareHleCleanBufferWriteForMemset(const VAddr address,
                                                const u64 size) {
        if (!ExecutorHleCleanBufferFastPathEnabled()) {
            return false;
        }
        static const size_t host_page_size =
            static_cast<size_t>(::sysconf(_SC_PAGESIZE));
        auto& counters = executor_hle_clean_buffer_counters;
        const bool collect_stats = ExecutorHleCleanBufferStatsEnabled();
        const u64 ordinal = collect_stats
                                ? counters.attempts.fetch_add(
                                      1, std::memory_order_relaxed) +
                                      1
                                : 0;
        const auto reject = [&](const ExecutorHleCleanBufferFallback reason) {
            ExecutorHleCleanBufferRecordFallback(reason, ordinal);
            return false;
        };
        if (hle_clean_buffer_fail_closed.load(std::memory_order_acquire)) {
            return reject(ExecutorHleCleanBufferFallback::Shader);
        }

        auto& ticket = executor_hle_clean_buffer_write_ticket;
        if (ticket.active || ticket.topology_guard.has_value() ||
            executor_handling_guest_fault ||
            executor_tracked_bulk_write_gate_active ||
            executor_unwatched_bulk_write_active ||
            CurrentFexBulkLeasePhase() != ExecutorFexBulkLeasePhase::Idle ||
            FexWriteEpochs().enabled.load(std::memory_order_acquire)) {
            return reject(ExecutorHleCleanBufferFallback::Nested);
        }
        if (size == 0 || address >= (u64{1} << ADDRESS_BITS) ||
            size > (u64{1} << ADDRESS_BITS) - address ||
            host_page_size != PAGE_SIZE) {
            return reject(ExecutorHleCleanBufferFallback::Size);
        }

        const VAddr page_begin = Common::AlignDown(address, PAGE_SIZE);
        const VAddr page_limit = Common::AlignUp(address + size, PAGE_SIZE);
        const u64 page_bytes = page_limit - page_begin;
        const size_t first_page = page_begin >> PAGE_BITS;
        const size_t page_end = page_limit >> PAGE_BITS;
        if (page_end <= first_page ||
            page_end - first_page > ExecutorHleCleanBufferMaxPages) {
            return reject(ExecutorHleCleanBufferFallback::Size);
        }

        Core::MemoryManager* const memory = Core::Memory::TryInstance();
        if (memory == nullptr || memory->PhysicalAliasingMayExist()) {
            return reject(ExecutorHleCleanBufferFallback::Alias);
        }
        auto topology_guard = memory->AcquireSnapshotReadGuard();
        if (memory->PhysicalAliasingMayExist() ||
            !memory->SnapshotGpuTrackingActive(topology_guard) ||
            !memory->ValidateSnapshotAccess(topology_guard, page_begin,
                                            page_bytes, false, true) ||
            !rasterizer->IsMapped(page_begin, page_bytes) ||
            !InitialGpuMappingRegistrationComplete()) {
            return reject(ExecutorHleCleanBufferFallback::Mapping);
        }

        EnterTrackedBulkWriteGate(address, size);
        ticket.topology_guard.emplace(std::move(topology_guard));
        ticket.address = address;
        ticket.size = size;
        ticket.page_begin = page_begin;
        ticket.page_bytes = page_bytes;
        ticket.first_page = first_page;
        ticket.page_end = page_end;
        ticket.attempt_ordinal = ordinal;

        ExecutorHleCleanBufferFallback page_reject =
            ExecutorHleCleanBufferFallback::Page;
        bool pages_valid = true;
        {
            const auto lock_start =
                locks.begin() + (first_page / PAGES_PER_LOCK);
            const auto lock_end =
                locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
            Common::RangeLockGuard lk(lock_start, lock_end);
            for (size_t page = first_page; page < page_end; ++page) {
                if (hle_clean_buffer_fail_closed.load(
                        std::memory_order_acquire)) {
                    pages_valid = false;
                    page_reject = ExecutorHleCleanBufferFallback::Shader;
                    break;
                }
                const PageChunk* const chunk = FindChunk(page);
                const PageState* const state = FindState(page);
                const size_t local_page = page % PAGES_PER_LOCK;
                const u8 buffer_watchers =
                    chunk != nullptr
                        ? chunk->buffer_write_watchers[local_page]
                        : 0;
                const u8 texture_watchers =
                    chunk != nullptr
                        ? chunk->texture_write_watchers[local_page]
                        : 0;
                if (ShaderReadResidentCount(page) != 0) {
                    pages_valid = false;
                    page_reject = ExecutorHleCleanBufferFallback::Shader;
                    break;
                }
                if (TextureResidentCount(page) != 0) {
                    pages_valid = false;
                    page_reject = ExecutorHleCleanBufferFallback::Texture;
                    break;
                }
                if (state == nullptr || !GpuMapped(page) ||
                    !True(BaseHostPerms(page) &
                          Core::MemoryPermission::Write) ||
                    !GuestCpuAccessAllowed(page, true) ||
                    state->num_read_watchers != 0 ||
                    ActualReadProtected(page) || GpuDirty(page) ||
                    state->memory_wait_write_watcher != 0 ||
                    state->jit_poll_write_watcher != 0 ||
                    state->descriptor_snapshot_write_watcher != 0 ||
                    DescriptorSnapshotConflicted(page) ||
                    BulkWriteActive(page) ||
                    BulkPrepareReservation(page) != 0 ||
                    WriteFaultBypassed(page) ||
                    FexDeferredWriteBypassed(page) ||
                    FexBufferCaptureBlocked(page)) {
                    pages_valid = false;
                    break;
                }
                // Every admitted page must be the same exact protected
                // buffer-watcher state. A mixed range containing even one
                // retained-CPU/unprotected page can race a stable primary
                // upload backing read after RegionManager locks are released.
                if (!ActualWriteProtected(page) ||
                    state->num_write_watchers != 1 ||
                    buffer_watchers != 1 || texture_watchers != 0 ||
                    WriteWatcherSourceMask(page) != 1) {
                    pages_valid = false;
                    break;
                }
            }
            if (pages_valid) {
                for (size_t page = first_page; page < page_end; ++page) {
                    const u8 writer_count = UpdateBulkWriterCount(
                        page, true, "hle-clean-enter");
                    ASSERT_MSG(writer_count == 1,
                               "Clean HLE writer is not exclusive");
                }
                ticket.active = true;
            }
        }
        if (!pages_valid) {
            LeaveTrackedBulkWriteGate();
            ClearHleCleanBufferWriteTicket();
            return reject(page_reject);
        }
        if (hle_clean_buffer_fail_closed.load(std::memory_order_acquire)) {
            ReleaseHleCleanBufferWriteTicket(false);
            return reject(ExecutorHleCleanBufferFallback::Shader);
        }

        // Publish the active-write generation before MemoryTracker metadata
        // or host PTEs can become writable.  Shader readers retry across this
        // interval and never accept a torn SearchBinaryInfo scan.
        if (!rasterizer->BeginHleCleanBufferWrite(page_begin, page_bytes)) {
            // A shader header is currently being scanned. This is ordinary
            // transient contention, not an invariant violation: unwind the
            // complete ticket and synchronously queue behind that reader for
            // this call only. The wrapper then performs its ordinary Prepare
            // and rechecks the residency published by the completed scan.
            ReleaseHleCleanBufferWriteTicket(false);
            (void)rasterizer
                ->InvalidateMemoryBeforeGuestWriteFromGuestThread(page_begin,
                                                                   page_bytes);
            ReleaseDeferredWriteProtectionRange(page_begin, page_bytes);
            return reject(ExecutorHleCleanBufferFallback::Shader);
        }
        ticket.pipeline_active = true;

        const CpuOwnedWritePublishResult publish_result =
            rasterizer->TryPublishHleCpuOwnedWrite(page_begin, page_bytes);
        if (publish_result != CpuOwnedWritePublishResult::Complete) {
            ReleaseHleCleanBufferWriteTicket(false);
            const auto reason = [&] {
                switch (publish_result) {
                case CpuOwnedWritePublishResult::MissingManager:
                    return ExecutorHleCleanBufferFallback::RegionMissing;
                case CpuOwnedWritePublishResult::Busy:
                    return ExecutorHleCleanBufferFallback::RegionBusy;
                case CpuOwnedWritePublishResult::GpuDirty:
                    return ExecutorHleCleanBufferFallback::RegionGpuDirty;
                case CpuOwnedWritePublishResult::Complete:
                    break;
                }
                return ExecutorHleCleanBufferFallback::Postvalidate;
            }();
            return reject(reason);
        }

        bool postvalidated = true;
        {
            const auto lock_start =
                locks.begin() + (first_page / PAGES_PER_LOCK);
            const auto lock_end =
                locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
            Common::RangeLockGuard lk(lock_start, lock_end);
            for (size_t page = first_page; page < page_end; ++page) {
                const PageChunk* const chunk = FindChunk(page);
                const PageState* const state = FindState(page);
                const size_t local_page = page % PAGES_PER_LOCK;
                const u8 buffer_watchers =
                    chunk != nullptr
                        ? chunk->buffer_write_watchers[local_page]
                        : 0;
                const u8 texture_watchers =
                    chunk != nullptr
                        ? chunk->texture_write_watchers[local_page]
                        : 0;
                if (state == nullptr || state->num_bulk_writers != 1 ||
                    hle_clean_buffer_fail_closed.load(
                        std::memory_order_acquire) ||
                    state->bulk_writer_overflow != 0 || !GpuMapped(page) ||
                    !True(BaseHostPerms(page) &
                          Core::MemoryPermission::Write) ||
                    !GuestCpuAccessAllowed(page, true) ||
                    state->num_write_watchers != 0 ||
                    state->num_read_watchers != 0 ||
                    state->memory_wait_write_watcher != 0 ||
                    state->jit_poll_write_watcher != 0 ||
                    state->descriptor_snapshot_write_watcher != 0 ||
                    buffer_watchers != 0 || texture_watchers != 0 ||
                    WriteWatcherSourceMask(page) != 0 ||
                    WriteWatcherHazard(page) ||
                    TextureResidentCount(page) != 0 ||
                    ShaderReadResidentCount(page) != 0 ||
                    ActualReadProtected(page) || ActualWriteProtected(page) ||
                    GpuDirty(page) || DescriptorSnapshotConflicted(page) ||
                    BulkPrepareReservation(page) != 0 ||
                    WriteFaultBypassed(page) ||
                    FexDeferredWriteBypassed(page) ||
                    FexBufferCaptureBlocked(page)) {
                    postvalidated = false;
                    break;
                }
            }
        }
        if (!postvalidated) {
            // MemoryTracker may already have opened the PTE.  Drop every
            // lease/gate/topology guard before entering the ordinary
            // synchronous GpuComm path; otherwise SnapshotReadGuard can form
            // a FIFO deadlock with command processing.
            ReleaseHleCleanBufferWriteTicket(false);
            (void)rasterizer
                ->InvalidateMemoryBeforeGuestWriteFromGuestThread(page_begin,
                                                                   page_bytes);
            ReleaseDeferredWriteProtectionRange(page_begin, page_bytes);
            ExecutorHleCleanBufferRecordFallback(
                ExecutorHleCleanBufferFallback::Postvalidate, ordinal);
            return false;
        }

        if (collect_stats) {
            const u64 active =
                counters.active.fetch_add(1, std::memory_order_acq_rel) + 1;
            ticket.counted_active = true;
            ExecutorHleCleanBufferUpdateMax(counters.active_max, active);
            counters.eligible.fetch_add(1, std::memory_order_relaxed);
            counters.avoided_sync.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }
#endif

    void PrepareBulkGuestWrite(const VAddr address, const u64 size) {
        if (size == 0 || address >= (u64{1} << ADDRESS_BITS)) {
            return;
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (!FexWriteEpochs().enabled.load(std::memory_order_acquire)) {
            const u64 bounded_size =
                std::min(size, (u64{1} << ADDRESS_BITS) - address);
            const size_t first_page = address >> PAGE_BITS;
            const size_t page_end =
                Common::DivCeil(address + bounded_size, PAGE_SIZE);
            if (ExecutorExactProtectedBulkInvalidationRunsEnabled()) {
                boost::container::small_vector<std::pair<VAddr, u64>, 8>
                    protected_runs;
                size_t page = first_page;
                while (page < page_end) {
                    while (page < page_end && !ActualWriteProtected(page)) {
                        ++page;
                    }
                    const size_t run_first_page = page;
                    while (page < page_end && ActualWriteProtected(page)) {
                        ++page;
                    }
                    if (run_first_page != page) {
                        protected_runs.emplace_back(
                            static_cast<VAddr>(run_first_page) << PAGE_BITS,
                            static_cast<u64>(page - run_first_page)
                                << PAGE_BITS);
                    }
                }
                if (protected_runs.empty()) {
                    return;
                }

                // A bulk libc write only needs to retire pages which are
                // currently protected by a GPU/cache watcher.  Keep the
                // existing single FIFO hand-off, but do not make clean gaps
                // participate in cache invalidation, readback discovery or
                // protection reconciliation.  EnterBulkGuestWrite performs
                // the existing post-invalidation gate/recheck, so a watcher
                // installed after this snapshot is still caught before the
                // host write can begin.
                (void)rasterizer
                    ->InvalidateMemoryBatchBeforeGuestWriteFromGuestThread(
                        protected_runs);
                for (const auto& [run_begin, run_size] : protected_runs) {
                    ReleaseDeferredWriteProtectionRange(run_begin, run_size);
                }

                if (ExecutorGuestInvalidationSourceProfileEnabled()) {
                    static std::atomic<u64> exact_run_calls{0};
                    static std::atomic<u64> exact_run_requested_pages{0};
                    static std::atomic<u64> exact_run_invalidated_pages{0};
                    static std::atomic<u64> exact_run_count{0};
                    u64 invalidated_pages = 0;
                    for (const auto& [run_begin, run_size] : protected_runs) {
                        (void)run_begin;
                        invalidated_pages += run_size >> PAGE_BITS;
                    }
                    const u64 calls = exact_run_calls.fetch_add(
                                          1, std::memory_order_relaxed) +
                                      1;
                    exact_run_requested_pages.fetch_add(
                        page_end - first_page, std::memory_order_relaxed);
                    exact_run_invalidated_pages.fetch_add(
                        invalidated_pages, std::memory_order_relaxed);
                    exact_run_count.fetch_add(protected_runs.size(),
                                              std::memory_order_relaxed);
                    if ((calls & 1023u) == 0 && !IsExecutorLiveQuiet()) {
                        __android_log_print(
                            ANDROID_LOG_INFO, "LSX4Native",
                            "[EXECUTOR_EXACT_PROTECTED_RUNS] calls=%llu "
                            "requestedPages=%llu invalidatedPages=%llu "
                            "runs=%llu",
                            static_cast<unsigned long long>(calls),
                            static_cast<unsigned long long>(
                                exact_run_requested_pages.load(
                                    std::memory_order_relaxed)),
                            static_cast<unsigned long long>(
                                exact_run_invalidated_pages.load(
                                    std::memory_order_relaxed)),
                            static_cast<unsigned long long>(
                                exact_run_count.load(
                                    std::memory_order_relaxed)));
                    }
                }
                return;
            }
            size_t first_invalidated_page = first_page;
            while (first_invalidated_page < page_end &&
                   !ActualWriteProtected(first_invalidated_page)) {
                ++first_invalidated_page;
            }
            const bool invalidation_required =
                first_invalidated_page != page_end;
            const bool profile_sources =
                ExecutorShouldSampleGuestInvalidationSources();
            const ExecutorBulkInvalidationProfileSample* const profile_sample =
                profile_sources
                    ? SampleBulkGuestInvalidation(
                          first_page, page_end, invalidation_required)
                    : nullptr;
            if (!invalidation_required) {
                if (profile_sources) {
                    ExecutorRecordBulkInvalidationProfile(
                        page_end - first_page, 0, 0, 0, 0, 0,
                        profile_sample->eligibility);
                }
                return;
            }
            const VAddr page_begin = Common::AlignDown(address, PAGE_SIZE);
            const VAddr page_limit =
                Common::AlignUp(address + bounded_size, PAGE_SIZE);
            const u64 potential_span_pages =
                profile_sources && profile_sample->protected_pages != 0
                    ? profile_sample->last_protected_page -
                          profile_sample->first_protected_page + 1
                    : 0;
            const auto sync_wait_start =
                profile_sources ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
            rasterizer->InvalidateMemoryBeforeGuestWriteFromGuestThread(
                page_begin, page_limit - page_begin);
            const u64 sync_wait_ns =
                profile_sources
                    ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - sync_wait_start)
                          .count()
                    : 0;
            if (profile_sources) {
                ExecutorRecordBulkInvalidationProfile(
                    page_end - first_page, profile_sample->protected_pages,
                    (page_limit - page_begin) >> PAGE_BITS, potential_span_pages,
                    profile_sample->protected_runs, sync_wait_ns,
                    profile_sample->eligibility);
            }
            ReleaseDeferredWriteProtectionRange(page_begin,
                                                page_limit - page_begin);
            return;
        }
        if (TryEnterUnwatchedBulkGuestWrite(address, size)) {
            return;
        }
#endif
        const u64 bounded_size = std::min(size, (u64{1} << ADDRESS_BITS) - address);
        const size_t first_page = address >> PAGE_BITS;
        const size_t page_end = Common::DivCeil(address + bounded_size, PAGE_SIZE);
        const auto close_deferred_write_bypass = [&] {
            const auto lock_start =
                locks.begin() + (first_page / PAGES_PER_LOCK);
            const auto lock_end =
                locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
            Common::RangeLockGuard lk(lock_start, lock_end);
            for (size_t page = first_page; page < page_end; ++page) {
                CloseFexDeferredWriteBypass(page);
            }
        };
        const auto has_protected_page = [&] {
            for (size_t page = first_page; page < page_end; ++page) {
                if (ActualWriteProtected(page)) {
                    return true;
                }
            }
            return false;
        };
        const VAddr page_begin = Common::AlignDown(address, PAGE_SIZE);
        const VAddr page_limit =
            Common::AlignUp(address + bounded_size, PAGE_SIZE);
        const auto invalidate_protected_pages = [&] {
            rasterizer->InvalidateMemoryBeforeGuestWriteFromGuestThread(
                page_begin, page_limit - page_begin);
            ReleaseDeferredWriteProtectionRange(page_begin,
                                                page_limit - page_begin);
        };
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        for (;;) {
            close_deferred_write_bypass();
            if (has_protected_page()) {
                invalidate_protected_pages();
            }

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            const bool ordered_global_admission =
                initial_gpu_mapping_registration_complete.load(
                    std::memory_order_acquire) &&
                FexWriteEpochs().enabled.load(std::memory_order_acquire) &&
                (ExecutorFexBufferCaptureAckEnabled() || !IsGpuThread());
            if (ordered_global_admission &&
                !executor_fex_bulk_writer_admitted) {
                // Global capture admission must precede the local tracked
                // gate. The synchronous invalidation above must precede both,
                // because its GpuComm drainer may own the capture coordinator.
                if (!AdmitCurrentUntrackedFexBulkWriter(address, size)) {
                    ASSERT_MSG(CurrentFexBulkLeasePhase() ==
                                   ExecutorFexBulkLeasePhase::Prepared &&
                                   executor_fex_bulk_lease_epoch != 0,
                               "Failed FEX bulk admission lost its lease");
                    PageManager::AbortCurrentFexBulkLeaseAfter(
                        executor_fex_bulk_lease_epoch - 1);
                    return;
                }
            }
#endif
            EnterTrackedBulkWriteGate(address, size);
            if (!has_protected_page()) {
                return;
            }
            LeaveTrackedBulkWriteGate();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            if (ordered_global_admission) {
                ReleaseCurrentFexBulkWriterAdmission();
            }
#endif
        }
#else
        close_deferred_write_bypass();
        if (has_protected_page()) {
            invalidate_protected_pages();
        }
        return;
#endif
#if 0
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (!FexWriteEpochs().enabled.load(std::memory_order_acquire)) {
            {
                const auto lock_start =
                    locks.begin() + (first_page / PAGES_PER_LOCK);
                const auto lock_end =
                    locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
                Common::RangeLockGuard lk(lock_start, lock_end);
                for (size_t page = first_page; page < page_end; ++page) {
                    CloseFexDeferredWriteBypass(page);
                }
            }
            bool protected_page = false;
            for (size_t page = first_page; page < page_end; ++page) {
                if (ActualWriteProtected(page)) {
                    protected_page = true;
                    break;
                }
            }
            if (!protected_page) {
                return;
            }
            const VAddr page_begin = Common::AlignDown(address, PAGE_SIZE);
            const VAddr page_limit =
                Common::AlignUp(address + bounded_size, PAGE_SIZE);
            rasterizer->InvalidateMemoryBeforeGuestWriteFromGuestThread(
                page_begin, page_limit - page_begin);
            ReleaseDeferredWriteProtectionRange(page_begin,
                                                page_limit - page_begin);
            return;
        }
        auto& ticket = executor_bulk_prepare_ticket;
        ASSERT_MSG(ticket.phase == ExecutorBulkPreparePhase::Idle,
                   "Nested bulk guest write preparation");
        ticket.phase = ExecutorBulkPreparePhase::Preparing;
        ticket.address = address;
        ticket.size = size;
        bool committed = false;
        SCOPE_EXIT {
            if (!committed) {
                ReleaseBulkPrepareReservation(
                    ticket.address, ticket.size, ticket.reservation_token);
                ClearCurrentBulkPrepareTicket();
            }
        };
        ticket.generations.resize(page_end - first_page);
        u32 retry_waits = 0;
        for (;;) {
            ReleaseBulkPrepareReservation(
                ticket.address, ticket.size, ticket.reservation_token);
            ticket.reservation_token = NextBulkPrepareReservationToken();
            const bool protected_page = ReserveBulkGuestWrite(
                address, bounded_size, ticket.reservation_token,
                ticket.generations);

            if (protected_page) {
                const VAddr page_begin = Common::AlignDown(address, PAGE_SIZE);
                const VAddr page_limit =
                    Common::AlignUp(address + bounded_size, PAGE_SIZE);
                const bool invalidated =
                    rasterizer->InvalidateMemoryBeforeGuestWriteFromGuestThread(
                        page_begin, page_limit - page_begin,
                        &Impl::ValidateBulkPrepareReservationCallback, this,
                        ticket.reservation_token);
                if (!invalidated) {
                    ReleaseBulkPrepareReservation(
                        ticket.address, ticket.size,
                        ticket.reservation_token);
                    ticket.reservation_token = 0;
                    BackoffBulkWriterWait(retry_waits);
                    continue;
                }
                ReleaseDeferredWriteProtectionRange(
                    page_begin, page_limit - page_begin);
            }

            bool valid = true;
            {
                const auto lock_start =
                    locks.begin() + (first_page / PAGES_PER_LOCK);
                const auto lock_end =
                    locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
                Common::RangeLockGuard lk(lock_start, lock_end);
                for (size_t page = first_page; page < page_end; ++page) {
                    if (BulkPrepareReservation(page) !=
                            ticket.reservation_token ||
                        BulkWriteActive(page) ||
                        FexDeferredWriteBypassed(page) ||
                        State(page).descriptor_snapshot_write_watcher != 0 ||
                        BulkContractGeneration(page) !=
                            ticket.generations[page - first_page]) {
                        valid = false;
                        break;
                    }
                }
            }
            if (valid) {
                ticket.phase = ExecutorBulkPreparePhase::Prepared;
                committed = true;
                return;
            }
            ReleaseBulkPrepareReservation(
                ticket.address, ticket.size, ticket.reservation_token);
            ticket.reservation_token = 0;
            BackoffBulkWriterWait(retry_waits);
        }
#else
        {
            const auto lock_start = locks.begin() + (first_page / PAGES_PER_LOCK);
            const auto lock_end =
                locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
            Common::RangeLockGuard lk(lock_start, lock_end);
            for (size_t current = first_page; current < page_end; ++current) {
                CloseFexDeferredWriteBypass(current);
            }
        }
        bool protected_page = false;
        size_t page = first_page;
        while (page < page_end) {
            if (ActualWriteProtected(page)) {
                protected_page = true;
                break;
            }
            ++page;
        }
        if (!protected_page) {
            return;
        }
        const VAddr page_begin = Common::AlignDown(address, PAGE_SIZE);
        const VAddr page_limit = Common::AlignUp(address + bounded_size, PAGE_SIZE);
        rasterizer->InvalidateMemoryBeforeGuestWriteFromGuestThread(
            page_begin, page_limit - page_begin);
        ReleaseDeferredWriteProtectionRange(page_begin, page_limit - page_begin);
#endif
#endif
    }

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    bool TryBeginFastBulkGuestWrite(const VAddr address,
                                    const u64 size) noexcept {
        // This is deliberately narrower than the ordinary D51 path.  It
        // fuses only the case in which PrepareBulkGuestWrite would observe no
        // protected page and EnterBulkGuestWrite would then acquire exactly
        // the same PageManager locks to publish bulk-writer activity.  A
        // protected page, descriptor snapshot, deferred epoch or malformed
        // range falls back before any state is changed.
        if (!ExecutorFastBulkGuestWriteEnabled() ||
            !ExecutorFusedFastBulkGuestWriteLeaseEnabled() || size == 0 ||
            address >= (u64{1} << ADDRESS_BITS) ||
            FexWriteEpochs().enabled.load(std::memory_order_acquire)) {
            return false;
        }

        const u64 bounded_size =
            std::min(size, (u64{1} << ADDRESS_BITS) - address);
        if (bounded_size != size) {
            return false;
        }
        const size_t first_page = address >> PAGE_BITS;
        const size_t page_end = Common::DivCeil(address + size, PAGE_SIZE);
        if (page_end <= first_page) {
            return false;
        }

        const auto lock_start =
            locks.begin() + (first_page / PAGES_PER_LOCK);
        const auto lock_end =
            locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        Common::RangeLockGuard lk(lock_start, lock_end);
        const bool publish_sm8650_write_hazard =
            Common::IsSnapdragon8Gen3Host();

        // Prevalidate the complete range before publishing even one writer.
        // Watcher transitions use these same locks, so the subsequent commit
        // is one all-or-nothing no-protection-change transaction.
        for (size_t page = first_page; page < page_end; ++page) {
            const PageState& state = State(page);
            if (ActualWriteProtected(page) ||
                (publish_sm8650_write_hazard &&
                 (BulkWriteActive(page) || FexDeferredWriteBypassed(page))) ||
                state.descriptor_snapshot_write_watcher != 0) {
                return false;
            }
        }

        for (size_t page = first_page; page < page_end; ++page) {
            CloseFexDeferredWriteBypass(page);
            const u8 writer_count = UpdateBulkWriterCount(
                page, true, "fused-fast-enter");
            ASSERT_MSG(!publish_sm8650_write_hazard || writer_count == 1,
                       "Fused fast bulk writer is not exclusive");
            // D51 may elide a redundant RW mprotect only after publishing the
            // lock-free writer hazard consumed by descriptor admission and
            // the signal/fault ownership paths.  A750 previously skipped both
            // operations here, leaving a rare window in which a live HLE
            // memcpy looked like an idle guest range.  Keep A840 byte-for-byte
            // on its measured route and narrow the safety repair to SM8650.
            if (publish_sm8650_write_hazard &&
                !WriteFaultBypassed(page)) {
                SetWriteFaultBypass(page, true);
            }
        }

        static std::atomic<bool> first_hit_logged{false};
        if (!first_hit_logged.load(std::memory_order_relaxed)) {
            bool expected = false;
            if (first_hit_logged.compare_exchange_strong(
                    expected, true, std::memory_order_relaxed)) {
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_FUSED_FAST_BULK_LEASE] firstHit=1 "
                    "rangeLock=1 allWritable=1 descriptorConflict=0 "
                    "protectionChanges=0 fallback=legacy-two-step pages=%zu",
                    page_end - first_page);
            }
        }
        return true;
    }
#endif

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    void CancelPreparedBulkGuestWrite(const VAddr address,
                                      const u64 size) noexcept {
        auto& ticket = executor_bulk_prepare_ticket;
        if (ticket.phase == ExecutorBulkPreparePhase::Idle) {
            return;
        }
        ASSERT_MSG(ticket.phase == ExecutorBulkPreparePhase::Preparing ||
                       (ticket.address == address && ticket.size == size),
                   "Mismatched bulk guest write preparation cancel");
        ReleaseBulkPrepareReservation(
            ticket.address, ticket.size, ticket.reservation_token);
        ClearCurrentBulkPrepareTicket();
    }

    bool TryEnterPreparedBulkGuestWriteActivity(const VAddr address,
                                                const u64 size) {
        auto& ticket = executor_bulk_prepare_ticket;
        ASSERT_MSG(ticket.phase == ExecutorBulkPreparePhase::Prepared &&
                       ticket.address == address && ticket.size == size,
                   "Bulk guest write entered without matching preparation");
        const u64 bounded_size =
            std::min(size, (u64{1} << ADDRESS_BITS) - address);
        const size_t first_page = address >> PAGE_BITS;
        const size_t page_end =
            Common::DivCeil(address + bounded_size, PAGE_SIZE);
        const auto lock_start = locks.begin() + (first_page / PAGES_PER_LOCK);
        const auto lock_end =
            locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        Common::RangeLockGuard lk(lock_start, lock_end);
        for (size_t page = first_page; page < page_end; ++page) {
            const PageState& state = State(page);
            if (BulkPrepareReservation(page) != ticket.reservation_token ||
                BulkWriteActive(page) || FexDeferredWriteBypassed(page) ||
                state.descriptor_snapshot_write_watcher != 0 ||
                BulkContractGeneration(page) !=
                    ticket.generations[page - first_page]) {
                return false;
            }
        }
        const bool publish_sm8650_write_hazard =
            Common::IsSnapdragon8Gen3Host();
        for (size_t page = first_page; page < page_end; ++page) {
            PageState& state = State(page);
            SetBulkPrepareReservation(page, 0);
            const u8 writer_count = UpdateBulkWriterCount(
                page, true, "prepared-enter");
            ASSERT_MSG(writer_count == 1,
                       "Prepared bulk writer is not exclusive");
            const bool elide_redundant_pte_transition =
                ExecutorFastBulkGuestWriteEnabled() &&
                !ActualWriteProtected(page);
            if (!WriteFaultBypassed(page) &&
                (!elide_redundant_pte_transition ||
                 publish_sm8650_write_hazard)) {
                SetWriteFaultBypass(page, true);
                if (!elide_redundant_pte_transition) {
                    const Core::MemoryPermission perms =
                        state.ReadPerm() | Core::MemoryPermission::Write;
                    Protect(static_cast<VAddr>(page) << PAGE_BITS, PAGE_SIZE,
                            perms);
                }
            }
        }
        ClearCurrentBulkPrepareTicket();
        return true;
    }
#endif

    bool TryPrepareFexDeferredBulkGuestWrite(const VAddr address,
                                             const u64 size,
                                             const bool require_read_ready) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        auto& ticket = executor_fex_deferred_bulk_write_ticket;
        if (ticket.active || ticket.count != 0 || size == 0 ||
            address >= (u64{1} << ADDRESS_BITS)) {
            return false;
        }
        const auto* memory = Core::Memory::Instance();
        if (memory == nullptr || memory->PhysicalAliasingMayExist() ||
            !TryEnterFexBulkEpochArming()) {
            return false;
        }
        SCOPE_EXIT { LeaveFexBulkEpochArming(); };

        const u64 bounded_size =
            std::min(size, (u64{1} << ADDRESS_BITS) - address);
        const size_t first_page = address >> PAGE_BITS;
        const size_t page_end =
            Common::DivCeil(address + bounded_size, PAGE_SIZE);
        if (page_end - first_page >
            ExecutorFexDeferredBulkWritePageCapacity) {
            return false;
        }

        std::array<size_t, ExecutorFexDeferredBulkWritePageCapacity>
            watched_pages{};
        size_t watched_count = 0;
        {
            const auto lock_start =
                locks.begin() + (first_page / PAGES_PER_LOCK);
            const auto lock_end =
                locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
            Common::RangeLockGuard lk(lock_start, lock_end);
            for (size_t page = first_page; page < page_end; ++page) {
                if (require_read_ready && ActualReadProtected(page)) {
                    return false;
                }
                const bool deferred_open = FexDeferredWriteBypassed(page);
                if (BulkPrepareReservation(page) != 0) {
                    return false;
                }
                if (!ActualWriteProtected(page)) {
                    if (WriteFaultBypassed(page) ||
                        (!deferred_open && BulkWriteActive(page))) {
                        return false;
                    }
                    if (!deferred_open) {
                        continue;
                    }
                }
                if (!FexDeferredBulkPageContractValid(page)) {
                    return false;
                }
                watched_pages[watched_count++] = page;
            }
        }
        if (watched_count == 0) {
            return false;
        }

        size_t acquired_count = 0;
        size_t newly_reserved_count = 0;
        for (; acquired_count < watched_count; ++acquired_count) {
            u64 token = 0;
            bool owned = false;
            FexWriteEpochSlot* const slot = AcquireFexBulkPageEpoch(
                static_cast<VAddr>(watched_pages[acquired_count]) << PAGE_BITS,
                token, owned);
            if (slot == nullptr) {
                break;
            }
            ticket.tokens[acquired_count] = token;
            ticket.owned[acquired_count] = owned;
            newly_reserved_count += owned ? 1 : 0;
        }
        if (acquired_count != watched_count) {
            FexWriteEpochs().armed.fetch_add(newly_reserved_count,
                                              std::memory_order_relaxed);
            for (size_t index = 0; index < acquired_count; ++index) {
                const u64 token = ticket.tokens[index];
                ReleaseFexBulkPageEpoch(token);
                ticket.tokens[index] = 0;
                ticket.owned[index] = false;
            }
            return false;
        }

        bool pages_still_safe = true;
        {
            const auto lock_start =
                locks.begin() + (first_page / PAGES_PER_LOCK);
            const auto lock_end =
                locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
            Common::RangeLockGuard lk(lock_start, lock_end);
            for (size_t index = 0; index < watched_count; ++index) {
                const size_t page = watched_pages[index];
                if ((require_read_ready && ActualReadProtected(page)) ||
                    !FexDeferredBulkPageContractValid(page)) {
                    pages_still_safe = false;
                    break;
                }
            }
            if (pages_still_safe) {
                for (size_t index = 0; index < watched_count; ++index) {
                    const size_t page = watched_pages[index];
                    if (FexDeferredWriteBypassed(page)) {
                        continue;
                    }
                    SetFexDeferredWriteBypass(page, true);
                    Protect(static_cast<VAddr>(page) << PAGE_BITS, PAGE_SIZE,
                            State(page).WatcherPerms());
                    ASSERT_MSG(!ActualWriteProtected(page),
                               "Failed to open FEX bulk page epoch");
                }
            }
        }
        if (!pages_still_safe) {
            FexWriteEpochs().armed.fetch_add(newly_reserved_count,
                                              std::memory_order_relaxed);
            for (size_t index = 0; index < acquired_count; ++index) {
                ReleaseFexBulkPageEpoch(ticket.tokens[index]);
                ticket.tokens[index] = 0;
                ticket.owned[index] = false;
            }
            return false;
        }

        ticket.count = static_cast<u32>(watched_count);
        ticket.active = true;
        auto& epochs = FexWriteEpochs();
        ASSERT_MSG(!executor_fex_bulk_writer_admitted,
                   "Nested FEX deferred bulk write admission");
        BeginCurrentFexBulkLease(address, size);
        executor_fex_bulk_writer_admitted = true;
        executor_fex_bulk_writer_untracked = false;
        epochs.active_bulk_writers.fetch_add(1, std::memory_order_release);
        epochs.armed.fetch_add(newly_reserved_count,
                               std::memory_order_relaxed);
        epochs.bulk_write_calls.fetch_add(1, std::memory_order_relaxed);
        epochs.bulk_write_pages.fetch_add(watched_count,
                                          std::memory_order_relaxed);
        return true;
#else
        (void)address;
        (void)size;
        return false;
#endif
    }

    bool TryEnterFexDeferredBulkGuestWriteActivity(
        const VAddr address, const u64 size,
        const bool require_read_ready = false) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        const auto& ticket = executor_fex_deferred_bulk_write_ticket;
        if (!ticket.active || ticket.count == 0 || size == 0 ||
            address >= (u64{1} << ADDRESS_BITS)) {
            return false;
        }
        const u64 bounded_size =
            std::min(size, (u64{1} << ADDRESS_BITS) - address);
        const size_t first_page = address >> PAGE_BITS;
        const size_t page_end =
            Common::DivCeil(address + bounded_size, PAGE_SIZE);
        const auto lock_start = locks.begin() + (first_page / PAGES_PER_LOCK);
        const auto lock_end =
            locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        Common::RangeLockGuard lk(lock_start, lock_end);

        u32 ticket_index = 0;
        for (size_t page = first_page; page < page_end; ++page) {
            const VAddr page_address = static_cast<VAddr>(page) << PAGE_BITS;
            if ((require_read_ready && ActualReadProtected(page)) ||
                BulkPrepareReservation(page) != 0) {
                return false;
            }
            bool ticket_page = false;
            if (ticket_index < ticket.count) {
                const u64 token = ticket.tokens[ticket_index];
                if (token == 0) {
                    return false;
                }
                const FexWriteEpochSlot& slot = FexWriteEpochs().slots[
                    static_cast<size_t>(token & FexWriteEpochSlotMask)];
                if (slot.token.load(std::memory_order_acquire) != token ||
                    slot.address < page_address) {
                    return false;
                }
                if (slot.address == page_address) {
                    const u32 state = slot.state.load(std::memory_order_acquire);
                    if (state != static_cast<u32>(FexWriteEpochState::BulkOpen) &&
                        state != static_cast<u32>(FexWriteEpochState::BulkDraining)) {
                        return false;
                    }
                    ticket_page = true;
                    ++ticket_index;
                }
            }
            if (ticket_page) {
                if (!FexDeferredWriteBypassed(page) ||
                    !FexDeferredBulkPageContractValid(page)) {
                    return false;
                }
            } else if (ActualWriteProtected(page)) {
                return false;
            }
        }
        if (ticket_index != ticket.count) {
            return false;
        }
        for (size_t page = first_page; page < page_end; ++page) {
            (void)UpdateBulkWriterCount(page, true, "deferred-enter");
        }
        return true;
#else
        (void)address;
        (void)size;
        return false;
#endif
    }

    bool TryEnterFexDirectReadyBulkGuestWriteActivity(
        const VAddr address, const u64 size,
        const bool require_read_ready) noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (size == 0 || address >= (u64{1} << ADDRESS_BITS)) {
            return false;
        }
        const auto* memory = Core::Memory::Instance();
        if (memory == nullptr || memory->PhysicalAliasingMayExist()) {
            return false;
        }
        const u64 bounded_size =
            std::min(size, (u64{1} << ADDRESS_BITS) - address);
        const size_t first_page = address >> PAGE_BITS;
        const size_t page_end =
            Common::DivCeil(address + bounded_size, PAGE_SIZE);
        const auto lock_start = locks.begin() + (first_page / PAGES_PER_LOCK);
        const auto lock_end =
            locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        Common::RangeLockGuard lk(lock_start, lock_end);

        for (size_t page = first_page; page < page_end; ++page) {
            if ((require_read_ready && ActualReadProtected(page)) ||
                WriteWatcherHazard(page) || ActualWriteProtected(page) ||
                WriteFaultBypassed(page) || FexDeferredWriteBypassed(page) ||
                BulkPrepareReservation(page) != 0) {
                return false;
            }
        }
        for (size_t page = first_page; page < page_end; ++page) {
            (void)UpdateBulkWriterCount(page, true, "direct-ready-enter");
        }
        return true;
#else
        (void)address;
        (void)size;
        (void)require_read_ready;
        return false;
#endif
    }

    bool IsFexGuestMemoryReadReady(const VAddr address,
                                   const u64 size) noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (size == 0 || address >= (u64{1} << ADDRESS_BITS) ||
            size > (u64{1} << ADDRESS_BITS) - address) {
            return false;
        }
        const size_t first_page = address >> PAGE_BITS;
        const size_t page_end = Common::DivCeil(address + size, PAGE_SIZE);
        const auto lock_start = locks.begin() + (first_page / PAGES_PER_LOCK);
        const auto lock_end =
            locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        Common::RangeLockGuard lk(lock_start, lock_end);
        for (size_t page = first_page; page < page_end; ++page) {
            if (ActualReadProtected(page)) {
                return false;
            }
        }
        return true;
#else
        (void)address;
        (void)size;
        return false;
#endif
    }

    u64 DebugFexGuestMemoryLeaseState(const VAddr address, const u64 size,
                                      const bool require_read_ready) noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        // Bits 0..5 describe process/thread admission; bits 8..15 are ORed
        // page-state predicates. The snapshot is intentionally read-only.
        u64 state = 0;
        const auto& epochs = FexWriteEpochs();
        state |= epochs.enabled.load(std::memory_order_acquire) ? 0 : (u64{1} << 0);
        state |= epochs.bulk_gate_open.load(std::memory_order_acquire) ? 0
                                                                      : (u64{1} << 1);
        state |= CurrentFexBulkLeasePhase() == ExecutorFexBulkLeasePhase::Idle
                     ? 0
                     : (u64{1} << 2);
        state |= (executor_fex_deferred_bulk_write_ticket.active ||
                  executor_fex_deferred_bulk_write_ticket.count != 0)
                     ? (u64{1} << 3)
                     : 0;
        const auto* memory = Core::Memory::Instance();
        state |= memory == nullptr ? (u64{1} << 4) : 0;
        state |= memory != nullptr && memory->PhysicalAliasingMayExist()
                     ? (u64{1} << 5)
                     : 0;
        if (size == 0 || address >= (u64{1} << ADDRESS_BITS) ||
            size > (u64{1} << ADDRESS_BITS) - address) {
            return state | (u64{1} << 6);
        }
        const size_t first_page = address >> PAGE_BITS;
        const size_t page_end = Common::DivCeil(address + size, PAGE_SIZE);
        const auto lock_start = locks.begin() + (first_page / PAGES_PER_LOCK);
        const auto lock_end =
            locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        Common::RangeLockGuard lk(lock_start, lock_end);
        for (size_t page = first_page; page < page_end; ++page) {
            state |= require_read_ready && ActualReadProtected(page)
                         ? (u64{1} << 8)
                         : 0;
            state |= WriteWatcherHazard(page) ? (u64{1} << 9) : 0;
            state |= ActualWriteProtected(page) ? (u64{1} << 10) : 0;
            state |= WriteFaultBypassed(page) ? (u64{1} << 11) : 0;
            state |= FexDeferredWriteBypassed(page) ? (u64{1} << 12) : 0;
            state |= BulkPrepareReservation(page) != 0 ? (u64{1} << 13) : 0;
            state |= BulkWriteActive(page) ? (u64{1} << 14) : 0;
            state |= GpuMapped(page) ? (u64{1} << 15) : 0;
            state |= FexDeferredBulkPageContractValid(page) ? (u64{1} << 16) : 0;
        }
        return state;
#else
        (void)address;
        (void)size;
        (void)require_read_ready;
        return 0;
#endif
    }

    void CompleteFexDeferredBulkGuestWrite() noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        PublishAndClearCurrentFexDeferredBulkTicket();
#endif
    }

    void PrepareBulkGuestRead(const VAddr address, const u64 size) {
        if (size == 0 || address >= (u64{1} << ADDRESS_BITS)) {
            return;
        }
        const u64 bounded_size = std::min(size, (u64{1} << ADDRESS_BITS) - address);
        size_t page = address >> PAGE_BITS;
        const size_t page_end = Common::DivCeil(address + bounded_size, PAGE_SIZE);
        while (page < page_end && !ActualReadProtected(page)) {
            ++page;
        }
        if (page != page_end) {
            rasterizer->ReadMemoryFromGuestThread(address, bounded_size);
        }
    }

    bool PrepareTextureGuestReadProtectedRuns(const VAddr address,
                                              const u64 size) {
        if (size == 0) {
            return true;
        }
        if (rasterizer == nullptr || address >= (u64{1} << ADDRESS_BITS) ||
            size > (u64{1} << ADDRESS_BITS) - address) {
            return false;
        }
        const VAddr range_end = address + size;
        const size_t first_page = address >> PAGE_BITS;
        const size_t page_end = Common::DivCeil(range_end, PAGE_SIZE);
        const auto lock_start =
            locks.begin() + (first_page / PAGES_PER_LOCK);
        const auto lock_end =
            locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        struct ProtectedRun {
            VAddr begin;
            u64 size;
            u64 pages;
        };
        boost::container::small_vector<ProtectedRun, 8> runs;
        u64 protected_runs = 0;
        u64 protected_pages = 0;
        u64 readback_bytes = 0;
        u64 failed_runs = 0;
        u64 readback_passes = 0;

        // Snapshot the protection bitmap under the same range locks used by
        // watcher transitions, then release those locks before entering the
        // renderer.  Holding a PageManager lock across a synchronous Vulkan
        // readback would invert the retirement path's lock order.
        const auto collect_protected_runs = [&] {
            runs.clear();
            Common::RangeLockGuard lk(lock_start, lock_end);
            size_t page = first_page;
            while (page < page_end) {
                while (page < page_end && !ActualReadProtected(page)) {
                    ++page;
                }
                if (page == page_end) {
                    break;
                }
                const size_t run_first_page = page++;
                while (page < page_end && ActualReadProtected(page)) {
                    ++page;
                }
                const VAddr run_begin = std::max(
                    address,
                    static_cast<VAddr>(run_first_page) << PAGE_BITS);
                const VAddr run_end = std::min(
                    range_end, static_cast<VAddr>(page) << PAGE_BITS);
                if (run_begin < run_end) {
                    runs.push_back(ProtectedRun{
                        .begin = run_begin,
                        .size = run_end - run_begin,
                        .pages = page - run_first_page,
                    });
                }
            }
            return runs.empty();
        };

        bool read_ready = false;
        constexpr u32 MaxReadbackPasses = 2;
        for (u32 pass = 0; pass < MaxReadbackPasses; ++pass) {
            if (collect_protected_runs()) {
                read_ready = true;
                break;
            }
            ++readback_passes;
            for (const ProtectedRun& run : runs) {
                ++protected_runs;
                protected_pages += run.pages;
                readback_bytes += run.size;
                if (!rasterizer->ReadMemoryFromGuestThread(run.begin,
                                                           run.size)) {
                    ++failed_runs;
                }
            }
            if (failed_runs != 0) {
                break;
            }
        }
        // A readback can retire or expose a second watched range while the
        // PageManager locks are released.  Recheck the complete image under
        // the locks.  If it is still unstable after the bounded passes, the
        // caller must leave the image Dirty and retry instead of hashing.
        if (!read_ready && failed_runs == 0) {
            read_ready = collect_protected_runs();
        }

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (ExecutorReadbackContractTraceEnabled()) {
            static std::atomic<u64> calls{0};
            static std::atomic<u64> total_candidate_bytes{0};
            static std::atomic<u64> total_readback_bytes{0};
            static std::atomic<u64> total_protected_runs{0};
            static std::atomic<u64> total_protected_pages{0};
            static std::atomic<u64> total_failed_runs{0};
            static std::atomic<u64> total_unstable_calls{0};
            const u64 ordinal =
                calls.fetch_add(1, std::memory_order_relaxed) + 1;
            // The former whole-image path issued bounded_size only when it
            // found at least one protected page.  Count that exact baseline,
            // rather than clean images which never caused a readback.
            const u64 candidate_bytes =
                protected_runs != 0 ? size : 0;
            const u64 candidate_total =
                total_candidate_bytes.fetch_add(candidate_bytes,
                                                std::memory_order_relaxed) +
                candidate_bytes;
            const u64 readback_total =
                total_readback_bytes.fetch_add(readback_bytes,
                                               std::memory_order_relaxed) +
                readback_bytes;
            const u64 runs_total =
                total_protected_runs.fetch_add(protected_runs,
                                               std::memory_order_relaxed) +
                protected_runs;
            const u64 pages_total =
                total_protected_pages.fetch_add(protected_pages,
                                                std::memory_order_relaxed) +
                protected_pages;
            const u64 failed_total =
                total_failed_runs.fetch_add(failed_runs,
                                            std::memory_order_relaxed) +
                failed_runs;
            const u64 unstable_total =
                total_unstable_calls.fetch_add(read_ready ? 0u : 1u,
                                               std::memory_order_relaxed) +
                (read_ready ? 0u : 1u);
            const u64 avoided_total =
                candidate_total > readback_total
                    ? candidate_total - readback_total
                    : 0;
            if (ExecutorShouldLogReadbackOrdinal(ordinal)) {
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_TEXTURE_READ_PREFLIGHT] n=%llu surfaceBytes=%llu "
                    "candidateBytes=%llu protectedRuns=%llu protectedPages=%llu "
                    "readbackBytes=%llu passes=%llu ready=%u failedRuns=%llu "
                    "totalCandidateBytes=%llu "
                    "totalReadbackBytes=%llu totalAvoidedBytes=%llu totalRuns=%llu "
                    "totalPages=%llu totalFailedRuns=%llu totalUnstableCalls=%llu",
                    static_cast<unsigned long long>(ordinal),
                    static_cast<unsigned long long>(size),
                    static_cast<unsigned long long>(candidate_bytes),
                    static_cast<unsigned long long>(protected_runs),
                    static_cast<unsigned long long>(protected_pages),
                    static_cast<unsigned long long>(readback_bytes),
                    static_cast<unsigned long long>(readback_passes),
                    read_ready ? 1u : 0u,
                    static_cast<unsigned long long>(failed_runs),
                    static_cast<unsigned long long>(candidate_total),
                    static_cast<unsigned long long>(readback_total),
                    static_cast<unsigned long long>(avoided_total),
                    static_cast<unsigned long long>(runs_total),
                    static_cast<unsigned long long>(pages_total),
                    static_cast<unsigned long long>(failed_total),
                    static_cast<unsigned long long>(unstable_total));
            }
        }
#endif
        return read_ready;
    }

    bool IsGpuThread() const noexcept {
        return rasterizer != nullptr && rasterizer->IsGpuThread();
    }

    void SetBulkGuestWriteActivity(const VAddr address, const u64 size,
                                   const bool active,
                                   const bool deferred_epoch) {
        if (size == 0 || address >= (u64{1} << ADDRESS_BITS)) {
            return;
        }
        const u64 bounded_size = std::min(size, (u64{1} << ADDRESS_BITS) - address);
        size_t page = address >> PAGE_BITS;
        const size_t page_end = Common::DivCeil(address + bounded_size, PAGE_SIZE);
#if defined(LSX4_APPLE_RUNTIME)
        static const bool apple_snapshot_restart = [] {
            const char* value = std::getenv("LSX4_APPLE_BULK_SNAPSHOT_RESTART");
            return value != nullptr && value[0] == '1' && value[1] == '\0';
        }();
        const size_t range_first_page = page;
        u32 snapshot_restart_waits = 0;
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        const bool fast_bulk_guest_write = ExecutorFastBulkGuestWriteEnabled();
        const bool publish_sm8650_write_hazard =
            fast_bulk_guest_write && Common::IsSnapdragon8Gen3Host();
#else
        constexpr bool fast_bulk_guest_write = false;
        constexpr bool publish_sm8650_write_hazard = false;
#endif
        while (page < page_end) {
            bool retry_snapshot = false;
            {
                std::scoped_lock lk{locks[page / PAGES_PER_LOCK]};
                PageState& state = State(page);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
                const bool page_deferred =
                    deferred_epoch &&
                    CurrentFexDeferredBulkTicketOwnsPage(
                        static_cast<VAddr>(page) << PAGE_BITS);
#else
                const bool page_deferred = false;
#endif
                if (active && state.descriptor_snapshot_write_watcher != 0) {
                    retry_snapshot = true;
                } else {
                    if (active && !page_deferred) {
                        CloseFexDeferredWriteBypass(page);
                    }
                    const u8 writer_count = UpdateBulkWriterCount(
                        page, active,
                        active ? "generic-enter" : "generic-leave");
                    if (page_deferred) {
                        ++page;
                        continue;
                    }
                    if (active) {
                        if (writer_count != 1) {
                            ++page;
                            continue;
                        }
                        const bool elide_redundant_pte_transition =
                            fast_bulk_guest_write &&
                            !ActualWriteProtected(page);
                        if (elide_redundant_pte_transition &&
                            !publish_sm8650_write_hazard) {
                            ++page;
                            continue;
                        }
                        if (!WriteFaultBypassed(page)) {
                            SetWriteFaultBypass(page, true);
                            if (!elide_redundant_pte_transition) {
                                const Core::MemoryPermission perms =
                                    state.ReadPerm() |
                                    Core::MemoryPermission::Write;
                                Protect(static_cast<VAddr>(page) << PAGE_BITS,
                                        PAGE_SIZE, perms);
                            }
                        }
                    } else if (writer_count == 0 && WriteFaultBypassed(page)) {
                        SetWriteFaultBypass(page, false);
                        Protect(static_cast<VAddr>(page) << PAGE_BITS, PAGE_SIZE,
                                state.WatcherPerms());
                    }
                }
            }
            if (retry_snapshot) {
#if defined(LSX4_APPLE_RUNTIME)
                if (apple_snapshot_restart) {
                    // Gold L28: an upload can hold a descriptor watcher on a
                    // later page while waiting for the writer counts already
                    // entered on this prefix. No host write has begun yet.
                    // Unwind only our exact prefix through the ordinary leave
                    // path, including protection restoration, before retry.
                    if (page > range_first_page) {
                        SetBulkGuestWriteActivity(
                            static_cast<VAddr>(range_first_page) << PAGE_BITS,
                            static_cast<u64>(page - range_first_page) * PAGE_SIZE,
                            false, deferred_epoch);
                    }
                    static std::atomic<u64> restart_count{0};
                    const u64 ordinal = restart_count.fetch_add(
                        1, std::memory_order_relaxed) + 1;
                    if (ordinal <= 8 || (ordinal & (ordinal - 1)) == 0) {
                        __android_log_print(
                            ANDROID_LOG_INFO, "LSX4Native",
                            "[APPLE_BULK_SNAPSHOT_RESTART] n=%llu "
                            "firstPage=%llu conflictPage=%llu prefixPages=%llu",
                            static_cast<unsigned long long>(ordinal),
                            static_cast<unsigned long long>(range_first_page),
                            static_cast<unsigned long long>(page),
                            static_cast<unsigned long long>(page - range_first_page));
                    }
                    BackoffBulkWriterWait(snapshot_restart_waits);
                    page = range_first_page;
                    continue;
                }
#endif
                std::this_thread::yield();
                continue;
            }
            ++page;
        }
    }

    static bool GuestFaultSignalHandler(void* context, void* fault_address) {
        const auto addr = reinterpret_cast<VAddr>(fault_address);
        const auto host_pc = reinterpret_cast<VAddr>(Common::GetRip(context));
        const bool is_execute = addr == host_pc;
        const bool is_write = !is_execute && Common::IsWriteError(context);

        bool guest_cpu_access = true;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        const auto classify_pc = owner != nullptr
                                     ? owner->signal_context_pc_classifier
                                     : nullptr;
        int pc_class = classify_pc != nullptr ? classify_pc(context) : -1;
        const bool jit_generated_pc = pc_class > 0;
        const auto classify_fex_pc = owner != nullptr
                                         ? owner->fex_signal_context_pc_classifier
                                         : nullptr;
        const bool fex_generated_pc =
            !jit_generated_pc && classify_fex_pc != nullptr &&
            classify_fex_pc(context) > 0;
        if (fex_generated_pc) {
            // Keep FEX provenance distinct from the Current-JIT path below.
            // The latter decodes a read width from JIT publication state/x21,
            // which has no meaning in FEX-generated code.
            pc_class = 2;
        }
        guest_cpu_access = pc_class != 0;

        const u64 provenance_page_begin =
            executor_write_provenance_page_begin.load(std::memory_order_acquire);
        const u64 provenance_page_end =
            executor_write_provenance_page_end.load(std::memory_order_acquire);
        if (is_write && provenance_page_begin != 0 &&
            addr >= provenance_page_begin && addr < provenance_page_end) {
            const u64 exact_begin = executor_write_provenance_exact_begin.load(
                std::memory_order_relaxed);
            const u64 exact_end = executor_write_provenance_exact_end.load(
                std::memory_order_relaxed);
            const bool exact_hit = addr >= exact_begin && addr < exact_end;
            u64 instruction_low = 0;
            u64 instruction_high = 0;
            u32 instruction_meta = 0;
            u64 guest_rip = 0;
            const char* backend = "host";
            if (jit_generated_pc) {
                backend = "jit";
                if (executor_jit_signal_context_fault_provenance != nullptr) {
                    guest_rip = executor_jit_signal_context_fault_provenance(
                        context, &instruction_low, &instruction_high,
                        &instruction_meta);
                }
            } else if (fex_generated_pc) {
                backend = "fex";
                if (executor_lsx4_android_fex_signal_context_guest_rip != nullptr) {
                    guest_rip =
                        executor_lsx4_android_fex_signal_context_guest_rip(context);
                }
            }
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_WRITE_PROVENANCE] backend=%s addr=0x%llx "
                "exactBegin=0x%llx exactEnd=0x%llx exactHit=%u "
                "hostPc=0x%llx guestRip=0x%llx instruction=%016llx:%016llx "
                "instructionLength=%u mnemonic=%u pcClass=%d",
                backend,
                static_cast<unsigned long long>(addr),
                static_cast<unsigned long long>(exact_begin),
                static_cast<unsigned long long>(exact_end),
                exact_hit ? 1u : 0u,
                static_cast<unsigned long long>(host_pc),
                static_cast<unsigned long long>(guest_rip),
                static_cast<unsigned long long>(instruction_high),
                static_cast<unsigned long long>(instruction_low),
                instruction_meta >> 24u,
                instruction_meta & 0x00ffffffu, pc_class);
            executor_write_provenance_page_begin.store(0,
                                                        std::memory_order_release);
            executor_write_provenance_page_end.store(0,
                                                      std::memory_order_release);
            executor_write_provenance_exact_begin.store(
                0, std::memory_order_release);
            executor_write_provenance_exact_end.store(
                0, std::memory_order_release);
        }
#endif

        u16 fault_generation = 0;
        u8 write_watcher_source = 0;
        u8 special_write_watchers = 0;
        FaultOwnership ownership =
            owner != nullptr
                ? owner->ClassifyAndRepairFault(
                      addr, is_write, is_execute, guest_cpu_access,
                      fault_generation, write_watcher_source,
                      special_write_watchers)
                : FaultOwnership::Unowned;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (ownership != FaultOwnership::Unowned && pc_class == 2 && is_write &&
            owner != nullptr && owner->WaitForFexBufferCapture(addr)) {
            ownership = owner->ClassifyAndRepairFault(
                addr, is_write, is_execute, guest_cpu_access,
                fault_generation, write_watcher_source,
                special_write_watchers);
        }
#endif
        if (ownership == FaultOwnership::Unowned) {
            return false;
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (!VideoCore::IsExecutorLiveQuiet()) {
            if (pc_class < 0) {
                executor_owned_unclassified_fault_count.fetch_add(
                    1, std::memory_order_relaxed);
            } else if (pc_class != 0) {
                const u64 generated_ordinal =
                    executor_owned_generated_fault_count.fetch_add(
                        1, std::memory_order_relaxed) +
                    1;
                if (is_write) {
                    auto* counter = write_watcher_source == 1
                                        ? &executor_generated_buffer_write_fault_count
                                    : write_watcher_source == 2
                                        ? &executor_generated_texture_write_fault_count
                                        : &executor_generated_other_write_fault_count;
                    counter->fetch_add(1, std::memory_order_relaxed);
                    if (special_write_watchers != 0) {
                        executor_generated_special_write_fault_count.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    if ((generated_ordinal & 4095u) == 0) {
                        __android_log_print(
                            ANDROID_LOG_INFO, "LSX4Native",
                            "[EXECUTOR_GENERATED_WRITE_FAULTS] total=%llu buffer=%llu "
                            "texture=%llu other=%llu special=%llu "
                            "sequentialWindows=%llu sequentialPages=%llu",
                            static_cast<unsigned long long>(generated_ordinal),
                            static_cast<unsigned long long>(
                                executor_generated_buffer_write_fault_count.load(
                                    std::memory_order_relaxed)),
                            static_cast<unsigned long long>(
                                executor_generated_texture_write_fault_count.load(
                                    std::memory_order_relaxed)),
                            static_cast<unsigned long long>(
                                executor_generated_other_write_fault_count.load(
                                    std::memory_order_relaxed)),
                            static_cast<unsigned long long>(
                                executor_generated_special_write_fault_count.load(
                                    std::memory_order_relaxed)),
                            static_cast<unsigned long long>(
                                executor_sequential_buffer_fault_window_count.load(
                                    std::memory_order_relaxed)),
                            static_cast<unsigned long long>(
                                executor_sequential_buffer_fault_window_pages.load(
                                    std::memory_order_relaxed)));
                    }
                }
            } else {
                executor_owned_host_fault_count.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
#endif
        if (ownership == FaultOwnership::Repaired) {
            return true;
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (pc_class == 2 && is_write && write_watcher_source == 1 &&
            special_write_watchers == 0 && owner != nullptr &&
            executor_lsx4_android_fex_deferred_write_epoch_arm_current != nullptr &&
            Core::Memory::Instance() != nullptr &&
            !Core::Memory::Instance()->PhysicalAliasingMayExist() &&
            EnterFexWriteEpochArming()) {
            u64 token = 0;
            FexWriteEpochSlot* const slot = ReserveFexWriteEpoch(addr, token);
            const bool armed =
                slot != nullptr &&
                executor_lsx4_android_fex_deferred_write_epoch_arm_current(token) == 1;
            const bool opened = armed && owner->TryOpenFexDeferredWrite(addr);
            if (!opened && slot != nullptr) {
                CancelFexWriteEpoch(*slot, token);
            }
            if (opened) {
                FexWriteEpochs().armed.fetch_add(1, std::memory_order_relaxed);
            }
            LeaveFexWriteEpochArming();
            if (opened) {
                return true;
            }
        }
        if (!VideoCore::IsExecutorLiveQuiet()) {
            const int fault_tid = static_cast<int>(LSX4_PAGE_HOST_TID());
            if (pc_class == 0 && rasterizer != nullptr && rasterizer->IsGpuThread()) {
                const u64 ordinal = executor_gpu_thread_host_fault_count.fetch_add(
                                        1, std::memory_order_relaxed) +
                                    1;
                // Signal-context logcat is itself expensive. Keep the early
                // liveness samples and logarithmic diagnostics, but never tax each
                // watched-page fault in normal gameplay.
                if (ordinal <= 8 || (ordinal & (ordinal - 1)) == 0 ||
                    (ordinal & 4095u) == 0) {
                    __android_log_print(
                        ANDROID_LOG_ERROR, "LSX4Native",
                        "[EXECUTOR_GPU_THREAD_PAGE_FAULT] n=%llu tid=%d addr=0x%llx "
                        "hostPc=0x%llx hostLr=0x%llx write=%u generation=%u",
                        static_cast<unsigned long long>(ordinal), fault_tid,
                        static_cast<unsigned long long>(addr),
                        static_cast<unsigned long long>(host_pc),
                        static_cast<unsigned long long>(reinterpret_cast<VAddr>(
                            Common::GetLinkRegister(context))),
                        is_write ? 1u : 0u, static_cast<unsigned>(fault_generation));
                }
            }
        }
        const bool previous_fault_state = executor_handling_guest_fault;
        executor_handling_guest_fault = true;
#endif
        const bool wait_event = is_write && rasterizer->ConsumeMemoryWaitWriteFault(addr);
        const bool jit_poll_event =
            is_write && rasterizer->ConsumeJitPollWriteFault(addr);
        const bool descriptor_snapshot_event =
            is_write && owner->ConsumeDescriptorSnapshotWriteFault(addr);
        VAddr cache_addr = Common::AlignDown(addr, PAGE_SIZE);
        u64 cache_size = PAGE_SIZE;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        VAddr active_bulk_write_begin = 0;
        VAddr active_bulk_write_end = 0;
        bool* active_bulk_write_consumed = nullptr;
        if (is_write) {
            if (auto* const state = ResolveBulkGuestWriteThreadState();
                state != nullptr) {
                active_bulk_write_begin = state->begin;
                active_bulk_write_end = state->end;
                active_bulk_write_consumed = &state->consumed;
            } else {
                active_bulk_write_begin = executor_bulk_write_begin;
                active_bulk_write_end = executor_bulk_write_end;
                active_bulk_write_consumed = &executor_bulk_write_consumed;
            }
            if (!*active_bulk_write_consumed &&
                addr >= active_bulk_write_begin && addr < active_bulk_write_end) {
                cache_addr = Common::AlignDown(active_bulk_write_begin, PAGE_SIZE);
                cache_size =
                    Common::AlignUp(active_bulk_write_end, PAGE_SIZE) - cache_addr;
                *active_bulk_write_consumed = true;
            }
            // An ordinary fault owns only the faulting 4 KiB page. Opening
            // speculative neighbours lets later CPU writes bypass the next
            // invalidation event and progressively makes GPU resources stale.
            // Wider invalidation is legal only through the explicit bulk
            // producer contract above.
        }
#endif
        VAddr completion_addr = cache_addr;
        u64 completion_size = cache_size;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (!is_write && jit_generated_pc) {
            const u64 access_size =
                executor_jit_signal_context_fault_memory_read_size(context);
            if (access_size != 0) {
                const u64 radius = access_size - 1u;
                completion_addr = addr >= radius ? addr - radius : 0;
                completion_size = access_size + radius;
                if (completion_addr >
                    std::numeric_limits<VAddr>::max() - completion_size) {
                    completion_size =
                        std::numeric_limits<VAddr>::max() - completion_addr;
                }
            }
        }
#endif
        const bool cache_event =
            is_write ? rasterizer->InvalidateMemoryBeforeGuestWriteFromGuestThread(
                           cache_addr, cache_size)
                     : rasterizer->ReadMemoryFromGuestThread(
                           cache_addr, cache_size, completion_addr,
                           completion_size);
        const bool deferred_release =
            is_write && owner != nullptr &&
            owner->ReleaseDeferredWriteProtection(addr);
        bool committed_write = false;
        if (is_write && owner != nullptr) {
            const bool persistent =
                addr >= active_bulk_write_begin && addr < active_bulk_write_end;
            committed_write = owner->CommitWriteFault(addr, persistent);
        }
        if (is_write && cache_size > PAGE_SIZE && owner != nullptr) {
            owner->ReleaseDeferredWriteProtectionRange(cache_addr, cache_size);
        }
        // Work performed is not proof that the fault made progress. Returning
        // true while PageManager's protection remains active retries the same
        // host instruction forever. A descriptor write consumes its watcher
        // above and marks the in-flight snapshot conflicted; the reader then
        // discards that copy instead of publishing torn data.
        const bool handled =
            owner->FaultAccessResolved(addr, is_write, is_execute,
                                       guest_cpu_access, fault_generation);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        executor_handling_guest_fault = previous_fault_state;
        if (ExecutorReadbackContractTraceEnabled()) {
            const u64 ordinal =
                executor_readback_fault_count.fetch_add(1, std::memory_order_relaxed) + 1;
            const u64 typed_ordinal =
                (is_write ? executor_readback_write_fault_count
                          : executor_readback_read_fault_count)
                    .fetch_add(1, std::memory_order_relaxed) +
                1;
            if (ExecutorShouldLogReadbackOrdinal(ordinal) ||
                ExecutorShouldLogReadbackOrdinal(typed_ordinal)) {
                const u64 guest_rip = executor_jit_current_fault_guest_rip();
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_READBACK_FAULT] n=%llu typed=%llu addr=0x%llx isWrite=%u "
                    "handled=%u guestRip=0x%llx hostPc=0x%llx caller=0x%llx "
                    "copySparseCaller=0x%llx reads=%llu writes=%llu "
                    "generatedPcFaults=%llu hostPcFaults=%llu unclassifiedPcFaults=%llu "
                    "waitEvent=%u jitPollEvent=%u descriptorSnapshotEvent=%u "
                    "cacheEvent=%u deferredRelease=%u committedWrite=%u",
                    static_cast<unsigned long long>(ordinal),
                    static_cast<unsigned long long>(typed_ordinal),
                    static_cast<unsigned long long>(addr), is_write ? 1u : 0u,
                    handled ? 1u : 0u,
                    static_cast<unsigned long long>(guest_rip),
                    static_cast<unsigned long long>(
                        reinterpret_cast<uintptr_t>(Common::GetRip(context))),
                    static_cast<unsigned long long>(
                        reinterpret_cast<uintptr_t>(Common::GetLinkRegister(context))),
                    static_cast<unsigned long long>(Core::ExecutorCopySparseCaller()),
                    static_cast<unsigned long long>(
                        executor_readback_read_fault_count.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        executor_readback_write_fault_count.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        executor_owned_generated_fault_count.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        executor_owned_host_fault_count.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        executor_owned_unclassified_fault_count.load(
                            std::memory_order_relaxed)),
                    wait_event ? 1u : 0u, jit_poll_event ? 1u : 0u,
                    descriptor_snapshot_event ? 1u : 0u,
                    cache_event ? 1u : 0u, deferred_release ? 1u : 0u,
                    committed_write ? 1u : 0u);
            }
        }
#endif
        return handled;
    }
#endif

    ~Impl() {
        if (owner == this) {
            owner = nullptr;
            rasterizer = nullptr;
        }
        for (auto& slot : page_chunks) {
            delete slot.load(std::memory_order_relaxed);
        }
    }

    template <bool track>
    bool UpdateTextureResidency(const VAddr addr, const u64 size) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (!ExecutorHleCleanBufferFastPathEnabled()) {
            return false;
        }
        if (size == 0 || addr >= (u64{1} << ADDRESS_BITS)) {
            if constexpr (track) {
                hle_clean_buffer_fail_closed.store(true,
                                                   std::memory_order_release);
            }
            return false;
        }
        // Cover the entire valid prefix even if corrupt metadata describes a
        // range which extends beyond the guest address space.  The HLE path
        // can only target this valid prefix, so silently dropping the whole
        // lease here would be fail-open.
        const u64 bounded_size =
            std::min(size, (u64{1} << ADDRESS_BITS) - addr);
        if constexpr (track) {
            if (bounded_size != size) {
                hle_clean_buffer_fail_closed.store(true,
                                                   std::memory_order_release);
            }
        }
        const size_t first_page = addr >> PAGE_BITS;
        const size_t page_end =
            Common::DivCeil(addr + bounded_size, PAGE_SIZE);
        bool update_succeeded = true;
        const auto update = [&] {
            for (size_t page = first_page; page < page_end; ++page) {
                update_succeeded &= UpdateTextureResidentCount(page, track);
            }
        };
        if constexpr (track) {
            // Registration publishes this sidecar before TextureCache's
            // Registered flag/page-table.  Waiting for an active clean HLE
            // writer is safe because that writer never enters TextureCache.
            WithQuiescedBulkWriters(first_page, page_end, update);
        } else {
            const auto lock_start =
                locks.begin() + (first_page / PAGES_PER_LOCK);
            const auto lock_end =
                locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
            Common::RangeLockGuard lk(lock_start, lock_end);
            update();
        }
        return update_succeeded;
#else
        (void)addr;
        (void)size;
        return false;
#endif
    }

    template <bool track>
    bool UpdateHleShaderReadResidency(const VAddr addr, const u64 size) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (!ExecutorHleCleanBufferFastPathEnabled() || size == 0 ||
            addr >= (u64{1} << ADDRESS_BITS) ||
            size > (u64{1} << ADDRESS_BITS) - addr) {
            if constexpr (track) {
                hle_clean_buffer_fail_closed.store(true,
                                                   std::memory_order_release);
            }
            return false;
        }
        const size_t first_page = addr >> PAGE_BITS;
        const size_t page_end = Common::DivCeil(addr + size, PAGE_SIZE);
        bool update_succeeded = true;
        const auto update = [&] {
            for (size_t page = first_page; page < page_end; ++page) {
                update_succeeded &=
                    UpdateShaderReadResidentCount(page, track);
            }
        };
        if constexpr (track) {
            // This uses the same locks and BulkWriteActive state as HLE
            // admission. An earlier writer completes before publication and
            // changes the shader generation; a later writer sees the lease.
            WithQuiescedBulkWriters(first_page, page_end, update);
        } else {
            const auto lock_start =
                locks.begin() + (first_page / PAGES_PER_LOCK);
            const auto lock_end =
                locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
            Common::RangeLockGuard lk(lock_start, lock_end);
            update();
        }
        return update_succeeded;
#else
        (void)addr;
        (void)size;
        return false;
#endif
    }

    template <bool track, bool is_read>
    void UpdatePageWatchers(VAddr addr, u64 size,
                            const u8 write_watcher_source = 1) {
        RENDERER_TRACE;
        const size_t first_page = addr >> PAGE_BITS;
        const u64 page_end = Common::DivCeil(addr + size, PAGE_SIZE);
        const auto update = [&] {
            size_t page = first_page;
            auto perms = State(page).WatcherPerms();
            u64 range_begin = 0;
            u64 range_bytes = 0;
            u64 potential_range_bytes = 0;

            const auto release_pending = [&] {
                if (range_bytes > 0) {
                    RENDERER_TRACE;
                    ApplyWatcherProtection<track, is_read>(
                        range_begin << PAGE_BITS, range_bytes, perms);
                    range_bytes = 0;
                    potential_range_bytes = 0;
                }
            };

            const u64 aligned_addr = page << PAGE_BITS;
            const u64 aligned_end = page_end << PAGE_BITS;
            if (!rasterizer->IsMapped(aligned_addr,
                                      aligned_end - aligned_addr)) {
                LOG_WARNING(
                    Render,
                    "Tracking memory region {:#x} - {:#x} which is not fully GPU mapped.",
                    aligned_addr, aligned_end);
            }

            for (; page != page_end; ++page) {
                PageState& state = State(page);
                const u8 new_count =
                    state.AddDelta<track ? 1 : -1, is_read>();
                AdvanceFaultGeneration(page, !is_read);
                if constexpr (!is_read) {
                    UpdateWriteWatcherSource(page, write_watcher_source, track);
                    PublishWriteWatcherHazard(page, state);
                }
                if constexpr (track) {
                    CloseFexDeferredWriteBypass(page);
                    if (WriteFaultBypassed(page)) {
                        SetWriteFaultBypass(page, false);
                    }
                }
                if (auto new_perms = state.WatcherPerms();
                    new_perms != perms) [[unlikely]] {
                    release_pending();
                    perms = new_perms;
                } else if (range_bytes != 0) {
                    potential_range_bytes += PAGE_SIZE;
                }
                if ((new_count == 0 && !track) ||
                    (new_count == 1 && track)) {
                    if (range_bytes == 0) {
                        range_begin = page;
                        potential_range_bytes = PAGE_SIZE;
                    }
                    range_bytes = potential_range_bytes;
                }
            }
            release_pending();
        };
        if constexpr (track) {
            WithQuiescedBulkWriters(first_page, page_end, update);
        } else {
            const auto lock_start =
                locks.begin() + (first_page / PAGES_PER_LOCK);
            const auto lock_end =
                locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
            Common::RangeLockGuard lk(lock_start, lock_end);
            update();
        }
    }

    template <bool track, bool is_read>
    void UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) {
        RENDERER_TRACE;
        auto start_range = mask.FirstRange();
        auto end_range = mask.LastRange();

        if (start_range.second == end_range.second) {
            const VAddr start_addr = base_addr + (start_range.first << PAGE_BITS);
            const u64 size = (start_range.second - start_range.first) << PAGE_BITS;
            return UpdatePageWatchers<track, is_read>(start_addr, size, 1);
        }

        const size_t base_page = (base_addr >> PAGE_BITS);
        ASSERT(base_page % PAGES_PER_LOCK == 0);
        const size_t first_page = base_page + start_range.first;
        const size_t page_end = base_page + end_range.second;
        const auto update_region = [&] {
            auto perms = State(first_page).WatcherPerms();
            u64 range_begin = 0;
            u64 range_bytes = 0;
            u64 potential_range_bytes = 0;

            const auto release_pending = [&] {
                if (range_bytes > 0) {
                    RENDERER_TRACE;
                    ApplyWatcherProtection<track, is_read>(
                        range_begin << PAGE_BITS, range_bytes, perms);
                    range_bytes = 0;
                    potential_range_bytes = 0;
                }
            };

            for (size_t page = start_range.first; page < end_range.second;
                 ++page) {
                PageState& state = State(base_page + page);
                const bool update = mask.Get(page);
                const u8 new_count =
                    update ? state.AddDelta<track ? 1 : -1, is_read>()
                           : state.AddDelta<0, is_read>();
                if (update) {
                    AdvanceFaultGeneration(base_page + page, !is_read);
                    if constexpr (!is_read) {
                        UpdateWriteWatcherSource(base_page + page, 1, track);
                        PublishWriteWatcherHazard(base_page + page, state);
                    }
                }
                const size_t absolute_page = base_page + page;
                if constexpr (track) {
                    if (update) {
                        CloseFexDeferredWriteBypass(absolute_page);
                        if (WriteFaultBypassed(absolute_page)) {
                            SetWriteFaultBypass(absolute_page, false);
                        }
                    }
                }
                if (auto new_perms = state.WatcherPerms();
                    new_perms != perms) [[unlikely]] {
                    release_pending();
                    perms = new_perms;
                } else if (range_bytes != 0) {
                    potential_range_bytes += PAGE_SIZE;
                }
                if (!update) {
                    continue;
                }
                if ((new_count == 0 && !track) ||
                    (new_count == 1 && track)) {
                    if (range_bytes == 0) {
                        range_begin = absolute_page;
                        potential_range_bytes = PAGE_SIZE;
                    }
                    range_bytes = potential_range_bytes;
                }
            }
            release_pending();
        };
        if constexpr (track) {
            WithQuiescedBulkWriters(first_page, page_end, update_region);
        } else {
            std::scoped_lock lk(locks[base_page / PAGES_PER_LOCK]);
            update_region();
        }
    }

    template <bool track>
    void UpdateMemoryWaitWriteWatchers(VAddr addr, u64 size) {
        if (size == 0 || addr >= (u64{1} << ADDRESS_BITS) ||
            size > (u64{1} << ADDRESS_BITS) - addr) {
            return;
        }
        const size_t first_page = addr >> PAGE_BITS;
        const u64 page_end = Common::DivCeil(addr + size, PAGE_SIZE);
        const auto update = [&] {
            for (size_t page = first_page; page != page_end; ++page) {
                PageState& state = State(page);
                const auto old_perms = state.WatcherPerms();
                const u8 desired = track ? 1u : 0u;
                if (state.memory_wait_write_watcher == desired) {
                    continue;
                }
                if constexpr (track) {
                    active_memory_wait_write_watchers.fetch_add(
                        1, std::memory_order_release);
                }
                state.memory_wait_write_watcher = desired;
                PublishWriteWatcherHazard(page, state);
                AdvanceFaultGeneration(page, true);
                if constexpr (track) {
                    CloseFexDeferredWriteBypass(page);
                    if (WriteFaultBypassed(page)) {
                        SetWriteFaultBypass(page, false);
                    }
                }
                const auto new_perms = state.WatcherPerms();
                if (new_perms != old_perms) {
                    Protect(static_cast<VAddr>(page) << PAGE_BITS, PAGE_SIZE,
                            new_perms, true);
                }
                if constexpr (!track) {
                    active_memory_wait_write_watchers.fetch_sub(
                        1, std::memory_order_release);
                }
            }
        };
        if constexpr (track) {
            WithQuiescedBulkWriters(first_page, page_end, update);
        } else {
            const auto lock_start =
                locks.begin() + (first_page / PAGES_PER_LOCK);
            const auto lock_end =
                locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
            Common::RangeLockGuard lk(lock_start, lock_end);
            update();
        }
    }

    bool ConsumeMemoryWaitWriteFault(VAddr addr) {
        if (addr >= (u64{1} << ADDRESS_BITS) ||
            active_memory_wait_write_watchers.load(std::memory_order_acquire) == 0) {
            return false;
        }
        const size_t page = addr >> PAGE_BITS;
        std::scoped_lock lk{locks[page / PAGES_PER_LOCK]};
        PageState& state = State(page);
        if (state.memory_wait_write_watcher == 0) {
            return false;
        }
        const auto old_perms = state.WatcherPerms();
        state.memory_wait_write_watcher = 0;
        PublishWriteWatcherHazard(page, state);
        AdvanceFaultGeneration(page, true);
        const auto new_perms = state.WatcherPerms();
        if (new_perms != old_perms) {
            Protect(static_cast<VAddr>(page) << PAGE_BITS, PAGE_SIZE,
                    new_perms, true);
        }
        active_memory_wait_write_watchers.fetch_sub(1, std::memory_order_release);
        return true;
    }

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    void ProtectDescriptorSnapshotPages(
        const bool track, const size_t first_page, const size_t page_count,
        const Core::MemoryPermission permissions) {
        if (!ExecutorDescriptorProtectionCohortDiagnosticEnabled()) {
            Protect(static_cast<VAddr>(first_page) << PAGE_BITS,
                    page_count << PAGE_BITS, permissions, true);
            return;
        }
        const u64 begin_cpu_ns = ExecutorDescriptorDiagnosticThreadCpuNow();
        Protect(static_cast<VAddr>(first_page) << PAGE_BITS,
                page_count << PAGE_BITS, permissions, true);
        const u64 end_cpu_ns = ExecutorDescriptorDiagnosticThreadCpuNow();
        ExecutorRecordDescriptorProtectRun(
            track, first_page, page_count, permissions,
            end_cpu_ns >= begin_cpu_ns ? end_cpu_ns - begin_cpu_ns : 0);
    }

    bool CloseFexDeferredWriteBypassSinglePage(
        PageChunk& chunk, const size_t local_page, const size_t page,
        const PageState& state) {
        const u64 mask = u64{1} << (local_page & 63);
        auto& word = chunk.fex_deferred_write_bypass[local_page >> 6];
        if ((word.load(std::memory_order_acquire) & mask) == 0) {
            return false;
        }
        ASSERT_MSG(state.num_bulk_writers == 0 &&
                       state.bulk_writer_overflow == 0,
                   "FEX deferred bypass closed under an active bulk writer");
        word.fetch_and(~mask, std::memory_order_release);
        AdvanceFaultGeneration(chunk, local_page, true);
        Protect(static_cast<VAddr>(page) << PAGE_BITS, PAGE_SIZE,
                state.WatcherPerms(), true);
        return true;
    }

    DescriptorSnapshotTrackResult TryTrackDescriptorSnapshotWriteSinglePage(
        const size_t page) {
        std::scoped_lock lk{locks[page / PAGES_PER_LOCK]};
        PageChunk& chunk = EnsureChunk(page);
        const size_t local_page = page % PAGES_PER_LOCK;
        PageState& state = chunk.pages[local_page];
        const u64 mask = u64{1} << (local_page & 63);

        if (state.num_bulk_writers != 0 || state.bulk_writer_overflow != 0 ||
            (chunk.write_fault_bypass[local_page >> 6].load(
                 std::memory_order_acquire) &
             mask) != 0) {
            return DescriptorSnapshotTrackResult::Busy;
        }
        if (state.descriptor_snapshot_write_watcher != 0) {
            return DescriptorSnapshotTrackResult::Overlap;
        }
        if ((chunk.descriptor_snapshot_conflict[local_page >> 6] & mask) != 0) {
            return DescriptorSnapshotTrackResult::Busy;
        }

        CloseFexDeferredWriteBypassSinglePage(chunk, local_page, page, state);
        const auto old_perms = state.WatcherPerms();
        state.descriptor_snapshot_write_watcher = 1;
        PublishWriteWatcherHazard(chunk, local_page, state);
        AdvanceFaultGeneration(chunk, local_page, true);
        const auto new_perms = state.WatcherPerms();
        if (new_perms != old_perms) {
            ProtectDescriptorSnapshotPages(true, page, 1, new_perms);
        }
        return DescriptorSnapshotTrackResult::Tracked;
    }

    bool UntrackDescriptorSnapshotWriteSinglePage(const size_t page) {
        std::scoped_lock lk{locks[page / PAGES_PER_LOCK]};
        PageChunk& chunk = EnsureChunk(page);
        const size_t local_page = page % PAGES_PER_LOCK;
        PageState& state = chunk.pages[local_page];
        const u64 mask = u64{1} << (local_page & 63);
        const auto old_perms = state.WatcherPerms();
        const bool conflicted =
            (chunk.descriptor_snapshot_conflict[local_page >> 6] & mask) != 0;
        bool clean = true;

        if (state.descriptor_snapshot_write_watcher != 0) {
            state.descriptor_snapshot_write_watcher = 0;
            PublishWriteWatcherHazard(chunk, local_page, state);
            AdvanceFaultGeneration(chunk, local_page, true);
        } else if (conflicted) {
            clean = false;
        } else {
            ASSERT_MSG(false, "Descriptor snapshot watcher underflow");
            clean = false;
        }
        if (conflicted) {
            clean = false;
            chunk.descriptor_snapshot_conflict[local_page >> 6] &= ~mask;
        }
        const auto new_perms = state.WatcherPerms();
        if (new_perms != old_perms) {
            ProtectDescriptorSnapshotPages(false, page, 1, new_perms);
        }
        return clean;
    }

    DescriptorSnapshotTrackResult TryTrackDescriptorSnapshotWriteSingleChunk(
        size_t page, const size_t page_end) {
        ASSERT(page < page_end);
        ASSERT(page / PAGES_PER_LOCK == (page_end - 1) / PAGES_PER_LOCK);
        std::scoped_lock lk{locks[page / PAGES_PER_LOCK]};
        PageChunk& chunk = EnsureChunk(page);

        for (size_t probe = page; probe != page_end; ++probe) {
            const size_t local_page = probe % PAGES_PER_LOCK;
            const PageState& state = chunk.pages[local_page];
            const u64 mask = u64{1} << (local_page & 63);
            if (state.num_bulk_writers != 0 || state.bulk_writer_overflow != 0 ||
                (chunk.write_fault_bypass[local_page >> 6].load(
                     std::memory_order_acquire) &
                 mask) != 0) {
                return DescriptorSnapshotTrackResult::Busy;
            }
            if (state.descriptor_snapshot_write_watcher != 0) {
                return DescriptorSnapshotTrackResult::Overlap;
            }
            if ((chunk.descriptor_snapshot_conflict[local_page >> 6] & mask) != 0) {
                return DescriptorSnapshotTrackResult::Busy;
            }
        }
        for (size_t probe = page; probe != page_end; ++probe) {
            const size_t local_page = probe % PAGES_PER_LOCK;
            CloseFexDeferredWriteBypassSinglePage(
                chunk, local_page, probe, chunk.pages[local_page]);
        }

        struct ProtectionRun {
            size_t first_page;
            size_t page_count;
            Core::MemoryPermission permissions;
        };
        boost::container::small_vector<ProtectionRun, 8> protection_runs;
        const auto append_protection = [&](const size_t changed_page,
                                           const Core::MemoryPermission permissions) {
            if (!protection_runs.empty()) {
                ProtectionRun& previous = protection_runs.back();
                if (previous.first_page + previous.page_count == changed_page &&
                    previous.permissions == permissions) {
                    ++previous.page_count;
                    return;
                }
            }
            protection_runs.push_back({changed_page, 1, permissions});
        };

        const size_t first_page = page;
        for (; page != page_end; ++page) {
            const size_t local_page = page % PAGES_PER_LOCK;
            PageState& state = chunk.pages[local_page];
            const auto old_perms = state.WatcherPerms();
            state.descriptor_snapshot_write_watcher = 1;
            PublishWriteWatcherHazard(chunk, local_page, state);
            AdvanceFaultGeneration(chunk, local_page, true);
            const auto new_perms = state.WatcherPerms();
            if (new_perms != old_perms) {
                append_protection(page, new_perms);
            }
        }
        for (const ProtectionRun& run : protection_runs) {
            ProtectDescriptorSnapshotPages(
                true, run.first_page, run.page_count, run.permissions);
        }
        static std::atomic<bool> first_hit_logged{false};
        if (!first_hit_logged.load(std::memory_order_relaxed) &&
            !first_hit_logged.exchange(true, std::memory_order_relaxed)) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_SINGLE_CHUNK_DESCRIPTOR_SNAPSHOT] firstHit=1 "
                "phase=track pages=%zu chunkLookup=1 exactRange=1",
                page_end - first_page);
        }
        return DescriptorSnapshotTrackResult::Tracked;
    }

    bool UntrackDescriptorSnapshotWriteSingleChunk(size_t page,
                                                   const size_t page_end) {
        ASSERT(page < page_end);
        ASSERT(page / PAGES_PER_LOCK == (page_end - 1) / PAGES_PER_LOCK);
        std::scoped_lock lk{locks[page / PAGES_PER_LOCK]};
        PageChunk& chunk = EnsureChunk(page);

        struct ProtectionRun {
            size_t first_page;
            size_t page_count;
            Core::MemoryPermission permissions;
        };
        boost::container::small_vector<ProtectionRun, 8> protection_runs;
        const auto append_protection = [&](const size_t changed_page,
                                           const Core::MemoryPermission permissions) {
            if (!protection_runs.empty()) {
                ProtectionRun& previous = protection_runs.back();
                if (previous.first_page + previous.page_count == changed_page &&
                    previous.permissions == permissions) {
                    ++previous.page_count;
                    return;
                }
            }
            protection_runs.push_back({changed_page, 1, permissions});
        };

        bool clean = true;
        const size_t first_page = page;
        for (; page != page_end; ++page) {
            const size_t local_page = page % PAGES_PER_LOCK;
            const u64 mask = u64{1} << (local_page & 63);
            PageState& state = chunk.pages[local_page];
            const auto old_perms = state.WatcherPerms();
            const bool conflicted =
                (chunk.descriptor_snapshot_conflict[local_page >> 6] & mask) != 0;
            if (state.descriptor_snapshot_write_watcher != 0) {
                state.descriptor_snapshot_write_watcher = 0;
                PublishWriteWatcherHazard(chunk, local_page, state);
                AdvanceFaultGeneration(chunk, local_page, true);
            } else if (conflicted) {
                clean = false;
            } else {
                ASSERT_MSG(false, "Descriptor snapshot watcher underflow");
                clean = false;
            }
            if (conflicted) {
                clean = false;
                chunk.descriptor_snapshot_conflict[local_page >> 6] &= ~mask;
            }
            const auto new_perms = state.WatcherPerms();
            if (new_perms != old_perms) {
                append_protection(page, new_perms);
            }
        }
        for (const ProtectionRun& run : protection_runs) {
            ProtectDescriptorSnapshotPages(
                false, run.first_page, run.page_count, run.permissions);
        }
        static std::atomic<bool> first_hit_logged{false};
        if (!first_hit_logged.load(std::memory_order_relaxed) &&
            !first_hit_logged.exchange(true, std::memory_order_relaxed)) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_SINGLE_CHUNK_DESCRIPTOR_SNAPSHOT] firstHit=1 "
                "phase=untrack pages=%zu chunkLookup=1 exactRange=1 clean=%u",
                page_end - first_page, clean ? 1u : 0u);
        }
        return clean;
    }
#endif

    DescriptorSnapshotTrackResult TryTrackDescriptorSnapshotWrite(VAddr addr, u64 size) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        static const bool compatible_host_page =
            static_cast<size_t>(::sysconf(_SC_PAGESIZE)) == PAGE_SIZE;
        if (!compatible_host_page) {
            return DescriptorSnapshotTrackResult::Unsupported;
        }
#endif
        if (size == 0 || addr >= (u64{1} << ADDRESS_BITS) ||
            size > (u64{1} << ADDRESS_BITS) - addr) {
            return DescriptorSnapshotTrackResult::Invalid;
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        UnwatchedBulkWriteCutoff unwatched_cutoff{*this};
#endif
        size_t page = addr >> PAGE_BITS;
        const u64 page_end = Common::DivCeil(addr + size, PAGE_SIZE);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (ExecutorSinglePageDescriptorSnapshotFastPathEnabled() &&
            page_end == page + 1) {
            return TryTrackDescriptorSnapshotWriteSinglePage(page);
        }
        if (ExecutorSingleChunkDescriptorSnapshotFastPathEnabled() &&
            page_end > page + 1 &&
            page / PAGES_PER_LOCK == (page_end - 1) / PAGES_PER_LOCK) {
            return TryTrackDescriptorSnapshotWriteSingleChunk(page, page_end);
        }
        const bool local_chunk_hoist =
            ExecutorDescriptorSnapshotLocalChunkHoistEnabled();
#endif
        const auto lock_start = locks.begin() + (page / PAGES_PER_LOCK);
        const auto lock_end = locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        Common::RangeLockGuard lk(lock_start, lock_end);

        for (size_t probe = page; probe != page_end; ++probe) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            if (local_chunk_hoist) {
                PageChunk& chunk = EnsureChunk(probe);
                const size_t local_page = probe % PAGES_PER_LOCK;
                const PageState& state = chunk.pages[local_page];
                const u64 mask = u64{1} << (local_page & 63);
                if (state.num_bulk_writers != 0 ||
                    state.bulk_writer_overflow != 0 ||
                    (chunk.write_fault_bypass[local_page >> 6].load(
                         std::memory_order_acquire) &
                     mask) != 0) {
                    return DescriptorSnapshotTrackResult::Busy;
                }
                if (state.descriptor_snapshot_write_watcher != 0) {
                    return DescriptorSnapshotTrackResult::Overlap;
                }
                if ((chunk.descriptor_snapshot_conflict[local_page >> 6] &
                     mask) != 0) {
                    return DescriptorSnapshotTrackResult::Busy;
                }
                continue;
            }
#endif
            if (BulkWriteActive(probe) || WriteFaultBypassed(probe)) {
                return DescriptorSnapshotTrackResult::Busy;
            }
            if (State(probe).descriptor_snapshot_write_watcher != 0) {
                return DescriptorSnapshotTrackResult::Overlap;
            }
            if (DescriptorSnapshotConflicted(probe)) {
                return DescriptorSnapshotTrackResult::Busy;
            }
        }
        for (size_t probe = page; probe != page_end; ++probe) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            if (local_chunk_hoist) {
                PageChunk& chunk = EnsureChunk(probe);
                const size_t local_page = probe % PAGES_PER_LOCK;
                CloseFexDeferredWriteBypassSinglePage(
                    chunk, local_page, probe, chunk.pages[local_page]);
                continue;
            }
#endif
            CloseFexDeferredWriteBypass(probe);
        }

        struct ProtectionRun {
            size_t first_page;
            size_t page_count;
            Core::MemoryPermission permissions;
        };
        boost::container::small_vector<ProtectionRun, 8> protection_runs;
        const auto append_protection = [&](const size_t changed_page,
                                           const Core::MemoryPermission permissions) {
            if (!protection_runs.empty()) {
                ProtectionRun& previous = protection_runs.back();
                if (previous.first_page + previous.page_count == changed_page &&
                    previous.permissions == permissions) {
                    ++previous.page_count;
                    return;
                }
            }
            protection_runs.push_back({changed_page, 1, permissions});
        };

        if (ExecutorSm8650DescriptorHazardWordPublishEnabled()) {
            // UnwatchedBulkWriteCutoff rejects every lock-free bulk-write
            // admission for this whole transaction, while RangeLockGuard
            // serializes the page state and every other watcher publisher.
            // Descriptor tracking always removes WritePerm, so publish the
            // exact same hazard bits once per atomic word after its PageState
            // and fault generations are complete.  Every release still
            // precedes the protection runs below.  This removes up to 63
            // redundant LSE fetch_or operations without weakening the
            // descriptor snapshot or signal-fault contracts.
            const size_t end_page = static_cast<size_t>(page_end);
            while (page != end_page) {
                PageChunk& chunk = EnsureChunk(page);
                const size_t chunk_end = std::min(
                    end_page,
                    page + PAGES_PER_LOCK - (page % PAGES_PER_LOCK));
                while (page != chunk_end) {
                    const size_t first_local_page = page % PAGES_PER_LOCK;
                    const size_t word_index = first_local_page >> 6;
                    const size_t word_end = std::min(
                        chunk_end, page + 64 - (first_local_page & 63));
                    u64 hazard_mask = 0;
                    for (; page != word_end; ++page) {
                        const size_t local_page = page % PAGES_PER_LOCK;
                        PageState& state = chunk.pages[local_page];
                        const auto old_perms = state.WatcherPerms();
                        state.descriptor_snapshot_write_watcher = 1;
                        ASSERT_MSG(
                            !True(state.WritePerm() &
                                  Core::MemoryPermission::Write),
                            "Descriptor snapshot tracking must publish a "
                            "write hazard");
                        hazard_mask |= u64{1} << (local_page & 63);
                        AdvanceFaultGeneration(chunk, local_page, true);
                        const auto new_perms = state.WatcherPerms();
                        if (new_perms != old_perms) {
                            append_protection(page, new_perms);
                        }
                    }
                    chunk.write_watcher_hazard[word_index].fetch_or(
                        hazard_mask, std::memory_order_release);
                }
            }
        } else {
            for (; page != page_end; ++page) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
                if (local_chunk_hoist) {
                    PageChunk& chunk = EnsureChunk(page);
                    const size_t local_page = page % PAGES_PER_LOCK;
                    PageState& state = chunk.pages[local_page];
                    const auto old_perms = state.WatcherPerms();
                    state.descriptor_snapshot_write_watcher = 1;
                    PublishWriteWatcherHazard(chunk, local_page, state);
                    AdvanceFaultGeneration(chunk, local_page, true);
                    const auto new_perms = state.WatcherPerms();
                    if (new_perms != old_perms) {
                        append_protection(page, new_perms);
                    }
                    continue;
                }
#endif
                PageState& state = State(page);
                const auto old_perms = state.WatcherPerms();
                state.descriptor_snapshot_write_watcher = 1;
                PublishWriteWatcherHazard(page, state);
                AdvanceFaultGeneration(page, true);
                const auto new_perms = state.WatcherPerms();
                if (new_perms != old_perms) {
                    append_protection(page, new_perms);
                }
            }
        }
        for (const ProtectionRun& run : protection_runs) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            ProtectDescriptorSnapshotPages(
                true, run.first_page, run.page_count, run.permissions);
#else
            Protect(static_cast<VAddr>(run.first_page) << PAGE_BITS,
                    run.page_count << PAGE_BITS, run.permissions, true);
#endif
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (local_chunk_hoist) {
            static std::atomic<bool> first_track_hit_logged{false};
            if (!first_track_hit_logged.load(std::memory_order_relaxed) &&
                !first_track_hit_logged.exchange(true,
                                                 std::memory_order_relaxed)) {
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_DESCRIPTOR_SNAPSHOT_LOCAL_CHUNK_HOIST] "
                    "firstHit=1 phase=track pageLocalChunk=1 "
                    "rangeLockPreserved=1 exactState=1");
            }
        }
#endif
        return DescriptorSnapshotTrackResult::Tracked;
    }

    bool ConsumeDescriptorSnapshotWriteFault(VAddr addr) {
        if (addr >= (u64{1} << ADDRESS_BITS)) {
            return false;
        }
        const size_t page = addr >> PAGE_BITS;
        std::scoped_lock lk{locks[page / PAGES_PER_LOCK]};
        PageState& state = State(page);
        if (state.descriptor_snapshot_write_watcher == 0) {
            return false;
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        descriptor_protection_diagnostic.write_faults.fetch_add(
            ExecutorDescriptorProtectionCohortDiagnosticEnabled() ? 1 : 0,
            std::memory_order_relaxed);
#endif
        const auto old_perms = state.WatcherPerms();
        state.descriptor_snapshot_write_watcher = 0;
        PublishWriteWatcherHazard(page, state);
        AdvanceFaultGeneration(page, true);
        SetDescriptorSnapshotConflict(page, true);
        const auto new_perms = state.WatcherPerms();
        if (new_perms != old_perms) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            ProtectDescriptorSnapshotPages(false, page, 1, new_perms);
#else
            Protect(static_cast<VAddr>(page) << PAGE_BITS, PAGE_SIZE,
                    new_perms, true);
#endif
        }
        return true;
    }

    bool UntrackDescriptorSnapshotWrite(VAddr addr, u64 size) {
        if (size == 0 || addr >= (u64{1} << ADDRESS_BITS) ||
            size > (u64{1} << ADDRESS_BITS) - addr) {
            return false;
        }
        size_t page = addr >> PAGE_BITS;
        const u64 page_end = Common::DivCeil(addr + size, PAGE_SIZE);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (ExecutorSinglePageDescriptorSnapshotFastPathEnabled() &&
            page_end == page + 1) {
            return UntrackDescriptorSnapshotWriteSinglePage(page);
        }
        if (ExecutorSingleChunkDescriptorSnapshotFastPathEnabled() &&
            page_end > page + 1 &&
            page / PAGES_PER_LOCK == (page_end - 1) / PAGES_PER_LOCK) {
            return UntrackDescriptorSnapshotWriteSingleChunk(page, page_end);
        }
        const bool local_chunk_hoist =
            ExecutorDescriptorSnapshotLocalChunkHoistEnabled();
#endif
        const auto lock_start = locks.begin() + (page / PAGES_PER_LOCK);
        const auto lock_end = locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        Common::RangeLockGuard lk(lock_start, lock_end);

        struct ProtectionRun {
            size_t first_page;
            size_t page_count;
            Core::MemoryPermission permissions;
        };
        boost::container::small_vector<ProtectionRun, 8> protection_runs;
        const auto append_protection = [&](const size_t changed_page,
                                           const Core::MemoryPermission permissions) {
            if (!protection_runs.empty()) {
                ProtectionRun& previous = protection_runs.back();
                if (previous.first_page + previous.page_count == changed_page &&
                    previous.permissions == permissions) {
                    ++previous.page_count;
                    return;
                }
            }
            protection_runs.push_back({changed_page, 1, permissions});
        };

        bool clean = true;
        for (; page != page_end; ++page) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            if (local_chunk_hoist) {
                PageChunk& chunk = EnsureChunk(page);
                const size_t local_page = page % PAGES_PER_LOCK;
                const u64 mask = u64{1} << (local_page & 63);
                PageState& state = chunk.pages[local_page];
                const auto old_perms = state.WatcherPerms();
                const bool conflicted =
                    (chunk.descriptor_snapshot_conflict[local_page >> 6] &
                     mask) != 0;
                if (state.descriptor_snapshot_write_watcher != 0) {
                    state.descriptor_snapshot_write_watcher = 0;
                    PublishWriteWatcherHazard(chunk, local_page, state);
                    AdvanceFaultGeneration(chunk, local_page, true);
                } else if (conflicted) {
                    clean = false;
                } else {
                    ASSERT_MSG(false,
                               "Descriptor snapshot watcher underflow");
                    clean = false;
                }
                if (conflicted) {
                    clean = false;
                    chunk.descriptor_snapshot_conflict[local_page >> 6] &=
                        ~mask;
                }
                const auto new_perms = state.WatcherPerms();
                if (new_perms != old_perms) {
                    append_protection(page, new_perms);
                }
                continue;
            }
#endif
            PageState& state = State(page);
            const auto old_perms = state.WatcherPerms();
            if (state.descriptor_snapshot_write_watcher != 0) {
                state.descriptor_snapshot_write_watcher = 0;
                PublishWriteWatcherHazard(page, state);
                AdvanceFaultGeneration(page, true);
            } else if (DescriptorSnapshotConflicted(page)) {
                clean = false;
            } else {
                ASSERT_MSG(false, "Descriptor snapshot watcher underflow");
                clean = false;
            }
            if (DescriptorSnapshotConflicted(page)) {
                clean = false;
                SetDescriptorSnapshotConflict(page, false);
            }
            const auto new_perms = state.WatcherPerms();
            if (new_perms != old_perms) {
                append_protection(page, new_perms);
            }
        }
        for (const ProtectionRun& run : protection_runs) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            ProtectDescriptorSnapshotPages(
                false, run.first_page, run.page_count, run.permissions);
#else
            Protect(static_cast<VAddr>(run.first_page) << PAGE_BITS,
                    run.page_count << PAGE_BITS, run.permissions, true);
#endif
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (local_chunk_hoist) {
            static std::atomic<bool> first_untrack_hit_logged{false};
            if (!first_untrack_hit_logged.load(std::memory_order_relaxed) &&
                !first_untrack_hit_logged.exchange(
                    true, std::memory_order_relaxed)) {
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_DESCRIPTOR_SNAPSHOT_LOCAL_CHUNK_HOIST] "
                    "firstHit=1 phase=untrack pageLocalChunk=1 "
                    "rangeLockPreserved=1 exactState=1 clean=%u",
                    clean ? 1u : 0u);
            }
        }
#endif
        return clean;
    }

    template <bool track>
    bool UpdateJitPollWriteWatchers(VAddr addr, u64 size) {
        if (size == 0 || addr >= (u64{1} << ADDRESS_BITS) ||
            size > (u64{1} << ADDRESS_BITS) - addr) {
            return false;
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        std::unique_ptr<UnwatchedBulkWriteCutoff> unwatched_cutoff;
        if constexpr (track) {
            unwatched_cutoff =
                std::make_unique<UnwatchedBulkWriteCutoff>(*this);
        }
#endif
        size_t page = addr >> PAGE_BITS;
        const u64 page_end = Common::DivCeil(addr + size, PAGE_SIZE);
        const auto lock_start = locks.begin() + (page / PAGES_PER_LOCK);
        const auto lock_end = locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        Common::RangeLockGuard lk(lock_start, lock_end);

        if constexpr (track) {
            for (size_t probe = page; probe != page_end; ++probe) {
                if (BulkWriteActive(probe)) {
                    return false;
                }
            }
        }

        for (; page != page_end; ++page) {
            PageState& state = State(page);
            const auto old_perms = state.WatcherPerms();
            const u8 desired = track ? 1u : 0u;
            if (state.jit_poll_write_watcher == desired) {
                continue;
            }
            if constexpr (track) {
                active_jit_poll_write_watchers.fetch_add(1,
                                                         std::memory_order_release);
                CloseFexDeferredWriteBypass(page);
                if (WriteFaultBypassed(page)) {
                    SetWriteFaultBypass(page, false);
                }
            }
            state.jit_poll_write_watcher = desired;
            PublishWriteWatcherHazard(page, state);
            AdvanceFaultGeneration(page, true);
            const auto new_perms = state.WatcherPerms();
            if (new_perms != old_perms) {
                Protect(static_cast<VAddr>(page) << PAGE_BITS, PAGE_SIZE,
                        new_perms, true);
            }
            if constexpr (!track) {
                active_jit_poll_write_watchers.fetch_sub(1,
                                                         std::memory_order_release);
            }
        }
        return true;
    }

    bool ConsumeJitPollWriteFault(VAddr addr) {
        if (addr >= (u64{1} << ADDRESS_BITS) ||
            active_jit_poll_write_watchers.load(std::memory_order_acquire) == 0) {
            return false;
        }
        {
            const size_t page = addr >> PAGE_BITS;
            std::scoped_lock lk{locks[page / PAGES_PER_LOCK]};
            PageState& state = State(page);
            if (state.jit_poll_write_watcher == 0) {
                return false;
            }
            const auto old_perms = state.WatcherPerms();
            state.jit_poll_write_watcher = 0;
            PublishWriteWatcherHazard(page, state);
            AdvanceFaultGeneration(page, true);
            const auto new_perms = state.WatcherPerms();
            if (new_perms != old_perms) {
                Protect(static_cast<VAddr>(page) << PAGE_BITS, PAGE_SIZE,
                        new_perms, true);
            }
            active_jit_poll_write_watchers.fetch_sub(1,
                                                     std::memory_order_release);
        }

        const auto callback =
            jit_poll_write_publish_callback.load(std::memory_order_acquire);
        if (callback != nullptr) {
            callback(addr);
        }
        return true;
    }

    void SetJitPollWritePublishCallback(
        const JitPollWritePublishCallback callback) noexcept {
        jit_poll_write_publish_callback.store(callback, std::memory_order_release);
    }

    std::atomic<u32> active_memory_wait_write_watchers{0};
    std::atomic<u32> active_jit_poll_write_watchers{0};
    std::atomic<u64> bulk_writer_overflows{0};
    std::atomic<u64> bulk_writer_underflows{0};
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    std::atomic<u64> unwatched_bulk_write_gate{0};
    std::atomic<u32> active_unwatched_bulk_writers{0};
    std::atomic<u32> tracked_bulk_writers{0};
    std::atomic<bool> initial_gpu_mapping_registration_complete{false};
    std::atomic<bool> hle_clean_buffer_fail_closed{false};
    std::mutex unwatched_bulk_write_gate_mutex;
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    SignalContextPcClassifier signal_context_pc_classifier{};
    SignalContextPcClassifier fex_signal_context_pc_classifier{};
#endif
    std::atomic<JitPollWritePublishCallback> jit_poll_write_publish_callback{nullptr};
    static_assert(
        std::atomic<JitPollWritePublishCallback>::is_always_lock_free,
        "The JIT poll publication callback must be signal-safe");
    std::array<std::atomic<PageChunk*>, NUM_ADDRESS_CHUNKS> page_chunks{};
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    using LockType = Common::ParkingMutex;
#elif defined(PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP)
    using LockType = Common::AdaptiveMutex;
#else
    using LockType = Common::SpinLock;
#endif
    std::array<LockType, NUM_ADDRESS_LOCKS> locks{};
};

PageManager::PageManager(Vulkan::Rasterizer* rasterizer_)
    : impl{std::make_unique<Impl>(rasterizer_)} {}

PageManager::~PageManager() = default;

void PageManager::CompleteInitialGpuMappingRegistration() noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    impl->CompleteInitialGpuMappingRegistration();
#endif
}

void PageManager::SetFexDeferredWriteEpochEnabled(const bool enabled) noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    auto& epochs = FexWriteEpochs();
    if (!enabled) {
        epochs.gate_open.store(false, std::memory_order_release);
        epochs.bulk_gate_open.store(false, std::memory_order_release);
        epochs.enabled.store(false, std::memory_order_release);
        return;
    }
    std::scoped_lock drain_lock{epochs.drain_mutex};
    ASSERT_MSG(epochs.active_armers.load(std::memory_order_acquire) == 0 &&
                   epochs.active_bulk_armers.load(std::memory_order_acquire) == 0 &&
                   epochs.active_bulk_writers.load(std::memory_order_acquire) == 0 &&
                   epochs.active_untracked_bulk_writers.load(
                       std::memory_order_acquire) == 0 &&
                   epochs.pending.load(std::memory_order_acquire) == 0 &&
                   epochs.faulted.load(std::memory_order_acquire) == 0,
               "Cannot enable FEX write epochs with stale coordinator state");
    for (auto& token : epochs.bulk_page_tokens) {
        token.store(0, std::memory_order_relaxed);
    }
    for (auto& refs : epochs.pending_page_refs) {
        refs.store(0, std::memory_order_relaxed);
    }
    for (auto& token : epochs.pending_page_tokens) {
        token.store(0, std::memory_order_relaxed);
    }
    epochs.enabled.store(true, std::memory_order_release);
    epochs.gate_open.store(true, std::memory_order_release);
    epochs.bulk_gate_open.store(true, std::memory_order_release);
    if (ExecutorExactFexEpochPageIndexEnabled()) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_FEX_EPOCH_INDEX] requested=1 effective=1 "
            "mode=exact-single-ref-fallback-full-scan");
    }
#else
    (void)enabled;
#endif
}

void PageManager::ShutdownFexDeferredWriteEpochs() noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    auto& epochs = FexWriteEpochs();
    epochs.gate_open.store(false, std::memory_order_release);
    epochs.bulk_gate_open.store(false, std::memory_order_release);
    epochs.enabled.store(false, std::memory_order_release);

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(500);
    while (epochs.active_armers.load(std::memory_order_acquire) != 0 ||
           epochs.active_bulk_armers.load(std::memory_order_acquire) != 0 ||
           epochs.active_bulk_writers.load(std::memory_order_acquire) != 0 ||
           epochs.active_untracked_bulk_writers.load(
               std::memory_order_acquire) != 0 ||
           epochs.active_buffer_captures.load(std::memory_order_acquire) != 0 ||
           epochs.faulted.load(std::memory_order_acquire) != 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            ASSERT_MSG(false,
                       "FEX write epochs did not quiesce during rasterizer shutdown");
        }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }

    std::scoped_lock drain_lock{epochs.drain_mutex};
    Impl* const current_owner = Impl::owner;
    std::array<std::pair<VAddr, u64>, FexWriteEpochSlotCount> ranges{};
    size_t range_count = 0;
    for (FexWriteEpochSlot& slot : epochs.slots) {
        u32 state = slot.state.load(std::memory_order_acquire);
        if (state == static_cast<u32>(FexWriteEpochState::Ready)) {
            if (!slot.state.compare_exchange_strong(
                    state, static_cast<u32>(FexWriteEpochState::Draining),
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                ASSERT_MSG(false, "FEX write epoch changed during shutdown");
            }
            RemoveFexPendingPageRef(slot.address);
        } else if (state == static_cast<u32>(FexWriteEpochState::BulkOpen)) {
            if (!slot.state.compare_exchange_strong(
                    state, static_cast<u32>(FexWriteEpochState::BulkDraining),
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                ASSERT_MSG(false, "FEX bulk epoch changed during shutdown");
            }
            ASSERT_MSG(slot.bulk_writers.load(std::memory_order_acquire) == 0,
                       "Active FEX bulk page writer during shutdown");
            RemoveFexPendingPageRef(slot.address);
        } else {
            ASSERT_MSG(state == static_cast<u32>(FexWriteEpochState::Free),
                       "Unexpected FEX write epoch state during shutdown");
            continue;
        }
        if (current_owner != nullptr) {
            current_owner->ReprotectFexDeferredWrite(slot.address);
        }
        ranges[range_count++] = {slot.address, PAGE_SIZE};
    }
    if (range_count != 0 && Impl::rasterizer != nullptr) {
        std::sort(ranges.begin(), ranges.begin() + range_count,
                  [](const auto& lhs, const auto& rhs) {
                      return lhs.first < rhs.first;
                  });
        size_t output = 0;
        for (size_t i = 0; i < range_count; ++i) {
            if (output != 0 &&
                ranges[i].first <=
                    ranges[output - 1].first + ranges[output - 1].second) {
                const VAddr end = std::max(
                    ranges[output - 1].first + ranges[output - 1].second,
                    ranges[i].first + ranges[i].second);
                ranges[output - 1].second =
                    end - ranges[output - 1].first;
            } else {
                ranges[output++] = ranges[i];
            }
        }
        (void)Impl::rasterizer->InvalidateMemoryBatchFromGuestThread(
            std::span<const std::pair<VAddr, u64>>{ranges.data(), output});
    }
    for (FexWriteEpochSlot& slot : epochs.slots) {
        const u32 state = slot.state.load(std::memory_order_acquire);
        if (state == static_cast<u32>(FexWriteEpochState::Draining) ||
            state == static_cast<u32>(FexWriteEpochState::BulkDraining)) {
            slot.bulk_writers.store(0, std::memory_order_relaxed);
            slot.state.store(static_cast<u32>(FexWriteEpochState::Free),
                             std::memory_order_release);
        }
    }
    epochs.pending.store(0, std::memory_order_release);
    epochs.faulted.store(0, std::memory_order_release);
    for (auto& token : epochs.bulk_page_tokens) {
        token.store(0, std::memory_order_relaxed);
    }
    for (auto& token : epochs.pending_page_tokens) {
        token.store(0, std::memory_order_relaxed);
    }
#endif
}

bool PageManager::BeginFexDeferredWriteSubmitBoundary() {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    auto& epochs = FexWriteEpochs();
    if (CurrentFexBulkLeasePhase() != ExecutorFexBulkLeasePhase::Idle) {
        return false;
    }
    const bool capture_ack = ExecutorFexBufferCaptureAckEnabled();
    if (capture_ack &&
        epochs.pending.load(std::memory_order_acquire) <
            FexWriteEpochPressureThreshold) {
        return false;
    }
    if (!epochs.enabled.load(std::memory_order_acquire)) {
        return false;
    }
    epochs.drain_mutex.lock();
    if (!epochs.enabled.load(std::memory_order_acquire)) {
        epochs.drain_mutex.unlock();
        return false;
    }
    if (capture_ack &&
        epochs.pending.load(std::memory_order_acquire) <
            FexWriteEpochPressureThreshold) {
        epochs.drain_mutex.unlock();
        return false;
    }
    epochs.gate_open.store(false, std::memory_order_release);
    epochs.bulk_gate_open.store(false, std::memory_order_release);
    const auto watchdog = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(10);
    bool watchdog_reported = false;
    u32 armer_waits = 0;
    while (epochs.active_armers.load(std::memory_order_acquire) != 0 ||
           epochs.active_bulk_armers.load(std::memory_order_acquire) != 0 ||
           epochs.active_untracked_bulk_writers.load(
               std::memory_order_acquire) != 0) {
        if (!watchdog_reported &&
            std::chrono::steady_clock::now() >= watchdog) {
            watchdog_reported = true;
            __android_log_print(
                ANDROID_LOG_WARN, "LSX4Native",
                "[EXECUTOR_FEX_WRITE_EPOCH] submit boundary waiting for "
                "%u/%u armer(s), %u untracked bulk writer(s)",
                epochs.active_armers.load(std::memory_order_relaxed),
                epochs.active_bulk_armers.load(std::memory_order_relaxed),
                epochs.active_untracked_bulk_writers.load(
                    std::memory_order_relaxed));
        }
        if (++armer_waits < 128) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
    return true;
#else
    return false;
#endif
}

void PageManager::EndFexDeferredWriteSubmitBoundary(
    const bool active) noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (!active) {
        return;
    }
    auto& epochs = FexWriteEpochs();
    if (epochs.enabled.load(std::memory_order_acquire)) {
        epochs.gate_open.store(true, std::memory_order_release);
        epochs.bulk_gate_open.store(true, std::memory_order_release);
    }
    epochs.drain_mutex.unlock();
#else
    (void)active;
#endif
}

void PageManager::PublishFexDeferredWriteEpochReady(
    void*, const u64 token) noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    auto& epochs = FexWriteEpochs();
    FexWriteEpochSlot& slot =
        epochs.slots[static_cast<size_t>(token & FexWriteEpochSlotMask)];
    if (slot.token.load(std::memory_order_acquire) != token) {
        return;
    }
    u32 expected = static_cast<u32>(FexWriteEpochState::Faulted);
    if (slot.state.compare_exchange_strong(
            expected, static_cast<u32>(FexWriteEpochState::Ready),
            std::memory_order_release, std::memory_order_relaxed)) {
        epochs.faulted.fetch_sub(1, std::memory_order_release);
    }
#else
    (void)token;
#endif
}

bool PageManager::MayHaveFexDeferredBufferCapture(
    const VAddr address, const u64 size) noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    auto& epochs = FexWriteEpochs();
    if (!ExecutorFexBufferCaptureAckEnabled() || size == 0 ||
        address > std::numeric_limits<VAddr>::max() - size ||
        !epochs.enabled.load(std::memory_order_acquire) ||
        Impl::owner == nullptr ||
        Core::Memory::Instance() == nullptr) {
        return false;
    }
    Impl* const owner = Impl::owner;
    if (epochs.active_armers.load(std::memory_order_acquire) != 0 ||
        epochs.active_bulk_armers.load(std::memory_order_acquire) != 0 ||
        epochs.active_bulk_writers.load(std::memory_order_acquire) != 0 ||
        epochs.active_untracked_bulk_writers.load(std::memory_order_acquire) != 0 ||
        owner->tracked_bulk_writers.load(std::memory_order_acquire) != 0 ||
        owner->active_unwatched_bulk_writers.load(std::memory_order_acquire) != 0) {
        return true;
    }
    const VAddr page_begin =
        Common::AlignDown(address, TRACKER_BYTES_PER_PAGE);
    const VAddr page_end =
        Common::AlignUp(address + size, TRACKER_BYTES_PER_PAGE);
    if ((page_end - page_begin) / TRACKER_BYTES_PER_PAGE >
        ExecutorFexBufferCaptureTicket::PageCapacity) {
        return epochs.pending.load(std::memory_order_acquire) != 0;
    }
    for (VAddr page = page_begin; page < page_end; page += TRACKER_BYTES_PER_PAGE) {
        if (epochs.pending_page_refs[FexPendingPageBucket(page)].load(std::memory_order_acquire) !=
            0) {
            return true;
        }
    }
    return false;
#else
    (void)address;
    (void)size;
    return false;
#endif
}

std::span<const VAddr> PageManager::CurrentFexDeferredBufferCaptureDirtyPages() noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const auto& ticket = executor_fex_buffer_capture_ticket;
    if (!ticket.active) {
        return {};
    }
    return {ticket.dirty_pages.data(), ticket.dirty_page_count};
#else
    return {};
#endif
}

bool PageManager::BeginFexDeferredBufferCapture(const VAddr address, const u64 size,
                                                const bool texture_cache_lock_held) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (CurrentFexBulkLeasePhase() != ExecutorFexBulkLeasePhase::Idle) {
        return false;
    }
    auto& epochs = FexWriteEpochs();
    auto& ticket = executor_fex_buffer_capture_ticket;
    if (!MayHaveFexDeferredBufferCapture(address, size) || ticket.active ||
        ticket.token_count != 0 || ticket.guarded_page_count != 0 || ticket.dirty_page_count != 0) {
        return false;
    }
    const VAddr capture_begin = Common::AlignDown(address, TRACKER_BYTES_PER_PAGE);
    const VAddr capture_end = Common::AlignUp(address + size, TRACKER_BYTES_PER_PAGE);
    if ((capture_end - capture_begin) / TRACKER_BYTES_PER_PAGE >
        ExecutorFexBufferCaptureTicket::PageCapacity) {
        return false;
    }

    u32 faulted_retries = 0;
    for (;;) {
        std::unique_lock coordinator_lock{epochs.drain_mutex};
        if (!epochs.enabled.load(std::memory_order_acquire) || Impl::owner == nullptr) {
            return false;
        }
        epochs.gate_open.store(false, std::memory_order_release);
        epochs.bulk_gate_open.store(false, std::memory_order_release);
        const auto reopen_gates = [&] {
            if (epochs.enabled.load(std::memory_order_acquire)) {
                epochs.gate_open.store(true, std::memory_order_release);
                epochs.bulk_gate_open.store(true, std::memory_order_release);
            }
        };
        u32 waits = 0;
        while (epochs.active_armers.load(std::memory_order_acquire) != 0 ||
               epochs.active_bulk_armers.load(std::memory_order_acquire) != 0 ||
               epochs.active_bulk_writers.load(std::memory_order_acquire) != 0 ||
               epochs.active_untracked_bulk_writers.load(std::memory_order_acquire) != 0) {
            if (++waits < 256) {
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        }

        const auto overlaps = [&](const FexWriteEpochSlot& slot) {
            return slot.address < capture_end && slot.address + PAGE_SIZE > capture_begin;
        };
        bool has_ready = false;
        bool must_retry = false;
        bool unsupported = false;
        u64 first_faulted_token = 0;
        VAddr first_faulted_address = 0;
        int first_faulted_owner_tid = 0;
        const auto inspect_slot = [&](const FexWriteEpochSlot& slot) {
            const auto state =
                static_cast<FexWriteEpochState>(slot.state.load(std::memory_order_acquire));
            has_ready |= state == FexWriteEpochState::Ready;
            if (state == FexWriteEpochState::Ready) {
                const auto dirty_end = ticket.dirty_pages.begin() + ticket.dirty_page_count;
                if (std::find(ticket.dirty_pages.begin(), dirty_end, slot.address) == dirty_end) {
                    ASSERT_MSG(ticket.dirty_page_count < ticket.dirty_pages.size(),
                               "FEX buffer capture dirty-page overflow");
                    ticket.dirty_pages[ticket.dirty_page_count++] = slot.address;
                }
            }
            must_retry |= state == FexWriteEpochState::Faulted;
            if (state == FexWriteEpochState::Faulted && first_faulted_token == 0) {
                first_faulted_token = slot.token.load(std::memory_order_acquire);
                first_faulted_address = slot.address;
                first_faulted_owner_tid = slot.owner_tid;
            }
            unsupported |=
                state == FexWriteEpochState::BulkOpen || state == FexWriteEpochState::BulkDraining;
        };

        // The index is an optional exact accelerator, never an authority.  A
        // bucket is usable only with one live ref and a token whose slot still
        // names this exact page.  Collisions, transitional publication, or a
        // stale token retain the original complete 4096-slot scan.
        const u64 capture_page_count =
            (capture_end - capture_begin) / TRACKER_BYTES_PER_PAGE;
        bool exact_indexed = ExecutorExactFexEpochPageIndexEnabled() &&
                             capture_page_count < FexWriteEpochSlotCount;
        u32 indexed_token_count = 0;
        if (exact_indexed) {
            for (VAddr page = capture_begin; page < capture_end;
                 page += TRACKER_BYTES_PER_PAGE) {
                const size_t bucket = FexPendingPageBucket(page);
                const u32 refs = epochs.pending_page_refs[bucket].load(
                    std::memory_order_acquire);
                if (refs == 0) {
                    continue;
                }
                const u64 token = epochs.pending_page_tokens[bucket].load(
                    std::memory_order_acquire);
                if (refs != 1 || token == 0 ||
                    indexed_token_count == ticket.tokens.size()) {
                    exact_indexed = false;
                    break;
                }
                const FexWriteEpochSlot& slot = epochs.slots[
                    static_cast<size_t>(token & FexWriteEpochSlotMask)];
                if (slot.token.load(std::memory_order_acquire) != token ||
                    slot.address != page) {
                    exact_indexed = false;
                    break;
                }
                ticket.tokens[indexed_token_count++] = token;
            }
        }
        if (exact_indexed) {
            for (u32 index = 0; index < indexed_token_count; ++index) {
                const u64 token = ticket.tokens[index];
                inspect_slot(epochs.slots[static_cast<size_t>(
                    token & FexWriteEpochSlotMask)]);
            }
        } else {
            for (u32 index = 0; index < indexed_token_count; ++index) {
                ticket.tokens[index] = 0;
            }
            indexed_token_count = 0;
            for (const FexWriteEpochSlot& slot : epochs.slots) {
                if (overlaps(slot)) {
                    inspect_slot(slot);
                }
            }
        }
        if (must_retry) {
            for (u32 index = 0; index < indexed_token_count; ++index) {
                ticket.tokens[index] = 0;
            }
            for (u32 index = 0; index < ticket.dirty_page_count; ++index) {
                ticket.dirty_pages[index] = 0;
            }
            ticket.dirty_page_count = 0;
            reopen_gates();
            coordinator_lock.unlock();
            if (++faulted_retries < FexBufferCaptureFaultedRetryLimit) {
                std::this_thread::yield();
                continue;
            }
            const u64 fallback_ordinal =
                epochs.buffer_capture_faulted_fallbacks.fetch_add(1, std::memory_order_relaxed) + 1;
            if (fallback_ordinal <= 8 || (fallback_ordinal & (fallback_ordinal - 1)) == 0) {
                __android_log_print(
                    ANDROID_LOG_WARN, "LSX4Native",
                    "[EXECUTOR_FEX_BUFFER_CAPTURE] event=faulted-fallback "
                    "count=%llu retries=%u token=0x%llx ownerTid=%d "
                    "epochAddress=0x%llx captureAddress=0x%llx size=%llu "
                    "pending=%u faulted=%u",
                    static_cast<unsigned long long>(fallback_ordinal), faulted_retries,
                    static_cast<unsigned long long>(first_faulted_token), first_faulted_owner_tid,
                    static_cast<unsigned long long>(first_faulted_address),
                    static_cast<unsigned long long>(address), static_cast<unsigned long long>(size),
                    epochs.pending.load(std::memory_order_relaxed),
                    epochs.faulted.load(std::memory_order_relaxed));
            }
            // Faulted means the guest store has not reached a proven FEX safe
            // point yet. Brief retries keep the common transient case on the
            // capture fast path; the caller's snapshot-boundary fallback owns
            // the bounded wait after the retry budget is exhausted.
            return false;
        }
        if (!has_ready || unsupported) {
            for (u32 index = 0; index < indexed_token_count; ++index) {
                ticket.tokens[index] = 0;
            }
            for (u32 index = 0; index < ticket.dirty_page_count; ++index) {
                ticket.dirty_pages[index] = 0;
            }
            ticket.dirty_page_count = 0;
            reopen_gates();
            return false;
        }

        Impl* const current_owner = Impl::owner;
        bool guarded = true;
        for (VAddr page = capture_begin; page < capture_end; page += TRACKER_BYTES_PER_PAGE) {
            const bool requires_buffer_epoch =
                std::find(ticket.dirty_pages.begin(),
                          ticket.dirty_pages.begin() + ticket.dirty_page_count,
                          page) != ticket.dirty_pages.begin() + ticket.dirty_page_count;
            if (ticket.guarded_page_count >= ticket.guarded_pages.size() ||
                !current_owner->GuardFexBufferCapturePage(page, requires_buffer_epoch,
                                                          texture_cache_lock_held)) {
                guarded = false;
                break;
            }
            ticket.guarded_pages[ticket.guarded_page_count++] = page;
        }
        if (!guarded) {
            for (u32 index = 0; index < ticket.guarded_page_count; ++index) {
                current_owner->EndFexBufferCapture(ticket.guarded_pages[index]);
                ticket.guarded_pages[index] = 0;
            }
            ticket.guarded_page_count = 0;
            for (u32 index = 0; index < ticket.dirty_page_count; ++index) {
                ticket.dirty_pages[index] = 0;
            }
            ticket.dirty_page_count = 0;
            reopen_gates();
            return false;
        }

        const auto capture_ready_slot = [&](FexWriteEpochSlot& slot) {
            u32 expected = static_cast<u32>(FexWriteEpochState::Ready);
            if (!slot.state.compare_exchange_strong(
                    expected, static_cast<u32>(FexWriteEpochState::Capturing),
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                return;
            }
            const u64 token = slot.token.load(std::memory_order_acquire);
            ASSERT_MSG(ticket.token_count < ticket.tokens.size() && token != 0,
                       "FEX buffer capture token overflow");
            ticket.tokens[ticket.token_count++] = token;
        };
        if (exact_indexed) {
            for (u32 index = 0; index < indexed_token_count; ++index) {
                const u64 token = ticket.tokens[index];
                capture_ready_slot(epochs.slots[static_cast<size_t>(
                    token & FexWriteEpochSlotMask)]);
            }
        } else {
            for (FexWriteEpochSlot& slot : epochs.slots) {
                if (overlaps(slot)) {
                    capture_ready_slot(slot);
                }
            }
        }
        ASSERT_MSG(ticket.token_count != 0 && ticket.dirty_page_count != 0,
                   "FEX buffer capture lost its ready epoch set");
        std::sort(ticket.dirty_pages.begin(), ticket.dirty_pages.begin() + ticket.dirty_page_count);
        ticket.active = true;
        epochs.active_buffer_captures.fetch_add(1, std::memory_order_release);
        const u64 capture_ordinal =
            epochs.buffer_captures.fetch_add(1, std::memory_order_relaxed) + 1;
        epochs.buffer_capture_pages.fetch_add(ticket.dirty_page_count, std::memory_order_relaxed);
        if (capture_ordinal <= 8 || (capture_ordinal & (capture_ordinal - 1)) == 0) {
            __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                                "[EXECUTOR_FEX_BUFFER_CAPTURE] captures=%llu pages=%llu "
                                "waits=%llu pending=%u",
                                static_cast<unsigned long long>(capture_ordinal),
                                static_cast<unsigned long long>(
                                    epochs.buffer_capture_pages.load(std::memory_order_relaxed)),
                                static_cast<unsigned long long>(
                                    epochs.buffer_capture_waits.load(std::memory_order_relaxed)),
                                epochs.pending.load(std::memory_order_relaxed));
        }
        // Keep the coordinator and both writer gates across the staging copy.
        // Reopening here lets a new HLE bulk writer bypass the per-page fault guard
        // and race the captured bytes. EndFexDeferredBufferCapture owns the
        // matching unlock after the snapshot has durable CPU authority.
        (void)coordinator_lock.release();
        return true;
    }
#else
    (void)address;
    (void)size;
    (void)texture_cache_lock_held;
    return false;
#endif
}

void PageManager::EndFexDeferredBufferCapture(
    const bool active, const bool texture_cache_lock_held) noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (!active) {
        return;
    }
    auto& epochs = FexWriteEpochs();
    auto& ticket = executor_fex_buffer_capture_ticket;
    ASSERT_MSG(ticket.active && ticket.token_count != 0 &&
                   ticket.guarded_page_count != 0 &&
                   ticket.dirty_page_count != 0,
               "FEX buffer capture leave without a live ticket");
    if (Impl::rasterizer != nullptr) {
        Impl::rasterizer->PublishCapturedGuestBufferWritePages(
            std::span<const VAddr>{ticket.dirty_pages.data(),
                                   ticket.dirty_page_count},
            texture_cache_lock_held);
    }
    // Begin keeps drain_mutex owned and admission closed for the entire
    // capture. No writer can enter this interval, so waiting here would only
    // reintroduce the bulk-writer/capture deadlock this boundary removes.
    Impl* const current_owner = Impl::owner;
    ASSERT_MSG(current_owner != nullptr,
               "FEX buffer capture lost PageManager owner");
    for (u32 index = 0; index < ticket.guarded_page_count; ++index) {
        current_owner->EndFexBufferCapture(ticket.guarded_pages[index]);
        ticket.guarded_pages[index] = 0;
    }
    for (u32 index = 0; index < ticket.token_count; ++index) {
        const u64 token = ticket.tokens[index];
        FexWriteEpochSlot& slot = epochs.slots[
            static_cast<size_t>(token & FexWriteEpochSlotMask)];
        ASSERT_MSG(slot.token.load(std::memory_order_acquire) == token &&
                       slot.state.load(std::memory_order_acquire) ==
                           static_cast<u32>(FexWriteEpochState::Capturing),
                   "Stale FEX buffer capture token");
        RemoveFexPendingPageRef(slot.address);
        slot.state.store(static_cast<u32>(FexWriteEpochState::Free),
                         std::memory_order_release);
        ticket.tokens[index] = 0;
    }
    epochs.pending.fetch_sub(ticket.token_count, std::memory_order_release);
    epochs.drained.fetch_add(ticket.token_count, std::memory_order_relaxed);
    for (u32 index = 0; index < ticket.dirty_page_count; ++index) {
        ticket.dirty_pages[index] = 0;
    }
    ticket.token_count = 0;
    ticket.guarded_page_count = 0;
    ticket.dirty_page_count = 0;
    ticket.active = false;
    epochs.active_buffer_captures.fetch_sub(1, std::memory_order_release);
    if (epochs.enabled.load(std::memory_order_acquire)) {
        epochs.gate_open.store(true, std::memory_order_release);
        epochs.bulk_gate_open.store(true, std::memory_order_release);
    }
    epochs.drain_mutex.unlock();
#else
    (void)active;
    (void)texture_cache_lock_held;
#endif
}

bool PageManager::BeginFexBufferSnapshotBoundary(
    const VAddr address, const u64 size) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (CurrentFexBulkLeasePhase() != ExecutorFexBulkLeasePhase::Idle) {
        return false;
    }
    auto& epochs = FexWriteEpochs();
    if (!ExecutorFexBufferCaptureAckEnabled() || size == 0 ||
        address > std::numeric_limits<VAddr>::max() - size ||
        !epochs.enabled.load(std::memory_order_acquire)) {
        return false;
    }
    for (;;) {
        epochs.drain_mutex.lock();
        if (!epochs.enabled.load(std::memory_order_acquire)) {
            epochs.drain_mutex.unlock();
            return false;
        }
        if (epochs.active_buffer_captures.load(
                std::memory_order_acquire) == 0) {
            break;
        }
        epochs.drain_mutex.unlock();
        std::this_thread::yield();
    }

    // Stop new single-store epochs first, but leave bulk HLE admission open
    // until every already-faulted instruction has reached its FEX safe point.
    // A faulted guest thread can enter memcpy/memset before the interrupt-page
    // exit is observed. Closing the bulk gate here would make that thread wait
    // for GpuComm while GpuComm waits for its Faulted token.
    epochs.gate_open.store(false, std::memory_order_release);
    const auto safe_point_deadline = std::chrono::steady_clock::now() +
                                     std::chrono::milliseconds(250);
    const VAddr snapshot_begin =
        Common::AlignDown(address, TRACKER_BYTES_PER_PAGE);
    const VAddr snapshot_end =
        Common::AlignUp(address + size, TRACKER_BYTES_PER_PAGE);
    const auto has_overlapping_faulted_epoch = [&] {
        for (const FexWriteEpochSlot& slot : epochs.slots) {
            if (slot.state.load(std::memory_order_acquire) ==
                    static_cast<u32>(FexWriteEpochState::Faulted) &&
                slot.address < snapshot_end &&
                slot.address + PAGE_SIZE > snapshot_begin) {
                return true;
            }
        }
        return false;
    };
    std::array<int, 128> kicked_owner_tids{};
    size_t kicked_owner_count = 0;
    const auto kick_overlapping_faulted_owners = [&] {
        if (executor_lsx4_android_fex_deferred_write_epoch_kick_owner ==
            nullptr) {
            return;
        }
        for (const FexWriteEpochSlot& slot : epochs.slots) {
            if (slot.state.load(std::memory_order_acquire) !=
                    static_cast<u32>(FexWriteEpochState::Faulted) ||
                slot.address >= snapshot_end ||
                slot.address + PAGE_SIZE <= snapshot_begin ||
                slot.owner_tid <= 0 ||
                std::find(kicked_owner_tids.begin(),
                          kicked_owner_tids.begin() + kicked_owner_count,
                          slot.owner_tid) !=
                    kicked_owner_tids.begin() + kicked_owner_count) {
                continue;
            }
            if (kicked_owner_count == kicked_owner_tids.size()) {
                break;
            }
            kicked_owner_tids[kicked_owner_count++] = slot.owner_tid;
            const bool kicked =
                executor_lsx4_android_fex_deferred_write_epoch_kick_owner(
                    slot.owner_tid) == 1;
            (kicked ? epochs.buffer_capture_owner_kicks
                    : epochs.buffer_capture_owner_kick_failures)
                .fetch_add(1, std::memory_order_relaxed);
        }
    };
    kick_overlapping_faulted_owners();
    while (epochs.active_armers.load(std::memory_order_acquire) != 0 ||
           has_overlapping_faulted_epoch()) {
        if (!epochs.enabled.load(std::memory_order_acquire) ||
            std::chrono::steady_clock::now() >= safe_point_deadline) {
            if (epochs.enabled.load(std::memory_order_acquire)) {
                epochs.gate_open.store(true, std::memory_order_release);
                epochs.bulk_gate_open.store(true, std::memory_order_release);
            }
            epochs.drain_mutex.unlock();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }

    epochs.gate_open.store(false, std::memory_order_release);
    epochs.bulk_gate_open.store(false, std::memory_order_release);
    DrainFexDeferredWriteEpochs(true, address, size, true);
    return true;
#else
    (void)address;
    (void)size;
    return false;
#endif
}

void PageManager::EndFexBufferSnapshotBoundary(const bool active) noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (!active) {
        return;
    }
    auto& epochs = FexWriteEpochs();
    if (epochs.enabled.load(std::memory_order_acquire)) {
        epochs.gate_open.store(true, std::memory_order_release);
        epochs.bulk_gate_open.store(true, std::memory_order_release);
    }
    epochs.drain_mutex.unlock();
#else
    (void)active;
#endif
}

void PageManager::DrainFexDeferredWriteEpochs(const bool wait_for_gpu,
                                              const VAddr overlap_address,
                                              const u64 overlap_size,
                                              const bool coordinator_locked) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    auto& epochs = FexWriteEpochs();
    const bool overlap_only = overlap_size != 0;
    if (!epochs.enabled.load(std::memory_order_acquire) &&
        (!coordinator_locked || !overlap_only ||
         (epochs.active_armers.load(std::memory_order_acquire) == 0 &&
          epochs.active_bulk_armers.load(std::memory_order_acquire) == 0 &&
          epochs.active_bulk_writers.load(std::memory_order_acquire) == 0 &&
          epochs.pending.load(std::memory_order_acquire) == 0))) {
        return;
    }
    std::unique_lock drain_lock{epochs.drain_mutex, std::defer_lock};
    if (coordinator_locked) {
    } else if (overlap_only) {
        drain_lock.lock();
    } else if (!drain_lock.try_lock()) {
        return;
    }
    epochs.gate_open.store(false, std::memory_order_release);
    epochs.bulk_gate_open.store(false, std::memory_order_release);
    SCOPE_EXIT {
        if (!coordinator_locked &&
            epochs.enabled.load(std::memory_order_acquire)) {
            epochs.gate_open.store(true, std::memory_order_release);
            epochs.bulk_gate_open.store(true, std::memory_order_release);
        }
    };

    const auto wait_deadline = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(250);
    const auto wait_watchdog = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(10);
    bool armer_watchdog_reported = false;
    u32 armer_waits = 0;
    while (epochs.active_armers.load(std::memory_order_acquire) != 0 ||
           epochs.active_bulk_armers.load(std::memory_order_acquire) != 0 ||
           epochs.active_untracked_bulk_writers.load(
               std::memory_order_acquire) != 0 ||
           (overlap_only &&
            epochs.active_bulk_writers.load(std::memory_order_acquire) != 0)) {
        if (std::chrono::steady_clock::now() >= wait_deadline) {
            if (overlap_only) {
                ASSERT_MSG(false,
                           "FEX bulk writer did not quiesce before GPU mapping change");
            }
        }
        if (!armer_watchdog_reported &&
            std::chrono::steady_clock::now() >= wait_watchdog) {
            armer_watchdog_reported = true;
            __android_log_print(
                ANDROID_LOG_WARN, "LSX4Native",
                "[EXECUTOR_FEX_WRITE_EPOCH] drain cutoff waiting for "
                "%u/%u armer(s), %u/%u untracked/total bulk writer(s)",
                epochs.active_armers.load(std::memory_order_relaxed),
                epochs.active_bulk_armers.load(std::memory_order_relaxed),
                epochs.active_untracked_bulk_writers.load(
                    std::memory_order_relaxed),
                epochs.active_bulk_writers.load(std::memory_order_relaxed));
        }
        if (++armer_waits < 128) {
#if defined(__aarch64__)
            asm volatile("yield" ::: "memory");
#else
            std::this_thread::yield();
#endif
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    }

    if (!overlap_only &&
        epochs.pending.load(std::memory_order_acquire) == 0) {
        return;
    }

    const VAddr overlap_end =
        overlap_only && overlap_size <= std::numeric_limits<VAddr>::max() -
                                           overlap_address
            ? overlap_address + overlap_size
            : std::numeric_limits<VAddr>::max();
    const auto in_scope = [&](const VAddr address) {
        return !overlap_only ||
               (address < overlap_end &&
                address + PAGE_SIZE > overlap_address);
    };

    // Regular single-store epochs become ready after the faulting instruction
    // retires. Bulk epochs instead carry a per-page writer refcount. Cut every
    // old BulkOpen token over to BulkDraining before reopening admission, then
    // wait only for the writers that actually touch those old pages. New HLE
    // copies allocate successor tokens and never join this submit's wait set.
    std::array<FexWriteEpochSlot*, FexWriteEpochSlotCount> ready_slots{};
    std::array<VAddr, FexWriteEpochSlotCount> ready_addresses{};
    std::array<bool, FexWriteEpochSlotCount> ready_is_bulk{};
    std::array<VAddr, FexWriteEpochSlotCount> faulted_addresses{};
    size_t faulted_address_count = 0;
    const auto collect_faulted = [&] {
        faulted_address_count = 0;
        for (FexWriteEpochSlot& slot : epochs.slots) {
            if (slot.state.load(std::memory_order_acquire) ==
                    static_cast<u32>(FexWriteEpochState::Faulted) &&
                in_scope(slot.address)) {
                faulted_addresses[faulted_address_count++] = slot.address;
            }
        }
    };
    collect_faulted();
    while (overlap_only && faulted_address_count != 0) {
        if (std::chrono::steady_clock::now() >= wait_deadline) {
            ASSERT_MSG(false,
                       "FEX write epoch did not reach a safe point before GPU unmap");
        }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        collect_faulted();
    }
    std::sort(faulted_addresses.begin(),
              faulted_addresses.begin() + faulted_address_count);

    size_t ready_count = 0;
    for (FexWriteEpochSlot& slot : epochs.slots) {
        if (!in_scope(slot.address)) {
            continue;
        }
        u32 state = slot.state.load(std::memory_order_acquire);
        bool is_bulk = false;
        if (state == static_cast<u32>(FexWriteEpochState::Ready)) {
            if (std::binary_search(
                    faulted_addresses.begin(),
                    faulted_addresses.begin() + faulted_address_count,
                    slot.address) ||
                !slot.state.compare_exchange_strong(
                    state, static_cast<u32>(FexWriteEpochState::Draining),
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                continue;
            }
        } else if (state == static_cast<u32>(FexWriteEpochState::BulkOpen)) {
            if (std::binary_search(
                    faulted_addresses.begin(),
                    faulted_addresses.begin() + faulted_address_count,
                    slot.address) ||
                !slot.state.compare_exchange_strong(
                    state,
                    static_cast<u32>(FexWriteEpochState::BulkDraining),
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                continue;
            }
            is_bulk = true;
            const u64 token = slot.token.load(std::memory_order_acquire);
            const size_t cache_index = static_cast<size_t>(
                (slot.address / TRACKER_BYTES_PER_PAGE) &
                FexBulkPageTokenCacheMask);
            u64 expected_token = token;
            (void)epochs.bulk_page_tokens[cache_index]
                .compare_exchange_strong(expected_token, 0,
                                         std::memory_order_acq_rel,
                                         std::memory_order_relaxed);
        } else {
            continue;
        }
        if (ready_count < ready_slots.size()) {
            ready_slots[ready_count] = &slot;
            ready_addresses[ready_count] = slot.address;
            ready_is_bulk[ready_count] = is_bulk;
            ++ready_count;
        } else {
            ASSERT_MSG(false, "FEX write epoch drain set overflow");
        }
    }

    // The cutoff is complete: every old tracked writer now owns a ref on a
    // BulkDraining slot. Admission stays closed until PublishSubmit completes:
    // successor tokens share the same backing guest bytes, so reopening before
    // the current invalidation is captured would mix two generations.
    const u64 deferred_faulted =
        faulted_address_count != 0 && !overlap_only
            ? epochs.deferred_faulted_submits.fetch_add(
                  faulted_address_count, std::memory_order_relaxed) +
                  faulted_address_count
            : epochs.deferred_faulted_submits.load(std::memory_order_relaxed);
    if (ready_count == 0) {
        return;
    }

    const auto bulk_wait_watchdog = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(10);
    bool bulk_wait_reported = false;
    u32 bulk_waits = 0;
    for (;;) {
        size_t active_bulk_slots = 0;
        u64 active_bulk_refs = 0;
        for (size_t i = 0; i < ready_count; ++i) {
            if (!ready_is_bulk[i]) {
                continue;
            }
            const u32 refs = ready_slots[i]->bulk_writers.load(
                std::memory_order_acquire);
            active_bulk_slots += refs != 0 ? 1 : 0;
            active_bulk_refs += refs;
        }
        if (active_bulk_slots == 0) {
            break;
        }
        if (!bulk_wait_reported &&
            std::chrono::steady_clock::now() >= bulk_wait_watchdog) {
            bulk_wait_reported = true;
            __android_log_print(
                ANDROID_LOG_WARN, "LSX4Native",
                "[EXECUTOR_FEX_WRITE_EPOCH] submit waiting for %zu old bulk "
                "slot(s), refs=%llu totalActive=%u",
                active_bulk_slots,
                static_cast<unsigned long long>(active_bulk_refs),
                epochs.active_bulk_writers.load(std::memory_order_relaxed));
        }
        if (++bulk_waits < 512) {
#if defined(__aarch64__)
            asm volatile("yield" ::: "memory");
#else
            std::this_thread::yield();
#endif
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    }

    std::sort(ready_addresses.begin(), ready_addresses.begin() + ready_count);
    size_t unique_count = 0;
    for (size_t i = 0; i < ready_count; ++i) {
        if (unique_count == 0 ||
            ready_addresses[i] != ready_addresses[unique_count - 1]) {
            ready_addresses[unique_count++] = ready_addresses[i];
        }
    }

    Impl* const current_owner = Impl::owner;
    size_t range_count = 0;
    if (current_owner != nullptr) {
        for (size_t i = 0; i < ready_count; ++i) {
            current_owner->ReprotectFexDeferredWrite(
                ready_slots[i]->address);
        }

        std::array<std::pair<VAddr, u64>, FexWriteEpochSlotCount> ranges{};
        for (size_t i = 0; i < unique_count;) {
            const VAddr begin = ready_addresses[i];
            VAddr end = begin + PAGE_SIZE;
            while (++i < unique_count && ready_addresses[i] == end) {
                end += PAGE_SIZE;
            }
            ranges[range_count++] = {begin, end - begin};
        }
        if (Impl::rasterizer != nullptr) {
            const auto invalidation_ranges =
                std::span<const std::pair<VAddr, u64>>{ranges.data(),
                                                       range_count};
            // A pressure submit keeps both writer gates closed until the PM4
            // task is published. Queueing the invalidation ahead of that task
            // preserves ordering without synchronously waiting for GpuComm
            // while drain_mutex is held.
            const bool synchronous_invalidation =
                wait_for_gpu &&
                !(coordinator_locked && !overlap_only &&
                  ExecutorFexBufferCaptureAckEnabled());
            if (synchronous_invalidation) {
                (void)Impl::rasterizer->InvalidateMemoryBatchFromGuestThread(
                    invalidation_ranges);
            } else {
                Impl::rasterizer->EnqueueMemoryInvalidationBatchFromGuestThread(
                    invalidation_ranges);
            }
        }
    }

    for (size_t i = 0; i < ready_count; ++i) {
        ASSERT_MSG(!ready_is_bulk[i] ||
                       ready_slots[i]->bulk_writers.load(
                           std::memory_order_acquire) == 0,
                   "Releasing active FEX bulk epoch slot");
        ready_slots[i]->bulk_writers.store(0, std::memory_order_relaxed);
        RemoveFexPendingPageRef(ready_slots[i]->address);
        ready_slots[i]->state.store(static_cast<u32>(FexWriteEpochState::Free),
                                    std::memory_order_release);
    }
    epochs.pending.fetch_sub(static_cast<u32>(ready_count),
                             std::memory_order_release);
    const u64 before = epochs.drained.fetch_add(ready_count,
                                                std::memory_order_relaxed);
    const u64 after = before + ready_count;
    if (overlap_only) {
        epochs.forced_overlap_drains.fetch_add(ready_count,
                                               std::memory_order_relaxed);
    }
    if ((before >> 12) != (after >> 12) ||
        (deferred_faulted != 0 &&
         (deferred_faulted & (deferred_faulted - 1)) == 0)) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_FEX_WRITE_EPOCH] armed=%llu drained=%llu pending=%u "
            "faulted=%u fallbacks=%llu bulkCalls=%llu bulkPages=%llu "
            "bulkPageHits=%llu "
            "protectOs=%llu protectNoops=%llu uniquePages=%zu ranges=%zu "
            "armerWaits=%u bulkWaits=%u deferredFaulted=%llu "
            "forcedOverlap=%llu ownerKicks=%llu ownerKickFailures=%llu",
            static_cast<unsigned long long>(
                epochs.armed.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(after),
            epochs.pending.load(std::memory_order_relaxed),
            epochs.faulted.load(std::memory_order_relaxed),
            static_cast<unsigned long long>(
                epochs.fallbacks.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                epochs.bulk_write_calls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                epochs.bulk_write_pages.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                epochs.bulk_page_token_hits.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                executor_readback_protect_os_count.load(
                    std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                executor_readback_protect_noop_count.load(
                    std::memory_order_relaxed)),
            unique_count, range_count, armer_waits, bulk_waits,
            static_cast<unsigned long long>(deferred_faulted),
            static_cast<unsigned long long>(
                epochs.forced_overlap_drains.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                epochs.buffer_capture_owner_kicks.load(
                    std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                epochs.buffer_capture_owner_kick_failures.load(
                    std::memory_order_relaxed)));
    }
#else
    (void)wait_for_gpu;
    (void)overlap_address;
    (void)overlap_size;
    (void)coordinator_locked;
#endif
}

void PageManager::PrepareBulkGuestRead(const VAddr address, const u64 size) {
    if (Impl::owner != nullptr) {
        Impl::owner->PrepareBulkGuestRead(address, size);
    }
}

bool PageManager::PrepareTextureGuestReadProtectedRuns(
    const VAddr address, const u64 size) {
    if (Impl::owner != nullptr) {
        return Impl::owner->PrepareTextureGuestReadProtectedRuns(address,
                                                                 size);
    }
    return false;
}

void PageManager::PrepareBulkGuestWrite(const VAddr address, const u64 size) {
    if (Impl::owner == nullptr) {
        return;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (ExecutorFexDeferredBulkGuestWritesEnabled() &&
        Impl::owner->TryPrepareFexDeferredBulkGuestWrite(address, size,
                                                         false)) {
        return;
    }
#endif
    Impl::owner->PrepareBulkGuestWrite(address, size);
}

bool PageManager::TryBeginFastBulkGuestWrite(const VAddr address,
                                             const u64 size) noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    return Impl::owner != nullptr &&
           Impl::owner->TryBeginFastBulkGuestWrite(address, size);
#else
    (void)address;
    (void)size;
    return false;
#endif
}

void PageManager::PrepareBulkGuestWriteForMemset(const VAddr address,
                                                 const u64 size) {
    if (Impl::owner == nullptr) {
        return;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (Impl::owner->TryPrepareHleCleanBufferWriteForMemset(address, size)) {
        return;
    }
    if (ExecutorFexDeferredBulkGuestWritesEnabled() &&
        Impl::owner->TryPrepareFexDeferredBulkGuestWrite(address, size,
                                                         false)) {
        return;
    }
#endif
    Impl::owner->PrepareBulkGuestWrite(address, size);
}

bool PageManager::TryPrepareFexDeferredBulkGuestWrite(
    const VAddr address, const u64 size) {
    return Impl::owner != nullptr &&
           Impl::owner->TryPrepareFexDeferredBulkGuestWrite(address, size,
                                                            false);
}

bool PageManager::TryBeginFexDeferredBulkGuestWrite(
    const VAddr address, const u64 size,
    const bool require_read_ready) noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (Impl::owner == nullptr) {
        return false;
    }
    auto& phase = CurrentFexBulkLeasePhase();
    if (phase != ExecutorFexBulkLeasePhase::Idle ||
        executor_fex_deferred_bulk_write_ticket.active ||
        executor_fex_deferred_bulk_write_ticket.count != 0 ||
        executor_fex_bulk_writer_admitted ||
        executor_tracked_bulk_write_gate_active) {
        return false;
    }

    const u64 captured_epoch = executor_fex_bulk_lease_epoch;
    bool committed = false;
    SCOPE_EXIT {
        if (!committed) {
            AbortCurrentFexBulkLeaseAfter(captured_epoch);
        }
    };

    if (Impl::owner->TryPrepareFexDeferredBulkGuestWrite(
            address, size, require_read_ready)) {
        if (!Impl::owner->TryEnterFexDeferredBulkGuestWriteActivity(
                address, size, require_read_ready)) {
            return false;
        }
        executor_fex_bulk_activity_deferred = true;
    } else {
        if (!TryAdmitCurrentUntrackedFexBulkWriterOnce(address, size) ||
            !Impl::owner->TryEnterFexDirectReadyBulkGuestWriteActivity(
                address, size, require_read_ready)) {
            return false;
        }
        executor_fex_bulk_activity_deferred = false;
    }

    ASSERT_MSG(phase == ExecutorFexBulkLeasePhase::Prepared,
               "Transactional FEX bulk lease lost its prepared phase");
    phase = ExecutorFexBulkLeasePhase::Entered;
    committed = true;
    return true;
#else
    (void)address;
    (void)size;
    (void)require_read_ready;
    return false;
#endif
}

bool PageManager::FexDeferredWriteEpochEnabled() noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    return FexWriteEpochs().enabled.load(std::memory_order_acquire);
#else
    return false;
#endif
}

FexDeferredWriteActivitySnapshot
PageManager::GetFexDeferredWriteActivitySnapshot() noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const auto& epochs = FexWriteEpochs();
    return {
        .active_armers =
            epochs.active_armers.load(std::memory_order_acquire),
        .active_bulk_armers =
            epochs.active_bulk_armers.load(std::memory_order_acquire),
        .active_bulk_writers =
            epochs.active_bulk_writers.load(std::memory_order_acquire),
        .active_untracked_bulk_writers =
            epochs.active_untracked_bulk_writers.load(std::memory_order_acquire),
    };
#else
    return {};
#endif
}

bool PageManager::IsFexGuestMemoryReadReady(
    const VAddr address, const u64 size) noexcept {
    return Impl::owner != nullptr &&
           Impl::owner->IsFexGuestMemoryReadReady(address, size);
}

u64 PageManager::DebugFexGuestMemoryLeaseState(
    const VAddr address, const u64 size,
    const bool require_read_ready) noexcept {
    return Impl::owner == nullptr
               ? (u64{1} << 7)
               : Impl::owner->DebugFexGuestMemoryLeaseState(
                     address, size, require_read_ready);
}

void PageManager::EnterBulkGuestWrite(const VAddr address, const u64 size) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (Impl::owner != nullptr &&
        Impl::owner->TryEnterHleCleanBufferWrite(address, size)) {
        return;
    }
    if (executor_unwatched_bulk_write_active) {
        ASSERT_MSG(executor_unwatched_bulk_write_address == address &&
                       executor_unwatched_bulk_write_size == size,
                   "Mismatched unwatched bulk write enter");
        return;
    }
    if (!FexWriteEpochs().enabled.load(std::memory_order_acquire)) {
        if (Impl::owner != nullptr) {
            Impl::owner->SetBulkGuestWriteActivity(address, size, true, false);
        }
        return;
    }
    auto& phase = CurrentFexBulkLeasePhase();
    if (phase == ExecutorFexBulkLeasePhase::Idle) {
        BeginCurrentFexBulkLease(address, size);
    } else {
        ASSERT_MSG(
            phase == ExecutorFexBulkLeasePhase::Prepared &&
                executor_fex_bulk_lease_address == address &&
                executor_fex_bulk_lease_size == size,
            "Mismatched FEX bulk lease enter");
    }
    if (size == 0) {
        executor_fex_bulk_activity_deferred = false;
        phase = ExecutorFexBulkLeasePhase::Entered;
        return;
    }
#endif
    if (Impl::owner != nullptr) {
        bool deferred_activity = false;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (executor_fex_deferred_bulk_write_ticket.active) {
            deferred_activity =
                Impl::owner->TryEnterFexDeferredBulkGuestWriteActivity(
                    address, size);
            if (!deferred_activity) {
                PublishAndClearCurrentFexDeferredBulkTicket();
                ReleaseCurrentFexBulkWriterAdmission();
                Impl::owner->PrepareBulkGuestWrite(address, size);
            }
        }
        if (!deferred_activity) {
            const bool bootstrap_registration =
                !Impl::owner->InitialGpuMappingRegistrationComplete();
            if (!bootstrap_registration &&
                (ExecutorFexBufferCaptureAckEnabled() ||
                 !Impl::owner->IsGpuThread()) &&
                !executor_fex_bulk_writer_admitted) {
                (void)AdmitCurrentUntrackedFexBulkWriter(address, size);
            }
            Impl::owner->SetBulkGuestWriteActivity(address, size, true, false);
        }
        executor_fex_bulk_activity_deferred = deferred_activity;
#else
        if (!deferred_activity) {
            Impl::owner->SetBulkGuestWriteActivity(address, size, true, false);
        }
#endif
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (executor_tracked_bulk_write_gate_active) {
        executor_tracked_bulk_write_activity_entered = true;
    }
    phase = ExecutorFexBulkLeasePhase::Entered;
#endif
}

void PageManager::LeaveBulkGuestWrite(const VAddr address, const u64 size) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (Impl::owner != nullptr &&
        Impl::owner->TryLeaveHleCleanBufferWrite(address, size)) {
        return;
    }
    if (Impl::owner != nullptr &&
        Impl::owner->LeaveUnwatchedBulkGuestWrite(address, size)) {
        return;
    }
    if (!FexWriteEpochs().enabled.load(std::memory_order_acquire)) {
        if (Impl::owner != nullptr) {
            Impl::owner->SetBulkGuestWriteActivity(address, size, false, false);
        }
        return;
    }
    ASSERT_MSG(CurrentFexBulkLeasePhase() != ExecutorFexBulkLeasePhase::Idle &&
                   executor_fex_bulk_lease_address == address &&
                   executor_fex_bulk_lease_size == size,
               "Mismatched FEX bulk lease leave");
    const u64 previous_epoch = executor_fex_bulk_lease_epoch - 1;
    AbortCurrentFexBulkLeaseAfter(previous_epoch);
#else
    if (Impl::owner != nullptr) {
        Impl::owner->SetBulkGuestWriteActivity(address, size, false, false);
    }
#endif
}

u64 PageManager::CaptureCurrentFexBulkLeaseEpoch() noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    return executor_fex_bulk_lease_epoch;
#else
    return 0;
#endif
}

void PageManager::AbortCurrentFexBulkLeaseAfter(
    const u64 captured_epoch) noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (Impl::owner != nullptr && executor_unwatched_bulk_write_active) {
        (void)Impl::owner->LeaveUnwatchedBulkGuestWrite(
            executor_unwatched_bulk_write_address,
            executor_unwatched_bulk_write_size);
        EndBulkGuestWrite();
    }
    auto& current_phase = CurrentFexBulkLeasePhase();
    const auto phase = current_phase;
    if (phase == ExecutorFexBulkLeasePhase::Idle &&
        executor_tracked_bulk_write_gate_active) {
        if (Impl::owner != nullptr) {
            if (executor_tracked_bulk_write_activity_entered) {
                Impl::owner->SetBulkGuestWriteActivity(
                    executor_tracked_bulk_write_address,
                    executor_tracked_bulk_write_size, false, false);
            }
            Impl::owner->LeaveTrackedBulkWriteGate();
        }
        EndBulkGuestWrite();
    }
    if (phase == ExecutorFexBulkLeasePhase::Idle ||
        phase == ExecutorFexBulkLeasePhase::Closing ||
        executor_fex_bulk_lease_epoch == captured_epoch) {
        return;
    }
    current_phase = ExecutorFexBulkLeasePhase::Closing;
    const VAddr address = executor_fex_bulk_lease_address;
    const u64 size = executor_fex_bulk_lease_size;
    const bool activity_entered =
        phase == ExecutorFexBulkLeasePhase::Entered ||
        phase == ExecutorFexBulkLeasePhase::Writing;
    if (activity_entered && Impl::owner != nullptr) {
        Impl::owner->SetBulkGuestWriteActivity(
            address, size, false,
            executor_fex_bulk_activity_deferred);
    }
    if (Impl::owner != nullptr &&
        executor_bulk_prepare_ticket.phase !=
            ExecutorBulkPreparePhase::Idle) {
        Impl::owner->CancelPreparedBulkGuestWrite(address, size);
    }
    EndBulkGuestWrite();
    PublishAndClearCurrentFexDeferredBulkTicket();

    executor_fex_bulk_lease_address = 0;
    executor_fex_bulk_lease_size = 0;
    executor_fex_bulk_activity_deferred = false;
    ReleaseCurrentFexBulkWriterAdmission();
    current_phase = ExecutorFexBulkLeasePhase::Idle;
    if (Impl::owner != nullptr) {
        Impl::owner->LeaveTrackedBulkWriteGate();
    }
#else
    (void)captured_epoch;
#endif
}

OwnedPageFaultPcStats PageManager::GetOwnedPageFaultPcStats() noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const u64 generated_buffer_write =
        executor_generated_buffer_write_fault_count.load(
            std::memory_order_relaxed);
    const u64 generated_texture_write =
        executor_generated_texture_write_fault_count.load(
            std::memory_order_relaxed);
    const u64 generated_other_write =
        executor_generated_other_write_fault_count.load(
            std::memory_order_relaxed);
    const u64 generated_buffer_texture_write =
        generated_buffer_write + generated_texture_write;
    const bool generated_buffer_texture_overflow =
        generated_buffer_texture_write < generated_buffer_write;
    const u64 generated_code_write =
        generated_buffer_texture_write + generated_other_write;
    const bool generated_code_write_saturated =
        generated_buffer_texture_overflow ||
        generated_code_write < generated_buffer_texture_write;
    return {
        .generated_code = executor_owned_generated_fault_count.load(
            std::memory_order_relaxed),
        .generated_code_write = generated_code_write,
        .host_code = executor_owned_host_fault_count.load(
            std::memory_order_relaxed),
        .unclassified = executor_owned_unclassified_fault_count.load(
            std::memory_order_relaxed),
        .generated_code_write_saturated = generated_code_write_saturated,
    };
#else
    return {};
#endif
}

u64 PageManager::DebugWriteFaultEpoch() noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    return executor_readback_write_fault_count.load(std::memory_order_acquire);
#else
    return 0;
#endif
}

bool PageManager::TryTrackJitPollWrite(
    const VAddr addr, const u64 size,
    const JitPollWritePublishCallback callback) noexcept {
    Impl* const owner = Impl::owner;
    if (owner == nullptr || callback == nullptr || size == 0 ||
        addr >= (u64{1} << Impl::ADDRESS_BITS) ||
        size > (u64{1} << Impl::ADDRESS_BITS) - addr) {
        return false;
    }
    Core::MemoryManager* const memory = Core::Memory::Instance();
    if (memory == nullptr || !memory->IsWritableMapping(addr, size)) {
        return false;
    }
    owner->SetJitPollWritePublishCallback(callback);
    return owner->UpdateJitPollWriteWatchers<true>(addr, size);
}

bool PageManager::TryUntrackJitPollWrite(const VAddr addr,
                                         const u64 size) noexcept {
    Impl* const owner = Impl::owner;
    if (owner == nullptr || size == 0 ||
        addr >= (u64{1} << Impl::ADDRESS_BITS) ||
        size > (u64{1} << Impl::ADDRESS_BITS) - addr) {
        return false;
    }
    return owner->UpdateJitPollWriteWatchers<false>(addr, size);
}

bool PageManager::TryArmDiagnosticWriteProvenance(
    const VAddr addr, const u64 size) noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    Impl* const owner = Impl::owner;
    if (owner == nullptr || size == 0 ||
        addr >= (u64{1} << Impl::ADDRESS_BITS) ||
        size > (u64{1} << Impl::ADDRESS_BITS) - addr) {
        return false;
    }
    Core::MemoryManager* const memory = Core::Memory::Instance();
    if (memory == nullptr || !memory->IsWritableMapping(addr, size)) {
        return false;
    }
    const VAddr exact_end = addr + size;
    const VAddr page_begin = Common::AlignDown(addr, PAGE_SIZE);
    const VAddr page_end = Common::AlignUp(exact_end, PAGE_SIZE);
    executor_write_provenance_exact_begin.store(addr,
                                                 std::memory_order_relaxed);
    executor_write_provenance_exact_end.store(exact_end,
                                               std::memory_order_relaxed);
    executor_write_provenance_page_end.store(page_end,
                                              std::memory_order_relaxed);
    executor_write_provenance_page_begin.store(page_begin,
                                                std::memory_order_release);
    if (!owner->UpdateJitPollWriteWatchers<true>(addr, size)) {
        executor_write_provenance_page_begin.store(0,
                                                    std::memory_order_release);
        executor_write_provenance_page_end.store(0,
                                                  std::memory_order_relaxed);
        executor_write_provenance_exact_begin.store(0,
                                                     std::memory_order_relaxed);
        executor_write_provenance_exact_end.store(0,
                                                   std::memory_order_relaxed);
        return false;
    }
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_WRITE_PROVENANCE_ARM] exactBegin=0x%llx exactEnd=0x%llx "
        "pageBegin=0x%llx pageEnd=0x%llx",
        static_cast<unsigned long long>(addr),
        static_cast<unsigned long long>(exact_end),
        static_cast<unsigned long long>(page_begin),
        static_cast<unsigned long long>(page_end));
    return true;
#else
    (void)addr;
    (void)size;
    return false;
#endif
}

void PageManager::OnGpuMap(VAddr address, size_t size,
                           Core::MemoryPermission host_permissions,
                           bool guest_cpu_read, bool guest_cpu_write) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    auto& epochs = FexWriteEpochs();
    std::unique_lock coordinator_lock{epochs.drain_mutex};
    epochs.gate_open.store(false, std::memory_order_release);
    epochs.bulk_gate_open.store(false, std::memory_order_release);
    DrainFexDeferredWriteEpochs(true, address, size, true);
#endif
    impl->OnMap(address, size, host_permissions, guest_cpu_read,
                guest_cpu_write);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (epochs.enabled.load(std::memory_order_acquire)) {
        epochs.gate_open.store(true, std::memory_order_release);
        epochs.bulk_gate_open.store(true, std::memory_order_release);
    }
#endif
}

void PageManager::OnGpuUnmap(VAddr address, size_t size) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    auto& epochs = FexWriteEpochs();
    std::unique_lock coordinator_lock{epochs.drain_mutex};
    epochs.gate_open.store(false, std::memory_order_release);
    epochs.bulk_gate_open.store(false, std::memory_order_release);
#endif
    DrainFexDeferredWriteEpochs(true, address, size, true);
    impl->OnUnmap(address, size);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (epochs.enabled.load(std::memory_order_acquire)) {
        epochs.gate_open.store(true, std::memory_order_release);
        epochs.bulk_gate_open.store(true, std::memory_order_release);
    }
#endif
}

bool PageManager::BeginGuestProtectionChange(const VAddr address,
                                             const u64 size) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    auto& epochs = FexWriteEpochs();
    if (!epochs.enabled.load(std::memory_order_acquire)) {
        return false;
    }
    ASSERT_MSG(CurrentFexBulkLeasePhase() == ExecutorFexBulkLeasePhase::Idle,
               "Guest protection cutoff entered from an active FEX bulk lease");
    ASSERT_MSG(!executor_guest_protection_change_active,
               "Nested guest protection cutoff");
    epochs.drain_mutex.lock();
    if (!epochs.enabled.load(std::memory_order_acquire)) {
        epochs.drain_mutex.unlock();
        return false;
    }
    epochs.gate_open.store(false, std::memory_order_release);
    epochs.bulk_gate_open.store(false, std::memory_order_release);
    DrainFexDeferredWriteEpochs(true, address, size, true);
    executor_guest_protection_change_active = true;
    return true;
#else
    (void)address;
    (void)size;
    return false;
#endif
}

void PageManager::EndGuestProtectionChange(const bool active) noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (!active) {
        return;
    }
    auto& epochs = FexWriteEpochs();
    ASSERT_MSG(executor_guest_protection_change_active,
               "Guest protection cutoff leave without enter");
    executor_guest_protection_change_active = false;
    if (epochs.enabled.load(std::memory_order_acquire)) {
        epochs.gate_open.store(true, std::memory_order_release);
        epochs.bulk_gate_open.store(true, std::memory_order_release);
    }
    epochs.drain_mutex.unlock();
#else
    (void)active;
#endif
}

void PageManager::UpdateGpuDirtyPages(const VAddr addr, const u64 size,
                                      const bool dirty) const {
    impl->UpdateGpuDirtyPages(addr, size, dirty);
}

bool PageManager::OnGuestProtectionChanged(
    VAddr address, size_t size, Core::MemoryPermission host_permissions,
    bool guest_cpu_read, bool guest_cpu_write) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    ASSERT_MSG(CurrentFexBulkLeasePhase() == ExecutorFexBulkLeasePhase::Idle,
               "Guest protection changed from inside an active FEX bulk lease");
    ASSERT_MSG(!FexWriteEpochs().enabled.load(std::memory_order_acquire) ||
                   executor_guest_protection_change_active,
               "Guest protection update bypassed the FEX writer cutoff");
#endif
    return impl->OnGuestProtectionChanged(address, size, host_permissions,
                                          guest_cpu_read, guest_cpu_write);
}

void PageManager::TrackPendingCompletionRead(VAddr addr, u64 size) const {
    impl->UpdatePageWatchers<true, true>(addr, size);
}

void PageManager::UntrackPendingCompletionRead(VAddr addr, u64 size) const {
    impl->UpdatePageWatchers<false, true>(addr, size);
}

void PageManager::TrackMemoryWaitWrite(VAddr addr, u64 size) const {
    impl->UpdateMemoryWaitWriteWatchers<true>(addr, size);
}

void PageManager::UntrackMemoryWaitWrite(VAddr addr, u64 size) const {
    impl->UpdateMemoryWaitWriteWatchers<false>(addr, size);
}

bool PageManager::ConsumeMemoryWaitWriteFault(VAddr addr) const {
    return impl->ConsumeMemoryWaitWriteFault(addr);
}

void BeginDescriptorProtectionCohortDiagnosticBatch() noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (!ExecutorDescriptorProtectionCohortDiagnosticEnabled()) {
        return;
    }
    auto& batch = descriptor_protection_batch;
    ASSERT_MSG(!batch.active,
               "Descriptor protection diagnostic batch overlap");
    batch.active = true;
    batch.ranges = 0;
    batch.untrack_runs.clear();
    descriptor_protection_diagnostic.batches.fetch_add(
        1, std::memory_order_relaxed);
#endif
}

void EndDescriptorProtectionCohortDiagnosticBatch() noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (!ExecutorDescriptorProtectionCohortDiagnosticEnabled()) {
        return;
    }
    auto& batch = descriptor_protection_batch;
    if (!batch.active) {
        return;
    }
    auto& counters = descriptor_protection_diagnostic;
    ExecutorDescriptorDiagnosticUpdateMax(counters.max_ranges_per_batch,
                                          batch.ranges);
    std::ranges::sort(
        batch.untrack_runs, [](const auto& left, const auto& right) {
            const u32 left_permissions = static_cast<u32>(left.permissions);
            const u32 right_permissions = static_cast<u32>(right.permissions);
            return left_permissions != right_permissions
                       ? left_permissions < right_permissions
                       : left.first_page < right.first_page;
        });
    u64 model_runs = 0;
    u64 model_pages = 0;
    size_t current_first = 0;
    size_t current_end = 0;
    Core::MemoryPermission current_permissions{};
    bool have_run = false;
    for (const auto& run : batch.untrack_runs) {
        const size_t run_end = run.first_page + run.page_count;
        if (have_run && run.permissions == current_permissions &&
            run.first_page <= current_end) {
            current_end = std::max(current_end, run_end);
            continue;
        }
        if (have_run) {
            ++model_runs;
            model_pages += current_end - current_first;
        }
        current_first = run.first_page;
        current_end = run_end;
        current_permissions = run.permissions;
        have_run = true;
    }
    if (have_run) {
        ++model_runs;
        model_pages += current_end - current_first;
    }
    counters.cohort_untrack_runs.fetch_add(model_runs,
                                           std::memory_order_relaxed);
    counters.cohort_untrack_unique_pages.fetch_add(model_pages,
                                                   std::memory_order_relaxed);
    batch.active = false;
    batch.ranges = 0;
    batch.untrack_runs.clear();
    ExecutorPublishDescriptorProtectionDiagnostic();
#endif
}

void RecordDescriptorProtectionCohortTransaction(
    const u64 write_ranges) noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (!ExecutorDescriptorProtectionCohortDiagnosticEnabled()) {
        return;
    }
    descriptor_protection_diagnostic.transactions.fetch_add(
        1, std::memory_order_relaxed);
    if (descriptor_protection_batch.active) {
        ExecutorDescriptorDiagnosticUpdateMax(
            descriptor_protection_diagnostic.max_ranges_per_batch,
            descriptor_protection_batch.ranges + write_ranges);
    }
#else
    (void)write_ranges;
#endif
}

DescriptorSnapshotTrackResult PageManager::TryTrackDescriptorSnapshotWrite(
    VAddr addr, u64 size) const {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (ExecutorDescriptorProtectionCohortDiagnosticEnabled()) {
        const u64 begin_cpu_ns = ExecutorDescriptorDiagnosticThreadCpuNow();
        const auto result = impl->TryTrackDescriptorSnapshotWrite(addr, size);
        const u64 end_cpu_ns = ExecutorDescriptorDiagnosticThreadCpuNow();
        ExecutorRecordDescriptorTrackCall(
            addr, size, result,
            end_cpu_ns >= begin_cpu_ns ? end_cpu_ns - begin_cpu_ns : 0);
        return result;
    }
#endif
    return impl->TryTrackDescriptorSnapshotWrite(addr, size);
}

bool PageManager::UntrackDescriptorSnapshotWrite(VAddr addr, u64 size) const {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (ExecutorDescriptorProtectionCohortDiagnosticEnabled()) {
        const u64 begin_cpu_ns = ExecutorDescriptorDiagnosticThreadCpuNow();
        const bool clean = impl->UntrackDescriptorSnapshotWrite(addr, size);
        const u64 end_cpu_ns = ExecutorDescriptorDiagnosticThreadCpuNow();
        ExecutorRecordDescriptorUntrackCall(
            clean, end_cpu_ns >= begin_cpu_ns ? end_cpu_ns - begin_cpu_ns : 0);
        return clean;
    }
#endif
    return impl->UntrackDescriptorSnapshotWrite(addr, size);
}

void PageManager::TrackJitPollWrite(VAddr addr, u64 size) const {
    while (!impl->UpdateJitPollWriteWatchers<true>(addr, size)) {
        std::this_thread::yield();
    }
}

void PageManager::UntrackJitPollWrite(VAddr addr, u64 size) const {
    (void)impl->UpdateJitPollWriteWatchers<false>(addr, size);
}

bool PageManager::ConsumeJitPollWriteFault(VAddr addr) const {
    return impl->ConsumeJitPollWriteFault(addr);
}

void PageManager::SetJitPollWritePublishCallback(
    const JitPollWritePublishCallback callback) const noexcept {
    impl->SetJitPollWritePublishCallback(callback);
}

template <bool track>
void PageManager::UpdatePageWatchers(VAddr addr, u64 size) const {
    impl->UpdatePageWatchers<track, false>(addr, size, 1);
}

template <bool track>
void PageManager::UpdateTexturePageWatchers(VAddr addr, u64 size) const {
    impl->UpdatePageWatchers<track, false>(addr, size, 2);
}

bool PageManager::TrackTextureResidency(const VAddr addr,
                                        const u64 size) const {
    return impl->UpdateTextureResidency<true>(addr, size);
}

void PageManager::UntrackTextureResidency(const VAddr addr,
                                          const u64 size) const {
    impl->UpdateTextureResidency<false>(addr, size);
}

bool PageManager::HleCleanBufferFastPathEnabled() const noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    return ExecutorHleCleanBufferFastPathEnabled();
#else
    return false;
#endif
}

bool PageManager::TrackHleShaderReadResidency(const VAddr addr,
                                              const u64 size) const {
    return impl->UpdateHleShaderReadResidency<true>(addr, size);
}

void PageManager::UntrackHleShaderReadResidency(const VAddr addr,
                                                const u64 size) const {
    impl->UpdateHleShaderReadResidency<false>(addr, size);
}

void PageManager::DisableHleCleanBufferFastPathFailClosed() const noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    impl->hle_clean_buffer_fail_closed.store(true, std::memory_order_release);
#endif
}

template <bool track, bool is_read>
void PageManager::UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) const {
    impl->UpdatePageWatchersForRegion<track, is_read>(base_addr, mask);
}

template void PageManager::UpdatePageWatchers<true>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<false>(VAddr addr, u64 size) const;
template void PageManager::UpdateTexturePageWatchers<true>(VAddr addr,
                                                            u64 size) const;
template void PageManager::UpdateTexturePageWatchers<false>(VAddr addr,
                                                             u64 size) const;
template void PageManager::UpdatePageWatchersForRegion<true, true>(VAddr base_addr,
                                                                   RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<true, false>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, true>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, false>(VAddr base_addr,
                                                                     RegionBits& mask) const;

}

extern "C" bool executor_funnel_track_jit_poll_write(
    const std::uint64_t address, const std::uint64_t size,
    const VideoCore::JitPollWritePublishCallback callback) noexcept {
    return VideoCore::PageManager::TryTrackJitPollWrite(address, size,
                                                        callback);
}

extern "C" bool executor_funnel_untrack_jit_poll_write(
    const std::uint64_t address, const std::uint64_t size) noexcept {
    return VideoCore::PageManager::TryUntrackJitPollWrite(address, size);
}

extern "C" bool executor_funnel_arm_diagnostic_write_provenance(
    const std::uint64_t address, const std::uint64_t size) noexcept {
    return VideoCore::PageManager::TryArmDiagnosticWriteProvenance(address,
                                                                   size);
}
