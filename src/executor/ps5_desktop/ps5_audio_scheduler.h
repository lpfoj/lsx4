// SPDX-FileCopyrightText: Copyright 2026 LSX4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace Lsx4::Ps5Desktop {

struct Ps5AudioPortConfig {
    std::uint32_t buffer_frames{};
    std::uint32_t sample_rate{};
    std::int32_t format{};
    std::uint32_t guest_channels{};
    std::uint32_t bytes_per_sample{};
    std::int32_t port_type{};
};

struct Ps5AudioPort;
using Ps5AudioPortHandle = std::shared_ptr<Ps5AudioPort>;
using Ps5AudioGuestReadCallback =
    bool (*)(std::uint64_t address, void* destination,
             std::size_t byte_count);



void SetPs5AudioSessionPaused(bool paused) noexcept;



[[nodiscard]] Ps5AudioPortHandle OpenPs5AudioPort(
    const Ps5AudioPortConfig& config);



void ClosePs5AudioPort(Ps5AudioPortHandle& port) noexcept;



[[nodiscard]] bool QueuePs5AudioBuffer(
    const Ps5AudioPortHandle& port, std::uint64_t source_address,
    float gain, std::uint64_t sequence,
    Ps5AudioGuestReadCallback read_guest) noexcept;

}
