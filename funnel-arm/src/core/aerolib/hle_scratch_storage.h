// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cerrno>
#include <cstring>

namespace Core::AeroLib::Detail {

// Apple Current dispatches a registered address before executing these bytes.
// A missed registration must trap, never silently return success to the guest.
inline bool EmitCurrentHleDispatchToken(std::uint8_t* code, std::size_t size) noexcept {
    if (code == nullptr || size < 2) return false;
    std::memset(code, 0xcc, size);
    code[0] = 0x0f;
    code[1] = 0x0b; // UD2
    return true;
}

struct HleScratchStorage {
    void* pool{};
    void* cursor{};
    const char* failed_stage{};
    int error{};
    explicit operator bool() const noexcept { return pool != nullptr && cursor != nullptr; }
};

// The successful result is process-lifetime storage. Before publication every
// allocation is scoped, including when allocating the second mapping throws.
// Allocate/release operate only on fresh mappings owned by this transaction.
template<class Allocate, class Release>
HleScratchStorage AllocateHleScratchStorage(std::size_t pool_bytes,
                                          std::size_t cursor_bytes,
                                          Allocate&& allocate, Release&& release) {
    const auto failed = [](void* value) {
        return value == nullptr || value == reinterpret_cast<void*>(~std::uintptr_t{0});
    };
    struct Mapping {
        void* value{};
        std::size_t bytes{};
        Release& release;
        ~Mapping() { if (value != nullptr) release(value, bytes); }
    };
    void* pool = allocate(pool_bytes);
    if (failed(pool)) return {nullptr, nullptr, "pool", errno};
    Mapping owned_pool{pool, pool_bytes, release};
    void* cursor = allocate(cursor_bytes);
    if (failed(cursor)) return {nullptr, nullptr, "cursor", errno};
    Mapping owned_cursor{cursor, cursor_bytes, release};
    std::memset(cursor, 0, sizeof(std::uint64_t));
    owned_pool.value = nullptr;
    owned_cursor.value = nullptr;
    return {pool, cursor, nullptr, 0};
}

} // namespace Core::AeroLib::Detail
