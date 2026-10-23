// SPDX-License-Identifier: MIT
#pragma once

#include <FEXCore/Utils/CompilerDefs.h>

#include <cstddef>
#include <cstdint>

namespace FEXCore::ExecutableCode {

constexpr uint32_t OBSERVER_ABI_V2 = 2;
constexpr uint32_t OBSERVER_ABI_V3 = 3;

enum class RangeKind : uint32_t {
  SharedJIT = 1,
  PersistentCache = 2,
};

struct ObserverV2 {
  uint32_t StructSize;
  uint32_t Version;
  void* Opaque;
  bool (*Reserve)(void* Opaque, RangeKind Kind, const void* Base, size_t Size, size_t Granule) noexcept;
  bool (*Publish)(void* Opaque, RangeKind Kind, const void* Base, size_t Size) noexcept;
  bool (*PublishBlock)(void* Opaque, RangeKind Kind, const void* BlockBegin, size_t BlockSize) noexcept;
  bool (*BeginRetire)(void* Opaque, RangeKind Kind, const void* Base, size_t Size) noexcept;
  void (*FinishRetire)(void* Opaque, RangeKind Kind, const void* Base, size_t Size, bool Unmapped) noexcept;
};

static_assert(sizeof(ObserverV2) == 56);

struct BlockRange {
  const void* Begin;
  size_t Size;
};

// V3 extends V2 without changing the established prefix. PublishBlocks is a
// single transaction: an observer must prevalidate and reserve the complete
// batch before making any block discoverable, or return false with no visible
// registry change.
struct ObserverV3 {
  uint32_t StructSize;
  uint32_t Version;
  void* Opaque;
  bool (*Reserve)(void* Opaque, RangeKind Kind, const void* Base, size_t Size, size_t Granule) noexcept;
  bool (*Publish)(void* Opaque, RangeKind Kind, const void* Base, size_t Size) noexcept;
  bool (*PublishBlock)(void* Opaque, RangeKind Kind, const void* BlockBegin, size_t BlockSize) noexcept;
  bool (*BeginRetire)(void* Opaque, RangeKind Kind, const void* Base, size_t Size) noexcept;
  void (*FinishRetire)(void* Opaque, RangeKind Kind, const void* Base, size_t Size, bool Unmapped) noexcept;
  bool (*PublishBlocks)(void* Opaque, RangeKind Kind, const BlockRange* Blocks, size_t Count) noexcept;
};

static_assert(offsetof(ObserverV3, PublishBlocks) == sizeof(ObserverV2));
static_assert(sizeof(ObserverV3) == 64);

// Installation is process-lifetime and must happen before the first Context is initialized.
FEX_DEFAULT_VISIBILITY bool InstallObserverV2(const ObserverV2* Observer) noexcept;
FEX_DEFAULT_VISIBILITY bool InstallObserverV3(const ObserverV3* Observer) noexcept;
FEX_DEFAULT_VISIBILITY bool ObserverInstalled() noexcept;

bool Reserve(RangeKind Kind, const void* Base, size_t Size, size_t Granule) noexcept;
bool Publish(RangeKind Kind, const void* Base, size_t Size) noexcept;
bool PublishBlock(RangeKind Kind, const void* BlockBegin, size_t BlockSize) noexcept;
bool PublishBlocks(RangeKind Kind, const BlockRange* Blocks, size_t Count) noexcept;
bool BeginRetire(RangeKind Kind, const void* Base, size_t Size) noexcept;
void FinishRetire(RangeKind Kind, const void* Base, size_t Size, bool Unmapped) noexcept;

} // namespace FEXCore::ExecutableCode
