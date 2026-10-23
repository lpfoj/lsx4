//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstring>
#include <limits>

#include "common/arch.h"
#include "common/types.h"

#if defined(ARCH_X86_64)
#include <xmmintrin.h>
#endif

namespace Common {

#if defined(ARCH_X86_64)
using VaFpReg = __m128;
#else
using VaFpReg = u128;
#endif

}

#define VA_ARGS                                                                                    \
    uint64_t rdi, uint64_t rsi, uint64_t rdx, uint64_t rcx, uint64_t r8, uint64_t r9,              \
        uint64_t overflow_arg_area, ::Common::VaFpReg xmm0, ::Common::VaFpReg xmm1,                \
        ::Common::VaFpReg xmm2, ::Common::VaFpReg xmm3, ::Common::VaFpReg xmm4,                   \
        ::Common::VaFpReg xmm5, ::Common::VaFpReg xmm6, ::Common::VaFpReg xmm7

#define VA_CTX(ctx)                                                                                \
    alignas(16)::Common::VaCtx ctx{};                                                              \
    (ctx).reg_save_area.gp[0] = rdi;                                                               \
    (ctx).reg_save_area.gp[1] = rsi;                                                               \
    (ctx).reg_save_area.gp[2] = rdx;                                                               \
    (ctx).reg_save_area.gp[3] = rcx;                                                               \
    (ctx).reg_save_area.gp[4] = r8;                                                                \
    (ctx).reg_save_area.gp[5] = r9;                                                                \
    (ctx).reg_save_area.fp[0] = xmm0;                                                              \
    (ctx).reg_save_area.fp[1] = xmm1;                                                              \
    (ctx).reg_save_area.fp[2] = xmm2;                                                              \
    (ctx).reg_save_area.fp[3] = xmm3;                                                              \
    (ctx).reg_save_area.fp[4] = xmm4;                                                              \
    (ctx).reg_save_area.fp[5] = xmm5;                                                              \
    (ctx).reg_save_area.fp[6] = xmm6;                                                              \
    (ctx).reg_save_area.fp[7] = xmm7;                                                              \
    (ctx).va_list.reg_save_area = &(ctx).reg_save_area;                                            \
    (ctx).va_list.gp_offset = offsetof(::Common::VaRegSave, gp);                                   \
    (ctx).va_list.fp_offset = offsetof(::Common::VaRegSave, fp);                                   \
    (ctx).va_list.overflow_arg_area = &overflow_arg_area;

namespace Common {

using VaOverflowReader = bool (*)(u64 address, void* destination, std::size_t size);

struct VaList {
    u32 gp_offset;
    u32 fp_offset;
    void* overflow_arg_area;
    void* reg_save_area;
    VaOverflowReader overflow_reader{};
    bool overflow_read_failed{};
};

struct alignas(16) VaRegSave {
    u64 gp[6];
    VaFpReg fp[8];
};

struct alignas(16) VaCtx {
    VaRegSave reg_save_area;
    VaList va_list;
};

template <class T, uint32_t Size>
T vaArgRegSaveAreaGp(VaList* l) {
    static_assert(sizeof(T) <= Size);
    T value{};
    std::memcpy(&value, static_cast<u8*>(l->reg_save_area) + l->gp_offset, sizeof(value));
    l->gp_offset += Size;
    return value;
}
template <class T, u64 Align, u64 Size>
T vaArgOverflowArgArea(VaList* l) {
    static_assert(Align != 0 && (Align & (Align - 1)) == 0);
    static_assert(Size >= sizeof(T));
    if (l->overflow_read_failed) {
        return {};
    }
    const u64 unaligned = reinterpret_cast<u64>(l->overflow_arg_area);
    if (unaligned > std::numeric_limits<u64>::max() - (Align - 1)) {
        l->overflow_read_failed = true;
        return {};
    }
    const u64 ptr = (unaligned + (Align - 1)) & ~(Align - 1);
    if (ptr > std::numeric_limits<u64>::max() - Size) {
        l->overflow_read_failed = true;
        return {};
    }
    l->overflow_arg_area = reinterpret_cast<void*>(ptr + Size);
    if (l->overflow_reader != nullptr) {
        T value{};
        if (!l->overflow_reader(ptr, &value, sizeof(value))) {
            l->overflow_read_failed = true;
            return {};
        }
        return value;
    }
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(ptr), sizeof(value));
    return value;
}

template <class T, uint32_t Size>
T vaArgRegSaveAreaFp(VaList* l) {
    static_assert(sizeof(T) <= Size);
    T value{};
    std::memcpy(&value, static_cast<u8*>(l->reg_save_area) + l->fp_offset, sizeof(value));
    l->fp_offset += Size;
    return value;
}

inline int vaArgInteger(VaList* l) {
    if (l->gp_offset <= 40) {
        return vaArgRegSaveAreaGp<int, 8>(l);
    }
    return vaArgOverflowArgArea<int, 1, 8>(l);
}

inline long long vaArgLongLong(VaList* l) {
    if (l->gp_offset <= 40) {
        return vaArgRegSaveAreaGp<long long, 8>(l);
    }
    return vaArgOverflowArgArea<long long, 1, 8>(l);
}
// The guest follows AMD64 SysV LP64 even when the host is Windows LLP64.
inline s64 vaArgLong(VaList* l) {
    if (l->gp_offset <= 40) {
        return vaArgRegSaveAreaGp<s64, 8>(l);
    }
    return vaArgOverflowArgArea<s64, 1, 8>(l);
}

inline u64 vaArgULong(VaList* l) {
    if (l->gp_offset <= 40) {
        return vaArgRegSaveAreaGp<u64, 8>(l);
    }
    return vaArgOverflowArgArea<u64, 1, 8>(l);
}

inline double vaArgDouble(VaList* l) {
    if (l->fp_offset <= 160) {
        return vaArgRegSaveAreaFp<double, 16>(l);
    }
    return vaArgOverflowArgArea<double, 1, 8>(l);
}

template <class T>
T* vaArgPtr(VaList* l) {
    if (l->gp_offset <= 40) {
        return vaArgRegSaveAreaGp<T*, 8>(l);
    }
    return vaArgOverflowArgArea<T*, 1, 8>(l);
}

}
