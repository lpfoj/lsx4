// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <span>
#include <vector>
namespace Lsx4::Apple {
std::vector<std::uint8_t> TransformSurface32(std::uint32_t tileMode,std::uint32_t pitch,
    std::uint32_t height,std::span<const std::uint8_t> input,bool tile);
}
