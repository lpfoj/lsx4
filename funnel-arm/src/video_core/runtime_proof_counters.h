// SPDX-FileCopyrightText: Copyright 2026 LSX4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

#define LSX4_FUNNEL_RUNTIME_PROOF_COUNTERS_ABI_V1 UINT32_C(1)
#define LSX4_FUNNEL_RUNTIME_PROOF_COUNTERS_QUERY_V1_SYMBOL \
    "executor_funnel_runtime_proof_counters_query_v1"

// A zero value is evidence only while the corresponding bit is present in
// valid_mask. requested_mask is exhaustive for ABI v1; unavailable_mask is
// explicit and must never be interpreted as a measured zero.
enum Lsx4FunnelRuntimeProofCounterBitsV1 : std::uint64_t {
    LSX4_FUNNEL_RUNTIME_PROOF_GUEST_INVALIDATION_REQUESTS = UINT64_C(1) << 0,
    LSX4_FUNNEL_RUNTIME_PROOF_GUEST_INVALIDATION_COMPLETIONS = UINT64_C(1) << 1,
    LSX4_FUNNEL_RUNTIME_PROOF_GUEST_INVALIDATION_WAIT_NS = UINT64_C(1) << 2,
    LSX4_FUNNEL_RUNTIME_PROOF_OWNED_GENERATED_WRITE_FAULTS = UINT64_C(1) << 3,
    // Funnel has no exact fastmem-miss observer. This bit is deliberately
    // requested and unavailable in ABI v1 so callers cannot substitute a
    // page-fault count, invalidation count, or zero as a proxy.
    LSX4_FUNNEL_RUNTIME_PROOF_FASTMEM_MISSES = UINT64_C(1) << 4,
};

enum Lsx4FunnelRuntimeProofCounterModeV1 : std::uint32_t {
    LSX4_FUNNEL_RUNTIME_PROOF_COUNTERS_DISABLED = 0,
    LSX4_FUNNEL_RUNTIME_PROOF_COUNTERS_DYNAMIC_DIAGNOSTIC = 1,
};

enum Lsx4FunnelRuntimeProofCaptureFlagsV1 : std::uint32_t {
    // Atomic fields are read during one bounded query, but producers are not
    // stopped; the snapshot is not a transaction or a seqlock snapshot.
    LSX4_FUNNEL_RUNTIME_PROOF_CAPTURE_RELAXED_MULTI_FIELD = UINT32_C(1) << 0,
    // The profile marker adds steady-clock reads around invalidation waits.
    LSX4_FUNNEL_RUNTIME_PROOF_CAPTURE_WAIT_CLOCKS = UINT32_C(1) << 1,
    // Existing PageManager owned-fault telemetry is active. This flag is absent
    // (and the metric unavailable) under EXECUTOR_LIVE_QUIET; the proof path
    // does not add a marker-off fault-handler load merely to manufacture data.
    LSX4_FUNNEL_RUNTIME_PROOF_CAPTURE_WRITE_FAULT_COUNTER = UINT32_C(1) << 2,
};

enum Lsx4FunnelRuntimeProofStatusV1 : std::int32_t {
    LSX4_FUNNEL_RUNTIME_PROOF_STATUS_OK = 0,
    LSX4_FUNNEL_RUNTIME_PROOF_STATUS_INVALID_ARGUMENT = -22,
    LSX4_FUNNEL_RUNTIME_PROOF_STATUS_INVALID_STATE = -61,
};

typedef struct Lsx4FunnelRuntimeProofCountersV1 {
    std::uint32_t struct_size;
    std::uint32_t abi_version;
    std::int32_t status;
    std::uint32_t mode;
    std::uint32_t capture_flags;
    std::uint32_t saturated;
    std::uint64_t requested_mask;
    std::uint64_t valid_mask;
    std::uint64_t unavailable_mask;
    std::uint64_t exact_mask;
    std::uint64_t sampled_mask;
    std::uint64_t static_mask;
    std::uint64_t dynamic_mask;
    // Every valid v1 value describes a workload carrying diagnostic overhead.
    std::uint64_t perturbed_mask;
    // Request and fault-entry counters observe admission/entry, not successful
    // completion or instruction retirement. The separate completion counter
    // is the only invalidation completion measure.
    std::uint64_t attempted_not_retired_mask;
    // Nonzero process-local generation, reset with the renderer-side counters.
    std::uint64_t epoch;
    // Monotonic query ordinal within epoch; it is not a mutation seqlock.
    std::uint64_t sequence;
    // Outer guest-thread invalidation calls admitted after validator success;
    // nested TLS invalidations and pre-admission validator rejects are excluded.
    std::uint64_t guest_invalidation_requests;
    // Counted outer calls which left their wait loop, regardless of the final
    // boolean result. It is paired with requests, not a successful-work count.
    std::uint64_t guest_invalidation_completions;
    // Sum of steady-clock time from admission through completion for those calls.
    std::uint64_t guest_invalidation_wait_ns;
    // Owned write faults whose host PC classifier identified JIT/FEX generated
    // code. Valid only when capture flag WRITE_FAULT_COUNTER is present.
    std::uint64_t owned_generated_code_write_faults;
    // Always unavailable in ABI v1. Its storage remains zero by contract.
    std::uint64_t fastmem_misses;
    std::uint64_t reserved[13];
} Lsx4FunnelRuntimeProofCountersV1;

typedef std::int32_t (*Lsx4FunnelRuntimeProofCountersQueryFnV1)(
    Lsx4FunnelRuntimeProofCountersV1* counters);

#if defined(__GNUC__)
#define LSX4_FUNNEL_RUNTIME_PROOF_EXPORT __attribute__((visibility("default")))
#else
#define LSX4_FUNNEL_RUNTIME_PROOF_EXPORT
#endif

extern "C" LSX4_FUNNEL_RUNTIME_PROOF_EXPORT std::int32_t
executor_funnel_runtime_proof_counters_query_v1(
    Lsx4FunnelRuntimeProofCountersV1* counters) noexcept;

#undef LSX4_FUNNEL_RUNTIME_PROOF_EXPORT

static_assert(sizeof(Lsx4FunnelRuntimeProofCountersV1) == 256);
static_assert(offsetof(Lsx4FunnelRuntimeProofCountersV1,
                       guest_invalidation_requests) == 112);
