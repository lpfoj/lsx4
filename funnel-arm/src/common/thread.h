// SPDX-FileCopyrightText: 2013 Dolphin Emulator Project
// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include "common/types.h"

namespace Common {

enum class ThreadPriority : u32 {
    Low = 0,
    Normal = 1,
    High = 2,
    VeryHigh = 3,
    Critical = 4,
};

enum class ThreadAffinityClass : u32 {
    LatencyPerformance = 0,
    GuestCompute = 1,
    GraphicsDriver = 2,
    // The primary guest thread is kept distinct from other latency work. Some
    // Android kernels make a singleton affinity request sticky for every
    // pthread subsequently created by that thread.
    PrimaryGuest = 3,
};

// Captures the Android process' original allowed CPU mask before any emulator
// thread narrows its own affinity. It is a no-op on other platforms.
void InitializeAndroidThreadAffinity();

// True only when Android exposes both the SM8650 SoC identity and an allowed
// Arm Cortex-X4 CPU (implementer 0x41, part 0xD82). Other Android hosts and all
// non-Android builds return false.
bool IsSnapdragon8Gen3Host();

// True only when Android reports the exact SM8850 SoC identity used by the
// Snapdragon 8 Gen 5 route. Other Android hosts and all non-Android builds
// return false.
bool IsSnapdragon8Gen5Host();

// Process-immutable policy materialized by the Android launcher before guest
// threads are created. Values match the public CPU-tuning profile ABI.
void ConfigureAndroidThreadScheduler(u32 scheduler_mode, u32 preferred_heavy_threads);

// Applies a topology-derived subset of the captured Android CPU mask to the
// calling thread. Returns false when topology discovery is unavailable or the
// kernel rejects the request; both cases leave execution otherwise unchanged.
bool ApplyCurrentThreadAffinity(ThreadAffinityClass affinity_class);

void SetCurrentThreadRealtime(std::chrono::nanoseconds period_ns);

void SetCurrentThreadPriority(ThreadPriority new_priority);

void SetCurrentThreadName(const char* name);

void SetThreadName(void* thread, const char* name);

bool AccurateSleep(std::chrono::nanoseconds duration, std::chrono::nanoseconds* remaining,
                   bool interruptible);

class AccurateTimer {
    std::chrono::nanoseconds target_interval{};
    std::chrono::nanoseconds total_wait{};

    std::chrono::high_resolution_clock::time_point start_time;

public:
    explicit AccurateTimer(std::chrono::nanoseconds target_interval);

    void Start();

    void End();

    std::chrono::nanoseconds GetTotalWait() const {
        return total_wait;
    }
};

std::string GetCurrentThreadName();

}
