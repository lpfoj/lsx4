// SPDX-License-Identifier: GPL-2.0-or-later
#include "AppleVulkanSurface.h"
#include "AppleVulkanCompute.h"
#include "AppleSurfaceShader.h"
#include <cstring>
#include <stdexcept>
namespace Lsx4::Apple {
std::vector<std::uint8_t> TransformSurface32(std::uint32_t mode,std::uint32_t pitch,
    std::uint32_t height,std::span<const std::uint8_t> input,bool tile) {
    const auto size=std::uint64_t(pitch)*height*4;
    if(!pitch || !height || pitch>16384 || height>16384 || size!=input.size() ||
       size>64ull*1024*1024-512 || (pitch&7) || (height&7))
        throw std::runtime_error("Invalid 32-bit surface dimensions");
    if(mode==8 || mode==31)return {input.begin(),input.end()};
    CompiledComputeShader shader;shader.spirv=CompileSurfaceShader32(mode,tile);
    std::vector<std::vector<std::uint8_t>> buffers(3);
    buffers[0].resize(size);buffers[1].resize(size);buffers[2].resize(12+16*16);
    std::memcpy(buffers[tile?1:0].data(),input.data(),input.size());
    const std::array<std::uint32_t,7> info{0,1,1,std::uint32_t(size),pitch,height,0};
    std::memcpy(buffers[2].data(),info.data(),sizeof(info));
    for(unsigned i=0;i<3;++i) {
        ComputeBufferBinding binding;binding.size=buffers[i].size();binding.inlineBytes=buffers[i];
        binding.written=i==(tile?0u:1u);shader.buffers.push_back(std::move(binding));
    }
    DispatchVulkanCompute(shader,{std::uint32_t((size/4+63)/64),1,1},buffers);
    return std::move(buffers[tile?0:1]);
}
}
