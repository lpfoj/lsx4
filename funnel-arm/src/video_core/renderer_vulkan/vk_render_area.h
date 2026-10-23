// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

// With no attachments, their dimensions cannot bound the render area. All
// rasterized fragments are still constrained by one of the active scissors.
// Keep the origin at zero and include every scissor, including its offset.
// Do not use this for attached passes: load/store and clear coverage matters.
inline vk::Extent2D AttachmentlessRenderExtent(std::span<const vk::Rect2D> scissors,
                                              vk::Extent2D maximum) {
    if (scissors.empty()) {
        return maximum;
    }
    std::int64_t right = 1;
    std::int64_t bottom = 1;
    for (const auto& scissor : scissors) {
        if (scissor.extent.width == 0 || scissor.extent.height == 0) {
            continue;
        }
        right = std::max(right, std::int64_t{scissor.offset.x} + scissor.extent.width);
        bottom = std::max(bottom, std::int64_t{scissor.offset.y} + scissor.extent.height);
    }
    return {static_cast<std::uint32_t>(std::min<std::int64_t>(maximum.width, right)),
            static_cast<std::uint32_t>(std::min<std::int64_t>(maximum.height, bottom))};
}

} // namespace Vulkan
