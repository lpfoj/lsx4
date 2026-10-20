import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "funnel-arm" / "src" / "video_core" / "page_manager.cpp"


class SingleChunkDescriptorSnapshotFastPathContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = SOURCE.read_text(encoding="utf-8")
        track_start = cls.source.index(
            "DescriptorSnapshotTrackResult TryTrackDescriptorSnapshotWriteSingleChunk"
        )
        untrack_start = cls.source.index(
            "bool UntrackDescriptorSnapshotWriteSingleChunk", track_start
        )
        public_track = cls.source.index(
            "DescriptorSnapshotTrackResult TryTrackDescriptorSnapshotWrite(VAddr", untrack_start
        )
        cls.track = cls.source[track_start:untrack_start]
        cls.untrack = cls.source[untrack_start:public_track]

    def test_candidate_is_default_off_and_requires_d58_parent(self):
        marker = "run-enable-single-chunk-descriptor-snapshot-fast-path"
        self.assertIn(marker, self.source)
        self.assertIn("return requested && parent_enabled;", self.source)
        self.assertIn("ExecutorSinglePageDescriptorSnapshotFastPathEnabled()", self.source)

    def test_only_exact_multi_page_single_chunk_ranges_select_candidate(self):
        required = (
            "page_end > page + 1",
            "page / PAGES_PER_LOCK == (page_end - 1) / PAGES_PER_LOCK",
            "TryTrackDescriptorSnapshotWriteSingleChunk(page, page_end)",
            "UntrackDescriptorSnapshotWriteSingleChunk(page, page_end)",
        )
        for proof in required:
            with self.subTest(proof=proof):
                self.assertIn(proof, self.source)

    def test_track_keeps_all_busy_overlap_conflict_and_protection_proofs(self):
        required = (
            "state.num_bulk_writers != 0",
            "state.bulk_writer_overflow != 0",
            "chunk.write_fault_bypass",
            "state.descriptor_snapshot_write_watcher != 0",
            "chunk.descriptor_snapshot_conflict",
            "CloseFexDeferredWriteBypassSinglePage",
            "PublishWriteWatcherHazard(chunk, local_page, state)",
            "AdvanceFaultGeneration(chunk, local_page, true)",
            "Protect(static_cast<VAddr>(run.first_page)",
        )
        for proof in required:
            with self.subTest(proof=proof):
                self.assertIn(proof, self.track)

    def test_untrack_keeps_conflict_underflow_generation_and_protection_semantics(self):
        required = (
            "Descriptor snapshot watcher underflow",
            "clean = false",
            "chunk.descriptor_snapshot_conflict",
            "PublishWriteWatcherHazard(chunk, local_page, state)",
            "AdvanceFaultGeneration(chunk, local_page, true)",
            "Protect(static_cast<VAddr>(run.first_page)",
        )
        for proof in required:
            with self.subTest(proof=proof):
                self.assertIn(proof, self.untrack)

    def test_hot_range_does_one_chunk_lookup_per_phase(self):
        self.assertEqual(self.track.count("EnsureChunk(page)"), 1)
        self.assertEqual(self.untrack.count("EnsureChunk(page)"), 1)
        self.assertNotIn("State(", self.track)
        self.assertNotIn("State(", self.untrack)
        self.assertIn("chunkLookup=1 exactRange=1", self.track)
        self.assertIn("chunkLookup=1 exactRange=1", self.untrack)


if __name__ == "__main__":
    unittest.main()
