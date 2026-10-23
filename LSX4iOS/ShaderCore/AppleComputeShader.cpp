// SPDX-License-Identifier: GPL-2.0-or-later
#include "AppleComputeShader.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "AppleGcnValidation.h"
#include <stdexcept>
namespace Lsx4::Apple {

CompiledComputeShader CompileComputeShader(const ComputeShaderInput& input) {
    if(input.code.empty() || input.code.size()>16384)throw std::runtime_error("Invalid compute program size");
    std::uint64_t threads=1;
    for(auto value:input.workgroup) {
        if(!value || value>1024)throw std::runtime_error("Unsupported compute workgroup");
        threads*=value;
    }
    if(threads>1024 || ((input.rsrc2>>1)&31)>16 || (input.rsrc2&1))
        throw std::runtime_error("Unsupported compute register or scratch requirements");
    ValidateDirectGcn(input.code);
    Shader::ShaderParams params{std::span<const u32,16>(input.userData),input.code,0};
    Shader::Info info(Shader::Stage::Compute,Shader::LogicalStage::Compute,params);
    info.pgm_base=input.address;
    union Holder {Shader::RuntimeInfo runtime;Holder(){}~Holder(){}} holder;
    auto& runtime=holder.runtime;runtime.Initialize(Shader::Stage::Compute);
    runtime.num_user_data=(input.rsrc2>>1)&31;
    runtime.num_allocated_vgprs=(input.rsrc1&63)*4; // Same convention as shared PipelineCache.
    runtime.cs_info.workgroup_size=input.workgroup;
    for(unsigned i=0;i<3;++i)runtime.cs_info.tgid_enable[i]=(input.rsrc2>>(7+i))&1;
    runtime.cs_info.shared_memory_size=((input.rsrc2>>15)&511)*512;
    Shader::Profile profile{};profile.subgroup_size=32;profile.max_ubo_size=16384;profile.max_shared_memory_size=16384;
    // Each descriptor starts at its logical byte zero in aligned staging.
    // Keep common software bounds relative to the guest descriptor size.
    profile.needs_buffer_offsets=false;
    if(runtime.cs_info.shared_memory_size>profile.max_shared_memory_size)
        throw std::runtime_error("Compute shared memory exceeds portable limit");
    Shader::Pools pools;Shader::Backend::Bindings bindings{};
    const auto program=Shader::TranslateProgram(input.code,pools,info,runtime,profile);
    if(info.translation_failed || !info.images.empty() || !info.samplers.empty() || !info.srt_info.portable_nodes.empty())
        throw std::runtime_error("Unsupported compute resources or translation");
    CompiledComputeShader result;
    result.spirv=Shader::Backend::SPIRV::EmitSPIRV(profile,runtime,program,bindings);
    if(result.spirv.size()<5 || result.spirv[0]!=0x07230203)throw std::runtime_error("Invalid compute SPIR-V");
    Shader::PushData push{};Shader::Backend::Bindings pushed{};info.PushUd(pushed,push);
    for(const auto& resource:info.buffers) {
        ComputeBufferBinding binding;binding.written=resource.is_written;
        if(resource.buffer_type==Shader::BufferType::Flatbuf) {
            const auto flat=info.FlattenedUserData();binding.size=flat.size_bytes();
            const auto bytes=std::as_bytes(flat);
            binding.inlineBytes.resize(bytes.size());std::memcpy(binding.inlineBytes.data(),bytes.data(),bytes.size());
        } else if(resource.buffer_type==Shader::BufferType::Guest) {
            const auto sharp=resource.GetSharp(info);
            binding.address=sharp.base_address;
            binding.size=std::uint64_t(sharp.num_records)*(sharp.stride?sharp.stride:1);
            if(!sharp.Valid() || binding.size>UINT32_MAX)throw std::runtime_error("Invalid compute buffer descriptor");
        } else throw std::runtime_error("Unsupported special compute buffer");
        result.buffers.push_back(std::move(binding));
    }
    std::memcpy(result.pushConstants.data(),&push,sizeof(push));
    return result;
}
}
