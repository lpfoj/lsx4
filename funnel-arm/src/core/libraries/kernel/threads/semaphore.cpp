#if defined(LSX4_APPLE_RUNTIME)
#include "AppleHostServices.h"
#define LSX4_SEM_HOST_TID() Lsx4::Apple::HostThreadId()
#else
#define LSX4_SEM_HOST_TID() ::syscall(SYS_gettid)
#endif
// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <condition_variable>
#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <semaphore>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
#include <android/log.h>
#if !defined(__APPLE__)
#include <sys/syscall.h>
#endif
#include <unistd.h>

extern "C" void executor_lsx4_android_dump_box64_emu_states(const char* reason)
    __attribute__((weak));
extern "C" int executor_live_get_current_hle_call_site(std::uint64_t* guest_return,
                                                       std::uint64_t* return_off,
                                                       std::uint64_t* arg0,
                                                       char* symbol,
                                                       std::size_t symbol_size,
                                                       char* module,
                                                       std::size_t module_size)
    __attribute__((weak));
extern "C" void executor_live_dump_current_hle_context(const char* reason,
                                                       std::uint64_t focus)
    __attribute__((weak));
extern "C" void executor_jit_dump_thread_states(const char* reason) __attribute__((weak));
extern "C" void executor_live_mono_notify_pending_event_flag_waiter(void* guest_thread)
    __attribute__((weak));

static std::atomic<std::uintptr_t> g_executor_unity_gfx_worker{0};

extern "C" void executor_live_note_unity_gfx_worker(void* worker) {
    const auto value = reinterpret_cast<std::uintptr_t>(worker);
    g_executor_unity_gfx_worker.store(value, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_UNITY_GFX_WORKER_NOTE] worker=%p", worker);
}

static bool ExecutorLightOracleMode() {
    const char* value = std::getenv("EXECUTOR_LIGHT_ORACLE");
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}
#endif

#include "core/libraries/kernel/sync/semaphore.h"

#include "common/logging/log.h"
#include "core/memory.h"
#include "common/slot_vector.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
#include "video_core/amdgpu/liverpool.h"
#endif

namespace Libraries::Kernel {

constexpr s32 ORBIS_KERNEL_SEM_VALUE_MAX = 0x7FFFFFFF;

struct PthreadSem {
    explicit PthreadSem(s32 value_)
        :
#if !defined(__ANDROID__) && !defined(LSX4_APPLE_RUNTIME)
          semaphore{value_},
#endif
          value{value_} {
    }

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    std::mutex wait_mutex;
    std::condition_variable wait_cv;
    u32 waiters = 0;
    u64 signal_generation = 0;
    std::vector<Pthread*> signal_waiters;
#else
    CountingSemaphore semaphore;
#endif
    std::atomic<s32> value;
};

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
struct ExecutorPosixWaitProbe {
    PthreadSem** slot{};
    PthreadSem* native{};
    s32 before{};
    std::uint64_t guest_return{};
    std::uint64_t return_off{};
    std::string symbol;
    std::string module;
};

static std::mutex g_executor_posix_wait_probe_lock;
static std::unordered_map<Pthread*, ExecutorPosixWaitProbe> g_executor_posix_wait_probes;

static bool ExecutorPosixWaitProbeEnabled() {
    static const bool enabled = [] {
        std::FILE* file = std::fopen(
            "/data/data/app.lsx4.android/files/lsx4-home/diag-kernel-cond-wake", "r");
        if (file == nullptr) {
            return false;
        }
        std::fclose(file);
        return true;
    }();
    return enabled;
}

static bool IsPosixWaitProbeThread(const Pthread* thread) {
    if (thread == nullptr) {
        return false;
    }
    const auto& name = thread->name;
    return name.find("Game:Main") != std::string::npos ||
           name.find("Loading.") != std::string::npos ||
           name.find("UnityPreload") != std::string::npos ||
           name.find("Job.Worker") != std::string::npos ||
           name.find("IL2CPP Threadpool") != std::string::npos ||
           name.find("GC Finalizer") != std::string::npos ||
           name.find("Requests Thread") != std::string::npos ||
           name.find("Thread12") != std::string::npos ||
           name.find("Geo") != std::string::npos ||
           name.find("Enlighten") != std::string::npos;
}

static void RecordPosixWaitProbe(Pthread* thread, PthreadSem** slot, PthreadSem* native,
                                 s32 before) {
    if (!ExecutorPosixWaitProbeEnabled() || !IsPosixWaitProbeThread(thread)) {
        return;
    }
    std::uint64_t guest_return = 0;
    std::uint64_t return_off = 0;
    std::uint64_t arg0 = 0;
    char symbol[128]{};
    char module[128]{};
    if (executor_live_get_current_hle_call_site != nullptr) {
        (void)executor_live_get_current_hle_call_site(
            &guest_return, &return_off, &arg0, symbol, sizeof(symbol), module, sizeof(module));
    }
    std::scoped_lock lock{g_executor_posix_wait_probe_lock};
    g_executor_posix_wait_probes[thread] = {
        slot, native, before, guest_return, return_off, symbol, module};
}

static void ErasePosixWaitProbe(Pthread* thread) {
    if (!ExecutorPosixWaitProbeEnabled() || thread == nullptr) {
        return;
    }
    std::scoped_lock lock{g_executor_posix_wait_probe_lock};
    g_executor_posix_wait_probes.erase(thread);
}

static bool PostPthreadSemToken(PthreadSem* native, s32* before_out = nullptr) {
    bool wake_waiter = false;
    {
        std::lock_guard lock{native->wait_mutex};
        const s32 before = native->value.load(std::memory_order_relaxed);
        if (before_out != nullptr) {
            *before_out = before;
        }
        if (before == ORBIS_KERNEL_SEM_VALUE_MAX) {
            return false;
        }
        native->value.store(before + 1, std::memory_order_release);
        wake_waiter = native->waiters != 0;
    }
    if (wake_waiter) {
        native->wait_cv.notify_one();
    }
    return true;
}

static u32 ExecutorEopTraceThreadKind() {
    if (!g_curthread) {
        return 0;
    }
    if (g_curthread->name == "Game:Main") {
        return 1;
    }
    if (g_curthread->name.find("UnityGfxDeviceWorker") != std::string::npos) {
        return 2;
    }
    return 0;
}
#endif

static std::mutex pthread_sem_pool_mutex;
static std::vector<std::unique_ptr<PthreadSem>> pthread_sem_pool;
static std::array<std::mutex, 64> pthread_sem_slot_mutexes;

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
static void NotifyPthreadSemPendingSignalWaiter(Pthread* const guest_thread) {
    if (guest_thread == nullptr) {
        return;
    }
    std::vector<PthreadSem*> wake;
    {
        std::scoped_lock pool_lock{pthread_sem_pool_mutex};
        wake.reserve(pthread_sem_pool.size());
        for (const auto& entry : pthread_sem_pool) {
            PthreadSem* const native = entry.get();
            if (native == nullptr) {
                continue;
            }
            std::scoped_lock wait_lock{native->wait_mutex};
            if (std::find(native->signal_waiters.begin(), native->signal_waiters.end(),
                          guest_thread) ==
                native->signal_waiters.end()) {
                continue;
            }
            ++native->signal_generation;
            wake.push_back(native);
        }
    }
    for (PthreadSem* const native : wake) {
        native->wait_cv.notify_all();
    }
}
#endif

static std::mutex& PthreadSemSlotMutex(PthreadSem** sem) {
    const auto address = reinterpret_cast<std::uintptr_t>(sem);
    return pthread_sem_slot_mutexes[(address >> 2) % pthread_sem_slot_mutexes.size()];
}

static PthreadSem* LoadPthreadSem(PthreadSem** sem) {
    if (sem == nullptr) {
        return nullptr;
    }
    std::scoped_lock lock{PthreadSemSlotMutex(sem)};
    PthreadSem* native;
    std::memcpy(&native, sem, sizeof(native));
    return native;
}

static void StorePthreadSem(PthreadSem** sem, PthreadSem* native) {
    std::scoped_lock lock{PthreadSemSlotMutex(sem)};
    std::memcpy(sem, &native, sizeof(native));
}

static PthreadSem* DetachPthreadSem(PthreadSem** sem) {
    if (sem == nullptr) {
        return nullptr;
    }
    std::scoped_lock lock{PthreadSemSlotMutex(sem)};
    PthreadSem* native;
    std::memcpy(&native, sem, sizeof(native));
    PthreadSem* const empty = nullptr;
    std::memcpy(sem, &empty, sizeof(empty));
    return native;
}

class OrbisSem;

#if !defined(__ANDROID__) && !defined(LSX4_APPLE_RUNTIME)
static bool PcOracleMonoSyncEnabled() {
    const char* value = std::getenv("EXECUTOR_PC_ORACLE_MONO_SYNC");
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

static const char* PcOracleCurrentThreadName() {
    return g_curthread && !g_curthread->name.empty() ? g_curthread->name.c_str() : "<none>";
}

static std::uint64_t PcOracleReturnOff(const void* guest_return) {
    const auto return_va = reinterpret_cast<std::uintptr_t>(guest_return);
    return return_va >= 0x800000000ULL ? return_va - 0x800000000ULL : 0ULL;
}

static bool PcOracleTryReadQword(const void* base, std::size_t index, std::uint64_t& out) {
    out = 0;
    auto* memory = Core::Memory::Instance();
    const auto addr = reinterpret_cast<VAddr>(base) + index * sizeof(out);
    if (base == nullptr || memory == nullptr || !memory->IsValidMapping(addr, sizeof(out))) {
        return false;
    }
    memory->CopySparseMemory(addr, reinterpret_cast<u8*>(&out), sizeof(out));
    return true;
}

static std::string PcOracleBytesHex(const void* base, std::size_t size) {
    auto* memory = Core::Memory::Instance();
    const auto addr = reinterpret_cast<VAddr>(base);
    if (base == nullptr || memory == nullptr || !memory->IsValidMapping(addr, size)) {
        return "<invalid>";
    }
    std::vector<u8> bytes(size);
    memory->CopySparseMemory(addr, bytes.data(), size);
    static constexpr char Hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 3);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) {
            out.push_back(' ');
        }
        const auto byte = bytes[i];
        out.push_back(Hex[(byte >> 4) & 0xf]);
        out.push_back(Hex[byte & 0xf]);
    }
    return out;
}

static void PcOraclePosixSemLog(const char* op, PthreadSem** slot, PthreadSem* sem, s32 before,
                                s32 after, s32 ret, const void* guest_return) {
    static std::atomic_int budget{8192};
    if (!PcOracleMonoSyncEnabled() || budget.fetch_sub(1, std::memory_order_relaxed) <= 0) {
        return;
    }
    std::fprintf(stderr,
                 "[EXECUTOR_PC_PSEM] op=%s threadName=%s slot=%p native=%p before=%d after=%d "
                 "ret=%d retaddr=%p returnOff=0x%llx\n",
                 op, PcOracleCurrentThreadName(), slot, sem, before, after, ret, guest_return,
                 static_cast<unsigned long long>(PcOracleReturnOff(guest_return)));
    std::fflush(stderr);
}

static void PcOraclePosixSemInvalidLog(const char* op, PthreadSem** slot,
                                       const void* guest_return) {
    static std::atomic_int budget{256};
    if (!PcOracleMonoSyncEnabled() || budget.fetch_sub(1, std::memory_order_relaxed) <= 0) {
        return;
    }
    std::uint64_t q0 = 0;
    std::uint64_t q1 = 0;
    const bool q0_ok = PcOracleTryReadQword(slot, 0, q0);
    const bool q1_ok = PcOracleTryReadQword(slot, 1, q1);
    const auto address = reinterpret_cast<std::uintptr_t>(slot);
    const auto window = address >= 0x20 ? reinterpret_cast<const void*>(address - 0x20) : slot;
    std::fprintf(stderr,
                 "[EXECUTOR_PC_PSEM_INVALID] op=%s threadName=%s slot=%p q0=0x%llx q0Ok=%d "
                 "q1=0x%llx q1Ok=%d retaddr=%p returnOff=0x%llx window=%p bytes=%s\n",
                 op, PcOracleCurrentThreadName(), slot, static_cast<unsigned long long>(q0),
                 q0_ok ? 1 : 0, static_cast<unsigned long long>(q1), q1_ok ? 1 : 0,
                 guest_return, static_cast<unsigned long long>(PcOracleReturnOff(guest_return)),
                 window, PcOracleBytesHex(window, 0x80).c_str());
    std::fflush(stderr);
}
#endif

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
class OrbisSem;

extern "C" void executor_live_mono_run_pending_signal_safe_point() __attribute__((weak));
extern "C" bool executor_lsx4_android_jit_active() __attribute__((weak));

static bool UseJitNativeSemaphorePark() {
    if (executor_lsx4_android_jit_active == nullptr ||
        !executor_lsx4_android_jit_active()) {
        return false;
    }
    // A guest thread blocked inside an HLE semaphore wait is still an
    // interruptible PS4 thread. Keep the native event-driven park only for
    // ordinary host-side waits; committed FEX HLE waits must register with the
    // pending-signal notifier so Mono/IL2CPP stop-the-world SIGUSR1 delivery
    // can run on the owning guest thread. This does not add polling: the
    // signal-aware path parks on the same condition variable and wakes only on
    // a semaphore post or an emulated signal notification.
    if (g_curthread != nullptr &&
        g_curthread->fex_hle_atomic_depth.load(std::memory_order_acquire) != 0) {
        return false;
    }
    return true;
}

static void RunMonoPendingSignalSafePointFromWait() {
    if (executor_live_mono_run_pending_signal_safe_point != nullptr) {
        executor_live_mono_run_pending_signal_safe_point();
    }
}

static bool ExecutorEnvFlag(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

static bool ShouldBypassLiveMonoSemWait(PthreadSem** sem) {
    if (!ExecutorEnvFlag("EXECUTOR_LIVE_MONO_POSIX_SEM_WAIT_BYPASS") || sem == nullptr) {
        return false;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(sem);
    const bool module_data = address >= 0x800000000ULL && address < 0x810000000ULL;
    if (!module_data) {
        return false;
    }
    const std::string thread_name = g_curthread ? g_curthread->name : "";
    return thread_name.find("mono") != std::string::npos ||
           thread_name.find("Mono") != std::string::npos ||
           thread_name.find("Finalizer") != std::string::npos ||
           thread_name.find("Thread Pool") != std::string::npos ||
           thread_name.find("Game:Main") != std::string::npos;
}

static bool IsLiveRenderThreadName(const std::string& name) {
    return name.find("UnityGfxDeviceWorker") != std::string::npos ||
           name.find("UnityWorker") != std::string::npos ||
           name.find("Game:Main") != std::string::npos ||
           name.find("UnityPreload") != std::string::npos ||
           name.find("Submit Done Thread") != std::string::npos;
}

static bool IsLiveInterestingThreadName(const std::string& name) {
    if (ExecutorEnvFlag("EXECUTOR_TRACE_LIVE_RENDER_THREAD_ONLY")) {
        return IsLiveRenderThreadName(name);
    }
    if (ExecutorEnvFlag("EXECUTOR_LIGHT_ORACLE") &&
        (name.find("FMOD") != std::string::npos ||
         name.find("AudioOut") != std::string::npos ||
         name.find("audio") != std::string::npos ||
         name.find("Audio") != std::string::npos)) {
        return false;
    }
    return IsLiveRenderThreadName(name) ||
           name.find("Gamedata_Worker") != std::string::npos ||
           name.find("Task_Bagger") != std::string::npos ||
           name.find("FMOD") != std::string::npos ||
           name.find("mono") != std::string::npos || name.find("Mono") != std::string::npos ||
           name.find("Finalizer") != std::string::npos ||
           name.find("Thread Pool") != std::string::npos;
}

static bool IsLiveKnownNoisyThreadName(const std::string& name) {
    return name.find("FMOD") != std::string::npos ||
           name.find("AudioOut") != std::string::npos ||
           name.find("audio") != std::string::npos ||
           name.find("Audio") != std::string::npos;
}

static bool ShouldTraceLiveSync() {
    static const bool enabled = ExecutorEnvFlag("EXECUTOR_TRACE_LIVE_WIDE") ||
                                ExecutorEnvFlag("EXECUTOR_TRACE_LIVE_SYNC") ||
                                ExecutorEnvFlag("EXECUTOR_LIGHT_ORACLE");
    return enabled;
}

static bool ShouldRecordKernelSemFrontier() {
    static const bool enabled = ShouldTraceLiveSync() ||
                                ExecutorEnvFlag("EXECUTOR_SYNC_FRONTIER") ||
                                ExecutorEnvFlag("EXECUTOR_LIVE_HLE_FLIGHT");
    return enabled;
}

static bool ShouldRecordPosixSemFrontier() {
    static const bool enabled = ShouldTraceLiveSync() ||
                                ExecutorEnvFlag("EXECUTOR_SYNC_FRONTIER") ||
                                ExecutorEnvFlag("EXECUTOR_LIVE_HLE_FLIGHT");
    return enabled;
}

static bool ShouldTraceLiveDirectPosixSem(PthreadSem** sem) {
    if (!ExecutorEnvFlag("EXECUTOR_TRACE_LIVE_POSIX_DIRECT_SEM") || sem == nullptr) {
        return false;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(sem);
    return address >= 0x200000000ULL && address < 0x400000000ULL;
}

static bool IsCurrentLiveInterestingThread() {
    return ShouldTraceLiveSync() && g_curthread && IsLiveInterestingThreadName(g_curthread->name);
}

static std::string GuestBytesHex(u64 address, std::size_t size) {
    auto* memory = Core::Memory::Instance();
    if (memory == nullptr || address == 0 || size == 0 || !memory->IsValidMapping(address, size)) {
        return "<unmapped>";
    }

    std::vector<u8> bytes(size);
    memory->CopySparseMemory(address, bytes.data(), size);
    static constexpr char Hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 3);
    for (std::size_t i = 0; i < bytes.size(); i++) {
        if (i != 0) {
            out.push_back(' ');
        }
        out.push_back(Hex[bytes[i] >> 4]);
        out.push_back(Hex[bytes[i] & 0xf]);
    }
    return out;
}

static u64 GuestReadQword(u64 address, bool* ok = nullptr) {
    if (ok != nullptr) {
        *ok = false;
    }
    auto* memory = Core::Memory::Instance();
    if (memory == nullptr || address == 0 || !memory->IsValidMapping(address, sizeof(u64))) {
        return 0;
    }
    u64 value = 0;
    memory->CopySparseMemory(address, reinterpret_cast<u8*>(&value), sizeof(value));
    if (ok != nullptr) {
        *ok = true;
    }
    return value;
}

static u32 GuestReadDword(u64 address, bool* ok = nullptr) {
    if (ok != nullptr) {
        *ok = false;
    }
    auto* memory = Core::Memory::Instance();
    if (memory == nullptr || address == 0 || !memory->IsValidMapping(address, sizeof(u32))) {
        return 0;
    }
    u32 value = 0;
    memory->CopySparseMemory(address, reinterpret_cast<u8*>(&value), sizeof(value));
    if (ok != nullptr) {
        *ok = true;
    }
    return value;
}

static u8 GuestReadByte(u64 address, bool* ok = nullptr) {
    if (ok != nullptr) {
        *ok = false;
    }
    auto* memory = Core::Memory::Instance();
    if (memory == nullptr || address == 0 || !memory->IsValidMapping(address, sizeof(u8))) {
        return 0;
    }
    u8 value = 0;
    memory->CopySparseMemory(address, &value, sizeof(value));
    if (ok != nullptr) {
        *ok = true;
    }
    return value;
}

static long long SignedDelta(std::uintptr_t lhs, std::uintptr_t rhs) {
    if (lhs >= rhs) {
        return static_cast<long long>(lhs - rhs);
    }
    const auto diff = rhs - lhs;
    return -static_cast<long long>(diff);
}

static bool IsWatchedPosixSem(PthreadSem** sem, bool mark) {
    static std::atomic<std::uintptr_t> watched[64]{};
    const auto value = reinterpret_cast<std::uintptr_t>(sem);
    if (value == 0) {
        return false;
    }
    for (auto& slot : watched) {
        if (slot.load(std::memory_order_relaxed) == value) {
            return true;
        }
    }
    if (!mark) {
        return false;
    }
    for (auto& slot : watched) {
        std::uintptr_t expected = 0;
        if (slot.compare_exchange_strong(expected, value, std::memory_order_relaxed) ||
            expected == value) {
            return true;
        }
    }
    return false;
}

static bool IsWatchedKernelSem(Common::SlotId sem, bool mark) {
    static std::atomic<u32> watched[128]{};
    const auto value = sem.index + 1;
    if (value == 0) {
        return false;
    }
    for (auto& slot : watched) {
        if (slot.load(std::memory_order_relaxed) == value) {
            return true;
        }
    }
    if (!mark) {
        return false;
    }
    for (auto& slot : watched) {
        u32 expected = 0;
        if (slot.compare_exchange_strong(expected, value, std::memory_order_relaxed) ||
            expected == value) {
            return true;
        }
    }
    return false;
}

struct LivePosixSemRecord {
    std::uintptr_t sem{};
    std::uintptr_t native{};
    s32 init_value{};
    s32 last_before{};
    s32 last_after{};
    s32 last_ret{};
    std::uint32_t init_count{};
    std::uint32_t wait_enter_count{};
    std::uint32_t wait_return_count{};
    std::uint32_t post_count{};
    std::uint32_t trywait_count{};
    std::uint32_t timedwait_count{};
    std::uint32_t invalid_count{};
    long init_tid{};
    long last_tid{};
    std::string init_thread;
    std::string last_thread;
    std::string last_op;
    std::uint64_t init_guest_off{};
    std::uint64_t last_wait_guest_off{};
    std::uint64_t last_post_guest_off{};
    std::string init_guest_module;
    std::string last_wait_guest_module;
    std::string last_post_guest_module;
};

struct LiveKernelSemRecord {
    u32 slot{};
    std::uintptr_t native{};
    s32 init_value{};
    s32 max_value{};
    s32 last_before{};
    s32 last_after{};
    s32 last_count{};
    s32 last_ret{};
    std::uint32_t create_count{};
    std::uint32_t wait_enter_count{};
    std::uint32_t wait_return_count{};
    std::uint32_t signal_count{};
    std::uint32_t poll_count{};
    std::uint32_t cancel_count{};
    std::uint32_t invalid_count{};
    long init_tid{};
    long last_tid{};
    std::string init_thread;
    std::string last_thread;
    std::string last_op;
    std::string name;
};

static std::mutex& LivePosixSemRecordsMutex() {
    static std::mutex mutex;
    return mutex;
}

static std::unordered_map<std::uintptr_t, LivePosixSemRecord>& LivePosixSemRecords() {
    static std::unordered_map<std::uintptr_t, LivePosixSemRecord> records;
    return records;
}

static std::string ExecutorSemModuleName(const char* module_name) {
    if (module_name == nullptr || module_name[0] == '\0') {
        return "<unknown>";
    }
    const std::string value{module_name};
    const auto slash = value.find_last_of("/\\");
    return slash == std::string::npos ? value : value.substr(slash + 1);
}

extern "C" void executor_live_record_posix_sem_hle_site(const char* op, void* sem_addr,
                                                        std::uint64_t guest_return,
                                                        std::uint64_t module_offset,
                                                        const char* module_name) {
    if (!ShouldRecordPosixSemFrontier() || sem_addr == nullptr || op == nullptr) {
        return;
    }
    const auto key = reinterpret_cast<std::uintptr_t>(sem_addr);
    const std::string thread_name = g_curthread ? g_curthread->name : "<no-gcurthread>";
    const long tid = static_cast<long>(LSX4_SEM_HOST_TID());
    const auto module = ExecutorSemModuleName(module_name);
    {
        std::lock_guard lock{LivePosixSemRecordsMutex()};
        auto& record = LivePosixSemRecords()[key];
        record.sem = key;
        const std::string_view opname{op};
        if (opname == "init") {
            record.init_guest_off = module_offset;
            record.init_guest_module = module;
        } else if (opname.find("wait") != std::string_view::npos) {
            record.last_wait_guest_off = module_offset;
            record.last_wait_guest_module = module;
        } else if (opname == "post") {
            record.last_post_guest_off = module_offset;
            record.last_post_guest_module = module;
        }
    }
    static std::atomic_int budget{2048};
    if (ShouldTraceLiveSync() && budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_LIVE_SEM_SITE] op=%s sem=%p ret=%p off=0x%llx module=%s "
            "thread=%s tid=%ld",
            op, sem_addr, reinterpret_cast<void*>(guest_return),
            static_cast<unsigned long long>(module_offset), module.c_str(), thread_name.c_str(),
            tid);
    }
}

extern "C" void executor_live_dump_posix_sem_records(const char* reason) {
    if (!ShouldRecordPosixSemFrontier()) {
        return;
    }
    struct Snapshot {
        LivePosixSemRecord record;
        s32 pending{};
    };
    std::vector<Snapshot> snapshots;
    snapshots.reserve(64);
    {
        std::lock_guard lock{LivePosixSemRecordsMutex()};
        for (const auto& [_, record] : LivePosixSemRecords()) {
            const s32 pending = static_cast<s32>(record.wait_enter_count) -
                                static_cast<s32>(record.wait_return_count);
            const bool render_sem =
                record.init_thread.find("Unity") != std::string::npos ||
                record.last_thread.find("Unity") != std::string::npos ||
                record.last_thread.find("Game:Main") != std::string::npos ||
                record.last_thread.find("Submit Done Thread") != std::string::npos ||
                pending > 0;
            if (render_sem) {
                snapshots.push_back(Snapshot{record, pending});
            }
        }
    }
    std::sort(snapshots.begin(), snapshots.end(), [](const Snapshot& lhs, const Snapshot& rhs) {
        if (lhs.pending != rhs.pending) {
            return lhs.pending > rhs.pending;
        }
        if (lhs.record.wait_enter_count != rhs.record.wait_enter_count) {
            return lhs.record.wait_enter_count > rhs.record.wait_enter_count;
        }
        return lhs.record.sem < rhs.record.sem;
    });
    static std::atomic_int budget{256};
    std::uint32_t emitted = 0;
    std::FILE* watchdog_trace = nullptr;
    if (::access("/data/data/app.lsx4.android/files/lsx4-home/diag-jit-live-watchdog",
                 F_OK) == 0) {
        watchdog_trace = std::fopen(
            "/data/data/app.lsx4.android/files/executor-sem-watchdog.log", "a");
    }
    for (const auto& snapshot : snapshots) {
        if (emitted++ >= 24 || budget.fetch_sub(1, std::memory_order_relaxed) <= 0) {
            break;
        }
        const auto& r = snapshot.record;
        if (watchdog_trace != nullptr) {
            std::fprintf(
                watchdog_trace,
                "kind=posix reason=%s sem=%p native=%p pending=%d init=%u wait=%u return=%u "
                "post=%u before=%d after=%d initThread=%s lastThread=%s lastOp=%s "
                "initSite=%s+0x%llx waitSite=%s+0x%llx postSite=%s+0x%llx\n",
                reason && reason[0] ? reason : "<unknown>", reinterpret_cast<void*>(r.sem),
                reinterpret_cast<void*>(r.native), snapshot.pending, r.init_count,
                r.wait_enter_count, r.wait_return_count, r.post_count, r.last_before,
                r.last_after, r.init_thread.c_str(), r.last_thread.c_str(), r.last_op.c_str(),
                r.init_guest_module.empty() ? "<unknown>" : r.init_guest_module.c_str(),
                static_cast<unsigned long long>(r.init_guest_off),
                r.last_wait_guest_module.empty() ? "<unknown>" : r.last_wait_guest_module.c_str(),
                static_cast<unsigned long long>(r.last_wait_guest_off),
                r.last_post_guest_module.empty() ? "<unknown>" : r.last_post_guest_module.c_str(),
                static_cast<unsigned long long>(r.last_post_guest_off));
        }
        __android_log_print(
            ANDROID_LOG_WARN, "LSX4Native",
            "[EXECUTOR_LIVE_SEM_PENDING_SUMMARY] reason=%s sem=%p native=%p pending=%d "
            "init=%u wait_enter=%u wait_return=%u post=%u value_before=%d value_after=%d "
            "last_ret=%d initThread=%s lastThread=%s lastOp=%s "
            "initSite=%s+0x%llx waitSite=%s+0x%llx postSite=%s+0x%llx",
            reason && reason[0] ? reason : "<unknown>", reinterpret_cast<void*>(r.sem),
            reinterpret_cast<void*>(r.native), snapshot.pending, r.init_count,
            r.wait_enter_count, r.wait_return_count, r.post_count, r.last_before, r.last_after,
            r.last_ret, r.init_thread.c_str(), r.last_thread.c_str(), r.last_op.c_str(),
            r.init_guest_module.empty() ? "<unknown>" : r.init_guest_module.c_str(),
            static_cast<unsigned long long>(r.init_guest_off),
            r.last_wait_guest_module.empty() ? "<unknown>" : r.last_wait_guest_module.c_str(),
            static_cast<unsigned long long>(r.last_wait_guest_off),
            r.last_post_guest_module.empty() ? "<unknown>" : r.last_post_guest_module.c_str(),
            static_cast<unsigned long long>(r.last_post_guest_off));
    }
    if (watchdog_trace != nullptr) {
        std::fclose(watchdog_trace);
    }
}

extern "C" int executor_live_signal_pending_mono_posix_sems(int max_posts) {
    if (max_posts <= 0) {
        return 0;
    }
    struct Candidate {
        std::uintptr_t sem{};
        std::uintptr_t native{};
        std::uint32_t wait_enter{};
        std::uint32_t wait_return{};
        std::uint32_t post{};
        std::string init_module;
        std::string wait_module;
        std::string last_thread;
    };
    std::vector<Candidate> candidates;
    {
        std::lock_guard lock{LivePosixSemRecordsMutex()};
        for (const auto& [_, record] : LivePosixSemRecords()) {
            const auto pending = static_cast<s32>(record.wait_enter_count) -
                                 static_cast<s32>(record.wait_return_count);
            const bool mono_module =
                record.init_guest_module.find("mono-ps4") != std::string::npos ||
                record.last_wait_guest_module.find("mono-ps4") != std::string::npos;
            const bool mono_waiter = record.last_thread.find("mono thread") != std::string::npos ||
                                     record.last_thread.find("Mono") != std::string::npos;
            if (pending <= 0 || record.native == 0 || !mono_module || !mono_waiter) {
                continue;
            }
            candidates.push_back(Candidate{record.sem, record.native, record.wait_enter_count,
                                           record.wait_return_count, record.post_count,
                                           record.init_guest_module, record.last_wait_guest_module,
                                           record.last_thread});
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& lhs,
                                                       const Candidate& rhs) {
        const auto lhs_pending = static_cast<s32>(lhs.wait_enter) -
                                 static_cast<s32>(lhs.wait_return);
        const auto rhs_pending = static_cast<s32>(rhs.wait_enter) -
                                 static_cast<s32>(rhs.wait_return);
        if (lhs_pending != rhs_pending) {
            return lhs_pending > rhs_pending;
        }
        return lhs.sem < rhs.sem;
    });

    int signaled = 0;
    for (const auto& candidate : candidates) {
        if (signaled >= max_posts) {
            break;
        }
        auto* native = reinterpret_cast<PthreadSem*>(candidate.native);
        if (native == nullptr) {
            continue;
        }
        s32 before = 0;
        if (!PostPthreadSemToken(native, &before)) {
            __android_log_print(
                ANDROID_LOG_WARN, "LSX4Native",
                "[EXECUTOR_MONO_POSIX_SEM_SIGNAL] sem=%p native=%p result=overflow before=%d "
                "initSite=%s waitSite=%s lastThread=%s",
                reinterpret_cast<void*>(candidate.sem), native, before,
                candidate.init_module.c_str(), candidate.wait_module.c_str(),
                candidate.last_thread.c_str());
            continue;
        }
        const s32 after = native->value.load(std::memory_order_relaxed);
        {
            std::lock_guard records_lock{LivePosixSemRecordsMutex()};
            auto it = LivePosixSemRecords().find(candidate.sem);
            if (it != LivePosixSemRecords().end()) {
                auto& record = it->second;
                record.post_count++;
                record.last_before = before;
                record.last_after = after;
                record.last_ret = 0;
                record.last_tid = static_cast<long>(LSX4_SEM_HOST_TID());
                record.last_thread = g_curthread ? g_curthread->name : "<no-gcurthread>";
                record.last_op = "signal_mono_posix";
                record.last_post_guest_module = "<executor>";
                record.last_post_guest_off = 0;
            }
        }
        signaled++;
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_MONO_POSIX_SEM_SIGNAL] sem=%p native=%p before=%d after=%d "
            "wait_enter=%u wait_return=%u post_before=%u initSite=%s waitSite=%s "
            "lastThread=%s reason=coop-sigusr1",
            reinterpret_cast<void*>(candidate.sem), native, before, after, candidate.wait_enter,
            candidate.wait_return, candidate.post, candidate.init_module.c_str(),
            candidate.wait_module.c_str(), candidate.last_thread.c_str());
    }
    if (signaled == 0) {
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_MONO_POSIX_SEM_SIGNAL] result=none maxPosts=%d",
                            max_posts);
    }
    return signaled;
}

static std::mutex& LiveKernelSemRecordsMutex() {
    static std::mutex mutex;
    return mutex;
}

static std::unordered_map<u32, LiveKernelSemRecord>& LiveKernelSemRecords() {
    static std::unordered_map<u32, LiveKernelSemRecord> records;
    return records;
}

static void UpdateLivePosixSemRecord(const char* op, PthreadSem** sem, PthreadSem* native,
                                     s32 before, s32 after, s32 ret) {
    if (!ShouldRecordPosixSemFrontier() || sem == nullptr) {
        return;
    }
    const auto key = reinterpret_cast<std::uintptr_t>(sem);
    const auto native_value = reinterpret_cast<std::uintptr_t>(native);
    const long tid = static_cast<long>(LSX4_SEM_HOST_TID());
    const std::string thread_name = g_curthread ? g_curthread->name : "<no-gcurthread>";
    std::lock_guard lock{LivePosixSemRecordsMutex()};
    auto& record = LivePosixSemRecords()[key];
    record.sem = key;
    record.native = native_value;
    record.last_before = before;
    record.last_after = after;
    record.last_ret = ret;
    record.last_tid = tid;
    record.last_thread = thread_name;
    record.last_op = op ? op : "<null>";

    const std::string_view opname{op ? op : ""};
    if (opname == "init") {
        record.init_value = before;
        record.init_count++;
        record.init_tid = tid;
        record.init_thread = thread_name;
    } else if (opname.find("wait_enter") != std::string_view::npos) {
        record.wait_enter_count++;
    } else if (opname.find("wait_return") != std::string_view::npos) {
        record.wait_return_count++;
    } else if (opname.find("post_return") != std::string_view::npos) {
        record.post_count++;
    } else if (opname.find("trywait") != std::string_view::npos) {
        record.trywait_count++;
    } else if (opname.find("timedwait") != std::string_view::npos) {
        record.timedwait_count++;
    } else if (opname.find("invalid") != std::string_view::npos ||
               opname.find("overflow") != std::string_view::npos) {
        record.invalid_count++;
    }
}

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
bool ExecutorLivePosixSemHasWaitEnter(void* sem_slot) {
    if (!ShouldTraceLiveSync() || sem_slot == nullptr) {
        return false;
    }
    const auto key = reinterpret_cast<std::uintptr_t>(sem_slot);
    std::lock_guard lock{LivePosixSemRecordsMutex()};
    const auto it = LivePosixSemRecords().find(key);
    return it != LivePosixSemRecords().end() && it->second.wait_enter_count != 0;
}
#endif

static void DumpLivePosixSemNeighborhood(PthreadSem** stuck_sem) {
    if (!ShouldTraceLiveSync() || stuck_sem == nullptr) {
        return;
    }
    const auto stuck = reinterpret_cast<std::uintptr_t>(stuck_sem);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (executor_jit_dump_thread_states != nullptr) {
        executor_jit_dump_thread_states("posix-sem-stuck");
    }
#endif
    struct Snapshot {
        LivePosixSemRecord record;
        std::uintptr_t distance{};
    };
    std::vector<Snapshot> snapshots;
    snapshots.reserve(64);
    {
        std::lock_guard lock{LivePosixSemRecordsMutex()};
        const auto& records = LivePosixSemRecords();
        const auto stuck_group = stuck & 0xffffffffff000000ULL;
        for (const auto& [key, record] : records) {
            const auto distance = key > stuck ? key - stuck : stuck - key;
            const bool same_group = (key & 0xffffffffff000000ULL) == stuck_group;
            if (key == stuck || same_group || distance <= 0x100000ULL) {
                snapshots.push_back(Snapshot{record, distance});
            }
        }
    }
    std::sort(snapshots.begin(), snapshots.end(), [](const Snapshot& lhs, const Snapshot& rhs) {
        if (lhs.distance != rhs.distance) {
            return lhs.distance < rhs.distance;
        }
        return lhs.record.sem < rhs.record.sem;
    });
    static std::atomic_int budget{512};
    std::uint32_t emitted = 0;
    for (const auto& snapshot : snapshots) {
        if (emitted++ >= 48 || budget.fetch_sub(1, std::memory_order_relaxed) <= 0) {
            break;
        }
        const auto& r = snapshot.record;
        __android_log_print(
            ANDROID_LOG_WARN, "LSX4Native",
            "[EXECUTOR_LIVE_SEM_LEDGER] stuck=%p sem=%p dist=0x%llx native=%p init=%u "
            "wait_enter=%u wait_return=%u post=%u trywait=%u timedwait=%u invalid=%u "
            "init_value=%d last_before=%d last_after=%d last_ret=%d init_thread=%s "
            "init_tid=%ld last_op=%s last_thread=%s last_tid=%ld "
            "initSite=%s+0x%llx waitSite=%s+0x%llx postSite=%s+0x%llx",
            reinterpret_cast<void*>(stuck), reinterpret_cast<void*>(r.sem),
            static_cast<unsigned long long>(snapshot.distance), reinterpret_cast<void*>(r.native),
            r.init_count, r.wait_enter_count, r.wait_return_count, r.post_count,
            r.trywait_count, r.timedwait_count, r.invalid_count, r.init_value, r.last_before,
            r.last_after, r.last_ret, r.init_thread.c_str(), r.init_tid, r.last_op.c_str(),
            r.last_thread.c_str(), r.last_tid,
            r.init_guest_module.empty() ? "<unknown>" : r.init_guest_module.c_str(),
            static_cast<unsigned long long>(r.init_guest_off),
            r.last_wait_guest_module.empty() ? "<unknown>" : r.last_wait_guest_module.c_str(),
            static_cast<unsigned long long>(r.last_wait_guest_off),
            r.last_post_guest_module.empty() ? "<unknown>" : r.last_post_guest_module.c_str(),
            static_cast<unsigned long long>(r.last_post_guest_off));
    }
}

static void UpdateLiveKernelSemRecord(const char* op, Common::SlotId sem, const OrbisSem* native,
                                      s32 before, s32 after, s32 count, s32 ret,
                                      const char* name) {
    if (!ShouldRecordKernelSemFrontier()) {
        return;
    }
    const auto key = sem.index + 1;
    if (key == 0) {
        return;
    }
    const long tid = static_cast<long>(LSX4_SEM_HOST_TID());
    const std::string thread_name = g_curthread ? g_curthread->name : "<no-gcurthread>";
    std::lock_guard lock{LiveKernelSemRecordsMutex()};
    auto& record = LiveKernelSemRecords()[key];
    record.slot = sem.index;
    record.native = reinterpret_cast<std::uintptr_t>(native);
    record.last_before = before;
    record.last_after = after;
    record.last_count = count;
    record.last_ret = ret;
    record.last_tid = tid;
    record.last_thread = thread_name;
    record.last_op = op ? op : "<null>";
    if (name && name[0] != '\0') {
        record.name = name;
    }

    const std::string_view opname{op ? op : ""};
    if (opname == "create") {
        record.init_value = before;
        record.max_value = count;
        record.create_count++;
        record.init_tid = tid;
        record.init_thread = thread_name;
    } else if (opname.find("wait_enter") != std::string_view::npos) {
        record.wait_enter_count++;
    } else if (opname.find("wait_return") != std::string_view::npos) {
        record.wait_return_count++;
    } else if (opname.find("signal") != std::string_view::npos) {
        record.signal_count++;
    } else if (opname.find("poll") != std::string_view::npos) {
        record.poll_count++;
    } else if (opname.find("cancel") != std::string_view::npos) {
        record.cancel_count++;
    } else if (opname.find("missing") != std::string_view::npos ||
               opname.find("invalid") != std::string_view::npos) {
        record.invalid_count++;
    }
}

static void DumpLiveKernelSemLedger(Common::SlotId stuck_sem) {
    if (!ShouldRecordKernelSemFrontier()) {
        return;
    }
    const auto stuck_key = stuck_sem.index + 1;
    struct Snapshot {
        LiveKernelSemRecord record;
        u32 distance{};
    };
    std::vector<Snapshot> snapshots;
    snapshots.reserve(64);
    {
        std::lock_guard lock{LiveKernelSemRecordsMutex()};
        for (const auto& [key, record] : LiveKernelSemRecords()) {
            const auto distance = key > stuck_key ? key - stuck_key : stuck_key - key;
            if (key == stuck_key || distance <= 32) {
                snapshots.push_back(Snapshot{record, distance});
            }
        }
    }
    std::sort(snapshots.begin(), snapshots.end(), [](const Snapshot& lhs, const Snapshot& rhs) {
        if (lhs.distance != rhs.distance) {
            return lhs.distance < rhs.distance;
        }
        return lhs.record.slot < rhs.record.slot;
    });
    static std::atomic_int budget{512};
    std::uint32_t emitted = 0;
    for (const auto& snapshot : snapshots) {
        if (emitted++ >= 48 || budget.fetch_sub(1, std::memory_order_relaxed) <= 0) {
            break;
        }
        const auto& r = snapshot.record;
        __android_log_print(
            ANDROID_LOG_WARN, "LSX4Native",
            "[EXECUTOR_LIVE_KSEMA_LEDGER] stuck_slot=%u slot=%u dist=%u native=%p "
            "create=%u wait_enter=%u wait_return=%u signal=%u poll=%u cancel=%u invalid=%u "
            "init=%d max=%d last_before=%d last_after=%d last_count=%d last_ret=0x%x "
            "init_thread=%s init_tid=%ld last_op=%s last_thread=%s last_tid=%ld name=%s",
            stuck_sem.index, r.slot, snapshot.distance, reinterpret_cast<void*>(r.native),
            r.create_count, r.wait_enter_count, r.wait_return_count, r.signal_count, r.poll_count,
            r.cancel_count, r.invalid_count, r.init_value, r.max_value, r.last_before,
            r.last_after, r.last_count, static_cast<u32>(r.last_ret), r.init_thread.c_str(),
            r.init_tid, r.last_op.c_str(), r.last_thread.c_str(), r.last_tid,
            r.name.empty() ? "<none>" : r.name.c_str());
    }
}

extern "C" void executor_live_dump_kernel_sem_records(const char* reason) {
    if (!ShouldRecordKernelSemFrontier()) {
        return;
    }
    std::vector<LiveKernelSemRecord> snapshot;
    {
        std::lock_guard lock{LiveKernelSemRecordsMutex()};
        snapshot.reserve(LiveKernelSemRecords().size());
        for (const auto& [key, record] : LiveKernelSemRecords()) {
            (void)key;
            snapshot.push_back(record);
        }
    }
    std::sort(snapshot.begin(), snapshot.end(), [](const auto& lhs, const auto& rhs) {
        const auto lhs_outstanding =
            static_cast<std::int64_t>(lhs.wait_enter_count) - lhs.wait_return_count;
        const auto rhs_outstanding =
            static_cast<std::int64_t>(rhs.wait_enter_count) - rhs.wait_return_count;
        if (lhs_outstanding != rhs_outstanding) {
            return lhs_outstanding > rhs_outstanding;
        }
        return lhs.slot < rhs.slot;
    });

    std::size_t active = 0;
    std::FILE* watchdog_trace = nullptr;
    if (::access("/data/data/app.lsx4.android/files/lsx4-home/diag-jit-live-watchdog",
                 F_OK) == 0) {
        watchdog_trace = std::fopen(
            "/data/data/app.lsx4.android/files/executor-sem-watchdog.log", "a");
    }
    for (const auto& record : snapshot) {
        const auto outstanding =
            static_cast<std::int64_t>(record.wait_enter_count) - record.wait_return_count;
        if (outstanding <= 0) {
            continue;
        }
        ++active;
        if (watchdog_trace != nullptr) {
            std::fprintf(
                watchdog_trace,
                "kind=kernel reason=%s slot=%u native=%p name=%s outstanding=%lld create=%u "
                "wait=%u return=%u signal=%u init=%d max=%d before=%d after=%d "
                "initThread=%s lastThread=%s lastOp=%s\n",
                reason && reason[0] ? reason : "<none>", record.slot,
                reinterpret_cast<void*>(record.native),
                record.name.empty() ? "<none>" : record.name.c_str(),
                static_cast<long long>(outstanding), record.create_count,
                record.wait_enter_count, record.wait_return_count, record.signal_count,
                record.init_value, record.max_value, record.last_before, record.last_after,
                record.init_thread.c_str(), record.last_thread.c_str(), record.last_op.c_str());
        }
        __android_log_print(
            ANDROID_LOG_WARN, "LSX4Native",
            "[EXECUTOR_KERNEL_SEM_FRONTIER] reason=%s slot=%u native=%p name=%s "
            "outstanding=%lld create=%u wait_enter=%u wait_return=%u signal=%u poll=%u "
            "cancel=%u invalid=%u init=%d max=%d last_before=%d last_after=%d "
            "last_count=%d last_ret=0x%x init_thread=%s last_op=%s last_thread=%s",
            reason && reason[0] ? reason : "<none>", record.slot,
            reinterpret_cast<void*>(record.native),
            record.name.empty() ? "<none>" : record.name.c_str(),
            static_cast<long long>(outstanding), record.create_count, record.wait_enter_count,
            record.wait_return_count, record.signal_count, record.poll_count,
            record.cancel_count, record.invalid_count, record.init_value, record.max_value,
            record.last_before, record.last_after, record.last_count,
            static_cast<u32>(record.last_ret), record.init_thread.c_str(),
            record.last_op.c_str(), record.last_thread.c_str());
    }
    __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                        "[EXECUTOR_KERNEL_SEM_FRONTIER_SUMMARY] reason=%s records=%zu active=%zu",
                         reason && reason[0] ? reason : "<none>", snapshot.size(), active);
    if (watchdog_trace != nullptr) {
        std::fclose(watchdog_trace);
    }
}

static void TraceLiveUnityGfxSemContext(const char* op, PthreadSem** sem) {
    if (!ShouldTraceLiveSync() || sem == nullptr) {
        return;
    }
    if (ExecutorLightOracleMode() && !ExecutorEnvFlag("EXECUTOR_VERBOSE_UNITY_GFX_SEM_CTX")) {
        return;
    }
    const auto worker = g_executor_unity_gfx_worker.load(std::memory_order_acquire);
    if (worker == 0) {
        return;
    }

    const std::string thread_name = g_curthread ? g_curthread->name : "<no-gcurthread>";
    const bool interesting_thread =
        thread_name.find("UnityGfxDeviceWorker") != std::string::npos ||
        thread_name.find("Game:Main") != std::string::npos ||
        thread_name.find("Submit Done Thread") != std::string::npos;
    if (!interesting_thread) {
        return;
    }

    const auto sem_addr = reinterpret_cast<std::uintptr_t>(sem);
    bool stream_ok = false;
    bool device_ok = false;
    bool stop_ok = false;
    const u64 stream = GuestReadQword(worker + 0x18, &stream_ok);
    const u64 device = GuestReadQword(worker + 0x208, &device_ok);
    const u8 stop = GuestReadByte(worker + 0x211, &stop_ok);

    const auto worker_sem_d0 = worker + 0xd0;
    const auto worker_sem_e4 = worker + 0xe4;
    const auto stream_sem_d0 = stream != 0 ? stream + 0xd0 : 0;
    const auto stream_sem_e4 = stream != 0 ? stream + 0xe4 : 0;
    const bool sem_related =
        sem_addr == worker_sem_d0 || sem_addr == worker_sem_e4 || sem_addr == stream_sem_d0 ||
        sem_addr == stream_sem_e4 ||
        (stream != 0 && sem_addr > stream && sem_addr < stream + 0x180) ||
        (sem_addr > worker && sem_addr < worker + 0x240);
    if (!sem_related && thread_name.find("UnityGfxDeviceWorker") == std::string::npos) {
        return;
    }

    bool stream_buf_ok = false;
    bool stream_pos_ok = false;
    bool stream_cap_ok = false;
    const u64 stream_buf = stream != 0 ? GuestReadQword(stream + 0x8, &stream_buf_ok) : 0;
    const u32 stream_pos = stream != 0 ? GuestReadDword(stream + 0x100, &stream_pos_ok) : 0;
    const u32 stream_cap = stream != 0 ? GuestReadDword(stream + 0x104, &stream_cap_ok) : 0;

    static std::atomic_int budget{4096};
    if (budget.fetch_sub(1, std::memory_order_relaxed) <= 0) {
        return;
    }
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_UNITY_GFX_SEM_CTX] op=%s thread=%s tid=%ld sem=%p worker=%p "
        "stop=%u stopOk=%d stream=%p streamOk=%d streamBuf=%p streamBufOk=%d "
        "streamPos=0x%x streamPosOk=%d streamCap=0x%x streamCapOk=%d device=%p deviceOk=%d "
        "dWorkerD0=%lld dWorkerE4=%lld dStreamD0=%lld dStreamE4=%lld "
        "workerBytes=%s streamBytes=%s",
        op ? op : "<unknown>", thread_name.c_str(), static_cast<long>(LSX4_SEM_HOST_TID()),
        sem, reinterpret_cast<void*>(worker), static_cast<unsigned>(stop), stop_ok ? 1 : 0,
        reinterpret_cast<void*>(stream), stream_ok ? 1 : 0, reinterpret_cast<void*>(stream_buf),
        stream_buf_ok ? 1 : 0, stream_pos, stream_pos_ok ? 1 : 0, stream_cap,
        stream_cap_ok ? 1 : 0, reinterpret_cast<void*>(device), device_ok ? 1 : 0,
        SignedDelta(sem_addr, worker_sem_d0), SignedDelta(sem_addr, worker_sem_e4),
        stream != 0 ? SignedDelta(sem_addr, stream_sem_d0) : 0,
        stream != 0 ? SignedDelta(sem_addr, stream_sem_e4) : 0,
        GuestBytesHex(worker, 0x240).c_str(), stream != 0 ? GuestBytesHex(stream, 0x180).c_str()
                                                          : "<none>");
}

static void TraceLivePosixSem(const char* op, PthreadSem** sem, PthreadSem* native, s32 before,
                              s32 after, s32 ret) {
    const bool record_enabled = ShouldRecordPosixSemFrontier();
    const bool trace_enabled = ShouldTraceLiveSync();
    const bool direct_sem = ShouldTraceLiveDirectPosixSem(sem);
    if (!record_enabled && !trace_enabled && !direct_sem) {
        return;
    }
    if (record_enabled) {
        UpdateLivePosixSemRecord(op, sem, native, before, after, ret);
    }
    if (!trace_enabled && !direct_sem) {
        return;
    }
    if (trace_enabled) {
        TraceLiveUnityGfxSemContext(op, sem);
    }
    const bool interesting = IsCurrentLiveInterestingThread();
    if (ExecutorEnvFlag("EXECUTOR_TRACE_LIVE_RENDER_THREAD_ONLY") && g_curthread &&
        !interesting && !direct_sem && IsLiveKnownNoisyThreadName(g_curthread->name)) {
        return;
    }
    if (interesting) {
        (void)IsWatchedPosixSem(sem, true);
    }
    if (!direct_sem && !interesting && !IsWatchedPosixSem(sem, false)) {
        return;
    }
    static std::atomic_int budget{8192};
    if (budget.fetch_sub(1, std::memory_order_relaxed) <= 0) {
        return;
    }
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_LIVE_SEM] op=%s thread=%s tid=%ld watched=%u direct=%u sem=%p native=%p "
                        "before=%d after=%d ret=%d",
                        op, g_curthread ? g_curthread->name.c_str() : "<no-gcurthread>",
                        static_cast<long>(LSX4_SEM_HOST_TID()), interesting ? 0u : 1u,
                        direct_sem ? 1u : 0u, sem, native, before, after, ret);
}

static void TraceLivePosixSemDuration(const char* op, PthreadSem** sem, PthreadSem* native,
                                      s32 ret, long long elapsed_us) {
    if (!ShouldTraceLiveSync() && !ShouldTraceLiveDirectPosixSem(sem)) {
        return;
    }
    if (elapsed_us >= 1000) {
        TraceLiveUnityGfxSemContext(op, sem);
    }
    const bool direct_sem = ShouldTraceLiveDirectPosixSem(sem);
    if (ExecutorEnvFlag("EXECUTOR_TRACE_LIVE_RENDER_THREAD_ONLY") && g_curthread &&
        !IsCurrentLiveInterestingThread() && !direct_sem &&
        IsLiveKnownNoisyThreadName(g_curthread->name)) {
        return;
    }
    if (!direct_sem && !IsCurrentLiveInterestingThread() && !IsWatchedPosixSem(sem, false)) {
        return;
    }
    static std::atomic_int budget{4096};
    if (budget.fetch_sub(1, std::memory_order_relaxed) <= 0 && elapsed_us < 1000) {
        return;
    }
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_LIVE_SEM_WAIT_TIME] op=%s thread=%s tid=%ld sem=%p native=%p "
                        "elapsed_us=%lld ret=%d",
                        op, g_curthread ? g_curthread->name.c_str() : "<no-gcurthread>",
                        static_cast<long>(LSX4_SEM_HOST_TID()), sem, native, elapsed_us, ret);
}

static void TraceLiveInvalidPosixSemSlot(const char* op, PthreadSem** sem) {
    if (!ShouldTraceLiveSync()) {
        return;
    }
    static std::atomic_int budget{128};
    if (budget.fetch_sub(1, std::memory_order_relaxed) <= 0) {
        return;
    }

    const auto address = reinterpret_cast<u64>(sem);
    bool q0_ok = false;
    bool q1_ok = false;
    const u64 q0 = GuestReadQword(address, &q0_ok);
    const u64 q1 = GuestReadQword(address + sizeof(u64), &q1_ok);
    const u64 window = address >= 0x20 ? address - 0x20 : address;
    const std::string bytes = GuestBytesHex(window, 0x80);
    std::uint64_t hle_ret = 0;
    std::uint64_t hle_off = 0;
    std::uint64_t hle_arg0 = 0;
    char hle_symbol[128]{};
    char hle_module[128]{};
    const int hle_site_rc = executor_live_get_current_hle_call_site
                                ? executor_live_get_current_hle_call_site(
                                      &hle_ret, &hle_off, &hle_arg0, hle_symbol,
                                      sizeof(hle_symbol), hle_module, sizeof(hle_module))
                                : -1;
    __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                        "[EXECUTOR_LIVE_SEM_INVALID_SLOT] op=%s thread=%s tid=%ld sem=%p "
                        "slotQ0=%p slotQ0Ok=%d slotQ1=%p slotQ1Ok=%d "
                        "hleRc=%d hleRet=%p hleOff=0x%llx hleArg0=%p hleModule=%s "
                        "hleSymbol=%s window=%p bytes=%s",
                        op ? op : "<unknown>",
                        g_curthread ? g_curthread->name.c_str() : "<no-gcurthread>",
                        static_cast<long>(LSX4_SEM_HOST_TID()), sem,
                        reinterpret_cast<void*>(q0), q0_ok ? 1 : 0,
                        reinterpret_cast<void*>(q1), q1_ok ? 1 : 0, hle_site_rc,
                        reinterpret_cast<void*>(hle_ret),
                        static_cast<unsigned long long>(hle_off),
                        reinterpret_cast<void*>(hle_arg0),
                        hle_module[0] ? hle_module : "<unknown>",
                        hle_symbol[0] ? hle_symbol : "<unknown>",
                        reinterpret_cast<void*>(window), bytes.c_str());
    if (executor_live_dump_current_hle_context != nullptr &&
        (!ExecutorLightOracleMode() ||
         std::getenv("EXECUTOR_TRACE_LIGHT_INVALID_SEM_HLE_DUMP") != nullptr) &&
        g_curthread && g_curthread->name.find("UnityPreload") != std::string::npos) {
        executor_live_dump_current_hle_context(op ? op : "posix_sem_invalid", address);
    }
}

static std::shared_ptr<std::atomic_bool> BeginLivePosixSemWaitWatch(PthreadSem** sem,
                                                                    PthreadSem* native,
                                                                    s32 before) {
    if (!ShouldTraceLiveSync() || !IsCurrentLiveInterestingThread()) {
        return {};
    }
    const auto done = std::make_shared<std::atomic_bool>(false);
    const std::string thread_name = g_curthread ? g_curthread->name : "<no-gcurthread>";
    const long tid = static_cast<long>(LSX4_SEM_HOST_TID());
    std::thread([done, sem, native, before, thread_name, tid] {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (done->load(std::memory_order_acquire)) {
            return;
        }
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_LIVE_SEM_STUCK] elapsed_ms=2000 thread=%s tid=%ld "
                            "sem=%p native=%p before=%d note=wait_enter_without_return",
                            thread_name.c_str(), tid, sem, native, before);
        if (ExecutorEnvFlag("EXECUTOR_TRACE_LIVE_EMU_DUMP") &&
            executor_lsx4_android_dump_box64_emu_states) {
            executor_lsx4_android_dump_box64_emu_states("posix-sem-stuck");
        }
        DumpLivePosixSemNeighborhood(sem);
    }).detach();
    return done;
}

static void TraceLiveKernelSem(const char* op, Common::SlotId sem, const OrbisSem* native,
                               s32 before, s32 after, s32 count, const u32* timeout, s32 ret,
                               const char* name = nullptr) {
    const bool record_enabled = ShouldRecordKernelSemFrontier();
    const bool trace_enabled = ShouldTraceLiveSync();
    if (!record_enabled && !trace_enabled) {
        return;
    }
    if (record_enabled) {
        UpdateLiveKernelSemRecord(op, sem, native, before, after, count, ret, name);
    }
    if (!trace_enabled) {
        return;
    }
    const bool interesting = IsCurrentLiveInterestingThread();
    if (ExecutorEnvFlag("EXECUTOR_LIGHT_ORACLE") && g_curthread &&
        IsLiveKnownNoisyThreadName(g_curthread->name)) {
        return;
    }
    if (ExecutorEnvFlag("EXECUTOR_TRACE_LIVE_RENDER_THREAD_ONLY") && g_curthread &&
        !interesting && IsLiveKnownNoisyThreadName(g_curthread->name)) {
        return;
    }
    if (interesting) {
        (void)IsWatchedKernelSem(sem, true);
    }
    if (!interesting && !IsWatchedKernelSem(sem, false)) {
        return;
    }
    static std::atomic_int budget{8192};
    if (budget.fetch_sub(1, std::memory_order_relaxed) <= 0) {
        return;
    }
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_LIVE_KSEMA] op=%s thread=%s tid=%ld watched=%u slot=%u "
                        "native=%p before=%d after=%d count=%d timeout=%lld ret=0x%x name=%s",
                        op, g_curthread ? g_curthread->name.c_str() : "<no-gcurthread>",
                        static_cast<long>(LSX4_SEM_HOST_TID()), interesting ? 0u : 1u,
                        sem.index, native, before, after, count,
                        timeout ? static_cast<long long>(*timeout) : -1LL, static_cast<u32>(ret),
                        name ? name : "<none>");
}

static std::shared_ptr<std::atomic_bool> BeginLiveKernelSemWaitWatch(Common::SlotId sem,
                                                                     const OrbisSem* native,
                                                                     s32 before, s32 need_count,
                                                                     const u32* timeout) {
    if (!ShouldTraceLiveSync() || !IsCurrentLiveInterestingThread()) {
        return {};
    }
    const auto done = std::make_shared<std::atomic_bool>(false);
    const std::string thread_name = g_curthread ? g_curthread->name : "<no-gcurthread>";
    const long tid = static_cast<long>(LSX4_SEM_HOST_TID());
    const long long timeout_value = timeout ? static_cast<long long>(*timeout) : -1LL;
    std::thread([done, sem, native, before, need_count, timeout_value, thread_name, tid] {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (done->load(std::memory_order_acquire)) {
            return;
        }
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_LIVE_KSEMA_STUCK] elapsed_ms=2000 thread=%s tid=%ld "
                            "slot=%u native=%p before=%d need=%d timeout=%lld "
                            "note=wait_enter_without_return",
                            thread_name.c_str(), tid, sem.index, native, before, need_count,
                            timeout_value);
        if (ExecutorEnvFlag("EXECUTOR_TRACE_LIVE_EMU_DUMP") &&
            executor_lsx4_android_dump_box64_emu_states) {
            executor_lsx4_android_dump_box64_emu_states("kernel-sem-stuck");
        }
        DumpLiveKernelSemLedger(sem);
    }).detach();
    return done;
}

static void TraceLiveKernelSemDuration(Common::SlotId sem, const OrbisSem* native, s32 ret,
                                       long long elapsed_us, const u32* timeout) {
    if (!ShouldTraceLiveSync()) {
        return;
    }
    if (ExecutorEnvFlag("EXECUTOR_LIGHT_ORACLE") && g_curthread &&
        IsLiveKnownNoisyThreadName(g_curthread->name)) {
        return;
    }
    const bool watched = IsWatchedKernelSem(sem, false);
    if (g_curthread && IsLiveKnownNoisyThreadName(g_curthread->name) && !watched) {
        return;
    }
    if (ExecutorEnvFlag("EXECUTOR_TRACE_LIVE_RENDER_THREAD_ONLY") && g_curthread &&
        !IsCurrentLiveInterestingThread() && IsLiveKnownNoisyThreadName(g_curthread->name)) {
        return;
    }
    if (!IsCurrentLiveInterestingThread() && !watched) {
        return;
    }
    static std::atomic_int budget{4096};
    if (budget.fetch_sub(1, std::memory_order_relaxed) <= 0 && elapsed_us < 1000) {
        return;
    }
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_LIVE_KSEMA_WAIT_TIME] thread=%s tid=%ld slot=%u native=%p "
                        "elapsed_us=%lld timeout_after=%lld ret=0x%x",
                        g_curthread ? g_curthread->name.c_str() : "<no-gcurthread>",
                        static_cast<long>(LSX4_SEM_HOST_TID()), sem.index, native, elapsed_us,
                        timeout ? static_cast<long long>(*timeout) : -1LL, static_cast<u32>(ret));
}
#endif

class OrbisSem {
public:
    OrbisSem(s32 init_count, s32 max_count, std::string_view name, bool is_fifo)
        : name{name}, token_count{init_count}, max_count{max_count}, init_count{init_count},
          is_fifo{is_fifo} {}
    ~OrbisSem() = default;

    s32 Wait(bool can_block, s32 need_count, u32* timeout) {
        std::unique_lock lk{mutex};
        if (token_count >= need_count) {
            token_count -= need_count;
            return ORBIS_OK;
        }
        if (!can_block) {
            return ORBIS_KERNEL_ERROR_EBUSY;
        }

        if (timeout && *timeout == 0) {
            return ORBIS_KERNEL_ERROR_ETIMEDOUT;
        }

        WaitingThread waiter{need_count, is_fifo};
        const auto it = AddWaiter(&waiter);

        const s32 result = waiter.Wait(lk, timeout);
        if (result == ORBIS_KERNEL_ERROR_ETIMEDOUT) {
            wait_list.erase(it);
        }
        return result;
    }

    bool Signal(s32 signal_count) {
        std::scoped_lock lk{mutex};
        if (token_count + signal_count > max_count) {
            for (auto it = wait_list.begin(); it != wait_list.end();) {
                auto* waiter = *it;
                if (waiter->need_count > token_count) {
                    ++it;
                    continue;
                }
                it = wait_list.erase(it);
                token_count -= waiter->need_count;
                waiter->was_signaled = true;
                waiter->Wake();
            }
            return false;
        }
        token_count += signal_count;

        for (auto it = wait_list.begin(); it != wait_list.end();) {
            auto* waiter = *it;
            if (waiter->need_count > token_count) {
                ++it;
                continue;
            }
            it = wait_list.erase(it);
            token_count -= waiter->need_count;
            waiter->was_signaled = true;
            waiter->Wake();
        }

        return true;
    }

    const std::string& Name() const {
        return name;
    }

    s32 TokenCount() const {
        return token_count.load(std::memory_order_relaxed);
    }

    s32 Cancel(s32 set_count, s32* num_waiters) {
        std::scoped_lock lk{mutex};
        if (num_waiters) {
            *num_waiters = static_cast<s32>(wait_list.size());
        }
        for (auto* waiter : wait_list) {
            waiter->was_canceled = true;
            waiter->Wake();
        }
        wait_list.clear();
        token_count = set_count < 0 ? init_count : set_count;
        return ORBIS_OK;
    }

    void Delete() {
        std::scoped_lock lk{mutex};
        for (auto* waiter : wait_list) {
            waiter->was_deleted = true;
            waiter->Wake();
        }
        wait_list.clear();
    }

public:
    struct WaitingThread {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        std::mutex park_mutex;
        std::condition_variable park_cv;
        bool park_posted{};
        bool safe_point_posted{};
        bool handling_safe_point{};
        bool retry_safe_point{};
        Pthread* guest_thread{};
#else
        BinarySemaphore sem;
#endif
        u32 priority;
        s32 need_count;
        std::string thr_name;
        bool was_signaled{};
        bool was_deleted{};
        bool was_canceled{};

        explicit WaitingThread(s32 need_count, bool is_fifo)
            :
#if !defined(__ANDROID__) && !defined(LSX4_APPLE_RUNTIME)
              sem{0},
#endif
              priority{0}, need_count{need_count} {
            const auto* curthread = CurrentOrFallbackPthread();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            guest_thread = const_cast<Pthread*>(curthread);
#endif
            if (!is_fifo && curthread != nullptr) {
                priority = curthread->attr.prio;
            }

            thr_name = curthread != nullptr ? curthread->name : "<no-gcurthread>";
        }

        [[nodiscard]] s32 GetResult() const {
            if (was_signaled) {
                return ORBIS_OK;
            }
            if (was_deleted) {
                return ORBIS_KERNEL_ERROR_EACCES;
            }
            if (was_canceled) {
                return ORBIS_KERNEL_ERROR_ECANCELED;
            }
            return ORBIS_KERNEL_ERROR_ETIMEDOUT;
        }

        void Wake() {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            {
                std::lock_guard park_lock{park_mutex};
                park_posted = true;
            }
            park_cv.notify_one();
#else
            sem.release();
#endif
        }

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        static std::mutex& SafePointRegistryMutex() {
            static std::mutex mutex;
            return mutex;
        }

        static std::vector<WaitingThread*>& SafePointRegistry() {
            static std::vector<WaitingThread*> waiters;
            return waiters;
        }

        void RegisterSafePointWaiter() {
            if (guest_thread == nullptr) {
                return;
            }
            std::scoped_lock registry_lock{SafePointRegistryMutex()};
            SafePointRegistry().push_back(this);
        }

        void UnregisterSafePointWaiter() {
            if (guest_thread == nullptr) {
                return;
            }
            std::scoped_lock registry_lock{SafePointRegistryMutex()};
            auto& waiters = SafePointRegistry();
            const auto it = std::find(waiters.begin(), waiters.end(), this);
            if (it != waiters.end()) {
                waiters.erase(it);
            }
        }

        void NotifySafePoint() {
            {
                std::lock_guard park_lock{park_mutex};
                if (handling_safe_point) {
                    retry_safe_point = true;
                } else {
                    safe_point_posted = true;
                }
            }
            park_cv.notify_one();
        }

        static void NotifyPendingSignal(Pthread* target) {
            if (target == nullptr) {
                return;
            }
            std::scoped_lock registry_lock{SafePointRegistryMutex()};
            for (auto* waiter : SafePointRegistry()) {
                if (waiter != nullptr && waiter->guest_thread == target) {
                    waiter->NotifySafePoint();
                }
            }
        }

        void RunSafePoint(std::unique_lock<std::mutex>& park_lock) {
            safe_point_posted = false;
            handling_safe_point = true;
            park_lock.unlock();
            RunMonoPendingSignalSafePointFromWait();
            park_lock.lock();
            handling_safe_point = false;
        }
#endif

        s32 Wait(std::unique_lock<std::mutex>& lk, u32* timeout) {
            lk.unlock();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            const auto start = std::chrono::steady_clock::now();
            RegisterSafePointWaiter();
            std::unique_lock park_lock{park_mutex};
            RunSafePoint(park_lock);
            if (!timeout) {
                while (!park_posted) {
                    if (safe_point_posted) {
                        RunSafePoint(park_lock);
                        continue;
                    }
                    if (retry_safe_point) {
                        retry_safe_point = false;
                        if (!park_cv.wait_for(
                                park_lock, std::chrono::milliseconds(1),
                                [this] { return park_posted || safe_point_posted; })) {
                            RunSafePoint(park_lock);
                        }
                        continue;
                    }
                    park_cv.wait(park_lock,
                                 [this] { return park_posted || safe_point_posted; });
                }
            } else {
                const auto deadline = start + std::chrono::microseconds(*timeout);
                while (!park_posted) {
                    if (safe_point_posted) {
                        RunSafePoint(park_lock);
                        continue;
                    }
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= deadline) {
                        break;
                    }
                    if (retry_safe_point) {
                        retry_safe_point = false;
                        const auto retry_deadline =
                            std::min(deadline, now + std::chrono::milliseconds(1));
                        if (!park_cv.wait_until(
                                park_lock, retry_deadline,
                                [this] { return park_posted || safe_point_posted; }) &&
                            retry_deadline < deadline) {
                            RunSafePoint(park_lock);
                        }
                        continue;
                    }
                    park_cv.wait_until(
                        park_lock, deadline,
                        [this] { return park_posted || safe_point_posted; });
                }
            }
            if (safe_point_posted) {
                RunSafePoint(park_lock);
            }
            park_lock.unlock();
            UnregisterSafePointWaiter();
            const auto end = std::chrono::steady_clock::now();
            lk.lock();
            if (timeout) {
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
                if (was_signaled) {
                    *timeout = elapsed >= *timeout ? 0 : *timeout - static_cast<u32>(elapsed);
                } else {
                    *timeout = 0;
                }
            }
#else
            if (!timeout) {
                sem.acquire();
                lk.lock();
            } else {
                const auto start = std::chrono::high_resolution_clock::now();
                sem.try_acquire_for(std::chrono::microseconds(*timeout));
                const auto end = std::chrono::high_resolution_clock::now();
                const auto time =
                    std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
                lk.lock();
                if (was_signaled) {
                    *timeout = time >= *timeout ? 0 : *timeout - static_cast<u32>(time);
                } else {
                    *timeout = 0;
                }
            }
#endif
            return GetResult();
        }
    };

    using WaitList = std::list<WaitingThread*>;

    WaitList::iterator AddWaiter(WaitingThread* waiter) {
        if (is_fifo) {
            wait_list.push_back(waiter);
            return --wait_list.end();
        }
        auto it = wait_list.begin();
        while (it != wait_list.end() && (*it)->priority <= waiter->priority) {
            ++it;
        }
        return wait_list.insert(it, waiter);
    }

    WaitList wait_list;
    std::string name;
    std::atomic<s32> token_count;
    std::mutex mutex;
    s32 max_count;
    s32 init_count;
    bool is_fifo;
};

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
struct ExecutorKernelWaitProbe {
    Common::SlotId slot{};
    std::string sem_name;
    s32 before{};
    s32 need{};
};

static std::mutex g_executor_kernel_wait_probe_lock;
static std::unordered_map<Pthread*, ExecutorKernelWaitProbe> g_executor_kernel_wait_probes;

static bool ExecutorKernelWaitProbeEnabled() {
    static const bool enabled = [] {
        std::FILE* file = std::fopen(
            "/data/data/app.lsx4.android/files/lsx4-home/diag-kernel-cond-wake", "r");
        if (file == nullptr) {
            return false;
        }
        std::fclose(file);
        return true;
    }();
    return enabled;
}

static bool IsKernelWaitProbeThread(const Pthread* thread) {
    if (thread == nullptr) {
        return false;
    }
    const auto& name = thread->name;
    return name.find("Game:Main") != std::string::npos ||
           name.find("Loading.") != std::string::npos ||
           name.find("UnityPreload") != std::string::npos ||
           name.find("SceFios") != std::string::npos ||
           name.find("GC Finalizer") != std::string::npos ||
           name.find("Requests Thread") != std::string::npos ||
           name.find("Thread12") != std::string::npos;
}

static void RecordKernelWaitProbe(Pthread* thread, Common::SlotId slot, const OrbisSem* sem,
                                  s32 before, s32 need) {
    if (!ExecutorKernelWaitProbeEnabled() || !IsKernelWaitProbeThread(thread)) {
        return;
    }
    std::scoped_lock lock{g_executor_kernel_wait_probe_lock};
    g_executor_kernel_wait_probes[thread] = {
        slot, sem != nullptr ? sem->Name() : std::string{"<null>"}, before, need};
}

static void EraseKernelWaitProbe(Pthread* thread) {
    if (!ExecutorKernelWaitProbeEnabled() || thread == nullptr) {
        return;
    }
    std::scoped_lock lock{g_executor_kernel_wait_probe_lock};
    g_executor_kernel_wait_probes.erase(thread);
}

extern "C" void executor_live_dump_kernel_wait_probe() {
    if (!ExecutorKernelWaitProbeEnabled()) {
        return;
    }
    std::scoped_lock lock{g_executor_kernel_wait_probe_lock};
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_KERNEL_WAIT_SNAPSHOT] phase=begin count=%zu",
                        g_executor_kernel_wait_probes.size());
    for (const auto& [thread, wait] : g_executor_kernel_wait_probes) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_KERNEL_WAIT_SNAPSHOT] thread=%p name=%s tid=%d slot=%u semName=%s "
            "before=%d need=%d hleDepth=%u hleFlags=0x%x",
            thread, thread ? thread->name.c_str() : "<none>",
            thread ? thread->tid.load(std::memory_order_relaxed) : -1, wait.slot.index,
            wait.sem_name.c_str(), wait.before, wait.need,
            thread ? thread->fex_hle_atomic_depth.load(std::memory_order_acquire) : 0u,
            thread ? thread->fex_hle_atomic_flags.load(std::memory_order_acquire) : 0u);
    }
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_KERNEL_WAIT_SNAPSHOT] phase=end");

    std::scoped_lock posix_lock{g_executor_posix_wait_probe_lock};
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_POSIX_WAIT_SNAPSHOT] phase=begin count=%zu",
                        g_executor_posix_wait_probes.size());
    for (const auto& [thread, wait] : g_executor_posix_wait_probes) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_POSIX_WAIT_SNAPSHOT] thread=%p name=%s tid=%d slot=%p native=%p "
            "before=%d guestReturn=0x%llx returnOff=0x%llx module=%s symbol=%s "
            "hleDepth=%u hleFlags=0x%x",
            thread, thread ? thread->name.c_str() : "<none>",
            thread ? thread->tid.load(std::memory_order_relaxed) : -1, wait.slot, wait.native,
            wait.before, static_cast<unsigned long long>(wait.guest_return),
            static_cast<unsigned long long>(wait.return_off),
            wait.module.empty() ? "<unknown>" : wait.module.c_str(),
            wait.symbol.empty() ? "<unknown>" : wait.symbol.c_str(),
            thread ? thread->fex_hle_atomic_depth.load(std::memory_order_acquire) : 0u,
            thread ? thread->fex_hle_atomic_flags.load(std::memory_order_acquire) : 0u);
    }
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_POSIX_WAIT_SNAPSHOT] phase=end");
}
#endif

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
extern "C" void executor_live_mono_notify_pending_signal_waiter(void* guest_thread) {
    auto* const target = static_cast<Pthread*>(guest_thread);
    // Condvar and barrier waits sleep on the target Pthread's wake semaphore,
    // not either semaphore registry below.  Post one coalesced wake so their
    // existing loop reaches RunPendingMonoSuspendSigusr1AtSafePoint.  PostWake
    // also makes this safe when a normal wake races the signal notification.
    if (target != nullptr) {
        target->PostWake("mono-pending-signal");
    }
    OrbisSem::WaitingThread::NotifyPendingSignal(target);
    NotifyPthreadSemPendingSignalWaiter(target);
    if (executor_live_mono_notify_pending_event_flag_waiter) {
        executor_live_mono_notify_pending_event_flag_waiter(target);
    }
}
#endif

#if !defined(__ANDROID__) && !defined(LSX4_APPLE_RUNTIME)
static bool PcOracleKernelSemaName(const OrbisSem* sem) {
    if (!PcOracleMonoSyncEnabled() || sem == nullptr) {
        return false;
    }
    const char* wide = std::getenv("EXECUTOR_PC_ORACLE_WIDE_SEM");
    if (wide != nullptr && wide[0] != '\0' && std::strcmp(wide, "0") != 0) {
        return true;
    }
    return sem->Name() == "SuspendSemaphore" || sem->Name() == "ResumeSemaphore";
}

static void PcOracleKernelSemaLog(const char* op, Common::SlotId slot, const OrbisSem* sem,
                                  s32 before, s32 after, s32 count, s32 ret) {
    if (!PcOracleKernelSemaName(sem)) {
        return;
    }
    std::fprintf(stderr,
                 "[EXECUTOR_PC_KSEMA] op=%s threadName=%s slot=%u native=%p name=%s before=%d "
                 "after=%d count=%d ret=%d\n",
                 op, PcOracleCurrentThreadName(), slot.index, sem, sem->Name().c_str(), before,
                 after, count, ret);
    std::fflush(stderr);
}
#endif

using OrbisKernelSema = Common::SlotId;

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
static_assert(sizeof(OrbisKernelSema) == sizeof(u32));
static_assert(alignof(OrbisKernelSema) == alignof(u32));
static_assert(std::is_trivially_copyable_v<OrbisKernelSema>);
static_assert(Core::AeroLib::HleAbiDetail::TypeAbi<OrbisKernelSema>::supported);
static_assert(Core::AeroLib::HleAbiDetail::TypeAbi<OrbisKernelSema>::classes[0] ==
              Core::AeroLib::AndroidX64SysVClass::Integer);
#endif

static Common::SlotVector<std::unique_ptr<OrbisSem>> orbis_sems;

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
bool ExecutorSignalKernelSemaByNameForLiveMono(const char* name, s32 signal_count) {
    if (name == nullptr || signal_count <= 0) {
        return false;
    }
    for (auto& sem : orbis_sems) {
        if (!sem || sem->Name() != name) {
            continue;
        }
        const s32 before = sem->TokenCount();
        const bool ok = sem->Signal(signal_count);
        const s32 after = sem->TokenCount();
        __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                            "[EXECUTOR_MONO_COOP_SEMA_SIGNAL] name=%s count=%d before=%d "
                            "after=%d ok=%d",
                            name, signal_count, before, after, ok ? 1 : 0);
        return ok;
    }
    __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                        "[EXECUTOR_MONO_COOP_SEMA_SIGNAL] name=%s count=%d result=missing",
                        name, signal_count);
    return false;
}

bool ExecutorWaitKernelSemaByNameForLiveMono(const char* name, s32 need_count) {
    if (name == nullptr || need_count <= 0) {
        return false;
    }
    for (auto& sem : orbis_sems) {
        if (!sem || sem->Name() != name) {
            continue;
        }
        const s32 before = sem->TokenCount();
        const s32 ret = sem->Wait(true, need_count, nullptr);
        const s32 after = sem->TokenCount();
        __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                            "[EXECUTOR_MONO_COOP_SEMA_WAIT] name=%s count=%d before=%d "
                            "after=%d ret=0x%x",
                            name, need_count, before, after, static_cast<u32>(ret));
        return ret == ORBIS_OK;
    }
    __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                        "[EXECUTOR_MONO_COOP_SEMA_WAIT] name=%s count=%d result=missing",
                        name, need_count);
    return false;
}

static std::atomic_int g_executor_live_mono_synthetic_resume_acks{0};

extern "C" int executor_live_mono_synthetic_suspend_ack(void* guest_thread, const char* reason) {
    const bool suspend_acked = ExecutorSignalKernelSemaByNameForLiveMono("SuspendSemaphore", 1);
    if (!suspend_acked) {
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_MONO_SYNTH_SUSPEND] result=missing_suspend thread=%p "
                            "reason=%s",
                            guest_thread, reason ? reason : "");
        return -2;
    }
    const int before =
        g_executor_live_mono_synthetic_resume_acks.fetch_add(1, std::memory_order_acq_rel);
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_MONO_SYNTH_SUSPEND] result=ok thread=%p pendingBefore=%d "
                        "pendingAfter=%d reason=%s",
                        guest_thread, before, before + 1, reason ? reason : "");
    return 0;
}

extern "C" int executor_live_mono_synthetic_resume_ack(const char* reason) {
    const int count = g_executor_live_mono_synthetic_resume_acks.exchange(
        0, std::memory_order_acq_rel);
    if (count <= 0) {
        return 0;
    }
    const bool resume_acked = ExecutorSignalKernelSemaByNameForLiveMono("ResumeSemaphore", count);
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_MONO_SYNTH_RESUME] count=%d resumeAck=%d reason=%s", count,
                        resume_acked ? 1 : 0, reason ? reason : "");
    return resume_acked ? count : -count;
}
#endif

s32 PS4_SYSV_ABI sceKernelCreateSema(OrbisKernelSema* sem, const char* pName, u32 attr,
                                     s32 initCount, s32 maxCount, const void* pOptParam) {
    if (!pName || attr > 2 || initCount < 0 || maxCount <= 0 || initCount > maxCount) {
        LOG_ERROR(Lib_Kernel, "Semaphore creation parameters are invalid!");
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        TraceLiveKernelSem("create_invalid", Common::SlotId{}, nullptr, initCount, -1, maxCount,
                           nullptr, ORBIS_KERNEL_ERROR_EINVAL, pName);
#endif
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    *sem = orbis_sems.insert(
        std::move(std::make_unique<OrbisSem>(initCount, maxCount, pName, attr == 1)));
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    TraceLiveKernelSem("create", *sem, orbis_sems[*sem].get(), initCount, initCount, maxCount,
                       nullptr, ORBIS_OK, pName);
#else
    PcOracleKernelSemaLog("create", *sem, orbis_sems[*sem].get(), initCount, initCount, maxCount,
                          ORBIS_OK);
#endif
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelWaitSema(OrbisKernelSema sem, s32 needCount, u32* pTimeout) {
    if (!orbis_sems.is_allocated(sem)) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        TraceLiveKernelSem("wait_missing", sem, nullptr, -1, -1, needCount, pTimeout,
                           ORBIS_KERNEL_ERROR_ESRCH);
#endif
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    auto* native = orbis_sems[sem].get();
    const s32 before = native->token_count.load(std::memory_order_relaxed);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool executor_mono_sync_sem =
        native->Name() == "SuspendSemaphore" || native->Name() == "ResumeSemaphore";
    static std::atomic_int executor_mono_wait_log_budget{96};
    if (executor_mono_sync_sem &&
        executor_mono_wait_log_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_MONO_KSEMA_AUDIT] op=wait_enter thread=%s slot=%u name=%s "
            "before=%d need=%d timeout=%lld",
            g_curthread ? g_curthread->name.c_str() : "<no-gcurthread>", sem.index,
            native->Name().c_str(), before, needCount,
            pTimeout ? static_cast<long long>(*pTimeout) : -1LL);
    }
    const char* mono_suspend = std::getenv("MONO_THREADS_SUSPEND");
    const bool mono_coop = mono_suspend && std::strcmp(mono_suspend, "coop") == 0;
    const bool mono_kernel_suspend_sem =
        native->name == "SuspendSemaphore" || native->name == "ResumeSemaphore";
    if (mono_coop && ExecutorEnvFlag("EXECUTOR_LIVE_MONO_SEM_WAIT_BYPASS") &&
        mono_kernel_suspend_sem) {
        static std::atomic_int log_budget{64};
        if (log_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
            __android_log_print(
                ANDROID_LOG_WARN, "LSX4Native",
                "[EXECUTOR_LIVE_MONO_KSEMA_BYPASS] op=wait slot=%u native=%p before=%d "
                "need=%d thread=%s tid=%ld name=%s reason=coop-suspend",
                sem.index, native, before, needCount,
                g_curthread ? g_curthread->name.c_str() : "<no-gcurthread>",
                static_cast<long>(LSX4_SEM_HOST_TID()), native->name.c_str());
        }
        TraceLiveKernelSem("wait_bypass_mono_suspend", sem, native, before, before, needCount,
                           pTimeout, ORBIS_OK);
        return ORBIS_OK;
    }
    TraceLiveKernelSem("wait_enter", sem, native, before, before, needCount, pTimeout, ORBIS_OK);
    auto executor_wait_watch = BeginLiveKernelSemWaitWatch(sem, native, before, needCount, pTimeout);
    const bool executor_trace_wait_duration = ShouldTraceLiveSync();
    std::chrono::steady_clock::time_point executor_wait_start{};
    if (executor_trace_wait_duration) {
        executor_wait_start = std::chrono::steady_clock::now();
    }
#else
    PcOracleKernelSemaLog("wait_enter", sem, native, before, before, needCount, ORBIS_OK);
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    RecordKernelWaitProbe(g_curthread, sem, native, before, needCount);
#endif
    const s32 ret = native->Wait(true, needCount, pTimeout);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    EraseKernelWaitProbe(g_curthread);
#endif
    const s32 after = native->token_count.load(std::memory_order_relaxed);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (executor_mono_sync_sem &&
        executor_mono_wait_log_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_MONO_KSEMA_AUDIT] op=wait_return thread=%s slot=%u name=%s "
            "before=%d after=%d need=%d ret=0x%x",
            g_curthread ? g_curthread->name.c_str() : "<no-gcurthread>", sem.index,
            native->Name().c_str(), before, after, needCount, static_cast<u32>(ret));
    }
    if (executor_wait_watch) {
        executor_wait_watch->store(true, std::memory_order_release);
    }
    TraceLiveKernelSem("wait_return", sem, native, before, after, needCount, pTimeout, ret);
    if (executor_trace_wait_duration) {
        const auto executor_wait_end = std::chrono::steady_clock::now();
        TraceLiveKernelSemDuration(
            sem, native, ret,
            std::chrono::duration_cast<std::chrono::microseconds>(executor_wait_end -
                                                                  executor_wait_start)
                .count(),
            pTimeout);
    }
#else
    PcOracleKernelSemaLog("wait_return", sem, native, before, after, needCount, ret);
#endif
    return ret;
}

s32 PS4_SYSV_ABI sceKernelSignalSema(OrbisKernelSema sem, s32 signalCount) {
    if (!orbis_sems.is_allocated(sem)) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        TraceLiveKernelSem("signal_missing", sem, nullptr, -1, -1, signalCount, nullptr,
                           ORBIS_KERNEL_ERROR_ESRCH);
#endif
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    auto* native = orbis_sems[sem].get();
    const s32 before = native->token_count.load(std::memory_order_relaxed);
    if (!native->Signal(signalCount)) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        TraceLiveKernelSem("signal_invalid", sem, native, before,
                           native->token_count.load(std::memory_order_relaxed), signalCount,
                           nullptr, ORBIS_KERNEL_ERROR_EINVAL);
#endif
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool executor_mono_sync_sem =
        native->Name() == "SuspendSemaphore" || native->Name() == "ResumeSemaphore";
    static std::atomic_int executor_mono_signal_log_budget{128};
    if (executor_mono_sync_sem &&
        executor_mono_signal_log_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_MONO_KSEMA_AUDIT] op=signal thread=%s slot=%u name=%s "
            "before=%d after=%d count=%d ret=0x%x",
            g_curthread ? g_curthread->name.c_str() : "<no-gcurthread>", sem.index,
            native->Name().c_str(), before,
            native->token_count.load(std::memory_order_relaxed), signalCount, ORBIS_OK);
    }
    TraceLiveKernelSem("signal", sem, native, before,
                       native->token_count.load(std::memory_order_relaxed), signalCount, nullptr,
                       ORBIS_OK);
    if (ExecutorEnvFlag("EXECUTOR_LIGHT_ORACLE") &&
        (native->Name() == "SuspendSemaphore" || native->Name() == "ResumeSemaphore")) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_MONO_KSEMA_DIRECT] op=signal thread=%s slot=%u name=%s "
            "before=%d after=%d count=%d ret=0x%x",
            g_curthread ? g_curthread->name.c_str() : "<no-gcurthread>", sem.index,
            native->Name().c_str(), before,
            native->token_count.load(std::memory_order_relaxed), signalCount, ORBIS_OK);
    }
#else
    PcOracleKernelSemaLog("signal", sem, native, before,
                          native->token_count.load(std::memory_order_relaxed), signalCount,
                          ORBIS_OK);
#endif
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelPollSema(OrbisKernelSema sem, s32 needCount) {
    if (!orbis_sems.is_allocated(sem)) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        TraceLiveKernelSem("poll_missing", sem, nullptr, -1, -1, needCount, nullptr,
                           ORBIS_KERNEL_ERROR_ESRCH);
#endif
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    auto* native = orbis_sems[sem].get();
    const s32 before = native->token_count.load(std::memory_order_relaxed);
    const s32 ret = native->Wait(false, needCount, nullptr);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    TraceLiveKernelSem("poll", sem, native, before,
                       native->token_count.load(std::memory_order_relaxed), needCount, nullptr,
                       ret);
#endif
    return ret;
}

s32 PS4_SYSV_ABI sceKernelCancelSema(OrbisKernelSema sem, s32 setCount, s32* pNumWaitThreads) {
    if (!orbis_sems.is_allocated(sem)) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        TraceLiveKernelSem("cancel_missing", sem, nullptr, -1, -1, setCount, nullptr,
                           ORBIS_KERNEL_ERROR_ESRCH);
#endif
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    auto* native = orbis_sems[sem].get();
    const s32 before = native->token_count.load(std::memory_order_relaxed);
    const s32 ret = native->Cancel(setCount, pNumWaitThreads);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    TraceLiveKernelSem("cancel", sem, native, before,
                       native->token_count.load(std::memory_order_relaxed), setCount, nullptr,
                       ret);
#endif
    return ret;
}

s32 PS4_SYSV_ABI sceKernelDeleteSema(OrbisKernelSema sem) {
    if (!orbis_sems.is_allocated(sem)) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        TraceLiveKernelSem("delete_missing", sem, nullptr, -1, -1, 0, nullptr,
                           ORBIS_KERNEL_ERROR_ESRCH);
#endif
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    TraceLiveKernelSem("delete", sem, orbis_sems[sem].get(),
                       orbis_sems[sem]->token_count.load(std::memory_order_relaxed), -1, 0,
                       nullptr, ORBIS_OK);
#endif
    orbis_sems[sem]->Delete();
    orbis_sems.erase(sem);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI posix_sem_init(PthreadSem** sem, s32 pshared, u32 value) {
    if (sem == nullptr || value > ORBIS_KERNEL_SEM_VALUE_MAX) {
        *__Error() = POSIX_EINVAL;
        return -1;
    }
    PthreadSem* native;
    {
        std::scoped_lock lock{pthread_sem_pool_mutex};
        native = pthread_sem_pool.emplace_back(
            std::make_unique<PthreadSem>(static_cast<s32>(value))).get();
    }
    StorePthreadSem(sem, native);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    TraceLivePosixSem("init", sem, native, static_cast<s32>(value),
                      native->value.load(std::memory_order_relaxed), 0);
#else
    PcOraclePosixSemLog("init", sem, native, static_cast<s32>(value),
                        native->value.load(std::memory_order_relaxed), 0,
                        __builtin_return_address(0));
#endif
    return 0;
}

s32 PS4_SYSV_ABI posix_sem_destroy(PthreadSem** sem) {
    if (DetachPthreadSem(sem) == nullptr) {
        *__Error() = POSIX_EINVAL;
        return -1;
    }
    return 0;
}

s32 PS4_SYSV_ABI posix_sem_wait(PthreadSem** sem) {
    PthreadSem* const native = LoadPthreadSem(sem);
    if (native == nullptr) {
        *__Error() = POSIX_EINVAL;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        TraceLivePosixSem("wait_invalid", sem, nullptr, -1, -1, -1);
        TraceLiveInvalidPosixSemSlot("wait_invalid", sem);
#else
        PcOraclePosixSemLog("wait_invalid", sem, nullptr, -1, -1, -1,
                            __builtin_return_address(0));
        PcOraclePosixSemInvalidLog("wait_invalid", sem, __builtin_return_address(0));
#endif
        return -1;
    }
    const s32 before = native->value.load(std::memory_order_relaxed);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const u32 executor_eop_thread_kind = ExecutorEopTraceThreadKind();
    if (executor_eop_thread_kind != 0) {
        AmdGpu::ExecutorEopTraceSemSync(0, reinterpret_cast<uintptr_t>(sem),
                                       reinterpret_cast<uintptr_t>(native), before, before, 0,
                                       executor_eop_thread_kind);
    }
    if (ShouldBypassLiveMonoSemWait(sem)) {
        static std::atomic_int log_budget{128};
        if (log_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
            __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                                "[EXECUTOR_LIVE_MONO_SEM_BYPASS] op=wait sem=%p native=%p "
                                "before=%d thread=%s tid=%ld ret=0 note=nonblocking_live_probe",
                                sem, native, before,
                                g_curthread ? g_curthread->name.c_str() : "<no-gcurthread>",
                                static_cast<long>(LSX4_SEM_HOST_TID()));
        }
        TraceLivePosixSem("wait_bypass", sem, native, before, before, 0);
        return 0;
    }
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    TraceLivePosixSem("wait_enter", sem, native, before, before, 0);
    auto executor_wait_watch = BeginLivePosixSemWaitWatch(sem, native, before);
    const bool executor_trace_wait_duration =
        ShouldTraceLiveSync() || ShouldTraceLiveDirectPosixSem(sem);
    std::chrono::steady_clock::time_point executor_wait_start{};
    if (executor_trace_wait_duration) {
        executor_wait_start = std::chrono::steady_clock::now();
    }
#else
    const void* pc_oracle_return = __builtin_return_address(0);
    PcOraclePosixSemLog("wait_enter", sem, native, before, before, 0, pc_oracle_return);
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    RecordPosixWaitProbe(g_curthread, sem, native, before);
    {
        std::unique_lock wait_lock{native->wait_mutex};
        ++native->waiters;
        Pthread* const signal_waiter =
            !UseJitNativeSemaphorePark() ? g_curthread : nullptr;
        if (signal_waiter != nullptr) {
            native->signal_waiters.push_back(signal_waiter);
        }
        while (native->value.load(std::memory_order_acquire) == 0) {
            if (UseJitNativeSemaphorePark()) {
                native->wait_cv.wait(wait_lock);
                continue;
            }
            if (signal_waiter == nullptr) {
                native->wait_cv.wait_for(wait_lock, std::chrono::milliseconds(1));
                if (native->value.load(std::memory_order_acquire) != 0) {
                    break;
                }
                wait_lock.unlock();
                RunMonoPendingSignalSafePointFromWait();
                std::this_thread::yield();
                wait_lock.lock();
                continue;
            }
            const u64 signal_generation = native->signal_generation;
            wait_lock.unlock();
            RunMonoPendingSignalSafePointFromWait();
            wait_lock.lock();
            if (native->value.load(std::memory_order_acquire) != 0) {
                break;
            }
            if (native->signal_generation != signal_generation) {
                // The safe-point requeued its signal while this waiter was not
                // parked. Keep the old bounded retry only for that rare lost-
                // notification window; ordinary FEX waits remain event-driven.
                native->wait_cv.wait_for(wait_lock, std::chrono::milliseconds(1));
            } else {
                native->wait_cv.wait(wait_lock);
            }
        }
        if (signal_waiter != nullptr) {
            const auto it = std::find(native->signal_waiters.begin(),
                                      native->signal_waiters.end(), signal_waiter);
            if (it != native->signal_waiters.end()) {
                native->signal_waiters.erase(it);
            }
        }
        --native->waiters;
        native->value.fetch_sub(1, std::memory_order_acq_rel);
    }
    ErasePosixWaitProbe(g_curthread);
#else
    native->semaphore.acquire();
    native->value.fetch_sub(1, std::memory_order_acq_rel);
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (executor_eop_thread_kind != 0) {
        AmdGpu::ExecutorEopTraceSemSync(
            1, reinterpret_cast<uintptr_t>(sem), reinterpret_cast<uintptr_t>(native), before,
            native->value.load(std::memory_order_relaxed), 0, executor_eop_thread_kind);
    }
    if (executor_wait_watch) {
        executor_wait_watch->store(true, std::memory_order_release);
    }
    TraceLivePosixSem("wait_return", sem, native, before,
                      native->value.load(std::memory_order_relaxed), 0);
    if (executor_trace_wait_duration) {
        const auto executor_wait_end = std::chrono::steady_clock::now();
        TraceLivePosixSemDuration(
            "wait", sem, native, 0,
            std::chrono::duration_cast<std::chrono::microseconds>(executor_wait_end -
                                                                  executor_wait_start)
                .count());
    }
#else
    PcOraclePosixSemLog("wait_return", sem, native, before,
                        native->value.load(std::memory_order_relaxed), 0, pc_oracle_return);
#endif
    return 0;
}

s32 PS4_SYSV_ABI posix_sem_trywait(PthreadSem** sem) {
    PthreadSem* const native = LoadPthreadSem(sem);
    if (native == nullptr) {
        *__Error() = POSIX_EINVAL;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        TraceLivePosixSem("trywait_invalid", sem, nullptr, -1, -1, -1);
        TraceLiveInvalidPosixSemSlot("trywait_invalid", sem);
#else
        PcOraclePosixSemLog("trywait_invalid", sem, nullptr, -1, -1, -1,
                            __builtin_return_address(0));
        PcOraclePosixSemInvalidLog("trywait_invalid", sem, __builtin_return_address(0));
#endif
        return -1;
    }
    const s32 before = native->value.load(std::memory_order_relaxed);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    {
        std::lock_guard wait_lock{native->wait_mutex};
        if (native->value.load(std::memory_order_acquire) == 0) {
            *__Error() = POSIX_EAGAIN;
            TraceLivePosixSem("trywait_busy", sem, native, before,
                              native->value.load(std::memory_order_relaxed), -1);
            return -1;
        }
        native->value.fetch_sub(1, std::memory_order_acq_rel);
    }
#else
    if (!native->semaphore.try_acquire()) {
        *__Error() = POSIX_EAGAIN;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        TraceLivePosixSem("trywait_busy", sem, native, before,
                          native->value.load(std::memory_order_relaxed), -1);
#endif
        return -1;
    }
    native->value.fetch_sub(1, std::memory_order_acq_rel);
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    TraceLivePosixSem("trywait_return", sem, native, before,
                      native->value.load(std::memory_order_relaxed), 0);
#else
    PcOraclePosixSemLog("trywait_return", sem, native, before,
                        native->value.load(std::memory_order_relaxed), 0,
                        __builtin_return_address(0));
#endif
    return 0;
}

s32 PS4_SYSV_ABI posix_sem_timedwait(PthreadSem** sem, const OrbisKernelTimespec* t) {
    PthreadSem* const native = LoadPthreadSem(sem);
    if (native == nullptr) {
        *__Error() = POSIX_EINVAL;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        TraceLivePosixSem("timedwait_invalid", sem, nullptr, -1, -1, -1);
        TraceLiveInvalidPosixSemSlot("timedwait_invalid", sem);
#else
        PcOraclePosixSemLog("timedwait_invalid", sem, nullptr, -1, -1, -1,
                            __builtin_return_address(0));
        PcOraclePosixSemInvalidLog("timedwait_invalid", sem, __builtin_return_address(0));
#endif
        return -1;
    }
    const s32 before = native->value.load(std::memory_order_relaxed);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    TraceLivePosixSem("timedwait_enter", sem, native, before, before, 0);
    auto executor_wait_watch = BeginLivePosixSemWaitWatch(sem, native, before);
    const bool executor_trace_wait_duration =
        ShouldTraceLiveSync() || ShouldTraceLiveDirectPosixSem(sem);
    std::chrono::steady_clock::time_point executor_wait_start{};
    if (executor_trace_wait_duration) {
        executor_wait_start = std::chrono::steady_clock::now();
    }
#else
    const void* pc_oracle_return = __builtin_return_address(0);
    PcOraclePosixSemLog("timedwait_enter", sem, native, before, before, 0, pc_oracle_return);
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    RecordPosixWaitProbe(g_curthread, sem, native, before);
#endif
    {
        bool acquired = false;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        {
            const auto deadline = t->TimePoint();
            std::unique_lock wait_lock{native->wait_mutex};
            ++native->waiters;
            Pthread* const signal_waiter =
                !UseJitNativeSemaphorePark() ? g_curthread : nullptr;
            if (signal_waiter != nullptr) {
                native->signal_waiters.push_back(signal_waiter);
            }
            while (native->value.load(std::memory_order_acquire) == 0) {
                if (UseJitNativeSemaphorePark()) {
                    if (native->wait_cv.wait_until(wait_lock, deadline) ==
                            std::cv_status::timeout &&
                        native->value.load(std::memory_order_acquire) == 0) {
                        break;
                    }
                    continue;
                }
                const auto now = std::chrono::system_clock::now();
                if (now >= deadline) {
                    break;
                }
                if (signal_waiter == nullptr) {
                    const auto slice_deadline =
                        std::min(deadline, now + std::chrono::milliseconds(1));
                    native->wait_cv.wait_until(wait_lock, slice_deadline);
                    if (native->value.load(std::memory_order_acquire) != 0) {
                        break;
                    }
                    wait_lock.unlock();
                    RunMonoPendingSignalSafePointFromWait();
                    std::this_thread::yield();
                    wait_lock.lock();
                    continue;
                }
                const u64 signal_generation = native->signal_generation;
                wait_lock.unlock();
                RunMonoPendingSignalSafePointFromWait();
                wait_lock.lock();
                if (native->value.load(std::memory_order_acquire) != 0) {
                    break;
                }
                if (native->signal_generation != signal_generation) {
                    const auto retry_deadline = std::min(
                        deadline, std::chrono::system_clock::now() +
                                      std::chrono::milliseconds(1));
                    native->wait_cv.wait_until(wait_lock, retry_deadline);
                } else {
                    native->wait_cv.wait_until(wait_lock, deadline);
                }
            }
            acquired = native->value.load(std::memory_order_acquire) != 0;
            if (signal_waiter != nullptr) {
                const auto it = std::find(native->signal_waiters.begin(),
                                          native->signal_waiters.end(), signal_waiter);
                if (it != native->signal_waiters.end()) {
                    native->signal_waiters.erase(it);
                }
            }
            --native->waiters;
            if (acquired) {
                native->value.fetch_sub(1, std::memory_order_acq_rel);
            }
        }
#else
        acquired = native->semaphore.try_acquire_until(t->TimePoint());
#endif
        if (!acquired) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            ErasePosixWaitProbe(g_curthread);
#endif
            *__Error() = POSIX_ETIMEDOUT;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            if (executor_wait_watch) {
                executor_wait_watch->store(true, std::memory_order_release);
            }
            TraceLivePosixSem("timedwait_timeout", sem, native, before,
                              native->value.load(std::memory_order_relaxed), -1);
            if (executor_trace_wait_duration) {
                const auto executor_wait_end = std::chrono::steady_clock::now();
                TraceLivePosixSemDuration(
                    "timedwait_timeout", sem, native, -1,
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        executor_wait_end - executor_wait_start)
                        .count());
            }
#endif
            return -1;
        }
#if !defined(__ANDROID__) && !defined(LSX4_APPLE_RUNTIME)
        native->value.fetch_sub(1, std::memory_order_acq_rel);
#endif
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    ErasePosixWaitProbe(g_curthread);
    if (executor_wait_watch) {
        executor_wait_watch->store(true, std::memory_order_release);
    }
    TraceLivePosixSem("timedwait_return", sem, native, before,
                      native->value.load(std::memory_order_relaxed), 0);
    if (executor_trace_wait_duration) {
        const auto executor_wait_end = std::chrono::steady_clock::now();
        TraceLivePosixSemDuration(
            "timedwait", sem, native, 0,
            std::chrono::duration_cast<std::chrono::microseconds>(executor_wait_end -
                                                                  executor_wait_start)
                .count());
    }
#else
    PcOraclePosixSemLog("timedwait_return", sem, native, before,
                        native->value.load(std::memory_order_relaxed), 0, pc_oracle_return);
#endif
    return 0;
}

s32 PS4_SYSV_ABI posix_sem_post(PthreadSem** sem) {
    PthreadSem* const native = LoadPthreadSem(sem);
    if (native == nullptr) {
        *__Error() = POSIX_EINVAL;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        TraceLivePosixSem("post_invalid", sem, nullptr, -1, -1, -1);
        TraceLiveInvalidPosixSemSlot("post_invalid", sem);
#else
        PcOraclePosixSemLog("post_invalid", sem, nullptr, -1, -1, -1,
                            __builtin_return_address(0));
        PcOraclePosixSemInvalidLog("post_invalid", sem, __builtin_return_address(0));
#endif
        return -1;
    }
    const s32 before = native->value.load(std::memory_order_relaxed);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const u32 executor_eop_thread_kind = ExecutorEopTraceThreadKind();
    if (!PostPthreadSemToken(native)) {
        *__Error() = POSIX_EOVERFLOW;
        TraceLivePosixSem("post_overflow", sem, native, before, before, -1);
        return -1;
    }
    if (executor_eop_thread_kind != 0) {
        AmdGpu::ExecutorEopTraceSemSync(
            2, reinterpret_cast<uintptr_t>(sem), reinterpret_cast<uintptr_t>(native), before,
            native->value.load(std::memory_order_relaxed), 0, executor_eop_thread_kind);
    }
#else
    if (native->value == ORBIS_KERNEL_SEM_VALUE_MAX) {
        *__Error() = POSIX_EOVERFLOW;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        TraceLivePosixSem("post_overflow", sem, native, before, before, -1);
#endif
        return -1;
    }
    ++native->value;
    native->semaphore.release();
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    TraceLivePosixSem("post_return", sem, native, before,
                      native->value.load(std::memory_order_relaxed), 0);
#else
    PcOraclePosixSemLog("post_return", sem, native, before,
                        native->value.load(std::memory_order_relaxed), 0,
                        __builtin_return_address(0));
#endif
    return 0;
}

s32 PS4_SYSV_ABI posix_sem_getvalue(PthreadSem** sem, s32* sval) {
    PthreadSem* const native = LoadPthreadSem(sem);
    if (native == nullptr) {
        *__Error() = POSIX_EINVAL;
        return -1;
    }
    if (sval) {
        *sval = native->value;
    }
    return 0;
}

s32 PS4_SYSV_ABI scePthreadSemInit(PthreadSem** sem, s32 flag, u32 value, const char* name) {
    if (flag != 0) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    s32 ret = posix_sem_init(sem, 0, value);
    if (ret != 0) {
        return ErrnoToSceKernelError(*__Error());
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePthreadSemDestroy(PthreadSem** sem) {
    s32 ret = posix_sem_destroy(sem);
    if (ret != 0) {
        return ErrnoToSceKernelError(*__Error());
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePthreadSemWait(PthreadSem** sem) {
    s32 ret = posix_sem_wait(sem);
    if (ret != 0) {
        return ErrnoToSceKernelError(*__Error());
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePthreadSemTrywait(PthreadSem** sem) {
    s32 ret = posix_sem_trywait(sem);
    if (ret != 0) {
        return ErrnoToSceKernelError(*__Error());
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePthreadSemTimedwait(PthreadSem** sem, u32 usec) {
    OrbisKernelTimespec now{};
    sceKernelClockGettime(ORBIS_CLOCK_REALTIME, &now);
    const u64 total_nsec = static_cast<u64>(now.tv_nsec) +
                           static_cast<u64>(usec % 1000000) * 1000ULL;

    OrbisKernelTimespec time{};
    time.tv_sec = now.tv_sec + usec / 1000000 + total_nsec / 1000000000ULL;
    time.tv_nsec = total_nsec % 1000000000ULL;

    s32 ret = posix_sem_timedwait(sem, &time);
    if (ret != 0) {
        return ErrnoToSceKernelError(*__Error());
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePthreadSemPost(PthreadSem** sem) {
    s32 ret = posix_sem_post(sem);
    if (ret != 0) {
        return ErrnoToSceKernelError(*__Error());
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePthreadSemGetvalue(PthreadSem** sem, s32* sval) {
    s32 ret = posix_sem_getvalue(sem, sval);
    if (ret != 0) {
        return ErrnoToSceKernelError(*__Error());
    }

    return ORBIS_OK;
}

void RegisterSemaphore(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("188x57JYp0g", "libkernel", 1, "libkernel", sceKernelCreateSema);
    LIB_FUNCTION("Zxa0VhQVTsk", "libkernel", 1, "libkernel", sceKernelWaitSema);
    LIB_FUNCTION("4czppHBiriw", "libkernel", 1, "libkernel", sceKernelSignalSema);
    LIB_FUNCTION("12wOHk8ywb0", "libkernel", 1, "libkernel", sceKernelPollSema);
    LIB_FUNCTION("4DM06U2BNEY", "libkernel", 1, "libkernel", sceKernelCancelSema);
    LIB_FUNCTION("R1Jvn8bSCW8", "libkernel", 1, "libkernel", sceKernelDeleteSema);

    LIB_FUNCTION("pDuPEf3m4fI", "libScePosix", 1, "libkernel", posix_sem_init);
    LIB_FUNCTION("cDW233RAwWo", "libScePosix", 1, "libkernel", posix_sem_destroy);
    LIB_FUNCTION("YCV5dGGBcCo", "libScePosix", 1, "libkernel", posix_sem_wait);
    LIB_FUNCTION("WBWzsRifCEA", "libScePosix", 1, "libkernel", posix_sem_trywait);
    LIB_FUNCTION("w5IHyvahg-o", "libScePosix", 1, "libkernel", posix_sem_timedwait);
    LIB_FUNCTION("IKP8typ0QUk", "libScePosix", 1, "libkernel", posix_sem_post);
    LIB_FUNCTION("Bq+LRV-N6Hk", "libScePosix", 1, "libkernel", posix_sem_getvalue);

    LIB_FUNCTION("GEnUkDZoUwY", "libkernel", 1, "libkernel", scePthreadSemInit);
    LIB_FUNCTION("Vwc+L05e6oE", "libkernel", 1, "libkernel", scePthreadSemDestroy);
    LIB_FUNCTION("C36iRE0F5sE", "libkernel", 1, "libkernel", scePthreadSemWait);
    LIB_FUNCTION("H2a+IN9TP0E", "libkernel", 1, "libkernel", scePthreadSemTrywait);
    LIB_FUNCTION("fjN6NQHhK8k", "libkernel", 1, "libkernel", scePthreadSemTimedwait);
    LIB_FUNCTION("aishVAiFaYM", "libkernel", 1, "libkernel", scePthreadSemPost);
    LIB_FUNCTION("DjpBvGlaWbQ", "libkernel", 1, "libkernel", scePthreadSemGetvalue);
}

}
