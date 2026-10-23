// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace Lsx4::Portable {
// The lock bit and waiter count share one atomic, preventing a lost wake when
// an unlock races with admission to the condition-variable slow path.
class AtomicTimedMutex {
public:
    AtomicTimedMutex() = default;
    AtomicTimedMutex(const AtomicTimedMutex&) = delete;
    AtomicTimedMutex& operator=(const AtomicTimedMutex&) = delete;

    bool try_lock() noexcept {
        auto value = state.load(std::memory_order_relaxed);
        for (;;) {
            if (value & Locked) return false;
            if (state.compare_exchange_weak(value, value | Locked,
                    std::memory_order_acquire, std::memory_order_relaxed)) return true;
        }
    }
    void lock() {
        if (try_lock()) return;
        std::unique_lock lock(slowMutex);
        Waiter waiter(*this);
        condition.wait(lock, [this] { return try_lock(); });
    }
    void unlock() noexcept {
        const auto previous = state.fetch_and(~Locked, std::memory_order_release);
        assert(previous & Locked);
        if (previous > Locked) {
            // A waiter holds this mutex until condition.wait atomically parks.
            // Synchronize notification with that transition, after publishing
            // the released lock bit. No slow mutex is touched without waiters.
            std::lock_guard lock(slowMutex);
            condition.notify_one();
        }
    }
    template<class Clock, class Duration>
    bool try_lock_until(const std::chrono::time_point<Clock, Duration>& deadline) {
        if (try_lock()) return true;
        if (deadline <= Clock::now()) return false;
        std::unique_lock lock(slowMutex);
        Waiter waiter(*this);
        return condition.wait_until(lock, deadline, [this] { return try_lock(); });
    }
    template<class Rep, class Period>
    bool try_lock_for(const std::chrono::duration<Rep, Period>& duration) {
        using Clock = std::chrono::steady_clock;
        if (try_lock()) return true;
        const auto seconds = std::chrono::duration<long double>(duration).count();
        if (!(seconds > 0)) return false;
        const auto now = Clock::now();
        const auto maximum = Clock::time_point::max();
        const auto remaining = std::chrono::duration<long double>(maximum - now).count();
        const auto deadline = seconds >= remaining ? maximum :
            now + std::chrono::duration_cast<Clock::duration>(duration);
        return try_lock_until(deadline);
    }
private:
    static constexpr unsigned Locked = 1;
    struct Waiter {
        explicit Waiter(AtomicTimedMutex& mutex) : owner(mutex) {
            owner.state.fetch_add(2, std::memory_order_relaxed);
        }
        ~Waiter() {
            owner.state.fetch_sub(2, std::memory_order_relaxed);
        }
        AtomicTimedMutex& owner;
    };
    std::atomic<unsigned> state{0};
    std::mutex slowMutex;
    std::condition_variable condition;
};
}
