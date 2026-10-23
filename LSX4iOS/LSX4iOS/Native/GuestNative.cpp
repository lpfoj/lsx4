// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestNative.h"
#include "common/x86_decoder.h"
#include "executor/dynamic_translation/scalar_semantics.h"
#include <cstring>

namespace Lsx4::Apple {
using namespace Translation;
NativeStep GuestNative::Execute(const Instruction& instruction,CpuFrame& frame,std::uint64_t& next,Dispatch& dispatch) {
    if(!enabled_)return NativeStep::Fallback;
    if(!dispatch.prepared) {
    dispatch.prepared=true;
    if(instruction.prefix_attributes&X86_ATTRIB_HAS_LOCK)return NativeStep::Fallback;
    // Memory instructions use the checked guest-memory port. This first native
    // integration admits only register/constant operations and direct jumps.
    for(unsigned i=0;i<instruction.operand_count;++i)
        if(instruction.operands[i].form==OperandForm::Memory) return NativeStep::Fallback;
    auto& entry=entries_[instruction.address];
    if(entry.length!=instruction.length || entry.bytes!=instruction.encoding_bytes) {
        entry={instruction.encoding_bytes,instruction.length,{}};
        RegionPlan plan;plan.first_address=instruction.address;plan.continuation=instruction.address+instruction.length;
        plan.sequence.push_back({instruction,ClassifyFlow(instruction.mnemonic)});
        auto program=BuildScalarProgram(plan);
        if(program.semantic_steps.empty()) {
            entry.artifact=CompileScalarIr(program,arena_);
            if(entry.artifact.Succeeded()) ++compilations;
            else if(entry.artifact.failure==NativeCompileFailure::ArenaExhausted) enabled_=false;
        }
    }
    if(entry.artifact.Succeeded())dispatch.entry=&entry;
    }
    if(!dispatch.entry)return NativeStep::Fallback;
    auto& entry=*dispatch.entry;
    // Performance runs still validate the first use of every compiled entry.
    // Guest encoding changes replace the entire entry and reset validation.
    // The strict regression mode below remains the default.
    if(!validateEveryExecution_ && entry.validated) {
        next=entry.artifact.entry(&frame);++executions;return NativeStep::Executed;
    }
    // During porting, every generated execution is checked against the common
    // interpreter on the identical register state. No guest memory is modified
    // by the reference execution. A mismatch stops the run with its real state.
    CpuFrame reference;std::memcpy(&reference,&frame,sizeof(frame));
    const auto expected=ExecuteScalarInstruction(instruction,reference,{});
    if(expected.stop!=SemanticStop::Continue && expected.stop!=SemanticStop::Transfer) return NativeStep::Fallback;
    const auto& reservation=entry.artifact.reservation;
    if(std::memcmp(reservation.writable.data(),reservation.executable.data(),reservation.writable.size())) {
        ++mismatches;return NativeStep::Mismatch;
    }
    next=entry.artifact.entry(&frame);++executions;++checkedExecutions;
    reference.resume_address=expected.next_address;
    if(next!=expected.next_address || std::memcmp(&reference,&frame,sizeof(frame))) {
        ++mismatches;return NativeStep::Mismatch;
    }
    entry.validated=true;return NativeStep::Executed;
}
}
