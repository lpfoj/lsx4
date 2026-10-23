// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "executor/dynamic_translation/runtime_gateway.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>

namespace ScalarI64BoundaryFixture {
// storage is a mapped guest-readable/executable range of at least 128 bytes.
template<class Check> void Run(void* storage, Check check) {
    using namespace Executor::Jit;
    auto* memory=static_cast<std::uint8_t*>(storage);
        // Execute real x86 conversions at the signed64 boundary. Apple arm64
        // long double has no extra precision, so +2^63 must be rejected before
        // a C++ cast; rounding INT64_MAX to double is not a valid upper bound.
        LsxEntryPacket call{};
        struct ScalarConversionCase { double input; std::uint64_t expected; };
        const std::array<ScalarConversionCase,7> scalarCases{{
            {0x1p63,UINT64_C(0x8000000000000000)},
            {0x1.fffffffffffffp62,UINT64_C(0x7ffffffffffffc00)},
            {-0x1p63,UINT64_C(0x8000000000000000)},
            {-0x1.0000000000001p63,UINT64_C(0x8000000000000000)},
            {std::numeric_limits<double>::infinity(),UINT64_C(0x8000000000000000)},
            {std::numeric_limits<double>::quiet_NaN(),UINT64_C(0x8000000000000000)},
            {42.0,42},
        }};
        for(unsigned mode=0;mode<4;++mode) {
            auto* entry=memory+mode*16;
            if(mode<2) {
            const std::array<std::uint8_t,6> code{0xf2,0x48,0x0f,
                static_cast<std::uint8_t>(mode?0x2d:0x2c),0x07,0xc3};
            std::memcpy(entry,code.data(),code.size());
            } else {
                // FNINIT; FLD qword[rdi]; FISTTP/FISTP qword[rsi]; MOV rax,[rsi]; RET.
                const std::array<std::uint8_t,10> code{0xdb,0xe3,0xdd,0x07,
                    static_cast<std::uint8_t>(mode==2?0xdd:0xdf),
                    static_cast<std::uint8_t>(mode==2?0x0e:0x3e),
                    0x48,0x8b,0x06,0xc3};
                std::memcpy(entry,code.data(),code.size());
            }
            for(const auto& test:scalarCases) {
                std::memcpy(memory+64,&test.input,sizeof(test.input));
                std::memset(memory+80,0xa5,sizeof(std::uint64_t));
                call.arg_count=2;call.args={reinterpret_cast<std::uint64_t>(memory+64),
                    reinterpret_cast<std::uint64_t>(memory+80)};
                const auto converted=DispatchGuestOnArm64(reinterpret_cast<std::uint64_t>(entry),call);
                if(converted!=test.expected) std::fprintf(stderr,
                    "[SCALAR_I64_BOUNDARY] mode=%u input=%a result=%llx expected=%llx\n",
                    mode,test.input,static_cast<unsigned long long>(converted),
                    static_cast<unsigned long long>(test.expected));
                check(converted==test.expected,"scalar64 conversion boundary/indefinite mismatch");
            }
        }
        std::puts("[SCALAR_I64_BOUNDARY] sseTruncating=7 sseRounded=7 x87Truncating=7 x87Rounded=7 passed");

 }
}
