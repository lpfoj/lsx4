// SPDX-License-Identifier: MIT
#include <FEXCore/Core/ExecutableCodeObserver.h>

#include <atomic>

namespace FEXCore::ExecutableCode {
namespace {

std::atomic<const ObserverV2*> ProcessObserver {};
static_assert(std::atomic<const ObserverV2*>::is_always_lock_free);

const ObserverV2* GetObserver() noexcept {
  return ProcessObserver.load(std::memory_order_acquire);
}

bool IsValidV2(const ObserverV2* Observer) noexcept {
  return Observer != nullptr && Observer->StructSize >= sizeof(ObserverV2) && Observer->Version == OBSERVER_ABI_V2 &&
         Observer->Reserve != nullptr && Observer->Publish != nullptr && Observer->PublishBlock != nullptr &&
         Observer->BeginRetire != nullptr && Observer->FinishRetire != nullptr;
}

bool IsValidV3(const ObserverV3* Observer) noexcept {
  return Observer != nullptr && Observer->StructSize >= sizeof(ObserverV3) && Observer->Version == OBSERVER_ABI_V3 &&
         Observer->Reserve != nullptr && Observer->Publish != nullptr && Observer->PublishBlock != nullptr &&
         Observer->BeginRetire != nullptr && Observer->FinishRetire != nullptr && Observer->PublishBlocks != nullptr;
}

} // namespace

bool InstallObserverV2(const ObserverV2* Observer) noexcept {
  if (!IsValidV2(Observer)) {
    return false;
  }
  const ObserverV2* expected {};
  return ProcessObserver.compare_exchange_strong(expected, Observer, std::memory_order_release, std::memory_order_acquire) || expected == Observer;
}

bool InstallObserverV3(const ObserverV3* Observer) noexcept {
  if (!IsValidV3(Observer)) {
    return false;
  }
  const auto* Prefix = reinterpret_cast<const ObserverV2*>(Observer);
  const ObserverV2* expected {};
  return ProcessObserver.compare_exchange_strong(expected, Prefix, std::memory_order_release, std::memory_order_acquire) || expected == Prefix;
}

bool ObserverInstalled() noexcept {
  return GetObserver() != nullptr;
}

bool Reserve(RangeKind Kind, const void* Base, size_t Size, size_t Granule) noexcept {
  const auto* Observer = GetObserver();
  return Observer == nullptr || Observer->Reserve(Observer->Opaque, Kind, Base, Size, Granule);
}

bool Publish(RangeKind Kind, const void* Base, size_t Size) noexcept {
  const auto* Observer = GetObserver();
  return Observer == nullptr || Observer->Publish(Observer->Opaque, Kind, Base, Size);
}

bool PublishBlock(RangeKind Kind, const void* BlockBegin, size_t BlockSize) noexcept {
  const auto* Observer = GetObserver();
  return Observer == nullptr || Observer->PublishBlock(Observer->Opaque, Kind, BlockBegin, BlockSize);
}

bool PublishBlocks(RangeKind Kind, const BlockRange* Blocks, size_t Count) noexcept {
  const auto* Observer = GetObserver();
  if (Observer == nullptr) {
    return true;
  }
  if (Observer->Version >= OBSERVER_ABI_V3 && Observer->StructSize >= sizeof(ObserverV3)) {
    const auto* ObserverV3Ptr = reinterpret_cast<const ObserverV3*>(Observer);
    return ObserverV3Ptr->PublishBlocks(ObserverV3Ptr->Opaque, Kind, Blocks, Count);
  }

  // V2 has no all-or-nothing batch contract. Only preserve compatibility for
  // a single block; callers requiring a transaction must fail closed.
  return Count == 0 || (Count == 1 && Blocks != nullptr && PublishBlock(Kind, Blocks[0].Begin, Blocks[0].Size));
}

bool BeginRetire(RangeKind Kind, const void* Base, size_t Size) noexcept {
  const auto* Observer = GetObserver();
  return Observer == nullptr || Observer->BeginRetire(Observer->Opaque, Kind, Base, Size);
}

void FinishRetire(RangeKind Kind, const void* Base, size_t Size, bool Unmapped) noexcept {
  const auto* Observer = GetObserver();
  if (Observer != nullptr) {
    Observer->FinishRetire(Observer->Opaque, Kind, Base, Size, Unmapped);
  }
}

} // namespace FEXCore::ExecutableCode
