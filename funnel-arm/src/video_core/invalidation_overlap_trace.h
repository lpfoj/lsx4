// SPDX-FileCopyrightText: Copyright 2026 LSX4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace VideoCore::InvalidationOverlapTrace {

// The v1 trace intentionally contains only synchronous producer WAIT spans and
// GpuComm DRAIN callback spans. Frame-boundary and present epochs are reserved
// for a later ABI revision; callers must not infer per-frame causality from v1.
enum class EventKind : std::uint32_t {
    Wait = 1,
    Drain = 2,
    FrameBoundary = 3,
    Present = 4,
};

enum Feature : std::uint64_t {
    FeatureWaitSpans = 1ull << 0,
    FeatureDrainSpans = 1ull << 1,
    FeatureFrameBoundaries = 1ull << 2,
    FeaturePresentMapping = 1ull << 3,
};

enum RecordCommit : std::uint32_t {
    RecordKindMask = 0xffu,
    RecordResult = 1u << 8,
    RecordSizeTruncated = 1u << 9,
    RecordInvalidInterval = 1u << 10,
    RecordEpochCrossed = 1u << 11,
};

// ABI v2 is a separate opt-in capture. V1 remains the interval-only ABI above.
// V2 adds stable identities and explicit ownership edges; an offline parser can
// therefore establish request -> drain batch -> render epoch -> present chains
// without guessing from timestamp overlap. This establishes membership, not by
// itself a counterfactual claim that the invalidation caused all frame delay.
enum class EventKindV2 : std::uint32_t {
    RequestWait = 1,
    RequestBatch = 2,
    DrainBatch = 3,
    FrameBoundary = 4,
    Present = 5,
};

enum FeatureV2 : std::uint64_t {
    FeatureV2RequestIdentity = 1ull << 0,
    FeatureV2RequestBatchMapping = 1ull << 1,
    FeatureV2DrainBatchIdentity = 1ull << 2,
    FeatureV2FrameEpochs = 1ull << 3,
    FeatureV2PresentIdentity = 1ull << 4,
    FeatureV2CaptureEpoch = 1ull << 5,
    FeatureV2InstrumentedPerturbation = 1ull << 6,
    FeatureV2FrameOwnershipToken = 1ull << 7,
};

struct StatsV1 {
    std::uint32_t abi_version{};
    std::uint32_t struct_size{};
    std::uint64_t capacity_entries{};
    std::uint64_t reserved_entries{};
    std::uint64_t committed_entries{};
    std::uint64_t dropped_entries{};
    std::uint32_t enabled{};
    std::uint32_t saturated{};
    std::uint32_t coverage_exact{};
    std::uint32_t epoch_mapping_exact{};
    std::uint64_t feature_flags{};
};

// Little-endian snapshot layout returned by the cold snapshot ABI. A quiesced
// exact non-wrapping capture has one committed record in every reserved slot;
// commit==0 inside that prefix therefore makes coverage inexact.
struct SnapshotHeaderV1 {
    char magic[8]{};
    std::uint32_t abi_version{};
    std::uint32_t header_size{};
    std::uint32_t record_size{};
    std::uint32_t clock_id{};
    std::uint64_t capacity_entries{};
    std::uint64_t reserved_entries{};
    std::uint64_t committed_entries{};
    std::uint64_t dropped_entries{};
    std::uint32_t enabled{};
    std::uint32_t saturated{};
    std::uint32_t coverage_exact{};
    std::uint32_t epoch_mapping_exact{};
    std::uint64_t feature_flags{};
    std::uint64_t reserved_zero{};
};

struct SnapshotRecordV1 {
    std::uint64_t begin_ns{};
    std::uint64_t end_ns{};
    std::uint64_t argument{};
    std::uint32_t size{};
    std::uint32_t commit{};
};

struct StatsV2 {
    std::uint32_t abi_version{};
    std::uint32_t struct_size{};
    std::uint64_t capacity_entries{};
    std::uint64_t reserved_entries{};
    std::uint64_t committed_entries{};
    std::uint64_t dropped_entries{};
    std::uint32_t enabled{};
    std::uint32_t saturated{};
    std::uint32_t coverage_exact{};
    std::uint32_t epoch_mapping_exact{};
    std::uint64_t feature_flags{};
    std::uint64_t capture_epoch{};
    std::uint64_t capture_start_ns{};
    std::uint64_t capture_stop_ns{};
    std::uint64_t active_spans{};
    std::uint64_t left_censored_requests{};
    std::uint64_t right_censored_requests{};
    std::uint64_t mixed_generation_batches{};
    std::uint64_t epoch_crossing_batches{};
};

struct SnapshotHeaderV2 {
    char magic[8]{};
    std::uint32_t abi_version{};
    std::uint32_t header_size{};
    std::uint32_t record_size{};
    std::uint32_t clock_id{};
    std::uint64_t capacity_entries{};
    std::uint64_t reserved_entries{};
    std::uint64_t committed_entries{};
    std::uint64_t dropped_entries{};
    std::uint32_t enabled{};
    std::uint32_t saturated{};
    std::uint32_t coverage_exact{};
    std::uint32_t epoch_mapping_exact{};
    std::uint64_t feature_flags{};
    std::uint64_t capture_epoch{};
    std::uint64_t capture_start_ns{};
    std::uint64_t capture_stop_ns{};
    std::uint64_t active_spans{};
    std::uint64_t left_censored_requests{};
    std::uint64_t right_censored_requests{};
    std::uint64_t mixed_generation_batches{};
    std::uint64_t epoch_crossing_batches{};
    std::uint64_t reserved_zero{};
};

// Generic v2 record fields by kind:
// RequestWait:  primary=request, related=enqueue epoch, argument=guest address.
// RequestBatch: primary=request, related=batch, argument=drain epoch.
// DrainBatch:   primary=batch, related=begin epoch, argument=end epoch.
// FrameBoundary:primary=boundary, related=closed epoch, argument=Frame token.
// Present:      primary=present, related=frame epoch, argument=Frame token.
struct SnapshotRecordV2 {
    std::uint64_t begin_ns{};
    std::uint64_t end_ns{};
    std::uint64_t primary_id{};
    std::uint64_t related_id{};
    std::uint64_t argument{};
    std::uint32_t size{};
    std::uint32_t commit{};
};

struct RequestTokenV2 {
    void* context{};
    std::uint64_t request_id{};
    std::uint64_t enqueue_epoch{};
    std::chrono::steady_clock::time_point begin{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return context != nullptr && request_id != 0;
    }
};

struct DrainTokenV2 {
    void* context{};
    std::uint64_t batch_id{};
    std::uint64_t frame_epoch{};
    std::uint64_t request_count{};
    std::chrono::steady_clock::time_point begin{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return context != nullptr && batch_id != 0;
    }
};

struct PresentTokenV2 {
    void* context{};
    std::uint64_t present_id{};
    std::uint64_t frame_epoch{};
    std::uint64_t frame_token{};
    std::chrono::steady_clock::time_point begin{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return context != nullptr && present_id != 0;
    }
};

// Must run after the launcher has resolved diagnostic marker environment
// variables and before gameplay starts. It is idempotent and starts only the
// diagnostic controller. The record hot path stays disabled until that
// controller observes its arm marker, and it is never enabled while the source
// profiler is active.
void Initialize() noexcept;

namespace Detail {
#if defined(__GNUC__)
__attribute__((visibility("hidden")))
#endif
extern std::atomic<void*>
    ActiveContext;

void RecordWaitSpanEnabled(void* context, std::chrono::steady_clock::time_point begin,
                           std::chrono::steady_clock::time_point end, std::uint64_t address,
                           std::uint64_t size, bool result) noexcept;
} // namespace Detail

// One inlined active-context pointer load. The caller uses this as the single
// early gate before taking DRAIN timestamps.
[[nodiscard]] inline bool Enabled() noexcept {
    return Detail::ActiveContext.load(std::memory_order_acquire) != nullptr;
}

// V2 is selected only by an explicit v2 control marker/environment request.
// These gates are false in production and V1 captures.
[[nodiscard]] bool V2Enabled() noexcept;
[[nodiscard]] bool V2MayHaveOutstandingSpans() noexcept;

[[nodiscard]] RequestTokenV2 BeginRequestV2(
    std::chrono::steady_clock::time_point begin, std::uint64_t frame_epoch) noexcept;
void EndRequestV2(const RequestTokenV2& token,
                  std::chrono::steady_clock::time_point end, std::uint64_t address,
                  std::uint64_t size, bool result) noexcept;

[[nodiscard]] DrainTokenV2 BeginDrainBatchV2(
    std::chrono::steady_clock::time_point begin, std::uint64_t frame_epoch,
    const RequestTokenV2* first_captured_request, std::uint64_t untracked_request_count,
    std::uint64_t request_count) noexcept;
void RecordRequestBatchMappingV2(const RequestTokenV2& request,
                                 const DrainTokenV2& batch,
                                 std::chrono::steady_clock::time_point point) noexcept;
void EndDrainBatchV2(const DrainTokenV2& token,
                     std::chrono::steady_clock::time_point end,
                     std::uint64_t frame_epoch) noexcept;

[[nodiscard]] std::uint64_t ReserveFrameTokenV2() noexcept;
void RecordFrameBoundaryV2(std::chrono::steady_clock::time_point point,
                           std::uint64_t boundary_id, std::uint64_t closed_epoch,
                           std::uint64_t frame_token, bool is_eop) noexcept;
[[nodiscard]] PresentTokenV2 BeginPresentV2(std::uint64_t frame_epoch,
                                            std::uint64_t frame_token) noexcept;
void EndPresentV2(const PresentTokenV2& token, bool result) noexcept;

// WAIT reuses the caller's existing begin/end steady-clock reads. The disabled
// path is one unlikely active-context pointer branch and touches no TLS/clock.
inline void RecordWaitSpan(std::chrono::steady_clock::time_point begin,
                           std::chrono::steady_clock::time_point end, std::uint64_t address,
                           std::uint64_t size, bool result) noexcept {
    void* const context = Detail::ActiveContext.load(std::memory_order_acquire);
    if (context != nullptr) [[unlikely]] {
        Detail::RecordWaitSpanEnabled(context, begin, end, address, size, result);
    }
}

// Enabled-only DRAIN recorder. Call only after Enabled() returned true; it does
// not read the clock itself.
void RecordDrainSpanEnabled(std::chrono::steady_clock::time_point begin,
                            std::chrono::steady_clock::time_point end) noexcept;

} // namespace VideoCore::InvalidationOverlapTrace

#if defined(__GNUC__)
#define LSX4_INVALIDATION_OVERLAP_EXPORT __attribute__((visibility("default")))
#else
#define LSX4_INVALIDATION_OVERLAP_EXPORT
#endif

// Cold diagnostic ABI. Snapshot returns zero until recording has been stopped
// and every enabled writer has quiesced. Afterwards it returns the required byte
// count and copies a complete v1 header plus fixed-slot records only when
// output_capacity is large enough. The diagnostic controller normally uses the
// same quiesced representation to self-dump inside the app-private control
// directory; external callers must not attempt a live snapshot.
extern "C" LSX4_INVALIDATION_OVERLAP_EXPORT std::uint32_t
executor_funnel_invalidation_overlap_trace_stats_v1(
    VideoCore::InvalidationOverlapTrace::StatsV1* output, std::uint32_t output_size) noexcept;

extern "C" LSX4_INVALIDATION_OVERLAP_EXPORT std::uint64_t
executor_funnel_invalidation_overlap_trace_snapshot_v1(void* output,
                                                       std::uint64_t output_capacity) noexcept;

extern "C" LSX4_INVALIDATION_OVERLAP_EXPORT std::uint32_t
executor_funnel_invalidation_overlap_trace_stats_v2(
    VideoCore::InvalidationOverlapTrace::StatsV2* output,
    std::uint32_t output_size) noexcept;

extern "C" LSX4_INVALIDATION_OVERLAP_EXPORT std::uint64_t
executor_funnel_invalidation_overlap_trace_snapshot_v2(void* output,
                                                       std::uint64_t output_capacity) noexcept;

#undef LSX4_INVALIDATION_OVERLAP_EXPORT
