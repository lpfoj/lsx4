// SPDX-FileCopyrightText: Copyright 2026 LSX4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "executor/android_performance_hint.h"
#include "executor/android_api_level.h"

#if defined(__ANDROID__)
#include <android/log.h>
#include <android/performance_hint.h>
#include <dirent.h>
#include <dlfcn.h>
#include <linux/sched.h>
#include <linux/sched/types.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Executor::AndroidPerformanceHint {
namespace {

// sceGnmSubmitDone is the guest producer-cycle boundary. Present cadence is
// unsuitable here because retained/reused frames may be presented at 60 Hz
// while the guest producer is running below its target.
constexpr std::int64_t FullRateProducerCycleDurationNs = 16'666'667;
constexpr std::int64_t RelaxedProducerCycleDurationNs = 33'333'333;
constexpr std::int64_t MaximumReportedProducerCycleDurationNs = 1'000'000'000;
constexpr auto ThreadRefreshPeriod = std::chrono::seconds{2};
constexpr std::uint64_t StatusLogPeriod = 300;
constexpr std::uint32_t CriticalThreadUclampMin = 512;

std::atomic_bool g_enabled{false};
std::atomic<std::int64_t> g_target_producer_cycle_duration_ns{
    FullRateProducerCycleDurationNs};

std::int64_t TargetProducerCycleDurationNs() noexcept {
    return g_target_producer_cycle_duration_ns.load(std::memory_order_acquire);
}

#if defined(__ANDROID__)

using GetManagerFn = APerformanceHintManager* (*)();
using CreateSessionFn = APerformanceHintSession* (*)(
    APerformanceHintManager*, const std::int32_t*, std::size_t, std::int64_t);
using GetPreferredRateFn = std::int64_t (*)(APerformanceHintManager*);
using UpdateTargetFn = int (*)(APerformanceHintSession*, std::int64_t);
using ReportActualFn = int (*)(APerformanceHintSession*, std::int64_t);
using CloseSessionFn = void (*)(APerformanceHintSession*);
using SetThreadsFn = int (*)(APerformanceHintSession*, const pid_t*, std::size_t);
using SetPreferPowerEfficiencyFn = int (*)(APerformanceHintSession*, bool);

struct ThreadSet {
    std::vector<std::int32_t> tids;
    std::vector<std::int32_t> latency_tids;
    std::string description;
};

struct State {
    std::mutex mutex;
    bool load_attempted{};
    bool api_ready{};
    bool unsupported_logged{};
    void* library{};
    GetManagerFn get_manager{};
    CreateSessionFn create_session{};
    GetPreferredRateFn get_preferred_rate{};
    UpdateTargetFn update_target{};
    ReportActualFn report_actual{};
    CloseSessionFn close_session{};
    SetThreadsFn set_threads{};
    SetPreferPowerEfficiencyFn set_prefer_power_efficiency{};
    APerformanceHintManager* manager{};
    APerformanceHintSession* session{};
    std::vector<std::int32_t> tids;
    std::unordered_map<std::int32_t, std::uint64_t> previous_cpu_ticks;
    std::unordered_map<std::int32_t, std::uint32_t> original_uclamp_min;
    std::unordered_map<std::int32_t, int> original_nice;
    std::unordered_map<std::int32_t, std::chrono::steady_clock::time_point>
        producer_cycle_boundaries;
    std::chrono::steady_clock::time_point next_thread_refresh{};
    std::uint64_t reports{};
    std::uint64_t report_errors{};
    std::uint64_t session_generation{};
    std::int64_t preferred_rate_ns{};
};

State& GetState() {
    static State* state = new State{};
    return *state;
}

template <typename Function>
Function Resolve(void* library, const char* name) {
    return reinterpret_cast<Function>(dlsym(library, name));
}

void LogUnsupportedOnce(State& state, const char* reason) {
    if (state.unsupported_logged) {
        return;
    }
    state.unsupported_logged = true;
    __android_log_print(ANDROID_LOG_INFO, "LSX4-ADPF",
                        "[EXECUTOR_ADPF] supported=0 enabled=1 reason=%s api=%d",
                        reason, AndroidDeviceApiLevel());
}

bool LoadApi(State& state) {
    if (state.load_attempted) {
        return state.api_ready;
    }
    state.load_attempted = true;
    if (AndroidDeviceApiLevel() < 31) {
        LogUnsupportedOnce(state, "android_api_below_31");
        return false;
    }

    state.library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (state.library == nullptr) {
        LogUnsupportedOnce(state, "libandroid_unavailable");
        return false;
    }
    state.get_manager =
        Resolve<GetManagerFn>(state.library, "APerformanceHint_getManager");
    state.create_session =
        Resolve<CreateSessionFn>(state.library, "APerformanceHint_createSession");
    state.get_preferred_rate = Resolve<GetPreferredRateFn>(
        state.library, "APerformanceHint_getPreferredUpdateRateNanos");
    state.update_target = Resolve<UpdateTargetFn>(
        state.library, "APerformanceHint_updateTargetWorkDuration");
    state.report_actual = Resolve<ReportActualFn>(
        state.library, "APerformanceHint_reportActualWorkDuration");
    state.close_session = Resolve<CloseSessionFn>(
        state.library, "APerformanceHint_closeSession");
    state.set_threads =
        Resolve<SetThreadsFn>(state.library, "APerformanceHint_setThreads");
    state.set_prefer_power_efficiency = Resolve<SetPreferPowerEfficiencyFn>(
        state.library, "APerformanceHint_setPreferPowerEfficiency");

    state.api_ready = state.get_manager != nullptr &&
                      state.create_session != nullptr &&
                      state.get_preferred_rate != nullptr &&
                      state.update_target != nullptr &&
                      state.report_actual != nullptr &&
                      state.close_session != nullptr;
    if (!state.api_ready) {
        LogUnsupportedOnce(state, "required_symbols_unavailable");
        return false;
    }
    state.manager = state.get_manager();
    if (state.manager == nullptr) {
        state.api_ready = false;
        LogUnsupportedOnce(state, "manager_unavailable");
        return false;
    }
    state.preferred_rate_ns = state.get_preferred_rate(state.manager);
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4-ADPF",
        "[EXECUTOR_ADPF] supported=1 enabled=1 api=%d targetNs=%lld "
        "preferredRateNs=%lld dynamicThreads=%d cadence=producer_cycle",
        AndroidDeviceApiLevel(),
        static_cast<long long>(TargetProducerCycleDurationNs()),
        static_cast<long long>(state.preferred_rate_ns),
        state.set_threads != nullptr ? 1 : 0);
    return true;
}

std::string ReadThreadName(const std::int32_t tid) {
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
    FILE* file = std::fopen(path, "r");
    if (file == nullptr) {
        return {};
    }
    char name[64]{};
    const char* result = std::fgets(name, sizeof(name), file);
    std::fclose(file);
    if (result == nullptr) {
        return {};
    }
    std::string value{name};
    while (!value.empty() &&
           (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

enum class CriticalThreadRole {
    None,
    GameMain,
    GpuCommand,
    GraphicsWorker,
    AudioMixer,
    DependencyWorker,
};

CriticalThreadRole ClassifyCriticalThread(std::string_view name) {
    std::string lowered{name};
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](const unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });
    if (lowered.find("game:main") != std::string::npos) {
        return CriticalThreadRole::GameMain;
    }
    if (lowered.find("gpucomm") != std::string::npos) {
        return CriticalThreadRole::GpuCommand;
    }
    // Linux truncates /proc/<pid>/task/<tid>/comm to 15 characters, so
    // UnityGfxDeviceWorker is observed as UnityGfxDeviceW. Match the stable
    // engine-role prefix and keep it separate from GpuComm: both participate
    // in the frame's latency chain and selecting only the busier one leaves
    // the other outside the ADPF session.
    if (lowered.starts_with("unitygfx")) {
        return CriticalThreadRole::GraphicsWorker;
    }
    if (lowered.find("fmod mixer") != std::string::npos) {
        return CriticalThreadRole::AudioMixer;
    }
    if (lowered.find("havokworker") != std::string::npos ||
        lowered.find("fmod stream") != std::string::npos ||
        lowered.starts_with("unityworker")) {
        return CriticalThreadRole::DependencyWorker;
    }
    if (lowered.starts_with("thread") && lowered.size() > 6 &&
        std::all_of(lowered.begin() + 6, lowered.end(),
                    [](const unsigned char value) { return std::isdigit(value) != 0; })) {
        return CriticalThreadRole::DependencyWorker;
    }
    return CriticalThreadRole::None;
}

std::uint64_t ReadThreadCpuTicks(const std::int32_t tid) {
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
    FILE* file = std::fopen(path, "r");
    if (file == nullptr) {
        return 0;
    }
    char record[1024]{};
    const char* result = std::fgets(record, sizeof(record), file);
    std::fclose(file);
    if (result == nullptr) {
        return 0;
    }
    const std::string_view line{record};
    const std::size_t comm_end = line.rfind(')');
    if (comm_end == std::string_view::npos ||
        comm_end + 2 >= line.size()) {
        return 0;
    }
    std::istringstream fields{
        std::string{line.substr(comm_end + 2)}};
    std::string token;
    std::uint64_t user_ticks = 0;
    std::uint64_t system_ticks = 0;
    for (std::size_t index = 0; index <= 12; ++index) {
        if (!(fields >> token)) {
            return 0;
        }
        if (index == 11) {
            user_ticks = std::strtoull(token.c_str(), nullptr, 10);
        } else if (index == 12) {
            system_ticks = std::strtoull(token.c_str(), nullptr, 10);
        }
    }
    return user_ticks + system_ticks;
}

struct ThreadCandidate {
    std::uint64_t activity{};
    std::int32_t tid{};
    std::string name;
};

ThreadSet FindCriticalThreads(State& state) {
    constexpr std::size_t MaxDependencyThreads = 9;
    ThreadSet result;
    std::vector<ThreadCandidate> game_threads;
    std::vector<ThreadCandidate> gpu_threads;
    std::vector<ThreadCandidate> graphics_worker_threads;
    std::vector<ThreadCandidate> audio_threads;
    std::vector<ThreadCandidate> dependency_threads;
    std::unordered_map<std::int32_t, std::uint64_t> current_cpu_ticks;
    DIR* directory = opendir("/proc/self/task");
    if (directory != nullptr) {
        while (dirent* entry = readdir(directory)) {
            char* end{};
            const long parsed = std::strtol(entry->d_name, &end, 10);
            if (end == entry->d_name || *end != '\0' || parsed <= 0 ||
                parsed > INT32_MAX) {
                continue;
            }
            const auto tid = static_cast<std::int32_t>(parsed);
            std::string name = ReadThreadName(tid);
            const auto role = ClassifyCriticalThread(name);
            if (role == CriticalThreadRole::None) {
                continue;
            }
            const std::uint64_t ticks = ReadThreadCpuTicks(tid);
            current_cpu_ticks.emplace(tid, ticks);
            const auto previous = state.previous_cpu_ticks.find(tid);
            const std::uint64_t activity =
                previous != state.previous_cpu_ticks.end() && ticks >= previous->second
                    ? ticks - previous->second
                    : ticks;
            auto& destination = [&]() -> std::vector<ThreadCandidate>& {
                switch (role) {
                case CriticalThreadRole::GameMain:
                    return game_threads;
                case CriticalThreadRole::GpuCommand:
                    return gpu_threads;
                case CriticalThreadRole::GraphicsWorker:
                    return graphics_worker_threads;
                case CriticalThreadRole::AudioMixer:
                    return audio_threads;
                case CriticalThreadRole::DependencyWorker:
                    return dependency_threads;
                case CriticalThreadRole::None:
                    break;
                }
                return dependency_threads;
            }();
            destination.push_back(ThreadCandidate{activity, tid, std::move(name)});
        }
        closedir(directory);
    }
    state.previous_cpu_ticks = std::move(current_cpu_ticks);

    const auto append_candidate = [](const ThreadCandidate& candidate,
                                     ThreadSet& selected,
                                     const bool latency) {
        selected.tids.push_back(candidate.tid);
        if (latency) {
            selected.latency_tids.push_back(candidate.tid);
        }
        if (!selected.description.empty()) {
            selected.description += ',';
        }
        selected.description += candidate.name.empty() ? "unnamed" : candidate.name;
        selected.description += ':';
        selected.description += std::to_string(candidate.tid);
        selected.description += "@";
        selected.description += std::to_string(candidate.activity);
    };
    const auto select_most_active = [&](std::vector<ThreadCandidate>& candidates,
                                        ThreadSet& selected,
                                        const bool latency) {
        if (candidates.empty()) {
            return;
        }
        const auto best = std::max_element(
            candidates.begin(), candidates.end(),
            [](const ThreadCandidate& left, const ThreadCandidate& right) {
                if (left.activity != right.activity) {
                    return left.activity < right.activity;
                }
                return left.tid < right.tid;
            });
        append_candidate(*best, selected, latency);
    };
    select_most_active(game_threads, result, true);
    select_most_active(gpu_threads, result, true);
    select_most_active(graphics_worker_threads, result, true);
    select_most_active(audio_threads, result, false);

    std::sort(dependency_threads.begin(), dependency_threads.end(),
              [](const ThreadCandidate& left, const ThreadCandidate& right) {
                  if (left.activity != right.activity) {
                      return left.activity > right.activity;
                  }
                  return left.tid < right.tid;
              });
    for (std::size_t index = 0;
         index < std::min(MaxDependencyThreads, dependency_threads.size()); ++index) {
        append_candidate(dependency_threads[index], result, false);
    }

    if (result.tids.empty()) {
        const auto tid = static_cast<std::int32_t>(syscall(SYS_gettid));
        result.tids.push_back(tid);
        result.latency_tids.push_back(tid);
        const std::string name = ReadThreadName(tid);
        result.description += name.empty() ? "unnamed" : name;
        result.description += ':';
        result.description += std::to_string(tid);
    }
    std::sort(result.tids.begin(), result.tids.end());
    std::sort(result.latency_tids.begin(), result.latency_tids.end());
    return result;
}

std::uint32_t ReadUclampMin(const std::int32_t tid) {
#if defined(SYS_sched_getattr)
    sched_attr attributes{};
    attributes.size = sizeof(attributes);
    if (syscall(SYS_sched_getattr, tid, &attributes, sizeof(attributes), 0) == 0) {
        return attributes.sched_util_min;
    }
#endif
    return 0;
}

int SetUclampMin(const std::int32_t tid, const std::uint32_t value) {
#if defined(SYS_sched_setattr)
    sched_attr attributes{};
    attributes.size = sizeof(attributes);
    attributes.sched_flags = SCHED_FLAG_KEEP_POLICY | SCHED_FLAG_KEEP_PARAMS |
                             SCHED_FLAG_UTIL_CLAMP_MIN;
    attributes.sched_util_min = std::min(value, 1024u);
    return static_cast<int>(syscall(SYS_sched_setattr, tid, &attributes, 0));
#else
    (void)tid;
    (void)value;
    errno = ENOSYS;
    return -1;
#endif
}

void ApplyCriticalThreadScheduling(State& state, const ThreadSet& threads) {
    for (const std::int32_t tid : threads.latency_tids) {
        const std::uint32_t current_uclamp_min = ReadUclampMin(tid);
        if (!state.original_uclamp_min.contains(tid)) {
            state.original_uclamp_min.emplace(tid, current_uclamp_min);
        }
        // Treat the LSX4 value as a floor. OEM game modes can already assign a
        // stronger boost, and replacing that value with 512 would be a
        // self-inflicted performance regression.
        const std::uint32_t target_uclamp_min =
            std::max(current_uclamp_min, CriticalThreadUclampMin);
        errno = 0;
        const int clamp_result = SetUclampMin(tid, target_uclamp_min);
        const int clamp_errno = errno;

        errno = 0;
        const int current_nice = getpriority(PRIO_PROCESS, tid);
        const int nice_read_errno = errno;
        int nice_result = 0;
        int nice_errno = 0;
        if (nice_read_errno == 0 && current_nice > -4) {
            state.original_nice.try_emplace(tid, current_nice);
            errno = 0;
            nice_result = setpriority(PRIO_PROCESS, tid, -4);
            nice_errno = errno;
        }
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4-ADPF",
            "[EXECUTOR_ADPF_SCHED] tid=%d uclampMin=%u clampResult=%d "
            "clampErrno=%d niceBefore=%d niceResult=%d niceErrno=%d",
            tid, target_uclamp_min, clamp_result, clamp_errno,
            nice_read_errno == 0 ? current_nice : 999, nice_result, nice_errno);
    }
}

void RestoreCriticalThreadScheduling(State& state) {
    for (const auto& [tid, value] : state.original_uclamp_min) {
        (void)SetUclampMin(tid, value);
    }
    for (const auto& [tid, value] : state.original_nice) {
        (void)setpriority(PRIO_PROCESS, tid, value);
    }
    state.original_uclamp_min.clear();
    state.original_nice.clear();
}

void CloseSession(State& state) {
    RestoreCriticalThreadScheduling(state);
    if (state.session != nullptr && state.close_session != nullptr) {
        state.close_session(state.session);
    }
    state.session = nullptr;
    state.tids.clear();
    state.producer_cycle_boundaries.clear();
}

bool CreateSession(State& state, const ThreadSet& threads) {
    if (threads.tids.empty()) {
        return false;
    }
    state.session = state.create_session(
        state.manager, threads.tids.data(), threads.tids.size(),
        TargetProducerCycleDurationNs());
    if (state.session == nullptr) {
        __android_log_print(
            ANDROID_LOG_WARN, "LSX4-ADPF",
            "[EXECUTOR_ADPF] sessionActive=0 createFailed=1 tids=%s",
            threads.description.c_str());
        return false;
    }
    state.tids = threads.tids;
    ApplyCriticalThreadScheduling(state, threads);
    if (state.set_prefer_power_efficiency != nullptr) {
        (void)state.set_prefer_power_efficiency(state.session, false);
    }
    ++state.session_generation;
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4-ADPF",
        "[EXECUTOR_ADPF] sessionActive=1 generation=%llu targetNs=%lld "
        "hotTidCount=%zu tids=%s",
        static_cast<unsigned long long>(state.session_generation),
        static_cast<long long>(TargetProducerCycleDurationNs()), state.tids.size(),
        threads.description.c_str());
    return true;
}

void RefreshThreads(State& state,
                    const std::chrono::steady_clock::time_point now) {
    // A rejected session is still subject to the refresh cadence.  Some
    // vendor implementations expose the ADPF API but reject every session;
    // retrying from each present turns the fallback path into a hot loop that
    // scans /proc/self/task, allocates the candidate set and logs a warning
    // once per frame.  Keep retrying so transient service failures recover,
    // but never faster than ThreadRefreshPeriod.
    if (now < state.next_thread_refresh) {
        return;
    }
    state.next_thread_refresh = now + ThreadRefreshPeriod;
    ThreadSet threads = FindCriticalThreads(state);
    if (threads.tids == state.tids && state.session != nullptr) {
        return;
    }

    if (state.session != nullptr && state.set_threads != nullptr &&
        AndroidDeviceApiLevel() >= 34) {
        const int result = state.set_threads(
            state.session, reinterpret_cast<const pid_t*>(threads.tids.data()),
            threads.tids.size());
        if (result == 0) {
            RestoreCriticalThreadScheduling(state);
            state.tids = threads.tids;
            ApplyCriticalThreadScheduling(state, threads);
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4-ADPF",
                "[EXECUTOR_ADPF] sessionActive=1 threadsUpdated=1 "
                "hotTidCount=%zu tids=%s",
                state.tids.size(), threads.description.c_str());
            return;
        }
        __android_log_print(
            ANDROID_LOG_WARN, "LSX4-ADPF",
            "[EXECUTOR_ADPF] setThreadsFailed=%d recreate=1", result);
    }

    CloseSession(state);
    CreateSession(state, threads);
}

#endif

} // namespace

void SetFullRate60HzTarget(const bool enabled) noexcept {
    const std::int64_t target = enabled ? FullRateProducerCycleDurationNs
                                        : RelaxedProducerCycleDurationNs;
    const std::int64_t previous =
        g_target_producer_cycle_duration_ns.exchange(target, std::memory_order_acq_rel);
#if defined(__ANDROID__)
    State& state = GetState();
    const std::lock_guard lock{state.mutex};
    int update_result = 0;
    if (previous != target && state.session != nullptr && state.update_target != nullptr) {
        update_result = state.update_target(state.session, target);
        if (update_result != 0) {
            CloseSession(state);
            state.next_thread_refresh = {};
        }
    }
    __android_log_print(
        update_result == 0 ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, "LSX4-ADPF",
        "[EXECUTOR_ADPF_TARGET] fullRate60Hz=%d previousNs=%lld targetNs=%lld "
        "sessionActive=%d updateResult=%d",
        enabled ? 1 : 0, static_cast<long long>(previous),
        static_cast<long long>(target), state.session != nullptr ? 1 : 0,
        update_result);
#else
    (void)previous;
#endif
}

void SetEnabled(const bool enabled) noexcept {
    g_enabled.store(enabled, std::memory_order_release);
#if defined(__ANDROID__)
    State& state = GetState();
    const std::lock_guard lock{state.mutex};
    state.producer_cycle_boundaries.clear();
    state.next_thread_refresh = {};
    if (!enabled) {
        CloseSession(state);
        __android_log_print(ANDROID_LOG_INFO, "LSX4-ADPF",
                            "[EXECUTOR_ADPF] enabled=0 sessionActive=0");
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (LoadApi(state)) {
        state.next_thread_refresh = {};
        RefreshThreads(state, now);
    }
    __android_log_print(ANDROID_LOG_INFO, "LSX4-ADPF",
                        "[EXECUTOR_ADPF] enabled=1 sessionDeferred=0 "
                        "sessionActive=%d",
                        state.session != nullptr ? 1 : 0);
#else
    (void)enabled;
#endif
}

bool IsEnabled() noexcept {
    return g_enabled.load(std::memory_order_acquire);
}

void MaintainSessionThreads() noexcept {
#if defined(__ANDROID__)
    if (!g_enabled.load(std::memory_order_acquire)) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    State& state = GetState();
    const std::lock_guard lock{state.mutex};
    if (!g_enabled.load(std::memory_order_relaxed) || !LoadApi(state)) {
        return;
    }
    RefreshThreads(state, now);
#endif
}

void ReportProducerCycleComplete() noexcept {
#if defined(__ANDROID__)
    if (!g_enabled.load(std::memory_order_acquire)) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    State& state = GetState();
    const std::lock_guard lock{state.mutex};
    if (!g_enabled.load(std::memory_order_relaxed) || !LoadApi(state)) {
        return;
    }
    if (state.session == nullptr) {
        return;
    }

    const auto producer_tid = static_cast<std::int32_t>(syscall(SYS_gettid));
    const auto [boundary, inserted] =
        state.producer_cycle_boundaries.try_emplace(producer_tid, now);
    if (inserted) {
        return;
    }
    const auto actual_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - boundary->second).count();
    boundary->second = now;
    if (actual_ns <= 0 ||
        actual_ns > MaximumReportedProducerCycleDurationNs) {
        return;
    }

    const int result = state.report_actual(state.session, actual_ns);
    if (result != 0) {
        ++state.report_errors;
        __android_log_print(
            ANDROID_LOG_WARN, "LSX4-ADPF",
            "[EXECUTOR_ADPF] reportResult=%d actualNs=%lld errors=%llu",
            result, static_cast<long long>(actual_ns),
            static_cast<unsigned long long>(state.report_errors));
        CloseSession(state);
        state.next_thread_refresh = {};
        return;
    }

    ++state.reports;
    if (state.reports == 1 || state.reports % StatusLogPeriod == 0) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4-ADPF",
            "[EXECUTOR_ADPF] reportResult=0 reports=%llu actualNs=%lld "
            "targetNs=%lld hotTidCount=%zu generation=%llu producerTid=%d "
            "cadence=producer_cycle",
            static_cast<unsigned long long>(state.reports),
            static_cast<long long>(actual_ns),
            static_cast<long long>(TargetProducerCycleDurationNs()), state.tids.size(),
            static_cast<unsigned long long>(state.session_generation),
            producer_tid);
    }
#endif
}

void ReportFramePresented() noexcept {
    // Compatibility for the PS5 renderer. The PS4 renderer reports its guest
    // producer boundary explicitly from sceGnmSubmitDone instead.
    MaintainSessionThreads();
    ReportProducerCycleComplete();
}

} // namespace Executor::AndroidPerformanceHint
