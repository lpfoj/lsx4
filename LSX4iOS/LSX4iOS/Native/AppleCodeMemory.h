// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <span>
#include <string>

namespace Lsx4::Apple {
// Large shared Current traces exceed 256 MiB on real titles. This is a
// ceiling, not the default reservation: prepare only the requested capacity.
inline constexpr std::size_t ExecutablePoolLimitBytes = 512 * 1024 * 1024;
// Prepare bounded process-owned code storage while attached, before gameplay.
// Later thread arenas claim disjoint ranges without another debugger stop.
bool PrepareExecutableArenaPool(std::size_t bytes, std::string& error);
std::size_t PreparedExecutableArenaPoolClaimedBytes() noexcept;
// One RX mapping and one RW alias; never expose an RWX mapping.
// For development on a paired device with Xcode's debugger attached.
class CodeMemory final {
public:
    CodeMemory() = default;
    ~CodeMemory();
    CodeMemory(const CodeMemory&) = delete;
    CodeMemory& operator=(const CodeMemory&) = delete;
    bool Allocate(std::size_t bytes, std::string& error);
    bool Publish(std::size_t bytes, std::string& error);
    std::span<std::byte> Writable() { return {static_cast<std::byte*>(writable_), size_}; }
    const void* Executable() const { return executable_; }
private:
    void* executable_{};
    void* writable_{};
    std::size_t size_{};
};
}
