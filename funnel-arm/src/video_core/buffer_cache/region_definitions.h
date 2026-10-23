// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/bit_array.h"
#include "common/types.h"

namespace VideoCore {

#if defined(LSX4_APPLE_RUNTIME)
// Apple arm64 protects memory in 16 KiB units. Every ownership bit and watcher
// must cover that same unit; tracking a 4 KiB subpage cannot protect it alone.
constexpr u64 TRACKER_PAGE_BITS = 14;
#else
constexpr u64 TRACKER_PAGE_BITS = 12;
#endif
constexpr u64 TRACKER_BYTES_PER_PAGE = 1ULL << TRACKER_PAGE_BITS;

constexpr u64 TRACKER_HIGHER_PAGE_BITS = 22;
constexpr u64 TRACKER_HIGHER_PAGE_SIZE = 1ULL << TRACKER_HIGHER_PAGE_BITS;
constexpr u64 TRACKER_HIGHER_PAGE_MASK = TRACKER_HIGHER_PAGE_SIZE - 1ULL;
constexpr u64 NUM_PAGES_PER_REGION = TRACKER_HIGHER_PAGE_SIZE / TRACKER_BYTES_PER_PAGE;

enum class Type {
    CPU,
    GPU,
};

enum class CpuOwnedWritePublishResult : u8 {
    Complete,
    MissingManager,
    Busy,
    GpuDirty,
};

using RegionBits = Common::BitArray<NUM_PAGES_PER_REGION>;

}
