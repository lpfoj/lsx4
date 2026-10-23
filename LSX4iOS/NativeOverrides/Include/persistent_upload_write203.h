// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "persistent_stream_retirement.h"
namespace Lsx4::Portable {
// Destination shader writes do not modify the distinct committed staging
// source. Retire that source using its original Map/Commit timeline watches.
// Destination ownership, copy barriers and demand readback waits stay intact.
inline bool RetainWritableUploadSource(bool enabled, bool destination_written,
    bool distinct_source, bool committed_stream, bool exact_dirty, bool texel,
    bool external_guard, bool transient, bool matching_handle,
    std::uint64_t reserved, std::uint64_t copied, std::uint64_t chunk_limit,
    std::uint64_t stream_limit) {
    return destination_written && distinct_source && RetainPersistentUpload(
        enabled,committed_stream,exact_dirty,false,texel,external_guard,
        transient,matching_handle,reserved,copied,chunk_limit,stream_limit);
}
}
