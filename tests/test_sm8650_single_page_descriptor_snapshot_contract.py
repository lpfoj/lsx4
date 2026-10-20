import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "funnel-arm/src/video_core/page_manager.cpp").read_text(
    encoding="utf-8"
)


class Sm8650SinglePageDescriptorSnapshotContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        gate_start = SOURCE.index(
            "bool ExecutorSinglePageDescriptorSnapshotFastPathEnabled()"
        )
        gate_end = SOURCE.index(
            "bool ExecutorSingleChunkDescriptorSnapshotFastPathEnabled()", gate_start
        )
        cls.gate = SOURCE[gate_start:gate_end]
        track_start = SOURCE.index(
            "DescriptorSnapshotTrackResult TryTrackDescriptorSnapshotWriteSinglePage"
        )
        untrack_start = SOURCE.index(
            "bool UntrackDescriptorSnapshotWriteSinglePage", track_start
        )
        end = SOURCE.index(
            "DescriptorSnapshotTrackResult TryTrackDescriptorSnapshotWriteSingleChunk",
            untrack_start,
        )
        cls.track = SOURCE[track_start:untrack_start]
        cls.untrack = SOURCE[untrack_start:end]

    def test_gate_keeps_marker_and_adds_snapdragon_defaults(self):
        self.assertIn(
            "run-enable-single-page-descriptor-snapshot-fast-path", self.gate
        )
        self.assertIn("Common::IsSnapdragon8Gen3Host()", self.gate)
        self.assertIn("Common::IsSnapdragon8Gen5Host()", self.gate)
        self.assertIn("requested || snapdragon_default", self.gate)
        self.assertIn("requested=%u snapdragonDefault=%u", self.gate)

    def test_track_keeps_exact_single_page_contract(self):
        required = (
            "std::scoped_lock lk{locks[page / PAGES_PER_LOCK]}",
            "PageChunk& chunk = EnsureChunk(page)",
            "state.num_bulk_writers != 0",
            "state.bulk_writer_overflow != 0",
            "chunk.write_fault_bypass",
            "state.descriptor_snapshot_write_watcher != 0",
            "chunk.descriptor_snapshot_conflict",
            "CloseFexDeferredWriteBypassSinglePage",
            "PublishWriteWatcherHazard(chunk, local_page, state)",
            "AdvanceFaultGeneration(chunk, local_page, true)",
            "Protect(static_cast<VAddr>(page)",
        )
        for proof in required:
            with self.subTest(proof=proof):
                self.assertIn(proof, self.track)

    def test_untrack_keeps_conflict_underflow_and_protection(self):
        required = (
            "std::scoped_lock lk{locks[page / PAGES_PER_LOCK]}",
            "PageChunk& chunk = EnsureChunk(page)",
            "const bool conflicted =",
            "Descriptor snapshot watcher underflow",
            "PublishWriteWatcherHazard(chunk, local_page, state)",
            "AdvanceFaultGeneration(chunk, local_page, true)",
            "chunk.descriptor_snapshot_conflict",
            "Protect(static_cast<VAddr>(page)",
        )
        for proof in required:
            with self.subTest(proof=proof):
                self.assertIn(proof, self.untrack)

    def test_multi_page_child_remains_separately_marker_gated(self):
        child_gate = SOURCE.split(
            "bool ExecutorSingleChunkDescriptorSnapshotFastPathEnabled()", 1
        )[1].split("bool ExecutorDescriptorSnapshotLocalChunkHoistEnabled()", 1)[0]
        self.assertIn(
            "run-enable-single-chunk-descriptor-snapshot-fast-path", child_gate
        )
        self.assertIn("return requested && parent_enabled", child_gate)


if __name__ == "__main__":
    unittest.main()
