// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <compare>
#include <cstdint>

namespace PortableGpu {
// Exact inputs to the tiling compute shader's compile recipe. Dimensions,
// pitch and mip offsets are runtime parameters, so they do not belong here.
struct TilingPipelineIdentity {
    std::uint32_t tile_mode, bits_per_pixel, samples, array_mode;
    bool micro_tiled_mips, tiler, explicit_bounds;
    auto operator<=>(const TilingPipelineIdentity&) const = default;
};
}
