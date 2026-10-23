// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestVector.h"
#include "GuestFloatConversion.h"
#include "GuestPackedVector.h"
#include "common/x86_decoder.h"
#include "executor/dynamic_translation/register_access.h"
#include "executor/dynamic_translation/address_resolver.h"
#include <algorithm>
#include <bit>
#include "executor/dynamic_translation/vector_semantics.h"

namespace Lsx4::Apple {
// Floating-point bitwise opcodes have exactly the same bit semantics as
// packed integer logical operations. Preserve the encoding family, operands
// and widths so shared LSX semantics also preserve/clear upper lanes correctly.
Translation::VectorSemanticResult ExecuteGuestVector(
    const Translation::Instruction& instruction,Translation::CpuFrame& frame,
    const Translation::GuestMemoryPort& memory) {
    if(auto packed=ExecutePackedVector(instruction,frame,memory))return *packed;
    if(auto conversion=ExecuteFloatConversion(instruction,frame,memory))return *conversion;
    if(instruction.mnemonic==X86_MNEMONIC_VZEROUPPER || instruction.mnemonic==X86_MNEMONIC_VZEROALL) {
        const unsigned first=instruction.mnemonic==X86_MNEMONIC_VZEROUPPER?16:0;
        for(auto& value:frame.vectors) std::fill(value.begin()+first,value.end(),0);
        return {Translation::VectorStop::Continue,instruction.address+instruction.length};
    }
    bool vectorOperand=false;
    for(unsigned i=0;i<instruction.operand_count;++i) {
        const auto& operand=instruction.operands[i];
        if(operand.form==Translation::OperandForm::Register &&
           ((operand.register_id>=X86_REGISTER_XMM0 && operand.register_id<=X86_REGISTER_XMM15) ||
            (operand.register_id>=X86_REGISTER_YMM0 && operand.register_id<=X86_REGISTER_YMM15))) vectorOperand=true;
    }
    if(!vectorOperand && instruction.mnemonic!=X86_MNEMONIC_LDMXCSR &&
       instruction.mnemonic!=X86_MNEMONIC_STMXCSR)
        return {Translation::VectorStop::Unsupported,instruction.address,0};
    const bool duplicateLow=instruction.mnemonic==X86_MNEMONIC_MOVSLDUP || instruction.mnemonic==X86_MNEMONIC_VMOVSLDUP;
    const bool duplicateHigh=instruction.mnemonic==X86_MNEMONIC_MOVSHDUP || instruction.mnemonic==X86_MNEMONIC_VMOVSHDUP;
    const bool duplicateDouble=instruction.mnemonic==X86_MNEMONIC_MOVDDUP || instruction.mnemonic==X86_MNEMONIC_VMOVDDUP;
    if(duplicateLow || duplicateHigh || duplicateDouble) {
        using namespace Translation;
        const bool legacy=instruction.encoding_family==X86_INSTRUCTION_ENCODING_LEGACY;
        if(instruction.operand_count!=2 || (!legacy && instruction.encoding_family!=X86_INSTRUCTION_ENCODING_VEX))
            return {VectorStop::Unsupported,instruction.address};
        const auto& destination=instruction.operands[0];const auto& source=instruction.operands[1];
        const unsigned size=destination.bit_width/8;
        if((size!=16 && size!=32) || (legacy && size!=16))return {VectorStop::Unsupported,instruction.address};
        auto slot=[](const Operand& operand)->std::optional<unsigned> {
            if(operand.form!=OperandForm::Register)return std::nullopt;
            if(operand.register_id>=X86_REGISTER_XMM0 && operand.register_id<=X86_REGISTER_XMM15)return operand.register_id-X86_REGISTER_XMM0;
            if(operand.register_id>=X86_REGISTER_YMM0 && operand.register_id<=X86_REGISTER_YMM15)return operand.register_id-X86_REGISTER_YMM0;
            return std::nullopt;
        };
        const auto target=slot(destination);if(!target)return {VectorStop::Unsupported,instruction.address};
        std::array<std::uint8_t,32> input{};
        if(auto index=slot(source))input=frame.vectors[*index];
        else if(source.form==OperandForm::Memory) {
            const auto address=ResolveGuestAddress(instruction,source,frame);if(!address)return {VectorStop::Unsupported,instruction.address};
            const unsigned bytes=duplicateDouble && size==16?8:size;
            if(!memory.read || !memory.read(*address,std::span(input).first(bytes),memory.context))return {VectorStop::MemoryFault,instruction.address,*address};
        } else return {VectorStop::Unsupported,instruction.address};
        auto output=frame.vectors[*target];
        if(duplicateDouble) {
            for(unsigned i=0;i<size;++i)output[i]=input[(i/16)*16+i%8];
        } else {
            for(unsigned i=0;i<size;++i)output[i]=input[(i/8)*8+(duplicateHigh?4:0)+i%4];
        }
        if(!legacy && size==16)std::fill(output.begin()+16,output.end(),0);
        frame.vectors[*target]=output;return {VectorStop::Continue,instruction.address+instruction.length};
    }
    unsigned unpackBytes=0;bool unpackHigh=false;
    switch(instruction.mnemonic) {
    case X86_MNEMONIC_PUNPCKHBW:case X86_MNEMONIC_VPUNPCKHBW:unpackHigh=true;[[fallthrough]];
    case X86_MNEMONIC_PUNPCKLBW:case X86_MNEMONIC_VPUNPCKLBW:unpackBytes=1;break;
    case X86_MNEMONIC_PUNPCKHWD:case X86_MNEMONIC_VPUNPCKHWD:unpackHigh=true;[[fallthrough]];
    case X86_MNEMONIC_PUNPCKLWD:case X86_MNEMONIC_VPUNPCKLWD:unpackBytes=2;break;
    default:break;
    }
    if(unpackBytes) {
        using namespace Translation;
        const bool legacy=instruction.encoding_family==X86_INSTRUCTION_ENCODING_LEGACY;
        const unsigned rightIndex=legacy?1:2, size=instruction.operands[0].bit_width/8;
        if(!instruction.length || (instruction.prefix_attributes&X86_ATTRIB_HAS_LOCK) ||
           (!legacy && instruction.encoding_family!=X86_INSTRUCTION_ENCODING_VEX) ||
           instruction.operand_count!=rightIndex+1 || (size!=16 && size!=32) || (legacy && size!=16))
            return {VectorStop::Unsupported,instruction.address};
        auto slot=[&](const Operand& operand)->std::optional<unsigned> {
            const auto base=size==16?X86_REGISTER_XMM0:X86_REGISTER_YMM0;
            if(operand.form!=OperandForm::Register || operand.bit_width!=size*8 || operand.register_id<base || operand.register_id>base+15)return std::nullopt;
            return operand.register_id-base;
        };
        const auto destination=slot(instruction.operands[0]);
        const auto first=slot(instruction.operands[legacy?0:1]);
        if(!destination || !first)return {VectorStop::Unsupported,instruction.address};
        const auto left=frame.vectors[*first];std::array<std::uint8_t,32> right{};
        const auto& source=instruction.operands[rightIndex];
        if(const auto second=slot(source))right=frame.vectors[*second];
        else if(source.form==OperandForm::Memory && source.bit_width==size*8) {
            const auto address=ResolveGuestAddress(instruction,source,frame);
            if(!address || (legacy && *address%16))return {VectorStop::Unsupported,instruction.address};
            if(!memory.read || !memory.read(*address,std::span(right).first(size),memory.context))
                return {VectorStop::MemoryFault,instruction.address,*address};
        } else return {VectorStop::Unsupported,instruction.address};
        auto output=frame.vectors[*destination];
        if(!legacy)output.fill(0);
        // Intel SDM PUNPCKH/L: each 128-bit lane interleaves independently.
        // Snapshot both inputs before mutation, including aliased operands.
        for(unsigned lane=0;lane<size;lane+=16)for(unsigned pair=0;pair<8/unpackBytes;++pair) {
            const auto input=lane+(unpackHigh?8:0)+pair*unpackBytes;
            const auto target=lane+pair*2*unpackBytes;
            std::copy_n(left.begin()+input,unpackBytes,output.begin()+target);
            std::copy_n(right.begin()+input,unpackBytes,output.begin()+target+unpackBytes);
        }
        frame.vectors[*destination]=output;return {VectorStop::Continue,instruction.address+instruction.length};
    }
    unsigned narrowBytes=0,wideBytes=0;bool signWiden=false;
    switch(instruction.mnemonic) {
    case X86_MNEMONIC_PMOVSXBW:case X86_MNEMONIC_VPMOVSXBW:signWiden=true;[[fallthrough]];
    case X86_MNEMONIC_PMOVZXBW:case X86_MNEMONIC_VPMOVZXBW:narrowBytes=1;wideBytes=2;break;
    case X86_MNEMONIC_PMOVSXBD:case X86_MNEMONIC_VPMOVSXBD:signWiden=true;[[fallthrough]];
    case X86_MNEMONIC_PMOVZXBD:case X86_MNEMONIC_VPMOVZXBD:narrowBytes=1;wideBytes=4;break;
    case X86_MNEMONIC_PMOVSXBQ:case X86_MNEMONIC_VPMOVSXBQ:signWiden=true;[[fallthrough]];
    case X86_MNEMONIC_PMOVZXBQ:case X86_MNEMONIC_VPMOVZXBQ:narrowBytes=1;wideBytes=8;break;
    case X86_MNEMONIC_PMOVSXWD:case X86_MNEMONIC_VPMOVSXWD:signWiden=true;[[fallthrough]];
    case X86_MNEMONIC_PMOVZXWD:case X86_MNEMONIC_VPMOVZXWD:narrowBytes=2;wideBytes=4;break;
    case X86_MNEMONIC_PMOVSXWQ:case X86_MNEMONIC_VPMOVSXWQ:signWiden=true;[[fallthrough]];
    case X86_MNEMONIC_PMOVZXWQ:case X86_MNEMONIC_VPMOVZXWQ:narrowBytes=2;wideBytes=8;break;
    case X86_MNEMONIC_PMOVSXDQ:case X86_MNEMONIC_VPMOVSXDQ:signWiden=true;[[fallthrough]];
    case X86_MNEMONIC_PMOVZXDQ:case X86_MNEMONIC_VPMOVZXDQ:narrowBytes=4;wideBytes=8;break;
    default:break;
    }
    if(narrowBytes) {
        using namespace Translation;
        const bool legacy=instruction.encoding_family==X86_INSTRUCTION_ENCODING_LEGACY;
        if(instruction.operand_count!=2 || (!legacy && instruction.encoding_family!=X86_INSTRUCTION_ENCODING_VEX))
            return {VectorStop::Unsupported,instruction.address};
        const auto& destination=instruction.operands[0];const auto& source=instruction.operands[1];
        const unsigned outputBytes=destination.bit_width/8;
        if(destination.form!=OperandForm::Register || (outputBytes!=16 && outputBytes!=32) || (legacy && outputBytes!=16))
            return {VectorStop::Unsupported,instruction.address};
        const auto base=outputBytes==16?X86_REGISTER_XMM0:X86_REGISTER_YMM0;
        if(destination.register_id<base || destination.register_id>base+15)return {VectorStop::Unsupported,instruction.address};
        const auto index=destination.register_id-base;
        const unsigned inputBytes=outputBytes/wideBytes*narrowBytes;
        std::array<std::uint8_t,16> input{};
        if(source.form==OperandForm::Register && source.register_id>=X86_REGISTER_XMM0 && source.register_id<=X86_REGISTER_XMM15)
            std::copy_n(frame.vectors[source.register_id-X86_REGISTER_XMM0].begin(),inputBytes,input.begin());
        else if(source.form==OperandForm::Memory) {
            const auto address=ResolveGuestAddress(instruction,source,frame);
            if(!address)return {VectorStop::Unsupported,instruction.address};
            if(!memory.read || !memory.read(*address,std::span(input).first(inputBytes),memory.context))
                return {VectorStop::MemoryFault,instruction.address,*address};
        } else return {VectorStop::Unsupported,instruction.address};
        auto output=frame.vectors[index];
        if(!legacy)output.fill(0);
        for(unsigned i=0;i<outputBytes/wideBytes;++i) {
            const auto extension=signWiden && (input[i*narrowBytes+narrowBytes-1]&0x80)?0xff:0;
            for(unsigned byte=0;byte<wideBytes;++byte)
                output[i*wideBytes+byte]=byte<narrowBytes?input[i*narrowBytes+byte]:extension;
        }
        frame.vectors[index]=output;return {VectorStop::Continue,instruction.address+instruction.length};
    }
    if((instruction.mnemonic==X86_MNEMONIC_MOVSS || instruction.mnemonic==X86_MNEMONIC_MOVSD ||
        instruction.mnemonic==X86_MNEMONIC_VMOVSS || instruction.mnemonic==X86_MNEMONIC_VMOVSD) &&
        (instruction.operand_count==2 || instruction.operand_count==3)) {
        using namespace Translation;
        const unsigned size=instruction.mnemonic==X86_MNEMONIC_MOVSS || instruction.mnemonic==X86_MNEMONIC_VMOVSS?4:8;
        const bool legacy=instruction.encoding_family==X86_INSTRUCTION_ENCODING_LEGACY;
        auto xmm=[](const Operand& operand)->std::optional<unsigned> {
            if(operand.form==OperandForm::Register && operand.register_id>=X86_REGISTER_XMM0 && operand.register_id<=X86_REGISTER_XMM15)
                return operand.register_id-X86_REGISTER_XMM0;
            return std::nullopt;
        };
        const auto& destination=instruction.operands[0];
        const auto& source=instruction.operands[instruction.operand_count-1];
        std::array<std::uint8_t,8> low{};
        if(auto index=xmm(source))std::copy_n(frame.vectors[*index].begin(),size,low.begin());
        else {
            const auto address=ResolveGuestAddress(instruction,source,frame);
            if(!address)return {VectorStop::Unsupported,instruction.address};
            if(!memory.read || !memory.read(*address,std::span(low).first(size),memory.context))
                return {VectorStop::MemoryFault,instruction.address,*address};
        }
        if(auto index=xmm(destination)) {
            auto value=frame.vectors[*index];
            if(instruction.operand_count==3) {
                const auto merge=xmm(instruction.operands[1]);if(!merge)return {VectorStop::Unsupported,instruction.address};
                value=frame.vectors[*merge];
            } else if(source.form==OperandForm::Memory)std::fill_n(value.begin(),16,0);
            if(!legacy)std::fill(value.begin()+16,value.end(),0);
            std::copy_n(low.begin(),size,value.begin());frame.vectors[*index]=value;
        } else {
            const auto address=ResolveGuestAddress(instruction,destination,frame);
            if(!address)return {VectorStop::Unsupported,instruction.address};
            if(!memory.write || !memory.write(*address,std::span(low).first(size),memory.context))
                return {VectorStop::MemoryFault,instruction.address,*address};
        }
        return {VectorStop::Continue,instruction.address+instruction.length};
    }
    if(instruction.operand_count==2 &&
       (instruction.mnemonic==X86_MNEMONIC_MOVD || instruction.mnemonic==X86_MNEMONIC_VMOVD ||
        instruction.mnemonic==X86_MNEMONIC_MOVQ || instruction.mnemonic==X86_MNEMONIC_VMOVQ)) {
        using namespace Translation;
        const unsigned size=instruction.mnemonic==X86_MNEMONIC_MOVD || instruction.mnemonic==X86_MNEMONIC_VMOVD ? 4 : 8;
        auto xmm=[](const Operand& op)->std::optional<unsigned> {
            if(op.form==OperandForm::Register && op.register_id>=X86_REGISTER_XMM0 && op.register_id<=X86_REGISTER_XMM15)
                return op.register_id-X86_REGISTER_XMM0;
            return std::nullopt;
        };
        std::array<std::uint8_t,8> value{};
        const auto& source=instruction.operands[1];const auto& destination=instruction.operands[0];
        if(auto index=xmm(source)) std::copy_n(frame.vectors[*index].begin(),size,value.begin());
        else if(auto access=DecodeIntegerRegister(source)) {
            const auto bits=ReadInteger(frame,static_cast<IntegerRegister>(access->slot),size*8);
            for(unsigned i=0;i<size;++i) value[i]=bits>>(i*8);
        } else {
            const auto address=ResolveGuestAddress(instruction,source,frame);
            if(!address) return {VectorStop::Unsupported,instruction.address};
            if(!memory.read || !memory.read(*address,std::span(value).first(size),memory.context))
                return {VectorStop::MemoryFault,instruction.address,*address};
        }
        if(auto index=xmm(destination)) {
            const bool legacy=instruction.encoding_family==X86_INSTRUCTION_ENCODING_LEGACY;
            std::fill_n(frame.vectors[*index].begin(),legacy?16:32,0);
            std::copy_n(value.begin(),size,frame.vectors[*index].begin());
        } else if(auto access=DecodeIntegerRegister(destination)) {
            std::uint64_t bits=0;for(unsigned i=0;i<size;++i) bits|=std::uint64_t(value[i])<<(i*8);
            WriteInteger(frame,static_cast<IntegerRegister>(access->slot),bits,size*8);
        } else {
            const auto address=ResolveGuestAddress(instruction,destination,frame);
            if(!address) return {VectorStop::Unsupported,instruction.address};
            if(!memory.write || !memory.write(*address,std::span(value).first(size),memory.context))
                return {VectorStop::MemoryFault,instruction.address,*address};
        }
        return {VectorStop::Continue,instruction.address+instruction.length};
    }
    if(instruction.mnemonic==X86_MNEMONIC_INSERTPS || instruction.mnemonic==X86_MNEMONIC_VINSERTPS) {
        using namespace Translation;
        const bool legacy=instruction.encoding_family==X86_INSTRUCTION_ENCODING_LEGACY;
        const unsigned sourceIndex=legacy?1:2,immediateIndex=sourceIndex+1;
        if((!legacy && instruction.encoding_family!=X86_INSTRUCTION_ENCODING_VEX) ||
           instruction.operand_count!=immediateIndex+1 || instruction.operands[immediateIndex].form!=OperandForm::Immediate)
            return {VectorStop::Unsupported,instruction.address};
        auto xmm=[](const Operand& operand)->std::optional<unsigned> {
            if(operand.form==OperandForm::Register && operand.register_id>=X86_REGISTER_XMM0 && operand.register_id<=X86_REGISTER_XMM15)
                return operand.register_id-X86_REGISTER_XMM0;
            return std::nullopt;
        };
        const auto target=xmm(instruction.operands[0]),merge=xmm(instruction.operands[legacy?0:1]);
        if(!target || !merge)return {VectorStop::Unsupported,instruction.address};
        const auto control=instruction.operands[immediateIndex].immediate.bits;
        const auto& source=instruction.operands[sourceIndex];
        std::array<std::uint8_t,4> inserted{};
        if(auto index=xmm(source))std::copy_n(frame.vectors[*index].begin()+((control>>6)&3)*4,4,inserted.begin());
        else if(source.form==OperandForm::Memory) {
            const auto address=ResolveGuestAddress(instruction,source,frame);if(!address)return {VectorStop::Unsupported,instruction.address};
            if(!memory.read || !memory.read(*address,inserted,memory.context))return {VectorStop::MemoryFault,instruction.address,*address};
        } else return {VectorStop::Unsupported,instruction.address};
        auto output=frame.vectors[*merge];
        std::copy(inserted.begin(),inserted.end(),output.begin()+((control>>4)&3)*4);
        for(unsigned lane=0;lane<4;++lane)if(control&(1u<<lane))std::fill_n(output.begin()+lane*4,4,0);
        if(!legacy)std::fill(output.begin()+16,output.end(),0);
        frame.vectors[*target]=output;return {VectorStop::Continue,instruction.address+instruction.length};
    }
    const auto elementOp=instruction.mnemonic;
    unsigned elementBytes=0;bool insertElement=false;
    switch(elementOp) {
    case X86_MNEMONIC_PEXTRB:case X86_MNEMONIC_VPEXTRB:elementBytes=1;break;
    case X86_MNEMONIC_PEXTRW:case X86_MNEMONIC_VPEXTRW:elementBytes=2;break;
    case X86_MNEMONIC_PEXTRD:case X86_MNEMONIC_VPEXTRD:case X86_MNEMONIC_EXTRACTPS:case X86_MNEMONIC_VEXTRACTPS:elementBytes=4;break;
    case X86_MNEMONIC_PEXTRQ:case X86_MNEMONIC_VPEXTRQ:elementBytes=8;break;
    case X86_MNEMONIC_PINSRB:case X86_MNEMONIC_VPINSRB:elementBytes=1;insertElement=true;break;
    case X86_MNEMONIC_PINSRW:case X86_MNEMONIC_VPINSRW:elementBytes=2;insertElement=true;break;
    case X86_MNEMONIC_PINSRD:case X86_MNEMONIC_VPINSRD:elementBytes=4;insertElement=true;break;
    case X86_MNEMONIC_PINSRQ:case X86_MNEMONIC_VPINSRQ:elementBytes=8;insertElement=true;break;
    default:break;
    }
    if(elementBytes) {
        using namespace Translation;
        const bool legacy=instruction.encoding_family==X86_INSTRUCTION_ENCODING_LEGACY;
        const unsigned sourceIndex=insertElement&&!legacy?2:1,immediate=sourceIndex+1;
        if(instruction.operand_count!=immediate+1 || instruction.operands[immediate].form!=OperandForm::Immediate)
            return {VectorStop::Unsupported,instruction.address};
        const unsigned offset=(instruction.operands[immediate].immediate.bits&(16/elementBytes-1))*elementBytes;
        auto xmm=[](const Operand& op)->std::optional<unsigned> {
            if(op.form==OperandForm::Register && op.register_id>=X86_REGISTER_XMM0 && op.register_id<=X86_REGISTER_XMM15)return op.register_id-X86_REGISTER_XMM0;
            return std::nullopt;
        };
        const auto& source=instruction.operands[sourceIndex];const auto& destination=instruction.operands[0];
        std::array<std::uint8_t,8> bytes{};
        if(insertElement) {
            if(auto access=DecodeIntegerRegister(source)) {
                const auto value=ReadInteger(frame,static_cast<IntegerRegister>(access->slot),access->width);
                for(unsigned i=0;i<elementBytes;++i)bytes[i]=value>>(8*i);
            } else {
                const auto address=ResolveGuestAddress(instruction,source,frame);if(!address)return {VectorStop::Unsupported,instruction.address};
                if(!memory.read || !memory.read(*address,std::span(bytes).first(elementBytes),memory.context))return {VectorStop::MemoryFault,instruction.address,*address};
            }
            const auto target=xmm(destination),merge=xmm(instruction.operands[legacy?0:1]);
            if(!target || !merge)return {VectorStop::Unsupported,instruction.address};
            auto output=frame.vectors[*merge];std::copy_n(bytes.begin(),elementBytes,output.begin()+offset);
            if(!legacy)std::fill(output.begin()+16,output.end(),0);
            frame.vectors[*target]=output;
        } else {
            const auto index=xmm(source);if(!index)return {VectorStop::Unsupported,instruction.address};
            std::copy_n(frame.vectors[*index].begin()+offset,elementBytes,bytes.begin());
            if(auto access=DecodeIntegerRegister(destination)) {
                std::uint64_t value=0;for(unsigned i=0;i<elementBytes;++i)value|=std::uint64_t(bytes[i])<<(8*i);
                WriteInteger(frame,static_cast<IntegerRegister>(access->slot),value,access->width);
            } else {
                const auto address=ResolveGuestAddress(instruction,destination,frame);if(!address)return {VectorStop::Unsupported,instruction.address};
                if(!memory.write || !memory.write(*address,std::span(bytes).first(elementBytes),memory.context))return {VectorStop::MemoryFault,instruction.address,*address};
            }
        }
        return {VectorStop::Continue,instruction.address+instruction.length};
    }
    if(instruction.mnemonic==X86_MNEMONIC_VINSERTF128 || instruction.mnemonic==X86_MNEMONIC_VINSERTI128) {
        using namespace Translation;
        if(instruction.operand_count!=4)return {VectorStop::Unsupported,instruction.address};
        const auto& destination=instruction.operands[0];const auto& first=instruction.operands[1];const auto& source=instruction.operands[2];
        if(destination.register_id<X86_REGISTER_YMM0 || destination.register_id>X86_REGISTER_YMM15 ||
           first.register_id<X86_REGISTER_YMM0 || first.register_id>X86_REGISTER_YMM15 || instruction.operands[3].form!=OperandForm::Immediate)
            return {VectorStop::Unsupported,instruction.address};
        auto output=frame.vectors[first.register_id-X86_REGISTER_YMM0];
        std::array<std::uint8_t,16> inserted{};
        if(source.form==OperandForm::Register && source.register_id>=X86_REGISTER_XMM0 && source.register_id<=X86_REGISTER_XMM15)
            std::copy_n(frame.vectors[source.register_id-X86_REGISTER_XMM0].begin(),16,inserted.begin());
        else {
            const auto address=ResolveGuestAddress(instruction,source,frame);if(!address)return {VectorStop::Unsupported,instruction.address};
            if(!memory.read || !memory.read(*address,inserted,memory.context))return {VectorStop::MemoryFault,instruction.address,*address};
        }
        std::copy(inserted.begin(),inserted.end(),output.begin()+16*(instruction.operands[3].immediate.bits&1));
        frame.vectors[destination.register_id-X86_REGISTER_YMM0]=output;
        return {VectorStop::Continue,instruction.address+instruction.length};
    }
    if(instruction.mnemonic==X86_MNEMONIC_VEXTRACTF128 || instruction.mnemonic==X86_MNEMONIC_VEXTRACTI128) {
        using namespace Translation;
        if(instruction.operand_count!=3)return {VectorStop::Unsupported,instruction.address};
        const auto& destination=instruction.operands[0];const auto& source=instruction.operands[1];
        if(source.register_id<X86_REGISTER_YMM0 || source.register_id>X86_REGISTER_YMM15 || instruction.operands[2].form!=OperandForm::Immediate)
            return {VectorStop::Unsupported,instruction.address};
        std::array<std::uint8_t,16> extracted{};
        std::copy_n(frame.vectors[source.register_id-X86_REGISTER_YMM0].begin()+16*(instruction.operands[2].immediate.bits&1),16,extracted.begin());
        if(destination.form==OperandForm::Register && destination.register_id>=X86_REGISTER_XMM0 && destination.register_id<=X86_REGISTER_XMM15) {
            auto& output=frame.vectors[destination.register_id-X86_REGISTER_XMM0];output.fill(0);std::copy(extracted.begin(),extracted.end(),output.begin());
        } else {
            const auto address=ResolveGuestAddress(instruction,destination,frame);if(!address)return {VectorStop::Unsupported,instruction.address};
            if(!memory.write || !memory.write(*address,extracted,memory.context))return {VectorStop::MemoryFault,instruction.address,*address};
        }
        return {VectorStop::Continue,instruction.address+instruction.length};
    }
    unsigned minMaxBytes=0;bool signedCompare=false,maximum=false;
    switch(instruction.mnemonic) {
    case X86_MNEMONIC_PMINSB:case X86_MNEMONIC_VPMINSB:minMaxBytes=1;signedCompare=true;break;
    case X86_MNEMONIC_PMINUB:case X86_MNEMONIC_VPMINUB:minMaxBytes=1;break;
    case X86_MNEMONIC_PMINSW:case X86_MNEMONIC_VPMINSW:minMaxBytes=2;signedCompare=true;break;
    case X86_MNEMONIC_PMINUW:case X86_MNEMONIC_VPMINUW:minMaxBytes=2;break;
    case X86_MNEMONIC_PMINSD:case X86_MNEMONIC_VPMINSD:minMaxBytes=4;signedCompare=true;break;
    case X86_MNEMONIC_PMINUD:case X86_MNEMONIC_VPMINUD:minMaxBytes=4;break;
    case X86_MNEMONIC_PMAXSB:case X86_MNEMONIC_VPMAXSB:minMaxBytes=1;signedCompare=true;maximum=true;break;
    case X86_MNEMONIC_PMAXUB:case X86_MNEMONIC_VPMAXUB:minMaxBytes=1;maximum=true;break;
    case X86_MNEMONIC_PMAXSW:case X86_MNEMONIC_VPMAXSW:minMaxBytes=2;signedCompare=true;maximum=true;break;
    case X86_MNEMONIC_PMAXUW:case X86_MNEMONIC_VPMAXUW:minMaxBytes=2;maximum=true;break;
    case X86_MNEMONIC_PMAXSD:case X86_MNEMONIC_VPMAXSD:minMaxBytes=4;signedCompare=true;maximum=true;break;
    case X86_MNEMONIC_PMAXUD:case X86_MNEMONIC_VPMAXUD:minMaxBytes=4;maximum=true;break;
    default:break;
    }
    if(minMaxBytes) {
        using namespace Translation;
        const bool legacy=instruction.encoding_family==X86_INSTRUCTION_ENCODING_LEGACY;
        const unsigned left=legacy?0:1,right=legacy?1:2;
        if(instruction.operand_count!=right+1)return {VectorStop::Unsupported,instruction.address};
        auto vector=[](const Operand& op)->std::optional<unsigned> {
            if(op.form!=OperandForm::Register)return std::nullopt;
            if(op.register_id>=X86_REGISTER_XMM0 && op.register_id<=X86_REGISTER_XMM15)return op.register_id-X86_REGISTER_XMM0;
            if(op.register_id>=X86_REGISTER_YMM0 && op.register_id<=X86_REGISTER_YMM15)return op.register_id-X86_REGISTER_YMM0;
            return std::nullopt;
        };
        const auto destination=vector(instruction.operands[0]),first=vector(instruction.operands[left]);
        const unsigned size=instruction.operands[0].bit_width/8;
        if(!destination || !first || (size!=16 && size!=32))return {VectorStop::Unsupported,instruction.address};
        std::array<std::uint8_t,32> second{};
        const auto& source=instruction.operands[right];
        if(auto index=vector(source))second=frame.vectors[*index];
        else {
            const auto address=ResolveGuestAddress(instruction,source,frame);if(!address)return {VectorStop::Unsupported,instruction.address};
            if(!memory.read || !memory.read(*address,std::span(second).first(size),memory.context))return {VectorStop::MemoryFault,instruction.address,*address};
        }
        auto output=frame.vectors[*destination];
        for(unsigned offset=0;offset<size;offset+=minMaxBytes) {
            std::uint64_t a=0,b=0;
            for(unsigned i=0;i<minMaxBytes;++i){a|=std::uint64_t(frame.vectors[*first][offset+i])<<(8*i);b|=std::uint64_t(second[offset+i])<<(8*i);}
            const bool less=signedCompare?std::bit_cast<std::int64_t>(SignExtend(a,minMaxBytes*8))<std::bit_cast<std::int64_t>(SignExtend(b,minMaxBytes*8)):a<b;
            const auto value=less!=maximum?a:b;
            for(unsigned i=0;i<minMaxBytes;++i)output[offset+i]=value>>(8*i);
        }
        if(!legacy && size==16)std::fill(output.begin()+16,output.end(),0);
        frame.vectors[*destination]=output;return {VectorStop::Continue,instruction.address+instruction.length};
    }
    const auto multiply=instruction.mnemonic;
    if(multiply==X86_MNEMONIC_PMULLD || multiply==X86_MNEMONIC_VPMULLD ||
       multiply==X86_MNEMONIC_PMULLW || multiply==X86_MNEMONIC_VPMULLW ||
       multiply==X86_MNEMONIC_PMULHW || multiply==X86_MNEMONIC_VPMULHW ||
       multiply==X86_MNEMONIC_PMULHUW || multiply==X86_MNEMONIC_VPMULHUW ||
       multiply==X86_MNEMONIC_PMULDQ || multiply==X86_MNEMONIC_VPMULDQ ||
       multiply==X86_MNEMONIC_PMULUDQ || multiply==X86_MNEMONIC_VPMULUDQ) {
        using namespace Translation;
        const bool legacy=instruction.encoding_family==X86_INSTRUCTION_ENCODING_LEGACY;
        const unsigned left=legacy?0:1,right=legacy?1:2;
        if(instruction.operand_count!=right+1)return {VectorStop::Unsupported,instruction.address};
        const bool wide=multiply==X86_MNEMONIC_PMULDQ || multiply==X86_MNEMONIC_VPMULDQ || multiply==X86_MNEMONIC_PMULUDQ || multiply==X86_MNEMONIC_VPMULUDQ;
        const bool dword=wide || multiply==X86_MNEMONIC_PMULLD || multiply==X86_MNEMONIC_VPMULLD;
        const bool high=multiply==X86_MNEMONIC_PMULHW || multiply==X86_MNEMONIC_VPMULHW || multiply==X86_MNEMONIC_PMULHUW || multiply==X86_MNEMONIC_VPMULHUW;
        const bool signedMultiply=multiply==X86_MNEMONIC_PMULHW || multiply==X86_MNEMONIC_VPMULHW || multiply==X86_MNEMONIC_PMULDQ || multiply==X86_MNEMONIC_VPMULDQ;
        const unsigned sourceBytes=dword?4:2,lane=wide?8:sourceBytes;
        auto vector=[](const Operand& op)->std::optional<unsigned> {
            if(op.form!=OperandForm::Register)return std::nullopt;
            if(op.register_id>=X86_REGISTER_XMM0 && op.register_id<=X86_REGISTER_XMM15)return op.register_id-X86_REGISTER_XMM0;
            if(op.register_id>=X86_REGISTER_YMM0 && op.register_id<=X86_REGISTER_YMM15)return op.register_id-X86_REGISTER_YMM0;
            return std::nullopt;
        };
        const auto destination=vector(instruction.operands[0]),first=vector(instruction.operands[left]);
        const unsigned size=instruction.operands[0].bit_width/8;
        if(!destination || !first || (size!=16 && size!=32))return {VectorStop::Unsupported,instruction.address};
        std::array<std::uint8_t,32> second{};
        const auto& source=instruction.operands[right];
        if(auto index=vector(source))second=frame.vectors[*index];
        else {
            const auto address=ResolveGuestAddress(instruction,source,frame);if(!address)return {VectorStop::Unsupported,instruction.address};
            if(!memory.read || !memory.read(*address,std::span(second).first(size),memory.context))return {VectorStop::MemoryFault,instruction.address,*address};
        }
        auto output=frame.vectors[*destination];
        for(unsigned offset=0;offset<size;offset+=lane) {
            std::uint64_t a=0,b=0;
            for(unsigned i=0;i<sourceBytes;++i){a|=std::uint64_t(frame.vectors[*first][offset+i])<<(i*8);b|=std::uint64_t(second[offset+i])<<(i*8);}
            const auto product=signedMultiply?static_cast<std::uint64_t>(std::bit_cast<std::int64_t>(SignExtend(a,sourceBytes*8))*std::bit_cast<std::int64_t>(SignExtend(b,sourceBytes*8))):a*b;
            const auto value=high?product>>16:product;
            for(unsigned i=0;i<lane;++i)output[offset+i]=value>>(8*i);
        }
        if(!legacy && size==16)std::fill(output.begin()+16,output.end(),0);
        frame.vectors[*destination]=output;return {VectorStop::Continue,instruction.address+instruction.length};
    }
    if(instruction.mnemonic==X86_MNEMONIC_CMPPS || instruction.mnemonic==X86_MNEMONIC_VCMPPS ||
       instruction.mnemonic==X86_MNEMONIC_CMPPD || instruction.mnemonic==X86_MNEMONIC_VCMPPD ||
       instruction.mnemonic==X86_MNEMONIC_CMPSS || instruction.mnemonic==X86_MNEMONIC_VCMPSS ||
       instruction.mnemonic==X86_MNEMONIC_CMPSD || instruction.mnemonic==X86_MNEMONIC_VCMPSD) {
        using namespace Translation;
        const auto m=instruction.mnemonic;
        const bool legacy=instruction.encoding_family==X86_INSTRUCTION_ENCODING_LEGACY;
        const bool scalar=m==X86_MNEMONIC_CMPSS || m==X86_MNEMONIC_VCMPSS || m==X86_MNEMONIC_CMPSD || m==X86_MNEMONIC_VCMPSD;
        const bool single=m==X86_MNEMONIC_CMPPS || m==X86_MNEMONIC_VCMPPS || m==X86_MNEMONIC_CMPSS || m==X86_MNEMONIC_VCMPSS;
        const unsigned left=legacy?0:1,right=legacy?1:2,immediate=right+1,lane=single?4:8;
        if(instruction.operand_count!=immediate+1 || instruction.operands[immediate].form!=OperandForm::Immediate)
            return {VectorStop::Unsupported,instruction.address};
        auto vector=[](const Operand& op)->std::optional<unsigned> {
            if(op.form!=OperandForm::Register)return std::nullopt;
            if(op.register_id>=X86_REGISTER_XMM0 && op.register_id<=X86_REGISTER_XMM15)return op.register_id-X86_REGISTER_XMM0;
            if(op.register_id>=X86_REGISTER_YMM0 && op.register_id<=X86_REGISTER_YMM15)return op.register_id-X86_REGISTER_YMM0;
            return std::nullopt;
        };
        const auto destination=vector(instruction.operands[0]),first=vector(instruction.operands[left]);
        const unsigned width=instruction.operands[0].bit_width/8,size=scalar?lane:width;
        if(!destination || !first || (width!=16 && width!=32))return {VectorStop::Unsupported,instruction.address};
        const unsigned predicate=instruction.operands[immediate].immediate.bits&(legacy?7:31);
        const bool signaling=bool((0x6666>>(predicate&15))&1)!=(predicate>=16);
        std::array<std::uint8_t,32> second{};
        const auto& source=instruction.operands[right];
        if(auto index=vector(source))second=frame.vectors[*index];
        else {
            const auto address=ResolveGuestAddress(instruction,source,frame);if(!address)return {VectorStop::Unsupported,instruction.address};
            if(!memory.read || !memory.read(*address,std::span(second).first(size),memory.context))return {VectorStop::MemoryFault,instruction.address,*address};
        }
        auto output=frame.vectors[*first];unsigned exceptions=0;
        for(unsigned offset=0;offset<size;offset+=lane) {
            bool unordered=false;
            auto decode=[&](const auto& bytes) {
                std::uint64_t bits=0;for(unsigned i=0;i<lane;++i)bits|=std::uint64_t(bytes[offset+i])<<(8*i);
                const unsigned fraction=single?23:52;const auto expMask=single?255:2047;
                const auto exponent=(bits>>fraction)&expMask,fractionBits=bits&((1ull<<fraction)-1);
                if(exponent==expMask && fractionBits) {
                    unordered=true;if(signaling || !(fractionBits&(1ull<<(fraction-1))))exceptions|=1;
                    return 0.0;
                }
                if(!exponent && fractionBits) {
                    if(frame.simd_control&64)bits&=1ull<<(lane*8-1);
                    else exceptions|=2;
                }
                return single?double(std::bit_cast<float>(std::uint32_t(bits))):std::bit_cast<double>(bits);
            };
            const double a=decode(frame.vectors[*first]),b=decode(second);
            bool value=false;
            switch(predicate&15) {
            case 0:value=!unordered && a==b;break;case 1:value=!unordered && a<b;break;
            case 2:value=!unordered && a<=b;break;case 3:value=unordered;break;
            case 4:value=unordered || a!=b;break;case 5:value=unordered || !(a<b);break;
            case 6:value=unordered || !(a<=b);break;case 7:value=!unordered;break;
            case 8:value=unordered || a==b;break;case 9:value=unordered || !(a>=b);break;
            case 10:value=unordered || !(a>b);break;case 11:value=false;break;
            case 12:value=!unordered && a!=b;break;case 13:value=!unordered && a>=b;break;
            case 14:value=!unordered && a>b;break;case 15:value=true;break;
            }
            std::fill_n(output.begin()+offset,lane,value?0xff:0);
        }
        frame.simd_control|=exceptions;
        if(exceptions&~(frame.simd_control>>7)&0x3f)throw GuestSIMDFault();
        if(!legacy && width==16)std::fill(output.begin()+16,output.end(),0);
        frame.vectors[*destination]=output;return {VectorStop::Continue,instruction.address+instruction.length};
    }
    const bool immediateBlend=instruction.mnemonic==X86_MNEMONIC_PBLENDW || instruction.mnemonic==X86_MNEMONIC_VPBLENDW ||
        instruction.mnemonic==X86_MNEMONIC_BLENDPS || instruction.mnemonic==X86_MNEMONIC_VBLENDPS ||
        instruction.mnemonic==X86_MNEMONIC_BLENDPD || instruction.mnemonic==X86_MNEMONIC_VBLENDPD || instruction.mnemonic==X86_MNEMONIC_VPBLENDD;
    if(immediateBlend || instruction.mnemonic==X86_MNEMONIC_VBLENDVPS || instruction.mnemonic==X86_MNEMONIC_VBLENDVPD ||
       instruction.mnemonic==X86_MNEMONIC_VPBLENDVB || instruction.mnemonic==X86_MNEMONIC_BLENDVPS ||
       instruction.mnemonic==X86_MNEMONIC_BLENDVPD || instruction.mnemonic==X86_MNEMONIC_PBLENDVB) {
        using namespace Translation;
        const bool legacy=instruction.encoding_family==X86_INSTRUCTION_ENCODING_LEGACY;
        const unsigned left=legacy?0:1,right=legacy?1:2;
        if(instruction.operand_count<(legacy?2:4))return {VectorStop::Unsupported,instruction.address};
        const unsigned lane=instruction.mnemonic==X86_MNEMONIC_PBLENDW || instruction.mnemonic==X86_MNEMONIC_VPBLENDW?2:
            instruction.mnemonic==X86_MNEMONIC_VPBLENDVB || instruction.mnemonic==X86_MNEMONIC_PBLENDVB?1:
            instruction.mnemonic==X86_MNEMONIC_VBLENDVPD || instruction.mnemonic==X86_MNEMONIC_BLENDVPD || instruction.mnemonic==X86_MNEMONIC_BLENDPD || instruction.mnemonic==X86_MNEMONIC_VBLENDPD?8:4;
        auto vector=[](const Operand& op)->std::optional<unsigned> {
            if(op.form!=OperandForm::Register)return std::nullopt;
            if(op.register_id>=X86_REGISTER_XMM0 && op.register_id<=X86_REGISTER_XMM15)return op.register_id-X86_REGISTER_XMM0;
            if(op.register_id>=X86_REGISTER_YMM0 && op.register_id<=X86_REGISTER_YMM15)return op.register_id-X86_REGISTER_YMM0;
            return std::nullopt;
        };
        const auto destination=vector(instruction.operands[0]),first=vector(instruction.operands[left]);
        const auto mask=legacy || immediateBlend?std::optional<unsigned>(0):vector(instruction.operands[3]);
        const auto selection=immediateBlend?instruction.operands[right+1].immediate.bits:0;
        if(immediateBlend && (instruction.operand_count!=right+2 || instruction.operands[right+1].form!=OperandForm::Immediate))return {VectorStop::Unsupported,instruction.address};
        const unsigned size=instruction.operands[0].bit_width/8;
        if(!destination || !first || !mask || (size!=16 && size!=32))return {VectorStop::Unsupported,instruction.address};
        std::array<std::uint8_t,32> second{};
        const auto& source=instruction.operands[right];
        if(auto index=vector(source))second=frame.vectors[*index];
        else {
            const auto address=ResolveGuestAddress(instruction,source,frame);if(!address)return {VectorStop::Unsupported,instruction.address};
            if(!memory.read || !memory.read(*address,std::span(second).first(size),memory.context))return {VectorStop::MemoryFault,instruction.address,*address};
        }
        auto output=frame.vectors[*destination];
        for(unsigned offset=0;offset<size;offset+=lane) {
            const bool choose=immediateBlend?bool((selection>>((offset/lane)&7))&1):bool(frame.vectors[*mask][offset+lane-1]&0x80);
            const auto& value=choose?second:frame.vectors[*first];
            std::copy_n(value.begin()+offset,lane,output.begin()+offset);
        }
        if(!legacy && size==16)std::fill(output.begin()+16,output.end(),0);
        frame.vectors[*destination]=output;return {VectorStop::Continue,instruction.address+instruction.length};
    }
    unsigned broadcastBytes=0;
    switch(instruction.mnemonic) {
    case X86_MNEMONIC_VBROADCASTSS:case X86_MNEMONIC_VPBROADCASTD:broadcastBytes=4;break;
    case X86_MNEMONIC_VBROADCASTSD:case X86_MNEMONIC_VPBROADCASTQ:broadcastBytes=8;break;
    case X86_MNEMONIC_VPBROADCASTB:broadcastBytes=1;break;
    case X86_MNEMONIC_VPBROADCASTW:broadcastBytes=2;break;
    case X86_MNEMONIC_VBROADCASTF128:case X86_MNEMONIC_VBROADCASTI128:broadcastBytes=16;break;
    default:break;
    }
    if(broadcastBytes && instruction.operand_count==2) {
        using namespace Translation;
        const auto& destination=instruction.operands[0];const auto& source=instruction.operands[1];
        unsigned slot;
        if(destination.form!=OperandForm::Register)return {VectorStop::Unsupported,instruction.address};
        if(destination.register_id>=X86_REGISTER_XMM0 && destination.register_id<=X86_REGISTER_XMM15)slot=destination.register_id-X86_REGISTER_XMM0;
        else if(destination.register_id>=X86_REGISTER_YMM0 && destination.register_id<=X86_REGISTER_YMM15)slot=destination.register_id-X86_REGISTER_YMM0;
        else return {VectorStop::Unsupported,instruction.address};
        const unsigned size=destination.bit_width/8;
        if(size!=16 && size!=32)return {VectorStop::Unsupported,instruction.address};
        std::array<std::uint8_t,16> input{};
        if(source.form==OperandForm::Register && source.register_id>=X86_REGISTER_XMM0 && source.register_id<=X86_REGISTER_XMM15)
            std::copy_n(frame.vectors[source.register_id-X86_REGISTER_XMM0].begin(),broadcastBytes,input.begin());
        else {
            const auto address=ResolveGuestAddress(instruction,source,frame);if(!address)return {VectorStop::Unsupported,instruction.address};
            if(!memory.read || !memory.read(*address,std::span(input).first(broadcastBytes),memory.context))
                return {VectorStop::MemoryFault,instruction.address,*address};
        }
        std::array<std::uint8_t,32> value{};
        for(unsigned offset=0;offset<size;offset+=broadcastBytes)std::copy_n(input.begin(),broadcastBytes,value.begin()+offset);
        frame.vectors[slot]=value;return {VectorStop::Continue,instruction.address+instruction.length};
    }
    // Packed arithmetic is composed from the existing LSX scalar floating
    // operations. Snapshot both operands before writing an aliased destination.
    std::uint32_t scalar=0;unsigned scalarBytes=4;
    switch(instruction.mnemonic) {
    case X86_MNEMONIC_ADDPS:case X86_MNEMONIC_VADDPS:scalar=X86_MNEMONIC_ADDSS;break;
    case X86_MNEMONIC_ADDPD:case X86_MNEMONIC_VADDPD:scalar=X86_MNEMONIC_ADDSD;scalarBytes=8;break;
    case X86_MNEMONIC_SUBPS:case X86_MNEMONIC_VSUBPS:scalar=X86_MNEMONIC_SUBSS;break;
    case X86_MNEMONIC_SUBPD:case X86_MNEMONIC_VSUBPD:scalar=X86_MNEMONIC_SUBSD;scalarBytes=8;break;
    case X86_MNEMONIC_MULPS:case X86_MNEMONIC_VMULPS:scalar=X86_MNEMONIC_MULSS;break;
    case X86_MNEMONIC_MULPD:case X86_MNEMONIC_VMULPD:scalar=X86_MNEMONIC_MULSD;scalarBytes=8;break;
    case X86_MNEMONIC_DIVPS:case X86_MNEMONIC_VDIVPS:scalar=X86_MNEMONIC_DIVSS;break;
    case X86_MNEMONIC_DIVPD:case X86_MNEMONIC_VDIVPD:scalar=X86_MNEMONIC_DIVSD;scalarBytes=8;break;
    case X86_MNEMONIC_MINPS:case X86_MNEMONIC_VMINPS:scalar=X86_MNEMONIC_MINSS;break;
    case X86_MNEMONIC_MINPD:case X86_MNEMONIC_VMINPD:scalar=X86_MNEMONIC_MINSD;scalarBytes=8;break;
    case X86_MNEMONIC_MAXPS:case X86_MNEMONIC_VMAXPS:scalar=X86_MNEMONIC_MAXSS;break;
    case X86_MNEMONIC_MAXPD:case X86_MNEMONIC_VMAXPD:scalar=X86_MNEMONIC_MAXSD;scalarBytes=8;break;
    case X86_MNEMONIC_SQRTPS:case X86_MNEMONIC_VSQRTPS:scalar=X86_MNEMONIC_SQRTSS;break;
    case X86_MNEMONIC_SQRTPD:case X86_MNEMONIC_VSQRTPD:scalar=X86_MNEMONIC_SQRTSD;scalarBytes=8;break;
    default:break;
    }
    if(scalar) {
        using namespace Translation;
        const bool legacy=instruction.encoding_family==X86_INSTRUCTION_ENCODING_LEGACY;
        const bool root=scalar==X86_MNEMONIC_SQRTSS || scalar==X86_MNEMONIC_SQRTSD;
        const unsigned left=legacy?0:1,right=root?1:legacy?1:2;
        if(instruction.operand_count!=right+1)return {VectorStop::Unsupported,instruction.address};
        const unsigned size=instruction.operands[0].bit_width/8;
        auto vector=[](const Operand& op)->std::optional<unsigned> {
            if(op.form!=OperandForm::Register)return std::nullopt;
            if(op.register_id>=X86_REGISTER_XMM0 && op.register_id<=X86_REGISTER_XMM15)return op.register_id-X86_REGISTER_XMM0;
            if(op.register_id>=X86_REGISTER_YMM0 && op.register_id<=X86_REGISTER_YMM15)return op.register_id-X86_REGISTER_YMM0;
            return std::nullopt;
        };
        const auto destination=vector(instruction.operands[0]);
        if(!destination || (size!=16 && size!=32))return {VectorStop::Unsupported,instruction.address};
        std::array<std::array<std::uint8_t,32>,2> values{};
        for(unsigned n=0;n<2;++n) {
            const auto& op=instruction.operands[n?right:left];
            if(auto index=vector(op))values[n]=frame.vectors[*index];
            else {
                const auto address=ResolveGuestAddress(instruction,op,frame);
                if(!address)return {VectorStop::Unsupported,instruction.address};
                if(!memory.read || !memory.read(*address,std::span(values[n]).first(size),memory.context))
                    return {VectorStop::MemoryFault,instruction.address,*address};
            }
        }
        auto output=frame.vectors[*destination];
        auto operation=instruction;operation.mnemonic=scalar;operation.operand_count=2;
        operation.encoding_family=X86_INSTRUCTION_ENCODING_LEGACY;
        for(unsigned i=0;i<2;++i) {
            operation.operands[i]={};operation.operands[i].form=OperandForm::Register;
            operation.operands[i].register_id=X86_REGISTER_XMM0+i;operation.operands[i].bit_width=128;
        }
        for(unsigned offset=0;offset<size;offset+=scalarBytes) {
            auto lane=frame;
            std::copy_n(values[0].begin()+offset,scalarBytes,lane.vectors[0].begin());
            std::copy_n(values[1].begin()+offset,scalarBytes,lane.vectors[1].begin());
            const auto status=ExecuteVectorInstruction(operation,lane,{});
            if(status.stop!=VectorStop::Continue)return status;
            std::copy_n(lane.vectors[0].begin(),scalarBytes,output.begin()+offset);
        }
        if(!legacy && size==16)std::fill(output.begin()+16,output.end(),0);
        frame.vectors[*destination]=output;
        return {VectorStop::Continue,instruction.address+instruction.length};
    }
    // Streaming stores use the same bytes and permissions, with stronger host
    // ordering than x86's write-combining hint. Preserve required alignment.
    auto mnemonic=instruction.mnemonic;
    if(mnemonic==X86_MNEMONIC_MOVNTPS || mnemonic==X86_MNEMONIC_MOVNTPD ||
       mnemonic==X86_MNEMONIC_MOVNTDQ || mnemonic==X86_MNEMONIC_VMOVNTPS ||
       mnemonic==X86_MNEMONIC_VMOVNTPD || mnemonic==X86_MNEMONIC_VMOVNTDQ) {
        if(instruction.operand_count!=2 || instruction.operands[0].form!=Translation::OperandForm::Memory)
            return {Translation::VectorStop::Unsupported,instruction.address};
        const auto address=Translation::ResolveGuestAddress(instruction,instruction.operands[0],frame);
        const unsigned bytes=instruction.operands[1].bit_width/8;
        if(!address || (bytes!=16 && bytes!=32))return {Translation::VectorStop::Unsupported,instruction.address};
        if(*address%bytes)return {Translation::VectorStop::MemoryFault,instruction.address,*address};
        mnemonic=instruction.encoding_family==X86_INSTRUCTION_ENCODING_LEGACY?X86_MNEMONIC_MOVUPS:X86_MNEMONIC_VMOVUPS;
    }
    if(mnemonic==X86_MNEMONIC_MOVNTDQA || mnemonic==X86_MNEMONIC_VMOVNTDQA) {
        if(instruction.operand_count!=2 || instruction.operands[1].form!=Translation::OperandForm::Memory)
            return {Translation::VectorStop::Unsupported,instruction.address};
        const auto address=Translation::ResolveGuestAddress(instruction,instruction.operands[1],frame);
        const unsigned bytes=instruction.operands[0].bit_width/8;
        if(!address || (bytes!=16 && bytes!=32))return {Translation::VectorStop::Unsupported,instruction.address};
        if(*address%bytes)return {Translation::VectorStop::MemoryFault,instruction.address,*address};
        mnemonic=instruction.encoding_family==X86_INSTRUCTION_ENCODING_LEGACY?X86_MNEMONIC_MOVUPS:X86_MNEMONIC_VMOVUPS;
    }
    switch(instruction.mnemonic) {
    case X86_MNEMONIC_MOVAPD: mnemonic=X86_MNEMONIC_MOVAPS;break;
    case X86_MNEMONIC_MOVUPD: mnemonic=X86_MNEMONIC_MOVUPS;break;
    case X86_MNEMONIC_VMOVAPD: mnemonic=X86_MNEMONIC_VMOVAPS;break;
    case X86_MNEMONIC_VMOVUPD: mnemonic=X86_MNEMONIC_VMOVUPS;break;
    case X86_MNEMONIC_PUNPCKLQDQ: mnemonic=X86_MNEMONIC_UNPCKLPD;break;
    case X86_MNEMONIC_PUNPCKHQDQ: mnemonic=X86_MNEMONIC_UNPCKHPD;break;
    case X86_MNEMONIC_VPUNPCKLQDQ: mnemonic=X86_MNEMONIC_VUNPCKLPD;break;
    case X86_MNEMONIC_VPUNPCKHQDQ: mnemonic=X86_MNEMONIC_VUNPCKHPD;break;
    case X86_MNEMONIC_PUNPCKLDQ: mnemonic=X86_MNEMONIC_UNPCKLPS;break;
    case X86_MNEMONIC_PUNPCKHDQ: mnemonic=X86_MNEMONIC_UNPCKHPS;break;
    case X86_MNEMONIC_VPUNPCKLDQ: mnemonic=X86_MNEMONIC_VUNPCKLPS;break;
    case X86_MNEMONIC_VPUNPCKHDQ: mnemonic=X86_MNEMONIC_VUNPCKHPS;break;
    case X86_MNEMONIC_XORPS: case X86_MNEMONIC_XORPD: mnemonic=X86_MNEMONIC_PXOR;break;
    case X86_MNEMONIC_VXORPS: case X86_MNEMONIC_VXORPD: mnemonic=X86_MNEMONIC_VPXOR;break;
    case X86_MNEMONIC_ANDPS: case X86_MNEMONIC_ANDPD: mnemonic=X86_MNEMONIC_PAND;break;
    case X86_MNEMONIC_VANDPS: case X86_MNEMONIC_VANDPD: mnemonic=X86_MNEMONIC_VPAND;break;
    case X86_MNEMONIC_ANDNPS: case X86_MNEMONIC_ANDNPD: mnemonic=X86_MNEMONIC_PANDN;break;
    case X86_MNEMONIC_VANDNPS: case X86_MNEMONIC_VANDNPD: mnemonic=X86_MNEMONIC_VPANDN;break;
    case X86_MNEMONIC_ORPS: case X86_MNEMONIC_ORPD: mnemonic=X86_MNEMONIC_POR;break;
    case X86_MNEMONIC_VORPS: case X86_MNEMONIC_VORPD: mnemonic=X86_MNEMONIC_VPOR;break;
    default:break;
    }
    for(unsigned i=0;i<instruction.operand_count;++i)
        if(Translation::DecodeIntegerRegister(instruction.operands[i]))return {Translation::VectorStop::Unsupported,instruction.address};
    if(mnemonic==instruction.mnemonic)return Translation::ExecuteVectorInstruction(instruction,frame,memory);
    auto alias=instruction;alias.mnemonic=mnemonic;
    return Translation::ExecuteVectorInstruction(alias,frame,memory);
}
}
