// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <vector>
namespace Lsx4::Apple {
// Single-sample 32-bit 2D surfaces, base PS4 tiling. The mathematical shader
// body and tile geometry are compiled from the common renderer sources.
std::vector<std::uint32_t> CompileSurfaceShader32(std::uint32_t tileMode,bool tile);
}
