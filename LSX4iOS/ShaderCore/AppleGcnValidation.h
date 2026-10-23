// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "shader_recompiler/frontend/decode.h"
#include <set>
#include <stdexcept>
namespace Lsx4::Apple {
inline void ValidateDirectGcn(std::span<const std::uint32_t> words) {
    using namespace Shader::Gcn;
    GcnDecodeContext decoder;
    std::vector<std::uint32_t> padded(words.begin(),words.end());padded.resize(words.size()+4,0xbf810000);
    std::set<std::uint32_t> boundaries;std::vector<std::uint32_t> targets;
    bool terminator=false;
    for(std::uint32_t offset=0;offset<words.size()*4;) {
        boundaries.insert(offset);
        GcnCodeSlice slice(padded.data()+offset/4,padded.data()+padded.size());
        const auto ins=decoder.decodeInstruction(slice);
        if(!ins.length || (ins.length&3) || ins.length>words.size()*4-offset)
            throw std::runtime_error("Truncated GCN instruction");
        switch(ins.opcode) {
            case Opcode::S_LOAD_DWORD:case Opcode::S_LOAD_DWORDX2:case Opcode::S_LOAD_DWORDX4:
            case Opcode::S_LOAD_DWORDX8:case Opcode::S_LOAD_DWORDX16:case Opcode::S_SWAPPC_B64:
                throw std::runtime_error("Guest SRT/fetch memory adapter is required");
            default:break;
        }
        if(ins.IsConditionalBranch() || ins.IsUnconditionalBranch())targets.push_back(ins.BranchTarget(offset));
        terminator|=ins.opcode==Opcode::S_ENDPGM;
        offset+=ins.length;
    }
    if(!terminator)throw std::runtime_error("GCN program has no terminator");
    for(auto target:targets)if(!boundaries.contains(target))throw std::runtime_error("GCN branch is outside instruction boundaries");
}
}
