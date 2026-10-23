// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/libraries/kernel/sync/mutex.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "common/assert.h"

namespace Libraries::Kernel {
#if defined(LSX4_APPLE_RUNTIME)
bool AppleAtomicGuestMutexSelected() noexcept {
    static const bool selected=[] {
        const char* value=std::getenv("EXECUTOR_ATOMIC_TIMED_MUTEX");
        const bool enabled=value && std::strcmp(value,"1")==0;
        std::fprintf(stderr,"[APPLE_ATOMIC_TIMED_MUTEX] selected=%d processImmutable=1 "
            "legacySize=%zu fastSize=%zu wrapperSize=%zu ownershipLayerUnchanged=1\n",
            enabled,sizeof(std::timed_mutex),sizeof(Lsx4::Portable::AtomicTimedMutex),sizeof(TimedMutex));
        return enabled;
    }();
    return selected;
}
#endif

TimedMutex::TimedMutex() {
#if defined(LSX4_APPLE_RUNTIME)
    if(AppleAtomicGuestMutexSelected()) ::new(static_cast<void*>(&mtx.fast)) Lsx4::Portable::AtomicTimedMutex();
    else ::new(static_cast<void*>(&mtx.legacy)) std::timed_mutex();
#elif defined(_WIN64)
    mtx = CreateMutex(nullptr, false, nullptr);
    ASSERT(mtx);
#endif
}

TimedMutex::~TimedMutex() {
#if defined(LSX4_APPLE_RUNTIME)
    if(AppleAtomicGuestMutexSelected()) mtx.fast.~AtomicTimedMutex();
    else mtx.legacy.~timed_mutex();
#elif defined(_WIN64)
    CloseHandle(mtx);
#endif
}

void TimedMutex::lock() {
#if defined(LSX4_APPLE_RUNTIME)
    if(AppleAtomicGuestMutexSelected()) mtx.fast.lock();
    else mtx.legacy.lock();
#elif defined(_WIN64)
    for (;;) {
        u64 res = WaitForSingleObjectEx(mtx, INFINITE, true);
        if (res == WAIT_OBJECT_0) {
            return;
        }
    }
#else
    mtx.lock();
#endif
}

bool TimedMutex::try_lock() {
#if defined(LSX4_APPLE_RUNTIME)
    return AppleAtomicGuestMutexSelected() ? mtx.fast.try_lock() : mtx.legacy.try_lock();
#elif defined(_WIN64)
    return WaitForSingleObjectEx(mtx, 0, true) == WAIT_OBJECT_0;
#else
    return mtx.try_lock();
#endif
}

void TimedMutex::unlock() {
#if defined(LSX4_APPLE_RUNTIME)
    if(AppleAtomicGuestMutexSelected()) mtx.fast.unlock();
    else mtx.legacy.unlock();
#elif defined(_WIN64)
    ReleaseMutex(mtx);
#else
    mtx.unlock();
#endif
}

}
