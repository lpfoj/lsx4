// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>
namespace Lsx4::Apple {
struct ComputeShaderInput {
    std::uint64_t address{};
    std::span<const std::uint32_t> code;
    std::array<std::uint32_t,16> userData{};
    std::uint32_t rsrc1{},rsrc2{};
    std::array<std::uint32_t,3> workgroup{};
};
struct ComputeBufferBinding {
    std::uint64_t address{},size{};
    bool written{};
    std::vector<std::uint8_t> inlineBytes;
};
struct CompiledComputeShader {
    std::vector<std::uint32_t> spirv;
    std::vector<ComputeBufferBinding> buffers;
    std::array<std::uint8_t,128> pushConstants{};
};
// First portable route: direct buffer descriptors, no host dereference of guest
// SRT/fetch pointers. Unsupported forms fail before the common compiler runs.
CompiledComputeShader CompileComputeShader(const ComputeShaderInput& input);
}
