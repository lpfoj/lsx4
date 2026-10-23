// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace lsx4::fex {

template <size_t SlotCount = 2048>
class CompileSingleflight final {
  static_assert(SlotCount != 0 && (SlotCount & (SlotCount - 1)) == 0,
                "SlotCount must be a power of two");

public:
  static constexpr uint64_t kDefaultWaitBudgetNs = 1'000'000;
  static constexpr uint64_t kDefaultWatchdogNs = 100'000'000;

  enum class Kind : uint8_t {
    Owner,
    Follower,
    CollisionBypass,
    InitializingBypass,
    WatchdogBypass,
  };

  struct Stats {
    uint64_t owner_claims {};
    uint64_t owner_completions {};
    uint64_t owner_release_misses {};
    uint64_t followers {};
    uint64_t follower_wait_successes {};
    uint64_t follower_wait_timeouts {};
    uint64_t follower_wait_ns {};
    uint64_t follower_wait_max_ns {};
    uint64_t collisions {};
    uint64_t initializing_bypasses {};
    uint64_t watchdog_bypasses {};
    uint64_t cache_misses_after_wake {};
    uint64_t compile_failures {};
  };

private:
  static constexpr uint64_t kStateMask = 3;
  static constexpr uint64_t kInitializing = 1;
  static constexpr uint64_t kActive = 2;

  struct Slot {
    std::atomic<uint64_t> state {};
    std::atomic<uint64_t> guest_rip {};
    std::atomic<uintptr_t> cache_identity {};
    std::atomic<uint64_t> started_ns {};
  };

  struct Counters {
    std::atomic<uint64_t> owner_claims {};
    std::atomic<uint64_t> owner_completions {};
    std::atomic<uint64_t> owner_release_misses {};
    std::atomic<uint64_t> followers {};
    std::atomic<uint64_t> follower_wait_successes {};
    std::atomic<uint64_t> follower_wait_timeouts {};
    std::atomic<uint64_t> follower_wait_ns {};
    std::atomic<uint64_t> follower_wait_max_ns {};
    std::atomic<uint64_t> collisions {};
    std::atomic<uint64_t> initializing_bypasses {};
    std::atomic<uint64_t> watchdog_bypasses {};
    std::atomic<uint64_t> cache_misses_after_wake {};
    std::atomic<uint64_t> compile_failures {};
  };

public:
  class Turn final {
  public:
    Turn() = default;
    Turn(const Turn&) = delete;
    Turn& operator=(const Turn&) = delete;

    Turn(Turn&& other) noexcept
      : table_ {other.table_}
      , slot_ {other.slot_}
      , token_ {other.token_}
      , kind_ {other.kind_} {
      other.table_ = nullptr;
      other.slot_ = nullptr;
      other.token_ = 0;
    }

    Turn& operator=(Turn&& other) noexcept {
      if (this != &other) {
        Complete();
        table_ = other.table_;
        slot_ = other.slot_;
        token_ = other.token_;
        kind_ = other.kind_;
        other.table_ = nullptr;
        other.slot_ = nullptr;
        other.token_ = 0;
      }
      return *this;
    }

    ~Turn() {
      Complete();
    }

    [[nodiscard]] Kind GetKind() const noexcept {
      return kind_;
    }
    [[nodiscard]] bool IsOwner() const noexcept {
      return kind_ == Kind::Owner;
    }
    [[nodiscard]] bool IsFollower() const noexcept {
      return kind_ == Kind::Follower;
    }
    [[nodiscard]] bool ShouldBypass() const noexcept {
      return kind_ != Kind::Owner && kind_ != Kind::Follower;
    }

    [[nodiscard]] bool WaitFor(uint64_t budget_ns = kDefaultWaitBudgetNs) const noexcept {
      if (!IsFollower() || table_ == nullptr || slot_ == nullptr) {
        return false;
      }

      const uint64_t begin = NowNs();
      while (slot_->state.load(std::memory_order_acquire) == token_) {
        const uint64_t now = NowNs();
        if (now - begin >= budget_ns) {
          table_->counters_.follower_wait_timeouts.fetch_add(1, std::memory_order_relaxed);
          table_->RecordWaitDuration(now - begin);
          return false;
        }
        CpuRelax();
      }

      const uint64_t elapsed = NowNs() - begin;
      table_->counters_.follower_wait_successes.fetch_add(1, std::memory_order_relaxed);
      table_->RecordWaitDuration(elapsed);
      return true;
    }

    void Complete() noexcept {
      if (!IsOwner() || table_ == nullptr || slot_ == nullptr) {
        return;
      }
      uint64_t expected = token_;
      if (slot_->state.compare_exchange_strong(expected, 0, std::memory_order_release, std::memory_order_relaxed)) {
        table_->counters_.owner_completions.fetch_add(1, std::memory_order_relaxed);
      } else {
        table_->counters_.owner_release_misses.fetch_add(1, std::memory_order_relaxed);
      }
      table_ = nullptr;
      slot_ = nullptr;
      token_ = 0;
    }

  private:
    friend class CompileSingleflight;

    Turn(CompileSingleflight* table, Slot* slot, uint64_t token, Kind kind) noexcept
      : table_ {table}
      , slot_ {slot}
      , token_ {token}
      , kind_ {kind} {}

    CompileSingleflight* table_ {};
    Slot* slot_ {};
    uint64_t token_ {};
    Kind kind_ {Kind::CollisionBypass};
  };

  [[nodiscard]] Turn Acquire(uintptr_t cache_identity, uint64_t guest_rip, uint64_t now_ns = NowNs(),
                             uint64_t watchdog_ns = kDefaultWatchdogNs) noexcept {
    Slot& slot = slots_[Index(cache_identity, guest_rip)];

    for (uint32_t attempt = 0; attempt != 4; ++attempt) {
      uint64_t state = slot.state.load(std::memory_order_acquire);
      if (state == 0) {
        uint64_t ticket = next_ticket_.fetch_add(1, std::memory_order_relaxed);
        if (ticket == 0) {
          ticket = next_ticket_.fetch_add(1, std::memory_order_relaxed);
        }
        const uint64_t base = ticket << 2;
        const uint64_t initializing = base | kInitializing;
        const uint64_t active = base | kActive;
        if (!slot.state.compare_exchange_weak(state, initializing, std::memory_order_acq_rel, std::memory_order_acquire)) {
          continue;
        }

        slot.guest_rip.store(guest_rip, std::memory_order_relaxed);
        slot.cache_identity.store(cache_identity, std::memory_order_relaxed);
        slot.started_ns.store(now_ns, std::memory_order_relaxed);
        slot.state.store(active, std::memory_order_release);
        counters_.owner_claims.fetch_add(1, std::memory_order_relaxed);
        return Turn {this, &slot, active, Kind::Owner};
      }

      if ((state & kStateMask) == kInitializing) {
        CpuRelax();
        continue;
      }
      if ((state & kStateMask) != kActive) {
        break;
      }

      const uint64_t owner_rip = slot.guest_rip.load(std::memory_order_relaxed);
      const uintptr_t owner_identity = slot.cache_identity.load(std::memory_order_relaxed);
      const uint64_t started_ns = slot.started_ns.load(std::memory_order_relaxed);
      if (slot.state.load(std::memory_order_acquire) != state) {
        continue;
      }

      if (owner_rip != guest_rip || owner_identity != cache_identity) {
        counters_.collisions.fetch_add(1, std::memory_order_relaxed);
        return Turn {this, nullptr, 0, Kind::CollisionBypass};
      }
      if (started_ns != 0 && now_ns > started_ns && now_ns - started_ns >= watchdog_ns) {
        counters_.watchdog_bypasses.fetch_add(1, std::memory_order_relaxed);
        return Turn {this, nullptr, 0, Kind::WatchdogBypass};
      }

      counters_.followers.fetch_add(1, std::memory_order_relaxed);
      return Turn {this, &slot, state, Kind::Follower};
    }

    counters_.initializing_bypasses.fetch_add(1, std::memory_order_relaxed);
    return Turn {this, nullptr, 0, Kind::InitializingBypass};
  }

  void RecordCacheMissAfterWake() noexcept {
    counters_.cache_misses_after_wake.fetch_add(1, std::memory_order_relaxed);
  }

  void RecordCompileFailure() noexcept {
    counters_.compile_failures.fetch_add(1, std::memory_order_relaxed);
  }

  [[nodiscard]] Stats Snapshot() const noexcept {
    return {
      .owner_claims = Load(counters_.owner_claims),
      .owner_completions = Load(counters_.owner_completions),
      .owner_release_misses = Load(counters_.owner_release_misses),
      .followers = Load(counters_.followers),
      .follower_wait_successes = Load(counters_.follower_wait_successes),
      .follower_wait_timeouts = Load(counters_.follower_wait_timeouts),
      .follower_wait_ns = Load(counters_.follower_wait_ns),
      .follower_wait_max_ns = Load(counters_.follower_wait_max_ns),
      .collisions = Load(counters_.collisions),
      .initializing_bypasses = Load(counters_.initializing_bypasses),
      .watchdog_bypasses = Load(counters_.watchdog_bypasses),
      .cache_misses_after_wake = Load(counters_.cache_misses_after_wake),
      .compile_failures = Load(counters_.compile_failures),
    };
  }

  [[nodiscard]] static constexpr size_t SlotIndex(uintptr_t cache_identity, uint64_t guest_rip) noexcept {
    return Index(cache_identity, guest_rip);
  }

private:
  [[nodiscard]] static uint64_t NowNs() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  }

  static void CpuRelax() noexcept {
#if defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
  }

  [[nodiscard]] static constexpr uint64_t Mix(uint64_t value) noexcept {
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
  }

  [[nodiscard]] static constexpr size_t Index(uintptr_t cache_identity, uint64_t guest_rip) noexcept {
    const uint64_t identity = static_cast<uint64_t>(cache_identity);
    return static_cast<size_t>(Mix(guest_rip ^ (identity + UINT64_C(0x9e3779b97f4a7c15))) & (SlotCount - 1));
  }

  static uint64_t Load(const std::atomic<uint64_t>& value) noexcept {
    return value.load(std::memory_order_relaxed);
  }

  void RecordWaitDuration(uint64_t duration_ns) noexcept {
    counters_.follower_wait_ns.fetch_add(duration_ns, std::memory_order_relaxed);
    uint64_t previous = counters_.follower_wait_max_ns.load(std::memory_order_relaxed);
    while (previous < duration_ns && !counters_.follower_wait_max_ns.compare_exchange_weak(
                                       previous, duration_ns, std::memory_order_relaxed, std::memory_order_relaxed)) {}
  }

  std::array<Slot, SlotCount> slots_ {};
  std::atomic<uint64_t> next_ticket_ {1};
  Counters counters_ {};
};

} // namespace lsx4::fex
