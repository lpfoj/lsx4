// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "usbd.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <new>
#include <sys/time.h>

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"

namespace Libraries::Usbd {
namespace {

using namespace std::chrono_literals;

constexpr auto DefaultEventWait = 1s;
constexpr auto MaximumEventWait = 60s;
#ifdef __ANDROID__
constexpr auto AlertableEventWaitSlice = 20ms;
extern "C" void executor_live_mono_run_pending_signal_safe_point() __attribute__((weak));
#endif

std::mutex state_mutex;
std::condition_variable event_cv;
std::recursive_mutex event_mutex;
std::mutex waiter_mutex;
std::atomic<u32> init_count{};
std::atomic<u32> active_handlers{};
u64 event_generation{};

class ActiveHandler final {
public:
    ActiveHandler() {
        active_handlers.fetch_add(1, std::memory_order_acq_rel);
    }

    ~ActiveHandler() {
        active_handlers.fetch_sub(1, std::memory_order_acq_rel);
    }
};

std::chrono::microseconds EventWaitFrom(const timeval* timeout,
                                        std::chrono::microseconds fallback) {
    if (timeout == nullptr) {
        return fallback;
    }

    const auto seconds = std::clamp<std::int64_t>(timeout->tv_sec, 0, 60);
    const auto micros = std::clamp<std::int64_t>(timeout->tv_usec, 0, 999999);
    return std::min(std::chrono::seconds{seconds} + std::chrono::microseconds{micros},
                    std::chrono::duration_cast<std::chrono::microseconds>(MaximumEventWait));
}

s32 WaitForBackendEvent(std::chrono::microseconds timeout) {
    if (init_count.load(std::memory_order_acquire) == 0) {
        return 0x80240004; // SCE_USBD_ERROR_NO_DEVICE
    }

    ActiveHandler handler;
    std::unique_lock lock{state_mutex};
    const u64 observed_generation = event_generation;
    const auto event_ready = [observed_generation] {
        return init_count.load(std::memory_order_acquire) == 0 ||
               event_generation != observed_generation;
    };
#ifdef __ANDROID__
    // A PS4 USB event thread may remain in this call for the lifetime of the
    // process. Keep the native wait alertable so FEX can deliver a pending
    // guest suspend at a normal HLE safe-point while another thread loads or
    // unloads a module. Holding the wait for its full timeout would strand the
    // event thread outside translated code and can stall a process-wide stop.
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!event_ready()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            break;
        }
        event_cv.wait_until(lock, std::min(deadline, now + AlertableEventWaitSlice),
                            event_ready);
        lock.unlock();
        if (executor_live_mono_run_pending_signal_safe_point != nullptr) {
            executor_live_mono_run_pending_signal_safe_point();
        }
        lock.lock();
    }
#else
    event_cv.wait_for(lock, timeout, event_ready);
#endif
    return ORBIS_OK;
}

} // namespace

s32 PS4_SYSV_ABI sceUsbdInit() {
    // PS4 exposes an initialized USB service even when no devices are attached.
    // Android currently models that empty bus rather than a missing service so
    // callers can complete their normal subsystem construction.
    init_count.fetch_add(1, std::memory_order_acq_rel);
    LOG_DEBUG(Lib_Usbd, "initialized Android empty-device backend");
    return ORBIS_OK;
}

void PS4_SYSV_ABI sceUsbdExit() {
    u32 count = init_count.load(std::memory_order_acquire);
    while (count != 0 &&
           !init_count.compare_exchange_weak(count, count - 1, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
    }

    if (count == 1) {
        std::lock_guard lock{state_mutex};
        ++event_generation;
        event_cv.notify_all();
    }
    LOG_DEBUG(Lib_Usbd, "exited Android no-device backend");
}

s64 PS4_SYSV_ABI sceUsbdGetDeviceList(void*** list) {
    if (list == nullptr) {
        return 0x80240002; // SCE_USBD_ERROR_INVALID_PARAM
    }

    auto* empty_list = new (std::nothrow) void*[1];
    if (empty_list == nullptr) {
        return 0x8024000B; // SCE_USBD_ERROR_NO_MEM
    }
    empty_list[0] = nullptr;
    *list = empty_list;
    return 0;
}

void PS4_SYSV_ABI sceUsbdFreeDeviceList(void** list, s32) {
    delete[] list;
}

s32 PS4_SYSV_ABI sceUsbdHandleEvents() {
    return WaitForBackendEvent(
        std::chrono::duration_cast<std::chrono::microseconds>(DefaultEventWait));
}

s32 PS4_SYSV_ABI sceUsbdHandleEventsLocked(timeval* timeout) {
    return WaitForBackendEvent(EventWaitFrom(timeout, DefaultEventWait));
}

s32 PS4_SYSV_ABI sceUsbdHandleEventsTimeout(timeval* timeout) {
    std::lock_guard lock{event_mutex};
    return WaitForBackendEvent(EventWaitFrom(timeout, DefaultEventWait));
}

s32 PS4_SYSV_ABI sceUsbdEventHandlerActive() {
    return active_handlers.load(std::memory_order_acquire) != 0 ? 1 : 0;
}

s32 PS4_SYSV_ABI sceUsbdEventHandlingOk() {
    return init_count.load(std::memory_order_acquire) != 0 ? 1 : 0;
}

void PS4_SYSV_ABI sceUsbdLockEvents() {
    event_mutex.lock();
}

s32 PS4_SYSV_ABI sceUsbdTryLockEvents() {
    return event_mutex.try_lock() ? 0 : 1;
}

void PS4_SYSV_ABI sceUsbdUnlockEvents() {
    event_mutex.unlock();
}

void PS4_SYSV_ABI sceUsbdLockEventWaiters() {
    waiter_mutex.lock();
}

void PS4_SYSV_ABI sceUsbdUnlockEventWaiters() {
    waiter_mutex.unlock();
}

s32 PS4_SYSV_ABI sceUsbdWaitForEvent(timeval* timeout) {
    return WaitForBackendEvent(EventWaitFrom(timeout, DefaultEventWait));
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("TOhg7P6kTH4", "libSceUsbd", 1, "libSceUsbd", sceUsbdInit);
    LIB_FUNCTION("Fq6+0Fm55xU", "libSceUsbd", 1, "libSceUsbd", sceUsbdExit);
    LIB_FUNCTION("8qB9Ar4P5nc", "libSceUsbd", 1, "libSceUsbd", sceUsbdGetDeviceList);
    LIB_FUNCTION("EQ6SCLMqzkM", "libSceUsbd", 1, "libSceUsbd", sceUsbdFreeDeviceList);
    LIB_FUNCTION("EkqGLxWC-S0", "libSceUsbd", 1, "libSceUsbd", sceUsbdHandleEvents);
    LIB_FUNCTION("rt-WeUGibfg", "libSceUsbd", 1, "libSceUsbd",
                 sceUsbdHandleEventsLocked);
    LIB_FUNCTION("+wU6CGuZcWk", "libSceUsbd", 1, "libSceUsbd",
                 sceUsbdHandleEventsTimeout);
    LIB_FUNCTION("Vw8Hg1CN028", "libSceUsbd", 1, "libSceUsbd",
                 sceUsbdEventHandlerActive);
    LIB_FUNCTION("e7gp1xhu6RI", "libSceUsbd", 1, "libSceUsbd",
                 sceUsbdEventHandlingOk);
    LIB_FUNCTION("u9yKks02-rA", "libSceUsbd", 1, "libSceUsbd", sceUsbdLockEvents);
    LIB_FUNCTION("TcXVGc-LPbQ", "libSceUsbd", 1, "libSceUsbd", sceUsbdTryLockEvents);
    LIB_FUNCTION("RA2D9rFH-Uw", "libSceUsbd", 1, "libSceUsbd", sceUsbdUnlockEvents);
    LIB_FUNCTION("AeGaY8JrAV4", "libSceUsbd", 1, "libSceUsbd",
                 sceUsbdLockEventWaiters);
    LIB_FUNCTION("1DkGvUQYFKI", "libSceUsbd", 1, "libSceUsbd",
                 sceUsbdUnlockEventWaiters);
    LIB_FUNCTION("ys2e9VRBPrY", "libSceUsbd", 1, "libSceUsbd", sceUsbdWaitForEvent);
}

} // namespace Libraries::Usbd
