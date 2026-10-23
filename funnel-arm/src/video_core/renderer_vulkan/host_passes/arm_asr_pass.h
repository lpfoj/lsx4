// SPDX-FileCopyrightText: Copyright 2026 LSX4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "common/types.h"
#include "video_core/renderer_vulkan/host_passes/sgsr2_pass.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;
}

namespace Vulkan::HostPasses {

/// Host integration for Arm Accuracy Super Resolution's official generic
/// Vulkan backend. Temporal inputs are supplied by the renderer contract or by
/// Sgsr2Pass's emulator-owned optical reconstruction path.
class ArmAsrPass {
public:
    ArmAsrPass();
    ~ArmAsrPass();
    ArmAsrPass(const ArmAsrPass&) = delete;
    ArmAsrPass& operator=(const ArmAsrPass&) = delete;

    void Create(const Instance& instance, VmaAllocator allocator, u32 num_images);
    void Destroy();
    vk::ImageView Render(Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                         vk::Extent2D render_size, vk::Extent2D display_size,
                         const Sgsr2Pass::PreparedTemporalInputs& inputs,
                         float jitter_x = 0.f, float jitter_y = 0.f,
                         bool hdr = false, bool enable_sharpening = true,
                         float sharpness = 0.25f);

    [[nodiscard]] bool IsSupported() const noexcept;
    [[nodiscard]] bool LastDispatchSucceeded() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan::HostPasses
