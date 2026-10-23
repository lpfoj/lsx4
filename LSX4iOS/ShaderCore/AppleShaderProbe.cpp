// SPDX-License-Identifier: GPL-2.0-or-later
#include "AppleShaderProbe.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include <stdexcept>
namespace Lsx4::Apple {
std::vector<std::uint32_t> CompileShaderProbe() {
    const std::array<u32,16> user{};
    // S_MOV_B32 vcc_hi, literal; S_ENDPGM. No resources, lane operations or
    // optional hardware features are required by this test shader.
    const std::array<u32,3> code{0xbeeb03ff,0,0xbf810000};
    Shader::ShaderParams params{std::span<const u32,16>(user),code,0};
    Shader::Info info(Shader::Stage::Compute,Shader::LogicalStage::Compute,params);
    union Holder {Shader::RuntimeInfo runtime;Holder(){}~Holder(){}} holder;
    auto& runtime=holder.runtime;runtime.Initialize(Shader::Stage::Compute);
    runtime.num_allocated_vgprs=4;runtime.cs_info.workgroup_size={1,1,1};
    Shader::Profile profile{};profile.subgroup_size=32;
    profile.max_ubo_size=16384;profile.max_shared_memory_size=16384;
    Shader::Pools pools;Shader::Backend::Bindings bindings{};
    const auto program=Shader::TranslateProgram(code,pools,info,runtime,profile);
    auto spirv=Shader::Backend::SPIRV::EmitSPIRV(profile,runtime,program,bindings);
    if(info.translation_failed || spirv.size()<5 || spirv[0]!=0x07230203)
        throw std::runtime_error("GCN probe translation failed");
    return spirv;
}
}
