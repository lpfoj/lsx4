// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cstdint>

namespace Lsx4::Portable {
enum class RangeTransferResult { Ready, Retryable, Failed };
struct RangeTransferStats {
    std::uint64_t committedBytes{}, attempts{}, refinements{};
};

// A contended transaction may be retried with a smaller range only after the
// producer has released all temporary leases. Committed prefixes alone never
// admit a consumer: every byte must reach Ready before this returns true.
template<class Transfer>
bool TransferAdaptiveRange(std::uint64_t bytes, std::uint64_t maximum,
                           std::uint64_t minimum, Transfer&& transfer,
                           RangeTransferStats& stats) {
    stats = {};
    if (!bytes || !minimum || maximum < minimum) return false;
    while (stats.committedBytes < bytes) {
        auto span = std::min(maximum, bytes - stats.committedBytes);
        for (;;) {
            ++stats.attempts;
            const auto result = transfer(stats.committedBytes, span);
            if (result == RangeTransferResult::Ready) {
                stats.committedBytes += span;
                break;
            }
            if (result != RangeTransferResult::Retryable || span <= minimum)
                return false;
            span = std::max(minimum, span / 2);
            ++stats.refinements;
        }
    }
    return true;
}
}
