// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <limits>

namespace Lsx4::FexHleLease {

inline constexpr std::uint32_t ProtocolVersion = 2;
inline constexpr std::size_t StubCodeBytes = 1024;
inline constexpr std::size_t MinimumStubSpareBytes = 128;
inline constexpr std::size_t StubEmissionCanaryBytes = 64;
inline constexpr std::uint64_t PoolSize = UINT64_C(256) * 1024 * 1024;
inline constexpr std::uint32_t SlotSize = 0x1000;
inline constexpr std::uint32_t SlotCount =
    static_cast<std::uint32_t>(PoolSize / SlotSize);
inline constexpr std::uint32_t FrameOffset = 0x800;
inline constexpr std::uint32_t FrameBytes = 0xa0;
inline constexpr std::uint32_t OriginalReturnOffset = 0x80;
inline constexpr std::uint32_t GuardPointerOffset = 0x88;
inline constexpr std::uint32_t LeaseTokenOffset = 0x90;
inline constexpr std::uint32_t StubBeginOffset = 0x98;
inline constexpr std::uint32_t MaxTokenAllocateAttempts = SlotCount;
inline constexpr std::uint32_t MaxAcquireAttempts = SlotCount;
inline constexpr std::uint32_t MaxNestedLeases = 64;

constexpr std::uint64_t FingerprintWord(std::uint64_t hash,
                                        std::uint64_t word) noexcept {
    for (std::uint32_t byte = 0; byte < 8; ++byte) {
        hash ^= word & UINT64_C(0xff);
        hash *= UINT64_C(1099511628211);
        word >>= 8;
    }
    return hash;
}

constexpr std::uint64_t ComputeProtocolFingerprint() noexcept {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    hash = FingerprintWord(hash, ProtocolVersion);
    hash = FingerprintWord(hash, StubCodeBytes);
    hash = FingerprintWord(hash, PoolSize);
    hash = FingerprintWord(hash, SlotSize);
    hash = FingerprintWord(hash, FrameOffset);
    hash = FingerprintWord(hash, FrameBytes);
    hash = FingerprintWord(hash, OriginalReturnOffset);
    hash = FingerprintWord(hash, GuardPointerOffset);
    hash = FingerprintWord(hash, LeaseTokenOffset);
    hash = FingerprintWord(hash, StubBeginOffset);
    hash = FingerprintWord(hash, MaxTokenAllocateAttempts);
    hash = FingerprintWord(hash, MaxAcquireAttempts);
    hash = FingerprintWord(hash, MaxNestedLeases);
    return hash;
}

inline constexpr std::uint64_t ProtocolFingerprint =
    ComputeProtocolFingerprint();



inline constexpr std::uint32_t ManifestProtocolTag =
    UINT32_C(0x4c000000) | (ProtocolVersion << 16) |
    static_cast<std::uint32_t>(ProtocolFingerprint & UINT64_C(0xffff));
inline constexpr std::uint64_t ManifestProtocolMask =
    UINT64_C(0xffffffff00000000);
inline constexpr std::uint64_t ManifestOrdinalMask = UINT64_C(0xffffffff);
inline constexpr std::uint64_t ManifestGenerationSeed =
    static_cast<std::uint64_t>(ManifestProtocolTag) << 32;
inline constexpr std::uint64_t InitialManifestGeneration =
    ManifestGenerationSeed | UINT64_C(1);

struct alignas(std::uint64_t) FrameMetadata {
    std::uint64_t original_return{};
    std::uint64_t guard_pointer{};
    std::uint64_t lease_token{};
    std::uint64_t stub_begin{};
};

struct PoolGeometry {
    std::uint64_t base{};
    std::uint64_t size{PoolSize};
    std::uint32_t slot_size{SlotSize};
    std::uint32_t frame_offset{FrameOffset};
};

struct CombinedManifestGeneration {
    std::uint64_t funnel_source{};
    std::uint64_t box64_source{};
    std::uint64_t publication{};
};

enum class CleanupResult : std::uint8_t {
    Released,
    AlreadyReleased,
    InvalidMetadata,
    StaleOwner,
};

inline void ClearStackFallbackMetadata(FrameMetadata& metadata) noexcept {
    metadata = {};
}

inline bool TryAllocateToken(std::atomic<std::uint64_t>& counter,
                             std::uint64_t& token,
                             std::uint32_t* attempts_out = nullptr) noexcept {
    token = 0;
    if (attempts_out != nullptr) {
        *attempts_out = 0;
    }
    for (std::uint32_t attempt = 0;
         attempt < MaxTokenAllocateAttempts; ++attempt) {
        const std::uint32_t attempts_used = attempt + 1;
        std::uint64_t current = counter.load(std::memory_order_relaxed);
        if (current == std::numeric_limits<std::uint64_t>::max()) {
            if (attempts_out != nullptr) {
                *attempts_out = attempts_used;
            }
            return false;
        }
        const std::uint64_t next = current + 1;
        if (counter.compare_exchange_weak(current, next,
                                          std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {
            token = next;
            if (attempts_out != nullptr) {
                *attempts_out = attempts_used;
            }
            return true;
        }
    }
    if (attempts_out != nullptr) {
        *attempts_out = MaxTokenAllocateAttempts;
    }
    return false;
}

inline constexpr bool IsCurrentManifestGeneration(
    const std::uint64_t generation) noexcept {
    return generation != 0 &&
           (generation & ManifestProtocolMask) == ManifestGenerationSeed &&
           (generation & ManifestOrdinalMask) != 0;
}

inline bool TryAdvanceManifestGeneration(std::uint64_t& generation) noexcept {
    if (generation == 0) {
        generation = InitialManifestGeneration;
        return true;
    }
    if (!IsCurrentManifestGeneration(generation) ||
        (generation & ManifestOrdinalMask) == ManifestOrdinalMask) {
        return false;
    }
    ++generation;
    return true;
}

inline bool IsCombinedManifestGeneration(
    const CombinedManifestGeneration& generation) noexcept {
    return IsCurrentManifestGeneration(generation.funnel_source) &&
           IsCurrentManifestGeneration(generation.box64_source) &&
           IsCurrentManifestGeneration(generation.publication);
}






inline bool ComposeCombinedManifestGeneration(
    CombinedManifestGeneration& state, const std::uint64_t funnel_source,
    const std::uint64_t box64_source, std::uint64_t& publication) noexcept {
    publication = 0;
    if (!IsCurrentManifestGeneration(funnel_source) ||
        !IsCurrentManifestGeneration(box64_source)) {
        return false;
    }
    if (state.publication == 0) {
        if (state.funnel_source != 0 || state.box64_source != 0) {
            return false;
        }
        state = {funnel_source, box64_source, InitialManifestGeneration};
        publication = state.publication;
        return true;
    }
    if (!IsCombinedManifestGeneration(state) ||
        funnel_source < state.funnel_source ||
        box64_source < state.box64_source) {
        return false;
    }
    if (funnel_source == state.funnel_source &&
        box64_source == state.box64_source) {
        publication = state.publication;
        return true;
    }
    std::uint64_t next_publication = state.publication;
    if (!TryAdvanceManifestGeneration(next_publication)) {
        return false;
    }
    state = {funnel_source, box64_source, next_publication};
    publication = state.publication;
    return true;
}

inline bool TryEnterDepth(std::uint32_t& depth) noexcept {
    if (depth >= MaxNestedLeases) {
        return false;
    }
    ++depth;
    return true;
}

inline void LeaveDepth(std::uint32_t& depth) noexcept {
    if (depth != 0) {
        --depth;
    }
}

inline bool IsExactPoolFrame(const PoolGeometry& pool,
                             const std::uint64_t frame_address,
                             const std::uint64_t guard_address) noexcept {
    if (pool.base == 0 || pool.size == 0 || pool.slot_size == 0 ||
        (pool.slot_size & (pool.slot_size - 1)) != 0 ||
        pool.size % pool.slot_size != 0 ||
        pool.base > std::numeric_limits<std::uint64_t>::max() - pool.size ||
        guard_address < pool.base || guard_address >= pool.base + pool.size ||
        (guard_address - pool.base) % pool.slot_size != 0 ||
        guard_address > std::numeric_limits<std::uint64_t>::max() -
                            pool.frame_offset) {
        return false;
    }
    return frame_address == guard_address + pool.frame_offset;
}

inline bool ValidateOwnedFrame(const PoolGeometry& pool,
                               const std::uint64_t frame_address,
                               const FrameMetadata& metadata,
                               const std::uint64_t expected_stub_begin) noexcept {
    if (!IsExactPoolFrame(pool, frame_address, metadata.guard_pointer) ||
        metadata.lease_token == 0 || expected_stub_begin == 0 ||
        metadata.stub_begin != expected_stub_begin) {
        return false;
    }
    auto& guard = *reinterpret_cast<std::uint64_t*>(metadata.guard_pointer);
    return std::atomic_ref<std::uint64_t>(guard).load(std::memory_order_acquire) ==
           metadata.lease_token;
}

inline CleanupResult CleanupOwnedFrame(
    const PoolGeometry& pool, const std::uint64_t frame_address,
    FrameMetadata& metadata, const std::uint64_t expected_stub_begin) noexcept {
    if (metadata.guard_pointer == 0 && metadata.lease_token == 0 &&
        metadata.stub_begin == 0) {
        return CleanupResult::AlreadyReleased;
    }
    if (!IsExactPoolFrame(pool, frame_address, metadata.guard_pointer) ||
        metadata.lease_token == 0 || expected_stub_begin == 0 ||
        metadata.stub_begin != expected_stub_begin) {
        return CleanupResult::InvalidMetadata;
    }
    auto& guard = *reinterpret_cast<std::uint64_t*>(metadata.guard_pointer);
    std::uint64_t expected = metadata.lease_token;
    if (!std::atomic_ref<std::uint64_t>(guard).compare_exchange_strong(
            expected, 0, std::memory_order_release,
            std::memory_order_relaxed)) {
        return CleanupResult::StaleOwner;
    }
    metadata.guard_pointer = 0;
    metadata.lease_token = 0;
    metadata.stub_begin = 0;
    return CleanupResult::Released;
}

inline bool TryAcquireBounded(std::uint64_t* guards,
                              const std::uint32_t slot_count,
                              std::atomic<std::uint64_t>& cursor,
                              const std::uint64_t token,
                              std::uint32_t& acquired_slot,
                              std::uint32_t& attempts) noexcept {
    attempts = 0;
    acquired_slot = slot_count;
    if (guards == nullptr || slot_count == 0 || token == 0) {
        return false;
    }
    for (; attempts < slot_count; ++attempts) {
        const auto index = static_cast<std::uint32_t>(
            cursor.fetch_add(1, std::memory_order_relaxed) % slot_count);
        std::uint64_t expected = 0;
        if (std::atomic_ref<std::uint64_t>(guards[index])
                .compare_exchange_weak(expected, token,
                                       std::memory_order_acq_rel,
                                       std::memory_order_relaxed)) {
            acquired_slot = index;
            ++attempts;
            return true;
        }
    }
    return false;
}

static_assert(PoolSize == UINT64_C(0x10000000));
static_assert(SlotCount == 65536);
static_assert(MaxTokenAllocateAttempts == SlotCount);
static_assert(ProtocolVersion == 2);
static_assert(ProtocolVersion <= 0xff);
static_assert(ProtocolFingerprint != 0);
static_assert(ManifestProtocolTag != 0);
static_assert(IsCurrentManifestGeneration(InitialManifestGeneration));
static_assert(!IsCurrentManifestGeneration(1));
static_assert((StubCodeBytes & (StubCodeBytes - 1)) == 0);
static_assert(StubCodeBytes >= 1024);
static_assert(StubCodeBytes > MinimumStubSpareBytes);
static_assert((SlotSize & (SlotSize - 1)) == 0);
static_assert(SlotSize % std::atomic_ref<std::uint64_t>::required_alignment == 0);
static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free,
              "HLE lease guards must not call a runtime lock from guest code");
static_assert(FrameOffset + FrameBytes <= SlotSize);
static_assert(StubBeginOffset + sizeof(std::uint64_t) == FrameBytes);
static_assert(offsetof(FrameMetadata, original_return) == 0);
static_assert(offsetof(FrameMetadata, guard_pointer) ==
              GuardPointerOffset - OriginalReturnOffset);
static_assert(offsetof(FrameMetadata, lease_token) ==
              LeaseTokenOffset - OriginalReturnOffset);
static_assert(offsetof(FrameMetadata, stub_begin) ==
              StubBeginOffset - OriginalReturnOffset);
static_assert(sizeof(FrameMetadata) == FrameBytes - OriginalReturnOffset);

}
