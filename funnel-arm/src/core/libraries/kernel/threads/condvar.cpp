// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <cstdlib>
#include <chrono>
#include <thread>
#ifdef __ANDROID__
#include <android/log.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <unordered_map>
#include <vector>
#endif
#include "common/assert.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/libraries/kernel/threads/sleepq.h"
#include "core/libraries/kernel/threads/thread_state.h"
#include "core/libraries/libs.h"

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
extern "C" void executor_live_mono_run_pending_signal_safe_point() __attribute__((weak));
#endif
#ifdef __ANDROID__
extern "C" int executor_live_get_current_hle_call_site(std::uint64_t* guest_return,
                                                       std::uint64_t* return_off,
                                                       std::uint64_t* arg0, char* symbol,
                                                       std::size_t symbol_size, char* module,
                                                       std::size_t module_size)
    __attribute__((weak));
extern "C" void executor_live_dump_current_hle_context(const char* reason,
                                                        std::uint64_t focus)
    __attribute__((weak));
extern "C" void executor_live_dump_kernel_wait_probe() __attribute__((weak));
extern "C" void executor_jit_dump_thread_states(const char* reason) __attribute__((weak));
#endif

namespace Libraries::Kernel {

static std::mutex CondStaticLock;

#ifdef __ANDROID__
static bool ExecutorCondSpuriousBackoffDisabled() {
    static const bool off =
        std::fopen("/data/data/app.lsx4.android/files/lsx4-home/run-no-cond-spurious-backoff",
                   "r") != nullptr;
    return off;
}
static void ExecutorCondPersistSpurious(const char* name, std::uint64_t n) {
    static std::atomic<int> budget{200};
    if (budget.fetch_sub(1, std::memory_order_relaxed) <= 0) {
        return;
    }
    if (std::FILE* f =
            std::fopen("/data/data/app.lsx4.android/files/executor-condspurious.log", "a")) {
        std::fprintf(f, "spurious thread=%s count=%llu\n", name ? name : "?",
                     static_cast<unsigned long long>(n));
        std::fclose(f);
    }
}

// Diagnostic-only queue/wake probe.  Keeping the selector in a marker file
// lets us observe one emulated pthread without widening the already hot
// condvar logs for every title.  The marker is absent in production runs.
static const std::string& ExecutorCondWakeProbeThread() {
    static const std::string target = [] {
        constexpr const char* path =
            "/data/data/app.lsx4.android/files/lsx4-home/diag-kernel-cond-wake";
        std::FILE* file = std::fopen(path, "r");
        if (file == nullptr) {
            return std::string{};
        }
        char name[64]{};
        const char* const read = std::fgets(name, sizeof(name), file);
        std::fclose(file);
        if (read == nullptr) {
            return std::string{};
        }
        name[std::strcspn(name, "\r\n")] = '\0';
        return std::string{name};
    }();
    return target;
}

static bool ExecutorFiosProbeEnabled() {
    return !ExecutorCondWakeProbeThread().empty();
}

static std::atomic<PthreadCond*> ExecutorCondWakeProbeTarget{};
static std::atomic<Pthread*> ExecutorCondWakeProbeWaiter{};
static std::atomic<PthreadMutex*> ExecutorCondWakeProbeMutex{};
static std::atomic<std::uint64_t> ExecutorCondWakeProbeGeneration{};
static std::atomic<std::uint64_t> ExecutorCondWakeProbeSequence{};
static std::atomic<std::uint64_t> ExecutorCondWakeProbeGuestReturn{};
static std::atomic_bool ExecutorCondWakeProbeWaiting{};
static std::atomic_bool ExecutorCondWakeProbeQuiescent{};
static std::atomic_bool ExecutorCondWakeProbeWatcherStarted{};
static std::atomic_flag ExecutorCondWakeProbeContextDumped = ATOMIC_FLAG_INIT;

struct ExecutorCondWaitSnapshot {
    PthreadCond* cond{};
    PthreadMutex* mutex{};
};

static std::mutex ExecutorCondWaitSnapshotMutex;
static std::unordered_map<Pthread*, ExecutorCondWaitSnapshot> ExecutorCondWaitSnapshots;

static void RecordCondWaitSnapshot(Pthread* thread, PthreadCond* cond, PthreadMutex* mutex) {
    if (!ExecutorFiosProbeEnabled() || thread == nullptr) {
        return;
    }
    std::scoped_lock lock{ExecutorCondWaitSnapshotMutex};
    ExecutorCondWaitSnapshots[thread] = {cond, mutex};
}

static void EraseCondWaitSnapshot(Pthread* thread, PthreadCond* cond) {
    if (!ExecutorFiosProbeEnabled() || thread == nullptr) {
        return;
    }
    std::scoped_lock lock{ExecutorCondWaitSnapshotMutex};
    const auto it = ExecutorCondWaitSnapshots.find(thread);
    if (it != ExecutorCondWaitSnapshots.end() && it->second.cond == cond) {
        ExecutorCondWaitSnapshots.erase(it);
    }
}

static bool IsCondProbeThread(const std::string& name) {
    return name.find("Game:Main") != std::string::npos ||
           name.find("SceFios") != std::string::npos ||
           name.find("Loading.") != std::string::npos ||
           name.find("UnityPreload") != std::string::npos ||
           name.find("GC Finalizer") != std::string::npos ||
           name.find("Requests Thread") != std::string::npos ||
           name.find("Thread12") != std::string::npos ||
           name.find("Job.Worker") != std::string::npos ||
           name.find("IL2CPP Threadpool") != std::string::npos ||
           name.find("Geo") != std::string::npos ||
           name.find("Enlighten") != std::string::npos;
}

static void DumpCondWaitSnapshots() {
    std::vector<std::pair<Pthread*, ExecutorCondWaitSnapshot>> waits;
    {
        std::scoped_lock lock{ExecutorCondWaitSnapshotMutex};
        waits.reserve(ExecutorCondWaitSnapshots.size());
        for (const auto& entry : ExecutorCondWaitSnapshots) {
            if (entry.first != nullptr && IsCondProbeThread(entry.first->name)) {
                waits.push_back(entry);
            }
        }
    }

    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_COND_WAIT_SNAPSHOT] phase=begin count=%zu", waits.size());
    for (const auto& [thread, wait] : waits) {
        bool wake_posted = false;
        const char* last_wake_site = "";
        {
            std::scoped_lock wake_lock{thread->wake_sema_mutex};
            wake_posted = thread->wake_sema_posted;
            last_wake_site = thread->last_wake_site;
        }
        Pthread* const owner = wait.mutex != nullptr ? wait.mutex->m_owner : nullptr;
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_COND_WAIT_SNAPSHOT] thread=%p name=%s tid=%d cond=%p condName=%s "
            "wchan=%p mutex=%p owner=%p ownerName=%s mutexFlags=0x%x wakePosted=%d "
            "lastWake=%s defers=%d willSleep=%d hleDepth=%u hleFlags=0x%x",
            thread, thread->name.c_str(), thread->tid.load(std::memory_order_relaxed), wait.cond,
            wait.cond ? wait.cond->name.c_str() : "<null>", thread->wchan, wait.mutex, owner,
            owner ? owner->name.c_str() : "<none>",
            wait.mutex ? static_cast<unsigned>(wait.mutex->m_flags) : 0u,
            wake_posted ? 1 : 0, last_wake_site ? last_wake_site : "", thread->nwaiter_defer,
            thread->will_sleep ? 1 : 0,
            thread->fex_hle_atomic_depth.load(std::memory_order_acquire),
            thread->fex_hle_atomic_flags.load(std::memory_order_acquire));
    }
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_COND_WAIT_SNAPSHOT] phase=end");

    auto* const thread_state = ThrState::Instance();
    if (thread_state != nullptr) {
        std::scoped_lock list_lock{thread_state->thread_list_lock};
        __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                            "[EXECUTOR_PTHREAD_WAIT_SNAPSHOT] phase=begin");
        for (Pthread* const thread : thread_state->threads) {
            if (thread == nullptr || !IsCondProbeThread(thread->name)) {
                continue;
            }
            bool wake_posted = false;
            const char* last_wake_site = "";
            {
                std::scoped_lock wake_lock{thread->wake_sema_mutex};
                wake_posted = thread->wake_sema_posted;
                last_wake_site = thread->last_wake_site;
            }
            PthreadMutex* const mutex = thread->mutex_obj;
            Pthread* const owner = mutex != nullptr ? mutex->m_owner : nullptr;
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_PTHREAD_WAIT_SNAPSHOT] thread=%p name=%s tid=%d state=%u "
                "wchan=%p mutex=%p owner=%p ownerName=%s wakePosted=%d lastWake=%s "
                "defers=%d willSleep=%d hleDepth=%u hleFlags=0x%x",
                thread, thread->name.c_str(), thread->tid.load(std::memory_order_relaxed),
                static_cast<unsigned>(thread->state), thread->wchan, mutex, owner,
                owner ? owner->name.c_str() : "<none>", wake_posted ? 1 : 0,
                last_wake_site ? last_wake_site : "", thread->nwaiter_defer,
                thread->will_sleep ? 1 : 0,
                thread->fex_hle_atomic_depth.load(std::memory_order_acquire),
                thread->fex_hle_atomic_flags.load(std::memory_order_acquire));
        }
        __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                            "[EXECUTOR_PTHREAD_WAIT_SNAPSHOT] phase=end");
    }
    if (executor_live_dump_kernel_wait_probe != nullptr) {
        executor_live_dump_kernel_wait_probe();
    }
    if (executor_jit_dump_thread_states != nullptr) {
        executor_jit_dump_thread_states("enlighten-wait-quiescent");
    }
}

static void TraceCondWakeProbe(const char* phase, PthreadCond* cond, Pthread* waiter,
                               PthreadMutex* mutex, bool queued) {
    const auto& target = ExecutorCondWakeProbeThread();
    if (target.empty()) {
        return;
    }
    const std::uint64_t sequence =
        ExecutorCondWakeProbeSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    if (sequence > 64) {
        return;
    }
    bool wake_posted = false;
    if (waiter != nullptr) {
        std::scoped_lock wake_lock{waiter->wake_sema_mutex};
        wake_posted = waiter->wake_sema_posted;
    }
    Pthread* const owner = mutex != nullptr ? mutex->m_owner : nullptr;
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_COND_WAKE_PROBE] seq=%llu phase=%s cond=%p condName=%s "
        "current=%p currentName=%s waiter=%p waiterName=%s waiterWchan=%p "
        "wakePosted=%d mutex=%p owner=%p ownerName=%s ownerDefers=%d flags=0x%x "
        "queued=%d guestReturn=0x%llx",
        static_cast<unsigned long long>(sequence), phase ? phase : "?", cond,
        cond ? cond->name.c_str() : "<null>", g_curthread,
        g_curthread ? g_curthread->name.c_str() : "<none>", waiter,
        waiter ? waiter->name.c_str() : "<none>", waiter ? waiter->wchan : nullptr,
        wake_posted ? 1 : 0, mutex, owner,
        owner ? owner->name.c_str() : "<none>", owner ? owner->nwaiter_defer : 0,
        mutex ? static_cast<unsigned>(mutex->m_flags) : 0u, queued ? 1 : 0,
        static_cast<unsigned long long>(
            ExecutorCondWakeProbeGuestReturn.load(std::memory_order_acquire)));
}

static void StartCondWakeProbeWatcher() {
    if (ExecutorCondWakeProbeWatcherStarted.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    std::thread([] {
        std::uint64_t observed_generation = 0;
        unsigned stable_intervals = 0;
        // Exit 8 reaches its terminal UE frame-fence wait after the first
        // minute on a cold JIT cache.  Keep this diagnostic observer alive
        // long enough to see that wait, without adding work to the wait path.
        for (unsigned interval = 0; interval < 720; ++interval) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const std::uint64_t generation =
                ExecutorCondWakeProbeGeneration.load(std::memory_order_acquire);
            const bool waiting =
                ExecutorCondWakeProbeWaiting.load(std::memory_order_acquire);
            if (waiting && generation == observed_generation) {
                ++stable_intervals;
            } else {
                observed_generation = generation;
                stable_intervals = 0;
                ExecutorCondWakeProbeQuiescent.store(false, std::memory_order_release);
            }
            if (waiting && stable_intervals >= 2 &&
                !ExecutorCondWakeProbeQuiescent.exchange(true,
                                                          std::memory_order_acq_rel)) {
                TraceCondWakeProbe(
                    "quiescent_wait", ExecutorCondWakeProbeTarget.load(std::memory_order_acquire),
                    ExecutorCondWakeProbeWaiter.load(std::memory_order_acquire),
                    ExecutorCondWakeProbeMutex.load(std::memory_order_acquire), true);
                DumpCondWaitSnapshots();
            }
        }
    }).detach();
}

static bool ShouldTraceUnityCond() {
    const bool marker_probe = ExecutorFiosProbeEnabled();
    if (!marker_probe && std::getenv("EXECUTOR_TRACE_LIVE_WIDE") == nullptr &&
        std::getenv("EXECUTOR_TRACE_LIVE_SYNC") == nullptr &&
        std::getenv("EXECUTOR_LIGHT_ORACLE") == nullptr) {
        return false;
    }
    if (!g_curthread) {
        return false;
    }
    const auto& name = g_curthread->name;
    if (marker_probe) {
        return name.find("Game:Main") != std::string::npos ||
               name.find("SceFios") != std::string::npos;
    }
    if (std::getenv("EXECUTOR_TRACE_LIVE_RENDER_THREAD_ONLY") != nullptr) {
        return name.find("Game:Main") != std::string::npos ||
               name.find("UnityGfxDeviceWorker") != std::string::npos ||
               name.find("UnityWorker") != std::string::npos ||
               name.find("Submit Done Thread") != std::string::npos ||
               name.find("SceFios") != std::string::npos;
    }
    return name.find("Game:Main") != std::string::npos ||
           name.find("UnityPreload") != std::string::npos ||
           name.find("mono") != std::string::npos ||
           name.find("Mono") != std::string::npos ||
           name.find("UnityGfxDeviceWorker") != std::string::npos ||
           name.find("UnityWorker") != std::string::npos ||
           name.find("Submit Done Thread") != std::string::npos ||
           name.find("SceFios") != std::string::npos;
}

static void TraceUnityCond(const char* op, PthreadCondT* cond, PthreadCond* native,
                           PthreadMutexT* mutex, s32 ret) {
    if (!ShouldTraceUnityCond()) {
        return;
    }
    if (std::getenv("EXECUTOR_TRACE_LIVE_RENDER_THREAD_ONLY") != nullptr && native &&
        native->name == "SceFiosOpWait" && !native->has_user_waiters &&
        std::strstr(op, "broadcast") == op) {
        static std::atomic_uint fios_broadcast_count{0};
        const unsigned count = fios_broadcast_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((count & (count - 1)) != 0) {
            return;
        }
    }
    static std::atomic_int budget{1024};
    if (budget.fetch_sub(1, std::memory_order_relaxed) <= 0) {
        return;
    }
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_LIVE_COND] op=%s thread=%s cond=%p native=%p mutex=%p "
                        "waiters=%u ret=%d name=%s",
                        op, g_curthread ? g_curthread->name.c_str() : "<no-gcurthread>", cond,
                        native, mutex, native && native->has_user_waiters ? 1u : 0u, ret,
                        native ? native->name.c_str() : "<null>");
}

static void TraceFiosCondDefer(const char* op, PthreadCond* cv, Pthread* waiter,
                               PthreadMutex* mutex, bool deferred) {
    if (!ExecutorFiosProbeEnabled() && std::getenv("EXECUTOR_LIGHT_ORACLE") == nullptr &&
        std::getenv("EXECUTOR_TRACE_LIVE_WIDE") == nullptr &&
        std::getenv("EXECUTOR_TRACE_LIVE_SYNC") == nullptr) {
        return;
    }
    const bool is_fios_cond = cv && cv->name.find("SceFios") != std::string::npos;
    const bool is_fios_cur = g_curthread && g_curthread->name.find("SceFios") != std::string::npos;
    const bool is_fios_waiter = waiter && waiter->name.find("SceFios") != std::string::npos;
    if (!is_fios_cond && !is_fios_cur && !is_fios_waiter) {
        return;
    }
    static std::atomic_int budget{256};
    if (budget.fetch_sub(1, std::memory_order_relaxed) <= 0) {
        return;
    }
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_FIOS_COND_DEFER] op=%s cond=%p condName=%s cur=%p curName=%s "
                        "waiter=%p waiterName=%s mutex=%p owner=%p ownerName=%s flags=0x%x "
                        "deferred=%d curDefer=%d",
                        op ? op : "?", cv, cv ? cv->name.c_str() : "<null>", g_curthread,
                        g_curthread ? g_curthread->name.c_str() : "<no-gcurthread>", waiter,
                        waiter ? waiter->name.c_str() : "<none>", mutex,
                        mutex ? mutex->m_owner : nullptr,
                        mutex && mutex->m_owner ? mutex->m_owner->name.c_str() : "<none>",
                        mutex ? static_cast<u32>(mutex->m_flags) : 0u, deferred ? 1 : 0,
                        g_curthread ? g_curthread->nwaiter_defer : 0);
}
#endif

#define THR_COND_INITIALIZER ((PthreadCond*)nullptr)
#define THR_COND_DESTROYED ((PthreadCond*)1)

static constexpr PthreadCondAttr PthreadCondattrDefault = {
    .c_pshared = 0,
    .c_clockid = ClockId::Realtime,
};

static int CondInit(PthreadCondT* cond, const PthreadCondAttrT* cond_attr, const char* name) {
    auto* cvp = new (std::nothrow) PthreadCond{};
    if (cvp == nullptr) {
        return POSIX_ENOMEM;
    }

    if (name) {
        cvp->name = name;
    } else {
        static int CondId = 0;
        cvp->name = fmt::format("Cond{}", CondId++);
    }

    if (cond_attr == nullptr || *cond_attr == nullptr) {
        cvp->clock_id = ClockId::Realtime;
    } else {
        cvp->clock_id = (*cond_attr)->c_clockid;
    }
    *cond = cvp;
    return 0;
}

static int InitStatic(Pthread* thread, PthreadCondT* cond) {
    std::scoped_lock lk{CondStaticLock};
    if (*cond == nullptr) {
        return CondInit(cond, nullptr, nullptr);
    }
    return 0;
}

#define CHECK_AND_INIT_COND                                                                        \
    if (cvp = *cond; cvp <= THR_COND_DESTROYED) [[unlikely]] {                                     \
        if (cvp == THR_COND_INITIALIZER) {                                                         \
            int ret;                                                                               \
            ret = InitStatic(g_curthread, cond);                                                   \
            if (ret)                                                                               \
                return (ret);                                                                      \
        } else if (cvp == THR_COND_DESTROYED) {                                                    \
            return POSIX_EINVAL;                                                                   \
        }                                                                                          \
        cvp = *cond;                                                                               \
    }

int PS4_SYSV_ABI posix_pthread_cond_init(PthreadCondT* cond, const PthreadCondAttrT* cond_attr) {
    *cond = nullptr;
    return CondInit(cond, cond_attr, nullptr);
}

int PS4_SYSV_ABI scePthreadCondInit(PthreadCondT* cond, const PthreadCondAttrT* cond_attr,
                                    const char* name) {
    *cond = nullptr;
    return CondInit(cond, cond_attr, name);
}

int PS4_SYSV_ABI posix_pthread_cond_destroy(PthreadCondT* cond) {
    PthreadCond* cvp = *cond;
    if (cvp == THR_COND_INITIALIZER) {
        return 0;
    }
    if (cvp == THR_COND_DESTROYED) {
        return POSIX_EINVAL;
    }
    cvp = *cond;
    *cond = THR_COND_DESTROYED;
    delete cvp;
    return 0;
}

int PthreadCond::Wait(PthreadMutexT* mutex, const OrbisKernelTimespec* abstime, u64 usec) {
    PthreadMutex* mp = *mutex;
    if (const int error = mp->IsOwned(g_curthread); error != 0) {
        return error;
    }

    Pthread* curthread = g_curthread;
    ASSERT_MSG(curthread->wchan == nullptr, "Thread was already on queue.");
    SleepqLock(this);

    has_user_waiters = true;
    curthread->will_sleep = true;

    int recurse;
    mp->CvUnlock(&recurse);

    curthread->mutex_obj = mp;
    SleepqAdd(this, curthread);

#ifdef __ANDROID__
    RecordCondWaitSnapshot(curthread, this, mp);
#endif

#ifdef __ANDROID__
    const auto& wake_probe_thread = ExecutorCondWakeProbeThread();
    const bool wake_probe_wait =
        abstime == nullptr && !wake_probe_thread.empty() &&
        (wake_probe_thread == "*" || curthread->name == wake_probe_thread);
    if (wake_probe_wait) {
        std::uint64_t guest_return = 0;
        if (executor_live_get_current_hle_call_site != nullptr) {
            (void)executor_live_get_current_hle_call_site(&guest_return, nullptr, nullptr, nullptr,
                                                          0, nullptr, 0);
        }
        if (guest_return == 0x8003a4a2bULL &&
            executor_live_dump_current_hle_context != nullptr &&
            !ExecutorCondWakeProbeContextDumped.test_and_set(std::memory_order_acq_rel)) {
            executor_live_dump_current_hle_context("cond-wait-entry", guest_return);
        }
        ExecutorCondWakeProbeGuestReturn.store(guest_return, std::memory_order_release);
        ExecutorCondWakeProbeTarget.store(this, std::memory_order_release);
        ExecutorCondWakeProbeWaiter.store(curthread, std::memory_order_release);
        ExecutorCondWakeProbeMutex.store(mp, std::memory_order_release);
        ExecutorCondWakeProbeQuiescent.store(false, std::memory_order_release);
        ExecutorCondWakeProbeWaiting.store(true, std::memory_order_release);
        ExecutorCondWakeProbeGeneration.fetch_add(1, std::memory_order_acq_rel);
        StartCondWakeProbeWatcher();
    }
#endif

    int error = 0;
    for (;;) {
        curthread->ClearWake();
        SleepqUnlock(this);

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (executor_live_mono_run_pending_signal_safe_point) {
            executor_live_mono_run_pending_signal_safe_point();
        }
#endif
        error = curthread->Sleep(abstime, usec) ? 0 : POSIX_ETIMEDOUT;

        SleepqLock(this);
        if (curthread->wchan == nullptr) {
            error = 0;
            break;
        } else if (curthread->ShouldCancel()) {
            SleepQueue* sq = SleepqLookup(this);
            has_user_waiters = SleepqRemove(sq, curthread);
            SleepqUnlock(this);
            curthread->mutex_obj = nullptr;
            mp->CvLock(recurse);
            return 0;
        } else if (error == POSIX_ETIMEDOUT) {
            SleepQueue* sq = SleepqLookup(this);
            has_user_waiters = SleepqRemove(sq, curthread);
            break;
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        {
            const std::uint64_t n =
                curthread->spurious_cond_wakes.fetch_add(1, std::memory_order_relaxed) + 1;
#ifdef __ANDROID__
            if ((n & (n - 1)) == 0) {
                ExecutorCondPersistSpurious(curthread->name.c_str(), n);
            }
#else
            (void)n;
#endif
            SleepqUnlock(this);
#ifdef __ANDROID__
            if (!ExecutorCondSpuriousBackoffDisabled())
#endif
            {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            SleepqLock(this);
            // Signal/cancel can arrive while the queue is unlocked for the
            // backoff. Do not clear that wake and park an already removed
            // waiter. Android and Apple must retain the same queue contract.
            if (curthread->wchan == nullptr) {
                error = 0;
                break;
            }
            if (curthread->ShouldCancel()) {
                SleepQueue* sq = SleepqLookup(this);
                has_user_waiters = SleepqRemove(sq, curthread);
                error = 0;
                break;
            }
            continue;
        }
#else
        UNREACHABLE();
#endif
    }
    SleepqUnlock(this);
    curthread->mutex_obj = nullptr;
#ifdef __ANDROID__
    EraseCondWaitSnapshot(curthread, this);
    if (wake_probe_wait) {
        const bool was_quiescent =
            ExecutorCondWakeProbeQuiescent.exchange(false, std::memory_order_acq_rel);
        ExecutorCondWakeProbeWaiting.store(false, std::memory_order_release);
        ExecutorCondWakeProbeGeneration.fetch_add(1, std::memory_order_acq_rel);
        if (was_quiescent) {
            TraceCondWakeProbe("quiescent_removed", this, curthread, mp, false);
        }
    }
#endif
    const int error2 = mp->CvLock(recurse);
    if (error == 0) {
        error = error2;
    }
    return error;
}

int PS4_SYSV_ABI posix_pthread_cond_wait(PthreadCondT* cond, PthreadMutexT* mutex) {
    PthreadCond* cvp{};
    CHECK_AND_INIT_COND
#ifdef __ANDROID__
    TraceUnityCond("wait_enter", cond, cvp, mutex, 0);
#endif
    const int ret = cvp->Wait(mutex, nullptr);
#ifdef __ANDROID__
    TraceUnityCond("wait_return", cond, cvp, mutex, ret);
#endif
    return ret;
}

int PS4_SYSV_ABI posix_pthread_cond_timedwait(PthreadCondT* cond, PthreadMutexT* mutex,
                                              const OrbisKernelTimespec* abstime) {
    if (abstime == nullptr || abstime->tv_sec < 0 || abstime->tv_nsec < 0 ||
        abstime->tv_nsec >= 1000000000) {
        return POSIX_EINVAL;
    }

    PthreadCond* cvp{};
    CHECK_AND_INIT_COND
#ifdef __ANDROID__
    TraceUnityCond("timedwait_enter", cond, cvp, mutex, 0);
#endif
    const int ret = cvp->Wait(mutex, abstime);
#ifdef __ANDROID__
    TraceUnityCond("timedwait_return", cond, cvp, mutex, ret);
#endif
    return ret;
}

int PS4_SYSV_ABI posix_pthread_cond_reltimedwait_np(PthreadCondT* cond, PthreadMutexT* mutex,
                                                    u64 usec) {
    PthreadCond* cvp{};
    CHECK_AND_INIT_COND
#ifdef __ANDROID__
    TraceUnityCond("reltimedwait_enter", cond, cvp, mutex, 0);
#endif
    const int ret = cvp->Wait(mutex, THR_RELTIME, usec);
#ifdef __ANDROID__
    TraceUnityCond("reltimedwait_return", cond, cvp, mutex, ret);
#endif
    return ret;
}

int PthreadCond::Signal(Pthread* thread) {
    Pthread* curthread = g_curthread;
    if (thread != nullptr) {
        auto* thread_state = ThrState::Instance();
        const int ret = thread_state->FindThread(thread, false);
        if (ret != ORBIS_OK) {
            return ret;
        }
        thread->lock.unlock();
    }

    SleepqLock(this);
    SleepQueue* sq = SleepqLookup(this);
#ifdef __ANDROID__
    if (ExecutorCondWakeProbeTarget.load(std::memory_order_acquire) == this &&
        ExecutorCondWakeProbeQuiescent.load(std::memory_order_acquire)) {
        TraceCondWakeProbe("signal_observed", this,
                           sq != nullptr && !sq->sq_blocked.empty()
                               ? (thread != nullptr ? thread : sq->sq_blocked.front())
                               : thread,
                           sq != nullptr && !sq->sq_blocked.empty()
                               ? (thread != nullptr ? thread : sq->sq_blocked.front())->mutex_obj
                               : nullptr,
                           sq != nullptr);
    }
#endif
    if (sq == nullptr) {
        SleepqUnlock(this);
        return 0;
    }

    Pthread* td = thread ? thread : sq->sq_blocked.front();

    PthreadMutex* mp = td->mutex_obj;
    has_user_waiters = SleepqRemove(sq, td);

    Pthread* wake_thread = nullptr;
    if (mp->m_owner == curthread) {
        if (curthread->nwaiter_defer >= Pthread::MaxDeferWaiters) {
            curthread->WakeAll();
        }
        curthread->defer_waiters[curthread->nwaiter_defer++] = td;
        mp->m_flags |= PthreadMutexFlags::Deferred;
#ifdef __ANDROID__
        TraceFiosCondDefer("signal_defer", this, td, mp, true);
#endif
    } else {
        wake_thread = td;
#ifdef __ANDROID__
        TraceFiosCondDefer("signal_direct", this, td, mp, false);
#endif
    }

    SleepqUnlock(this);
    if (wake_thread != nullptr) {
        wake_thread->PostWake("cond-signal");
    }
    return 0;
}

struct BroadcastArg {
    Pthread* curthread;
    Pthread* waiters[Pthread::MaxDeferWaiters];
    int count;
};

int PthreadCond::Broadcast() {
    BroadcastArg ba;
    ba.curthread = g_curthread;
    ba.count = 0;

    const auto drop_cb = [](Pthread* td, void* arg) {
        auto* ba2 = static_cast<BroadcastArg*>(arg);
        Pthread* curthread = ba2->curthread;
        PthreadMutex* mp = td->mutex_obj;

        if (mp->m_owner == curthread) {
            if (curthread->nwaiter_defer >= Pthread::MaxDeferWaiters) {
                curthread->WakeAll();
            }
            curthread->defer_waiters[curthread->nwaiter_defer++] = td;
            mp->m_flags |= PthreadMutexFlags::Deferred;
#ifdef __ANDROID__
            TraceFiosCondDefer("broadcast_defer", nullptr, td, mp, true);
#endif
        } else {
            if (ba2->count >= Pthread::MaxDeferWaiters) {
                for (int i = 0; i < ba2->count; i++) {
                    ba2->waiters[i]->PostWake("cond-broadcast-batch");
                }
                ba2->count = 0;
            }
            ba2->waiters[ba2->count++] = td;
#ifdef __ANDROID__
            TraceFiosCondDefer("broadcast_direct", nullptr, td, mp, false);
#endif
        }
    };

    SleepqLock(this);
    SleepQueue* sq = SleepqLookup(this);
#ifdef __ANDROID__
    if (ExecutorCondWakeProbeTarget.load(std::memory_order_acquire) == this &&
        ExecutorCondWakeProbeQuiescent.load(std::memory_order_acquire)) {
        Pthread* const waiter =
            sq != nullptr && !sq->sq_blocked.empty() ? sq->sq_blocked.front() : nullptr;
        TraceCondWakeProbe("broadcast_observed", this, waiter,
                           waiter != nullptr ? waiter->mutex_obj : nullptr, sq != nullptr);
    }
#endif
    if (sq == nullptr) {
        SleepqUnlock(this);
        return 0;
    }

    SleepqDrop(sq, drop_cb, &ba);
    has_user_waiters = false;
    SleepqUnlock(this);

    for (int i = 0; i < ba.count; i++) {
        ba.waiters[i]->PostWake("cond-broadcast");
    }
    return 0;
}

int PS4_SYSV_ABI posix_pthread_cond_signal(PthreadCondT* cond) {
    PthreadCond* cvp{};
    CHECK_AND_INIT_COND
    const int ret = cvp->Signal(nullptr);
#ifdef __ANDROID__
    TraceUnityCond("signal", cond, cvp, nullptr, ret);
#endif
    return ret;
}

int PS4_SYSV_ABI posix_pthread_cond_signalto_np(PthreadCondT* cond, Pthread* thread) {
    PthreadCond* cvp{};
    CHECK_AND_INIT_COND
    const int ret = cvp->Signal(thread);
#ifdef __ANDROID__
    TraceUnityCond("signalto", cond, cvp, nullptr, ret);
#endif
    return ret;
}

int PS4_SYSV_ABI posix_pthread_cond_broadcast(PthreadCondT* cond) {
    PthreadCond* cvp{};
    CHECK_AND_INIT_COND
#ifdef __ANDROID__
    TraceUnityCond("broadcast_enter", cond, cvp, nullptr, 0);
#endif
    const int ret = cvp->Broadcast();
#ifdef __ANDROID__
    TraceUnityCond("broadcast_return", cond, cvp, nullptr, ret);
#endif
    return 0;
}

int PS4_SYSV_ABI posix_pthread_condattr_init(PthreadCondAttrT* attr) {
    auto* pattr = new (std::nothrow) PthreadCondAttr{};
    if (pattr == nullptr) {
        return POSIX_ENOMEM;
    }
    memcpy(pattr, &PthreadCondattrDefault, sizeof(PthreadCondAttr));
    *attr = pattr;
    return 0;
}

int PS4_SYSV_ABI posix_pthread_condattr_destroy(PthreadCondAttrT* attr) {
    if (attr == nullptr || *attr == nullptr) {
        return POSIX_EINVAL;
    }
    delete *attr;
    *attr = nullptr;
    return 0;
}

int PS4_SYSV_ABI posix_pthread_condattr_getclock(const PthreadCondAttrT* attr, ClockId* clock_id) {
    if (attr == nullptr || *attr == nullptr) {
        return POSIX_EINVAL;
    }
    *clock_id = (*attr)->c_clockid;
    return 0;
}

int PS4_SYSV_ABI posix_pthread_condattr_setclock(PthreadCondAttrT* attr, ClockId clock_id) {
    if (attr == nullptr || *attr == nullptr) {
        return POSIX_EINVAL;
    }
    if (clock_id != ClockId::Realtime && clock_id != ClockId::Virtual &&
        clock_id != ClockId::Prof && clock_id != ClockId::Monotonic) {
        return POSIX_EINVAL;
    }
    (*attr)->c_clockid = clock_id;
    return 0;
}

int PS4_SYSV_ABI posix_pthread_condattr_getpshared(const PthreadCondAttrT* attr, int* pshared) {
    if (attr == nullptr || *attr == nullptr) {
        return POSIX_EINVAL;
    }
    *pshared = 0;
    return 0;
}

int PS4_SYSV_ABI posix_pthread_condattr_setpshared(PthreadCondAttrT* attr, int pshared) {
    if (attr == nullptr || *attr == nullptr) {
        return POSIX_EINVAL;
    }
    if (pshared != 0) {
        return POSIX_EINVAL;
    }
    return 0;
}

void RegisterCond(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("mKoTx03HRWA", "libScePosix", 1, "libkernel", posix_pthread_condattr_init);
    LIB_FUNCTION("3BpP850hBT4", "libScePosix", 1, "libkernel",
                 posix_pthread_condattr_setpshared);
    LIB_FUNCTION("dJcuQVn6-Iw", "libScePosix", 1, "libkernel", posix_pthread_condattr_destroy);
    LIB_FUNCTION("EjllaAqAPZo", "libScePosix", 1, "libkernel", posix_pthread_condattr_setclock);
    LIB_FUNCTION("h0qUqSuOmC8", "libScePosix", 1, "libkernel",
                 posix_pthread_condattr_getpshared);
    LIB_FUNCTION("cTDYxTUNPhM", "libScePosix", 1, "libkernel",
                 posix_pthread_condattr_getclock);
    LIB_FUNCTION("0TyVk4MSLt0", "libScePosix", 1, "libkernel", posix_pthread_cond_init);
    LIB_FUNCTION("K953PF5u6Pc", "libScePosix", 1, "libkernel",
                 posix_pthread_cond_reltimedwait_np);
    LIB_FUNCTION("2MOy+rUfuhQ", "libScePosix", 1, "libkernel", posix_pthread_cond_signal);
    LIB_FUNCTION("CI6Qy73ae10", "libScePosix", 1, "libkernel",
                 posix_pthread_cond_signalto_np);
    LIB_FUNCTION("RXXqi4CtF8w", "libScePosix", 1, "libkernel", posix_pthread_cond_destroy);
    LIB_FUNCTION("Op8TBGY5KHg", "libScePosix", 1, "libkernel", posix_pthread_cond_wait);
    LIB_FUNCTION("27bAgiJmOh0", "libScePosix", 1, "libkernel", posix_pthread_cond_timedwait);
    LIB_FUNCTION("mkx2fVhNMsg", "libScePosix", 1, "libkernel", posix_pthread_cond_broadcast);

    LIB_FUNCTION("0TyVk4MSLt0", "libkernel", 1, "libkernel", posix_pthread_cond_init);
    LIB_FUNCTION("3BpP850hBT4", "libkernel", 1, "libkernel", posix_pthread_condattr_setpshared);
    LIB_FUNCTION("EjllaAqAPZo", "libkernel", 1, "libkernel", posix_pthread_condattr_setclock);
    LIB_FUNCTION("h0qUqSuOmC8", "libkernel", 1, "libkernel", posix_pthread_condattr_getpshared);
    LIB_FUNCTION("cTDYxTUNPhM", "libkernel", 1, "libkernel", posix_pthread_condattr_getclock);
    LIB_FUNCTION("K953PF5u6Pc", "libkernel", 1, "libkernel",
                 posix_pthread_cond_reltimedwait_np);
    LIB_FUNCTION("Op8TBGY5KHg", "libkernel", 1, "libkernel", posix_pthread_cond_wait);
    LIB_FUNCTION("mkx2fVhNMsg", "libkernel", 1, "libkernel", posix_pthread_cond_broadcast);
    LIB_FUNCTION("2MOy+rUfuhQ", "libkernel", 1, "libkernel", posix_pthread_cond_signal);
    LIB_FUNCTION("CI6Qy73ae10", "libkernel", 1, "libkernel", posix_pthread_cond_signalto_np);
    LIB_FUNCTION("RXXqi4CtF8w", "libkernel", 1, "libkernel", posix_pthread_cond_destroy);
    LIB_FUNCTION("27bAgiJmOh0", "libkernel", 1, "libkernel", posix_pthread_cond_timedwait);
    LIB_FUNCTION("mKoTx03HRWA", "libkernel", 1, "libkernel", posix_pthread_condattr_init);
    LIB_FUNCTION("dJcuQVn6-Iw", "libkernel", 1, "libkernel", posix_pthread_condattr_destroy);

    LIB_FUNCTION("2Tb92quprl0", "libkernel", 1, "libkernel", ORBIS(scePthreadCondInit));
    LIB_FUNCTION("m5-2bsNfv7s", "libkernel", 1, "libkernel", ORBIS(posix_pthread_condattr_init));
    LIB_FUNCTION("6xMew9+rZwI", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_condattr_setpshared));
    LIB_FUNCTION("c-bxj027czs", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_condattr_setclock));
    LIB_FUNCTION("Dn-DRWi9t54", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_condattr_getpshared));
    LIB_FUNCTION("6qM3kO5S3Oo", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_condattr_getclock));
    LIB_FUNCTION("JGgj7Uvrl+A", "libkernel", 1, "libkernel", ORBIS(posix_pthread_cond_broadcast));
    LIB_FUNCTION("WKAXJ4XBPQ4", "libkernel", 1, "libkernel", ORBIS(posix_pthread_cond_wait));
    LIB_FUNCTION("waPcxYiR3WA", "libkernel", 1, "libkernel", ORBIS(posix_pthread_condattr_destroy));
    LIB_FUNCTION("kDh-NfxgMtE", "libkernel", 1, "libkernel", ORBIS(posix_pthread_cond_signal));
    LIB_FUNCTION("BmMjYxmew1w", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_cond_reltimedwait_np));
    LIB_FUNCTION("g+PZd2hiacg", "libkernel", 1, "libkernel", ORBIS(posix_pthread_cond_destroy));
    LIB_FUNCTION("o69RpYO-Mu0", "libkernel", 1, "libkernel", ORBIS(posix_pthread_cond_signalto_np));
}

}
