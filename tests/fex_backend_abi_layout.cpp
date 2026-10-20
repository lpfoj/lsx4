// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#include "executor/fex_backend_abi_v3.h"

#include <cstdint>

static_assert(LSX4_FEX_GUEST_MEMORY_ACCESS_READ == (UINT32_C(1) << 0));
static_assert(LSX4_FEX_GUEST_MEMORY_ACCESS_WRITE == (UINT32_C(1) << 1));
static_assert(LSX4_FEX_GUEST_MEMORY_ACCESS_NO_HOST_REENTRY ==
              (UINT32_C(1) << 2));
static_assert((LSX4_FEX_GUEST_MEMORY_ACCESS_NO_HOST_REENTRY &
               (LSX4_FEX_GUEST_MEMORY_ACCESS_READ |
                LSX4_FEX_GUEST_MEMORY_ACCESS_WRITE)) == 0);
static_assert(LSX4_FEX_CONFIG_RELAX_VECTOR_TSO_AB == (UINT32_C(1) << 0));
static_assert(LSX4_FEX_CONFIG_RELAX_MEMCPY_SET_TSO_AB ==
              (UINT32_C(1) << 1));
static_assert(LSX4_FEX_CONFIG_EXACT_MULTIBLOCK_AB == (UINT32_C(1) << 2));
static_assert(LSX4_FEX_CONFIG_MAXINST_512_AB == (UINT32_C(1) << 3));
static_assert(LSX4_FEX_CONFIG_MAXINST_256_AB == (UINT32_C(1) << 4));
static_assert(LSX4_FEX_CONFIG_PRESERVE_TERMINAL_THUNK_DYNAMIC_REGS_AB ==
              (UINT32_C(1) << 6));
  static_assert(LSX4_FEX_CONFIG_PRESERVE_TYPED_TIME_COMMON_FRAME_AB ==
                (UINT32_C(1) << 7));
static_assert(LSX4_FEX_CONFIG_PRESERVE_ZERO_DEFERRED_PROMOTION_RMW_AB ==
              (UINT32_C(1) << 8));
  static_assert(LSX4_FEX_HOST_ISA_FCMA == (UINT64_C(1) << 14));
static_assert(LSX4_FEX_HOST_ISA_FRINTTS == (UINT64_C(1) << 15));
static_assert(LSX4_FEX_HOST_ISA_ECV == (UINT64_C(1) << 16));
static_assert(LSX4_FEX_HOST_ISA_RPRES == (UINT64_C(1) << 17));
static_assert(LSX4_FEX_SIGNAL_CONTEXT_ARG1 == (UINT32_C(1) << 2));
static_assert((LSX4_FEX_SIGNAL_CONTEXT_ARG1 &
               (LSX4_FEX_SIGNAL_USE_EXPLICIT_CONTEXT |
                LSX4_FEX_SIGNAL_SYNCHRONOUS)) == 0);
static_assert((LSX4_FEX_CONFIG_RELAX_VECTOR_TSO_AB &
               LSX4_FEX_CONFIG_RELAX_MEMCPY_SET_TSO_AB) == 0);
static_assert((LSX4_FEX_CONFIG_EXACT_MULTIBLOCK_AB &
               (LSX4_FEX_CONFIG_RELAX_VECTOR_TSO_AB |
                LSX4_FEX_CONFIG_RELAX_MEMCPY_SET_TSO_AB)) == 0);
static_assert((LSX4_FEX_CONFIG_MAXINST_512_AB &
               (LSX4_FEX_CONFIG_RELAX_VECTOR_TSO_AB |
                LSX4_FEX_CONFIG_RELAX_MEMCPY_SET_TSO_AB |
                LSX4_FEX_CONFIG_EXACT_MULTIBLOCK_AB)) == 0);
static_assert((LSX4_FEX_CONFIG_MAXINST_256_AB &
               (LSX4_FEX_CONFIG_RELAX_VECTOR_TSO_AB |
                LSX4_FEX_CONFIG_RELAX_MEMCPY_SET_TSO_AB |
                LSX4_FEX_CONFIG_EXACT_MULTIBLOCK_AB |
                LSX4_FEX_CONFIG_MAXINST_512_AB)) == 0);
static_assert(LSX4_FEX_CAP_MEMORY_MODEL_AB_OVERRIDES ==
              (UINT64_C(1) << 12));

int main() {
    Lsx4FexHostCallbacksV3 callbacks{};
    callbacks.struct_size = sizeof(callbacks);
    callbacks.abi_version = LSX4_FEX_BACKEND_ABI_V3;

    Lsx4FexBackendApiV3 api{};
    api.struct_size = sizeof(api);
    api.abi_version = LSX4_FEX_BACKEND_ABI_V3;
    api.lifetime_flags = LSX4_FEX_LIFETIME_PROCESS_RESIDENT |
                         LSX4_FEX_LIFETIME_ACTIVATE_ONCE |
                         LSX4_FEX_LIFETIME_NO_DEACTIVATE |
                         LSX4_FEX_LIFETIME_SINGLE_SIGNAL_OWNER;

    Lsx4FexHleCallFrameV3 call{};
    call.struct_size = sizeof(call);
    call.abi_version = LSX4_FEX_BACKEND_ABI_V3;
    call.return_class[0] = LSX4_FEX_SYSV_INTEGER;

    Lsx4FexExecutableRangeSnapshotV5 ranges{};
    ranges.struct_size = sizeof(ranges);
    ranges.abi_version = LSX4_FEX_EXECUTABLE_RANGE_ABI_V5;

    Lsx4FexGuestCpuProfileV6 profile{};
    profile.struct_size = sizeof(profile);
    profile.abi_version = LSX4_FEX_GUEST_CPU_PROFILE_ABI_V6;

    Lsx4FexQueryResultV3 query{};
    query.struct_size = sizeof(query);
    query.abi_version = LSX4_FEX_BACKEND_ABI_V3;

    Lsx4FexConfigureRequestV3 configure{};
    configure.configure_flags = LSX4_FEX_CONFIG_RELAX_VECTOR_TSO_AB;

    return callbacks.struct_size == 88 && api.struct_size == 200 &&
                   sizeof(configure) == 120 &&
                   configure.configure_flags == UINT32_C(1) &&
                   call.struct_size == 256 &&
                   ranges.struct_size == 40 && profile.struct_size == 80 &&
                   query.struct_size == 168
               ? 0
               : 1;
}
