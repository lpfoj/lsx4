// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LSX4_FEX_PROOF_METRICS_ABI_V1 UINT32_C(1)
#define LSX4_FEX_BACKEND_PROOF_METRICS_QUERY_V1_SYMBOL \
    "lsx4_fex_backend_proof_metrics_query_v1"

enum Lsx4FexProofMetricsModeV1 {
    LSX4_FEX_PROOF_METRICS_DISABLED = 0,
    /* Compile/publication counters only; translated guest code is untouched. */
    LSX4_FEX_PROOF_METRICS_STATIC = 1,
    /* Also inject an exact IR-block-entry counter. */
    LSX4_FEX_PROOF_METRICS_DYNAMIC = 2,
};

/*
 * A metric bit identifies both its value field and all semantic masks.  A
 * numeric zero is evidence only while its bit is present in valid_mask.
 */
enum Lsx4FexProofMetricBitsV1 {
    LSX4_FEX_PROOF_EXECUTED_GUEST_INSTRUCTION_ENTRIES = UINT64_C(1) << 0,
    LSX4_FEX_PROOF_EXECUTED_IR_BLOCK_ENTRIES = UINT64_C(1) << 1,
    LSX4_FEX_PROOF_EXECUTED_DISPATCHER_ENTRIES = UINT64_C(1) << 2,
    LSX4_FEX_PROOF_COMPILED_GUEST_INSTRUCTION_SITES = UINT64_C(1) << 3,
    LSX4_FEX_PROOF_COMPILED_GUEST_BYTES = UINT64_C(1) << 4,
    LSX4_FEX_PROOF_PUBLISHED_NATIVE_BLOCK_BYTES = UINT64_C(1) << 5,
    LSX4_FEX_PROOF_COMPILED_SPILL_SLOTS = UINT64_C(1) << 6,
    LSX4_FEX_PROOF_COMPILED_SPILL_SITES = UINT64_C(1) << 7,
    LSX4_FEX_PROOF_COMPILED_FILL_SITES = UINT64_C(1) << 8,
    LSX4_FEX_PROOF_COMPILED_HELPER_SITES = UINT64_C(1) << 9,
    LSX4_FEX_PROOF_COMPILED_FLAG_SITES = UINT64_C(1) << 10,
    LSX4_FEX_PROOF_COMPILED_FENCE_SITES = UINT64_C(1) << 11,
    LSX4_FEX_PROOF_COMPILED_DMB_SITES = UINT64_C(1) << 12,
    LSX4_FEX_PROOF_TRANSLATION_UNITS = UINT64_C(1) << 13,
    LSX4_FEX_PROOF_COMPILED_IR_BLOCKS = UINT64_C(1) << 14,
    LSX4_FEX_PROOF_PUBLISHED_NATIVE_BLOCKS = UINT64_C(1) << 15,
    LSX4_FEX_PROOF_COMPILED_EXPLICIT_HELPER_SITES = UINT64_C(1) << 16,
    LSX4_FEX_PROOF_COMPILED_EXPLICIT_FENCE_DMB_SITES = UINT64_C(1) << 17,
};

enum Lsx4FexProofCaptureFlagsV1 {
    /* Fields are read in one bounded call, but without a global seqlock. */
    LSX4_FEX_PROOF_CAPTURE_RELAXED_MULTI_FIELD_SNAPSHOT = UINT32_C(1) << 0,
    /* Instrumented and uninstrumented persistent blocks cannot mix. */
    LSX4_FEX_PROOF_CAPTURE_CODE_CACHE_DISABLED = UINT32_C(1) << 1,
};

typedef struct Lsx4FexProofMetricsV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    int32_t status;
    uint32_t mode;
    uint32_t capture_flags;
    uint32_t reserved;
    uint64_t requested_mask;
    uint64_t valid_mask;
    uint64_t unavailable_mask;
    uint64_t exact_mask;
    uint64_t sampled_mask;
    uint64_t static_mask;
    uint64_t dynamic_mask;
    uint64_t perturbed_mask;
    /* Reserved for a future valid entry/attempt counter; not retirement. */
    uint64_t attempted_not_retired_mask;
    uint64_t epoch;
    uint64_t sequence;
    uint64_t executed_guest_instruction_entries;
    uint64_t executed_ir_block_entries;
    uint64_t executed_dispatcher_entries;
    uint64_t compiled_guest_instruction_sites;
    uint64_t compiled_guest_bytes;
    uint64_t published_native_block_bytes;
    uint64_t compiled_spill_slots;
    uint64_t compiled_spill_sites;
    uint64_t compiled_fill_sites;
    uint64_t compiled_helper_sites;
    uint64_t compiled_flag_sites;
    uint64_t compiled_fence_sites;
    uint64_t compiled_dmb_sites;
    uint64_t translation_units;
    uint64_t compiled_ir_blocks;
    uint64_t published_native_blocks;
    uint64_t compiled_explicit_helper_sites;
    uint64_t compiled_explicit_fence_dmb_sites;
} Lsx4FexProofMetricsV1;

typedef int32_t (*Lsx4FexBackendProofMetricsQueryFnV1)(
    Lsx4FexProofMetricsV1* metrics);

#ifdef __cplusplus
} // extern "C"

static_assert(offsetof(Lsx4FexProofMetricsV1, executed_guest_instruction_entries) == 112);
static_assert(sizeof(Lsx4FexProofMetricsV1) == 256);
#endif
