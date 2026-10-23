// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/vulkan_driver_optimization.h"

#include <atomic>
#include <string>

#if defined(LSX4_APPLE_RUNTIME)
#include <cstdlib>
#include <cstring>
#endif

#ifdef __ANDROID__
#include <unistd.h>
#endif

namespace VideoCore {
namespace {

#if defined(LSX4_APPLE_RUNTIME)
enum class AppleDriverPolicy { Legacy, SubmitOnly, AndroidDefaults };

AppleDriverPolicy AppleVulkanDriverPolicy() noexcept {
    // Launch-only controls in this isolated candidate. No title or driver rule.
    static const AppleDriverPolicy policy = [] {
        const char* value = std::getenv("LSX4_APPLE_VULKAN_POLICY");
        if (value && std::strcmp(value, "off") == 0) return AppleDriverPolicy::Legacy;
        if (value && std::strcmp(value, "submit") == 0) return AppleDriverPolicy::SubmitOnly;
        return AppleDriverPolicy::AndroidDefaults;
    }();
    return policy;
}
#endif

std::atomic_bool enabled{false};
// Even values are active collection epochs; odd values mean reset in progress.
std::atomic<u64> stats_generation{2};
std::atomic<u64> pipeline_bind_attempts{0};
std::atomic<u64> pipeline_binds_elided{0};
std::atomic<u64> descriptor_bind_attempts{0};
std::atomic<u64> descriptor_binds_elided{0};
std::atomic<u64> push_constant_attempts{0};
std::atomic<u64> push_constants_elided{0};
std::atomic<u64> dynamic_state_commits{0};
std::atomic<u64> dynamic_state_full_replays{0};
std::atomic<u64> barriers_elided{0};
std::atomic<u64> render_scope_reuses{0};
std::atomic<u64> transfer_requests{0};
std::atomic<u64> transfer_flushes{0};
std::atomic<u64> transfer_regions{0};
std::atomic<u64> transfer_copy_calls{0};
std::atomic<u64> pm4_plan_lookups{0};
std::atomic<u64> pm4_plan_hits{0};
std::atomic<u64> barrier_requests{0};
std::atomic<u64> barrier_batches{0};
std::atomic<u64> barrier_regions{0};
std::atomic<u64> barrier_regions_merged{0};
std::atomic<u64> queue_submits{0};
std::atomic<u64> queue_submit_logical_packets{0};
std::atomic<u64> queue_submit_command_buffers{0};
std::atomic<u64> queue_submit_calls_saved{0};
std::atomic<u64> queue_submit_forced_boundaries{0};

#ifdef __ANDROID__
bool BisectionMarkerAbsent(const char* name) noexcept {
    const std::string path =
        std::string{"/data/data/app.lsx4.android/files/lsx4-home/"} + name;
    return ::access(path.c_str(), F_OK) != 0;
}
#endif

bool VulkanDriverOptimizationStatsEnabled() noexcept {
#ifdef __ANDROID__
    // Optimization decisions do not depend on these counters. Keep their
    // hot-path TLS/atomic bookkeeping off unless a diagnostic run explicitly
    // requests it before process startup.
    static const bool enabled =
        !BisectionMarkerAbsent("run-diagnostic-vulkan-optimization-stats");
    return enabled;
#elif defined(LSX4_APPLE_RUNTIME)
    if (AppleVulkanDriverPolicy() != AppleDriverPolicy::AndroidDefaults) return true;
    // Match Android's marker-free default: counters do not govern optimization.
    static const bool diagnostic = [] {
        const char* value = std::getenv("LSX4_APPLE_VULKAN_STATS");
        return value && std::strcmp(value, "1") == 0;
    }();
    return diagnostic;
#else
    return true;
#endif
}

struct LocalStats {
    u64 generation{};
    u32 events{};
    u64 pipeline_bind_attempts{};
    u64 pipeline_binds_elided{};
    u64 descriptor_bind_attempts{};
    u64 descriptor_binds_elided{};
    u64 push_constant_attempts{};
    u64 push_constants_elided{};
    u64 dynamic_state_commits{};
    u64 dynamic_state_full_replays{};
    u64 barriers_elided{};
    u64 render_scope_reuses{};
    u64 transfer_requests{};
    u64 transfer_flushes{};
    u64 transfer_regions{};
    u64 transfer_copy_calls{};
    u64 pm4_plan_lookups{};
    u64 pm4_plan_hits{};
    u64 barrier_requests{};
    u64 barrier_batches{};
    u64 barrier_regions{};
    u64 barrier_regions_merged{};
    u64 queue_submits{};
    u64 queue_submit_logical_packets{};
    u64 queue_submit_command_buffers{};
    u64 queue_submit_calls_saved{};
    u64 queue_submit_forced_boundaries{};
};

LocalStats& ThreadStats() noexcept {
    thread_local LocalStats stats{
        .generation = stats_generation.load(std::memory_order_relaxed),
    };
    return stats;
}

void FlushThreadStats(LocalStats& stats) noexcept {
    const u64 current_generation =
        stats_generation.load(std::memory_order_acquire);
    if ((current_generation & 1u) != 0 ||
        stats.generation != current_generation) {
        stats = {.generation = current_generation};
        return;
    }

    const auto accumulate = [](std::atomic<u64>& destination, const u64 value) {
        if (value != 0) {
            destination.fetch_add(value, std::memory_order_relaxed);
        }
    };
    accumulate(pipeline_bind_attempts, stats.pipeline_bind_attempts);
    accumulate(pipeline_binds_elided, stats.pipeline_binds_elided);
    accumulate(descriptor_bind_attempts, stats.descriptor_bind_attempts);
    accumulate(descriptor_binds_elided, stats.descriptor_binds_elided);
    accumulate(push_constant_attempts, stats.push_constant_attempts);
    accumulate(push_constants_elided, stats.push_constants_elided);
    accumulate(dynamic_state_commits, stats.dynamic_state_commits);
    accumulate(dynamic_state_full_replays, stats.dynamic_state_full_replays);
    accumulate(barriers_elided, stats.barriers_elided);
    accumulate(render_scope_reuses, stats.render_scope_reuses);
    accumulate(transfer_requests, stats.transfer_requests);
    accumulate(transfer_flushes, stats.transfer_flushes);
    accumulate(transfer_regions, stats.transfer_regions);
    accumulate(transfer_copy_calls, stats.transfer_copy_calls);
    accumulate(pm4_plan_lookups, stats.pm4_plan_lookups);
    accumulate(pm4_plan_hits, stats.pm4_plan_hits);
    accumulate(barrier_requests, stats.barrier_requests);
    accumulate(barrier_batches, stats.barrier_batches);
    accumulate(barrier_regions, stats.barrier_regions);
    accumulate(barrier_regions_merged, stats.barrier_regions_merged);
    accumulate(queue_submits, stats.queue_submits);
    accumulate(queue_submit_logical_packets,
               stats.queue_submit_logical_packets);
    accumulate(queue_submit_command_buffers,
               stats.queue_submit_command_buffers);
    accumulate(queue_submit_calls_saved, stats.queue_submit_calls_saved);
    accumulate(queue_submit_forced_boundaries,
               stats.queue_submit_forced_boundaries);
    stats = {.generation = current_generation};
}

void MaybeFlushThreadStats(LocalStats& stats) noexcept {
    constexpr u32 BatchSize = 256;
    if (++stats.events >= BatchSize) {
        FlushThreadStats(stats);
    }
}

} // namespace

void SetManagedVulkanDriverOptimization(const bool active) noexcept {
    enabled.store(active, std::memory_order_relaxed);
}

bool IsManagedVulkanDriverOptimization() noexcept {
    return enabled.load(std::memory_order_relaxed);
}

bool IsManagedVulkanStateDeduplication() noexcept {
#ifdef __ANDROID__
    static const bool allowed =
        BisectionMarkerAbsent("run-bisect-disable-vk-state-dedup");
    return IsManagedVulkanDriverOptimization() && allowed;
#else
    return IsManagedVulkanDriverOptimization();
#endif
}

bool IsManagedVulkanPipelineBindDeduplication() noexcept {
#ifdef __ANDROID__
    static const bool allowed =
        BisectionMarkerAbsent("run-bisect-disable-vk-pipeline-bind-dedup");
    return IsManagedVulkanStateDeduplication() && allowed;
#else
    return IsManagedVulkanStateDeduplication();
#endif
}

bool IsManagedVulkanPushConstantDeduplication() noexcept {
#ifdef __ANDROID__
    // Push constants are command-buffer state, not descriptor storage.  Keep
    // this exact-byte elision independently bisectable on Android so the
    // Qualcomm push-descriptor workaround below remains fully intact.
    static const bool allowed =
        !BisectionMarkerAbsent("run-enable-exact-vk-push-constant-dedup");
    return IsManagedVulkanStateDeduplication() && allowed;
#elif defined(LSX4_APPLE_RUNTIME)
    // Android requires an explicit experiment marker; do not inherit desktop ON.
    return AppleVulkanDriverPolicy() != AppleDriverPolicy::AndroidDefaults &&
           IsManagedVulkanStateDeduplication();
#else
    return IsManagedVulkanStateDeduplication();
#endif
}

bool IsManagedVulkanExactReadOnlyBufferBindReuse() noexcept {
#ifdef __ANDROID__
    // Candidate-only: reuse an already resolved exact read-only buffer range
    // inside one BindResources call. The rasterizer invalidates its tiny cache
    // at every writable/formatted binding and keeps descriptor submission,
    // barriers, command-buffer epochs, and resource lifetime unchanged.
    static const bool allowed =
        !BisectionMarkerAbsent("run-enable-exact-read-only-buffer-bind-reuse");
    return IsManagedVulkanDriverOptimization() && allowed;
#else
    return false;
#endif
}

bool IsManagedVulkanExactSurfaceBindBatch() noexcept {
#ifdef __ANDROID__
    // Candidate-only: one short TextureCache lock may cover consecutive
    // exact-hot surface identities in a shader stage.  The caller abandons
    // the batch before the first miss and whenever transfers, physical DRS,
    // replay/capture, or diagnostics are active.  View refresh, dirty/alias
    // handling and descriptor transitions keep using their original paths.
    static const bool allowed =
        !BisectionMarkerAbsent("run-enable-exact-surface-bind-batch");
    return IsManagedVulkanDriverOptimization() && allowed;
#else
    return false;
#endif
}

bool IsManagedVulkanDescriptorDeduplication(
    const bool profile_allows_exact_write_dedup_ab) noexcept {
#ifdef __ANDROID__
    // Re-emitting push descriptors remains the Android default. Some Qualcomm
    // drivers internally rotate push-descriptor storage and a signature-only
    // elision can leave a draw observing an older image binding for one frame.
    // Fingerprinting can also cost more than recording in descriptor-heavy
    // scenes. Admit only an exact A750/known-Turnip-build, marker-gated A/B;
    // every other route returns false before consulting managed state.
    static const bool requested =
        !BisectionMarkerAbsent("run-enable-exact-vk-descriptor-dedup-a750-turnip");
    return profile_allows_exact_write_dedup_ab && requested &&
           IsManagedVulkanStateDeduplication();
#elif defined(LSX4_APPLE_RUNTIME)
    (void)profile_allows_exact_write_dedup_ab;
    // Android's exact capability+marker experiment is not a portable default.
    return AppleVulkanDriverPolicy() != AppleDriverPolicy::AndroidDefaults &&
           IsManagedVulkanStateDeduplication();
#else
    (void)profile_allows_exact_write_dedup_ab;
    return IsManagedVulkanStateDeduplication();
#endif
}

bool IsManagedVulkanVertexBindDeduplication() noexcept {
#ifdef __ANDROID__
    static const bool allowed =
        BisectionMarkerAbsent("run-bisect-disable-vk-vertex-bind-dedup");
    return IsManagedVulkanStateDeduplication() && allowed;
#else
    return IsManagedVulkanStateDeduplication();
#endif
}

bool IsManagedVulkanTransferCoalescing() noexcept {
#ifdef __ANDROID__
    static const bool allowed =
        BisectionMarkerAbsent("run-bisect-disable-vk-transfer-coalescing");
    return IsManagedVulkanDriverOptimization() && allowed;
#else
    return IsManagedVulkanDriverOptimization();
#endif
}

bool IsManagedVulkanHleTransferDeferral() noexcept {
#ifdef __ANDROID__
    static const bool enabled =
        !BisectionMarkerAbsent("run-bisect-enable-vk-hle-transfer-deferral");
    return IsManagedVulkanTransferCoalescing() && enabled;
#else
    return false;
#endif
}

bool IsManagedVulkanSubmitCoalescing() noexcept {
#ifdef __ANDROID__
    // Internal FlushPlain packets are allowed to stop before reaching the
    // batch limit.  Publishing them from a timer thread races command-pool
    // recording on mobile drivers, while leaving them queued can deadlock a
    // guest that waits through an HLE path which does not call Scheduler::Wait.
    // Keep the optimization quarantined until batching has an owner-thread
    // progress contract.  State/bind/transfer deduplication remains enabled.
    return false;
#elif defined(LSX4_APPLE_RUNTIME)
    // Carry the Android quarantine across the bridge: a deferred FlushPlain can
    // strand CPU-visible completion while the guest is waiting for its result.
    // Other bind/state/transfer optimizations retain their original decisions.
    return AppleVulkanDriverPolicy() == AppleDriverPolicy::Legacy &&
           IsManagedVulkanDriverOptimization();
#else
    return IsManagedVulkanDriverOptimization();
#endif
}

bool IsManagedVulkanRenderScopeReuse() noexcept {
#ifdef __ANDROID__
    static const bool allowed =
        BisectionMarkerAbsent("run-bisect-disable-vk-render-scope-reuse");
    return IsManagedVulkanDriverOptimization() && allowed;
#else
    return IsManagedVulkanDriverOptimization();
#endif
}

void RecordVulkanPipelineBind(const bool elided) noexcept {
    if (!VulkanDriverOptimizationStatsEnabled()) {
        return;
    }
    auto& stats = ThreadStats();
    ++stats.pipeline_bind_attempts;
    if (elided) {
        ++stats.pipeline_binds_elided;
    }
    MaybeFlushThreadStats(stats);
}

void RecordVulkanDescriptorBind(const bool elided) noexcept {
    if (!VulkanDriverOptimizationStatsEnabled()) {
        return;
    }
    auto& stats = ThreadStats();
    ++stats.descriptor_bind_attempts;
    if (elided) {
        ++stats.descriptor_binds_elided;
    }
    MaybeFlushThreadStats(stats);
}

void RecordVulkanPushConstants(const bool elided) noexcept {
    if (!VulkanDriverOptimizationStatsEnabled()) {
        return;
    }
    auto& stats = ThreadStats();
    ++stats.push_constant_attempts;
    if (elided) {
        ++stats.push_constants_elided;
    }
    MaybeFlushThreadStats(stats);
}

void RecordVulkanDynamicState(const bool full_replay) noexcept {
    if (!VulkanDriverOptimizationStatsEnabled()) {
        return;
    }
    auto& stats = ThreadStats();
    ++stats.dynamic_state_commits;
    if (full_replay) {
        ++stats.dynamic_state_full_replays;
    }
    MaybeFlushThreadStats(stats);
}

void RecordVulkanBarrierElision() noexcept {
    if (!VulkanDriverOptimizationStatsEnabled()) {
        return;
    }
    auto& stats = ThreadStats();
    ++stats.barriers_elided;
    MaybeFlushThreadStats(stats);
}

void RecordVulkanRenderScopeReuse() noexcept {
    if (!VulkanDriverOptimizationStatsEnabled()) {
        return;
    }
    auto& stats = ThreadStats();
    ++stats.render_scope_reuses;
    MaybeFlushThreadStats(stats);
}

void RecordVulkanTransferRequest(const u64 regions) noexcept {
    if (!VulkanDriverOptimizationStatsEnabled()) {
        return;
    }
    auto& stats = ThreadStats();
    ++stats.transfer_requests;
    stats.transfer_regions += regions;
    MaybeFlushThreadStats(stats);
}

void RecordVulkanTransferFlush(const u64 regions, const u64 copy_calls) noexcept {
    if (!VulkanDriverOptimizationStatsEnabled()) {
        return;
    }
    auto& stats = ThreadStats();
    ++stats.transfer_flushes;
    stats.transfer_copy_calls += copy_calls;
    (void)regions;
    MaybeFlushThreadStats(stats);
}

void RecordVulkanPm4Plan(const bool hit) noexcept {
    if (!VulkanDriverOptimizationStatsEnabled()) {
        return;
    }
    auto& stats = ThreadStats();
    ++stats.pm4_plan_lookups;
    if (hit) {
        ++stats.pm4_plan_hits;
    }
    MaybeFlushThreadStats(stats);
}

void RecordVulkanBarrierBatch(const u64 requested_regions,
                              const u64 emitted_regions) noexcept {
    if (!VulkanDriverOptimizationStatsEnabled()) {
        return;
    }
    auto& stats = ThreadStats();
    ++stats.barrier_requests;
    ++stats.barrier_batches;
    stats.barrier_regions += emitted_regions;
    stats.barrier_regions_merged +=
        requested_regions > emitted_regions ? requested_regions - emitted_regions : 0;
    MaybeFlushThreadStats(stats);
}

void RecordVulkanQueueSubmit(const u64 logical_packets,
                             const u64 command_buffers,
                             const bool forced_boundary) noexcept {
    if (!VulkanDriverOptimizationStatsEnabled()) {
        return;
    }
    auto& stats = ThreadStats();
    ++stats.queue_submits;
    stats.queue_submit_logical_packets += logical_packets;
    stats.queue_submit_command_buffers += command_buffers;
    stats.queue_submit_calls_saved +=
        logical_packets > 1 ? logical_packets - 1 : 0;
    if (forced_boundary) {
        ++stats.queue_submit_forced_boundaries;
    }
    MaybeFlushThreadStats(stats);
}

VulkanDriverOptimizationStats GetVulkanDriverOptimizationStats() noexcept {
    return {
        .enabled = IsManagedVulkanDriverOptimization() ? 1ULL : 0ULL,
        .pipeline_bind_attempts =
            pipeline_bind_attempts.load(std::memory_order_relaxed),
        .pipeline_binds_elided =
            pipeline_binds_elided.load(std::memory_order_relaxed),
        .descriptor_bind_attempts =
            descriptor_bind_attempts.load(std::memory_order_relaxed),
        .descriptor_binds_elided =
            descriptor_binds_elided.load(std::memory_order_relaxed),
        .push_constant_attempts =
            push_constant_attempts.load(std::memory_order_relaxed),
        .push_constants_elided =
            push_constants_elided.load(std::memory_order_relaxed),
        .dynamic_state_commits =
            dynamic_state_commits.load(std::memory_order_relaxed),
        .dynamic_state_full_replays =
            dynamic_state_full_replays.load(std::memory_order_relaxed),
        .barriers_elided = barriers_elided.load(std::memory_order_relaxed),
        .render_scope_reuses =
            render_scope_reuses.load(std::memory_order_relaxed),
        .transfer_requests = transfer_requests.load(std::memory_order_relaxed),
        .transfer_flushes = transfer_flushes.load(std::memory_order_relaxed),
        .transfer_regions = transfer_regions.load(std::memory_order_relaxed),
        .transfer_copy_calls =
            transfer_copy_calls.load(std::memory_order_relaxed),
        .pm4_plan_lookups = pm4_plan_lookups.load(std::memory_order_relaxed),
        .pm4_plan_hits = pm4_plan_hits.load(std::memory_order_relaxed),
        .barrier_requests = barrier_requests.load(std::memory_order_relaxed),
        .barrier_batches = barrier_batches.load(std::memory_order_relaxed),
        .barrier_regions = barrier_regions.load(std::memory_order_relaxed),
        .barrier_regions_merged =
            barrier_regions_merged.load(std::memory_order_relaxed),
        .queue_submits = queue_submits.load(std::memory_order_relaxed),
        .queue_submit_logical_packets =
            queue_submit_logical_packets.load(std::memory_order_relaxed),
        .queue_submit_command_buffers =
            queue_submit_command_buffers.load(std::memory_order_relaxed),
        .queue_submit_calls_saved =
            queue_submit_calls_saved.load(std::memory_order_relaxed),
        .queue_submit_forced_boundaries =
            queue_submit_forced_boundaries.load(std::memory_order_relaxed),
    };
}

void ResetVulkanDriverOptimizationStats() noexcept {
    // In-flight per-thread batches from the previous A/B epoch are discarded
    // on their next flush instead of leaking into the reset counters.
    stats_generation.fetch_add(1, std::memory_order_acq_rel);
    pipeline_bind_attempts.store(0, std::memory_order_relaxed);
    pipeline_binds_elided.store(0, std::memory_order_relaxed);
    descriptor_bind_attempts.store(0, std::memory_order_relaxed);
    descriptor_binds_elided.store(0, std::memory_order_relaxed);
    push_constant_attempts.store(0, std::memory_order_relaxed);
    push_constants_elided.store(0, std::memory_order_relaxed);
    dynamic_state_commits.store(0, std::memory_order_relaxed);
    dynamic_state_full_replays.store(0, std::memory_order_relaxed);
    barriers_elided.store(0, std::memory_order_relaxed);
    render_scope_reuses.store(0, std::memory_order_relaxed);
    transfer_requests.store(0, std::memory_order_relaxed);
    transfer_flushes.store(0, std::memory_order_relaxed);
    transfer_regions.store(0, std::memory_order_relaxed);
    transfer_copy_calls.store(0, std::memory_order_relaxed);
    pm4_plan_lookups.store(0, std::memory_order_relaxed);
    pm4_plan_hits.store(0, std::memory_order_relaxed);
    barrier_requests.store(0, std::memory_order_relaxed);
    barrier_batches.store(0, std::memory_order_relaxed);
    barrier_regions.store(0, std::memory_order_relaxed);
    barrier_regions_merged.store(0, std::memory_order_relaxed);
    queue_submits.store(0, std::memory_order_relaxed);
    queue_submit_logical_packets.store(0, std::memory_order_relaxed);
    queue_submit_command_buffers.store(0, std::memory_order_relaxed);
    queue_submit_calls_saved.store(0, std::memory_order_relaxed);
    queue_submit_forced_boundaries.store(0, std::memory_order_relaxed);
    stats_generation.fetch_add(1, std::memory_order_release);
}

} // namespace VideoCore
