// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <functional>
#include <vector>
namespace Lsx4::Apple {
struct GuestFrame {
    std::uint32_t width{},height{};
    // Tightly packed RGBA8 with sRGB-encoded color values, row zero at top.
    std::vector<std::uint8_t> rgba;
};
// True means the output consumed the frame; physical visibility is tracked by the platform.
using GuestFrameSink=std::function<bool(const GuestFrame&)>;
}
