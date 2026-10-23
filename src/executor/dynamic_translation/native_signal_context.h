// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#if (defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)) && defined(__aarch64__)
#include <cstdint>
#if defined(LSX4_APPLE_RUNTIME)
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
namespace Lsx4::Translation {
inline std::uint64_t SignalContextGpr(const ucontext_t* context,unsigned index) noexcept {
#if defined(LSX4_APPLE_RUNTIME)
    const auto& regs=context->uc_mcontext->__ss;
    return index<29?regs.__x[index]:(index==29?regs.__fp:regs.__lr);
#else
    return context->uc_mcontext.regs[index];
#endif
}
inline std::uintptr_t SignalContextPc(const ucontext_t* context) noexcept {
#if defined(LSX4_APPLE_RUNTIME)
    return context->uc_mcontext->__ss.__pc;
#else
    return context->uc_mcontext.pc;
#endif
}
inline void SetSignalContextReturn(ucontext_t* context,std::uintptr_t sp,std::uintptr_t pc) noexcept {
#if defined(LSX4_APPLE_RUNTIME)
    context->uc_mcontext->__ss.__sp=sp;context->uc_mcontext->__ss.__pc=pc;
#else
    context->uc_mcontext.sp=sp;context->uc_mcontext.pc=pc;
#endif
}
}
#endif
