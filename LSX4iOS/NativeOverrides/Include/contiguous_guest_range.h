#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>

namespace Lsx4::Portable {
// A descriptor can cover adjacent mappings, but cannot jump an unmapped hole.
// The caller holds the mapping table's read lease for the complete traversal.
template <class Iterator>
std::uint64_t ClampContiguousMappedRange(Iterator current, Iterator finish,
                                       std::uint64_t address, std::uint64_t requested) noexcept {
    if (requested == 0 || current == finish) return 0;
    const auto& first = current->second;
    if (!first.IsMapped() || first.size == 0 ||
        first.base > std::numeric_limits<std::uint64_t>::max() - first.size) return 0;
    std::uint64_t end = first.base + first.size;
    if (address < first.base || address >= end) return 0;
    std::uint64_t available = end - address;
    for (++current; current != finish && available < requested; ++current) {
        const auto& next = current->second;
        if (!next.IsMapped() || next.base != end || next.size == 0 ||
            next.base > std::numeric_limits<std::uint64_t>::max() - next.size) break;
        const std::uint64_t needed = requested - available;
        if (next.size >= needed) return requested;
        available += next.size;
        end = next.base + next.size;
    }
    return std::min(available, requested);
}
} // namespace Lsx4::Portable
