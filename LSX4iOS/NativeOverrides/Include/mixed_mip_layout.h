// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <bit>
#include <cstdint>

namespace PortableGpu {
// Evaluate the unaligned mip dimensions, before padding can obscure the choice.
// Call only for a macro-tiled surface; levels are limited by the 16-entry ABI.
constexpr std::uint32_t MixedMacroMipMask(std::uint32_t pitch,
    std::uint32_t height, std::uint32_t levels, std::uint32_t macro_pitch,
    std::uint32_t macro_height, bool block_compressed) noexcept {
    std::uint32_t mask = 0;
    for (std::uint32_t mip = 1; mip < std::min(levels, 16u); ++mip) {
        auto w = pitch >> mip;
        auto h = height >> mip;
        if (block_compressed) {
            w = (w + 3) / 4;
            h = (h + 3) / 4;
        }
        w = std::bit_ceil(std::max(w, 1u));
        h = std::bit_ceil(std::max(h, 1u));
        if (w < macro_pitch || h < macro_height) mask |= 1u << mip;
    }
    return mask;
}
}
