// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <limits>
#ifdef __ANDROID__
#include <android/log.h>
#include <atomic>
#include <chrono>
#include <unistd.h>
#endif
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"

#include "common/assert.h"

namespace Vulkan {

constexpr u64 WAIT_TIMEOUT = std::numeric_limits<u64>::max();
#ifdef __ANDROID__
constexpr u64 ANDROID_WAIT_SLICE = 250'000'000ULL;

// Declared in vk_scheduler.cpp. Keep the terminal path independent of the
// scheduler header (which itself owns a MasterSemaphore) while still sealing
// the last recorded Vulkan operations when device loss is first observed by a
// timeline query rather than queueSubmit.
void ExecutorVkJournalDump(const char* reason, u64 failing_tick) noexcept;

bool ExecutorTimelineWaitStatsEnabled() {
    static const bool enabled =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-diagnostic-vk-timeline-wait-stats",
                 F_OK) == 0;
    return enabled;
}
#endif

std::atomic_bool MasterSemaphore::device_terminal{false};
std::atomic<u32> MasterSemaphore::live_instances{0};

MasterSemaphore::MasterSemaphore(const Instance& instance_) : instance{instance_} {
    if (live_instances.fetch_add(1, std::memory_order_acq_rel) == 0) {
        device_terminal.store(false, std::memory_order_release);
    }
    uses_timeline_semaphore = instance.IsTimelineSemaphoreSupported();
    if (!uses_timeline_semaphore) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                            "[EXECUTOR_VK_TIMELINE_FALLBACK] mode=fence_virtual_timeline");
#endif
        return;
    }
    const vk::StructureChain semaphore_chain = {
        vk::SemaphoreCreateInfo{},
        vk::SemaphoreTypeCreateInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        },
    };
    auto [semaphore_result, sem] =
        instance.GetDevice().createSemaphoreUnique(semaphore_chain.get());
    ASSERT_MSG(semaphore_result == vk::Result::eSuccess, "Failed to create master semaphore: {}",
               vk::to_string(semaphore_result));
    semaphore = std::move(sem);
}

MasterSemaphore::~MasterSemaphore() {
    live_instances.fetch_sub(1, std::memory_order_acq_rel);
}

void MasterSemaphore::MarkDeviceLost(vk::Result result, const char* where) noexcept {
    if (result != vk::Result::eErrorDeviceLost) {
        return;
    }

    MarkTerminal(result, where);
}

void MasterSemaphore::MarkTerminal(vk::Result result, const char* where) noexcept {
    device_lost.store(true, std::memory_order_release);
    legacy_fence_cv.notify_all();

    bool expected = false;
    if (device_terminal.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
        const u64 failing_tick = CurrentTick();
#ifdef __ANDROID__
        const std::string result_name = vk::to_string(result);
        __android_log_print(ANDROID_LOG_ERROR, "LSX4Native",
                            "[EXECUTOR_VK_TERMINAL_ORIGIN] where=%s result=%d name=%s", where,
                            static_cast<int>(result), result_name.c_str());
#endif
        LOG_ERROR(Render_Vulkan, "[EXECUTOR_VK_TERMINAL] where={} result={} terminal=1", where,
                  vk::to_string(result));
        if (result == vk::Result::eErrorDeviceLost) {
#ifdef __ANDROID__
            ExecutorVkJournalDump(where, failing_tick);
#endif
            instance.DumpDeviceFaultInfo(where, failing_tick);
        }
    }
}

vk::Fence MasterSemaphore::PrepareSubmitFence(const u64 tick) {
    if (uses_timeline_semaphore || IsDeviceLost()) {
        return {};
    }

    std::scoped_lock lock{legacy_fence_mutex};
    vk::UniqueFence fence;
    if (!legacy_free_fences.empty()) {
        fence = std::move(legacy_free_fences.back());
        legacy_free_fences.pop_back();
        const vk::Result reset_result = instance.GetDevice().resetFences(*fence);
        if (reset_result != vk::Result::eSuccess) {
            MarkTerminal(reset_result, "legacy_timeline_reset_fence");
            return {};
        }
    } else {
        auto [create_result, created_fence] =
            instance.GetDevice().createFenceUnique(vk::FenceCreateInfo{});
        if (create_result != vk::Result::eSuccess) {
            MarkTerminal(create_result, "legacy_timeline_create_fence");
            return {};
        }
        fence = std::move(created_fence);
    }
    const vk::Fence handle = *fence;
    legacy_pending_fences.push_back(LegacyFencePoint{tick, std::move(fence)});
    legacy_fence_cv.notify_all();
    return handle;
}

void MasterSemaphore::RefreshLegacyLocked() {
    u64 completed_tick = gpu_tick.load(std::memory_order_acquire);
    while (!legacy_pending_fences.empty()) {
        auto& point = legacy_pending_fences.front();
        const vk::Result status = instance.GetDevice().getFenceStatus(*point.fence);
        if (status == vk::Result::eNotReady) {
            break;
        }
        if (status != vk::Result::eSuccess) {
            MarkTerminal(status, "legacy_timeline_fence_status");
            return;
        }
        completed_tick = std::max(completed_tick, point.tick);
        legacy_free_fences.push_back(std::move(point.fence));
        legacy_pending_fences.pop_front();
    }
    gpu_tick.store(completed_tick, std::memory_order_release);
    legacy_fence_cv.notify_all();
}

void MasterSemaphore::Refresh() {
    if (IsDeviceLost()) {
        return;
    }

    if (!uses_timeline_semaphore) {
        std::scoped_lock lock{legacy_fence_mutex};
        RefreshLegacyLocked();
        return;
    }

    u64 this_tick{};
    u64 counter{};
    do {
        this_tick = gpu_tick.load(std::memory_order_acquire);
        auto [counter_result, cntr] = instance.GetDevice().getSemaphoreCounterValue(*semaphore);
        if (counter_result != vk::Result::eSuccess) [[unlikely]] {
            MarkTerminal(counter_result, "timeline_counter");
            if (counter_result != vk::Result::eErrorDeviceLost) {
                LOG_ERROR(Render_Vulkan, "Failed to get master semaphore value: {}",
                          vk::to_string(counter_result));
            }
            return;
        }
        counter = cntr;
        if (counter < this_tick) {
            return;
        }
    } while (!gpu_tick.compare_exchange_weak(this_tick, counter, std::memory_order_release,
                                             std::memory_order_relaxed));
}

void MasterSemaphore::Wait(u64 tick) {
    if (IsDeviceLost()) {
        return;
    }

    if (IsFree(tick)) {
        return;
    }
    Refresh();
    if (IsDeviceLost() || IsFree(tick)) {
        return;
    }

    if (!uses_timeline_semaphore) {
        std::unique_lock lock{legacy_fence_mutex};
        while (!IsDeviceLost() && gpu_tick.load(std::memory_order_acquire) < tick) {
            RefreshLegacyLocked();
            if (IsDeviceLost() || gpu_tick.load(std::memory_order_acquire) >= tick) {
                break;
            }
            const auto point = std::find_if(
                legacy_pending_fences.begin(), legacy_pending_fences.end(),
                [tick](const LegacyFencePoint& entry) { return entry.tick >= tick; });
            if (point == legacy_pending_fences.end()) {
                legacy_fence_cv.wait_for(lock, std::chrono::milliseconds{250});
                continue;
            }
#ifdef __ANDROID__
            const u64 timeout = ANDROID_WAIT_SLICE;
#else
            const u64 timeout = WAIT_TIMEOUT;
#endif
            const vk::Result wait_result =
                instance.GetDevice().waitForFences(*point->fence, true, timeout);
            if (wait_result == vk::Result::eTimeout) {
                continue;
            }
            if (wait_result != vk::Result::eSuccess) {
                MarkTerminal(wait_result, "legacy_timeline_fence_wait");
                return;
            }
            RefreshLegacyLocked();
        }
        return;
    }

    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &semaphore.get(),
        .pValues = &tick,
    };

    vk::Result wait_result{};
#ifdef __ANDROID__
    const bool executor_wait_stats_enabled = ExecutorTimelineWaitStatsEnabled();
    const auto executor_wait_begin = executor_wait_stats_enabled
                                         ? std::chrono::steady_clock::now()
                                         : std::chrono::steady_clock::time_point{};
    u32 executor_timeout_slices = 0;
    do {
        wait_result = instance.GetDevice().waitSemaphores(&wait_info, ANDROID_WAIT_SLICE);
        if (wait_result == vk::Result::eTimeout) {
            ++executor_timeout_slices;
            if (executor_timeout_slices == 1 ||
                (executor_timeout_slices & 3u) == 0u) {
                __android_log_print(
                    ANDROID_LOG_ERROR, "LSX4Native",
                    "[EXECUTOR_VK_TIMELINE_STALL] tid=%d target=%llu "
                    "current=%llu knownGpu=%llu slices=%u caller=%p",
                    static_cast<int>(gettid()),
                    static_cast<unsigned long long>(tick),
                    static_cast<unsigned long long>(CurrentTick()),
                    static_cast<unsigned long long>(KnownGpuTick()),
                    executor_timeout_slices,
                    __builtin_return_address(0));
            }
        }
    } while (wait_result == vk::Result::eTimeout);
    if (executor_wait_stats_enabled) {
        const u64 executor_wait_ns = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - executor_wait_begin)
                .count());
        static std::atomic<u64> executor_wait_samples{};
        static std::atomic<u64> executor_wait_total_ns{};
        static std::atomic<u64> executor_wait_max_ns{};
        executor_wait_total_ns.fetch_add(executor_wait_ns, std::memory_order_relaxed);
        u64 observed_max = executor_wait_max_ns.load(std::memory_order_relaxed);
        while (observed_max < executor_wait_ns &&
               !executor_wait_max_ns.compare_exchange_weak(
                   observed_max, executor_wait_ns, std::memory_order_relaxed)) {
        }
        const u64 wait_sample =
            executor_wait_samples.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((wait_sample & 1023u) == 0u) {
            const u64 total_ns =
                executor_wait_total_ns.exchange(0, std::memory_order_relaxed);
            const u64 max_ns =
                executor_wait_max_ns.exchange(0, std::memory_order_relaxed);
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_VK_TIMELINE_WAIT] samples=1024 totalMs=%.3f avgUs=%.3f maxMs=%.3f",
                static_cast<double>(total_ns) / 1'000'000.0,
                static_cast<double>(total_ns) / 1'024'000.0,
                static_cast<double>(max_ns) / 1'000'000.0);
        }
    }
#else
    wait_result = instance.GetDevice().waitSemaphores(&wait_info, WAIT_TIMEOUT);
#endif
    if (wait_result != vk::Result::eSuccess) [[unlikely]] {
        MarkTerminal(wait_result, "timeline_wait");
        if (wait_result != vk::Result::eErrorDeviceLost) {
            LOG_ERROR(Render_Vulkan, "Failed waiting for master semaphore: {}",
                      vk::to_string(wait_result));
        }
        return;
    }
    Refresh();
}

}
