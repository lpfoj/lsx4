// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "AppleGraphicsShader.h"
#include <functional>
namespace Lsx4::Apple {
struct ResolvedVertexFetch {
  GpuRegisters registers;
  std::vector<std::uint32_t> code;
  std::uint32_t descriptorLoads{}, fetchBytes{};
};
using GpuMemoryReader =
    std::function<bool(std::uint64_t, std::span<std::uint8_t>)>;
// Resolve a bounded, straight-line fetch prologue through guest GPU reads.
// Hoisted descriptors become user data in the common compiler, preserving
// ordinary buffer tracking without dereferencing a guest pointer on the host.
ResolvedVertexFetch
ResolveVertexFetch(const GpuRegisters &registers,
                   std::span<const std::uint32_t> vertexCode,
                   const GpuMemoryReader &read);
} // namespace Lsx4::Apple
