// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/x86_decoder.h"
#include "executor/dynamic_translation/scalar_semantics.h"
#include "executor/dynamic_translation/address_resolver.h"
#include "executor/dynamic_translation/register_access.h"
#include "executor/dynamic_translation/integer_math.h"
namespace Lsx4::Apple {
inline std::optional<Translation::SemanticResult> ExecuteGuestString(
    const Translation::Instruction& ins,Translation::CpuFrame& frame,const Translation::GuestMemoryPort& memory) {
    using namespace Translation;
    enum Kind {Move,Compare,Store,Load,Scan};Kind kind;
    unsigned bytes=0;
    switch(ins.mnemonic) {
    case X86_MNEMONIC_MOVSB:bytes=1;kind=Move;break;
    case X86_MNEMONIC_MOVSW:bytes=2;kind=Move;break;
    case X86_MNEMONIC_MOVSD:bytes=4;kind=Move;break;
    case X86_MNEMONIC_MOVSQ:bytes=8;kind=Move;break;
    case X86_MNEMONIC_CMPSB:bytes=1;kind=Compare;break;
    case X86_MNEMONIC_CMPSW:bytes=2;kind=Compare;break;
    case X86_MNEMONIC_CMPSD:bytes=4;kind=Compare;break;
    case X86_MNEMONIC_CMPSQ:bytes=8;kind=Compare;break;
    case X86_MNEMONIC_STOSB:bytes=1;kind=Store;break;
    case X86_MNEMONIC_STOSW:bytes=2;kind=Store;break;
    case X86_MNEMONIC_STOSD:bytes=4;kind=Store;break;
    case X86_MNEMONIC_STOSQ:bytes=8;kind=Store;break;
    case X86_MNEMONIC_LODSB:bytes=1;kind=Load;break;
    case X86_MNEMONIC_LODSW:bytes=2;kind=Load;break;
    case X86_MNEMONIC_LODSD:bytes=4;kind=Load;break;
    case X86_MNEMONIC_LODSQ:bytes=8;kind=Load;break;
    case X86_MNEMONIC_SCASB:bytes=1;kind=Scan;break;
    case X86_MNEMONIC_SCASW:bytes=2;kind=Scan;break;
    case X86_MNEMONIC_SCASD:bytes=4;kind=Scan;break;
    case X86_MNEMONIC_SCASQ:bytes=8;kind=Scan;break;
    default:return std::nullopt;
    }
    if(ins.operand_count!=2 || (ins.address_width!=32 && ins.address_width!=64) ||
       ins.encoding_family!=X86_INSTRUCTION_ENCODING_LEGACY || (ins.prefix_attributes&X86_ATTRIB_HAS_LOCK))return std::nullopt;
    const bool sourceMemory=kind==Move || kind==Compare || kind==Load;
    const bool destinationMemory=kind!=Load;
    const unsigned sourceIndex=kind==Compare?0:1,destinationIndex=kind==Scan || kind==Compare?1:0;
    if(sourceMemory && ins.operands[sourceIndex].form!=OperandForm::Memory)return std::nullopt;
    if(destinationMemory && ins.operands[destinationIndex].form!=OperandForm::Memory)return std::nullopt;
    if((kind==Store && ins.operands[1].form!=OperandForm::Register) ||
       ((kind==Load || kind==Scan) && ins.operands[0].form!=OperandForm::Register))return std::nullopt;
    const auto done=SemanticResult{SemanticStop::Continue,ins.address+ins.length};
    const bool repeat=ins.prefix_attributes&(X86_ATTRIB_HAS_REP|X86_ATTRIB_HAS_REPE|X86_ATTRIB_HAS_REPNE);
    const auto count=ReadInteger(frame,IntegerRegister::C,ins.address_width);
    if(repeat && !count)return done;
    std::array<std::uint8_t,8> source{},destination{};
    std::optional<std::uint64_t> sourceAddress,destinationAddress;
    if(sourceMemory)sourceAddress=ResolveGuestAddress(ins,ins.operands[sourceIndex],frame);
    if(destinationMemory)destinationAddress=ResolveGuestAddress(ins,ins.operands[destinationIndex],frame);
    if((sourceMemory && !sourceAddress) || (destinationMemory && !destinationAddress))return std::nullopt;
    auto fault=[&](std::uint64_t address){return SemanticResult{SemanticStop::MemoryFault,ins.address,address};};
    if(sourceMemory) {
        if(!memory.read || !memory.read(*sourceAddress,std::span(source).first(bytes),memory.context))return fault(*sourceAddress);
    } else {
        const auto value=ReadInteger(frame,IntegerRegister::A,bytes*8);
        for(unsigned i=0;i<bytes;++i)source[i]=value>>(8*i);
    }
    const bool compare=kind==Compare || kind==Scan;
    if(compare) {
        if(!memory.read || !memory.read(*destinationAddress,std::span(destination).first(bytes),memory.context))return fault(*destinationAddress);
        std::uint64_t a=0,b=0;
        for(unsigned i=0;i<bytes;++i){a|=std::uint64_t(source[i])<<(8*i);b|=std::uint64_t(destination[i])<<(8*i);}
        const auto value=SubtractWithBorrow(a,b,false,bytes*8);
        frame.condition_word=(frame.condition_word&~value.flag_mask)|value.flag_bits;
    } else if(kind==Load) {
        std::uint64_t value=0;for(unsigned i=0;i<bytes;++i)value|=std::uint64_t(source[i])<<(8*i);
        WriteInteger(frame,IntegerRegister::A,value,bytes*8);
    } else if(!memory.write || !memory.write(*destinationAddress,std::span(source).first(bytes),memory.context))return fault(*destinationAddress);
    const auto delta=(frame.condition_word&DirectionFlag)?0-std::uint64_t(bytes):std::uint64_t(bytes);
    if(sourceMemory)WriteInteger(frame,IntegerRegister::Source,frame.integer[6]+delta,ins.address_width);
    if(destinationMemory)WriteInteger(frame,IntegerRegister::Destination,frame.integer[7]+delta,ins.address_width);
    if(repeat) {
        WriteInteger(frame,IntegerRegister::C,count-1,ins.address_width);
        const bool repeatCondition=!compare || bool(frame.condition_word&ZeroFlag)!=bool(ins.prefix_attributes&X86_ATTRIB_HAS_REPNE);
        // One iteration per executor step provides bounded preemption and exact
        // restart state on a later fault, without treating REP as atomic memcpy.
        if(count>1 && repeatCondition)return SemanticResult{SemanticStop::Continue,ins.address};
    }
    return done;
}
}
