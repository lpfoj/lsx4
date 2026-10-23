// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "common/va_ctx.h"
#include "core/aerolib/hle_abi.h"

namespace Core::AeroLib {

inline bool BuildAndroidX64VaContext(const AndroidX64HleRawCall& call,
                                     const std::size_t fixed_gpr_arguments,
                                     const std::size_t fixed_sse_arguments,
                                     const std::size_t fixed_stack_bytes,
                                     Common::VaCtx* context) {
    if (context == nullptr || call.guest_rsp == 0 || ExecutorJitReadGuestBytes == nullptr ||
        (call.call_flags & AndroidX64CallVariadic) == 0 ||
        call.sysv_sse_arg_count > call.xmm.size() ||
        fixed_sse_arguments > call.sysv_sse_arg_count ||
        (fixed_stack_bytes & (sizeof(u64) - 1)) != 0) {
        return false;
    }
    *context = {};
    for (std::size_t index = 0; index < call.gpr.size(); ++index) {
        context->reg_save_area.gp[index] = call.gpr[index];
    }
    for (std::size_t index = 0; index < call.xmm.size(); ++index) {
        static_assert(sizeof(Common::VaFpReg) == sizeof(AndroidX64HleXmm128));
        std::memcpy(&context->reg_save_area.fp[index], &call.xmm[index],
                    sizeof(Common::VaFpReg));
    }
    u64 stack_base{};
    u64 overflow_address{};
    if (!HleAbiDetail::CheckedAdd(call.guest_rsp, sizeof(u64), &stack_base) ||
        !HleAbiDetail::CheckedAdd(stack_base, fixed_stack_bytes, &overflow_address)) {
        return false;
    }
    context->va_list.gp_offset = static_cast<u32>(
        std::min(fixed_gpr_arguments, call.gpr.size()) * sizeof(u64));
    context->va_list.fp_offset = static_cast<u32>(
        offsetof(Common::VaRegSave, fp) +
        std::min(fixed_sse_arguments, call.xmm.size()) * sizeof(Common::VaFpReg));
    context->va_list.overflow_arg_area = reinterpret_cast<void*>(overflow_address);
    context->va_list.reg_save_area = &context->reg_save_area;
    context->va_list.overflow_reader = ExecutorJitReadGuestBytes;
    return true;
}

} // namespace Core::AeroLib

#endif // __ANDROID__
