// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "GuestFilesystem.h"
#include <array>
namespace Lsx4::Apple {
class GuestSaveData {
public:
    // The anchor is the app's Documents directory (a temporary fixture in tests).
    explicit GuestSaveData(std::filesystem::path anchor,std::string title);
    ~GuestSaveData();
    GuestSaveData(const GuestSaveData&)=delete;
    GuestSaveData& operator=(const GuestSaveData&)=delete;
    std::optional<std::uint32_t> Call(std::string_view identity,std::uint64_t first,
        std::uint64_t second,GuestAddressSpace& memory,GuestFilesystem& filesystem);
private:
    struct Slot {std::string directory,mount;};
    std::array<Slot,16> slots_;
    std::filesystem::path anchor_;
    std::string title_;
    bool initialized_{};
    int OpenRoot(bool create);
};
}
