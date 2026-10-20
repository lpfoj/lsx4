// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#include "executor/dynamic_translation/executable_range_registry.h"
#include "executor/fex_backend_abi_v3.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>
#include <vector>

namespace {

using namespace Lsx4::Translation;

static_assert(sizeof(ExecutableRange) == sizeof(Lsx4FexExecutableRangeV5));
static_assert(ExecutableRangeFlagExecutable == LSX4_FEX_RANGE_EXECUTABLE);
static_assert(ExecutableRangeFlagWritable == LSX4_FEX_RANGE_WRITABLE);
static_assert(ExecutableRangeFlagHleTrampoline ==
              LSX4_FEX_RANGE_HLE_TRAMPOLINE);
static_assert(ExecutableRangeFlagSelfModifying ==
              LSX4_FEX_RANGE_SELF_MODIFYING);
static_assert(ExecutableRangeFlagHleFramePool == LSX4_FEX_RANGE_HLE_FRAME_POOL);
static_assert(ExecutableRangeInvalidateTranslations ==
              LSX4_FEX_RANGE_INVALIDATE_TRANSLATIONS);
static_assert(ExecutableRangeRemoveRegistrations ==
              LSX4_FEX_RANGE_REMOVE_REGISTRATIONS);

[[noreturn]] void Fail(const char* check) {
    std::fprintf(stderr, "EXECUTABLE_RANGE_REGISTRY_SMOKE failed=%s\n", check);
    std::abort();
}

void Require(const bool condition, const char* check) {
    if (!condition) {
        Fail(check);
    }
}

ExecutableRange CodeRange(const std::uint64_t ordinal,
                          const bool writable = false) {
    const std::uint64_t address = UINT64_C(0x100000) + ordinal * 0x1000u;
    return {
        .address = address,
        .size = 0x1000,
        .range_cookie = UINT64_C(0x7000000000000000) + ordinal + 1u,
        .protection = writable ? 7u : 5u,
        .range_flags =
            ExecutableRangeFlagExecutable |
            (writable ? ExecutableRangeFlagWritable |
                            ExecutableRangeFlagSelfModifying
                      : 0u),
    };
}

struct InvalidationProbe {
    ExecutableRangeRegistry* registry{};
    ExecutableRangeLease lease{};
    std::atomic<std::uint64_t> calls{};
    std::atomic<std::uint64_t> spans{};
    std::atomic<bool> lease_was_stale_inside_callback{};
    std::atomic<bool> snapshot_was_unavailable_inside_callback{};
    std::atomic<bool> fail_next{};
};

bool RetireTranslations(void* raw_context, const ExecutableRange*,
                        const std::size_t span_count,
                        const std::uint64_t retiring_mapping_epoch) noexcept {
    auto& probe = *static_cast<InvalidationProbe*>(raw_context);
    probe.calls.fetch_add(1, std::memory_order_relaxed);
    probe.spans.fetch_add(span_count, std::memory_order_relaxed);
    const bool stale = probe.registry != nullptr && probe.lease &&
                       !probe.registry->LeaseIsCurrent(probe.lease) &&
                       (retiring_mapping_epoch & 1u) != 0;
    probe.lease_was_stale_inside_callback.store(stale,
                                                std::memory_order_relaxed);
    probe.snapshot_was_unavailable_inside_callback.store(
        probe.registry != nullptr &&
            probe.registry->SnapshotSignalSafe() == nullptr,
        std::memory_order_relaxed);
    return !probe.fail_next.exchange(false, std::memory_order_relaxed);
}

}

int main() {
    InvalidationProbe probe{};
    ExecutableRangeRegistry registry{{&probe, RetireTranslations}};
    probe.registry = &registry;

    std::uint64_t generation = 999;
    Require(registry.BulkRegister({}, 0, generation) ==
                ExecutableRangeStatus::InvalidState,
            "register-before-stage-rejected");
    Require(registry.StageInitial({}, 40) == ExecutableRangeStatus::Ok,
            "zero-range-stage");
    Require(registry.StageInitial({}, 41) == ExecutableRangeStatus::InvalidState,
            "stage-once");
    Require(registry.BulkRegister({}, 40, generation) ==
                ExecutableRangeStatus::Ok &&
                generation == 40,
            "zero-range-register-noop");
    Require(registry.Invalidate({}, 0, 40, generation) ==
                ExecutableRangeStatus::Ok &&
                generation == 40 && probe.calls.load() == 0,
            "zero-range-invalidate-noop");

    const ExecutableRange first = CodeRange(0, true);
    Require(registry.BulkRegister(std::span{&first, 1}, 40, generation) ==
                ExecutableRangeStatus::Ok &&
                generation == 41,
            "one-range-register");
    Require(registry.BulkRegister(std::span{&first, 1}, 41, generation) ==
                ExecutableRangeStatus::Ok &&
                generation == 41,
            "idempotent-register");
    Require(registry.BulkRegister({}, 40, generation) ==
                ExecutableRangeStatus::InvalidState,
            "stale-register-generation");

    probe.lease = registry.AcquireLeaseSignalSafe(
        first.address + 16, 32,
        ExecutableRangeFlagExecutable | ExecutableRangeFlagSelfModifying);
    Require(probe.lease && registry.LeaseIsCurrent(probe.lease),
            "range-lease");
    Require(!registry.AcquireLeaseSignalSafe(first.address + first.size - 8, 16),
            "lease-cannot-cross-range");
    Require(!registry.AcquireLeaseSignalSafe(
                first.address, 1, ExecutableRangeFlagHleTrampoline),
            "lease-required-flags");
    Require(!registry.AcquireLeaseSignalSafe(
                std::numeric_limits<std::uint64_t>::max() - 3u, 8),
            "lease-overflow-rejected");

    Require(registry.Invalidate(std::span{&first, 1}, 0, 41, generation) ==
                ExecutableRangeStatus::Ok &&
                generation == 42 && probe.calls.load() == 1 &&
                probe.spans.load() == 1 &&
                probe.lease_was_stale_inside_callback.load() &&
                probe.snapshot_was_unavailable_inside_callback.load() &&
                !registry.LeaseIsCurrent(probe.lease),
            "smc-retirement-stales-lease-before-callback");
    Require(static_cast<bool>(registry.AcquireLeaseSignalSafe(first.address, 1)),
            "invalidate-keeps-registration");

    probe.lease = registry.AcquireLeaseSignalSafe(first.address, 1);
    probe.fail_next.store(true);
    Require(registry.Invalidate(std::span{&first, 1}, 0, 42, generation) ==
                ExecutableRangeStatus::BackendFailure &&
                generation == 42 && !registry.LeaseIsCurrent(probe.lease),
            "failed-retirement-fails-closed");
    Require(static_cast<bool>(registry.AcquireLeaseSignalSafe(first.address, 1)),
            "failed-retirement-keeps-registration");

    probe.lease = {};
    Require(registry.Invalidate(std::span{&first, 1},
                                ExecutableRangeRemoveRegistrations, 42,
                                generation) == ExecutableRangeStatus::Ok &&
                generation == 43 &&
                !registry.AcquireLeaseSignalSafe(first.address, 1),
            "remove-registration-after-retirement");
    Require(registry.Invalidate(std::span{&first, 1},
                                ExecutableRangeRemoveRegistrations, 43,
                                generation) == ExecutableRangeStatus::NotFound,
            "duplicate-removal-rejected-before-callback");

    std::vector<ExecutableRange> ranges;
    ranges.reserve(1000);
    for (std::uint64_t ordinal = 0; ordinal != 1000; ++ordinal) {
        ranges.push_back(CodeRange(ordinal + 8));
    }
    Require(registry.BulkRegister(std::span{ranges}.first(64), 43, generation) ==
                ExecutableRangeStatus::Ok &&
                generation == 44,
            "sixty-four-ranges");
    std::size_t required = 0;
    std::uint64_t copied_generation = 0;
    Require(registry.CopySnapshot({}, required, copied_generation) ==
                ExecutableRangeStatus::BufferTooSmall &&
                required == 64 && copied_generation == 44,
            "two-call-size-query-64");
    Require(registry.BulkRegister(std::span{ranges}.subspan(64, 1), 44,
                                  generation) == ExecutableRangeStatus::Ok &&
                generation == 45,
            "sixty-five-ranges");
    Require(registry.BulkRegister(std::span{ranges}.subspan(65), 45,
                                  generation) == ExecutableRangeStatus::Ok &&
                generation == 46,
            "one-thousand-ranges");

    std::vector<ExecutableRange> copied(1000);
    Require(registry.CopySnapshot(copied, required, copied_generation) ==
                ExecutableRangeStatus::Ok &&
                required == 1000 && copied_generation == 46 &&
                copied.front() == ranges.front() && copied.back() == ranges.back(),
            "copy-one-thousand-sorted-ranges");

    std::atomic<bool> stop_reader{};
    std::atomic<bool> reader_started{};
    std::atomic<std::uint64_t> reader_checks{};
    std::atomic<bool> reader_failed{};
    std::thread reader{[&] {
        reader_started.store(true, std::memory_order_release);
        while (!stop_reader.load(std::memory_order_acquire)) {
            const ExecutableRangeLease lease = registry.AcquireLeaseSignalSafe(
                ranges[500].address + 32, 16, ExecutableRangeFlagExecutable);
            if (lease) {
                if (lease.range->range_cookie != ranges[500].range_cookie ||
                    lease.range->address > ranges[500].address + 32 ||
                    lease.range->address + lease.range->size <
                        ranges[500].address + 48) {
                    reader_failed.store(true, std::memory_order_relaxed);
                    break;
                }
                reader_checks.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }};
    while (!reader_started.load(std::memory_order_acquire) ||
           reader_checks.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }
    for (unsigned iteration = 0; iteration != 128; ++iteration) {
        const ExecutableRange& span = ranges[iteration % ranges.size()];
        Require(registry.Invalidate(std::span{&span, 1}, 0, generation,
                                    generation) == ExecutableRangeStatus::Ok,
                "concurrent-reader-invalidation");
    }
    stop_reader.store(true, std::memory_order_release);
    reader.join();
    Require(!reader_failed.load() && reader_checks.load() != 0,
            "concurrent-signal-reader");
    const std::uint64_t final_generation = generation;

    ExecutableRange overlapping = ranges[0];
    overlapping.range_cookie ^= 0x10000u;
    Require(registry.BulkRegister(std::span{&overlapping, 1}, generation,
                                  generation) ==
                ExecutableRangeStatus::InvalidArgument,
            "overlap-rejected");
    ExecutableRange malformed = CodeRange(2000);
    malformed.address = std::numeric_limits<std::uint64_t>::max() - 4u;
    malformed.size = 8;
    Require(registry.BulkRegister(std::span{&malformed, 1}, generation,
                                  generation) ==
                ExecutableRangeStatus::InvalidArgument,
            "overflow-range-rejected");

    std::fprintf(stdout,
                 "EXECUTABLE_RANGE_REGISTRY_SMOKE ok ranges=%zu generation=%llu "
                 "invalidations=%llu readerChecks=%llu\n",
                 required, static_cast<unsigned long long>(final_generation),
                 static_cast<unsigned long long>(probe.calls.load()),
                 static_cast<unsigned long long>(reader_checks.load()));
    return 0;
}
