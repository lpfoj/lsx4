// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/libraries/kernel/threads/pthread.h"
#include "core/libraries/kernel/threads/sleepq.h"
#include "core/libraries/kernel/posix_error.h"
#include <atomic>
#include <chrono>
#include <thread>

namespace AppleCondWakeFixture {
using namespace Libraries::Kernel;
using namespace std::chrono_literals;

static void Run() {
    // Use the real guest mutex/condition/sleepqueue implementation. A host wake
    // is not a condition signal: the waiter must remain queued and reacquire
    // its recursive mutex only when signalled or when its deadline expires.
    Pthread main_thread{}, waiter{};
    SleepQueue queue{};
    waiter.sleepqueue=&queue;
    waiter.name="AppleCondWakeFixture";
    struct Current {
        Pthread* prior=g_curthread;
        explicit Current(Pthread* thread) { g_curthread=thread; }
        ~Current() { g_curthread=prior; }
    } current{&main_thread};
    PthreadMutex mutex{};
    mutex.m_flags=static_cast<PthreadMutexFlags>(PthreadMutexType::Recursive);
    mutex.m_protocol=PthreadMutexProt::None;
    PthreadMutexT mutex_cell=&mutex;
    PthreadCond cond{};
    cond.clock_id=ClockId::Realtime;
    std::atomic_bool done{};
    std::atomic_bool valid{};
    auto until=[&](auto predicate) {
        const auto end=std::chrono::steady_clock::now()+2s;
        while(!predicate() && std::chrono::steady_clock::now()<end)
            std::this_thread::sleep_for(50us);
        return predicate();
    };
    for(unsigned pass=0;pass<16;++pass) {
        done=false;valid=false;
        const auto wakes=waiter.spurious_cond_wakes.load();
        std::thread worker([&] {
            Current worker_current{&waiter};
            bool ok=mutex.Lock(nullptr)==0 && mutex.Lock(nullptr)==0;
            ok=ok && cond.Wait(&mutex_cell,nullptr)==0;
            ok=ok && mutex.IsOwned(&waiter)==0 && mutex.m_count==1 &&
                waiter.wchan==nullptr && waiter.mutex_obj==nullptr;
            ok=mutex.Unlock()==0 && ok;
            ok=mutex.Unlock()==0 && ok;
            valid=ok;done=true;
        });
        const bool queued=until([&] {
            SleepqLock(&cond);
            const bool result=waiter.wchan==&cond;
            SleepqUnlock(&cond);
            return result;
        });
        // Post repeatedly until a wake is consumed after ClearWake. Merely
        // observing queue insertion does not prove the worker is parked yet.
        const bool spurious=queued && until([&] {
            if(waiter.spurious_cond_wakes.load()>wakes) return true;
            waiter.PostWake("fixture-spurious");
            return false;
        });
        const bool premature=done.load();
        // This intentionally races the backoff after a spurious wake. A
        // signal that removes wchan must not be consumed by the next sleep.
        const int signalled=cond.Signal(nullptr);
        const bool completed=until([&]{return done.load();});
        if(!completed) waiter.PostWake("fixture-timeout-recovery");
        worker.join();
        Require(queued && spurious && !premature && signalled==0 && completed && valid,
                "Condition spurious wake lost queue/signal/recursive mutex ownership");
        Require(waiter.sleepqueue==&queue && !cond.has_user_waiters,
                "Condition wake leaked its sleepqueue");
    }
    // Timeout is a real guest result and still restores recursive ownership.
    Current waiter_current{&waiter};
    Require(mutex.Lock(nullptr)==0 && mutex.Lock(nullptr)==0,
            "Timed condition fixture mutex lock failed");
    const int timed=cond.Wait(&mutex_cell,THR_RELTIME,2000);
    const bool restored=mutex.IsOwned(&waiter)==0 && mutex.m_count==1 &&
        waiter.wchan==nullptr && waiter.mutex_obj==nullptr && !cond.has_user_waiters;
    const bool unlocked=mutex.Unlock()==0 && mutex.Unlock()==0;
    Require(timed==POSIX_ETIMEDOUT && restored && unlocked,
            "Condition timeout lost its error or recursive mutex ownership");
    std::fprintf(stderr,"[APPLE_COND_WAKE_TEST] spurious=16 signal=16 recursive=passed timeout=passed\n");
}
}
