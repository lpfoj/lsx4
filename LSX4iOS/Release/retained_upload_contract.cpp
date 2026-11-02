// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../../funnel-arm/src/video_core/buffer_cache/upload_snapshot_retirement.h"
#include <cassert>
#include <initializer_list>

int main() {
    using Lsx4::Portable::CanRetainCommittedSnapshot;
    using Lsx4::Portable::CommittedUploadSnapshot;
    const CommittedUploadSnapshot valid{true, true, true, true, false, false, true,
                                        4096, 2048, 8192, 16384, 7, 7};
    assert(CanRetainCommittedSnapshot(valid));
    auto written = valid;
    written.destination_written = true;
    assert(!CanRetainCommittedSnapshot(written));
    for (auto field : {&CommittedUploadSnapshot::transaction_complete,
                       &CommittedUploadSnapshot::committed_stream,
                       &CommittedUploadSnapshot::exact_dirty,
                       &CommittedUploadSnapshot::distinct_source,
                       &CommittedUploadSnapshot::matching_handle}) {
        auto incomplete = valid;
        incomplete.*field = false;
        assert(!CanRetainCommittedSnapshot(incomplete));
    }
    auto transient = valid;
    transient.transient = true;
    assert(!CanRetainCommittedSnapshot(transient));
    for (auto field : {&CommittedUploadSnapshot::reserved_bytes,
                       &CommittedUploadSnapshot::copied_bytes,
                       &CommittedUploadSnapshot::commit_tick}) {
        auto empty = valid;
        empty.*field = 0;
        assert(!CanRetainCommittedSnapshot(empty));
    }
    auto oversized = valid;
    oversized.copied_bytes = 4097;
    assert(!CanRetainCommittedSnapshot(oversized));
    auto chunk = valid;
    chunk.chunk_limit = 4095;
    assert(!CanRetainCommittedSnapshot(chunk));
    auto stream = valid;
    stream.stream_limit = 4095;
    assert(!CanRetainCommittedSnapshot(stream));
    auto stale = valid;
    stale.consuming_tick = 8;
    assert(!CanRetainCommittedSnapshot(stale));
}
