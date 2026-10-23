// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestFloatConversion.h"
#include "common/x86_decoder.h"
#include "executor/dynamic_translation/address_resolver.h"
#include "executor/dynamic_translation/register_access.h"
#include <algorithm>
#include <bit>
namespace Lsx4::Apple {
using namespace Translation;
namespace {
void Raise(CpuFrame& frame,unsigned exceptions) {
    frame.simd_control|=exceptions;
    if(exceptions&~(frame.simd_control>>7)&0x3f)throw GuestSIMDFault();
}
bool RoundUp(std::uint64_t integer,std::uint64_t remainder,unsigned discarded,bool negative,unsigned mode) {
    if(!remainder)return false;
    if(mode==1)return negative;
    if(mode==2)return !negative;
    if(mode==3 || discarded>64)return false;
    const auto halfway=1ull<<(discarded-1);
    return remainder>halfway || (remainder==halfway && (integer&1));
}
std::optional<unsigned> Xmm(const Operand& operand) {
    if(operand.form==OperandForm::Register && operand.register_id>=X86_REGISTER_XMM0 && operand.register_id<=X86_REGISTER_XMM15)
        return operand.register_id-X86_REGISTER_XMM0;
    return std::nullopt;
}
}
std::optional<VectorSemanticResult> ExecuteFloatConversion(const Instruction& ins,CpuFrame& frame,const GuestMemoryPort& memory) {
    const auto m=ins.mnemonic;
    const bool roundSingle=m==X86_MNEMONIC_ROUNDSS || m==X86_MNEMONIC_VROUNDSS || m==X86_MNEMONIC_ROUNDPS || m==X86_MNEMONIC_VROUNDPS;
    const bool roundDouble=m==X86_MNEMONIC_ROUNDSD || m==X86_MNEMONIC_VROUNDSD || m==X86_MNEMONIC_ROUNDPD || m==X86_MNEMONIC_VROUNDPD;
    if(roundSingle || roundDouble) {
        const auto unsupported=VectorSemanticResult{VectorStop::Unsupported,ins.address};
        const bool legacy=ins.encoding_family==X86_INSTRUCTION_ENCODING_LEGACY;
        const bool scalar=m==X86_MNEMONIC_ROUNDSS || m==X86_MNEMONIC_VROUNDSS || m==X86_MNEMONIC_ROUNDSD || m==X86_MNEMONIC_VROUNDSD;
        const unsigned right=scalar&&!legacy?2:1;
        if((!legacy && ins.encoding_family!=X86_INSTRUCTION_ENCODING_VEX) || ins.operand_count!=right+2 ||
           ins.operands[right+1].form!=OperandForm::Immediate)return unsupported;
        const unsigned size=ins.operands[0].bit_width/8,laneBytes=roundSingle?4:8;
        if((size!=16 && size!=32) || ((legacy || scalar) && size!=16))return unsupported;
        auto slot=[](const Operand& operand)->std::optional<unsigned> {
            if(operand.form!=OperandForm::Register)return std::nullopt;
            if(operand.register_id>=X86_REGISTER_XMM0 && operand.register_id<=X86_REGISTER_XMM15)return operand.register_id-X86_REGISTER_XMM0;
            if(operand.register_id>=X86_REGISTER_YMM0 && operand.register_id<=X86_REGISTER_YMM15)return operand.register_id-X86_REGISTER_YMM0;
            return std::nullopt;
        };
        const auto target=slot(ins.operands[0]);if(!target)return unsupported;
        auto output=frame.vectors[*target];
        if(scalar && !legacy) {
            const auto merge=Xmm(ins.operands[1]);if(!merge)return unsupported;
            output=frame.vectors[*merge];
        }
        const auto& operand=ins.operands[right];
        const unsigned sourceBytes=scalar?laneBytes:size;
        std::array<std::uint8_t,32> source{};
        if(const auto index=slot(operand))source=frame.vectors[*index];
        else if(operand.form==OperandForm::Memory) {
            const auto address=ResolveGuestAddress(ins,operand,frame);if(!address)return unsupported;
            if(!memory.read || !memory.read(*address,std::span(source).first(sourceBytes),memory.context))
                return VectorSemanticResult{VectorStop::MemoryFault,ins.address,*address};
        } else return unsupported;
        const auto immediate=ins.operands[right+1].immediate.bits;
        const unsigned mode=immediate&4?(frame.simd_control>>13)&3:immediate&3;
        const unsigned fraction=roundSingle?23:52,bias=roundSingle?127:1023,maxExponent=roundSingle?255:2047;
        unsigned exceptions=0;
        // Intel SDM ROUND{PS,PD,SS,SD}: only invalid (SNaN) and precision are
        // reported; DAZ converts denormals before rounding, without a D exception.
        // https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2b-manual.pdf
        for(unsigned offset=0;offset<sourceBytes;offset+=laneBytes) {
            std::uint64_t bits=0;
            for(unsigned i=0;i<laneBytes;++i)bits|=std::uint64_t(source[offset+i])<<(8*i);
            const auto sign=bits&(1ull<<(laneBytes*8-1)),absolute=bits&~sign;
            const auto rawExponent=(absolute>>fraction)&maxExponent;
            const auto mantissa=absolute&((1ull<<fraction)-1);
            auto rounded=bits;
            if(rawExponent==maxExponent) {
                if(mantissa && !(mantissa&(1ull<<(fraction-1)))) {exceptions|=1;rounded|=1ull<<(fraction-1);}
            } else if(!rawExponent && (frame.simd_control&64))rounded=sign;
            else if(absolute) {
                const int exponent=int(rawExponent?rawExponent:1)-int(bias);
                if(exponent<0) {
                    bool one=mode==1?bool(sign):mode==2?!sign:mode==0 && exponent==-1 && mantissa;
                    rounded=sign|(one?std::uint64_t(bias)<<fraction:0);
                    if(!(immediate&8))exceptions|=32;
                } else if(exponent<int(fraction)) {
                    const unsigned shift=fraction-exponent;
                    const auto mask=(1ull<<shift)-1,remainder=absolute&mask;
                    auto magnitude=absolute&~mask;
                    if(RoundUp(((1ull<<fraction)|mantissa)>>shift,remainder,shift,bool(sign),mode))magnitude+=1ull<<shift;
                    rounded=sign|magnitude;
                    if(remainder && !(immediate&8))exceptions|=32;
                }
            }
            for(unsigned i=0;i<laneBytes;++i)output[offset+i]=rounded>>(8*i);
        }
        Raise(frame,exceptions);
        if(!legacy && size==16)std::fill(output.begin()+16,output.end(),0);
        frame.vectors[*target]=output;
        return VectorSemanticResult{VectorStop::Continue,ins.address+ins.length};
    }
    const bool packedToFloat=m==X86_MNEMONIC_CVTDQ2PS || m==X86_MNEMONIC_VCVTDQ2PS;
    const bool packedToInt=m==X86_MNEMONIC_CVTPS2DQ || m==X86_MNEMONIC_VCVTPS2DQ || m==X86_MNEMONIC_CVTTPS2DQ || m==X86_MNEMONIC_VCVTTPS2DQ;
    if(packedToFloat || packedToInt) {
        const auto unsupported=VectorSemanticResult{VectorStop::Unsupported,ins.address};
        if(ins.operand_count!=2)return unsupported;
        const auto& destination=ins.operands[0];const auto& source=ins.operands[1];
        auto slot=[](const Operand& op)->std::optional<unsigned> {
            if(op.form!=OperandForm::Register)return std::nullopt;
            if(op.register_id>=X86_REGISTER_XMM0 && op.register_id<=X86_REGISTER_XMM15)return op.register_id-X86_REGISTER_XMM0;
            if(op.register_id>=X86_REGISTER_YMM0 && op.register_id<=X86_REGISTER_YMM15)return op.register_id-X86_REGISTER_YMM0;
            return std::nullopt;
        };
        const auto target=slot(destination);const unsigned size=destination.bit_width/8;
        if(!target || (size!=16 && size!=32))return unsupported;
        std::array<std::uint8_t,32> input{};
        if(auto index=slot(source))input=frame.vectors[*index];
        else {
            const auto address=ResolveGuestAddress(ins,source,frame);if(!address)return unsupported;
            if(!memory.read || !memory.read(*address,std::span(input).first(size),memory.context))return VectorSemanticResult{VectorStop::MemoryFault,ins.address,*address};
        }
        auto operation=ins;operation.encoding_family=X86_INSTRUCTION_ENCODING_LEGACY;
        operation.mnemonic=packedToFloat?X86_MNEMONIC_CVTSI2SS:
            m==X86_MNEMONIC_CVTTPS2DQ || m==X86_MNEMONIC_VCVTTPS2DQ?X86_MNEMONIC_CVTTSS2SI:X86_MNEMONIC_CVTSS2SI;
        for(unsigned i=0;i<2;++i) {
            const bool integer=packedToFloat?(i==1):(i==0);
            operation.operands[i]={};operation.operands[i].form=OperandForm::Register;
            operation.operands[i].register_id=integer?X86_REGISTER_EAX:X86_REGISTER_XMM0;
            operation.operands[i].bit_width=integer?32:128;
        }
        auto output=frame.vectors[*target];unsigned exceptions=0;
        for(unsigned offset=0;offset<size;offset+=4) {
            auto lane=frame;lane.simd_control=(frame.simd_control|0x1f80)&~0x3f;
            if(packedToFloat) {
                lane.integer[0]=0;for(unsigned i=0;i<4;++i)lane.integer[0]|=std::uint64_t(input[offset+i])<<(8*i);
            } else std::copy_n(input.begin()+offset,4,lane.vectors[0].begin());
            const auto status=ExecuteFloatConversion(operation,lane,{});
            if(!status || status->stop!=VectorStop::Continue)return unsupported;
            exceptions|=lane.simd_control&0x3f;
            if(packedToFloat)std::copy_n(lane.vectors[0].begin(),4,output.begin()+offset);
            else for(unsigned i=0;i<4;++i)output[offset+i]=lane.integer[0]>>(8*i);
        }
        Raise(frame,exceptions);
        if(ins.encoding_family!=X86_INSTRUCTION_ENCODING_LEGACY && size==16)std::fill(output.begin()+16,output.end(),0);
        frame.vectors[*target]=output;return VectorSemanticResult{VectorStop::Continue,ins.address+ins.length};
    }
    const bool widen=m==X86_MNEMONIC_CVTSS2SD || m==X86_MNEMONIC_VCVTSS2SD;
    const bool narrow=m==X86_MNEMONIC_CVTSD2SS || m==X86_MNEMONIC_VCVTSD2SS;
    if(widen || narrow) {
        const auto unsupported=VectorSemanticResult{VectorStop::Unsupported,ins.address};
        const bool legacy=ins.encoding_family==X86_INSTRUCTION_ENCODING_LEGACY;
        const unsigned right=legacy?1:2;
        if(ins.operand_count!=right+1)return unsupported;
        const auto destination=Xmm(ins.operands[0]),merge=Xmm(ins.operands[legacy?0:1]);
        if(!destination || !merge)return unsupported;
        const unsigned fromFraction=widen?23:52,toFraction=widen?52:23;
        const unsigned fromBias=widen?127:1023,toBias=widen?1023:127;
        const unsigned fromMax=widen?255:2047,toMax=widen?2047:255;
        const unsigned sourceBytes=widen?4:8,targetBytes=widen?8:4;
        std::uint64_t bits=0;
        const auto& source=ins.operands[right];
        if(auto index=Xmm(source))for(unsigned i=0;i<sourceBytes;++i)bits|=std::uint64_t(frame.vectors[*index][i])<<(8*i);
        else {
            const auto address=ResolveGuestAddress(ins,source,frame);if(!address)return unsupported;
            std::array<std::uint8_t,8> bytes{};
            if(!memory.read || !memory.read(*address,std::span(bytes).first(sourceBytes),memory.context))return VectorSemanticResult{VectorStop::MemoryFault,ins.address,*address};
            for(unsigned i=0;i<sourceBytes;++i)bits|=std::uint64_t(bytes[i])<<(8*i);
        }
        const bool negative=bits&(1ull<<(sourceBytes*8-1));
        const auto rawExponent=(bits>>fromFraction)&fromMax;
        auto significand=bits&((1ull<<fromFraction)-1);
        const auto sign=std::uint64_t(negative)<<(targetBytes*8-1);
        unsigned exceptions=0;std::uint64_t result=sign;
        if(rawExponent==fromMax) {
            result|=std::uint64_t(toMax)<<toFraction;
            if(significand) {
                if(!(significand&(1ull<<(fromFraction-1))))exceptions|=1;
                const auto payload=widen?significand<<(toFraction-fromFraction):significand>>(fromFraction-toFraction);
                result|=payload|(1ull<<(toFraction-1)); // quiet NaN, preserving payload/sign
            }
        } else {
            if(rawExponent)significand|=1ull<<fromFraction;
            else if(significand) {if(frame.simd_control&64)significand=0;else exceptions|=2;}
            if(significand) {
                const unsigned top=63-std::countl_zero(significand);
                const int exponent=int(rawExponent?rawExponent:1)-int(fromBias)-int(fromFraction)+int(top);
                int destinationExponent=exponent+int(toBias);
                const int shift=int(top)-int(toFraction)+std::max(0,1-destinationExponent);
                std::uint64_t rounded=significand,remainder=0;
                const unsigned rounding=(frame.simd_control>>13)&3;
                if(shift>0) {
                    rounded=shift>=64?0:significand>>shift;
                    remainder=shift>=64?significand:significand&((1ull<<shift)-1);
                    if(RoundUp(rounded,remainder,shift,negative,rounding))++rounded;
                } else rounded<<=-shift;
                if(destinationExponent<=0) {
                    if(rounded>=(1ull<<toFraction))destinationExponent=1;
                    else {
                        destinationExponent=0;
                        if(remainder)exceptions|=16;
                        if(rounded && (frame.simd_control&0x8000)){rounded=0;exceptions|=16|32;}
                    }
                } else if(rounded>=(1ull<<(toFraction+1))){rounded>>=1;++destinationExponent;}
                if(remainder)exceptions|=32;
                if(destinationExponent>=int(toMax)) {
                    exceptions|=8|32;
                    const bool infinity=rounding==0 || (rounding==1 && negative) || (rounding==2 && !negative);
                    result|=infinity?std::uint64_t(toMax)<<toFraction:(std::uint64_t(toMax)<<toFraction)-1;
                } else result|=(std::uint64_t(destinationExponent)<<toFraction)|(rounded&((1ull<<toFraction)-1));
            }
        }
        Raise(frame,exceptions);
        auto value=frame.vectors[*merge];if(!legacy)std::fill(value.begin()+16,value.end(),0);
        for(unsigned i=0;i<targetBytes;++i)value[i]=result>>(8*i);
        frame.vectors[*destination]=value;
        return VectorSemanticResult{VectorStop::Continue,ins.address+ins.length};
    }
    const bool toFloat=m==X86_MNEMONIC_CVTSI2SS || m==X86_MNEMONIC_CVTSI2SD || m==X86_MNEMONIC_VCVTSI2SS || m==X86_MNEMONIC_VCVTSI2SD;
    const bool toInt=m==X86_MNEMONIC_CVTSS2SI || m==X86_MNEMONIC_CVTSD2SI || m==X86_MNEMONIC_VCVTSS2SI || m==X86_MNEMONIC_VCVTSD2SI ||
        m==X86_MNEMONIC_CVTTSS2SI || m==X86_MNEMONIC_CVTTSD2SI || m==X86_MNEMONIC_VCVTTSS2SI || m==X86_MNEMONIC_VCVTTSD2SI;
    if(!toFloat && !toInt)return std::nullopt;
    const auto unsupported=VectorSemanticResult{VectorStop::Unsupported,ins.address};
    const bool single=m==X86_MNEMONIC_CVTSI2SS || m==X86_MNEMONIC_VCVTSI2SS ||
        m==X86_MNEMONIC_CVTSS2SI || m==X86_MNEMONIC_VCVTSS2SI || m==X86_MNEMONIC_CVTTSS2SI || m==X86_MNEMONIC_VCVTTSS2SI;
    const bool trunc=m==X86_MNEMONIC_CVTTSS2SI || m==X86_MNEMONIC_CVTTSD2SI || m==X86_MNEMONIC_VCVTTSS2SI || m==X86_MNEMONIC_VCVTTSD2SI;
    const bool legacy=ins.encoding_family==X86_INSTRUCTION_ENCODING_LEGACY;
    const unsigned right=toFloat&&!legacy?2:1;
    if(ins.operand_count!=right+1)return unsupported;
    const auto& source=ins.operands[right];const auto& destination=ins.operands[0];
    const unsigned width=toFloat?source.bit_width:destination.bit_width;
    if(width!=32 && width!=64)return unsupported;
    const unsigned size=toFloat?width/8:single?4:8;
    std::uint64_t bits=0;
    if(source.form==OperandForm::Memory) {
        const auto address=ResolveGuestAddress(ins,source,frame);if(!address)return unsupported;
        std::array<std::uint8_t,8> buffer{};
        if(!memory.read || !memory.read(*address,std::span(buffer).first(size),memory.context))return VectorSemanticResult{VectorStop::MemoryFault,ins.address,*address};
        for(unsigned i=0;i<size;++i)bits|=std::uint64_t(buffer[i])<<(8*i);
    } else if(toFloat) {
        auto access=DecodeIntegerRegister(source);if(!access)return unsupported;
        bits=ReadInteger(frame,static_cast<IntegerRegister>(access->slot),width);
    } else {
        auto index=Xmm(source);if(!index)return unsupported;
        for(unsigned i=0;i<size;++i)bits|=std::uint64_t(frame.vectors[*index][i])<<(8*i);
    }
    const unsigned fraction=single?23:52,bias=single?127:1023;
    unsigned exceptions=0;
    std::uint64_t result=0;
    if(toFloat) {
        const auto target=Xmm(destination),merge=Xmm(ins.operands[legacy?0:1]);
        if(!target || !merge)return unsupported;
        const bool negative=bits&(1ull<<(width-1));
        const auto magnitude=negative?(0-bits)&WidthMask(width):bits;
        if(magnitude) {
            unsigned exponent=63-std::countl_zero(magnitude);
            auto significand=magnitude;
            if(exponent>fraction) {
                const unsigned shift=exponent-fraction;
                const auto remainder=magnitude&((1ull<<shift)-1);
                significand=magnitude>>shift;
                if(remainder)exceptions|=32;
                if(RoundUp(significand,remainder,shift,negative,(frame.simd_control>>13)&3))++significand;
                if(significand==(1ull<<(fraction+1))){significand>>=1;++exponent;}
            } else significand<<=fraction-exponent;
            result=(std::uint64_t(negative)<<(single?31:63))|(std::uint64_t(exponent+bias)<<fraction)|(significand&((1ull<<fraction)-1));
        }
        Raise(frame,exceptions);
        auto value=frame.vectors[*merge];
        if(!legacy)std::fill(value.begin()+16,value.end(),0);
        for(unsigned i=0;i<(single?4:8);++i)value[i]=result>>(8*i);
        frame.vectors[*target]=value;
    } else {
        const auto target=DecodeIntegerRegister(destination);if(!target)return unsupported;
        const bool negative=bits&(1ull<<(single?31:63));
        const unsigned rawExponent=(bits>>fraction)&(single?255:2047);
        auto significand=bits&((1ull<<fraction)-1);
        bool invalid=rawExponent==(single?255:2047);
        if(rawExponent)significand|=1ull<<fraction;
        else if(frame.simd_control&64)significand=0; // DAZ
        const int shift=int(rawExponent?rawExponent:1)-int(bias)-int(fraction);
        unsigned __int128 magnitude=0;
        if(!invalid && significand) {
            if(shift>=0) {
                if(shift>64)invalid=true;
                else magnitude=static_cast<unsigned __int128>(significand)<<shift;
            } else {
                const unsigned discarded=-shift;
                auto integer=discarded>=64?0:significand>>discarded;
                const auto remainder=discarded>=64?significand:significand&((1ull<<discarded)-1);
                if(remainder)exceptions|=32;
                if(RoundUp(integer,remainder,discarded,negative,trunc?3:(frame.simd_control>>13)&3))++integer;
                magnitude=integer;
            }
        }
        const auto maximum=(static_cast<unsigned __int128>(1)<<(width-1))-(negative?0:1);
        if(magnitude>maximum)invalid=true;
        if(invalid){exceptions=1;result=1ull<<(width-1);}
        else {result=static_cast<std::uint64_t>(magnitude);if(negative)result=0-result;}
        Raise(frame,exceptions);
        WriteInteger(frame,static_cast<IntegerRegister>(target->slot),result,width);
    }
    return VectorSemanticResult{VectorStop::Continue,ins.address+ins.length};
}
}
