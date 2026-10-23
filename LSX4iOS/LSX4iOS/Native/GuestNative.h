// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "executor/dynamic_translation/native_compiler.h"
#include <unordered_map>

namespace Lsx4::Apple {
enum class NativeStep {Fallback,Executed,Mismatch};
class GuestNative {
    struct Entry;
public:
    // Owned by one decoded instruction. Reset whenever its guest encoding
    // changes; the runtime verifies executable bytes before every dispatch.
    struct Dispatch {
    private:
        friend class GuestNative;
        Entry* entry{};
        bool prepared{};
    };
    explicit GuestNative(bool enabled,bool validateEveryExecution=true):enabled_(enabled),validateEveryExecution_(validateEveryExecution) {entries_.reserve(16384);}
    NativeStep Execute(const Translation::Instruction& instruction,Translation::CpuFrame& frame,
                       std::uint64_t& next,Dispatch& dispatch);
    std::uint64_t executions{}, compilations{}, mismatches{}, checkedExecutions{};
private:
    struct Entry {
        std::array<std::uint8_t,Translation::MaximumInstructionBytes> bytes{};
        std::uint8_t length{};
        Translation::NativeArtifact artifact{};
        bool validated{};
    };
    bool enabled_{};
    bool validateEveryExecution_{true};
    Translation::ExecutableArena arena_{128*1024};
    std::unordered_map<std::uint64_t,Entry> entries_;
};
}
