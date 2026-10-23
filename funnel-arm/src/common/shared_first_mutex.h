// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "common/assert.h"

namespace Common {

class SharedFirstMutex {
public:
    void lock() {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this]() { return !writer_active && readers == 0; });
        writer_active = true;
    }

    bool try_lock() {
        std::lock_guard<std::mutex> lock(mtx);
        if (writer_active || readers > 0) {
            return false;
        }
        writer_active = true;
        return true;
    }

    template <typename Clock, typename Duration>
    bool try_lock_until(const std::chrono::time_point<Clock, Duration>& abs_time) {
        std::unique_lock<std::mutex> lock(mtx);
        if (!cv.wait_until(lock, abs_time, [this]() { return !writer_active && readers == 0; })) {
            return false;
        }
        writer_active = true;
        return true;
    }

    void unlock() {
        std::lock_guard<std::mutex> lock(mtx);
        writer_active = false;
        cv.notify_all();
    }

    void lock_shared() {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this]() { return !writer_active; });
        ++readers;
    }

    bool try_lock_shared() {
        std::lock_guard<std::mutex> lock(mtx);
        if (writer_active) {
            return false;
        }
        ++readers;
        return true;
    }

    template <typename Clock, typename Duration>
    bool try_lock_shared_until(const std::chrono::time_point<Clock, Duration>& abs_time) {
        std::unique_lock<std::mutex> lock(mtx);
        if (!cv.wait_until(lock, abs_time, [this]() { return !writer_active; })) {
            return false;
        }
        ++readers;
        return true;
    }

    void unlock_shared() {
        std::lock_guard<std::mutex> lock(mtx);
        if (--readers == 0) {
            cv.notify_all();
        }
    }

private:
    std::mutex mtx;
    std::condition_variable cv;
    int readers = 0;
    bool writer_active = false;
};

// Shared-first mutex with reader recursion tracked by the mutex itself.  This
// is intended for hot renderer locks whose callbacks can re-enter a shared
// read section.  Keeping ownership here avoids a process-wide thread_local
// lookup on every read while retaining SharedFirstMutex writer semantics.
class RecursiveSharedFirstMutex {
public:
    void lock() {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this]() { return !writer_active && readers == 0; });
        writer_active = true;
    }

    bool try_lock() {
        std::lock_guard<std::mutex> lock(mtx);
        if (writer_active || readers > 0) {
            return false;
        }
        writer_active = true;
        return true;
    }

    template <typename Clock, typename Duration>
    bool try_lock_until(const std::chrono::time_point<Clock, Duration>& abs_time) {
        std::unique_lock<std::mutex> lock(mtx);
        if (!cv.wait_until(lock, abs_time,
                           [this]() { return !writer_active && readers == 0; })) {
            return false;
        }
        writer_active = true;
        return true;
    }

    void unlock() {
        std::lock_guard<std::mutex> lock(mtx);
        if (!writer_active) [[unlikely]] {
            ReaderInvariantFailure("exclusive unlock without an active writer");
        }
        writer_active = false;
        cv.notify_all();
    }

    void lock_shared() {
        const auto owner = std::this_thread::get_id();
        std::unique_lock<std::mutex> lock(mtx);
        if (IncrementReaderOwnerLocked(owner)) {
            return;
        }
        cv.wait(lock, [this]() { return !writer_active; });
        AddReaderOwnerLocked(owner);
    }

    bool try_lock_shared() {
        const auto owner = std::this_thread::get_id();
        std::lock_guard<std::mutex> lock(mtx);
        if (IncrementReaderOwnerLocked(owner)) {
            return true;
        }
        if (writer_active) {
            return false;
        }
        AddReaderOwnerLocked(owner);
        return true;
    }

    template <typename Clock, typename Duration>
    bool try_lock_shared_until(const std::chrono::time_point<Clock, Duration>& abs_time) {
        const auto owner = std::this_thread::get_id();
        std::unique_lock<std::mutex> lock(mtx);
        if (IncrementReaderOwnerLocked(owner)) {
            return true;
        }
        if (!cv.wait_until(lock, abs_time, [this]() { return !writer_active; })) {
            return false;
        }
        AddReaderOwnerLocked(owner);
        return true;
    }

    void unlock_shared() {
        const auto owner = std::this_thread::get_id();
        std::lock_guard<std::mutex> lock(mtx);
        if (!DecrementReaderOwnerLocked(owner)) {
            return;
        }
        if (readers <= 0) [[unlikely]] {
            ReaderInvariantFailure("reader-owner count without a shared reader");
        }
        if (--readers == 0) {
            cv.notify_all();
        }
    }

private:
    struct ReaderOwner {
        std::thread::id thread{};
        int recursion{};
    };

    static constexpr std::size_t InlineReaderCapacity = 16;

    [[noreturn]] SHAD_NO_INLINE static void ReaderInvariantFailure(const char* reason) {
        UNREACHABLE_MSG("Recursive shared-first mutex invariant failed: {}", reason);
    }

    bool IncrementReaderOwnerLocked(const std::thread::id owner) {
        for (std::size_t index = 0; index < inline_owner_count; ++index) {
            auto& state = inline_owners[index];
            if (state.thread != owner) {
                continue;
            }
            if (state.recursion <= 0 ||
                state.recursion == std::numeric_limits<int>::max()) [[unlikely]] {
                ReaderInvariantFailure("invalid inline reader recursion");
            }
            ++state.recursion;
            return true;
        }
        if (!overflow_owners.empty()) {
            const auto it = overflow_owners.find(owner);
            if (it != overflow_owners.end()) {
                if (it->second <= 0 ||
                    it->second == std::numeric_limits<int>::max()) [[unlikely]] {
                    ReaderInvariantFailure("invalid overflow reader recursion");
                }
                ++it->second;
                return true;
            }
        }
        return false;
    }

    void AddReaderOwnerLocked(const std::thread::id owner) {
        if (readers == std::numeric_limits<int>::max()) [[unlikely]] {
            ReaderInvariantFailure("reader count overflow");
        }
        if (inline_owner_count < inline_owners.size()) {
            inline_owners[inline_owner_count++] = ReaderOwner{owner, 1};
        } else {
            const auto [it, inserted] = overflow_owners.try_emplace(owner, 1);
            if (!inserted) [[unlikely]] {
                ReaderInvariantFailure("duplicate overflow reader owner");
            }
        }
        ++readers;
    }

    // Returns true only when the outermost read ownership was released.
    bool DecrementReaderOwnerLocked(const std::thread::id owner) {
        for (std::size_t index = 0; index < inline_owner_count; ++index) {
            auto& state = inline_owners[index];
            if (state.thread != owner) {
                continue;
            }
            if (state.recursion <= 0) [[unlikely]] {
                ReaderInvariantFailure("invalid inline reader release");
            }
            if (--state.recursion != 0) {
                return false;
            }
            const std::size_t last = --inline_owner_count;
            if (index != last) {
                state = inline_owners[last];
            }
            inline_owners[last] = {};
            return true;
        }
        if (!overflow_owners.empty()) {
            const auto it = overflow_owners.find(owner);
            if (it != overflow_owners.end()) {
                if (it->second <= 0) [[unlikely]] {
                    ReaderInvariantFailure("invalid overflow reader release");
                }
                if (--it->second != 0) {
                    return false;
                }
                overflow_owners.erase(it);
                return true;
            }
        }
        ReaderInvariantFailure("shared unlock without reader ownership");
    }

    std::mutex mtx;
    std::condition_variable cv;
    std::array<ReaderOwner, InlineReaderCapacity> inline_owners{};
    std::size_t inline_owner_count{};
    std::unordered_map<std::thread::id, int> overflow_owners;
    int readers{};
    bool writer_active{};
};

}
