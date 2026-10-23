// SPDX-License-Identifier: MIT
#include "Interface/Core/LookupCache.h"
#include "Interface/Core/SharedCodeBufferManager.h"

#include <FEXCore/Core/ExecutableCodeObserver.h>
#include <FEXCore/fextl/memory.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/MathUtils.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <utility>

#ifndef _WIN32
#include <FEXCore/Utils/PrctlUtils.h>
#endif

namespace FEXCore::CPU {
static constexpr size_t INITIAL_CODE_SIZE = 1024 * 1024 * 16;
// Keep each segment inside the AArch64 direct-branch envelope. Cross-segment
// links already use the existing indirect-link path.
static constexpr size_t MAX_CODE_SIZE = 1024 * 1024 * 128;
static constexpr size_t MIN_RETAINED_CODE_BUDGET = 1024ULL * 1024 * 128;
static constexpr size_t MAX_RETAINED_CODE_BUDGET = 1024ULL * 1024 * 1024;

size_t SharedCodeBufferManager::RetainedCodeBudgetBytes() {
  static const size_t Budget = [] {
    if (const char* Override = std::getenv("EXECUTOR_FEX_JIT_CODE_CACHE_MIB")) {
      uint64_t MiB {};
      const std::string_view Text {Override};
      const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), MiB);
      if (Error == std::errc {} && End == Text.data() + Text.size() && MiB >= 64 && MiB <= 2048) {
        return static_cast<size_t>(MiB) * 1024 * 1024;
      }
    }

#ifndef _WIN32
    const long PageSize = ::sysconf(_SC_PAGESIZE);
    const long PhysicalPages = ::sysconf(_SC_PHYS_PAGES);
    if (PageSize > 0 && PhysicalPages > 0 &&
        static_cast<uint64_t>(PhysicalPages) <=
          std::numeric_limits<uint64_t>::max() / static_cast<uint64_t>(PageSize)) {
      const uint64_t PhysicalBytes = static_cast<uint64_t>(PhysicalPages) * static_cast<uint64_t>(PageSize);
      const uint64_t ProportionalBudget = PhysicalBytes / 32;
      return static_cast<size_t>(std::clamp<uint64_t>(
        ProportionalBudget, MIN_RETAINED_CODE_BUDGET, MAX_RETAINED_CODE_BUDGET));
    }
#endif

    return static_cast<size_t>(MIN_RETAINED_CODE_BUDGET);
  }();
  return Budget;
}

bool SharedCodeBufferManager::CanRetainCurrentGeneration(size_t NextBufferSize) const {
  if (ActiveLookupCache == nullptr || ActiveGenerationRanges == nullptr ||
      !ActiveGenerationRanges->CanPublish() || CurrentSegments.empty()) {
    return false;
  }
  const uint64_t Capacity = TelemetryCurrentCapacityBytes.load(std::memory_order_relaxed);
  const uint64_t Budget = RetainedCodeBudgetBytes();
  return Capacity <= Budget && NextBufferSize <= Budget - Capacity;
}

CodeBuffer::CodeBuffer(size_t Size, fextl::shared_ptr<GuestToHostMap> LookupCache_,
                       fextl::shared_ptr<CodeBufferGenerationRanges> GenerationRanges_)
  : AllocatedSize(Size)
  , GuardSize(
#ifdef _WIN32
      FEXCore::Utils::FEX_PAGE_SIZE
#else
      static_cast<size_t>(::sysconf(_SC_PAGESIZE))
#endif
    )
  , LookupCache {std::move(LookupCache_)}
  , GenerationRanges {std::move(GenerationRanges_)} {
  if (GuardSize == 0 || (GuardSize & (GuardSize - 1)) != 0 || GuardSize > AllocatedSize) {
    Ptr = nullptr;
    return;
  }
  Ptr = static_cast<uint8_t*>(FEXCore::Allocator::VirtualAlloc(Size, true));
  if (Ptr == nullptr
#ifndef _WIN32
      || Ptr == MAP_FAILED
#endif
  ) {
    Ptr = nullptr;
    return;
  }
  // Protect the last page of the allocated buffer to trigger SIGSEGV on write access
  uintptr_t LastPageAddr = AlignDown(reinterpret_cast<uintptr_t>(Ptr) + Size - 1, GuardSize);
  const bool GuardProtected = FEXCore::Allocator::VirtualProtect(reinterpret_cast<void*>(LastPageAddr), GuardSize,
                                                               FEXCore::Allocator::ProtectOptions::None);
  if (!GuardProtected ||
      !ExecutableCode::Reserve(ExecutableCode::RangeKind::SharedJIT, Ptr, AllocatedSize, GuardSize)) {
    FEXCore::Allocator::VirtualFree(Ptr, AllocatedSize);
    Ptr = nullptr;
    return;
  }
  ExecutableRangeReserved = true;

  FEXCore::Allocator::VirtualName("FEXMemJIT", Ptr, Size);

  // Huge-pages reduce the amount of iTLB misses dramatically when it works.
  FEXCore::Allocator::VirtualTHPControl(Ptr, Size, FEXCore::Allocator::THPControl::Enable);

  LOGMAN_THROW_A_FMT(LookupCache != nullptr, "Code buffer requires a shared lookup map");
  LOGMAN_THROW_A_FMT(GenerationRanges != nullptr, "Code buffer requires shared generation ranges");
}

CodeBuffer::~CodeBuffer() {
  if (Ptr == nullptr) {
    return;
  }
  if (!ExecutableRangeReserved || ExecutableCode::BeginRetire(ExecutableCode::RangeKind::SharedJIT, Ptr, AllocatedSize)) {
#ifdef _WIN32
    const bool Unmapped = ::VirtualFree(Ptr, 0, MEM_RELEASE) != 0;
#else
    const bool Unmapped = FEXCore::Allocator::munmap(Ptr, AllocatedSize) == 0;
#endif
    if (ExecutableRangeReserved) {
      ExecutableCode::FinishRetire(ExecutableCode::RangeKind::SharedJIT, Ptr, AllocatedSize, Unmapped);
    }
  }
}

fextl::shared_ptr<CodeBuffer> SharedCodeBufferManager::TryAllocateNew(size_t Size, bool RetainCurrentGeneration) {
#if !defined(_WIN32) && !defined(__APPLE__)
  // MDWE (Memory-Deny-Write-Execute) is a new Linux 6.3 feature.
  // It's equivalent to systemd's `MemoryDenyWriteExecute` but implemented entirely in the kernel.
  //
  // MDWE prevents applications from creating RWX memory mappings.
  // This prevents FEX from doing anything JIT related, as FEX uses RWX for JIT memory mappings.
  //
  // A potential workaround to make FEX work with MDWE is to call mprotect every time we need to write or modify code.
  // Alternatively, FEX could use a memory mirror where one half is mapped as RW and the other is RX.
  //
  // Once MDWE is enabled with the prctl, the feature is sealed and it can /NOT/ be turned off.
  //
  // Status of MDWE is queried through prctl using `PR_GET_MDWE`:
  // -1: The kernel doesn't support MDWE
  // 0: MDWE is supported but disabled
  // >0: MDWE is enabled, hence prohibiting RWX mappings
  int MDWE = ::prctl(PR_GET_MDWE, 0, 0, 0, 0);
  if (MDWE != -1 && MDWE != 0) {
    LogMan::Msg::EFmt("MDWE was set to 0x{:x} which means FEX can't allocate executable memory", MDWE);
  }
#endif

  const bool Retain = RetainCurrentGeneration && ActiveLookupCache != nullptr && ActiveGenerationRanges != nullptr;
  auto CandidateLookupCache = Retain ? ActiveLookupCache : fextl::make_shared<GuestToHostMap>();
  auto CandidateGenerationRanges =
    Retain ? ActiveGenerationRanges : fextl::make_shared<CodeBufferGenerationRanges>();
  auto Buffer = fextl::make_shared<CodeBuffer>(Size, CandidateLookupCache, CandidateGenerationRanges);
  if (!Buffer->Ptr) {
    return nullptr;
  }
  if (!CandidateGenerationRanges->Publish(Buffer->Ptr, Buffer->UsableSize())) {
    return nullptr;
  }

  if (!Retain) {
    if (!CurrentSegments.empty()) {
      RetiredGenerations.emplace_back(std::move(CurrentSegments));
      TelemetryGenerationResets.fetch_add(1, std::memory_order_relaxed);
    }
    ActiveLookupCache = std::move(CandidateLookupCache);
    ActiveGenerationRanges = std::move(CandidateGenerationRanges);
    TelemetryGeneration.fetch_add(1, std::memory_order_relaxed);
    TelemetryCurrentUsedBytes.store(0, std::memory_order_relaxed);
    TelemetryCurrentCapacityBytes.store(0, std::memory_order_relaxed);
    LastLoggedUsedBucket = 0;
  } else {
    TelemetryRetainedRotations.fetch_add(1, std::memory_order_relaxed);
  }

  Latest = Buffer;
  LatestOffset = 0;
  LatestCommittedBytes = 0;
  CurrentSegments.emplace_back(Buffer);

  TelemetryCurrentSegments.store(CurrentSegments.size(), std::memory_order_relaxed);
  TelemetryCurrentCapacityBytes.fetch_add(Buffer->UsableSize(), std::memory_order_relaxed);
  TelemetryTotalAllocatedBytes.fetch_add(Buffer->AllocatedSize, std::memory_order_relaxed);

  PruneRetiredGenerations();

  OnCodeBufferAllocated(Buffer);

  const auto Stats = SnapshotTelemetry();
  LogMan::Msg::IFmt(
    "FEX JIT code cache generation={} segments={} retained_budget={} MiB retained_rotations={} generation_resets={} capacity={} MiB retired_generations={} retired_segments={}",
    Stats.Generation, Stats.CurrentSegments, RetainedCodeBudgetBytes() / 1024 / 1024, Stats.RetainedRotations,
    Stats.GenerationResets, Stats.CurrentCapacityBytes / 1024 / 1024, Stats.RetiredGenerations, Stats.RetiredSegments);

  return Buffer;
}

fextl::shared_ptr<CodeBuffer> SharedCodeBufferManager::AllocateNew(size_t Size) {
  if (auto Buffer = TryAllocateNew(Size, false)) {
    return Buffer;
  }

  ERROR_AND_DIE_FMT("Couldn't allocate a {} MiB shared JIT code buffer", Size / 1024 / 1024);
}

fextl::shared_ptr<CodeBuffer> SharedCodeBufferManager::GetLatest() {
  if (!Latest) {
    AllocateNew(INITIAL_CODE_SIZE);
  }
  return Latest;
}

fextl::shared_ptr<CodeBuffer> SharedCodeBufferManager::StartLargerCodeBuffer() {
  if (!Latest) {
    // Allocate initial CodeBuffer and return it
    return GetLatest();
  }

  const auto CurrentSize = GetLatest()->AllocatedSize;
  const auto PreferredSize = std::min<size_t>(CurrentSize * 2, MAX_CODE_SIZE);
  for (size_t Size = PreferredSize; Size >= INITIAL_CODE_SIZE; Size /= 2) {
    const bool Retain = RetainedCodeBufferSegmentsAllowed() && CanRetainCurrentGeneration(Size);
    if (auto Buffer = TryAllocateNew(Size, Retain)) {
      if (Size != PreferredSize) {
        LogMan::Msg::IFmt("Falling back to a {} MiB shared JIT code buffer", Size / 1024 / 1024);
      }
      return Buffer;
    }
  }

  ERROR_AND_DIE_FMT("Couldn't rotate the shared JIT code buffer");
}

fextl::shared_ptr<CodeBuffer> SharedCodeBufferManager::StartMaximalCodeBuffer() {
  for (size_t Size = MAX_CODE_SIZE; Size >= INITIAL_CODE_SIZE; Size /= 2) {
    if (auto Buffer = TryAllocateNew(Size, false)) {
      if (Size != MAX_CODE_SIZE) {
        LogMan::Msg::IFmt("Falling back to a {} MiB shared JIT code buffer", Size / 1024 / 1024);
      }
      return Buffer;
    }
  }

  ERROR_AND_DIE_FMT("Couldn't allocate a shared JIT code buffer (tried 128, 64, 32, and 16 MiB)");
}

void SharedCodeBufferManager::RecordCodeCommit() {
  LOGMAN_THROW_A_FMT(Latest != nullptr, "Code commit without an active JIT code buffer");
  LOGMAN_THROW_A_FMT(LatestOffset >= LatestCommittedBytes, "JIT code buffer commit offset moved backwards");

  const uint64_t Delta = LatestOffset - LatestCommittedBytes;
  LatestCommittedBytes = LatestOffset;
  const uint64_t Used = TelemetryCurrentUsedBytes.fetch_add(Delta, std::memory_order_relaxed) + Delta;

  constexpr uint64_t LOG_BUCKET_SIZE = 16ULL * 1024 * 1024;
  const uint64_t Bucket = Used / LOG_BUCKET_SIZE;
  if (Bucket != 0 && Bucket != LastLoggedUsedBucket) {
    LastLoggedUsedBucket = Bucket;
    const auto Stats = SnapshotTelemetry();
    LogMan::Msg::IFmt(
      "FEX JIT code cache usage generation={} segments={} used={} MiB capacity={} MiB rotations={} generation_resets={}",
      Stats.Generation, Stats.CurrentSegments, Stats.CurrentUsedBytes / 1024 / 1024,
      Stats.CurrentCapacityBytes / 1024 / 1024, Stats.RetainedRotations, Stats.GenerationResets);
  }
}

void SharedCodeBufferManager::PruneRetiredGenerations() {
  auto It = RetiredGenerations.begin();
  while (It != RetiredGenerations.end()) {
    const bool HasExternalOwner = std::ranges::any_of(*It, [](const auto& Buffer) {
      return Buffer.use_count() != 1;
    });
    if (HasExternalOwner) {
      ++It;
    } else {
      It = RetiredGenerations.erase(It);
    }
  }

  uint64_t RetiredSegmentCount {};
  for (const auto& Generation : RetiredGenerations) {
    RetiredSegmentCount += Generation.size();
  }
  TelemetryRetiredGenerations.store(RetiredGenerations.size(), std::memory_order_relaxed);
  TelemetryRetiredSegments.store(RetiredSegmentCount, std::memory_order_relaxed);
}
} // namespace FEXCore::CPU
