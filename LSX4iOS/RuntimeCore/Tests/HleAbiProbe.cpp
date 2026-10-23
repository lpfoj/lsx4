// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise Android's actual typed marshaller against Apple's native C++ ABI.
#include "core/aerolib/hle_abi.h"
#include <bit>
#include <stdexcept>

namespace ProbeAbi {
struct Mixed { double real; u64 integer; };
struct Large { u64 a, b, c; };
struct Unknown { u64 a, b; };
unsigned calls;
long long Narrow(signed char a, short b, int c, long long d, signed char e,
                 short f, signed char g, short h, int i) {
    ++calls;
    return a + b + c + d + e + f + g + h + i;
}
double Floating(double a, double b, double c, double d, double e,
                double f, double g, double h, double i, double j) {
    return a+b+c+d+e+f+g+h+i+j;
}
Mixed Aggregate(Mixed value, u64 delta) { return {value.real * 2, value.integer + delta}; }
Large Indirect(u64 a, u64 b, u64 c) { return {a+1,b+2,c+3}; }
Unknown Unsupported(Unknown value) { return value; }
}
namespace Core::AeroLib {
template<> struct AndroidX64HleTypeAbiOverride<ProbeAbi::Mixed> {
    static constexpr bool enabled=true, memory=false;
    static constexpr u8 count=2;
    static constexpr std::array classes{AndroidX64SysVClass::Sse,AndroidX64SysVClass::Integer};
};
}

unsigned LSX4RunSharedHleAbiProbe(void* storage) {
    using namespace Core::AeroLib;
    using namespace ProbeAbi;
    const auto check=[](bool ok,const char* reason) { if(!ok) throw std::runtime_error(reason); };
    auto* stack=static_cast<u64*>(storage);
    AndroidX64HleRawCall raw{};
    AndroidX64HleRawResult result{};
    raw.guest_rsp=reinterpret_cast<u64>(stack);
    raw.gpr={u64(-1),u64(-2),u64(-3),4,5,6};
    stack[0]=0;stack[1]=u64(-7);stack[2]=u64(-8);stack[3]=9;
    constexpr auto narrow=HleAbiDetail::MakeDescriptor<decltype(&Narrow)>();
    static_assert(narrow.supported && narrow.stack_argument_bytes==24);
    check(narrow.invoke(reinterpret_cast<u64>(&Narrow),raw,result) &&
          std::bit_cast<long long>(result.rax)==3 && calls==1,"typed narrow/stack ABI");
    raw.guest_rsp=0x10000;
    check(!narrow.invoke(reinterpret_cast<u64>(&Narrow),raw,result) && calls==1,
          "typed invalid stack must reject before native call");

    raw={};result={};raw.guest_rsp=reinterpret_cast<u64>(stack);
    for(unsigned i=0;i<8;++i) raw.xmm[i].low=std::bit_cast<u64>(double(i+1));
    stack[1]=std::bit_cast<u64>(9.0);stack[2]=std::bit_cast<u64>(10.0);
    constexpr auto floating=HleAbiDetail::MakeDescriptor<decltype(&Floating)>();
    check(floating.invoke(reinterpret_cast<u64>(&Floating),raw,result) &&
          std::bit_cast<double>(result.xmm0.low)==55.0,"typed FP register/stack ABI");

    raw={};result={};raw.gpr={17,4};raw.xmm[0].low=std::bit_cast<u64>(1.25);
    constexpr auto mixed=HleAbiDetail::MakeDescriptor<decltype(&Aggregate)>();
    static_assert(mixed.supported);
    check(mixed.invoke(reinterpret_cast<u64>(&Aggregate),raw,result) &&
          std::bit_cast<double>(result.xmm0.low)==2.5 && result.rax==21,
          "typed mixed aggregate argument/return ABI");

    raw={};result={};raw.gpr={reinterpret_cast<u64>(stack+16),10,20,30};
    constexpr auto indirect=HleAbiDetail::MakeDescriptor<decltype(&Indirect)>();
    static_assert(indirect.supported);
    check(indirect.invoke(reinterpret_cast<u64>(&Indirect),raw,result) &&
          stack[16]==11 && stack[17]==22 && stack[18]==33 && result.rax==raw.gpr[0],
          "typed indirect aggregate return ABI");
    constexpr auto unknown=HleAbiDetail::MakeDescriptor<decltype(&Unsupported)>();
    static_assert(!unknown.supported);
    check(!unknown.invoke(reinterpret_cast<u64>(&Unsupported),raw,result),
          "unknown aggregate ABI must fail closed");
    return 6;
}
