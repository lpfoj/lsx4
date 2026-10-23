// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#include <cstdio>
#endif

#include "aio.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/cpu_tuning_policy.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "core/file_sys/fs.h"
#include "core/libraries/kernel/equeue.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "file_system.h"

namespace Libraries::Kernel {

#ifdef __ANDROID__
static bool AioProbeEnabled() {
    static const bool enabled = [] {
        constexpr const char* path =
            "/data/data/app.lsx4.android/files/lsx4-home/diag-kernel-cond-wake";
        std::FILE* file = std::fopen(path, "r");
        if (file == nullptr) {
            return false;
        }
        std::fclose(file);
        return true;
    }();
    return enabled;
}

static void TraceAio(const char* phase, s32 id, s32 index, s32 fd, u64 size,
                     s64 offset, s64 result, s32 state) {
    if (!AioProbeEnabled()) {
        return;
    }
    static std::atomic_int budget{256};
    if (budget.fetch_sub(1, std::memory_order_relaxed) <= 0) {
        return;
    }
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_AIO_PROBE] phase=%s id=%d index=%d fd=%d size=%llu "
        "offset=%lld result=%lld state=%d",
        phase, id, index, fd, static_cast<unsigned long long>(size),
        static_cast<long long>(offset), static_cast<long long>(result), state);
}
#else
static void TraceAio(const char*, s32, s32, s32, u64, s64, s64, s32) {}
#endif

#define MAX_QUEUE 512

static std::array<std::atomic<s32>, MAX_QUEUE> id_state{};
// An aborted request is terminal to the guest, but its worker may still be
// returning from host I/O. Such a slot must not be recycled until the worker
// retires, or the old completion can corrupt a newer request with the same id.
static std::array<bool, MAX_QUEUE> id_active{};
static std::atomic<u32> id_index{0};
static std::mutex aio_state_mutex;
static std::condition_variable aio_state_condition;
// Guest result records are part of the observable AIO state. Keep the records
// associated with their submit id so cancellation and completion can publish
// both views under the same lock. Otherwise a waiter may observe a terminal id
// while its result record still says PROCESSING.
static std::array<std::vector<OrbisKernelAioResult*>, MAX_QUEUE> id_results{};

static s32 FinishAioState(const s32 id, const s32 requested_state) {
    s32 observed;
    {
        std::lock_guard lock{aio_state_mutex};
        const auto index = static_cast<std::size_t>(id);
        observed = id_state[index].load(std::memory_order_acquire);
        if (observed == ORBIS_KERNEL_AIO_STATE_PROCESSING) {
            for (auto* result : id_results[index]) {
                if (result != nullptr) result->state = requested_state;
            }
            id_state[index].store(requested_state, std::memory_order_release);
            observed = requested_state;
        }
        id_active[index] = false;
    }
    aio_state_condition.notify_all();
    return observed;
}

class AioWorker {
public:
    AioWorker() : thread{[this](std::stop_token token) { Run(token); }} {}

    void Submit(std::function<void()> job) {
        {
            std::lock_guard lock{mutex};
            jobs.emplace_back(std::move(job));
        }
        condition.notify_one();
    }

    void Drain() {
        std::unique_lock lock{mutex};
        idle.wait(lock, [this] { return jobs.empty() && !running; });
    }

private:
    void Run(std::stop_token token) {
        Common::SetCurrentThreadName("shadPS4:AIO");
        while (!token.stop_requested()) {
            std::function<void()> job;
            {
                std::unique_lock lock{mutex};
                condition.wait(lock, token,
                               [this] { return !jobs.empty(); });
                if (token.stop_requested()) return;
                job = std::move(jobs.front());
                jobs.pop_front();
                running = true;
            }
            job();
            // Release captured guest pointers and file leases before Drain()
            // is allowed to report the session idle.
            job = {};
            {
                std::lock_guard lock{mutex};
                running = false;
            }
            idle.notify_all();
        }
    }

    std::mutex mutex;
    std::condition_variable_any condition;
    std::condition_variable idle;
    std::deque<std::function<void()>> jobs;
    bool running{};
    std::jthread thread;
};

static std::atomic<AioWorker*> aio_worker_instance{};

static AioWorker& GetAioWorker() {
    static AioWorker worker;
    aio_worker_instance.store(&worker, std::memory_order_release);
    return worker;
}

static s32 ReserveAioId() {
    std::lock_guard lock{aio_state_mutex};
    // A single cyclic pass naturally consumes untouched slots before wrap and
    // then reclaims inactive terminal slots. A two-pass free-first scan adds
    // 511 failed checks to every submission after the first full cycle.
    for (s32 attempt = 0; attempt < MAX_QUEUE - 1; ++attempt) {
        const u32 ticket = id_index.fetch_add(1, std::memory_order_relaxed);
        const s32 normalized = 1 + static_cast<s32>(
            ticket % static_cast<u32>(MAX_QUEUE - 1));
        const auto index = static_cast<std::size_t>(normalized);
        const s32 state = id_state[index].load(std::memory_order_acquire);
        const bool reusable = state == 0 ||
                              state == ORBIS_KERNEL_AIO_STATE_COMPLETED ||
                              state == ORBIS_KERNEL_AIO_STATE_ABORTED;
        if (!id_active[index] && reusable) {
            id_results[index].clear();
            id_active[index] = true;
            id_state[index].store(ORBIS_KERNEL_AIO_STATE_PROCESSING,
                                  std::memory_order_release);
            return normalized;
        }
    }
    return 0;
}

static void BindAioResults(const s32 id,
                           std::vector<OrbisKernelAioResult*> results) {
    std::lock_guard lock{aio_state_mutex};
    for (auto* result : results) {
        result->returnValue = 0;
        result->state = ORBIS_KERNEL_AIO_STATE_PROCESSING;
    }
    id_results[static_cast<std::size_t>(id)] = std::move(results);
}

static bool StoreAioResult(const s32 id, OrbisKernelAioResult* result,
                           const s64 return_value, const s32 requested_state,
                           const bool finish_id) {
    bool accepted = false;
    bool notify = false;
    {
        std::lock_guard lock{aio_state_mutex};
        const auto index = static_cast<std::size_t>(id);
        const s32 observed = id_state[index].load(std::memory_order_acquire);
        if (observed == ORBIS_KERNEL_AIO_STATE_PROCESSING) {
            accepted = true;
            result->returnValue = return_value;
            result->state = requested_state;
            if (finish_id) {
                id_active[index] = false;
                id_state[index].store(requested_state, std::memory_order_release);
                notify = true;
            }
        }
    }
    if (notify) aio_state_condition.notify_all();
    return accepted;
}

static void RetireCanceledAioRequest(const s32 id) {
    {
        std::lock_guard lock{aio_state_mutex};
        const auto index = static_cast<std::size_t>(id);
        if (id_state[index].load(std::memory_order_acquire) ==
            ORBIS_KERNEL_AIO_STATE_ABORTED) {
            id_active[index] = false;
        }
    }
    aio_state_condition.notify_all();
}

static void AbortAioBatch(const s32 id,
                          std::vector<OrbisKernelAioRWRequest>& requests,
                          const s32 first, const s64 return_value) {
    {
        std::lock_guard lock{aio_state_mutex};
        const auto state_index = static_cast<std::size_t>(id);
        if (id_state[state_index].load(std::memory_order_acquire) ==
            ORBIS_KERNEL_AIO_STATE_PROCESSING) {
            for (s32 index = first; index < static_cast<s32>(requests.size()); ++index) {
                auto* result = requests[index].result;
                if (result == nullptr) continue;
                result->returnValue = return_value;
                result->state = ORBIS_KERNEL_AIO_STATE_ABORTED;
            }
            id_state[state_index].store(ORBIS_KERNEL_AIO_STATE_ABORTED,
                                        std::memory_order_release);
        }
        id_active[state_index] = false;
    }
    aio_state_condition.notify_all();
}

static void SubmitAio(std::function<void()> job) {
    const auto policy = Common::GetCpuTuningPolicy();
    if (policy.subsystem_multithreading != 0 &&
        policy.separate_io_thread != 0) {
        GetAioWorker().Submit(std::move(job));
    } else {
        job();
    }
}

s32 PS4_SYSV_ABI sceKernelAioInitializeImpl(void* p, s32 size) {

    return 0;
}

s32 PS4_SYSV_ABI sceKernelAioDeleteRequest(OrbisKernelAioSubmitId id, s32* ret) {
    if (ret == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    if (id <= 0 || id >= MAX_QUEUE) return ORBIS_KERNEL_ERROR_EINVAL;
    {
        std::lock_guard lock{aio_state_mutex};
        const auto index = static_cast<std::size_t>(id);
        const s32 observed = id_state[index].load(std::memory_order_acquire);
        if (id_active[index] ||
            observed == ORBIS_KERNEL_AIO_STATE_PROCESSING) {
            *ret = ORBIS_KERNEL_ERROR_EBUSY;
            return 0;
        }
        id_results[index].clear();
        id_state[index].store(0, std::memory_order_release);
    }
    aio_state_condition.notify_all();
    *ret = 0;
    return 0;
}

s32 PS4_SYSV_ABI sceKernelAioDeleteRequests(OrbisKernelAioSubmitId id[], s32 num, s32 ret[]) {
    if (id == nullptr || ret == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    if (num <= 0) return ORBIS_KERNEL_ERROR_EINVAL;
    for (s32 i = 0; i < num; i++) {
        if (id[i] <= 0 || id[i] >= MAX_QUEUE) return ORBIS_KERNEL_ERROR_EINVAL;
        {
            std::lock_guard lock{aio_state_mutex};
            const auto index = static_cast<std::size_t>(id[i]);
            const s32 observed = id_state[index].load(std::memory_order_acquire);
            if (id_active[index] ||
                observed == ORBIS_KERNEL_AIO_STATE_PROCESSING) {
                ret[i] = ORBIS_KERNEL_ERROR_EBUSY;
            } else {
                id_results[index].clear();
                id_state[index].store(0, std::memory_order_release);
                ret[i] = 0;
            }
        }
        if (ret[i] == 0) aio_state_condition.notify_all();
    }

    return 0;
}
s32 PS4_SYSV_ABI sceKernelAioPollRequest(OrbisKernelAioSubmitId id, s32* state) {
    if (state == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    if (id <= 0 || id >= MAX_QUEUE) return ORBIS_KERNEL_ERROR_EINVAL;
    *state = id_state[static_cast<std::size_t>(id)].load(std::memory_order_acquire);
    return 0;
}

s32 PS4_SYSV_ABI sceKernelAioPollRequests(OrbisKernelAioSubmitId id[], s32 num, s32 state[]) {
    if (id == nullptr || state == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    if (num <= 0) return ORBIS_KERNEL_ERROR_EINVAL;
    for (s32 i = 0; i < num; i++) {
        if (id[i] <= 0 || id[i] >= MAX_QUEUE) return ORBIS_KERNEL_ERROR_EINVAL;
        state[i] = id_state[static_cast<std::size_t>(id[i])].load(
            std::memory_order_acquire);
    }

    return 0;
}

s32 PS4_SYSV_ABI sceKernelAioCancelRequest(OrbisKernelAioSubmitId id, s32* state) {
    if (state == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    if (id) {
        if (id <= 0 || id >= MAX_QUEUE) return ORBIS_KERNEL_ERROR_EINVAL;
        {
            std::lock_guard lock{aio_state_mutex};
            const auto index = static_cast<std::size_t>(id);
            const s32 observed = id_state[index].load(std::memory_order_acquire);
            if (observed == ORBIS_KERNEL_AIO_STATE_PROCESSING) {
                for (auto* result : id_results[index]) {
                    if (result == nullptr) continue;
                    result->returnValue = ORBIS_KERNEL_ERROR_ECANCELED;
                    result->state = ORBIS_KERNEL_AIO_STATE_ABORTED;
                }
                id_state[index].store(ORBIS_KERNEL_AIO_STATE_ABORTED,
                                      std::memory_order_release);
                *state = ORBIS_KERNEL_AIO_STATE_ABORTED;
            } else {
                *state = observed;
            }
        }
        aio_state_condition.notify_all();
    } else {
        *state = ORBIS_KERNEL_AIO_STATE_PROCESSING;
    }
    return 0;
}

s32 PS4_SYSV_ABI sceKernelAioCancelRequests(OrbisKernelAioSubmitId id[], s32 num, s32 state[]) {
    if (id == nullptr || state == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    if (num <= 0) return ORBIS_KERNEL_ERROR_EINVAL;
    for (s32 i = 0; i < num; i++) {
        if (id[i]) {
            if (id[i] <= 0 || id[i] >= MAX_QUEUE) return ORBIS_KERNEL_ERROR_EINVAL;
            {
                std::lock_guard lock{aio_state_mutex};
                const auto index = static_cast<std::size_t>(id[i]);
                const s32 observed = id_state[index].load(std::memory_order_acquire);
                if (observed == ORBIS_KERNEL_AIO_STATE_PROCESSING) {
                    for (auto* result : id_results[index]) {
                        if (result == nullptr) continue;
                        result->returnValue = ORBIS_KERNEL_ERROR_ECANCELED;
                        result->state = ORBIS_KERNEL_AIO_STATE_ABORTED;
                    }
                    id_state[index].store(ORBIS_KERNEL_AIO_STATE_ABORTED,
                                          std::memory_order_release);
                    state[i] = ORBIS_KERNEL_AIO_STATE_ABORTED;
                } else {
                    state[i] = observed;
                }
            }
            aio_state_condition.notify_all();
        } else {
            state[i] = ORBIS_KERNEL_AIO_STATE_PROCESSING;
        }
    }

    return 0;
}

s32 PS4_SYSV_ABI sceKernelAioWaitRequest(OrbisKernelAioSubmitId id, s32* state, u32* usec) {
    if (state == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    if (id <= 0 || id >= MAX_QUEUE) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    std::unique_lock lock{aio_state_mutex};
    const auto finished = [id] {
        return id_state[static_cast<std::size_t>(id)].load(
                   std::memory_order_acquire) !=
               ORBIS_KERNEL_AIO_STATE_PROCESSING;
    };
    bool ready = true;
    // The Orbis API and the previous implementation treat a null/zero
    // timeout as an unbounded wait, not as an immediate poll.
    if (usec == nullptr || *usec == 0) {
        aio_state_condition.wait(lock, finished);
    } else {
        ready = aio_state_condition.wait_for(
            lock, std::chrono::microseconds{*usec}, finished);
    }
    *state = id_state[static_cast<std::size_t>(id)].load(std::memory_order_acquire);
    return ready ? 0 : ORBIS_KERNEL_ERROR_ETIMEDOUT;
}

s32 PS4_SYSV_ABI sceKernelAioWaitRequests(OrbisKernelAioSubmitId id[], s32 num, s32 state[],
                                          u32 mode, u32* usec) {
    if (id == nullptr || state == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    if (num <= 0 || (mode != 0x01 && mode != 0x02)) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    for (s32 i = 0; i < num; i++) {
        if (id[i] <= 0 || id[i] >= MAX_QUEUE) {
            return ORBIS_KERNEL_ERROR_EINVAL;
        }
    }
    std::unique_lock lock{aio_state_mutex};
    const auto requested_state_reached = [&] {
        bool any_finished = false;
        bool all_finished = true;
        for (s32 i = 0; i < num; ++i) {
            state[i] = id_state[static_cast<std::size_t>(id[i])].load(
                std::memory_order_acquire);
            const bool finished = state[i] != ORBIS_KERNEL_AIO_STATE_PROCESSING;
            any_finished |= finished;
            all_finished &= finished;
        }
        return (mode == 0x02 && any_finished) ||
               (mode == 0x01 && all_finished);
    };
    bool ready = true;
    if (usec == nullptr || *usec == 0) {
        aio_state_condition.wait(lock, requested_state_reached);
    } else {
        ready = aio_state_condition.wait_for(
            lock, std::chrono::microseconds{*usec}, requested_state_reached);
    }
    (void)requested_state_reached();
    return ready ? 0 : ORBIS_KERNEL_ERROR_ETIMEDOUT;
}

static s64 ExecuteAioRequest(OrbisKernelAioRWRequest& request,
                             const Core::FileSys::AsyncFileLease& file,
                             const bool write) {
    if (!file) return ORBIS_KERNEL_ERROR_EBADF;
    return write ? sceKernelPwrite(request.fd, request.buf, request.nbyte, request.offset)
                 : sceKernelPread(request.fd, request.buf, request.nbyte, request.offset);
}

static std::vector<Core::FileSys::AsyncFileLease> AcquireAioFiles(
    const OrbisKernelAioRWRequest req[], const s32 size) {
    auto* handles = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    std::vector<Core::FileSys::AsyncFileLease> files;
    files.reserve(static_cast<std::size_t>(size));
    for (s32 index = 0; index < size; ++index) {
        files.push_back(handles->AcquireFileForAsync(req[index].fd));
    }
    return files;
}

static s32 SubmitAioBatch(OrbisKernelAioRWRequest req[], const s32 size,
                          OrbisKernelAioSubmitId* id, const bool write) {
    if (req == nullptr || id == nullptr) return ORBIS_KERNEL_ERROR_EFAULT;
    if (size <= 0) return ORBIS_KERNEL_ERROR_EINVAL;
    for (s32 index = 0; index < size; ++index) {
        if (req[index].result == nullptr) return ORBIS_KERNEL_ERROR_EFAULT;
    }
    auto files = AcquireAioFiles(req, size);
    const s32 submit_id = ReserveAioId();
    if (submit_id == 0) return ORBIS_KERNEL_ERROR_EBUSY;
    std::vector<OrbisKernelAioResult*> results;
    results.reserve(static_cast<std::size_t>(size));
    for (s32 index = 0; index < size; ++index) {
        results.push_back(req[index].result);
    }
    BindAioResults(submit_id, std::move(results));
    *id = submit_id;
    TraceAio(write ? "submit-write-batch" : "submit-read-batch", submit_id, -1,
             req[0].fd, req[0].nbyte, req[0].offset, size,
             ORBIS_KERNEL_AIO_STATE_PROCESSING);
    std::vector<OrbisKernelAioRWRequest> requests{
        req, req + static_cast<std::ptrdiff_t>(size)};
    SubmitAio([requests = std::move(requests), files = std::move(files),
               submit_id, write]() mutable {
        const s32 size = static_cast<s32>(requests.size());
        for (s32 index = 0; index < size; ++index) {
            if (id_state[static_cast<std::size_t>(submit_id)].load(
                    std::memory_order_acquire) == ORBIS_KERNEL_AIO_STATE_ABORTED) {
                RetireCanceledAioRequest(submit_id);
                return;
            }
            const s64 result = ExecuteAioRequest(
                requests[index], files[static_cast<std::size_t>(index)], write);
            TraceAio(write ? "finish-write" : "finish-read", submit_id, index,
                     requests[index].fd, requests[index].nbyte, requests[index].offset,
                     result, result < 0 ? ORBIS_KERNEL_AIO_STATE_ABORTED
                                        : ORBIS_KERNEL_AIO_STATE_COMPLETED);
            if (id_state[static_cast<std::size_t>(submit_id)].load(
                    std::memory_order_acquire) == ORBIS_KERNEL_AIO_STATE_ABORTED) {
                RetireCanceledAioRequest(submit_id);
                return;
            }
            const s32 state = result < 0 ? ORBIS_KERNEL_AIO_STATE_ABORTED
                                         : ORBIS_KERNEL_AIO_STATE_COMPLETED;
            if (result < 0) {
                AbortAioBatch(submit_id, requests, index, result);
                return;
            }
            if (!StoreAioResult(submit_id, requests[index].result, result,
                                state, false)) {
                RetireCanceledAioRequest(submit_id);
                return;
            }
        }
        (void)FinishAioState(submit_id, ORBIS_KERNEL_AIO_STATE_COMPLETED);
    });
    return ORBIS_OK;
}

static s32 SubmitAioMultiple(OrbisKernelAioRWRequest req[], const s32 size,
                             OrbisKernelAioSubmitId id[], const bool write) {
    if (req == nullptr || id == nullptr) return ORBIS_KERNEL_ERROR_EFAULT;
    if (size <= 0) return ORBIS_KERNEL_ERROR_EINVAL;
    for (s32 index = 0; index < size; ++index) {
        if (req[index].result == nullptr) return ORBIS_KERNEL_ERROR_EFAULT;
    }
    auto files = AcquireAioFiles(req, size);
    std::vector<s32> ids(static_cast<std::size_t>(size));
    for (s32 index = 0; index < size; ++index) {
        ids[static_cast<std::size_t>(index)] = ReserveAioId();
        if (ids[static_cast<std::size_t>(index)] == 0) {
            for (s32 reserved = 0; reserved < index; ++reserved) {
                const auto reserved_index =
                    static_cast<std::size_t>(ids[reserved]);
                std::lock_guard lock{aio_state_mutex};
                id_active[reserved_index] = false;
                id_results[reserved_index].clear();
                id_state[reserved_index].store(0, std::memory_order_release);
            }
            return ORBIS_KERNEL_ERROR_EBUSY;
        }
        BindAioResults(ids[static_cast<std::size_t>(index)],
                       {req[index].result});
        id[index] = ids[static_cast<std::size_t>(index)];
        TraceAio(write ? "submit-write-multiple" : "submit-read-multiple",
                 id[index], index, req[index].fd, req[index].nbyte,
                 req[index].offset, size, ORBIS_KERNEL_AIO_STATE_PROCESSING);
    }
    std::vector<OrbisKernelAioRWRequest> requests{
        req, req + static_cast<std::ptrdiff_t>(size)};
    SubmitAio([requests = std::move(requests), files = std::move(files),
               ids = std::move(ids), write]() mutable {
        const s32 size = static_cast<s32>(requests.size());
        for (s32 index = 0; index < size; ++index) {
            const s32 submit_id = ids[static_cast<std::size_t>(index)];
            if (id_state[static_cast<std::size_t>(submit_id)].load(
                    std::memory_order_acquire) == ORBIS_KERNEL_AIO_STATE_ABORTED) {
                RetireCanceledAioRequest(submit_id);
                continue;
            }
            const s64 result = ExecuteAioRequest(
                requests[index], files[static_cast<std::size_t>(index)], write);
            TraceAio(write ? "finish-write-multiple" : "finish-read-multiple",
                     submit_id, index, requests[index].fd, requests[index].nbyte,
                     requests[index].offset, result,
                     result < 0 ? ORBIS_KERNEL_AIO_STATE_ABORTED
                                : ORBIS_KERNEL_AIO_STATE_COMPLETED);
            if (id_state[static_cast<std::size_t>(submit_id)].load(
                    std::memory_order_acquire) == ORBIS_KERNEL_AIO_STATE_ABORTED) {
                RetireCanceledAioRequest(submit_id);
                continue;
            }
            const s32 state = result < 0 ? ORBIS_KERNEL_AIO_STATE_ABORTED
                                         : ORBIS_KERNEL_AIO_STATE_COMPLETED;
            if (!StoreAioResult(submit_id, requests[index].result, result,
                                state, true)) {
                RetireCanceledAioRequest(submit_id);
            }
        }
    });
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelAioSubmitReadCommands(OrbisKernelAioRWRequest req[], s32 size,
                                                s32 prio, OrbisKernelAioSubmitId* id) {
    return SubmitAioBatch(req, size, id, false);
}

s32 PS4_SYSV_ABI sceKernelAioSubmitReadCommandsMultiple(OrbisKernelAioRWRequest req[], s32 size,
                                                        s32 prio, OrbisKernelAioSubmitId id[]) {
    return SubmitAioMultiple(req, size, id, false);
}

s32 PS4_SYSV_ABI sceKernelAioSubmitWriteCommands(OrbisKernelAioRWRequest req[], s32 size,
                                                 s32 prio, OrbisKernelAioSubmitId* id) {
    return SubmitAioBatch(req, size, id, true);
}

s32 PS4_SYSV_ABI sceKernelAioSubmitWriteCommandsMultiple(OrbisKernelAioRWRequest req[], s32 size,
                                                         s32 prio, OrbisKernelAioSubmitId id[]) {
    return SubmitAioMultiple(req, size, id, true);
}

s32 PS4_SYSV_ABI sceKernelAioSetParam() {
    LOG_ERROR(Kernel, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelAioInitializeParam() {
    LOG_ERROR(Kernel, "(STUBBED) called");
    return ORBIS_OK;
}

void RegisterAio(Core::Loader::SymbolsResolver* sym) {
    // The Android game process can host consecutive sessions. Never reset ids
    // while an old worker closure can still write the previous guest address
    // space or publish a terminal state into the new session.
    if (auto* worker = aio_worker_instance.load(std::memory_order_acquire)) {
        worker->Drain();
    }
    id_index.store(0, std::memory_order_release);
    {
        std::lock_guard lock{aio_state_mutex};
        for (std::size_t index = 0; index < id_state.size(); ++index) {
            id_active[index] = false;
            id_results[index].clear();
            id_state[index].store(0, std::memory_order_relaxed);
        }
    }

    LIB_FUNCTION("fR521KIGgb8", "libkernel", 1, "libkernel", sceKernelAioCancelRequest);
    LIB_FUNCTION("3Lca1XBrQdY", "libkernel", 1, "libkernel", sceKernelAioCancelRequests);
    LIB_FUNCTION("5TgME6AYty4", "libkernel", 1, "libkernel", sceKernelAioDeleteRequest);
    LIB_FUNCTION("Ft3EtsZzAoY", "libkernel", 1, "libkernel", sceKernelAioDeleteRequests);
    LIB_FUNCTION("vYU8P9Td2Zo", "libkernel", 1, "libkernel", sceKernelAioInitializeImpl);
    LIB_FUNCTION("nu4a0-arQis", "libkernel", 1, "libkernel", sceKernelAioInitializeParam);
    LIB_FUNCTION("2pOuoWoCxdk", "libkernel", 1, "libkernel", sceKernelAioPollRequest);
    LIB_FUNCTION("o7O4z3jwKzo", "libkernel", 1, "libkernel", sceKernelAioPollRequests);
    LIB_FUNCTION("9WK-vhNXimw", "libkernel", 1, "libkernel", sceKernelAioSetParam);
    LIB_FUNCTION("HgX7+AORI58", "libkernel", 1, "libkernel", sceKernelAioSubmitReadCommands);
    LIB_FUNCTION("lXT0m3P-vs4", "libkernel", 1, "libkernel",
                 sceKernelAioSubmitReadCommandsMultiple);
    LIB_FUNCTION("XQ8C8y+de+E", "libkernel", 1, "libkernel", sceKernelAioSubmitWriteCommands);
    LIB_FUNCTION("xT3Cpz0yh6Y", "libkernel", 1, "libkernel",
                 sceKernelAioSubmitWriteCommandsMultiple);
    LIB_FUNCTION("KOF-oJbQVvc", "libkernel", 1, "libkernel", sceKernelAioWaitRequest);
    LIB_FUNCTION("lgK+oIWkJyA", "libkernel", 1, "libkernel", sceKernelAioWaitRequests);
}

}
