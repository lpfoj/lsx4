// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "GuestAddressSpace.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wignored-attributes"
#include "core/libraries/pad/pad.h"
#pragma clang diagnostic pop
#include <deque>
#include <optional>
#include <string_view>
namespace Lsx4::Apple {
// Uses the original console ABI. Apple input events are copied into samples;
// a missing input source remains disconnected, without synthesized presses.
class GuestPadInput {
public:
    GuestPadInput();
    void Update(std::uint32_t buttons,std::array<std::uint8_t,6> axes,bool connected,std::uint64_t timestamp, std::optional<Libraries::Pad::OrbisPadTouch> touch = std::nullopt);
    Libraries::Pad::OrbisPadData Current() const;
    std::vector<Libraries::Pad::OrbisPadData> Read(unsigned maximum);
private:
    mutable std::mutex mutex_;
    Libraries::Pad::OrbisPadData current_{};
    std::deque<Libraries::Pad::OrbisPadData> pending_;
};
GuestPadInput& PlatformPadInput();
class GuestPad {
public:
    explicit GuestPad(GuestPadInput& input):input_(input){}
    std::optional<std::uint64_t> Call(std::string_view identity,const Translation::CpuFrame& frame,GuestAddressSpace& memory);
    std::uint64_t samplesRead{},nonNeutralSamplesRead{};
    std::uint32_t buttonsReadMask{};
private:
    GuestPadInput& input_;
    bool initialized_{},opened_{};
    std::array<std::uint8_t,3> lightBar_{0,0,255};
};
}
