// SPDX-FileCopyrightText: Copyright 2026 LSX4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "executor/dynamic_translation/runtime_gateway.h"
#include "executor/fex_backend_abi_v3.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Executor::Ps5Fex {

inline constexpr std::size_t HleGuestStubSize = 0x100;
inline constexpr std::size_t HleThunkIdentitySize = 32;
inline constexpr std::size_t CurrentHleThunkIdentityOffset = 116;
inline constexpr std::size_t CurrentHleGuestStubEmittedSize =
    CurrentHleThunkIdentityOffset + HleThunkIdentitySize;
inline constexpr std::size_t HleThunkIdentityOffset = 136;
inline constexpr std::size_t HleGuestEpilogueOffset =
    HleThunkIdentityOffset + HleThunkIdentitySize;
inline constexpr std::size_t HleGuestStubEmittedSize = 225;
static_assert(HleGuestStubEmittedSize <= HleGuestStubSize);
static_assert(CurrentHleGuestStubEmittedSize <= HleGuestStubSize);




[[nodiscard]] std::size_t BuildCurrentHleGuestStub(
    std::span<std::uint8_t, HleGuestStubSize> code, std::uint64_t thunk,
    std::span<const std::uint8_t, HleThunkIdentitySize> identity) noexcept;

[[nodiscard]] std::size_t BuildHleGuestStub(
    std::span<std::uint8_t, HleGuestStubSize> code, std::uint64_t thunk,
    std::span<const std::uint8_t, HleThunkIdentitySize> identity) noexcept;

[[nodiscard]] bool BuildHleBridgeRequest(const Lsx4FexHleCallFrameV3& source,
                                         Lsx4::Translation::HleBridgeRequest& destination) noexcept;

void CompleteHleResult(const Lsx4FexHleCallFrameV3& source, std::uint64_t value, int fp_result_kind,
                       Lsx4FexHleResultFrameV3& destination) noexcept;

[[nodiscard]] std::uint64_t StableRangeCookie(std::uint64_t address, std::uint64_t size,
                                              std::uint32_t protection,
                                              std::uint32_t range_flags) noexcept;

struct ExecutableRangeDelta {
    std::vector<Lsx4FexExecutableRangeV5> removed;
    std::vector<Lsx4FexExecutableRangeV5> added;
};

[[nodiscard]] ExecutableRangeDelta PlanExecutableRangeUpdate(
    std::span<const Lsx4FexExecutableRangeV5> published,
    std::span<const Lsx4FexExecutableRangeV5> current);

}
