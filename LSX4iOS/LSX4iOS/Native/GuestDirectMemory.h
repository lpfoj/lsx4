// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "GuestAddressSpace.h"
#include <map>
#include <optional>
namespace Lsx4::Apple {
class GuestDirectMemory {
public:
    void Configure(std::uint64_t processParameters,GuestAddressSpace& memory);
    std::optional<std::uint64_t> Call(std::string_view identity,const Translation::CpuFrame& frame,GuestAddressSpace& memory);
    std::uint64_t Size() const {return size_;}
private:
    struct Block {
        std::uint64_t size{},offset{};
        std::uint32_t type{};
        std::shared_ptr<std::vector<std::uint8_t>> storage;
    };
    std::map<std::uint64_t,Block> blocks_;
    std::uint64_t size_{4608ull*1024*1024},nextVirtual_{0x2000000000};
    std::uint32_t sdk_{};
    bool aliasing_{};
};
}
