// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#include "executor/fex_backend_abi_v3.h"

int lsx4_fex_backend_abi_c_compile_test(void) {
    Lsx4FexHleResultFrameV3 result = {0};
    result.struct_size = (uint32_t)sizeof(result);
    result.abi_version = LSX4_FEX_BACKEND_ABI_V3;
    result.return_class[0] = LSX4_FEX_SYSV_SSE;
    Lsx4FexConfigureRequestV3 request = {0};
    Lsx4FexGuestCpuProfileV6 profile = {0};
    request.struct_size = (uint32_t)sizeof(request);
    request.configure_flags = LSX4_FEX_CONFIG_RELAX_MEMCPY_SET_TSO_AB;
    request.guest_cpu_profile_v6 = &profile;
    profile.struct_size = (uint32_t)sizeof(profile);
    profile.abi_version = LSX4_FEX_GUEST_CPU_PROFILE_ABI_V6;
    return result.struct_size == 96 && request.struct_size == 120 &&
                   request.configure_flags == (UINT32_C(1) << 1) &&
                   profile.struct_size == 80
               ? 0
               : 1;
}
