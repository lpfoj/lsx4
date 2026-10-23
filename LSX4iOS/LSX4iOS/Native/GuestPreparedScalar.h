// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "GuestAddressSpace.h"
#include "common/x86_decoder.h"
#include "executor/dynamic_translation/register_access.h"
#include "executor/dynamic_translation/scalar_semantics.h"
#include "executor/dynamic_translation/integer_math.h"
#include <bit>

namespace Lsx4::Apple {
// Predecode only operand metadata. Every access still uses the current checked
// guest address space; no host pointer or mapping lifetime is cached here.
class GuestPreparedScalar {
    using Frame=Translation::CpuFrame;
    enum class Operation : std::uint8_t {None,Move,ZeroExtend,SignExtend,Address,Add,Subtract,Compare,And,Or,Xor,Test,Carry,Borrow,BitMovement};
    enum class Kind : std::uint8_t {None,Register,Immediate,Memory};
    struct Value {
        Kind kind{};
        Translation::IntegerAccess reg{};
        std::uint64_t constant{};
        std::uint8_t base{255},index{255},scale{},segment{},bytes{};
        std::uint64_t Address(const Frame& frame) const {
            auto result=constant;
            if(base!=255)result+=frame.integer[base];
            if(index!=255)result+=frame.integer[index]*scale;
            if(segment==1)result+=frame.fs_origin;
            if(segment==2)result+=frame.gs_origin;
            return result;
        }
        std::uint64_t Register(const Frame& frame) const {
            return Translation::ReadInteger(frame,static_cast<Translation::IntegerRegister>(reg.slot),reg.width,reg.upper_byte);
        }
        bool Read(const Frame& frame,GuestAddressSpace& memory,std::uint64_t& value) const {
            value=0;
            if(kind==Kind::Register)value=Register(frame);
            else if(kind==Kind::Immediate)value=constant;
            else return memory.Read(Address(frame),{reinterpret_cast<std::uint8_t*>(&value),bytes});
            return true;
        }
        bool Write(Frame& frame,GuestAddressSpace& memory,std::uint64_t value) const {
            if(kind==Kind::Register) {
                Translation::WriteInteger(frame,static_cast<Translation::IntegerRegister>(reg.slot),value,reg.width,reg.upper_byte);return true;
            }
            return memory.Write(Address(frame),{reinterpret_cast<const std::uint8_t*>(&value),bytes});
        }
    };
    Operation operation_{};
    Value destination_,source_;
    std::uint64_t address_{},next_{};
    std::uint32_t mnemonic_{};
    static Value PrepareValue(const Translation::Instruction& ins,const Translation::Operand& operand) {
        using namespace Translation;
        Value out;
        if(operand.bit_width!=8 && operand.bit_width!=16 && operand.bit_width!=32 && operand.bit_width!=64)return out;
        out.bytes=operand.bit_width/8;
        if(operand.form==OperandForm::Immediate) {
            if(operand.immediate.relative)return {};
            out.kind=Kind::Immediate;out.constant=operand.immediate.bits&WidthMask(operand.bit_width);return out;
        }
        if(auto reg=DecodeIntegerRegister(operand)) {out.kind=Kind::Register;out.reg=*reg;return out;}
        if(operand.form!=OperandForm::Memory || ins.address_width!=64)return {};
        out.constant=static_cast<std::uint64_t>(operand.address.displacement);
        auto slot=[](std::uint32_t id)->std::optional<std::uint8_t> {
            Operand operand;operand.form=OperandForm::Register;operand.register_id=id;operand.bit_width=64;
            auto reg=DecodeIntegerRegister(operand);
            if(!reg || reg->width!=64 || reg->upper_byte)return {};
            return reg->slot;
        };
        if(operand.address.base==X86_REGISTER_RIP)out.constant+=ins.address+ins.length;
        else if(operand.address.base!=X86_REGISTER_NONE) {
            const auto reg=slot(operand.address.base);if(!reg)return {};out.base=*reg;
        }
        if(operand.address.index!=X86_REGISTER_NONE) {
            const auto reg=slot(operand.address.index);if(!reg)return {};out.index=*reg;
            if(operand.address.scale!=1 && operand.address.scale!=2 && operand.address.scale!=4 && operand.address.scale!=8)return {};
            out.scale=operand.address.scale;
        }
        out.segment=operand.address.segment==X86_REGISTER_FS?1:operand.address.segment==X86_REGISTER_GS?2:0;
        out.kind=Kind::Memory;return out;
    }
public:
    static GuestPreparedScalar Prepare(const Translation::Instruction& ins) {
        GuestPreparedScalar out;
        if(!ins.length || ins.operand_count!=2 || (ins.prefix_attributes&X86_ATTRIB_HAS_LOCK))return out;
        Operation operation;
        switch(ins.mnemonic) {
        case X86_MNEMONIC_MOV:operation=Operation::Move;break;
        case X86_MNEMONIC_MOVZX:operation=Operation::ZeroExtend;break;
        case X86_MNEMONIC_MOVSX:case X86_MNEMONIC_MOVSXD:operation=Operation::SignExtend;break;
        case X86_MNEMONIC_LEA:operation=Operation::Address;break;
        case X86_MNEMONIC_ADD:operation=Operation::Add;break;
        case X86_MNEMONIC_SUB:operation=Operation::Subtract;break;
        case X86_MNEMONIC_CMP:operation=Operation::Compare;break;
        case X86_MNEMONIC_AND:operation=Operation::And;break;
        case X86_MNEMONIC_OR:operation=Operation::Or;break;
        case X86_MNEMONIC_XOR:operation=Operation::Xor;break;
        case X86_MNEMONIC_TEST:operation=Operation::Test;break;
        case X86_MNEMONIC_ADC:operation=Operation::Carry;break;
        case X86_MNEMONIC_SBB:operation=Operation::Borrow;break;
        case X86_MNEMONIC_SHL:case X86_MNEMONIC_SHR:case X86_MNEMONIC_SAR:
        case X86_MNEMONIC_ROL:case X86_MNEMONIC_ROR:operation=Operation::BitMovement;break;
        default:return out;
        }
        auto sourceOperand=ins.operands[1];
        // LEA carries an address expression, not a sized memory read.
        if(operation==Operation::Address)sourceOperand.bit_width=ins.operands[0].bit_width;
        const auto destination=PrepareValue(ins,ins.operands[0]),source=PrepareValue(ins,sourceOperand);
        if(destination.kind!=Kind::Register && destination.kind!=Kind::Memory)return out;
        if(source.kind==Kind::None || (destination.kind==Kind::Memory && source.kind==Kind::Memory))return out;
        if(operation>Operation::Move && operation<=Operation::Address && destination.kind!=Kind::Register)return out;
        if(operation>=Operation::Add && operation!=Operation::BitMovement && destination.bytes!=source.bytes)return out;
        if(operation==Operation::Address && (source.kind!=Kind::Memory || source.segment))return out;
        out.operation_=operation;out.destination_=destination;out.source_=source;
        out.address_=ins.address;out.next_=ins.address+ins.length;out.mnemonic_=ins.mnemonic;return out;
    }
    Translation::SemanticResult Execute(Frame& frame,GuestAddressSpace& memory) const {
        using namespace Translation;
        static_assert(std::endian::native==std::endian::little);
        if(operation_==Operation::None)return {SemanticStop::Unsupported,0,0};
        if(operation_>=Operation::Add) {
            std::uint64_t left{},right{};
            const bool leftOK=destination_.Read(frame,memory,left),rightOK=source_.Read(frame,memory,right);
            const auto target=destination_.kind==Kind::Memory?destination_.Address(frame):0;
            if(!leftOK || !rightOK)return {SemanticStop::MemoryFault,address_,leftOK && destination_.kind==Kind::Memory?target:address_};
            const auto width=destination_.bytes*8;
            std::uint64_t value{};IntegerMathResult direct{};
            enum class Flags {Add,Subtract,Logic,Direct} flags=Flags::Logic;
            switch(operation_) {
            case Operation::Add:value=(left+right)&WidthMask(width);flags=Flags::Add;break;
            case Operation::Subtract:case Operation::Compare:value=(left-right)&WidthMask(width);flags=Flags::Subtract;break;
            case Operation::And:case Operation::Test:value=left&right;break;
            case Operation::Or:value=left|right;break;
            case Operation::Xor:value=left^right;break;
            case Operation::Carry:direct=AddWithCarry(left,right,(frame.condition_word&CarryFlag)!=0,width);flags=Flags::Direct;break;
            case Operation::Borrow:direct=SubtractWithBorrow(left,right,(frame.condition_word&CarryFlag)!=0,width);flags=Flags::Direct;break;
            case Operation::BitMovement: {
                const auto evaluated=EvaluateBitMovement(mnemonic_,left,right,width);
                if(!evaluated)return {SemanticStop::Unsupported,address_,0};
                if(!evaluated->effective)return {SemanticStop::Continue,next_,0};
                direct=*evaluated;flags=Flags::Direct;break;
            }
            default:return {SemanticStop::Unsupported,address_,0};
            }
            if(flags==Flags::Direct)value=direct.value;
            if(operation_!=Operation::Compare && operation_!=Operation::Test && !destination_.Write(frame,memory,value))
                return {SemanticStop::MemoryFault,address_,target};
            if(flags==Flags::Add)ApplyAdditionFlags(frame,left,right,width);
            else if(flags==Flags::Subtract)ApplySubtractionFlags(frame,left,right,width);
            else if(flags==Flags::Logic)ApplyLogicalFlags(frame,value,width);
            else frame.condition_word=(frame.condition_word&~direct.flag_mask)|direct.flag_bits;
            return {SemanticStop::Continue,next_,0};
        }
        std::uint64_t value{};
        if(operation_==Operation::Address)value=source_.Address(frame);
        else if(source_.kind==Kind::Register)value=source_.Register(frame);
        else if(source_.kind==Kind::Immediate)value=source_.constant;
        else if(!memory.Read(source_.Address(frame),{reinterpret_cast<std::uint8_t*>(&value),source_.bytes}))
            return {SemanticStop::MemoryFault,address_,operation_==Operation::Move?0:address_};
        if(operation_==Operation::SignExtend)value=SignExtend(value,source_.bytes*8);
        if(destination_.kind==Kind::Register) {
            const auto reg=destination_.reg;
            WriteInteger(frame,static_cast<IntegerRegister>(reg.slot),value,reg.width,reg.upper_byte);
        } else {
            const auto destination=destination_.Address(frame);
            if(!memory.Write(destination,{reinterpret_cast<const std::uint8_t*>(&value),destination_.bytes}))
                return {SemanticStop::MemoryFault,address_,destination};
        }
        return {SemanticStop::Continue,next_,0};
    }
};
}
