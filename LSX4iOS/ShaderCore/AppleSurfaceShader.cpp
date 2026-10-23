// SPDX-License-Identifier: GPL-2.0-or-later
#include "AppleSurfaceShader.h"
#include "HostTilingShader.h"
#include "video_core/amdgpu/tiling.h"
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>
#include <bit>
#include <mutex>
#include <map>
#include <stdexcept>
namespace Lsx4::Apple {
std::vector<std::uint32_t> CompileSurfaceShader32(std::uint32_t tileMode,bool tile) {
    if(tileMode>5 && tileMode!=9 && tileMode!=10 && tileMode!=13 && tileMode!=14)
        throw std::runtime_error("Unsupported 32-bit surface tile mode");
    static const bool initialized=glslang::InitializeProcess();
    if(!initialized)throw std::runtime_error("GLSL compiler initialization failed");
    static std::mutex mutex;static std::map<unsigned,std::vector<std::uint32_t>> cache;
    std::lock_guard guard(mutex);const unsigned key=tileMode*2+tile;
    if(auto it=cache.find(key);it!=cache.end())return it->second;
    const auto mode=AmdGpu::TileMode(tileMode);const auto array=AmdGpu::GetArrayMode(mode);
    const auto micro=AmdGpu::GetMicroTileMode(mode);
    std::string defines;
    auto define=[&](const char* name,unsigned value){defines+="#define "+std::string(name)+" "+std::to_string(value)+"\n";};
    define("BITS_PER_PIXEL",32);define("NUM_SAMPLES",1);define("ARRAY_MODE",unsigned(array));
    define("MICRO_TILE_MODE",unsigned(micro));define("MICRO_TILE_THICKNESS",AmdGpu::GetMicroTileThickness(array));
    if(AmdGpu::IsMacroTiled(array)) {
        auto macro=AmdGpu::CalculateMacrotileMode(mode,32,1);auto banks=AmdGpu::GetNumBanks(macro);
        define("PIPE_CONFIG",unsigned(AmdGpu::GetPipeConfig(mode)));define("BANK_WIDTH",AmdGpu::GetBankWidth(macro));
        define("BANK_HEIGHT",AmdGpu::GetBankHeight(macro));define("NUM_BANKS",banks);define("NUM_BANK_BITS",std::bit_width(banks)-1);
        define("TILE_SPLIT_BYTES",AmdGpu::CalculateTileSplit(mode,array,micro,32));define("MACRO_TILE_ASPECT",AmdGpu::GetMacrotileAspect(macro));
    }
    if(tile)define("IS_TILER",1);
    std::string source=SharedTilingSource;
    // All members of this 32-bit specialization have scalar alignment under
    // std430. Readonly SSBO metadata keeps the common offsets without requiring
    // Vulkan scalarBlockLayout/uniformBufferStandardLayout features.
    auto replace=[&](std::string_view before,std::string_view after) {
        std::size_t at=0;unsigned count=0;
        while((at=source.find(before,at))!=std::string::npos){source.replace(at,before.size(),after);at+=after.size();++count;}
        return count;
    };
    if(replace(", scalar)",", std430)")!=3 || replace("uniform TilingInfo","readonly buffer TilingInfo")!=1)
        throw std::runtime_error("Shared tiling interface changed");
    glslang::TShader shader(EShLangCompute);const char* text=source.c_str();shader.setStrings(&text,1);shader.setPreamble(defines.c_str());
    shader.setEnvInput(glslang::EShSourceGlsl,EShLangCompute,glslang::EShClientVulkan,450);
    shader.setEnvClient(glslang::EShClientVulkan,glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv,glslang::EShTargetSpv_1_0);
    const auto messages=EShMessages(EShMsgSpvRules|EShMsgVulkanRules);
    if(!shader.parse(GetDefaultResources(),450,false,messages))throw std::runtime_error(std::string("Surface shader: ")+shader.getInfoLog());
    glslang::TProgram program;program.addShader(&shader);
    if(!program.link(messages))throw std::runtime_error(std::string("Surface shader link: ")+program.getInfoLog());
    std::vector<std::uint32_t> spirv;glslang::SpvOptions options;options.disableOptimizer=true;
    glslang::GlslangToSpv(*program.getIntermediate(EShLangCompute),spirv,&options);
    if(spirv.size()<5 || spirv[0]!=0x07230203 || spirv[1]!=0x10000)throw std::runtime_error("Invalid portable surface shader");
    cache[key]=spirv;return spirv;
}
}
