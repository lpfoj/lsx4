// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#include "executor/dynamic_translation/executable_range_registry.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Lsx4::Translation {
namespace {

bool RangeLess(const ExecutableRange& left, const ExecutableRange& right) noexcept {
    if (left.address != right.address) {
        return left.address < right.address;
    }
    if (left.size != right.size) {
        return left.size < right.size;
    }
    return left.range_cookie < right.range_cookie;
}

bool SameRegistration(const ExecutableRange& left,
                      const ExecutableRange& right) noexcept {
    return left.address == right.address && left.size == right.size &&
           left.range_cookie == right.range_cookie;
}

bool RangeEnd(const ExecutableRange& range, std::uint64_t& end) noexcept {
    if (range.size == 0 ||
        range.size > std::numeric_limits<std::uint64_t>::max() - range.address) {
        return false;
    }
    end = range.address + range.size;
    return true;
}

bool NormalizeRegisteredRanges(std::vector<ExecutableRange>& ranges) noexcept {
    try {
        std::sort(ranges.begin(), ranges.end(), RangeLess);
        std::vector<ExecutableRange> normalized{};
        normalized.reserve(ranges.size());
        for (const ExecutableRange& range : ranges) {
            if (!normalized.empty() && range == normalized.back()) {
                continue;
            }
            normalized.push_back(range);
        }
        ranges = std::move(normalized);
    } catch (...) {
        return false;
    }

    for (std::size_t index = 0; index < ranges.size(); ++index) {
        std::uint64_t end = 0;
        if (!RangeEnd(ranges[index], end)) {
            return false;
        }
        if (index != 0) {
            std::uint64_t previous_end = 0;
            if (!RangeEnd(ranges[index - 1], previous_end) ||
                ranges[index].address < previous_end) {
                return false;
            }
        }
        for (std::size_t cookie_index = 0; cookie_index < index; ++cookie_index) {
            if (ranges[cookie_index].range_cookie == ranges[index].range_cookie) {
                return false;
            }
        }
    }
    return true;
}

bool NormalizeInvalidationSpans(std::vector<ExecutableRange>& spans) noexcept {
    try {
        std::sort(spans.begin(), spans.end(), RangeLess);
        spans.erase(std::unique(spans.begin(), spans.end(),
                                [](const ExecutableRange& left,
                                   const ExecutableRange& right) {
                                    return SameRegistration(left, right);
                                }),
                    spans.end());
        return true;
    } catch (...) {
        return false;
    }
}

std::uint64_t NextGeneration(const std::uint64_t generation) noexcept {
    return generation == std::numeric_limits<std::uint64_t>::max()
               ? 0
               : generation + 1u;
}

}

struct ExecutableRangeRegistry::OwnedSnapshot {
    std::unique_ptr<ExecutableRange[]> ranges{};
    ExecutableRangeSnapshot published{};
};

ExecutableRangeRegistry::ExecutableRangeRegistry(
    const TranslationInvalidationSink invalidation_sink) noexcept
    : invalidation_sink_{invalidation_sink} {
    auto empty = BuildSnapshot({}, 0);
    if (empty) {
        (void)PublishLocked(std::move(empty));
    }
}

ExecutableRangeRegistry::~ExecutableRangeRegistry() = default;

bool ExecutableRangeRegistry::ValidateRegisteredRange(
    const ExecutableRange& range) noexcept {
    std::uint64_t end = 0;
    if (range.address == 0 || range.range_cookie == 0 ||
        !RangeEnd(range, end) ||
        (range.range_flags & ~ExecutableRangeKnownFlags) != 0) {
        return false;
    }
    if ((range.range_flags & ExecutableRangeFlagSelfModifying) != 0 &&
        (range.range_flags &
         (ExecutableRangeFlagExecutable | ExecutableRangeFlagWritable)) !=
            (ExecutableRangeFlagExecutable | ExecutableRangeFlagWritable)) {
        return false;
    }
    if ((range.range_flags & ExecutableRangeFlagHleTrampoline) != 0 &&
        (range.range_flags & ExecutableRangeFlagExecutable) == 0) {
        return false;
    }
    if ((range.range_flags & ExecutableRangeFlagHleFramePool) != 0 &&
        (range.range_flags & ExecutableRangeFlagWritable) == 0) {
        return false;
    }
    return true;
}

bool ExecutableRangeRegistry::ValidateSpan(
    const ExecutableRange& range) noexcept {
    std::uint64_t end = 0;
    return range.address != 0 && RangeEnd(range, end) &&
           (range.range_flags & ~ExecutableRangeKnownFlags) == 0;
}

const ExecutableRange* ExecutableRangeRegistry::FindContaining(
    const ExecutableRangeSnapshot& snapshot, const std::uint64_t address,
    const std::size_t byte_count, const std::uint32_t required_flags) noexcept {
    if (snapshot.range_count == 0 || snapshot.ranges == nullptr || byte_count == 0 ||
        (required_flags & ~ExecutableRangeKnownFlags) != 0 ||
        byte_count > std::numeric_limits<std::uint64_t>::max() - address) {
        return nullptr;
    }
    const std::uint64_t end = address + byte_count;
    std::size_t first = 0;
    std::size_t last = snapshot.range_count;
    while (first < last) {
        const std::size_t middle = first + (last - first) / 2;
        if (snapshot.ranges[middle].address <= address) {
            first = middle + 1;
        } else {
            last = middle;
        }
    }
    if (first == 0) {
        return nullptr;
    }
    const ExecutableRange& candidate = snapshot.ranges[first - 1];
    std::uint64_t candidate_end = 0;
    if (!RangeEnd(candidate, candidate_end) || end > candidate_end ||
        (candidate.range_flags & required_flags) != required_flags) {
        return nullptr;
    }
    return &candidate;
}

bool ExecutableRangeRegistry::GenerationMatches(const std::uint64_t expected,
                                                const std::uint64_t actual) noexcept {
    return expected == 0 || expected == actual;
}

std::unique_ptr<ExecutableRangeRegistry::OwnedSnapshot>
ExecutableRangeRegistry::BuildSnapshot(std::vector<ExecutableRange> ranges,
                                       const std::uint64_t generation) const noexcept {
    try {
        auto storage = std::make_unique<OwnedSnapshot>();
        if (!ranges.empty()) {
            storage->ranges = std::make_unique<ExecutableRange[]>(ranges.size());
            std::copy(ranges.begin(), ranges.end(), storage->ranges.get());
        }
        storage->published = {
            .generation = generation,
            .ranges = storage->ranges.get(),
            .range_count = static_cast<std::uint32_t>(ranges.size()),
            .reserved = 0,
        };
        return storage;
    } catch (...) {
        return {};
    }
}

bool ExecutableRangeRegistry::PublishLocked(
    std::unique_ptr<OwnedSnapshot> snapshot) noexcept {
    if (!snapshot) {
        return false;
    }
    const ExecutableRangeSnapshot* published = &snapshot->published;
    try {
        resident_snapshots_.push_back(std::move(snapshot));
    } catch (...) {
        return false;
    }
    active_snapshot_.store(published, std::memory_order_release);
    return true;
}

std::uint64_t ExecutableRangeRegistry::BeginMappingMutation() noexcept {
    const std::uint64_t previous =
        mapping_epoch_.fetch_add(1, std::memory_order_acq_rel);
    return previous + 1u;
}

void ExecutableRangeRegistry::EndMappingMutation() noexcept {
    mapping_epoch_.fetch_add(1, std::memory_order_release);
}

ExecutableRangeStatus ExecutableRangeRegistry::StageInitial(
    const std::span<const ExecutableRange> ranges,
    const std::uint64_t initial_generation) noexcept {
    if (initial_generation == 0 ||
        initial_generation == std::numeric_limits<std::uint64_t>::max() ||
        ranges.size() > std::numeric_limits<std::uint32_t>::max()) {
        return ExecutableRangeStatus::InvalidArgument;
    }
    std::lock_guard lock{update_mutex_};
    if (initial_staged_) {
        return ExecutableRangeStatus::InvalidState;
    }
    std::vector<ExecutableRange> next{};
    try {
        next.assign(ranges.begin(), ranges.end());
    } catch (...) {
        return ExecutableRangeStatus::BackendFailure;
    }
    if (std::any_of(next.begin(), next.end(), [](const ExecutableRange& range) {
            return !ValidateRegisteredRange(range);
        }) ||
        !NormalizeRegisteredRanges(next)) {
        return ExecutableRangeStatus::InvalidArgument;
    }
    auto snapshot = BuildSnapshot(std::move(next), initial_generation);
    if (!snapshot) {
        return ExecutableRangeStatus::BackendFailure;
    }
    (void)BeginMappingMutation();
    if (!PublishLocked(std::move(snapshot))) {
        EndMappingMutation();
        return ExecutableRangeStatus::BackendFailure;
    }
    initial_staged_ = true;
    EndMappingMutation();
    return ExecutableRangeStatus::Ok;
}

ExecutableRangeStatus ExecutableRangeRegistry::BulkRegister(
    const std::span<const ExecutableRange> ranges,
    const std::uint64_t expected_generation,
    std::uint64_t& new_generation) noexcept {
    new_generation = 0;
    if (ranges.size() > std::numeric_limits<std::uint32_t>::max() ||
        std::any_of(ranges.begin(), ranges.end(), [](const ExecutableRange& range) {
            return !ValidateRegisteredRange(range);
        })) {
        return ExecutableRangeStatus::InvalidArgument;
    }
    std::lock_guard lock{update_mutex_};
    const ExecutableRangeSnapshot* current =
        active_snapshot_.load(std::memory_order_acquire);
    if (!initial_staged_ || current == nullptr) {
        return ExecutableRangeStatus::InvalidState;
    }
    new_generation = current->generation;
    if (!GenerationMatches(expected_generation, current->generation)) {
        return ExecutableRangeStatus::InvalidState;
    }
    if (ranges.empty()) {
        return ExecutableRangeStatus::Ok;
    }
    std::vector<ExecutableRange> additions{};
    try {
        additions.assign(ranges.begin(), ranges.end());
    } catch (...) {
        return ExecutableRangeStatus::BackendFailure;
    }
    if (!NormalizeRegisteredRanges(additions)) {
        return ExecutableRangeStatus::InvalidArgument;
    }

    std::vector<ExecutableRange> next{};
    try {
        if (current->range_count != 0) {
            next.assign(current->ranges,
                        current->ranges + current->range_count);
        }
        for (const ExecutableRange& addition : additions) {
            const auto same_cookie = std::find_if(
                next.begin(), next.end(), [&addition](const ExecutableRange& range) {
                    return range.range_cookie == addition.range_cookie;
                });
            if (same_cookie != next.end()) {
                if (*same_cookie != addition) {
                    return ExecutableRangeStatus::InvalidArgument;
                }
                continue;
            }
            next.push_back(addition);
        }
    } catch (...) {
        return ExecutableRangeStatus::BackendFailure;
    }
    if (next.size() == current->range_count) {
        return ExecutableRangeStatus::Ok;
    }
    if (next.size() > std::numeric_limits<std::uint32_t>::max() ||
        !NormalizeRegisteredRanges(next)) {
        return ExecutableRangeStatus::InvalidArgument;
    }
    const std::uint64_t generation = NextGeneration(current->generation);
    if (generation == 0) {
        return ExecutableRangeStatus::InvalidState;
    }
    auto snapshot = BuildSnapshot(std::move(next), generation);
    if (!snapshot) {
        return ExecutableRangeStatus::BackendFailure;
    }
    (void)BeginMappingMutation();
    if (!PublishLocked(std::move(snapshot))) {
        EndMappingMutation();
        return ExecutableRangeStatus::BackendFailure;
    }
    EndMappingMutation();
    new_generation = generation;
    return ExecutableRangeStatus::Ok;
}

ExecutableRangeStatus ExecutableRangeRegistry::Invalidate(
    const std::span<const ExecutableRange> spans,
    const std::uint32_t invalidate_flags,
    const std::uint64_t expected_generation,
    std::uint64_t& new_generation) noexcept {
    new_generation = 0;
    if ((invalidate_flags & ~ExecutableRangeKnownInvalidateFlags) != 0 ||
        spans.size() > std::numeric_limits<std::uint32_t>::max() ||
        std::any_of(spans.begin(), spans.end(), [](const ExecutableRange& span) {
            return !ValidateSpan(span);
        })) {
        return ExecutableRangeStatus::InvalidArgument;
    }
    std::lock_guard lock{update_mutex_};
    const ExecutableRangeSnapshot* current =
        active_snapshot_.load(std::memory_order_acquire);
    if (!initial_staged_ || current == nullptr) {
        return ExecutableRangeStatus::InvalidState;
    }
    new_generation = current->generation;
    if (!GenerationMatches(expected_generation, current->generation)) {
        return ExecutableRangeStatus::InvalidState;
    }
    if (spans.empty()) {
        return ExecutableRangeStatus::Ok;
    }
    std::vector<ExecutableRange> retire{};
    std::vector<ExecutableRange> next{};
    try {
        retire.assign(spans.begin(), spans.end());
        if (current->range_count != 0) {
            next.assign(current->ranges,
                        current->ranges + current->range_count);
        }
    } catch (...) {
        return ExecutableRangeStatus::BackendFailure;
    }
    if (!NormalizeInvalidationSpans(retire)) {
        return ExecutableRangeStatus::BackendFailure;
    }

    if ((invalidate_flags & ExecutableRangeRemoveRegistrations) != 0) {
        for (const ExecutableRange& span : retire) {
            if (span.range_cookie == 0) {
                return ExecutableRangeStatus::InvalidArgument;
            }
            const auto found = std::find_if(
                next.begin(), next.end(), [&span](const ExecutableRange& range) {
                    return SameRegistration(range, span);
                });
            if (found == next.end()) {
                return ExecutableRangeStatus::NotFound;
            }
            next.erase(found);
        }
    }
    const std::uint64_t generation = NextGeneration(current->generation);
    if (generation == 0) {
        return ExecutableRangeStatus::InvalidState;
    }
    auto snapshot = BuildSnapshot(std::move(next), generation);
    if (!snapshot) {
        return ExecutableRangeStatus::BackendFailure;
    }

    const std::uint64_t retiring_epoch = BeginMappingMutation();
    const bool retired = invalidation_sink_.invalidate == nullptr ||
                         invalidation_sink_.invalidate(
                             invalidation_sink_.context, retire.data(), retire.size(),
                             retiring_epoch);
    if (!retired) {
        EndMappingMutation();
        return ExecutableRangeStatus::BackendFailure;
    }
    if (!PublishLocked(std::move(snapshot))) {
        EndMappingMutation();
        return ExecutableRangeStatus::BackendFailure;
    }
    EndMappingMutation();
    new_generation = generation;
    return ExecutableRangeStatus::Ok;
}

const ExecutableRangeSnapshot*
ExecutableRangeRegistry::SnapshotSignalSafe() const noexcept {
    const std::uint64_t before = mapping_epoch_.load(std::memory_order_acquire);
    if ((before & 1u) != 0) {
        return nullptr;
    }
    const ExecutableRangeSnapshot* snapshot =
        active_snapshot_.load(std::memory_order_acquire);
    const std::uint64_t after = mapping_epoch_.load(std::memory_order_acquire);
    return before == after && (after & 1u) == 0 ? snapshot : nullptr;
}

ExecutableRangeLease ExecutableRangeRegistry::AcquireLeaseSignalSafe(
    const std::uint64_t address, const std::size_t byte_count,
    const std::uint32_t required_flags) const noexcept {
    const std::uint64_t before = mapping_epoch_.load(std::memory_order_acquire);
    if ((before & 1u) != 0) {
        return {};
    }
    const ExecutableRangeSnapshot* snapshot =
        active_snapshot_.load(std::memory_order_acquire);
    if (snapshot == nullptr) {
        return {};
    }
    const ExecutableRange* range =
        FindContaining(*snapshot, address, byte_count, required_flags);
    const std::uint64_t after = mapping_epoch_.load(std::memory_order_acquire);
    if (range == nullptr || before != after || (after & 1u) != 0) {
        return {};
    }
    return {snapshot, range, after};
}

bool ExecutableRangeRegistry::LeaseIsCurrent(
    const ExecutableRangeLease& lease) const noexcept {
    if (!lease) {
        return false;
    }
    const std::uint64_t epoch = mapping_epoch_.load(std::memory_order_acquire);
    if (epoch != lease.mapping_epoch || (epoch & 1u) != 0) {
        return false;
    }
    return active_snapshot_.load(std::memory_order_acquire) == lease.snapshot;
}

ExecutableRangeStatus ExecutableRangeRegistry::CopySnapshot(
    const std::span<ExecutableRange> output, std::size_t& required_count,
    std::uint64_t& generation) const noexcept {
    const ExecutableRangeSnapshot* snapshot =
        active_snapshot_.load(std::memory_order_acquire);
    if (snapshot == nullptr) {
        required_count = 0;
        generation = 0;
        return ExecutableRangeStatus::InvalidState;
    }
    required_count = snapshot->range_count;
    generation = snapshot->generation;
    if (output.size() < required_count) {
        return ExecutableRangeStatus::BufferTooSmall;
    }
    if (required_count != 0) {
        std::copy_n(snapshot->ranges, required_count, output.begin());
    }
    return ExecutableRangeStatus::Ok;
}

std::uint64_t ExecutableRangeRegistry::MappingEpoch() const noexcept {
    return mapping_epoch_.load(std::memory_order_acquire);
}

}
