// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "AppleComputeShader.h"
#include <string>
namespace Lsx4::Apple {
// Synchronous correctness route. Input bytes are published to Vulkan memory;
// written buffers change only after the GPU fence and readback succeed.
std::string DispatchVulkanCompute(const CompiledComputeShader& shader,
    std::array<std::uint32_t,3> groups,std::vector<std::vector<std::uint8_t>>& buffers);
}
