// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <type_traits>
#include <vector>

namespace Lsx4::Translation {

inline constexpr std::uint32_t ExecutableRangeFlagExecutable = 1u << 0u;
inline constexpr std::uint32_t ExecutableRangeFlagWritable = 1u << 1u;
inline constexpr std::uint32_t ExecutableRangeFlagHleTrampoline = 1u << 2u;
inline constexpr std::uint32_t ExecutableRangeFlagSelfModifying = 1u << 3u;
inline constexpr std::uint32_t ExecutableRangeFlagHleFramePool = 1u << 4u;
inline constexpr std::uint32_t ExecutableRangeKnownFlags =
    ExecutableRangeFlagExecutable | ExecutableRangeFlagWritable |
    ExecutableRangeFlagHleTrampoline | ExecutableRangeFlagSelfModifying |
    ExecutableRangeFlagHleFramePool;

inline constexpr std::uint32_t ExecutableRangeInvalidateTranslations = 1u << 0u;
inline constexpr std::uint32_t ExecutableRangeRemoveRegistrations = 1u << 1u;
inline constexpr std::uint32_t ExecutableRangeKnownInvalidateFlags =
    ExecutableRangeInvalidateTranslations | ExecutableRangeRemoveRegistrations;

struct ExecutableRange {
    std::uint64_t address{};
    std::uint64_t size{};
    std::uint64_t range_cookie{};
    std::uint32_t protection{};
    std::uint32_t range_flags{};

    friend constexpr bool operator==(const ExecutableRange&,
                                     const ExecutableRange&) = default;
};

static_assert(sizeof(ExecutableRange) == 32);
static_assert(alignof(ExecutableRange) == 8);
static_assert(std::is_standard_layout_v<ExecutableRange>);
static_assert(std::is_trivially_copyable_v<ExecutableRange>);






struct ExecutableRangeSnapshot {
    std::uint64_t generation{};
    const ExecutableRange* ranges{};
    std::uint32_t range_count{};
    std::uint32_t reserved{};
};

static_assert(sizeof(ExecutableRangeSnapshot) == 24);
static_assert(std::is_standard_layout_v<ExecutableRangeSnapshot>);
static_assert(std::is_trivially_copyable_v<ExecutableRangeSnapshot>);

struct ExecutableRangeLease {
    const ExecutableRangeSnapshot* snapshot{};
    const ExecutableRange* range{};
    std::uint64_t mapping_epoch{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return snapshot != nullptr && range != nullptr &&
               (mapping_epoch & 1u) == 0;
    }
};

enum class ExecutableRangeStatus : std::int32_t {
    Ok = 0,
    InvalidArgument = -1,
    InvalidState = -4,
    NotFound = -5,
    BufferTooSmall = -6,
    BackendFailure = -8,
};







struct TranslationInvalidationSink {
    using Function = bool (*)(void* context, const ExecutableRange* spans,
                              std::size_t span_count,
                              std::uint64_t retiring_mapping_epoch) noexcept;

    void* context{};
    Function invalidate{};
};

class ExecutableRangeRegistry {
public:
    explicit ExecutableRangeRegistry(
        TranslationInvalidationSink invalidation_sink = {}) noexcept;
    ~ExecutableRangeRegistry();

    ExecutableRangeRegistry(const ExecutableRangeRegistry&) = delete;
    ExecutableRangeRegistry& operator=(const ExecutableRangeRegistry&) = delete;


    [[nodiscard]] ExecutableRangeStatus StageInitial(
        std::span<const ExecutableRange> ranges,
        std::uint64_t initial_generation) noexcept;


    [[nodiscard]] ExecutableRangeStatus BulkRegister(
        std::span<const ExecutableRange> ranges,
        std::uint64_t expected_generation,
        std::uint64_t& new_generation) noexcept;






    [[nodiscard]] ExecutableRangeStatus Invalidate(
        std::span<const ExecutableRange> spans, std::uint32_t invalidate_flags,
        std::uint64_t expected_generation,
        std::uint64_t& new_generation) noexcept;


    [[nodiscard]] const ExecutableRangeSnapshot* SnapshotSignalSafe() const noexcept;
    [[nodiscard]] ExecutableRangeLease AcquireLeaseSignalSafe(
        std::uint64_t address, std::size_t byte_count,
        std::uint32_t required_flags = 0) const noexcept;
    [[nodiscard]] bool LeaseIsCurrent(
        const ExecutableRangeLease& lease) const noexcept;


    [[nodiscard]] ExecutableRangeStatus CopySnapshot(
        std::span<ExecutableRange> output, std::size_t& required_count,
        std::uint64_t& generation) const noexcept;

    [[nodiscard]] std::uint64_t MappingEpoch() const noexcept;

private:
    struct OwnedSnapshot;

    [[nodiscard]] static bool ValidateRegisteredRange(
        const ExecutableRange& range) noexcept;
    [[nodiscard]] static bool ValidateSpan(const ExecutableRange& range) noexcept;
    [[nodiscard]] static const ExecutableRange* FindContaining(
        const ExecutableRangeSnapshot& snapshot, std::uint64_t address,
        std::size_t byte_count, std::uint32_t required_flags) noexcept;
    [[nodiscard]] static bool GenerationMatches(
        std::uint64_t expected, std::uint64_t actual) noexcept;
    [[nodiscard]] std::unique_ptr<OwnedSnapshot> BuildSnapshot(
        std::vector<ExecutableRange> ranges,
        std::uint64_t generation) const noexcept;
    [[nodiscard]] bool PublishLocked(
        std::unique_ptr<OwnedSnapshot> snapshot) noexcept;
    [[nodiscard]] std::uint64_t BeginMappingMutation() noexcept;
    void EndMappingMutation() noexcept;

    TranslationInvalidationSink invalidation_sink_{};
    mutable std::mutex update_mutex_{};
    std::vector<std::unique_ptr<OwnedSnapshot>> resident_snapshots_{};
    std::atomic<const ExecutableRangeSnapshot*> active_snapshot_{nullptr};
    std::atomic<std::uint64_t> mapping_epoch_{0};
    bool initial_staged_{};
};

}
