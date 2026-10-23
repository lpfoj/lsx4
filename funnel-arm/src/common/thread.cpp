// SPDX-FileCopyrightText: 2013 Dolphin Emulator Project
// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>

#include "core/libraries/kernel/threads/pthread.h"

#include "common/error.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "ntapi.h"
#ifdef __ANDROID__
#include <android/log.h>
#include <sys/system_properties.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#elif defined(_WIN32)
#include <windows.h>
#include "common/string_util.h"
#else
#if defined(__Bitrig__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <pthread_np.h>
#else
#include <pthread.h>
#endif
#include <sched.h>
#endif
#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef __FreeBSD__
#define cpu_set_t cpuset_t
#endif

namespace Common {

#ifdef __ANDROID__
namespace {

constexpr const char* AndroidThreadAffinityLogTag = "LSX4Native";

// Four CPUs is the smallest pool that still lets the guest workers, the audio
// mixer and the JIT background compilers run without contending with each
// other once the latency and graphics CPUs have been reserved.
constexpr int kMinimumComputeCpus = 4;

std::atomic_uint32_t g_android_scheduler_mode{3};
std::atomic_uint32_t g_android_preferred_heavy_threads{4};

// Same bisection-marker convention as the Vulkan driver optimization path:
// the marker is absent in normal runs, and dropping the file into lsx4-home
// restores the previous behaviour for an A/B without rebuilding.
bool GuestComputeYieldsGraphicsCpu() noexcept {
    static const bool yields = [] {
        const std::string path{
            "/data/data/app.lsx4.android/files/lsx4-home/"
            "run-bisect-guest-compute-keeps-graphics-cpu"};
        return ::access(path.c_str(), F_OK) != 0;
    }();
    return yields;
}

struct AndroidCpuTopology {
    cpu_set_t baseline_allowed{};
    cpu_set_t prime_cpus{};
    cpu_set_t performance_cpus{};
    cpu_set_t latency_cpus{};
    cpu_set_t graphics_cpus{};
    cpu_set_t compute_cpus{};
    std::array<std::uint64_t, CPU_SETSIZE> scores{};
    const char* score_source{"none"};
    std::uint64_t minimum_score{};
    std::uint64_t maximum_score{};
    int baseline_count{};
    int performance_count{};
    int prime_count{};
    int latency_count{};
    int graphics_count{};
    int compute_count{};
    bool cortex_x4{};
    bool snapdragon_8_gen_3{};
    bool snapdragon_8_gen_5{};
    bool usable{};
};

AndroidCpuTopology& GetAndroidCpuTopology() {
    static AndroidCpuTopology topology;
    return topology;
}

std::once_flag& GetAndroidCpuTopologyOnceFlag() {
    static std::once_flag once;
    return once;
}

int CountCpus(const cpu_set_t& cpus) {
    int count = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        count += CPU_ISSET(cpu, &cpus) ? 1 : 0;
    }
    return count;
}

bool EqualCpus(const cpu_set_t& lhs, const cpu_set_t& rhs) {
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &lhs) != CPU_ISSET(cpu, &rhs)) {
            return false;
        }
    }
    return true;
}

std::uint64_t CpuMask64(const cpu_set_t& cpus) {
    std::uint64_t mask = 0;
    for (int cpu = 0; cpu < std::min(CPU_SETSIZE, 64); ++cpu) {
        if (CPU_ISSET(cpu, &cpus)) {
            mask |= std::uint64_t{1} << cpu;
        }
    }
    return mask;
}

bool ReadCpuMidr(const int cpu, std::uint32_t& midr) {
    std::ifstream input{"/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                        "/regs/identification/midr_el1"};
    std::uint64_t value = 0;
    input >> std::hex >> value;
    if (!input || value > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    midr = static_cast<std::uint32_t>(value);
    return true;
}

bool HasCortexX4(const cpu_set_t& allowed) {
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &allowed)) {
            continue;
        }
        std::uint32_t midr = 0;
        if (ReadCpuMidr(cpu, midr) && ((midr >> 24) & 0xffu) == 0x41u &&
            ((midr >> 4) & 0xfffu) == 0xd82u) {
            return true;
        }
    }
    return false;
}

bool IsSm8650() {
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get("ro.soc.model", value) <= 0) {
        return false;
    }
    std::string normalized{value};
    std::ranges::transform(normalized, normalized.begin(),
                           [](const unsigned char ch) {
                               return static_cast<char>(std::tolower(ch));
                           });
    return normalized == "sm8650";
}

bool IsSm8850() {
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get("ro.soc.model", value) <= 0) {
        return false;
    }
    std::string normalized{value};
    std::ranges::transform(normalized, normalized.begin(),
                           [](const unsigned char ch) {
                               return static_cast<char>(std::tolower(ch));
                           });
    return normalized == "sm8850";
}

std::uint64_t ReadPositiveCpuValue(const int cpu, const char* relative_path) {
    std::ifstream input{"/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/" +
                        relative_path};
    std::uint64_t value = 0;
    input >> value;
    return input ? value : 0;
}

bool DiscoverScoreSource(const cpu_set_t& allowed, const char* relative_path,
                         std::array<std::uint64_t, CPU_SETSIZE>& scores,
                         std::uint64_t& minimum, std::uint64_t& maximum) {
    minimum = std::numeric_limits<std::uint64_t>::max();
    maximum = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &allowed)) {
            continue;
        }
        const std::uint64_t value = ReadPositiveCpuValue(cpu, relative_path);
        if (value == 0) {
            return false;
        }
        scores[static_cast<std::size_t>(cpu)] = value;
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
    }
    return minimum != std::numeric_limits<std::uint64_t>::max() && maximum > minimum;
}

void DiscoverAndroidCpuTopology() {
    auto& topology = GetAndroidCpuTopology();
    CPU_ZERO(&topology.baseline_allowed);
    CPU_ZERO(&topology.prime_cpus);
    CPU_ZERO(&topology.performance_cpus);
    CPU_ZERO(&topology.latency_cpus);
    CPU_ZERO(&topology.graphics_cpus);
    CPU_ZERO(&topology.compute_cpus);

    if (sched_getaffinity(0, sizeof(topology.baseline_allowed),
                          &topology.baseline_allowed) != 0) {
        __android_log_print(ANDROID_LOG_WARN, AndroidThreadAffinityLogTag,
                            "[EXECUTOR_THREAD_AFFINITY_TOPOLOGY] enabled=0 "
                            "reason=affinity_read_failed errno=%d",
                            errno);
        return;
    }
    topology.baseline_count = CountCpus(topology.baseline_allowed);
    if (topology.baseline_count < 4) {
        __android_log_print(ANDROID_LOG_INFO, AndroidThreadAffinityLogTag,
                            "[EXECUTOR_THREAD_AFFINITY_TOPOLOGY] enabled=0 "
                            "reason=small_allowed_mask allowed=%d",
                            topology.baseline_count);
        return;
    }
    topology.cortex_x4 = HasCortexX4(topology.baseline_allowed);
    topology.snapdragon_8_gen_3 = topology.cortex_x4 && IsSm8650();
    topology.snapdragon_8_gen_5 = IsSm8850();

    std::array<std::uint64_t, CPU_SETSIZE> candidate_scores{};
    if (DiscoverScoreSource(topology.baseline_allowed, "cpu_capacity", candidate_scores,
                            topology.minimum_score, topology.maximum_score)) {
        topology.score_source = "cpu_capacity";
        topology.scores = candidate_scores;
    } else {
        candidate_scores.fill(0);
        if (DiscoverScoreSource(topology.baseline_allowed, "cpufreq/cpuinfo_max_freq",
                                candidate_scores, topology.minimum_score,
                                topology.maximum_score)) {
            topology.score_source = "cpuinfo_max_freq";
            topology.scores = candidate_scores;
        } else {
            candidate_scores.fill(0);
            if (DiscoverScoreSource(topology.baseline_allowed, "cpufreq/scaling_max_freq",
                                    candidate_scores, topology.minimum_score,
                                    topology.maximum_score)) {
                topology.score_source = "scaling_max_freq";
                topology.scores = candidate_scores;
            } else {
                __android_log_print(
                    ANDROID_LOG_INFO, AndroidThreadAffinityLogTag,
                    "[EXECUTOR_THREAD_AFFINITY_TOPOLOGY] enabled=0 "
                    "reason=no_heterogeneous_capacity_data allowed=%d",
                    topology.baseline_count);
                return;
            }
        }
    }

    // Treat differences below ten percent as a homogeneous topology. Pinning
    // such a device only removes scheduler freedom without selecting faster CPUs.
    if (topology.maximum_score * 100u <= topology.minimum_score * 110u) {
        __android_log_print(
            ANDROID_LOG_INFO, AndroidThreadAffinityLogTag,
            "[EXECUTOR_THREAD_AFFINITY_TOPOLOGY] enabled=0 "
            "reason=effectively_homogeneous source=%s allowed=%d min=%llu max=%llu",
            topology.score_source, topology.baseline_count,
            static_cast<unsigned long long>(topology.minimum_score),
            static_cast<unsigned long long>(topology.maximum_score));
        return;
    }

    const std::uint64_t performance_floor = topology.maximum_score * 4u / 5u;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &topology.baseline_allowed)) {
            continue;
        }
        const auto score = topology.scores[static_cast<std::size_t>(cpu)];
        if (score >= performance_floor) {
            CPU_SET(cpu, &topology.prime_cpus);
            CPU_SET(cpu, &topology.performance_cpus);
        }
    }

    topology.prime_count = CountCpus(topology.prime_cpus);
    topology.performance_count = CountCpus(topology.performance_cpus);
    // A two-core prime island is too small for the guest main and GPU command
    // threads together on sustained workloads. Include the next cluster so the
    // Android scheduler can balance latency-critical work without a hot-spot.
    if (topology.performance_count <= 2) {
        std::uint64_t next_cluster_score = 0;
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            if (!CPU_ISSET(cpu, &topology.baseline_allowed)) {
                continue;
            }
            const auto score = topology.scores[static_cast<std::size_t>(cpu)];
            if (score < topology.maximum_score) {
                next_cluster_score = std::max(next_cluster_score, score);
            }
        }
        if (next_cluster_score != 0) {
            for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
                if (CPU_ISSET(cpu, &topology.baseline_allowed) &&
                    topology.scores[static_cast<std::size_t>(cpu)] >= next_cluster_score) {
                    CPU_SET(cpu, &topology.performance_cpus);
                }
            }
            topology.performance_count = CountCpus(topology.performance_cpus);
        }
    }

    // Keep the frame-latency guest and GPU command threads on separate CPUs.
    // Guest workers may use the GPU-command CPU while it is idle, while the
    // strongest latency CPU remains exclusive to the guest main thread.
    int latency_cpu = -1;
    int graphics_cpu = -1;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &topology.baseline_allowed)) {
            continue;
        }
        if (latency_cpu < 0 ||
            topology.scores[static_cast<std::size_t>(cpu)] >
                topology.scores[static_cast<std::size_t>(latency_cpu)] ||
            (topology.scores[static_cast<std::size_t>(cpu)] ==
                 topology.scores[static_cast<std::size_t>(latency_cpu)] &&
             cpu > latency_cpu)) {
            latency_cpu = cpu;
        }
    }
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (cpu == latency_cpu || !CPU_ISSET(cpu, &topology.baseline_allowed)) {
            continue;
        }
        if (graphics_cpu < 0 ||
            topology.scores[static_cast<std::size_t>(cpu)] >
                topology.scores[static_cast<std::size_t>(graphics_cpu)] ||
            (topology.scores[static_cast<std::size_t>(cpu)] ==
                 topology.scores[static_cast<std::size_t>(graphics_cpu)] &&
             cpu > graphics_cpu)) {
            graphics_cpu = cpu;
        }
    }
    if (latency_cpu >= 0) {
        CPU_SET(latency_cpu, &topology.latency_cpus);
    }
    if (graphics_cpu >= 0) {
        CPU_SET(graphics_cpu, &topology.graphics_cpus);
    }
    topology.latency_count = CountCpus(topology.latency_cpus);
    topology.graphics_count = CountCpus(topology.graphics_cpus);

    topology.compute_cpus = topology.baseline_allowed;
    if (topology.baseline_count >= 6) {
        if (latency_cpu >= 0) {
            CPU_CLR(latency_cpu, &topology.compute_cpus);
        }
        // Guest compute used to keep the graphics CPU on the assumption that a
        // worker may borrow it while the GPU command thread is idle. Measured
        // on an 8-CPU device that assumption does not hold: a guest worker at
        // 83% duty shared the graphics CPU and left the GPU command thread
        // ready-but-unscheduled for 53% of the wall window, while the six
        // remaining CPUs idled near their minimum frequency. Yield the
        // graphics CPU whenever enough CPUs remain to absorb the workers.
        if (graphics_cpu >= 0 && topology.baseline_count - 2 >= kMinimumComputeCpus &&
            GuestComputeYieldsGraphicsCpu()) {
            CPU_CLR(graphics_cpu, &topology.compute_cpus);
        }
    }
    topology.compute_count = CountCpus(topology.compute_cpus);
    topology.usable = topology.latency_count == 1 &&
                      topology.graphics_count == 1 &&
                      topology.compute_count >= 2;
    __android_log_print(
        topology.usable ? ANDROID_LOG_INFO : ANDROID_LOG_WARN,
        AndroidThreadAffinityLogTag,
        "[EXECUTOR_THREAD_AFFINITY_TOPOLOGY] enabled=%d source=%s allowed=%d "
        "prime=%d performance=%d latency=%d graphics=%d compute=%d "
        "computeYieldsGraphics=%d cortexX4=%d snapdragon8Gen3=%d "
        "snapdragon8Gen5=%d "
        "latencyCpu=%d graphicsCpu=%d min=%llu max=%llu",
        topology.usable ? 1 : 0, topology.score_source, topology.baseline_count,
        topology.prime_count, topology.performance_count, topology.latency_count,
        topology.graphics_count, topology.compute_count,
        (graphics_cpu >= 0 && !CPU_ISSET(graphics_cpu, &topology.compute_cpus)) ? 1 : 0,
        topology.cortex_x4 ? 1 : 0, topology.snapdragon_8_gen_3 ? 1 : 0,
        topology.snapdragon_8_gen_5 ? 1 : 0,
        latency_cpu, graphics_cpu,
        static_cast<unsigned long long>(topology.minimum_score),
        static_cast<unsigned long long>(topology.maximum_score));
}

const char* ThreadAffinityClassName(const ThreadAffinityClass affinity_class) {
    switch (affinity_class) {
    case ThreadAffinityClass::LatencyPerformance:
        return "latency_performance";
    case ThreadAffinityClass::GuestCompute:
        return "guest_compute";
    case ThreadAffinityClass::GraphicsDriver:
        return "graphics_driver";
    case ThreadAffinityClass::PrimaryGuest:
        return "primary_guest";
    }
    return "unknown";
}

} // namespace
#endif

void InitializeAndroidThreadAffinity() {
#ifdef __ANDROID__
    std::call_once(GetAndroidCpuTopologyOnceFlag(), DiscoverAndroidCpuTopology);
#endif
}

bool IsSnapdragon8Gen3Host() {
#ifdef __ANDROID__
    InitializeAndroidThreadAffinity();
    return GetAndroidCpuTopology().snapdragon_8_gen_3;
#else
    return false;
#endif
}

bool IsSnapdragon8Gen5Host() {
#ifdef __ANDROID__
    InitializeAndroidThreadAffinity();
    return GetAndroidCpuTopology().snapdragon_8_gen_5;
#else
    return false;
#endif
}

void ConfigureAndroidThreadScheduler(const u32 scheduler_mode,
                                     const u32 preferred_heavy_threads) {
#ifdef __ANDROID__
    g_android_scheduler_mode.store(std::min(scheduler_mode, 3U),
                                   std::memory_order_release);
    g_android_preferred_heavy_threads.store(
        std::clamp(preferred_heavy_threads, 1U, 8U),
        std::memory_order_release);
    __android_log_print(
        ANDROID_LOG_INFO, AndroidThreadAffinityLogTag,
        "[EXECUTOR_THREAD_SCHEDULER_CONFIG] mode=%u preferredHeavy=%u",
        g_android_scheduler_mode.load(std::memory_order_relaxed),
        g_android_preferred_heavy_threads.load(std::memory_order_relaxed));
#else
    (void)scheduler_mode;
    (void)preferred_heavy_threads;
#endif
}

bool ApplyCurrentThreadAffinity(const ThreadAffinityClass affinity_class) {
#ifdef __ANDROID__
    InitializeAndroidThreadAffinity();
    const auto& topology = GetAndroidCpuTopology();
    const u32 requested_scheduler_mode =
        g_android_scheduler_mode.load(std::memory_order_acquire);
    // Auto resolves from the discovered topology instead of being an alias
    // for Emulator: heterogeneous usable CPUs get role placement, while a
    // homogeneous or incomplete topology is left to Android's scheduler.
    const u32 scheduler_mode = requested_scheduler_mode == 3
                                   ? (topology.usable &&
                                              topology.maximum_score > topology.minimum_score
                                          ? 1U
                                          : 0U)
                                   : requested_scheduler_mode;
    if (scheduler_mode == 0) {
        // OS mode is a true no-op: inheriting Android's current cpuset is not
        // equivalent to restoring the process-wide mask captured at startup.
        return true;
    }
    if (!topology.usable && scheduler_mode != 0) {
        return false;
    }

    cpu_set_t current_allowed;
    CPU_ZERO(&current_allowed);
    if (sched_getaffinity(0, sizeof(current_allowed), &current_allowed) != 0) {
        return false;
    }
    const int current_count = CountCpus(current_allowed);

    const int requested_heavy = static_cast<int>(
        g_android_preferred_heavy_threads.load(std::memory_order_acquire));
    const auto select_fastest = [&](const cpu_set_t& source) {
        cpu_set_t selected;
        CPU_ZERO(&selected);
        std::array<int, CPU_SETSIZE> ranked{};
        int ranked_count = 0;
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            if (CPU_ISSET(cpu, &source)) {
                ranked[static_cast<std::size_t>(ranked_count++)] = cpu;
            }
        }
        std::sort(ranked.begin(), ranked.begin() + ranked_count,
                  [&](const int lhs, const int rhs) {
                      const auto left = topology.scores[static_cast<std::size_t>(lhs)];
                      const auto right = topology.scores[static_cast<std::size_t>(rhs)];
                      return left != right ? left > right : lhs > rhs;
                  });
        for (int index = 0; index < std::min(requested_heavy, ranked_count); ++index) {
            CPU_SET(ranked[static_cast<std::size_t>(index)], &selected);
        }
        return selected;
    };
    const cpu_set_t performance_policy_cpus =
        select_fastest(topology.baseline_allowed);
    const cpu_set_t preferred_compute_cpus =
        select_fastest(topology.compute_cpus);
    const cpu_set_t& role_cpus = [&]() -> const cpu_set_t& {
        switch (affinity_class) {
        case ThreadAffinityClass::LatencyPerformance:
            return topology.latency_cpus;
        case ThreadAffinityClass::GuestCompute:
            if (scheduler_mode == 2 && CountCpus(performance_policy_cpus) != 0) {
                return performance_policy_cpus;
            }
            return CountCpus(preferred_compute_cpus) != 0
                       ? preferred_compute_cpus
                       : topology.compute_cpus;
        case ThreadAffinityClass::GraphicsDriver:
            return topology.graphics_cpus;
        case ThreadAffinityClass::PrimaryGuest:
            // On the observed SM8650/HyperOS path, a singleton request before
            // fan-out leaves child guest threads on that same CPU and their
            // later widening request does not change the effective mask.
            // Request the configured fastest set before the primary guest
            // fans out so child placement starts from the selected policy.
            return CountCpus(performance_policy_cpus) != 0
                       ? performance_policy_cpus
                       : (topology.snapdragon_8_gen_3
                              ? topology.performance_cpus
                              : topology.latency_cpus);
        }
        return topology.baseline_allowed;
    }();
    cpu_set_t requested;
    CPU_ZERO(&requested);
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &topology.baseline_allowed) &&
            CPU_ISSET(cpu, &role_cpus)) {
            CPU_SET(cpu, &requested);
        }
    }
    const int requested_count = CountCpus(requested);
    if (requested_count == 0) {
        return false;
    }

    const int result = sched_setaffinity(0, sizeof(requested), &requested);
    const int saved_errno = result == 0 ? 0 : errno;
    int fallback_result = -1;
    int fallback_errno = 0;
    int fallback_requested_count = 0;
    bool fallback_applied = false;
    cpu_set_t fallback_requested;
    CPU_ZERO(&fallback_requested);
    if (result != 0 && saved_errno == EINVAL &&
        (affinity_class == ThreadAffinityClass::GraphicsDriver ||
         affinity_class == ThreadAffinityClass::LatencyPerformance ||
         affinity_class == ThreadAffinityClass::PrimaryGuest)) {
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            if (!CPU_ISSET(cpu, &topology.baseline_allowed) ||
                !CPU_ISSET(cpu, &topology.performance_cpus)) {
                continue;
            }
            if (affinity_class == ThreadAffinityClass::GraphicsDriver &&
                CPU_ISSET(cpu, &topology.latency_cpus)) {
                continue;
            }
            CPU_SET(cpu, &fallback_requested);
        }
        fallback_requested_count = CountCpus(fallback_requested);
        if (fallback_requested_count != 0) {
            fallback_result =
                sched_setaffinity(0, sizeof(fallback_requested), &fallback_requested);
            fallback_errno = fallback_result == 0 ? 0 : errno;
            fallback_applied = fallback_result == 0;
        }
    }
    cpu_set_t actual;
    CPU_ZERO(&actual);
    const bool actual_read = (result == 0 || fallback_applied) &&
                             sched_getaffinity(0, sizeof(actual), &actual) == 0;
    const int actual_count = actual_read ? CountCpus(actual) : 0;
    const cpu_set_t& expected = fallback_applied ? fallback_requested : requested;
    const bool exact = actual_read && EqualCpus(actual, expected);

    static std::array<std::atomic_uint32_t, 4> log_counts{};
    const auto role_index = static_cast<std::size_t>(affinity_class);
    if (role_index < log_counts.size() &&
        log_counts[role_index].fetch_add(1, std::memory_order_relaxed) < 16) {
        __android_log_print(
            exact ? ANDROID_LOG_INFO : ANDROID_LOG_WARN,
            AndroidThreadAffinityLogTag,
            "[EXECUTOR_THREAD_AFFINITY] role=%s before=%d requested=%d actual=%d "
            "beforeMask=0x%llx requestedMask=0x%llx actualMask=0x%llx exact=%d "
            "result=%d errno=%d fallback=%d fallbackRequested=%d "
            "fallbackResult=%d fallbackErrno=%d schedulerRequested=%u "
            "schedulerEffective=%u preferredHeavy=%u",
            ThreadAffinityClassName(affinity_class), current_count, requested_count,
            actual_count,
            static_cast<unsigned long long>(CpuMask64(current_allowed)),
            static_cast<unsigned long long>(CpuMask64(expected)),
            static_cast<unsigned long long>(CpuMask64(actual)), exact ? 1 : 0,
            result, saved_errno, fallback_applied ? 1 : 0,
            fallback_requested_count, fallback_result, fallback_errno,
            requested_scheduler_mode, scheduler_mode,
            g_android_preferred_heavy_threads.load(std::memory_order_relaxed));
    }
    // A broad performance-cluster fallback keeps the thread off LITTLE CPUs,
    // but it is not the final isolated role placement. Return false so a
    // latency-critical caller can retry the preferred single CPU after Android
    // has brought that core online.
    return result == 0 && !fallback_applied && actual_read &&
           EqualCpus(actual, requested);
#else
    (void)affinity_class;
    return false;
#endif
}

#ifdef __APPLE__

void SetCurrentThreadRealtime(const std::chrono::nanoseconds period_ns) {
    const std::chrono::nanoseconds computation_ns = period_ns / 2;

    struct mach_timebase_info timebase{};
    mach_timebase_info(&timebase);
    const auto ticks_per_ns =
        static_cast<double>(timebase.denom) / static_cast<double>(timebase.numer);

    const auto period_ticks =
        static_cast<u32>(static_cast<double>(period_ns.count()) * ticks_per_ns);
    const auto computation_ticks =
        static_cast<u32>(static_cast<double>(computation_ns.count()) * ticks_per_ns);

    thread_time_constraint_policy policy = {
        .period = period_ticks,
        .computation = computation_ticks,
        .constraint = computation_ticks,
        .preemptible = false,
    };

    int ret = thread_policy_set(
        pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
        reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (ret != KERN_SUCCESS) {
        LOG_ERROR(Common, "Could not set thread to real-time with period {} ns: {}",
                  period_ns.count(), ret);
    }
}

#else

void SetCurrentThreadRealtime(const std::chrono::nanoseconds period_ns) {
}

#endif

#ifdef _WIN32

void SetCurrentThreadPriority(ThreadPriority new_priority) {
    auto handle = GetCurrentThread();
    int windows_priority = 0;
    switch (new_priority) {
    case ThreadPriority::Low:
        windows_priority = THREAD_PRIORITY_BELOW_NORMAL;
        break;
    case ThreadPriority::Normal:
        windows_priority = THREAD_PRIORITY_NORMAL;
        break;
    case ThreadPriority::High:
        windows_priority = THREAD_PRIORITY_ABOVE_NORMAL;
        break;
    case ThreadPriority::VeryHigh:
        windows_priority = THREAD_PRIORITY_HIGHEST;
        break;
    case ThreadPriority::Critical:
        windows_priority = THREAD_PRIORITY_TIME_CRITICAL;
        break;
    default:
        windows_priority = THREAD_PRIORITY_NORMAL;
        break;
    }
    SetThreadPriority(handle, windows_priority);
}

bool AccurateSleep(const std::chrono::nanoseconds duration, std::chrono::nanoseconds* remaining,
                   const bool interruptible) {
    const auto begin_sleep = std::chrono::high_resolution_clock::now();

    LARGE_INTEGER interval{
        .QuadPart = -1 * (duration.count() / 100u),
    };
    HANDLE timer = ::CreateWaitableTimer(NULL, TRUE, NULL);
    SetWaitableTimer(timer, &interval, 0, NULL, NULL, 0);
    const auto ret = WaitForSingleObjectEx(timer, INFINITE, interruptible);
    ::CloseHandle(timer);

    if (remaining) {
        const auto end_sleep = std::chrono::high_resolution_clock::now();
        const auto sleep_time = end_sleep - begin_sleep;
        *remaining = duration > sleep_time ? duration - sleep_time : std::chrono::nanoseconds(0);
    }
    return ret == WAIT_OBJECT_0;
}

#else

void SetCurrentThreadPriority(ThreadPriority new_priority) {
    pthread_t this_thread = pthread_self();

    const auto scheduling_type = SCHED_OTHER;
    s32 max_prio = sched_get_priority_max(scheduling_type);
    s32 min_prio = sched_get_priority_min(scheduling_type);
    u32 level = std::max(static_cast<u32>(new_priority) + 1, 4U);

    struct sched_param params;
    if (max_prio > min_prio) {
        params.sched_priority = min_prio + ((max_prio - min_prio) * level) / 4;
    } else {
        params.sched_priority = min_prio - ((min_prio - max_prio) * level) / 4;
    }

    pthread_setschedparam(this_thread, scheduling_type, &params);
}

bool AccurateSleep(const std::chrono::nanoseconds duration, std::chrono::nanoseconds* remaining,
                   const bool interruptible) {
    timespec request = {
        .tv_sec = duration.count() / 1'000'000'000,
        .tv_nsec = duration.count() % 1'000'000'000,
    };
    timespec remain;
    int ret;
    while ((ret = nanosleep(&request, &remain)) < 0 && errno == EINTR) {
        if (interruptible) {
            break;
        }
        request = remain;
    }
    if (remaining) {
        *remaining = std::chrono::nanoseconds(remain.tv_sec * 1'000'000'000 + remain.tv_nsec);
    }
    return ret == 0 || errno != EINTR;
}

#endif

#ifdef _MSC_VER

void SetCurrentThreadName(const char* name) {
    if (Libraries::Kernel::g_curthread) {
        Libraries::Kernel::g_curthread->name = name;
    }
    SetThreadDescription(GetCurrentThread(), UTF8ToUTF16W(name).data());
}

void SetThreadName(void* thread, const char* name) {
    SetThreadDescription(thread, UTF8ToUTF16W(name).data());
}

#else

#if !defined(_WIN32) || defined(_MSC_VER)
void SetCurrentThreadName(const char* name) {
    if (Libraries::Kernel::g_curthread) {
        Libraries::Kernel::g_curthread->name = name;
    }
#ifdef __APPLE__
    pthread_setname_np(name);
#elif defined(__Bitrig__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    pthread_set_name_np(pthread_self(), name);
#elif defined(__NetBSD__)
    pthread_setname_np(pthread_self(), "%s", (void*)name);
#elif defined(__linux__)
    std::string truncated(name, std::min(strlen(name), static_cast<std::size_t>(15)));
    if (int e = pthread_setname_np(pthread_self(), truncated.c_str())) {
        errno = e;
        LOG_ERROR(Common, "Failed to set thread name to '{}': {}", truncated, GetLastErrorMsg());
    }
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

void SetThreadName(void* thread, const char* name) {
}
#endif

#if defined(_WIN32)
void SetCurrentThreadName(const char*) {
    if (Libraries::Kernel::g_curthread) {
        Libraries::Kernel::g_curthread->name = name;
    }
}

void SetThreadName(void* thread, const char* name) {
}
#endif

#endif

AccurateTimer::AccurateTimer(std::chrono::nanoseconds target_interval)
    : target_interval(target_interval) {}

void AccurateTimer::Start() {
    const auto begin_sleep = std::chrono::high_resolution_clock::now();
    if (total_wait.count() > 0) {
        AccurateSleep(total_wait, nullptr, false);
    }
    start_time = std::chrono::high_resolution_clock::now();
    total_wait -= std::chrono::duration_cast<std::chrono::nanoseconds>(start_time - begin_sleep);
}

void AccurateTimer::End() {
    auto now = std::chrono::high_resolution_clock::now();
    total_wait +=
        target_interval - std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_time);
}

std::string GetCurrentThreadName() {
    using namespace Libraries::Kernel;
    if (g_curthread && !g_curthread->name.empty()) {
        return g_curthread->name;
    }
#ifdef _WIN32
    PWSTR name;
    GetThreadDescription(GetCurrentThread(), &name);
    return Common::UTF16ToUTF8(name);
#else
    char name[256];
    if (pthread_getname_np(pthread_self(), name, sizeof(name)) != 0) {
        return "<unknown name>";
    }
    return std::string{name};
#endif
}

}
