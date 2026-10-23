// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestScalar.h"
#include "GuestStrings.h"
#include "common/x86_decoder.h"
#include "executor/dynamic_translation/register_access.h"
#include "executor/dynamic_translation/address_resolver.h"
#include "executor/dynamic_translation/integer_math.h"
#include <array>
#include <bit>
#include <atomic>

namespace Lsx4::Apple {
using namespace Lsx4::Translation;
namespace {
std::optional<std::uint64_t> Value(const Instruction& instruction,const Operand& operand,
    const CpuFrame& frame,const GuestMemoryPort& memory,std::uint64_t& fault) {
    if(operand.form==OperandForm::Immediate) return operand.immediate.bits;
    if(auto access=DecodeIntegerRegister(operand))
        return ReadInteger(frame,static_cast<IntegerRegister>(access->slot),access->width,access->upper_byte);
    const auto address=ResolveGuestAddress(instruction,operand,frame);
    if(!address || !operand.bit_width || operand.bit_width>64 || operand.bit_width%8) return std::nullopt;
    fault=*address;std::array<std::uint8_t,8> bytes{};
    if(!memory.read || !memory.read(*address,std::span(bytes).first(operand.bit_width/8),memory.context)) return std::nullopt;
    std::uint64_t result=0;for(unsigned i=0;i<operand.bit_width/8;++i) result|=std::uint64_t(bytes[i])<<(i*8);
    return result;
}
bool SetRegister(const Operand& operand,CpuFrame& frame,std::uint64_t value) {
    const auto access=DecodeIntegerRegister(operand);if(!access) return false;
    WriteInteger(frame,static_cast<IntegerRegister>(access->slot),value,access->width,access->upper_byte);return true;
}
}

SemanticResult ExecuteGuestScalar(const Instruction& ins,CpuFrame& frame,const GuestMemoryPort& memory) {
    auto result=ExecuteScalarInstruction(ins,frame,memory);
    if(result.stop!=SemanticStop::Unsupported) return result;
    const auto done=SemanticResult{SemanticStop::Continue,ins.address+ins.length};
    if(auto string=ExecuteGuestString(ins,frame,memory))return *string;
    if(ins.mnemonic==X86_MNEMONIC_CLD || ins.mnemonic==X86_MNEMONIC_STD) {
        if(ins.prefix_attributes&X86_ATTRIB_HAS_LOCK)return result;
        frame.condition_word=(frame.condition_word&~DirectionFlag)|(ins.mnemonic==X86_MNEMONIC_STD?DirectionFlag:0);
        return done;
    }
    std::uint64_t fault=ins.address;
    const auto failed=[&] {return SemanticResult{SemanticStop::MemoryFault,ins.address,fault};};
    // Architectural cache hints do not read guest data and cannot fault on an
    // unmapped target. The checked memory backend is already coherent.
    if(ins.mnemonic==X86_MNEMONIC_PREFETCHNTA || ins.mnemonic==X86_MNEMONIC_PREFETCHT0 ||
       ins.mnemonic==X86_MNEMONIC_PREFETCHT1 || ins.mnemonic==X86_MNEMONIC_PREFETCHT2 ||
       ins.mnemonic==X86_MNEMONIC_PREFETCHW)return done;
    if(ins.mnemonic==X86_MNEMONIC_LFENCE || ins.mnemonic==X86_MNEMONIC_SFENCE || ins.mnemonic==X86_MNEMONIC_MFENCE) {
        std::atomic_thread_fence(std::memory_order_seq_cst);return done;
    }
    if(ins.mnemonic==X86_MNEMONIC_CWD || ins.mnemonic==X86_MNEMONIC_CDQ || ins.mnemonic==X86_MNEMONIC_CQO) {
        const unsigned width=ins.mnemonic==X86_MNEMONIC_CWD?16:ins.mnemonic==X86_MNEMONIC_CDQ?32:64;
        const bool negative=ReadInteger(frame,IntegerRegister::A,width)&(1ull<<(width-1));
        WriteInteger(frame,IntegerRegister::D,negative?WidthMask(width):0,width);return done;
    }
    if(ins.mnemonic==X86_MNEMONIC_CBW || ins.mnemonic==X86_MNEMONIC_CWDE || ins.mnemonic==X86_MNEMONIC_CDQE) {
        const unsigned width=ins.mnemonic==X86_MNEMONIC_CBW?8:ins.mnemonic==X86_MNEMONIC_CWDE?16:32;
        WriteInteger(frame,IntegerRegister::A,SignExtend(ReadInteger(frame,IntegerRegister::A,width),width),width*2);
        return done;
    }
    if((ins.mnemonic==X86_MNEMONIC_DIV || ins.mnemonic==X86_MNEMONIC_IDIV) && ins.operand_count==1 &&
        !(ins.prefix_attributes&X86_ATTRIB_HAS_LOCK)) {
        const auto width=ins.operands[0].bit_width;
        if(width!=8 && width!=16 && width!=32 && width!=64)return result;
        const auto source=Value(ins,ins.operands[0],frame,memory,fault);if(!source)return failed();
        const auto divisor=*source&WidthMask(width);
        if(!divisor)throw GuestDivideError();
        using U128=unsigned __int128;
        const U128 numerator=width==8?ReadInteger(frame,IntegerRegister::A,16):
            (static_cast<U128>(ReadInteger(frame,IntegerRegister::D,width))<<width)|ReadInteger(frame,IntegerRegister::A,width);
        const bool signedDivision=ins.mnemonic==X86_MNEMONIC_IDIV;
        const bool negativeNumerator=signedDivision && ((numerator>>(width*2-1))&1);
        const bool negativeDivisor=signedDivision && (divisor&(1ull<<(width-1)));
        const bool negativeQuotient=negativeNumerator!=negativeDivisor;
        const U128 doubleMask=width==64?~U128(0):(U128(1)<<(width*2))-1;
        const U128 magnitude=negativeNumerator?(U128(0)-numerator)&doubleMask:numerator;
        const std::uint64_t divisorMagnitude=negativeDivisor?(0-divisor)&WidthMask(width):divisor;
        U128 quotient=magnitude/divisorMagnitude;
        std::uint64_t remainder=magnitude%divisorMagnitude;
        const U128 maximum=signedDivision?(U128(1)<<(width-1))-(negativeQuotient?0:1):WidthMask(width);
        if(quotient>maximum)throw GuestDivideError();
        auto q=static_cast<std::uint64_t>(quotient);
        if(negativeQuotient)q=0-q;
        if(negativeNumerator)remainder=0-remainder;
        // Commit only after both zero and quotient-overflow checks. Preserve
        // undefined flags and the untouched part of 8/16-bit registers.
        if(width==8)WriteInteger(frame,IntegerRegister::A,(q&255)|((remainder&255)<<8),16);
        else {WriteInteger(frame,IntegerRegister::A,q,width);WriteInteger(frame,IntegerRegister::D,remainder,width);}
        return done;
    }
    if((ins.mnemonic==X86_MNEMONIC_BLSI || ins.mnemonic==X86_MNEMONIC_BLSR ||
        ins.mnemonic==X86_MNEMONIC_BLSMSK) && ins.operand_count==2) {
        const auto width=ins.operands[0].bit_width;if(width!=32 && width!=64)return result;
        const auto source=Value(ins,ins.operands[1],frame,memory,fault);if(!source)return failed();
        const auto input=*source&WidthMask(width);
        const auto value=(ins.mnemonic==X86_MNEMONIC_BLSI ? input&(0-input) :
                          ins.mnemonic==X86_MNEMONIC_BLSR ? input&(input-1) : input^(input-1))&WidthMask(width);
        if(!SetRegister(ins.operands[0],frame,value))return result;
        const bool carry=ins.mnemonic==X86_MNEMONIC_BLSI ? input!=0 : input==0;
        frame.condition_word=(frame.condition_word&~(CarryFlag|ZeroFlag|SignFlag|OverflowFlag))|
            (carry?CarryFlag:0)|(value==0?ZeroFlag:0)|(value&(1ull<<(width-1))?SignFlag:0);
        return done;
    }
    if((ins.mnemonic==X86_MNEMONIC_SHLX || ins.mnemonic==X86_MNEMONIC_SHRX ||
        ins.mnemonic==X86_MNEMONIC_SARX || ins.mnemonic==X86_MNEMONIC_RORX) && ins.operand_count==3) {
        const auto width=ins.operands[0].bit_width;if(width!=32 && width!=64)return result;
        const auto source=Value(ins,ins.operands[1],frame,memory,fault),amount=Value(ins,ins.operands[2],frame,memory,fault);
        if(!source || !amount)return failed();
        const auto input=*source&WidthMask(width),count=*amount&(width-1);
        std::uint64_t value=input;
        if(count) {
            if(ins.mnemonic==X86_MNEMONIC_SHLX)value=input<<count;
            else if(ins.mnemonic==X86_MNEMONIC_SHRX)value=input>>count;
            else if(ins.mnemonic==X86_MNEMONIC_SARX)value=std::bit_cast<std::int64_t>(SignExtend(input,width))>>count;
            else value=(input>>count)|(input<<(width-count));
        }
        if(!SetRegister(ins.operands[0],frame,value&WidthMask(width)))return result;
        return done;
    }
    if((ins.mnemonic==X86_MNEMONIC_IMUL || ins.mnemonic==X86_MNEMONIC_MUL) &&
       ins.operand_count>=1 && ins.operand_count<=3 && !(ins.prefix_attributes&X86_ATTRIB_HAS_LOCK)) {
        const bool one=ins.operand_count==1,signedMultiply=ins.mnemonic==X86_MNEMONIC_IMUL;
        if(!signedMultiply && !one)return result;
        const auto width=ins.operands[0].bit_width;
        if(width!=8 && width!=16 && width!=32 && width!=64)return result;
        if(!one && width==8)return result;
        const auto left=one ? std::optional(ReadInteger(frame,IntegerRegister::A,width)) :
            Value(ins,ins.operands[ins.operand_count==3?1:0],frame,memory,fault);
        const auto right=Value(ins,ins.operands[ins.operand_count-1],frame,memory,fault);
        if(!left || !right)return failed();
        auto signedValue=[&](std::uint64_t value){return std::bit_cast<std::int64_t>(SignExtend(value,width));};
        unsigned __int128 product;
        bool overflow;
        if(signedMultiply) {
            const __int128 signedProduct=static_cast<__int128>(signedValue(*left))*signedValue(*right);
            product=static_cast<unsigned __int128>(signedProduct);
            overflow=signedProduct!=signedValue(static_cast<std::uint64_t>(product)&WidthMask(width));
        } else {
            product=static_cast<unsigned __int128>(*left&WidthMask(width))*(*right&WidthMask(width));
            overflow=(product>>width)!=0;
        }
        if(one && width==8) WriteInteger(frame,IntegerRegister::A,static_cast<std::uint64_t>(product),16);
        else if(one) {
            WriteInteger(frame,IntegerRegister::A,static_cast<std::uint64_t>(product)&WidthMask(width),width);
            WriteInteger(frame,IntegerRegister::D,static_cast<std::uint64_t>(product>>width)&WidthMask(width),width);
        } else if(!SetRegister(ins.operands[0],frame,static_cast<std::uint64_t>(product)&WidthMask(width)))return result;
        frame.condition_word=(frame.condition_word&~(CarryFlag|OverflowFlag))|(overflow?(CarryFlag|OverflowFlag):0);
        return done;
    }
    if(ins.mnemonic==X86_MNEMONIC_POPCNT && ins.operand_count==2) {
        const auto width=ins.operands[0].bit_width;
        if(width!=16 && width!=32 && width!=64) return result;
        const auto source=Value(ins,ins.operands[1],frame,memory,fault);if(!source)return failed();
        const auto value=*source&WidthMask(width);
        if(!SetRegister(ins.operands[0],frame,std::popcount(value)))return result;
        frame.condition_word=(frame.condition_word&~ArithmeticFlagMask)|(value==0?ZeroFlag:0);
        return done;
    }
    if(ins.mnemonic==X86_MNEMONIC_BEXTR && ins.operand_count==3 && !(ins.prefix_attributes&X86_ATTRIB_HAS_LOCK)) {
        const auto width=ins.operands[0].bit_width;
        if(width!=32 && width!=64) return result;
        const auto source=Value(ins,ins.operands[1],frame,memory,fault),control=Value(ins,ins.operands[2],frame,memory,fault);
        if(!source || !control) return failed();
        const unsigned start=*control&255,length=(*control>>8)&255;
        const auto value=start>=width ? 0 : ((*source&WidthMask(width))>>start)&WidthMask(length);
        if(!SetRegister(ins.operands[0],frame,value)) return result;
        frame.condition_word=(frame.condition_word&~(CarryFlag|OverflowFlag|ZeroFlag))|(value==0?ZeroFlag:0);
        return done;
    }
    if(ins.mnemonic==X86_MNEMONIC_ANDN && ins.operand_count==3 && !(ins.prefix_attributes&X86_ATTRIB_HAS_LOCK)) {
        const auto width=ins.operands[0].bit_width;
        if(width!=32 && width!=64) return result;
        const auto a=Value(ins,ins.operands[1],frame,memory,fault),b=Value(ins,ins.operands[2],frame,memory,fault);
        if(!a || !b) return failed();
        const auto value=(~*a & *b)&WidthMask(width);
        if(!SetRegister(ins.operands[0],frame,value)) return result;
        constexpr auto flags=CarryFlag|OverflowFlag|SignFlag|ZeroFlag;
        frame.condition_word=(frame.condition_word&~flags)|(FlagsForLogicalResult(value,width).bits&(SignFlag|ZeroFlag));
        return done;
    }
    if(ins.mnemonic==X86_MNEMONIC_BT && ins.operand_count==2 && !(ins.prefix_attributes&X86_ATTRIB_HAS_LOCK)) {
        const auto width=ins.operands[0].bit_width;
        if(width!=16 && width!=32 && width!=64) return result;
        const auto index=Value(ins,ins.operands[1],frame,memory,fault);if(!index) return failed();
        auto operand=ins.operands[0];
        if(operand.form==OperandForm::Memory && ins.operands[1].form==OperandForm::Register) {
            const auto signedIndex=std::bit_cast<std::int64_t>(SignExtend(*index,width));
            const auto words=signedIndex<0 ? -((- (signedIndex+1))/width+1) : signedIndex/width;
            // Resolve first so RIP-relative and 32-bit addressing retain their normal rules.
            const auto address=ResolveGuestAddress(ins,operand,frame);if(!address) return result;
            fault=*address+static_cast<std::uint64_t>(words*static_cast<std::int64_t>(width/8));
            std::array<std::uint8_t,8> bytes{};
            if(!memory.read || !memory.read(fault,std::span(bytes).first(width/8),memory.context)) return failed();
            std::uint64_t value=0;for(unsigned i=0;i<width/8;++i) value|=std::uint64_t(bytes[i])<<(i*8);
            frame.condition_word=(frame.condition_word&~CarryFlag)|((value>>(*index&(width-1)))&1);
        } else {
            const auto value=Value(ins,operand,frame,memory,fault);if(!value) return failed();
            frame.condition_word=(frame.condition_word&~CarryFlag)|((*value>>(*index&(width-1)))&1);
        }
        return done;
    }
    if((ins.mnemonic==X86_MNEMONIC_BSF || ins.mnemonic==X86_MNEMONIC_BSR ||
        ins.mnemonic==X86_MNEMONIC_TZCNT || ins.mnemonic==X86_MNEMONIC_LZCNT) && ins.operand_count==2) {
        const auto width=ins.operands[0].bit_width;
        if(width!=16 && width!=32 && width!=64) return result;
        const auto source=Value(ins,ins.operands[1],frame,memory,fault);if(!source) return failed();
        const auto value=*source&WidthMask(width);
        const bool trailing=ins.mnemonic==X86_MNEMONIC_BSF || ins.mnemonic==X86_MNEMONIC_TZCNT;
        const bool count=ins.mnemonic==X86_MNEMONIC_TZCNT || ins.mnemonic==X86_MNEMONIC_LZCNT;
        if(!count && !value) {frame.condition_word|=ZeroFlag;return done;}
        const auto bits=trailing ? (value ? std::countr_zero(value) : width) : std::countl_zero(value)-(64-width);
        const auto answer=count || trailing ? bits : width-1-bits;
        if(!SetRegister(ins.operands[0],frame,answer)) return result;
        if(count) frame.condition_word=(frame.condition_word&~(ZeroFlag|CarryFlag))|
            (answer==0?ZeroFlag:0)|(value==0?CarryFlag:0);
        else frame.condition_word&=~ZeroFlag;
        return done;
    }
    if(ins.mnemonic==X86_MNEMONIC_BSWAP && ins.operand_count==1) {
        const auto value=Value(ins,ins.operands[0],frame,memory,fault);if(!value) return failed();
        const auto width=ins.operands[0].bit_width;
        if((width==32 || width==64) && SetRegister(ins.operands[0],frame,ReverseByteOrder(*value,width/8))) return done;
    }
    return result;
}
}
