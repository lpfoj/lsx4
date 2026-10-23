// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
namespace Lsx4::Portable {
// Existing StreamBuffer::Commit records the consuming queue tick. Map/Wrap
// wait for that tick before reusing any overlapping bytes. A small persistent
// upload therefore needs queue ordering, not whole-queue CPU retirement.
inline bool RetainPersistentUpload(bool enabled, bool committed_stream,
    bool exact_dirty, bool written, bool texel, bool external_guard,
    bool transient, bool matching_handle, std::uint64_t reserved,
    std::uint64_t copied, std::uint64_t chunk_limit, std::uint64_t stream_limit) {
    return enabled && committed_stream && exact_dirty && !written && !texel &&
        !external_guard && !transient && matching_handle && copied!=0 &&
        reserved!=0 && copied<=reserved && reserved<=chunk_limit && reserved<=stream_limit;
}
}
