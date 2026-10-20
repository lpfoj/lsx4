from pathlib import Path
import random
import unittest


ROOT = Path(__file__).resolve().parents[1]
REGION = (
    ROOT / "funnel-arm/src/video_core/buffer_cache/region_manager.h"
).read_text(encoding="utf-8")
TRACKER = (
    ROOT / "funnel-arm/src/video_core/buffer_cache/memory_tracker.h"
).read_text(encoding="utf-8")
PAGE_MANAGER = (
    ROOT / "funnel-arm/src/video_core/page_manager.cpp"
).read_text(encoding="utf-8")


class DirectRegionModifiedRangeContractTests(unittest.TestCase):
    def test_candidate_keeps_marker_and_adds_snapdragon_defaults(self):
        self.assertIn("run-enable-direct-region-modified-range", PAGE_MANAGER)
        self.assertIn("direct-region-modified-range.txt", PAGE_MANAGER)
        self.assertIn("selected=1 exactAnyInRange=1", PAGE_MANAGER)
        self.assertIn("legacyFallbackPreserved=1", PAGE_MANAGER)
        gate = PAGE_MANAGER.split(
            "bool ExecutorDirectRegionModifiedRangeEnabled()", 1
        )[1].split("#ifdef __ANDROID__\nnamespace {", 1)[0]
        self.assertIn("Common::IsSnapdragon8Gen3Host()", gate)
        self.assertIn("Common::IsSnapdragon8Gen5Host()", gate)
        self.assertIn("if (!requested && !snapdragon_default)", gate)

    def test_direct_path_uses_exact_existing_range_primitive(self):
        direct = REGION.split("IsRegionModifiedDirect", 1)[1].split(
            "HasGpuDirtyUploadOverlap", 1
        )[0]
        self.assertIn("AnyInRange(start_page, end_page)", direct)
        self.assertNotIn("SetRange", direct)
        self.assertNotIn("UnsetRange", direct)
        self.assertNotIn("UpdateProtection", direct)

    def test_legacy_path_remains_literal_fallback(self):
        legacy = REGION.split("IsRegionModified(u64", 1)[1].split(
            "IsRegionModifiedDirect", 1
        )[0]
        self.assertIn("RegionBits test(bits, start_page, end_page);", legacy)
        self.assertIn("return test.Any();", legacy)
        self.assertIn("if (direct_region_modified_range)", TRACKER)
        self.assertGreaterEqual(TRACKER.count("IsRegionModified<Type::"), 2)

    def test_cpu_and_gpu_keep_the_same_region_lock(self):
        for type_name, next_method in (
            ("CPU", "IsRegionGpuModified"),
            ("GPU", "IsRegionSnapshotStale"),
        ):
            block = TRACKER.split(f"IsRegion{type_name.title()}Modified", 1)[1]
            block = block.split(next_method, 1)[0]
            self.assertIn("std::scoped_lock lk{manager->lock};", block)
            self.assertIn(
                f"IsRegionModifiedDirect<Type::{type_name}>", block
            )

    def test_masked_copy_any_and_direct_range_are_equivalent(self):
        rng = random.Random(0xD83)
        for _ in range(2000):
            words = [rng.getrandbits(64) for _ in range(16)]
            start = rng.randrange(0, 1024)
            end = rng.randrange(start + 1, 1025)
            legacy = any(
                ((words[bit // 64] >> (bit % 64)) & 1) != 0
                for bit in range(start, end)
            )
            first_word = start // 64
            last_word = (end - 1) // 64
            direct = False
            for word_index in range(first_word, last_word + 1):
                low = start % 64 if word_index == first_word else 0
                high = (end - 1) % 64 if word_index == last_word else 63
                mask = ((1 << (high - low + 1)) - 1) << low
                direct |= (words[word_index] & mask) != 0
            self.assertEqual(legacy, direct)


if __name__ == "__main__":
    unittest.main()
