// SPDX-License-Identifier: GPL-2.0-or-later
// Same host-libm HLE policy and NIDs as libc_internal_math.cpp. This adapter
// marshals console XMM argument/return registers instead of using a host ABI.
#include "GuestLibcMath.h"
#include "GuestFloatConversion.h"
#include <array>
#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cerrno>
#include <cstring>
#pragma STDC FENV_ACCESS ON
namespace Lsx4::Apple {
namespace {
enum class Math {Sin,Cos,Tan,Asin,Acos,Atan,Atan2,Exp,Exp2,Pow,Log,Log10};
struct Entry {std::string_view nid;Math function;bool single;};
constexpr std::array entries{
    Entry{"H8ya2H00jbI",Math::Sin,false},Entry{"Q4rRL34CEeE",Math::Sin,true},
    Entry{"2WE3BTYVwKM",Math::Cos,false},Entry{"-P6FNMzk2Kc",Math::Cos,true},
    Entry{"T7uyNqP7vQA",Math::Tan,false},Entry{"ZE6RNL+eLbk",Math::Tan,true},
    Entry{"7Ly52zaL44Q",Math::Asin,false},Entry{"GZWjF-YIFFk",Math::Asin,true},
    Entry{"JBcgYuW8lPU",Math::Acos,false},Entry{"QI-x0SL8jhw",Math::Acos,true},
    Entry{"OXmauLdQ8kY",Math::Atan,false},Entry{"weDug8QD-lE",Math::Atan,true},
    Entry{"HUbZmOnT-Dg",Math::Atan2,false},Entry{"EH-x713A99c",Math::Atan2,true},
    Entry{"NVadfnzQhHQ",Math::Exp,false},Entry{"8zsu04XNsZ4",Math::Exp,true},
    Entry{"dnaeGXbjP6E",Math::Exp2,false},Entry{"wuAQt-j+p4o",Math::Exp2,true},
    Entry{"9LCjpWyQ5Zc",Math::Pow,false},Entry{"1D0H2KNjshE",Math::Pow,true},
    Entry{"rtV7-jWC6Yg",Math::Log,false},Entry{"RQXLbdT2lc4",Math::Log,true},
    Entry{"WuMbPBKN1TU",Math::Log10,false},Entry{"lhpd6Wk6ccs",Math::Log10,true}
};
const Entry* Find(std::string_view identity) {
    const auto separator=identity.find('#');if(separator==std::string_view::npos)return nullptr;
    const auto suffix=identity.substr(separator);
    if(suffix!="#libc#libc" && suffix!="#libSceLibcInternal#libSceLibcInternal")return nullptr;
    const auto nid=identity.substr(0,separator);
    for(const auto& e:entries)if(nid==e.nid)return &e;
    return nullptr;
}
template<class T>T Calculate(Math operation,T x,T y) {
    switch(operation) {
    case Math::Sin:return std::sin(x);case Math::Cos:return std::cos(x);case Math::Tan:return std::tan(x);
    case Math::Asin:return std::asin(x);case Math::Acos:return std::acos(x);case Math::Atan:return std::atan(x);
    case Math::Atan2:return std::atan2(x,y);case Math::Exp:return std::exp(x);case Math::Exp2:return std::exp2(x);
    case Math::Pow:return std::pow(x,y);case Math::Log:return std::log(x);case Math::Log10:return std::log10(x);
    }
}
}
bool IsLibcMath(std::string_view identity){return Find(identity)!=nullptr;}
bool ExecuteLibcMath(std::string_view identity,Translation::CpuFrame& frame) {
    const auto* entry=Find(identity);if(!entry || (frame.simd_control&(64|0x8000)))return false;
    fenv_t saved;
    if(std::feholdexcept(&saved))return false;
    const int previousErrno=errno;
    constexpr std::array modes{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO};
    if(std::fesetround(modes[(frame.simd_control>>13)&3])){std::fesetenv(&saved);return false;}
    std::array<std::uint8_t,8> output{};
    errno=0;
    if(entry->single) {
        float x,y;std::memcpy(&x,frame.vectors[0].data(),4);std::memcpy(&y,frame.vectors[1].data(),4);
        const auto value=Calculate(entry->function,x,y);std::memcpy(output.data(),&value,4);
    } else {
        double x,y;std::memcpy(&x,frame.vectors[0].data(),8);std::memcpy(&y,frame.vectors[1].data(),8);
        const auto value=Calculate(entry->function,x,y);std::memcpy(output.data(),&value,8);
    }
    const int mathErrno=errno,raised=std::fetestexcept(FE_ALL_EXCEPT);
    std::fesetenv(&saved);errno=previousErrno;
    // Error paths continue through the actual bundled libc until its guest
    // errno/fenv interfaces are connected. Never drop a guest-visible error.
    if(mathErrno || (raised&(FE_INVALID|FE_DIVBYZERO|FE_OVERFLOW|FE_UNDERFLOW)))return false;
    unsigned exceptions=0;
    if(raised&FE_INVALID)exceptions|=1;
    if(raised&FE_DIVBYZERO)exceptions|=4;
    if(raised&FE_OVERFLOW)exceptions|=8;
    if(raised&FE_UNDERFLOW)exceptions|=16;
    if(raised&FE_INEXACT)exceptions|=32;
    frame.simd_control|=exceptions;
    if(exceptions&~(frame.simd_control>>7)&0x3f)throw GuestSIMDFault();
    std::copy_n(output.begin(),entry->single?4:8,frame.vectors[0].begin());return true;
}
}
