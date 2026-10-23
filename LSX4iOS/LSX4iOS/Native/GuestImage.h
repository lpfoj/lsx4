// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include <functional>
#include <optional>

namespace Lsx4::Apple {
struct GuestSegment {
    std::uint64_t address{};
    std::uint32_t flags{};
    std::vector<std::uint8_t> bytes;
};
struct GuestSymbol {
    std::string name;
    std::uint64_t value{}, size{};
    std::uint16_t section{};
    std::uint8_t info{};
};
struct GuestRelocation {
    std::uint64_t offset{};
    std::uint32_t type{}, symbol{};
    std::int64_t addend{};
};
struct GuestImport {
    std::uint16_t id{};
    std::string name;
};
// Validates and loads already decrypted PS4 ELF/SELF segments into guest bytes.
// These are data, never native executable mappings. Relocation/HLE is separate.
class GuestImage {
public:
    static GuestImage Parse(std::span<const std::uint8_t> file);
    std::span<const std::uint8_t> BytesAt(std::uint64_t address) const;
    std::span<std::uint8_t> MutableBytesAt(std::uint64_t address);
    void Relocate(std::uint64_t base,
        const std::function<std::optional<std::uint64_t>(const GuestSymbol&)>& resolver,
        std::uint64_t tlsModuleID = 1);
    std::uint64_t entry{};
    std::uint64_t init{}, processParameters{};
    std::uint16_t type{};
    bool self{}, hasInit{};
    std::vector<GuestSegment> segments;
    std::vector<GuestSymbol> symbols;
    std::vector<GuestRelocation> relocations;
    std::vector<GuestImport> importedModules, importedLibraries;
    std::vector<GuestImport> exportedModules, exportedLibraries;
    std::vector<std::string> needed;
    std::vector<std::uint8_t> tlsInitial;
    std::uint64_t tlsBytes{}, tlsAlignment{1};
private:
    bool relocated_{};
};
}
