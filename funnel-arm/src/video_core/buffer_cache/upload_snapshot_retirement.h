// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace Lsx4::Portable {
struct CommittedUploadSnapshot {
    bool transaction_complete;
    bool committed_stream;
    bool exact_dirty;
    bool distinct_source;
    bool destination_written;
    bool transient;
    bool matching_handle;
    std::uint64_t reserved_bytes;
    std::uint64_t copied_bytes;
    std::uint64_t chunk_limit;
    std::uint64_t stream_limit;
    std::uint64_t commit_tick;
    std::uint64_t consuming_tick;
};

// A completed private staging copy no longer reads guest mappings. Its source
// bytes are protected by StreamBuffer's consuming-tick watches, independently
// of how long the caller retains its guest mapping guard. Never infer this
// from the presence of a guard alone: all copy and queue identity proofs apply.
inline bool CanRetainCommittedSnapshot(const CommittedUploadSnapshot& s) noexcept {
    return s.transaction_complete && s.committed_stream && s.exact_dirty &&
        s.distinct_source && !s.destination_written && !s.transient && s.matching_handle &&
        s.copied_bytes != 0 && s.reserved_bytes != 0 &&
        s.copied_bytes <= s.reserved_bytes && s.reserved_bytes <= s.chunk_limit &&
        s.reserved_bytes <= s.stream_limit && s.commit_tick != 0 &&
        s.commit_tick == s.consuming_tick;
}
}
