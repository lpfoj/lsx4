// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
namespace Lsx4::Apple {
class GuestSystemModules {
public:
    void AddLoaded(std::string name);
    std::optional<std::uint32_t> Call(std::string_view identity,std::uint64_t id) const;
private:
    std::set<std::string,std::less<>> loaded_;
};
}
