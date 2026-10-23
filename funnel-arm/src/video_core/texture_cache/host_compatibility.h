// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Copyright © 2015-2023 The Khronos Group Inc.
// Copyright © 2015-2023 Valve Corporation
// Copyright © 2015-2023 LunarG, Inc.

#pragma once

#include <span>
#include "video_core/renderer_vulkan/vk_common.h"

namespace VideoCore {

bool IsVulkanFormatCompatible(vk::Format base, vk::Format view);

// A detached presentation source must retain numeric and channel interpretation.
// Vulkan's 32-bit storage class also admits SNORM/UINT/float and is insufficient.
constexpr bool IsVideoOutUnormSrgbPair(vk::Format source, vk::Format scanout) {
    const auto family = [](vk::Format format) {
        switch (format) {
        case vk::Format::eR8G8B8A8Unorm:
        case vk::Format::eR8G8B8A8Srgb: return 1;
        case vk::Format::eB8G8R8A8Unorm:
        case vk::Format::eB8G8R8A8Srgb: return 2;
        default: return 0;
        }
    };
    return family(source) != 0 && family(source) == family(scanout);
}

// The complete alias domain accepted by IsVulkanFormatCompatible for a base
// format. The returned storage is immutable and process-lifetime stable.
std::span<const vk::Format> VulkanCompatibleViewFormats(vk::Format base);

}
