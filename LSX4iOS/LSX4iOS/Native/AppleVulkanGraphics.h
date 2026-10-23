// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "AppleGraphicsShader.h"
#include <string>
namespace Lsx4::Apple {
struct DepthClearDrawResult {
  std::vector<std::uint8_t> depth;
  std::string deviceName;
  bool occlusionVisible{};
};
// Synchronous portable render-pass route. Depth clear writes happen on the
// GPU, and the real vertex/fragment programs execute before fenced readback.
DepthClearDrawResult
RenderDepthClearDraw(const CompiledGraphicsShader &shaders,
                     const DepthClearDrawState &state,
                     const std::vector<std::vector<std::uint8_t>> &inputs);
struct ColorDrawResult {
  std::vector<std::uint8_t> color;
  std::string deviceName;
  bool occlusionVisible{};
};
ColorDrawResult
RenderColorDraw(const CompiledGraphicsShader &shaders,
                const ColorDrawState &state,
                const std::vector<std::vector<std::uint8_t>> &buffers,
                const std::vector<std::vector<std::uint8_t>> &textures,
                std::span<const std::uint8_t> initialColor,
                std::span<const std::uint8_t> indices);
} // namespace Lsx4::Apple
