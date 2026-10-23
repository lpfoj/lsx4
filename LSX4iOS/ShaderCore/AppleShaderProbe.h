// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <vector>
namespace Lsx4::Apple {
// Compiles a bounded, resource-free compute fixture through the unchanged GCN
// frontend and SPIR-V backend. Not an entry point for arbitrary guest shaders.
std::vector<std::uint32_t> CompileShaderProbe();
}
