// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef LSX4_EXECUTOR_GUEST_CONTEXT_SNAPSHOT_ABI_H
#define LSX4_EXECUTOR_GUEST_CONTEXT_SNAPSHOT_ABI_H

#include <stddef.h>
#include <stdint.h>

#define LSX4_GUEST_CONTEXT_SNAPSHOT_MAGIC UINT32_C(0x4c475843)
#define LSX4_GUEST_CONTEXT_SNAPSHOT_VERSION UINT32_C(2)
#define LSX4_GUEST_CONTEXT_GPR_COUNT UINT32_C(16)
#define LSX4_GUEST_CONTEXT_XMM_COUNT UINT32_C(16)

enum {
    LSX4_GUEST_CONTEXT_GPR_VALID = UINT32_C(1) << 0,
    LSX4_GUEST_CONTEXT_XMM_VALID = UINT32_C(1) << 1,
    LSX4_GUEST_CONTEXT_FLAGS_VALID = UINT32_C(1) << 2,
    LSX4_GUEST_CONTEXT_TLS_VALID = UINT32_C(1) << 3,
};

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Lsx4GuestContextLegacyPrefix {
    uint64_t regs[LSX4_GUEST_CONTEXT_GPR_COUNT];
    uint64_t rip;
    uint64_t old_ip;
    uint64_t fsbase;
    uint64_t gsbase;
    int32_t quit;
    int32_t exit;
    int32_t error;
} Lsx4GuestContextLegacyPrefix;

typedef struct Lsx4GuestContextXmm128 {
    uint64_t low;
    uint64_t high;
} Lsx4GuestContextXmm128;








typedef struct Lsx4GuestContextSnapshotV2 {
    Lsx4GuestContextLegacyPrefix legacy;
    uint32_t struct_size;
    uint32_t magic;
    uint32_t version;
    uint32_t context_flags;
    uint64_t rflags;
    uint32_t mxcsr;
    uint32_t reserved;
    Lsx4GuestContextXmm128 xmm[LSX4_GUEST_CONTEXT_XMM_COUNT];
} Lsx4GuestContextSnapshotV2;

#ifdef __cplusplus
}

static_assert(sizeof(Lsx4GuestContextLegacyPrefix) == 176);
static_assert(alignof(Lsx4GuestContextLegacyPrefix) == 8);
static_assert(offsetof(Lsx4GuestContextSnapshotV2, struct_size) ==
              sizeof(Lsx4GuestContextLegacyPrefix));
static_assert(offsetof(Lsx4GuestContextSnapshotV2, magic) == 180);
static_assert(offsetof(Lsx4GuestContextSnapshotV2, version) == 184);
static_assert(offsetof(Lsx4GuestContextSnapshotV2, context_flags) == 188);
static_assert(offsetof(Lsx4GuestContextSnapshotV2, rflags) == 192);
static_assert(offsetof(Lsx4GuestContextSnapshotV2, mxcsr) == 200);
static_assert(offsetof(Lsx4GuestContextSnapshotV2, reserved) == 204);
static_assert(offsetof(Lsx4GuestContextSnapshotV2, xmm) == 208);
static_assert(sizeof(Lsx4GuestContextSnapshotV2) == 464);
static_assert(alignof(Lsx4GuestContextSnapshotV2) == 8);
#endif

#endif
