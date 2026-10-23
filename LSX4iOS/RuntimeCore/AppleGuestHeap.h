// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstddef>
#include <limits>
#include <sys/mman.h>

namespace Lsx4::Apple {
// A chunk may be smaller than the preferred host reservation, never smaller
// than the guest request. Callers retain all published chunks for their normal
// lifetime and register the actual successful capacity with guest readers.
template<class Mapper>
void* MapGuestHeapChunkUsing(std::size_t minimum, std::size_t preferred,
                            std::size_t page, std::size_t& capacity, Mapper map) {
    capacity = 0;
    const auto limit = std::numeric_limits<std::size_t>::max();
    if (!minimum || !page || (page & (page - 1)) ||
        minimum > limit - (page - 1) || preferred > limit - (page - 1))
        return MAP_FAILED;
    const auto required = (minimum + page - 1) & ~(page - 1);
    auto candidate = (std::max(minimum, preferred) + page - 1) & ~(page - 1);
    for (;;) {
        void* result = map(candidate);
        if (result != MAP_FAILED) {
            capacity = candidate;
            return result;
        }
        if (candidate == required) return MAP_FAILED;
        candidate = std::max(required, (candidate / 2) & ~(page - 1));
    }
}
void* MapGuestHeapChunk(std::size_t minimum, std::size_t preferred,
                        std::size_t& capacity) noexcept;
}
