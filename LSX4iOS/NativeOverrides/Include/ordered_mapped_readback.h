// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstdlib>

namespace PortableGpu {
inline bool OrderedMappedReadbackEnabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("EXECUTOR_ORDERED_MAPPED_READBACK");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

// The caller keeps the live allocation on its owning GPU thread until the
// existing synchronous retirement wait returns. Mapping is not an ownership
// reference. HOST_COHERENT also does not replace a GPU-to-HOST barrier.
constexpr bool OrderedMappedRangeEligible(bool gpu_owner, bool incomplete_proof,
    bool allocation_valid, bool deleted, bool coherent, std::uint64_t mapped_size,
    std::uint64_t offset, std::uint64_t size) noexcept {
    return gpu_owner && !incomplete_proof && allocation_valid && !deleted && coherent &&
        size != 0 && offset <= mapped_size && size <= mapped_size - offset;
}
}
