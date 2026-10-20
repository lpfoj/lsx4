// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

namespace Executor {

enum class GuestCpuBackend : std::int32_t {
    Current = 0,
    FexExperimental = 1,
};

constexpr GuestCpuBackend SanitizeGuestCpuBackend(
    const std::int32_t value) noexcept {
    return value == static_cast<std::int32_t>(
                        GuestCpuBackend::FexExperimental)
        ? GuestCpuBackend::FexExperimental
        : GuestCpuBackend::Current;
}

constexpr const char* GuestCpuBackendName(
    const GuestCpuBackend backend) noexcept {
    return backend == GuestCpuBackend::FexExperimental
        ? "fex-experimental"
        : "current-aarch64-jit";
}

}
