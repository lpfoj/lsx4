// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
namespace Lsx4::Apple {
struct VulkanProbeResult {
    bool passed{},shaderExecuted{};
    std::string device,error;
    std::uint32_t apiVersion{},verifiedPixels{},shaderWords{};
};
// Tests GPU submission and host readback only. It does not emulate a guest GPU
// command, present a game frame, or mark the game runtime playable.
VulkanProbeResult ProbeVulkanTransfer();
}
