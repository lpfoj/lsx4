// SPDX-FileCopyrightText: Copyright 2026 LSX4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Scheduler;

void PipelineBarrier(const Scheduler& scheduler, vk::CommandBuffer command_buffer,
                     const vk::DependencyInfo& dependency_info);

} // namespace Vulkan

