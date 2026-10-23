// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/x86_decoder.h"
#include "executor/dynamic_translation/vector_semantics.h"
#include "executor/dynamic_translation/address_resolver.h"
#include "executor/dynamic_translation/register_access.h"
#include "executor/dynamic_translation/integer_math.h"
#include <algorithm>
#include <bit>
#include <cstdlib>

namespace Lsx4::Apple {
// Integer SSE/SSSE3 operations missing from the common executor. Each family
// shares operand validation and commits only after a complete checked read.
// Reference: Intel SDM vol. 2, the corresponding P*/VP* instruction entries.
inline std::optional<Translation::VectorSemanticResult> ExecutePackedVector(
    const Translation::Instruction& ins,Translation::CpuFrame& frame,
    const Translation::GuestMemoryPort& memory) {
    using namespace Translation;
    enum class Op {Abs,Sign,HorizontalAdd,HorizontalSub,MultiplyAdd,MultiplyBytes,
                   RoundedMultiply,Average,SumDifferences,Align,ShuffleHigh,ShuffleLow,Test,Mask};
    Op op;unsigned bytes=0;bool saturated=false;
#define PAIR(name,body) case X86_MNEMONIC_##name:case X86_MNEMONIC_V##name:body;break
    switch(ins.mnemonic) {
    PAIR(PABSB,op=Op::Abs;bytes=1);PAIR(PABSW,op=Op::Abs;bytes=2);PAIR(PABSD,op=Op::Abs;bytes=4);
    PAIR(PSIGNB,op=Op::Sign;bytes=1);PAIR(PSIGNW,op=Op::Sign;bytes=2);PAIR(PSIGND,op=Op::Sign;bytes=4);
    PAIR(PHADDW,op=Op::HorizontalAdd;bytes=2);PAIR(PHADDD,op=Op::HorizontalAdd;bytes=4);
    PAIR(PHADDSW,op=Op::HorizontalAdd;bytes=2;saturated=true);
    PAIR(PHSUBW,op=Op::HorizontalSub;bytes=2);PAIR(PHSUBD,op=Op::HorizontalSub;bytes=4);
    PAIR(PHSUBSW,op=Op::HorizontalSub;bytes=2;saturated=true);
    PAIR(PMADDWD,op=Op::MultiplyAdd);PAIR(PMADDUBSW,op=Op::MultiplyBytes);
    PAIR(PMULHRSW,op=Op::RoundedMultiply);
    PAIR(PAVGB,op=Op::Average;bytes=1);PAIR(PAVGW,op=Op::Average;bytes=2);
    PAIR(PSADBW,op=Op::SumDifferences);PAIR(PALIGNR,op=Op::Align);
    PAIR(PSHUFHW,op=Op::ShuffleHigh);PAIR(PSHUFLW,op=Op::ShuffleLow);
    PAIR(PTEST,op=Op::Test);PAIR(PMOVMSKB,op=Op::Mask);
    default:return std::nullopt;
    }
#undef PAIR
    const auto bad=VectorSemanticResult{VectorStop::Unsupported,ins.address};
    const auto done=VectorSemanticResult{VectorStop::Continue,ins.address+ins.length};
    const bool legacy=ins.encoding_family==X86_INSTRUCTION_ENCODING_LEGACY;
    const bool unary=op==Op::Abs || op==Op::ShuffleHigh || op==Op::ShuffleLow || op==Op::Mask;
    const bool immediate=op==Op::Align || op==Op::ShuffleHigh || op==Op::ShuffleLow;
    const bool flagOnly=op==Op::Test;
    const unsigned second=(legacy || unary || flagOnly)?1:2;
    const unsigned size=ins.operands[op==Op::Mask?1:0].bit_width/8;
    if(!ins.length || (ins.prefix_attributes&X86_ATTRIB_HAS_LOCK) ||
       (!legacy && ins.encoding_family!=X86_INSTRUCTION_ENCODING_VEX) ||
       ins.operand_count!=second+1+immediate || (size!=16 && size!=32) || (legacy && size!=16))return bad;
    if(immediate && ins.operands[second+1].form!=OperandForm::Immediate)return bad;
    const unsigned control=immediate?ins.operands[second+1].immediate.bits&255:0;
    auto slot=[&](const Operand& operand)->std::optional<unsigned> {
        const auto base=size==16?X86_REGISTER_XMM0:X86_REGISTER_YMM0;
        if(operand.form!=OperandForm::Register || operand.bit_width!=size*8 ||
           operand.register_id<base || operand.register_id>base+15)return std::nullopt;
        return operand.register_id-base;
    };
    const auto destination=slot(ins.operands[0]);
    const auto integer=DecodeIntegerRegister(ins.operands[0]);
    if(op==Op::Mask) {if(!integer || integer->width!=32)return bad;}
    else if(!destination)return bad;
    std::array<std::uint8_t,32> left{},right{};
    if(!unary) {
        const auto first=slot(ins.operands[(legacy || flagOnly)?0:1]);if(!first)return bad;
        left=frame.vectors[*first];
    }
    const auto& source=ins.operands[second];
    if(const auto index=slot(source))right=frame.vectors[*index];
    else if(op!=Op::Mask && source.form==OperandForm::Memory && source.bit_width==size*8) {
        const auto address=ResolveGuestAddress(ins,source,frame);
        if(!address || (legacy && *address%16))return bad;
        if(!memory.read || !memory.read(*address,std::span(right).first(size),memory.context))
            return VectorSemanticResult{VectorStop::MemoryFault,ins.address,*address};
    } else return bad;
    if(op==Op::Mask) {
        std::uint32_t mask=0;for(unsigned i=0;i<size;++i)mask|=std::uint32_t(right[i]>>7)<<i;
        WriteInteger(frame,static_cast<IntegerRegister>(integer->slot),mask,32);return done;
    }
    if(flagOnly) {
        unsigned both=0,other=0;for(unsigned i=0;i<size;++i){both|=left[i]&right[i];other|=(~left[i])&right[i];}
        frame.condition_word=(frame.condition_word&~ArithmeticFlagMask)|(both?0:ZeroFlag)|(other?0:CarryFlag);return done;
    }
    auto output=legacy?frame.vectors[*destination]:std::array<std::uint8_t,32>{};
    auto read=[](const auto& input,unsigned offset,unsigned width)->std::uint32_t {
        std::uint32_t value=0;for(unsigned i=0;i<width;++i)value|=std::uint32_t(input[offset+i])<<(8*i);return value;
    };
    auto sign=[&](const auto& input,unsigned offset,unsigned width)->std::int64_t {
        return std::bit_cast<std::int64_t>(SignExtend(read(input,offset,width),width*8));
    };
    auto write=[&](unsigned offset,unsigned width,std::uint64_t value) {
        for(unsigned i=0;i<width;++i)output[offset+i]=value>>(8*i);
    };
    auto clamp=[](std::int64_t value){return std::clamp<std::int64_t>(value,-32768,32767);};
    for(unsigned lane=0;lane<size;lane+=16) {
        if(op==Op::Abs || op==Op::Sign || op==Op::Average) {
            for(unsigned i=lane;i<lane+16;i+=bytes) {
                const auto value=read(op==Op::Abs?right:left,i,bytes);
                const auto signedControl=sign(right,i,bytes);
                const std::uint32_t result=op==Op::Average?(value+read(right,i,bytes)+1)/2:
                    op==Op::Abs?(signedControl<0?0u-value:value):
                    signedControl<0?0u-value:signedControl==0?0:value;
                write(i,bytes,result);
            }
        } else if(op==Op::HorizontalAdd || op==Op::HorizontalSub) {
            for(unsigned sourceIndex=0;sourceIndex<2;++sourceIndex)for(unsigned pair=0;pair<8/bytes;++pair) {
                const auto& input=sourceIndex?right:left;const auto at=lane+pair*bytes*2;
                const auto a=sign(input,at,bytes),b=sign(input,at+bytes,bytes);
                const auto value=op==Op::HorizontalAdd?a+b:a-b;
                write(lane+sourceIndex*8+pair*bytes,bytes,saturated?clamp(value):value);
            }
        } else if(op==Op::MultiplyAdd || op==Op::MultiplyBytes) {
            const unsigned width=op==Op::MultiplyAdd?2:1;
            for(unsigned i=lane;i<lane+16;i+=width*2) {
                const auto a=width==2?sign(left,i,width):read(left,i,width);
                const auto b=width==2?sign(left,i+width,width):read(left,i+width,width);
                const auto value=a*sign(right,i,width)+b*sign(right,i+width,width);
                write(i,width*2,width==1?clamp(value):value);
            }
        } else if(op==Op::RoundedMultiply) {
            for(unsigned i=lane;i<lane+16;i+=2)write(i,2,(sign(left,i,2)*sign(right,i,2)+16384)>>15);
        } else if(op==Op::SumDifferences) {
            for(unsigned half=0;half<16;half+=8) {
                unsigned sum=0;for(unsigned i=lane+half;i<lane+half+8;++i)sum+=std::abs(int(left[i])-int(right[i]));
                write(lane+half,8,sum);
            }
        } else if(op==Op::Align) {
            for(unsigned i=0;i<16;++i) {
                const auto at=control+i;output[lane+i]=at<16?right[lane+at]:at<32?left[lane+at-16]:0;
            }
        } else {
            std::copy_n(right.begin()+lane,16,output.begin()+lane);
            const unsigned half=op==Op::ShuffleHigh?8:0;
            for(unsigned i=0;i<4;++i)write(lane+half+i*2,2,read(right,lane+half+((control>>(i*2))&3)*2,2));
        }
    }
    frame.vectors[*destination]=output;return done;
}
}
