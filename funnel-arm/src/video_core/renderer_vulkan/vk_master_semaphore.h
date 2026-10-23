// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <queue>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Scheduler;

class MasterSemaphore {
public:
    explicit MasterSemaphore(const Instance& instance_);
    ~MasterSemaphore();

    [[nodiscard]] u64 CurrentTick() const noexcept {
        return current_tick.load(std::memory_order_acquire);
    }

    [[nodiscard]] u64 KnownGpuTick() const noexcept {
        return gpu_tick.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool IsDeviceLost() const noexcept {
        return device_lost.load(std::memory_order_acquire) ||
               device_terminal.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool IsFree(u64 tick) const noexcept {
        return !IsDeviceLost() && KnownGpuTick() >= tick;
    }

    [[nodiscard]] u64 NextTick() noexcept {
        return current_tick.fetch_add(1, std::memory_order_release);
    }

    [[nodiscard]] vk::Semaphore Handle() const noexcept {
        return semaphore.get();
    }

    [[nodiscard]] bool UsesTimelineSemaphore() const noexcept {
        return uses_timeline_semaphore;
    }

    // Returns a fence owned by the virtual-timeline fallback. The scheduler
    // must attach it to the queue submission that completes `tick`.
    [[nodiscard]] vk::Fence PrepareSubmitFence(u64 tick);

    void Refresh();

    void Wait(u64 tick);

    void MarkDeviceLost(vk::Result result, const char* where) noexcept;

    void MarkTerminal(vk::Result result, const char* where) noexcept;

    [[nodiscard]] static bool IsDeviceTerminal() noexcept {
        return device_terminal.load(std::memory_order_acquire);
    }

protected:
    struct LegacyFencePoint {
        u64 tick{};
        vk::UniqueFence fence;
    };

    void RefreshLegacyLocked();

    const Instance& instance;
    bool uses_timeline_semaphore{};
    vk::UniqueSemaphore semaphore;
    std::deque<LegacyFencePoint> legacy_pending_fences;
    std::vector<vk::UniqueFence> legacy_free_fences;
    std::mutex legacy_fence_mutex;
    std::condition_variable legacy_fence_cv;
    std::atomic<u64> gpu_tick{0};
    std::atomic<u64> current_tick{1};
    std::atomic_bool device_lost{false};
    static std::atomic_bool device_terminal;
    static std::atomic<u32> live_instances;
};

}
