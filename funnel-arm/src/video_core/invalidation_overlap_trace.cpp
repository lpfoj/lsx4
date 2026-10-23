// SPDX-FileCopyrightText: Copyright 2026 LSX4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/invalidation_overlap_trace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <type_traits>

#ifdef __ANDROID__
#include <android/log.h>
#include <unistd.h>
#endif

namespace VideoCore::InvalidationOverlapTrace {
namespace {

constexpr std::uint32_t TraceAbiVersion = 1;
constexpr std::uint32_t TraceAbiVersionV2 = 2;
constexpr std::uint32_t ClockSteadyNanoseconds = 1;
// A delayed 14-second capture needs roughly 300-400k WAIT+DRAIN records in the
// current heavy scene. 2^19 leaves margin without perturbing the app with the
// previous 32 MiB resident diagnostic buffer.
constexpr std::uint32_t TraceCapacityEntries = 1u << 19;
constexpr std::uint64_t TraceFeatures = FeatureWaitSpans | FeatureDrainSpans;
// V2 emits an explicit request-batch edge in addition to each wait and drain.
// 2^20 keeps a typical 14-second heavy capture non-wrapping; this 48 MiB buffer
// is allocated and prefaulted only after the proof-only v2 arm marker.
constexpr std::uint32_t TraceCapacityEntriesV2 = 1u << 20;
constexpr std::uint64_t TraceFeaturesV2 =
    FeatureV2RequestIdentity | FeatureV2RequestBatchMapping |
    FeatureV2DrainBatchIdentity | FeatureV2FrameEpochs |
    FeatureV2PresentIdentity | FeatureV2CaptureEpoch |
    FeatureV2InstrumentedPerturbation | FeatureV2FrameOwnershipToken;

#ifdef __ANDROID__
constexpr auto ControllerIdlePoll = std::chrono::milliseconds{100};
constexpr auto ControllerCapturePoll = std::chrono::milliseconds{25};
constexpr auto ControllerMaximumCapture = std::chrono::seconds{30};
constexpr auto ControllerMaximumQuiesce = std::chrono::seconds{2};
#endif

struct alignas(8) TraceRecord {
    std::uint64_t begin_ns{};
    std::uint64_t end_ns{};
    std::uint64_t argument{};
    std::uint32_t size{};
    // Written last with release ordering. Zero means unused.
    std::atomic<std::uint32_t> commit{};
};

struct alignas(8) TraceRecordV2 {
    std::uint64_t begin_ns{};
    std::uint64_t end_ns{};
    std::uint64_t primary_id{};
    std::uint64_t related_id{};
    std::uint64_t argument{};
    std::uint32_t size{};
    std::atomic<std::uint32_t> commit{};
};

static_assert(sizeof(TraceRecord) == 32);
static_assert(sizeof(TraceRecordV2) == 48);
static_assert(sizeof(SnapshotRecordV1) == 32);
static_assert(sizeof(SnapshotRecordV2) == 48);
static_assert(sizeof(SnapshotHeaderV1) == 88);
static_assert(sizeof(StatsV1) == 64);
static_assert(sizeof(SnapshotHeaderV2) == 152);
static_assert(sizeof(StatsV2) == 128);
static_assert(std::is_standard_layout_v<StatsV1>);
static_assert(std::is_standard_layout_v<SnapshotHeaderV1>);
static_assert(std::is_standard_layout_v<SnapshotRecordV1>);
static_assert(std::is_standard_layout_v<StatsV2>);
static_assert(std::is_standard_layout_v<SnapshotHeaderV2>);
static_assert(std::is_standard_layout_v<SnapshotRecordV2>);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

struct alignas(4096) TraceStorage {
    std::array<TraceRecord, TraceCapacityEntries> records{};
    // Enabled-only global reservation is intentionally simple and observable.
    // It costs one atomic RMW per record, but avoids Android emulated-TLS lookup
    // and first-use allocation on producer/GpuComm hot paths.
    std::atomic<std::uint64_t> next_entry{};
    std::atomic<std::uint64_t> dropped{};
    std::atomic<std::uint64_t> in_flight_writers{};
    std::atomic<std::uint32_t> saturated{};
    std::atomic<std::uint32_t> coverage_exact{};
    std::atomic<std::uint32_t> quiesced{};
};

struct alignas(4096) TraceStorageV2 {
    std::array<TraceRecordV2, TraceCapacityEntriesV2> records{};
    std::atomic<std::uint64_t> next_entry{};
    std::atomic<std::uint64_t> dropped{};
    std::atomic<std::uint64_t> in_flight_writers{};
    std::atomic<std::uint64_t> active_spans{};
    std::atomic<std::uint64_t> next_request_id{};
    std::atomic<std::uint64_t> next_batch_id{};
    std::atomic<std::uint64_t> next_present_id{};
    std::atomic<std::uint64_t> next_frame_token{};
    std::atomic<std::uint64_t> capture_epoch{};
    std::atomic<std::uint64_t> capture_start_ns{};
    std::atomic<std::uint64_t> capture_stop_ns{};
    std::atomic<std::uint64_t> left_censored_requests{};
    std::atomic<std::uint64_t> right_censored_requests{};
    std::atomic<std::uint64_t> mixed_generation_batches{};
    std::atomic<std::uint64_t> epoch_crossing_batches{};
    std::atomic<std::uint32_t> saturated{};
    std::atomic<std::uint32_t> coverage_exact{};
    std::atomic<std::uint32_t> epoch_mapping_exact{};
    std::atomic<std::uint32_t> quiesced{};
};

TraceStorage trace_storage;
TraceStorageV2 trace_storage_v2;
// V2Context remains published while operations admitted before stop drain to
// completion. ActiveContext is the admission gate shared with V1.
std::atomic<void*> v2_context{};
std::atomic<std::uint32_t> initialization_state{};

bool EnvironmentEnabled(const char* name) noexcept {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

std::uint64_t ToNanoseconds(const std::chrono::steady_clock::time_point point) noexcept {
    const auto value =
        std::chrono::duration_cast<std::chrono::nanoseconds>(point.time_since_epoch()).count();
    return value > 0 ? static_cast<std::uint64_t>(value) : 0;
}

void LeaveWriter(TraceStorage& storage) noexcept {
    storage.in_flight_writers.fetch_sub(1, std::memory_order_release);
}

bool EnterWriter(TraceStorage& storage) noexcept {
    storage.in_flight_writers.fetch_add(1, std::memory_order_acq_rel);
    // Stop first clears ActiveContext and then waits for writers which passed
    // this recheck. A thread that loaded the old pointer before stop but enters
    // afterwards observes nullptr here and never touches a record.
    if (Detail::ActiveContext.load(std::memory_order_acquire) != &storage) {
        LeaveWriter(storage);
        return false;
    }
    return true;
}

void Saturate(TraceStorage& storage) noexcept {
    if (storage.saturated.exchange(1, std::memory_order_acq_rel) == 0) {
        // One is an exact lower bound: the event which crossed capacity was not
        // recorded. Stop immediately; subsequent events avoid clocks and any
        // shared dropped-counter RMW hotspot.
        storage.dropped.store(1, std::memory_order_release);
        storage.coverage_exact.store(0, std::memory_order_release);
        void* expected = &storage;
        Detail::ActiveContext.compare_exchange_strong(
            expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
    }
}

TraceRecord* AcquireRecord(TraceStorage& storage) noexcept {
    const std::uint64_t index =
        storage.next_entry.fetch_add(1, std::memory_order_relaxed);
    if (index >= TraceCapacityEntries) {
        Saturate(storage);
        return nullptr;
    }
    return &storage.records[static_cast<std::size_t>(index)];
}

void RecordEnabled(TraceStorage& storage, const EventKind kind,
                   const std::chrono::steady_clock::time_point begin,
                   const std::chrono::steady_clock::time_point end, const std::uint64_t argument,
                   const std::uint64_t size, const bool result) noexcept {
    if (!EnterWriter(storage)) {
        return;
    }
    TraceRecord* const record = AcquireRecord(storage);
    if (record != nullptr) {
        const std::uint64_t begin_ns = ToNanoseconds(begin);
        const std::uint64_t end_ns = ToNanoseconds(end);
        std::uint32_t flags = result ? RecordResult : 0;
        if (size > std::numeric_limits<std::uint32_t>::max()) {
            flags |= RecordSizeTruncated;
        }
        if (end_ns < begin_ns) {
            flags |= RecordInvalidInterval;
        }
        record->begin_ns = begin_ns;
        record->end_ns = end_ns;
        record->argument = argument;
        record->size = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(size, std::numeric_limits<std::uint32_t>::max()));
        record->commit.store(static_cast<std::uint32_t>(kind) | flags,
                             std::memory_order_release);
    }
    LeaveWriter(storage);
}

void LeaveWriterV2(TraceStorageV2& storage) noexcept {
    storage.in_flight_writers.fetch_sub(1, std::memory_order_release);
}

bool EnterWriterV2(TraceStorageV2& storage) noexcept {
    storage.in_flight_writers.fetch_add(1, std::memory_order_acq_rel);
    if (v2_context.load(std::memory_order_acquire) != &storage) {
        LeaveWriterV2(storage);
        return false;
    }
    return true;
}

bool EnterAdmittedSpanV2(TraceStorageV2& storage) noexcept {
    storage.active_spans.fetch_add(1, std::memory_order_acq_rel);
    if (Detail::ActiveContext.load(std::memory_order_acquire) != &storage ||
        v2_context.load(std::memory_order_acquire) != &storage) {
        storage.active_spans.fetch_sub(1, std::memory_order_release);
        return false;
    }
    return true;
}

bool EnterDerivedSpanV2(TraceStorageV2& storage) noexcept {
    storage.active_spans.fetch_add(1, std::memory_order_acq_rel);
    if (v2_context.load(std::memory_order_acquire) != &storage) {
        storage.active_spans.fetch_sub(1, std::memory_order_release);
        return false;
    }
    return true;
}

void LeaveSpanV2(TraceStorageV2& storage) noexcept {
    storage.active_spans.fetch_sub(1, std::memory_order_release);
}

void SaturateV2(TraceStorageV2& storage) noexcept {
    if (storage.saturated.exchange(1, std::memory_order_acq_rel) == 0) {
        storage.dropped.store(1, std::memory_order_release);
        storage.coverage_exact.store(0, std::memory_order_release);
        storage.epoch_mapping_exact.store(0, std::memory_order_release);
        void* expected = &storage;
        Detail::ActiveContext.compare_exchange_strong(
            expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
        v2_context.store(nullptr, std::memory_order_release);
    }
}

TraceRecordV2* AcquireRecordV2(TraceStorageV2& storage) noexcept {
    const std::uint64_t index =
        storage.next_entry.fetch_add(1, std::memory_order_relaxed);
    if (index >= TraceCapacityEntriesV2) {
        SaturateV2(storage);
        return nullptr;
    }
    return &storage.records[static_cast<std::size_t>(index)];
}

void RecordEnabledV2(TraceStorageV2& storage, const EventKindV2 kind,
                     const std::chrono::steady_clock::time_point begin,
                     const std::chrono::steady_clock::time_point end,
                     const std::uint64_t primary_id, const std::uint64_t related_id,
                     const std::uint64_t argument, const std::uint64_t size,
                     const bool result, const std::uint32_t extra_flags = 0) noexcept {
    if (!EnterWriterV2(storage)) {
        storage.coverage_exact.store(0, std::memory_order_release);
        storage.epoch_mapping_exact.store(0, std::memory_order_release);
        return;
    }
    TraceRecordV2* const record = AcquireRecordV2(storage);
    if (record != nullptr) {
        const std::uint64_t begin_ns = ToNanoseconds(begin);
        const std::uint64_t end_ns = ToNanoseconds(end);
        std::uint32_t flags = result ? RecordResult : 0;
        flags |= extra_flags;
        if (size > std::numeric_limits<std::uint32_t>::max()) {
            flags |= RecordSizeTruncated;
        }
        if (end_ns < begin_ns) {
            flags |= RecordInvalidInterval;
        }
        record->begin_ns = begin_ns;
        record->end_ns = end_ns;
        record->primary_id = primary_id;
        record->related_id = related_id;
        record->argument = argument;
        record->size = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(size, std::numeric_limits<std::uint32_t>::max()));
        record->commit.store(static_cast<std::uint32_t>(kind) | flags,
                             std::memory_order_release);
    }
    LeaveWriterV2(storage);
}

std::uint64_t ReservedEntries(const TraceStorage& storage) noexcept {
    return std::min<std::uint64_t>(storage.next_entry.load(std::memory_order_acquire),
                                   TraceCapacityEntries);
}

std::uint64_t ReservedEntriesV2(const TraceStorageV2& storage) noexcept {
    return std::min<std::uint64_t>(storage.next_entry.load(std::memory_order_acquire),
                                   TraceCapacityEntriesV2);
}

std::uint64_t CountCommitted(const TraceStorage& storage, const std::uint64_t reserved) noexcept {
    std::uint64_t committed = 0;
    for (std::uint64_t index = 0; index < reserved; ++index) {
        committed += storage.records[index].commit.load(std::memory_order_acquire) != 0;
    }
    return committed;
}

std::uint64_t CountCommittedV2(const TraceStorageV2& storage,
                               const std::uint64_t reserved) noexcept {
    std::uint64_t committed = 0;
    for (std::uint64_t index = 0; index < reserved; ++index) {
        committed += storage.records[index].commit.load(std::memory_order_acquire) != 0;
    }
    return committed;
}

StatsV1 CaptureStats() noexcept {
    StatsV1 stats{};
    stats.abi_version = TraceAbiVersion;
    stats.struct_size = sizeof(StatsV1);
    if (initialization_state.load(std::memory_order_acquire) != 2) {
        return stats;
    }
    const bool enabled = Detail::ActiveContext.load(std::memory_order_acquire) != nullptr;
    const bool quiesced = trace_storage.quiesced.load(std::memory_order_acquire) != 0;
    const std::uint64_t reserved = ReservedEntries(trace_storage);
    stats.capacity_entries = TraceCapacityEntries;
    stats.reserved_entries = reserved;
    stats.committed_entries = CountCommitted(trace_storage, reserved);
    stats.dropped_entries = trace_storage.dropped.load(std::memory_order_acquire);
    stats.enabled = enabled;
    stats.saturated = trace_storage.saturated.load(std::memory_order_acquire);
    stats.coverage_exact = !enabled && quiesced && stats.committed_entries == reserved
                               ? trace_storage.coverage_exact.load(std::memory_order_acquire)
                               : 0;
    stats.epoch_mapping_exact = 0;
    stats.feature_flags = TraceFeatures;
    return stats;
}

StatsV2 CaptureStatsV2() noexcept {
    StatsV2 stats{};
    stats.abi_version = TraceAbiVersionV2;
    stats.struct_size = sizeof(StatsV2);
    if (initialization_state.load(std::memory_order_acquire) != 2 ||
        trace_storage_v2.capture_epoch.load(std::memory_order_acquire) == 0) {
        return stats;
    }
    const bool enabled =
        Detail::ActiveContext.load(std::memory_order_acquire) == &trace_storage_v2;
    const bool quiesced = trace_storage_v2.quiesced.load(std::memory_order_acquire) != 0;
    const std::uint64_t reserved = ReservedEntriesV2(trace_storage_v2);
    stats.capacity_entries = TraceCapacityEntriesV2;
    stats.reserved_entries = reserved;
    stats.committed_entries = CountCommittedV2(trace_storage_v2, reserved);
    stats.dropped_entries = trace_storage_v2.dropped.load(std::memory_order_acquire);
    stats.enabled = enabled;
    stats.saturated = trace_storage_v2.saturated.load(std::memory_order_acquire);
    stats.coverage_exact =
        !enabled && quiesced && stats.committed_entries == reserved &&
                trace_storage_v2.active_spans.load(std::memory_order_acquire) == 0
            ? trace_storage_v2.coverage_exact.load(std::memory_order_acquire)
            : 0;
    stats.epoch_mapping_exact =
        stats.coverage_exact != 0
            ? trace_storage_v2.epoch_mapping_exact.load(std::memory_order_acquire)
            : 0;
    stats.feature_flags = TraceFeaturesV2;
    stats.capture_epoch = trace_storage_v2.capture_epoch.load(std::memory_order_acquire);
    stats.capture_start_ns =
        trace_storage_v2.capture_start_ns.load(std::memory_order_acquire);
    stats.capture_stop_ns =
        trace_storage_v2.capture_stop_ns.load(std::memory_order_acquire);
    stats.active_spans = trace_storage_v2.active_spans.load(std::memory_order_acquire);
    stats.left_censored_requests =
        trace_storage_v2.left_censored_requests.load(std::memory_order_acquire);
    stats.right_censored_requests =
        trace_storage_v2.right_censored_requests.load(std::memory_order_acquire);
    stats.mixed_generation_batches =
        trace_storage_v2.mixed_generation_batches.load(std::memory_order_acquire);
    stats.epoch_crossing_batches =
        trace_storage_v2.epoch_crossing_batches.load(std::memory_order_acquire);
    return stats;
}

SnapshotHeaderV1 MakeFileHeader(const StatsV1& stats) noexcept {
    SnapshotHeaderV1 header{};
    constexpr char Magic[8] = {'L', 'S', 'X', '4', 'I', 'O', 'T', '1'};
    std::memcpy(header.magic, Magic, sizeof(Magic));
    header.abi_version = TraceAbiVersion;
    header.header_size = sizeof(SnapshotHeaderV1);
    header.record_size = sizeof(SnapshotRecordV1);
    header.clock_id = ClockSteadyNanoseconds;
    header.capacity_entries = stats.capacity_entries;
    header.reserved_entries = stats.reserved_entries;
    header.committed_entries = stats.committed_entries;
    header.dropped_entries = stats.dropped_entries;
    header.enabled = stats.enabled;
    header.saturated = stats.saturated;
    header.coverage_exact = stats.coverage_exact;
    header.epoch_mapping_exact = 0;
    header.feature_flags = TraceFeatures;
    return header;
}

SnapshotHeaderV2 MakeFileHeaderV2(const StatsV2& stats) noexcept {
    SnapshotHeaderV2 header{};
    constexpr char Magic[8] = {'L', 'S', 'X', '4', 'I', 'O', 'T', '2'};
    std::memcpy(header.magic, Magic, sizeof(Magic));
    header.abi_version = TraceAbiVersionV2;
    header.header_size = sizeof(SnapshotHeaderV2);
    header.record_size = sizeof(SnapshotRecordV2);
    header.clock_id = ClockSteadyNanoseconds;
    header.capacity_entries = stats.capacity_entries;
    header.reserved_entries = stats.reserved_entries;
    header.committed_entries = stats.committed_entries;
    header.dropped_entries = stats.dropped_entries;
    header.enabled = stats.enabled;
    header.saturated = stats.saturated;
    header.coverage_exact = stats.coverage_exact;
    header.epoch_mapping_exact = stats.epoch_mapping_exact;
    header.feature_flags = stats.feature_flags;
    header.capture_epoch = stats.capture_epoch;
    header.capture_start_ns = stats.capture_start_ns;
    header.capture_stop_ns = stats.capture_stop_ns;
    header.active_spans = stats.active_spans;
    header.left_censored_requests = stats.left_censored_requests;
    header.right_censored_requests = stats.right_censored_requests;
    header.mixed_generation_batches = stats.mixed_generation_batches;
    header.epoch_crossing_batches = stats.epoch_crossing_batches;
    return header;
}

void ResetAndPrefault(TraceStorage& storage) noexcept {
    Detail::ActiveContext.store(nullptr, std::memory_order_release);
    storage.next_entry.store(0, std::memory_order_relaxed);
    storage.dropped.store(0, std::memory_order_relaxed);
    storage.in_flight_writers.store(0, std::memory_order_relaxed);
    storage.saturated.store(0, std::memory_order_relaxed);
    storage.coverage_exact.store(1, std::memory_order_relaxed);
    storage.quiesced.store(0, std::memory_order_relaxed);
    // This is deliberately enabled-only work. Clearing every commit both resets
    // a future one-shot reuse and prefaults the complete 16 MiB record storage
    // before the ready marker can be observed.
    for (TraceRecord& record : storage.records) {
        record.commit.store(0, std::memory_order_relaxed);
    }
}

void ResetAndPrefaultV2(TraceStorageV2& storage) noexcept {
    Detail::ActiveContext.store(nullptr, std::memory_order_release);
    v2_context.store(nullptr, std::memory_order_release);
    storage.next_entry.store(0, std::memory_order_relaxed);
    storage.dropped.store(0, std::memory_order_relaxed);
    storage.in_flight_writers.store(0, std::memory_order_relaxed);
    storage.active_spans.store(0, std::memory_order_relaxed);
    storage.next_request_id.store(1, std::memory_order_relaxed);
    storage.next_batch_id.store(1, std::memory_order_relaxed);
    storage.next_present_id.store(1, std::memory_order_relaxed);
    storage.next_frame_token.store(1, std::memory_order_relaxed);
    storage.capture_epoch.store(0, std::memory_order_relaxed);
    storage.capture_start_ns.store(0, std::memory_order_relaxed);
    storage.capture_stop_ns.store(0, std::memory_order_relaxed);
    storage.left_censored_requests.store(0, std::memory_order_relaxed);
    storage.right_censored_requests.store(0, std::memory_order_relaxed);
    storage.mixed_generation_batches.store(0, std::memory_order_relaxed);
    storage.epoch_crossing_batches.store(0, std::memory_order_relaxed);
    storage.saturated.store(0, std::memory_order_relaxed);
    storage.coverage_exact.store(1, std::memory_order_relaxed);
    storage.epoch_mapping_exact.store(1, std::memory_order_relaxed);
    storage.quiesced.store(0, std::memory_order_relaxed);
    for (TraceRecordV2& record : storage.records) {
        record.commit.store(0, std::memory_order_relaxed);
    }
}

void Quiesce(TraceStorage& storage, const bool exact) noexcept {
    Detail::ActiveContext.exchange(nullptr, std::memory_order_acq_rel);
    while (storage.in_flight_writers.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
    if (!exact) {
        storage.coverage_exact.store(0, std::memory_order_release);
    }
    storage.quiesced.store(1, std::memory_order_release);
}


bool QuiesceV2(TraceStorageV2& storage, const bool exact) noexcept {
    void* expected = &storage;
    Detail::ActiveContext.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
    storage.capture_stop_ns.store(ToNanoseconds(std::chrono::steady_clock::now()),
                                  std::memory_order_release);
#ifdef __ANDROID__
    const auto deadline = std::chrono::steady_clock::now() + ControllerMaximumQuiesce;
    while ((storage.active_spans.load(std::memory_order_acquire) != 0 ||
            storage.in_flight_writers.load(std::memory_order_acquire) != 0) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
#else
    while (storage.active_spans.load(std::memory_order_acquire) != 0 ||
           storage.in_flight_writers.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
#endif
    const bool drained = storage.active_spans.load(std::memory_order_acquire) == 0 &&
                         storage.in_flight_writers.load(std::memory_order_acquire) == 0;
    if (!exact || !drained) {
        storage.coverage_exact.store(0, std::memory_order_release);
        storage.epoch_mapping_exact.store(0, std::memory_order_release);
    }
    v2_context.store(nullptr, std::memory_order_release);
    storage.quiesced.store(1, std::memory_order_release);
    return drained;
}

#ifdef __ANDROID__

std::string JoinPath(const std::string& directory, const char* leaf) {
    return directory + (directory.empty() || directory.back() == '/' ? "" : "/") + leaf;
}

bool PathExists(const std::string& path) noexcept {
    return ::access(path.c_str(), F_OK) == 0;
}

bool RemoveIfPresent(const std::string& path) noexcept {
    if (::unlink(path.c_str()) == 0) {
        return true;
    }
    return errno == ENOENT;
}

bool WriteSmallFile(const std::string& path, const std::string& contents) noexcept {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const bool wrote = contents.empty() ||
                       std::fwrite(contents.data(), 1, contents.size(), file) == contents.size();
    const bool flushed = std::fflush(file) == 0;
    const bool closed = std::fclose(file) == 0;
    return wrote && flushed && closed;
}

bool DumpQuiescedTrace(const std::string& output_path) {
    if (trace_storage.quiesced.load(std::memory_order_acquire) == 0 ||
        Detail::ActiveContext.load(std::memory_order_acquire) != nullptr) {
        return false;
    }
    const StatsV1 stats = CaptureStats();
    const SnapshotHeaderV1 header = MakeFileHeader(stats);
    const std::string temporary = output_path + ".tmp";
    std::FILE* file = std::fopen(temporary.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    bool ok = std::fwrite(&header, 1, sizeof(header), file) == sizeof(header);
    std::array<SnapshotRecordV1, 1024> records{};
    std::uint64_t index = 0;
    while (ok && index < stats.reserved_entries) {
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(records.size(), stats.reserved_entries - index));
        for (std::size_t offset = 0; offset < count; ++offset) {
            const TraceRecord& source = trace_storage.records[index + offset];
            SnapshotRecordV1& record = records[offset];
            record = {};
            record.commit = source.commit.load(std::memory_order_acquire);
            if (record.commit != 0) {
                record.begin_ns = source.begin_ns;
                record.end_ns = source.end_ns;
                record.argument = source.argument;
                record.size = source.size;
            }
        }
        ok = std::fwrite(records.data(), sizeof(SnapshotRecordV1), count, file) == count;
        index += count;
    }
    ok = std::fflush(file) == 0 && ok;
    const int descriptor = ::fileno(file);
    if (descriptor >= 0) {
        ok = ::fsync(descriptor) == 0 && ok;
    }
    ok = std::fclose(file) == 0 && ok;
    if (!ok || std::rename(temporary.c_str(), output_path.c_str()) != 0) {
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

bool DumpQuiescedTraceV2(const std::string& output_path) {
    if (trace_storage_v2.quiesced.load(std::memory_order_acquire) == 0 ||
        Detail::ActiveContext.load(std::memory_order_acquire) == &trace_storage_v2 ||
        v2_context.load(std::memory_order_acquire) != nullptr) {
        return false;
    }
    const StatsV2 stats = CaptureStatsV2();
    const SnapshotHeaderV2 header = MakeFileHeaderV2(stats);
    const std::string temporary = output_path + ".tmp";
    std::FILE* file = std::fopen(temporary.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    bool ok = std::fwrite(&header, 1, sizeof(header), file) == sizeof(header);
    std::array<SnapshotRecordV2, 512> records{};
    std::uint64_t index = 0;
    while (ok && index < stats.reserved_entries) {
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(records.size(), stats.reserved_entries - index));
        for (std::size_t offset = 0; offset < count; ++offset) {
            const TraceRecordV2& source = trace_storage_v2.records[index + offset];
            SnapshotRecordV2& record = records[offset];
            record = {};
            record.commit = source.commit.load(std::memory_order_acquire);
            if (record.commit != 0) {
                record.begin_ns = source.begin_ns;
                record.end_ns = source.end_ns;
                record.primary_id = source.primary_id;
                record.related_id = source.related_id;
                record.argument = source.argument;
                record.size = source.size;
            }
        }
        ok = std::fwrite(records.data(), sizeof(SnapshotRecordV2), count, file) == count;
        index += count;
    }
    ok = std::fflush(file) == 0 && ok;
    const int descriptor = ::fileno(file);
    if (descriptor >= 0) {
        ok = ::fsync(descriptor) == 0 && ok;
    }
    ok = std::fclose(file) == 0 && ok;
    if (!ok || std::rename(temporary.c_str(), output_path.c_str()) != 0) {
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

void ControllerMain(const std::string& control_directory) {
    const std::string arm_path = JoinPath(control_directory, "arm");
    const std::string ready_path = JoinPath(control_directory, "ready");
    const std::string ready_temporary_path = JoinPath(control_directory, "ready.tmp");
    const std::string stop_path = JoinPath(control_directory, "stop");
    const std::string done_path = JoinPath(control_directory, "done");
    const std::string error_path = JoinPath(control_directory, "error");
    const std::string output_path = JoinPath(control_directory, "trace-v1.bin");

    while (!PathExists(arm_path)) {
        std::this_thread::sleep_for(ControllerIdlePoll);
    }

    ResetAndPrefault(trace_storage);
    const std::string ready_contents =
        "enabled=1 abi=1 capacityEntries=" + std::to_string(TraceCapacityEntries) +
        " bytes=" + std::to_string(sizeof(trace_storage.records)) + "\n";
    if (!WriteSmallFile(ready_temporary_path, ready_contents)) {
        trace_storage.coverage_exact.store(0, std::memory_order_release);
        trace_storage.quiesced.store(1, std::memory_order_release);
        WriteSmallFile(error_path, "reason=ready-write-failed\n");
        WriteSmallFile(done_path, "dumped=0 exact=0 reason=ready-write-failed\n");
        return;
    }

    const auto deadline = std::chrono::steady_clock::now() + ControllerMaximumCapture;
    Detail::ActiveContext.store(&trace_storage, std::memory_order_release);
    if (std::rename(ready_temporary_path.c_str(), ready_path.c_str()) != 0) {
        Quiesce(trace_storage, false);
        std::remove(ready_temporary_path.c_str());
        WriteSmallFile(error_path, "reason=ready-publish-failed\n");
        WriteSmallFile(done_path, "dumped=0 exact=0 reason=ready-publish-failed\n");
        return;
    }
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_GUEST_INVALIDATION_OVERLAP] enabled=1 armed=1 "
                        "capacityEntries=%u bytes=%llu reservation=global-atomic "
                        "nonWrapping=1 maxWindowMs=%lld coverageExact=pending",
                        TraceCapacityEntries,
                        static_cast<unsigned long long>(sizeof(trace_storage.records)),
                        static_cast<long long>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                ControllerMaximumCapture)
                                .count()));

    bool timed_out = false;
    while (Detail::ActiveContext.load(std::memory_order_acquire) != nullptr &&
           !PathExists(stop_path)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            break;
        }
        std::this_thread::sleep_for(ControllerCapturePoll);
    }
    Quiesce(trace_storage, !timed_out);

    const bool dumped = DumpQuiescedTrace(output_path);
    const StatsV1 stats = CaptureStats();
    const bool exact = dumped && stats.coverage_exact != 0;
    if (!dumped) {
        WriteSmallFile(error_path, "reason=trace-dump-failed\n");
    } else if (timed_out) {
        WriteSmallFile(error_path, "reason=capture-timeout\n");
    } else if (stats.saturated != 0) {
        WriteSmallFile(error_path, "reason=trace-saturated\n");
    }
    const std::string done_contents =
        "dumped=" + std::to_string(dumped ? 1 : 0) +
        " exact=" + std::to_string(exact ? 1 : 0) +
        " saturated=" + std::to_string(stats.saturated) +
        " committedEntries=" + std::to_string(stats.committed_entries) +
        " droppedEntries=" + std::to_string(stats.dropped_entries) +
        " timedOut=" + std::to_string(timed_out ? 1 : 0) + "\n";
    WriteSmallFile(done_path, done_contents);
    __android_log_print(
        exact ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "LSX4Native",
        "[EXECUTOR_GUEST_INVALIDATION_OVERLAP] enabled=0 armed=0 dumped=%d exact=%d "
        "saturated=%u committed=%llu dropped=%llu timedOut=%d path=%s",
        dumped ? 1 : 0, exact ? 1 : 0, stats.saturated,
        static_cast<unsigned long long>(stats.committed_entries),
        static_cast<unsigned long long>(stats.dropped_entries), timed_out ? 1 : 0,
        output_path.c_str());
}

void ControllerMainV2(const std::string& control_directory) {
    const std::string arm_path = JoinPath(control_directory, "arm");
    const std::string ready_path = JoinPath(control_directory, "ready");
    const std::string ready_temporary_path = JoinPath(control_directory, "ready.tmp");
    const std::string stop_path = JoinPath(control_directory, "stop");
    const std::string done_path = JoinPath(control_directory, "done");
    const std::string error_path = JoinPath(control_directory, "error");
    const std::string output_path = JoinPath(control_directory, "trace-v2.bin");
    const std::array stale_paths{
        ready_path,
        ready_temporary_path,
        stop_path,
        done_path,
        error_path,
        output_path,
        output_path + ".tmp",
    };
    for (const std::string& stale_path : stale_paths) {
        if (!RemoveIfPresent(stale_path)) {
            __android_log_print(
                ANDROID_LOG_ERROR, "LSX4Native",
                "[EXECUTOR_GUEST_INVALIDATION_OVERLAP] enabled=0 armed=0 abi=2 "
                "reason=stale-control-cleanup-failed path=%s errno=%d",
                stale_path.c_str(), errno);
            return;
        }
    }

    while (!PathExists(arm_path)) {
        std::this_thread::sleep_for(ControllerIdlePoll);
    }
    if (!RemoveIfPresent(arm_path)) {
        __android_log_print(
            ANDROID_LOG_ERROR, "LSX4Native",
            "[EXECUTOR_GUEST_INVALIDATION_OVERLAP] enabled=0 armed=0 abi=2 "
            "reason=arm-consume-failed path=%s errno=%d",
            arm_path.c_str(), errno);
        return;
    }

    ResetAndPrefaultV2(trace_storage_v2);
    const auto capture_start = std::chrono::steady_clock::now();
    const std::uint64_t capture_start_ns = ToNanoseconds(capture_start);
    trace_storage_v2.capture_epoch.store(capture_start_ns, std::memory_order_release);
    trace_storage_v2.capture_start_ns.store(capture_start_ns, std::memory_order_release);
    const std::string ready_contents =
        "enabled=1 abi=2 structuralEpochComembership=1 delayCausality=0 perturbed=1 "
        "productionFrametime=0 capacityEntries=" +
        std::to_string(TraceCapacityEntriesV2) +
        " bytes=" + std::to_string(sizeof(trace_storage_v2.records)) +
        " captureEpoch=" + std::to_string(capture_start_ns) + "\n";
    if (!WriteSmallFile(ready_temporary_path, ready_contents)) {
        trace_storage_v2.coverage_exact.store(0, std::memory_order_release);
        trace_storage_v2.epoch_mapping_exact.store(0, std::memory_order_release);
        trace_storage_v2.quiesced.store(1, std::memory_order_release);
        WriteSmallFile(error_path, "reason=ready-write-failed\n");
        WriteSmallFile(done_path, "dumped=0 exact=0 causal=0 reason=ready-write-failed\n");
        return;
    }

    const auto deadline = capture_start + ControllerMaximumCapture;
    v2_context.store(&trace_storage_v2, std::memory_order_release);
    Detail::ActiveContext.store(&trace_storage_v2, std::memory_order_release);
    if (std::rename(ready_temporary_path.c_str(), ready_path.c_str()) != 0) {
        (void)QuiesceV2(trace_storage_v2, false);
        std::remove(ready_temporary_path.c_str());
        WriteSmallFile(error_path, "reason=ready-publish-failed\n");
        WriteSmallFile(done_path, "dumped=0 exact=0 causal=0 reason=ready-publish-failed\n");
        return;
    }
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_GUEST_INVALIDATION_OVERLAP] enabled=1 armed=1 abi=2 "
        "captureEpoch=%llu capacityEntries=%u bytes=%llu nonWrapping=1 "
        "structuralEpochComembership=pending delayCausality=not-claimed perturbed=1 "
        "productionFrametime=0 maxWindowMs=%lld",
        static_cast<unsigned long long>(capture_start_ns), TraceCapacityEntriesV2,
        static_cast<unsigned long long>(sizeof(trace_storage_v2.records)),
        static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(ControllerMaximumCapture)
                .count()));

    bool timed_out = false;
    while (Detail::ActiveContext.load(std::memory_order_acquire) == &trace_storage_v2 &&
           !PathExists(stop_path)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            break;
        }
        std::this_thread::sleep_for(ControllerCapturePoll);
    }
    const bool spans_drained = QuiesceV2(trace_storage_v2, !timed_out);
    const bool dumped = DumpQuiescedTraceV2(output_path);
    const StatsV2 stats = CaptureStatsV2();
    const bool exact = dumped && stats.coverage_exact != 0;
    const bool structural_mapping_exact = exact && stats.epoch_mapping_exact != 0;
    if (!dumped) {
        WriteSmallFile(error_path, "reason=trace-dump-failed\n");
    } else if (timed_out) {
        WriteSmallFile(error_path, "reason=capture-timeout\n");
    } else if (!spans_drained) {
        WriteSmallFile(error_path, "reason=span-quiesce-timeout\n");
    } else if (stats.saturated != 0) {
        WriteSmallFile(error_path, "reason=trace-saturated\n");
    } else if (!structural_mapping_exact) {
        WriteSmallFile(error_path, "reason=structural-mapping-inexact\n");
    }
    const std::string done_contents =
        "dumped=" + std::to_string(dumped ? 1 : 0) +
        " captureEpoch=" + std::to_string(stats.capture_epoch) +
        " exact=" + std::to_string(exact ? 1 : 0) +
        " structuralMappingExact=" +
        std::to_string(structural_mapping_exact ? 1 : 0) +
        " structuralVerdict=parser-required saturated=" +
        std::to_string(stats.saturated) +
        " committedEntries=" + std::to_string(stats.committed_entries) +
        " droppedEntries=" + std::to_string(stats.dropped_entries) +
        " activeSpans=" + std::to_string(stats.active_spans) +
        " timedOut=" + std::to_string(timed_out ? 1 : 0) + "\n";
    WriteSmallFile(done_path, done_contents);
    __android_log_print(
        structural_mapping_exact ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "LSX4Native",
        "[EXECUTOR_GUEST_INVALIDATION_OVERLAP] enabled=0 armed=0 abi=2 dumped=%d "
        "exact=%d structuralMappingExact=%d structuralVerdict=parser-required "
        "saturated=%u committed=%llu dropped=%llu activeSpans=%llu timedOut=%d path=%s",
        dumped ? 1 : 0, exact ? 1 : 0, structural_mapping_exact ? 1 : 0,
        stats.saturated, static_cast<unsigned long long>(stats.committed_entries),
        static_cast<unsigned long long>(stats.dropped_entries),
        static_cast<unsigned long long>(stats.active_spans), timed_out ? 1 : 0,
        output_path.c_str());
}

void ControllerEntry(std::string control_directory, const bool use_v2) noexcept {
    try {
        if (use_v2) {
            ControllerMainV2(control_directory);
        } else {
            ControllerMain(control_directory);
        }
    } catch (...) {
        if (use_v2) {
            (void)QuiesceV2(trace_storage_v2, false);
        } else {
            Quiesce(trace_storage, false);
        }
        __android_log_print(ANDROID_LOG_ERROR, "LSX4Native",
                            "[EXECUTOR_GUEST_INVALIDATION_OVERLAP] enabled=0 armed=0 "
                            "abi=%u dumped=0 exact=0 reason=controller-exception",
                            use_v2 ? 2u : 1u);
        try {
            WriteSmallFile(JoinPath(control_directory, "error"),
                           "reason=controller-exception\n");
            WriteSmallFile(JoinPath(control_directory, "done"),
                           "dumped=0 exact=0 reason=controller-exception\n");
        } catch (...) {
        }
    }
}

#endif

} // namespace

namespace Detail {
std::atomic<void*> ActiveContext{};
} // namespace Detail

void Initialize() noexcept {
    std::uint32_t expected = 0;
    if (!initialization_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        return;
    }
    const bool requested = EnvironmentEnabled("EXECUTOR_PROFILE_GUEST_INVALIDATION_OVERLAP");
    const bool source_profile = EnvironmentEnabled("EXECUTOR_PROFILE_GUEST_INVALIDATION_SOURCES");
    const char* const control_directory =
        std::getenv("EXECUTOR_GUEST_INVALIDATION_OVERLAP_CONTROL_DIR");
    if (!requested || source_profile || control_directory == nullptr || control_directory[0] == '\0') {
#ifdef __ANDROID__
        if (requested) {
            __android_log_print(
                ANDROID_LOG_ERROR, "LSX4Native",
                "[EXECUTOR_GUEST_INVALIDATION_OVERLAP] enabled=0 armed=0 reason=%s "
                "coverageExact=0 epochMappingExact=0",
                source_profile ? "source-profiler-conflict" : "missing-control-directory");
        }
#endif
        initialization_state.store(3, std::memory_order_release);
        return;
    }

    bool use_v2 = EnvironmentEnabled("EXECUTOR_PROFILE_GUEST_INVALIDATION_OVERLAP_V2");
#ifdef __ANDROID__
    // This file selector lets the existing launcher marker retain ABI v1 by
    // default while proof harnesses explicitly request v2 without changing the
    // production launcher contract. Consume it before starting: a stale v2
    // selector must never silently alter a later V1 capture.
    const std::string v2_selector = JoinPath(control_directory, "abi-v2");
    if (PathExists(v2_selector)) {
        if (::unlink(v2_selector.c_str()) != 0) {
            initialization_state.store(3, std::memory_order_release);
            __android_log_print(
                ANDROID_LOG_ERROR, "LSX4Native",
                "[EXECUTOR_GUEST_INVALIDATION_OVERLAP] enabled=0 armed=0 "
                "reason=v2-selector-consume-failed path=%s",
                v2_selector.c_str());
            return;
        }
        use_v2 = true;
    }
#endif

    if (use_v2) {
        trace_storage_v2.coverage_exact.store(0, std::memory_order_relaxed);
        trace_storage_v2.epoch_mapping_exact.store(0, std::memory_order_relaxed);
        trace_storage_v2.quiesced.store(0, std::memory_order_relaxed);
    } else {
        trace_storage.coverage_exact.store(0, std::memory_order_relaxed);
        trace_storage.quiesced.store(0, std::memory_order_relaxed);
    }
    initialization_state.store(2, std::memory_order_release);
#ifdef __ANDROID__
    try {
        std::thread{ControllerEntry, std::string{control_directory}, use_v2}.detach();
    } catch (...) {
        initialization_state.store(3, std::memory_order_release);
        __android_log_print(ANDROID_LOG_ERROR, "LSX4Native",
                            "[EXECUTOR_GUEST_INVALIDATION_OVERLAP] enabled=0 armed=0 "
                            "reason=controller-start-failed coverageExact=0");
        return;
    }
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_GUEST_INVALIDATION_OVERLAP] enabled=0 armed=0 "
                        "controller=waiting abi=%u controlDir=%s capacityEntries=%u bytes=%llu "
                        "nonWrapping=1 coverageExact=0 epochMappingExact=0 events=%s",
                        use_v2 ? 2u : 1u, control_directory,
                        use_v2 ? TraceCapacityEntriesV2 : TraceCapacityEntries,
                        static_cast<unsigned long long>(
                            use_v2 ? sizeof(trace_storage_v2.records)
                                   : sizeof(trace_storage.records)),
                        use_v2 ? "request,request-batch,drain,frame,present"
                               : "wait,drain");
#else
    initialization_state.store(3, std::memory_order_release);
#endif
}

void Detail::RecordWaitSpanEnabled(void* const context,
                                   const std::chrono::steady_clock::time_point begin,
                                   const std::chrono::steady_clock::time_point end,
                                   const std::uint64_t address, const std::uint64_t size,
                                   const bool result) noexcept {
    RecordEnabled(*static_cast<TraceStorage*>(context), EventKind::Wait, begin, end, address, size,
                  result);
}

void RecordDrainSpanEnabled(const std::chrono::steady_clock::time_point begin,
                            const std::chrono::steady_clock::time_point end) noexcept {
    RecordEnabled(trace_storage, EventKind::Drain, begin, end, 0, 0, true);
}

bool V2Enabled() noexcept {
    return Detail::ActiveContext.load(std::memory_order_acquire) == &trace_storage_v2;
}

bool V2MayHaveOutstandingSpans() noexcept {
    return v2_context.load(std::memory_order_acquire) == &trace_storage_v2;
}

RequestTokenV2 BeginRequestV2(const std::chrono::steady_clock::time_point begin,
                              const std::uint64_t frame_epoch) noexcept {
    if (Detail::ActiveContext.load(std::memory_order_acquire) != &trace_storage_v2 ||
        !EnterAdmittedSpanV2(trace_storage_v2)) {
        return {};
    }
    const std::uint64_t request_id =
        trace_storage_v2.next_request_id.fetch_add(1, std::memory_order_relaxed);
    if (request_id == 0) {
        trace_storage_v2.coverage_exact.store(0, std::memory_order_release);
        trace_storage_v2.epoch_mapping_exact.store(0, std::memory_order_release);
        LeaveSpanV2(trace_storage_v2);
        return {};
    }
    return {
        .context = &trace_storage_v2,
        .request_id = request_id,
        .enqueue_epoch = frame_epoch,
        .begin = begin,
    };
}

void EndRequestV2(const RequestTokenV2& token,
                  const std::chrono::steady_clock::time_point end,
                  const std::uint64_t address, const std::uint64_t size,
                  const bool result) noexcept {
    if (!token || token.context != &trace_storage_v2) {
        return;
    }
    RecordEnabledV2(trace_storage_v2, EventKindV2::RequestWait, token.begin, end,
                    token.request_id, token.enqueue_epoch, address, size, result);
    LeaveSpanV2(trace_storage_v2);
}

DrainTokenV2 BeginDrainBatchV2(
    const std::chrono::steady_clock::time_point begin,
    const std::uint64_t frame_epoch,
    const RequestTokenV2* const first_captured_request,
    const std::uint64_t untracked_request_count,
    const std::uint64_t request_count) noexcept {
    void* context = first_captured_request != nullptr
                        ? first_captured_request->context
                        : Detail::ActiveContext.load(std::memory_order_acquire);
    if (context != &trace_storage_v2 || !EnterDerivedSpanV2(trace_storage_v2)) {
        return {};
    }
    if (untracked_request_count != 0) {
        trace_storage_v2.epoch_mapping_exact.store(0, std::memory_order_release);
        if (Detail::ActiveContext.load(std::memory_order_acquire) ==
            &trace_storage_v2) {
            trace_storage_v2.left_censored_requests.fetch_add(
                untracked_request_count, std::memory_order_relaxed);
        } else {
            trace_storage_v2.right_censored_requests.fetch_add(
                untracked_request_count, std::memory_order_relaxed);
        }
        if (first_captured_request != nullptr) {
            trace_storage_v2.mixed_generation_batches.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    const std::uint64_t batch_id =
        trace_storage_v2.next_batch_id.fetch_add(1, std::memory_order_relaxed);
    if (batch_id == 0) {
        trace_storage_v2.coverage_exact.store(0, std::memory_order_release);
        trace_storage_v2.epoch_mapping_exact.store(0, std::memory_order_release);
        LeaveSpanV2(trace_storage_v2);
        return {};
    }
    return {
        .context = &trace_storage_v2,
        .batch_id = batch_id,
        .frame_epoch = frame_epoch,
        .request_count = request_count,
        .begin = begin,
    };
}

void RecordRequestBatchMappingV2(
    const RequestTokenV2& request, const DrainTokenV2& batch,
    const std::chrono::steady_clock::time_point point) noexcept {
    if (!request || !batch || request.context != batch.context ||
        request.context != &trace_storage_v2) {
        if (batch.context == &trace_storage_v2) {
            trace_storage_v2.epoch_mapping_exact.store(0, std::memory_order_release);
        }
        return;
    }
    RecordEnabledV2(trace_storage_v2, EventKindV2::RequestBatch, point, point,
                    request.request_id, batch.batch_id, batch.frame_epoch, 0, true);
}

void EndDrainBatchV2(const DrainTokenV2& token,
                     const std::chrono::steady_clock::time_point end,
                     const std::uint64_t frame_epoch) noexcept {
    if (!token || token.context != &trace_storage_v2) {
        return;
    }
    const bool crossed_epoch = frame_epoch != token.frame_epoch;
    if (crossed_epoch) {
        trace_storage_v2.epoch_crossing_batches.fetch_add(1, std::memory_order_relaxed);
        trace_storage_v2.epoch_mapping_exact.store(0, std::memory_order_release);
    }
    RecordEnabledV2(trace_storage_v2, EventKindV2::DrainBatch, token.begin, end,
                    token.batch_id, token.frame_epoch, frame_epoch,
                    token.request_count, true,
                    crossed_epoch ? RecordEpochCrossed : 0);
    LeaveSpanV2(trace_storage_v2);
}

std::uint64_t ReserveFrameTokenV2() noexcept {
    if (Detail::ActiveContext.load(std::memory_order_acquire) != &trace_storage_v2) {
        return 0;
    }
    const std::uint64_t frame_token =
        trace_storage_v2.next_frame_token.fetch_add(1, std::memory_order_relaxed);
    if (frame_token == 0) {
        trace_storage_v2.coverage_exact.store(0, std::memory_order_release);
        trace_storage_v2.epoch_mapping_exact.store(0, std::memory_order_release);
        return 0;
    }
    return frame_token;
}

void RecordFrameBoundaryV2(const std::chrono::steady_clock::time_point point,
                           const std::uint64_t boundary_id,
                           const std::uint64_t closed_epoch,
                           const std::uint64_t frame_token, const bool is_eop) noexcept {
    if (Detail::ActiveContext.load(std::memory_order_acquire) != &trace_storage_v2) {
        return;
    }
    RecordEnabledV2(trace_storage_v2, EventKindV2::FrameBoundary, point, point,
                    boundary_id, closed_epoch, frame_token,
                    is_eop ? 1 : 0, true);
}

PresentTokenV2 BeginPresentV2(const std::uint64_t frame_epoch,
                              const std::uint64_t frame_token) noexcept {
    if (frame_token == 0) {
        return {};
    }
    if (Detail::ActiveContext.load(std::memory_order_acquire) != &trace_storage_v2 ||
        !EnterAdmittedSpanV2(trace_storage_v2)) {
        return {};
    }
    const std::uint64_t present_id =
        trace_storage_v2.next_present_id.fetch_add(1, std::memory_order_relaxed);
    if (present_id == 0) {
        trace_storage_v2.coverage_exact.store(0, std::memory_order_release);
        trace_storage_v2.epoch_mapping_exact.store(0, std::memory_order_release);
        LeaveSpanV2(trace_storage_v2);
        return {};
    }
    return {
        .context = &trace_storage_v2,
        .present_id = present_id,
        .frame_epoch = frame_epoch,
        .frame_token = frame_token,
        .begin = std::chrono::steady_clock::now(),
    };
}

void EndPresentV2(const PresentTokenV2& token, const bool result) noexcept {
    if (!token || token.context != &trace_storage_v2) {
        return;
    }
    RecordEnabledV2(trace_storage_v2, EventKindV2::Present, token.begin,
                    std::chrono::steady_clock::now(),
                    token.present_id, token.frame_epoch, token.frame_token, 0, result);
    LeaveSpanV2(trace_storage_v2);
}

} // namespace VideoCore::InvalidationOverlapTrace

extern "C" std::uint32_t executor_funnel_invalidation_overlap_trace_stats_v1(
    VideoCore::InvalidationOverlapTrace::StatsV1* output,
    const std::uint32_t output_size) noexcept {
    using namespace VideoCore::InvalidationOverlapTrace;
    constexpr std::uint32_t Required = sizeof(StatsV1);
    if (output != nullptr && output_size >= Required) {
        *output = CaptureStats();
    }
    return Required;
}

extern "C" std::uint64_t executor_funnel_invalidation_overlap_trace_snapshot_v1(
    void* output, const std::uint64_t output_capacity) noexcept {
    using namespace VideoCore::InvalidationOverlapTrace;
    if (trace_storage.quiesced.load(std::memory_order_acquire) == 0 ||
        Detail::ActiveContext.load(std::memory_order_acquire) != nullptr) {
        return 0;
    }
    const StatsV1 stats = CaptureStats();
    const std::uint64_t required =
        sizeof(SnapshotHeaderV1) + stats.reserved_entries * sizeof(SnapshotRecordV1);
    if (output == nullptr || output_capacity < required) {
        return required;
    }
    auto* cursor = static_cast<std::byte*>(output);
    const SnapshotHeaderV1 header = MakeFileHeader(stats);
    std::memcpy(cursor, &header, sizeof(header));
    cursor += sizeof(header);
    for (std::uint64_t index = 0; index < stats.reserved_entries; ++index) {
        const TraceRecord& source = trace_storage.records[index];
        SnapshotRecordV1 record{};
        record.commit = source.commit.load(std::memory_order_acquire);
        if (record.commit != 0) {
            record.begin_ns = source.begin_ns;
            record.end_ns = source.end_ns;
            record.argument = source.argument;
            record.size = source.size;
        }
        std::memcpy(cursor, &record, sizeof(record));
        cursor += sizeof(record);
    }
    return required;
}

extern "C" std::uint32_t executor_funnel_invalidation_overlap_trace_stats_v2(
    VideoCore::InvalidationOverlapTrace::StatsV2* output,
    const std::uint32_t output_size) noexcept {
    using namespace VideoCore::InvalidationOverlapTrace;
    constexpr std::uint32_t Required = sizeof(StatsV2);
    if (output != nullptr && output_size >= Required) {
        *output = CaptureStatsV2();
    }
    return Required;
}

extern "C" std::uint64_t executor_funnel_invalidation_overlap_trace_snapshot_v2(
    void* output, const std::uint64_t output_capacity) noexcept {
    using namespace VideoCore::InvalidationOverlapTrace;
    if (trace_storage_v2.quiesced.load(std::memory_order_acquire) == 0 ||
        Detail::ActiveContext.load(std::memory_order_acquire) == &trace_storage_v2 ||
        v2_context.load(std::memory_order_acquire) != nullptr) {
        return 0;
    }
    const StatsV2 stats = CaptureStatsV2();
    const std::uint64_t required =
        sizeof(SnapshotHeaderV2) + stats.reserved_entries * sizeof(SnapshotRecordV2);
    if (output == nullptr || output_capacity < required) {
        return required;
    }
    auto* cursor = static_cast<std::byte*>(output);
    const SnapshotHeaderV2 header = MakeFileHeaderV2(stats);
    std::memcpy(cursor, &header, sizeof(header));
    cursor += sizeof(header);
    for (std::uint64_t index = 0; index < stats.reserved_entries; ++index) {
        const TraceRecordV2& source = trace_storage_v2.records[index];
        SnapshotRecordV2 record{};
        record.commit = source.commit.load(std::memory_order_acquire);
        if (record.commit != 0) {
            record.begin_ns = source.begin_ns;
            record.end_ns = source.end_ns;
            record.primary_id = source.primary_id;
            record.related_id = source.related_id;
            record.argument = source.argument;
            record.size = source.size;
        }
        std::memcpy(cursor, &record, sizeof(record));
        cursor += sizeof(record);
    }
    return required;
}
