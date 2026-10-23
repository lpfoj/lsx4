// SPDX-License-Identifier: MIT

#pragma once

#include <FEXCore/Utils/CompilerDefs.h>

#include <cstdint>

extern "C" {

enum Lsx4FexExactStoreWatchFlagsV1 : uint32_t {
  LSX4_FEX_EXACT_STORE_WATCH_ENABLE_INSTRUMENTATION = 1U << 0,
  LSX4_FEX_EXACT_STORE_WATCH_ARM_RANGE = 1U << 1,
  LSX4_FEX_EXACT_STORE_WATCH_CLEAR_HIT = 1U << 2,
};

struct Lsx4FexExactStoreWatchHitV1 {
  uint32_t struct_size;
  uint32_t sequence;
  uint64_t guest_rip;
  uint64_t effective_address;
  uint32_t access_size;
  uint32_t reserved;
};

FEX_DEFAULT_VISIBILITY int32_t
lsx4_fexcore_exact_store_watch_configure_v1(uint64_t begin, uint64_t end,
                                             uint32_t flags) noexcept;

FEX_DEFAULT_VISIBILITY int32_t
lsx4_fexcore_exact_store_watch_take_v1(
  Lsx4FexExactStoreWatchHitV1* hit) noexcept;

}

namespace FEXCore::CPU {

[[nodiscard]] bool ExactStoreWatchCompilationEnabled() noexcept;

}
