// SPDX-License-Identifier: GPL-2.0-or-later
#include "SharedCpuProbe.h"
#include "executor/dynamic_translation/scalar_semantics.h"
#include <algorithm>
#include <array>
using namespace Lsx4::Translation;
namespace {
struct Memory {
    std::array<std::uint8_t, 256> bytes{};
    static bool Read(std::uint64_t address, std::span<std::uint8_t> out, void* context) noexcept {
        auto& self = *static_cast<Memory*>(context);
        if (address < 0x1000 || address - 0x1000 > self.bytes.size() || out.size() > self.bytes.size() - (address - 0x1000)) return false;
        std::copy_n(self.bytes.data() + address - 0x1000, out.size(), out.data());
        return true;
    }
    static bool Write(std::uint64_t address, std::span<const std::uint8_t> in, void* context) noexcept {
        auto& self = *static_cast<Memory*>(context);
        if (address < 0x1000 || address - 0x1000 > self.bytes.size() || in.size() > self.bytes.size() - (address - 0x1000)) return false;
        std::copy(in.begin(), in.end(), self.bytes.data() + address - 0x1000);
        return true;
    }
};
}
bool LSX4SharedCpuProbe() {
    Memory memory;
    // x86-64: add 20+22, store/load through guest memory, compare, branch, return.
    const std::uint8_t code[] = {0xb8,20,0,0,0, 0x83,0xc0,22,
        0x89,0x04,0x25,0x80,0x10,0,0, 0x8b,0x1c,0x25,0x80,0x10,0,0,
        0x83,0xfb,42, 0x75,0x01, 0xc3, 0x0f,0x0b};
    std::copy(std::begin(code),std::end(code),memory.bytes.begin());
    memory.bytes[0xf0]=0x34; memory.bytes[0xf1]=0x12;
    CpuFrame frame;
    frame.integer[4]=0x10f0;
    GuestMemoryPort port{Memory::Read,Memory::Write,&memory};
    std::uint64_t ip = 0x1000;
    bool returned = false;
    for (int count=0;count<32;++count) {
        if (ip < 0x1000 || ip >= 0x1100) return false;
        Instruction instruction;
        if (DecodeInstruction(std::span<const std::uint8_t>(memory.bytes).subspan(ip-0x1000),ip,instruction)!=DecodeOutcome::Complete) return false;
        auto result=ExecuteScalarInstruction(instruction,frame,port);
        if (result.stop==SemanticStop::Return) {returned=result.next_address==0x1234;break;}
        if (result.stop!=SemanticStop::Continue && result.stop!=SemanticStop::Transfer) return false;
        ip=result.next_address;
    }
    if (!returned || frame.integer[0]!=42 || frame.integer[3]!=42 || memory.bytes[0x80]!=42) return false;
    // Invalid guest memory must produce a guest fault, never a host dereference.
    const std::uint8_t invalidRead[]={0x8b,0x04,0x25,0,0x20,0,0};
    Instruction instruction;
    if (DecodeInstruction(invalidRead,0x1000,instruction)!=DecodeOutcome::Complete) return false;
    return ExecuteScalarInstruction(instruction,frame,port).stop==SemanticStop::MemoryFault;
}
