// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>

namespace Common {

// Runtime-facing subset of the Android CPU profile.  It intentionally uses
// fixed-width scalar fields so funnel-arm can also be built independently of
// the launcher/executor ABI headers.
struct CpuTuningPolicy {
    std::uint32_t function_replacements{1};
    std::uint32_t guest_clock_percent{100};
    std::uint32_t cycle_skip{};
    std::uint32_t idle_loop_skip{1};
    std::uint32_t wait_loop_detection{1};
    std::uint32_t spin_loop_detection{1};
    std::uint32_t fast_event_timing{1};
    std::uint32_t accurate_dma_timing{};
    std::uint32_t memory_exceptions{1};
    std::uint32_t ignore_bad_accesses{};
    std::uint32_t cpu_gpu_dual_thread{1};
    std::uint32_t subsystem_multithreading{1};
    std::uint32_t separate_io_thread{1};
    std::uint32_t separate_audio_hle_thread{1};
    std::uint32_t sync_on_idle_skip{1};
    std::uint32_t cpu_gpu_sync{1};
    std::uint32_t force_real_clock_sync{};
    std::uint32_t hle_mode{};
    std::uint32_t hle_fast_timers{1};
};

enum class CpuTuningEvent : std::size_t {
    IdleYields,
    WaitParks,
    HleFastTimers,
    HleAccurateTimers,
    DmaDelays,
    CpuGpuSyncs,
    Count,
};

inline std::array<std::atomic_uint64_t,
                  static_cast<std::size_t>(CpuTuningEvent::Count)>&
CpuTuningEventCounters() {
    static std::array<std::atomic_uint64_t,
                      static_cast<std::size_t>(CpuTuningEvent::Count)> counters{};
    return counters;
}

inline void NoteCpuTuningEvent(const CpuTuningEvent event,
                               const std::uint64_t amount = 1) {
    CpuTuningEventCounters()[static_cast<std::size_t>(event)].fetch_add(
        amount, std::memory_order_relaxed);
}

inline std::uint64_t CpuTuningEventCount(const CpuTuningEvent event) {
    return CpuTuningEventCounters()[static_cast<std::size_t>(event)].load(
        std::memory_order_relaxed);
}

inline CpuTuningPolicy& MutableCpuTuningPolicy() {
    static CpuTuningPolicy policy;
    return policy;
}

inline std::atomic_bool& CpuTuningPolicyPublished() {
    static std::atomic_bool published{};
    return published;
}

inline void ConfigureCpuTuningPolicy(const CpuTuningPolicy& policy) {
    MutableCpuTuningPolicy() = policy;
    CpuTuningPolicyPublished().store(true, std::memory_order_release);
}

inline CpuTuningPolicy GetCpuTuningPolicy() {
    (void)CpuTuningPolicyPublished().load(std::memory_order_acquire);
    return MutableCpuTuningPolicy();
}

constexpr std::uint32_t CpuCycleSkipDivisor(const CpuTuningPolicy& policy) {
    if (policy.force_real_clock_sync != 0) return 0;
    switch (policy.cycle_skip) {
    case 1: return 16;
    case 2: return 8;
    case 3: return 4;
    default: return 0;
    }
}

constexpr std::uint64_t ScaleGuestDuration(const std::uint64_t value,
                                           const CpuTuningPolicy& policy) {
    if (policy.force_real_clock_sync != 0) return value;
    const auto divisor = CpuCycleSkipDivisor(policy);
    // The GameDB/default profile uses a real 100% guest clock with cycle skip
    // disabled.  Preserve that exact identity before entering the two integer
    // divisions below; time-query HLEs call this path tens of millions of
    // times in Bloodborne.
    if (policy.guest_clock_percent == 100 && divisor == 0) return value;
    std::uint64_t numerator = policy.guest_clock_percent;
    std::uint64_t denominator = 100;
    if (divisor != 0) {
        numerator *= divisor + 1;
        denominator *= divisor;
    }
    const auto quotient = value / denominator;
    const auto remainder = value % denominator;
    return quotient * numerator + remainder * numerator / denominator;
}

static_assert(ScaleGuestDuration(0, CpuTuningPolicy{}) == 0);
static_assert(ScaleGuestDuration(1, CpuTuningPolicy{}) == 1);
static_assert(ScaleGuestDuration(~std::uint64_t{}, CpuTuningPolicy{}) ==
              ~std::uint64_t{});

} // namespace Common
