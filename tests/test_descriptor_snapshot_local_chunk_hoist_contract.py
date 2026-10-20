import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "funnel-arm" / "src" / "video_core" / "page_manager.cpp"


class DescriptorSnapshotLocalChunkHoistContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = SOURCE.read_text(encoding="utf-8")
        track_start = cls.source.index(
            "DescriptorSnapshotTrackResult TryTrackDescriptorSnapshotWrite(VAddr"
        )
        untrack_start = cls.source.index(
            "bool UntrackDescriptorSnapshotWrite(VAddr", track_start
        )
        end = cls.source.index("template <bool track>", untrack_start)
        cls.track = cls.source[track_start:untrack_start]
        cls.untrack = cls.source[untrack_start:end]

    def test_candidate_keeps_marker_and_defaults_on_snapdragon(self):
        self.assertIn(
            "run-enable-descriptor-snapshot-local-chunk-hoist", self.source
        )
        self.assertIn(
            "ExecutorDescriptorSnapshotLocalChunkHoistEnabled()", self.source
        )
        gate_start = self.source.index(
            "bool ExecutorDescriptorSnapshotLocalChunkHoistEnabled()"
        )
        gate_end = self.source.index(
            "bool ExecutorSm8650DescriptorHazardWordPublishEnabled()", gate_start
        )
        gate = self.source[gate_start:gate_end]
        self.assertIn("Common::IsSnapdragon8Gen3Host()", gate)
        self.assertIn("Common::IsSnapdragon8Gen5Host()", gate)
        self.assertIn("requested || snapdragon_default", gate)
        self.assertIn("requested=%u snapdragonDefault=%u", gate)
        self.assertNotIn(
            "run-enable-descriptor-snapshot-local-chunk-hoist",
            self.source[: self.source.index("#ifdef __ANDROID__")],
        )

    def test_track_hoists_chunk_without_dropping_contracts(self):
        required = (
            "PageChunk& chunk = EnsureChunk(probe)",
            "state.num_bulk_writers != 0",
            "state.bulk_writer_overflow != 0",
            "chunk.write_fault_bypass",
            "state.descriptor_snapshot_write_watcher != 0",
            "chunk.descriptor_snapshot_conflict",
            "CloseFexDeferredWriteBypassSinglePage",
            "PublishWriteWatcherHazard(chunk, local_page, state)",
            "AdvanceFaultGeneration(chunk, local_page, true)",
            "append_protection(page, new_perms)",
            "rangeLockPreserved=1 exactState=1",
        )
        for proof in required:
            with self.subTest(proof=proof):
                self.assertIn(proof, self.track)

    def test_untrack_hoists_chunk_and_reads_conflict_once(self):
        required = (
            "PageChunk& chunk = EnsureChunk(page)",
            "const bool conflicted =",
            "Descriptor snapshot watcher underflow",
            "chunk.descriptor_snapshot_conflict",
            "PublishWriteWatcherHazard(chunk, local_page, state)",
            "AdvanceFaultGeneration(chunk, local_page, true)",
            "append_protection(page, new_perms)",
            "clean ? 1u : 0u",
        )
        for proof in required:
            with self.subTest(proof=proof):
                self.assertIn(proof, self.untrack)

    def test_legacy_path_and_d58_d76_selection_remain_present(self):
        required = (
            "TryTrackDescriptorSnapshotWriteSinglePage(page)",
            "TryTrackDescriptorSnapshotWriteSingleChunk(page, page_end)",
            "BulkWriteActive(probe) || WriteFaultBypassed(probe)",
            "PublishWriteWatcherHazard(page, state)",
            "AdvanceFaultGeneration(page, true)",
            "UntrackDescriptorSnapshotWriteSinglePage(page)",
            "UntrackDescriptorSnapshotWriteSingleChunk(page, page_end)",
            "DescriptorSnapshotConflicted(page)",
            "SetDescriptorSnapshotConflict(page, false)",
        )
        combined = self.track + self.untrack
        for proof in required:
            with self.subTest(proof=proof):
                self.assertIn(proof, combined)


if __name__ == "__main__":
    unittest.main()
