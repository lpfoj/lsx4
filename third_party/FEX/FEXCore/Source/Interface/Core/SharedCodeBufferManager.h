// SPDX-License-Identifier: MIT
/*
$info$
category: Thread shared code buffer management
tags: backend|shared
$end_info$
*/
#pragma once

#include <FEXCore/fextl/memory.h>
#include <FEXCore/fextl/vector.h>
#include <FEXCore/Utils/SignalScopeGuards.h>
#include <FEXCore/Utils/TypeDefines.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace FEXCore {
struct GuestToHostMap;
}

namespace FEXCore::CPU {
// A generation is append-only: one writer publishes ranges while signal
// handlers may concurrently classify PCs.  Fixed storage avoids allocation,
// locks, and reclamation from the signal path.
struct CodeBufferGenerationRanges {
  static constexpr uint32_t MAX_RANGES = 32;

  struct Range {
    std::atomic<uintptr_t> Begin {};
    std::atomic<uintptr_t> End {};
  };

  [[nodiscard]] bool CanPublish() const noexcept {
    return PublishedCount.load(std::memory_order_acquire) < MAX_RANGES;
  }

  [[nodiscard]] bool Publish(const void* Base, size_t Size) noexcept {
    const uintptr_t Begin = reinterpret_cast<uintptr_t>(Base);
    if (Begin == 0 || Size == 0 || Size > UINTPTR_MAX - Begin) {
      return false;
    }

    const uint32_t Index = PublishedCount.load(std::memory_order_relaxed);
    if (Index >= MAX_RANGES) {
      return false;
    }

    Ranges[Index].Begin.store(Begin, std::memory_order_relaxed);
    Ranges[Index].End.store(Begin + Size, std::memory_order_relaxed);
    PublishedCount.store(Index + 1, std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool Contains(uintptr_t Address) const noexcept {
    const uint32_t Count = PublishedCount.load(std::memory_order_acquire);
    for (uint32_t Index = 0; Index < Count; ++Index) {
      const uintptr_t Begin = Ranges[Index].Begin.load(std::memory_order_relaxed);
      const uintptr_t End = Ranges[Index].End.load(std::memory_order_relaxed);
      if (Address >= Begin && Address < End) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] uint32_t Count() const noexcept {
    return PublishedCount.load(std::memory_order_acquire);
  }

private:
  static_assert(std::atomic<uintptr_t>::is_always_lock_free);
  static_assert(std::atomic<uint32_t>::is_always_lock_free);

  std::array<Range, MAX_RANGES> Ranges {};
  std::atomic<uint32_t> PublishedCount {};
};

struct CodeBuffer {
  uint8_t* Ptr;
  size_t AllocatedSize; // including guard page; see UsableSize()
  size_t GuardSize;
  bool ExecutableRangeReserved {};

  // All code segments in one generation share this map.  Keeping the map in
  // every segment also keeps it alive while an older thread still executes
  // that generation.
  fextl::shared_ptr<GuestToHostMap> LookupCache;
  fextl::shared_ptr<CodeBufferGenerationRanges> GenerationRanges;

  CodeBuffer(size_t Size, fextl::shared_ptr<GuestToHostMap> LookupCache,
             fextl::shared_ptr<CodeBufferGenerationRanges> GenerationRanges);
  CodeBuffer(const CodeBuffer&) = delete;
  CodeBuffer& operator=(const CodeBuffer&) = delete;
  CodeBuffer(CodeBuffer&& oth) = delete;
  CodeBuffer& operator=(CodeBuffer&&) = delete;

  ~CodeBuffer();

  /// Returns the number of bytes available for storing code
  size_t UsableSize() const {
    return AllocatedSize - GuardSize;
  }
};

/**
 * A manager that coordinates access to the CodeBuffer used for compiling new code across threads.
 *
 * The CodeBuffer is managed as a partially persistent data structure:
 * - Exactly one CodeBuffer is now designated as "active", which means data can be appended to it
 * - Lossy modifications to the active CodeBuffer will not invalidate any data in use by other threads (which is what enables save CodeBuffer sharing across threads)
 * - Instead, such lossy modifications trigger a new "version" of the data in the modifying thread. Old versions of the CodeBuffer persist as read-only data for use by the other threads.
 * - The other threads can update their version of the CodeBuffer. This will decrease the reference count and eventually trigger deallocation of the old version
 */
class SharedCodeBufferManager {
public:
  struct Telemetry {
    uint64_t Generation {};
    uint64_t RetainedRotations {};
    uint64_t GenerationResets {};
    uint64_t CurrentSegments {};
    uint64_t RetiredGenerations {};
    uint64_t RetiredSegments {};
    uint64_t CurrentUsedBytes {};
    uint64_t CurrentCapacityBytes {};
    uint64_t TotalAllocatedBytes {};
  };

  // Get the CodeBuffer that was most recently allocated.
  // This is the only CodeBuffer that data may be written to.
  fextl::shared_ptr<CodeBuffer> GetLatest();

  // Allocate a new CodeBuffer with geometric growth up to an internal maximum.
  // Subsequent calls to GetLatest will point to the returned buffer.
  fextl::shared_ptr<CodeBuffer> StartLargerCodeBuffer();

  // Allocate a new CodeBuffer with maximum internal size.
  // Subsequent calls to GetLatest will point to the returned buffer.
  fextl::shared_ptr<CodeBuffer> StartMaximalCodeBuffer();

  // Write offset into the latest CodeBuffer
  std::size_t LatestOffset {};

  // Protects writes to the latest CodeBuffer and changes to LatestOffset
  FEXCore::ForkableUniqueMutex CodeBufferWriteMutex;

  // Called after publishing code while CodeBufferWriteMutex is held.
  void RecordCodeCommit();

  [[nodiscard]] Telemetry SnapshotTelemetry() const noexcept {
    return {
      .Generation = TelemetryGeneration.load(std::memory_order_relaxed),
      .RetainedRotations = TelemetryRetainedRotations.load(std::memory_order_relaxed),
      .GenerationResets = TelemetryGenerationResets.load(std::memory_order_relaxed),
      .CurrentSegments = TelemetryCurrentSegments.load(std::memory_order_relaxed),
      .RetiredGenerations = TelemetryRetiredGenerations.load(std::memory_order_relaxed),
      .RetiredSegments = TelemetryRetiredSegments.load(std::memory_order_relaxed),
      .CurrentUsedBytes = TelemetryCurrentUsedBytes.load(std::memory_order_relaxed),
      .CurrentCapacityBytes = TelemetryCurrentCapacityBytes.load(std::memory_order_relaxed),
      .TotalAllocatedBytes = TelemetryTotalAllocatedBytes.load(std::memory_order_relaxed),
    };
  }

  virtual void OnCodeBufferAllocated(const std::shared_ptr<CodeBuffer>&) {};

protected:
  // Native CodeCache serialization is single-buffer-only. ContextImpl gates
  // retained rotations while generating such a cache.
  [[nodiscard]] virtual bool RetainedCodeBufferSegmentsAllowed() const noexcept {
    return true;
  }

private:
  static size_t RetainedCodeBudgetBytes();
  [[nodiscard]] bool CanRetainCurrentGeneration(size_t NextBufferSize) const;

  fextl::shared_ptr<CodeBuffer> Latest;
  fextl::shared_ptr<GuestToHostMap> ActiveLookupCache;
  fextl::shared_ptr<CodeBufferGenerationRanges> ActiveGenerationRanges;
  fextl::vector<fextl::shared_ptr<CodeBuffer>> CurrentSegments;
  fextl::vector<fextl::vector<fextl::shared_ptr<CodeBuffer>>> RetiredGenerations;

  size_t LatestCommittedBytes {};

  std::atomic<uint64_t> TelemetryGeneration {};
  std::atomic<uint64_t> TelemetryRetainedRotations {};
  std::atomic<uint64_t> TelemetryGenerationResets {};
  std::atomic<uint64_t> TelemetryCurrentSegments {};
  std::atomic<uint64_t> TelemetryRetiredGenerations {};
  std::atomic<uint64_t> TelemetryRetiredSegments {};
  std::atomic<uint64_t> TelemetryCurrentUsedBytes {};
  std::atomic<uint64_t> TelemetryCurrentCapacityBytes {};
  std::atomic<uint64_t> TelemetryTotalAllocatedBytes {};
  uint64_t LastLoggedUsedBucket {};

  fextl::shared_ptr<CodeBuffer> TryAllocateNew(size_t Size, bool RetainCurrentGeneration);
  fextl::shared_ptr<CodeBuffer> AllocateNew(size_t Size);
  void PruneRetiredGenerations();
};
} // namespace FEXCore::CPU
