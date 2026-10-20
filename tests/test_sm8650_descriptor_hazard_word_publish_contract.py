from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "funnel-arm" / "src" / "video_core" / "page_manager.cpp"


class Sm8650DescriptorHazardWordPublishContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = SOURCE.read_text(encoding="utf-8")
        cls.track_start = cls.source.index(
            "DescriptorSnapshotTrackResult TryTrackDescriptorSnapshotWrite(VAddr"
        )
        cls.untrack_start = cls.source.index(
            "bool ConsumeDescriptorSnapshotWriteFault", cls.track_start
        )
        cls.track = cls.source[cls.track_start : cls.untrack_start]

    def test_default_is_hardware_gated_to_snapdragon_8_gen_3(self):
        gate_start = self.source.index(
            "bool ExecutorSm8650DescriptorHazardWordPublishEnabled()"
        )
        gate_end = self.source.index(
            "bool ExecutorShouldSampleGuestInvalidationSources()", gate_start
        )
        gate = self.source[gate_start:gate_end]
        self.assertIn("Common::IsSnapdragon8Gen3Host()", gate)
        self.assertNotIn("Adreno840", gate)
        self.assertNotIn("run-enable-", gate)

    def test_word_publish_is_inside_existing_transaction_guards(self):
        cutoff = self.track.index("UnwatchedBulkWriteCutoff unwatched_cutoff")
        range_lock = self.track.index("Common::RangeLockGuard lk")
        word_path = self.track.index(
            "ExecutorSm8650DescriptorHazardWordPublishEnabled()"
        )
        protect = self.track.index("for (const ProtectionRun& run")
        self.assertLess(cutoff, range_lock)
        self.assertLess(range_lock, word_path)
        self.assertLess(word_path, protect)

    def test_exact_page_state_and_generation_precede_one_release_per_word(self):
        required = (
            "PageChunk& chunk = EnsureChunk(page)",
            "const size_t word_index = first_local_page >> 6",
            "state.descriptor_snapshot_write_watcher = 1",
            "hazard_mask |= u64{1} << (local_page & 63)",
            "AdvanceFaultGeneration(chunk, local_page, true)",
            "chunk.write_watcher_hazard[word_index].fetch_or(\n"
            "                        hazard_mask, std::memory_order_release)",
            "append_protection(page, new_perms)",
        )
        for proof in required:
            with self.subTest(proof=proof):
                self.assertIn(proof, self.track)

    def test_non_sm8650_legacy_publication_path_remains_intact(self):
        self.assertIn("} else {\n            for (; page != page_end; ++page)", self.track)
        self.assertIn(
            "PublishWriteWatcherHazard(chunk, local_page, state)", self.track
        )
        self.assertIn("PublishWriteWatcherHazard(page, state)", self.track)


if __name__ == "__main__":
    unittest.main()
