// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include "common/alignment.h"
#include "common/types.h"
#include "video_core/buffer_cache//region_definitions.h"

namespace Vulkan {
class Rasterizer;
}

namespace Core {
enum class MemoryPermission : u32;
}

namespace VideoCore {

using JitPollWritePublishCallback = void (*)(VAddr address) noexcept;

enum class DescriptorSnapshotTrackResult : u8 {
    Tracked,
    Busy,
    Overlap,
    Unsupported,
    Invalid,
};

// Default-off POCO/A750 diagnostic. BufferCache brackets one existing
// draw/dispatch descriptor batch while PageManager records the exact legacy
// protection work performed inside it. These hooks never retain a watcher or
// alter snapshot data; they only model how many untrack protection runs could
// be merged by a future fail-closed cohort implementation.
void BeginDescriptorProtectionCohortDiagnosticBatch() noexcept;
void EndDescriptorProtectionCohortDiagnosticBatch() noexcept;
void RecordDescriptorProtectionCohortTransaction(u64 write_ranges) noexcept;

struct OwnedPageFaultPcStats {
    u64 generated_code{};
    // Exact sum of the mutually exclusive buffer/texture/other generated-code
    // write buckets while legacy live telemetry is enabled.
    u64 generated_code_write{};
    u64 host_code{};
    u64 unclassified{};
    bool generated_code_write_saturated{};
};

struct FexDeferredWriteActivitySnapshot {
    u32 active_armers{};
    u32 active_bulk_armers{};
    u32 active_bulk_writers{};
    u32 active_untracked_bulk_writers{};

    [[nodiscard]] constexpr bool HasActiveWriterOrArmer() const noexcept {
        return active_armers != 0 || active_bulk_armers != 0 ||
               active_bulk_writers != 0 ||
               active_untracked_bulk_writers != 0;
    }
};

// Opaque per-host-thread state for one synchronous bulk guest write.  Hot HLE
// producers may resolve it once and pass the same pointer to begin/end so an
// Android emulated-TLS runtime does not repeat pthread-key lookups for every
// field.  A null pointer deliberately selects the legacy access path.
struct BulkGuestWriteThreadState;

[[nodiscard]] BulkGuestWriteThreadState*
ResolveBulkGuestWriteThreadState() noexcept;
void BeginBulkGuestWrite(BulkGuestWriteThreadState* state, VAddr address,
                         u64 size) noexcept;
void EndBulkGuestWrite(BulkGuestWriteThreadState* state) noexcept;
void BeginBulkGuestWrite(VAddr address, u64 size) noexcept;
void EndBulkGuestWrite() noexcept;

class PageManager {
    static constexpr size_t PAGE_BITS = TRACKER_PAGE_BITS;
    static constexpr size_t PAGE_BYTES = TRACKER_BYTES_PER_PAGE;

    static constexpr size_t PAGES_PER_LOCK = NUM_PAGES_PER_REGION;

public:
    explicit PageManager(Vulkan::Rasterizer* rasterizer);
    ~PageManager();

    void CompleteInitialGpuMappingRegistration() noexcept;

    static void PrepareBulkGuestRead(VAddr address, u64 size);
    // Texture refresh may hash guest backing while renderer locks are held.
    // Resolve only the currently read-protected page runs before those locks
    // are acquired; expanding one protected page to the whole image turns a
    // small demand publication into a multi-megabyte synchronous readback.
    [[nodiscard]] static bool PrepareTextureGuestReadProtectedRuns(
        VAddr address, u64 size);
    static void PrepareBulkGuestWrite(VAddr address, u64 size);
    // Marker-gated D98 transaction for the common D51 case where the whole
    // range is already writable.  It atomically performs the no-invalidation
    // Prepare + Enter transition under the PageManager range locks.  False
    // means that the caller must execute the unchanged two-step path.
    [[nodiscard]] static bool TryBeginFastBulkGuestWrite(
        VAddr address, u64 size) noexcept;
    // The experimental clean-buffer transaction is intentionally op-aware:
    // destination-only admission cannot make memcpy/memmove source faults
    // safe while the topology guard is held.  Only source-free memset calls
    // may opt in; marker-off behavior remains the ordinary bulk path.
    static void PrepareBulkGuestWriteForMemset(VAddr address, u64 size);
    [[nodiscard]] static bool TryPrepareFexDeferredBulkGuestWrite(
        VAddr address, u64 size);
    // Starts a FEX deferred bulk-write lease as one transaction. This path
    // never falls back to synchronous guest-memory invalidation: a closed
    // epoch gate or any failed validation returns false after rolling back all
    // state owned by the attempt.
    [[nodiscard]] static bool TryBeginFexDeferredBulkGuestWrite(
        VAddr address, u64 size, bool require_read_ready = false) noexcept;
    [[nodiscard]] static bool FexDeferredWriteEpochEnabled() noexcept;
    // Diagnostic-only acquire snapshot. It does not close the FEX write
    // gates, so callers must not use it by itself to authorize a real skip.
    [[nodiscard]] static FexDeferredWriteActivitySnapshot
    GetFexDeferredWriteActivitySnapshot() noexcept;
    [[nodiscard]] static bool IsFexGuestMemoryReadReady(
        VAddr address, u64 size) noexcept;
    // Fail-only diagnostic bits for a rejected non-reentrant FEX lease.
    // This is a side-effect-free snapshot and is not used by the hot path.
    [[nodiscard]] static u64 DebugFexGuestMemoryLeaseState(
        VAddr address, u64 size, bool require_read_ready) noexcept;
    static void EnterBulkGuestWrite(VAddr address, u64 size);
    static void LeaveBulkGuestWrite(VAddr address, u64 size);
    [[nodiscard]] static u64 CaptureCurrentFexBulkLeaseEpoch() noexcept;
    static void AbortCurrentFexBulkLeaseAfter(u64 captured_epoch) noexcept;
    [[nodiscard]] static OwnedPageFaultPcStats GetOwnedPageFaultPcStats() noexcept;
    [[nodiscard]] static u64 DebugWriteFaultEpoch() noexcept;
    [[nodiscard]] static bool TryTrackJitPollWrite(
        VAddr addr, u64 size,
        JitPollWritePublishCallback callback) noexcept;
    [[nodiscard]] static bool TryUntrackJitPollWrite(VAddr addr,
                                                     u64 size) noexcept;
    [[nodiscard]] static bool TryArmDiagnosticWriteProvenance(
        VAddr addr, u64 size) noexcept;
    static void SetFexDeferredWriteEpochEnabled(bool enabled) noexcept;
    static void ShutdownFexDeferredWriteEpochs() noexcept;
    [[nodiscard]] static bool BeginFexDeferredWriteSubmitBoundary();
    static void EndFexDeferredWriteSubmitBoundary(bool active) noexcept;
    static void PublishFexDeferredWriteEpochReady(void* context,
                                                  u64 token) noexcept;
    [[nodiscard]] static bool BeginFexDeferredBufferCapture(
        VAddr address, u64 size,
        bool texture_cache_lock_held = false);
    [[nodiscard]] static bool MayHaveFexDeferredBufferCapture(
        VAddr address, u64 size) noexcept;
    [[nodiscard]] static std::span<const VAddr>
    CurrentFexDeferredBufferCaptureDirtyPages() noexcept;
    static void EndFexDeferredBufferCapture(
        bool active, bool texture_cache_lock_held = false) noexcept;
    [[nodiscard]] static bool BeginFexBufferSnapshotBoundary(
        VAddr address, u64 size);
    static void EndFexBufferSnapshotBoundary(bool active) noexcept;
    static void DrainFexDeferredWriteEpochs(bool wait_for_gpu = false,
                                            VAddr overlap_address = 0,
                                            u64 overlap_size = 0,
                                            bool coordinator_locked = false);

    void UpdateGpuDirtyPages(VAddr addr, u64 size, bool dirty) const;

    void OnGpuMap(VAddr address, size_t size,
                  Core::MemoryPermission host_permissions,
                  bool guest_cpu_read, bool guest_cpu_write);

    void OnGpuUnmap(VAddr address, size_t size);

    [[nodiscard]] bool BeginGuestProtectionChange(VAddr address, u64 size);
    void EndGuestProtectionChange(bool active) noexcept;

    [[nodiscard]] bool OnGuestProtectionChanged(
        VAddr address, size_t size,
        Core::MemoryPermission host_permissions,
        bool guest_cpu_read, bool guest_cpu_write);

    template <bool track>
    void UpdatePageWatchers(VAddr addr, u64 size) const;

    template <bool track>
    void UpdateTexturePageWatchers(VAddr addr, u64 size) const;

    // Tracks every registered guest-backed image, including images which are
    // temporarily untracked after becoming MaybeCpuDirty.  The clean-buffer
    // HLE experiment rejects resident texture pages rather than consulting
    // TextureCache from a guest thread.
    // Returns true only when the marker-gated sidecar actually acquired a
    // residency lease. Callers retain that fact so marker-off builds do not
    // grow bookkeeping vectors or issue unmatched releases.
    bool TrackTextureResidency(VAddr addr, u64 size) const;
    void UntrackTextureResidency(VAddr addr, u64 size) const;
    [[nodiscard]] bool HleCleanBufferFastPathEnabled() const noexcept;

    bool TrackHleShaderReadResidency(VAddr addr, u64 size) const;
    void UntrackHleShaderReadResidency(VAddr addr, u64 size) const;
    void DisableHleCleanBufferFastPathFailClosed() const noexcept;

    void TrackPendingCompletionRead(VAddr addr, u64 size) const;
    void UntrackPendingCompletionRead(VAddr addr, u64 size) const;

    void TrackMemoryWaitWrite(VAddr addr, u64 size) const;
    void UntrackMemoryWaitWrite(VAddr addr, u64 size) const;
    [[nodiscard]] bool ConsumeMemoryWaitWriteFault(VAddr addr) const;

    [[nodiscard]] DescriptorSnapshotTrackResult TryTrackDescriptorSnapshotWrite(
        VAddr addr, u64 size) const;
    [[nodiscard]] bool UntrackDescriptorSnapshotWrite(VAddr addr, u64 size) const;

    void TrackJitPollWrite(VAddr addr, u64 size) const;
    void UntrackJitPollWrite(VAddr addr, u64 size) const;
    [[nodiscard]] bool ConsumeJitPollWriteFault(VAddr addr) const;
    void SetJitPollWritePublishCallback(
        JitPollWritePublishCallback callback) const noexcept;

    template <bool track, bool is_read = false>
    void UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) const;

    static constexpr VAddr GetPageAddr(VAddr addr) {
        return Common::AlignDown(addr, PAGE_BYTES);
    }

    static constexpr VAddr GetNextPageAddr(VAddr addr) {
        return Common::AlignUp(addr + 1, PAGE_BYTES);
    }

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}

#if defined(__GNUC__)
#define LSX4_FUNNEL_BRIDGE_EXPORT __attribute__((visibility("default")))
#else
#define LSX4_FUNNEL_BRIDGE_EXPORT
#endif

extern "C" LSX4_FUNNEL_BRIDGE_EXPORT bool
executor_funnel_track_jit_poll_write(
    std::uint64_t address, std::uint64_t size,
    VideoCore::JitPollWritePublishCallback callback) noexcept;
extern "C" LSX4_FUNNEL_BRIDGE_EXPORT bool
executor_funnel_untrack_jit_poll_write(std::uint64_t address,
                                       std::uint64_t size) noexcept;
extern "C" LSX4_FUNNEL_BRIDGE_EXPORT bool
executor_funnel_arm_diagnostic_write_provenance(
    std::uint64_t address, std::uint64_t size) noexcept;

#undef LSX4_FUNNEL_BRIDGE_EXPORT
