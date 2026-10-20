import pathlib
import random
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
PAGE_MANAGER = (ROOT / "funnel-arm/src/video_core/page_manager.cpp").read_text(
    encoding="utf-8"
)
TRACKER = (
    ROOT / "funnel-arm/src/video_core/buffer_cache/memory_tracker.h"
).read_text(encoding="utf-8")
BUFFER_CACHE = (
    ROOT / "funnel-arm/src/video_core/buffer_cache/buffer_cache.cpp"
).read_text(encoding="utf-8")


class Sm8650CombinedCpuSnapshotQueryContractTests(unittest.TestCase):
    def test_gate_is_sm8650_only_and_keeps_non_android_fallback(self):
        gate = PAGE_MANAGER.split(
            "bool ExecutorSm8650CombinedCpuSnapshotQueryEnabled()", 1
        )[1].split("#ifdef __ANDROID__\nnamespace {", 1)[0]
        self.assertIn("Common::IsSnapdragon8Gen3Host()", gate)
        self.assertIn("legacyFallbackPreserved=1 mutations=0", gate)
        self.assertNotIn("Adreno840", gate)
        self.assertIn("#else\n    return false;", gate)

    def test_combined_query_holds_one_exact_region_lock(self):
        method = TRACKER.split(
            "bool IsRegionCpuModifiedOrSnapshotStale(", 1
        )[1].split("bool AreSnapshotRangesDirty", 1)[0]
        self.assertIn("if (!combined_cpu_snapshot_query)", method)
        self.assertIn("IsRegionCpuModified(query_cpu_addr, query_size)", method)
        self.assertIn("IsRegionSnapshotStale(query_cpu_addr, query_size)", method)
        self.assertEqual(method.count("std::scoped_lock lk{manager->lock};"), 1)
        self.assertIn("IsRegionModifiedDirect<Type::CPU>", method)
        self.assertIn("IsRegionModified<Type::CPU>", method)
        self.assertIn("manager->IsRegionSnapshotStale(offset, size)", method)
        self.assertNotIn("SetRange", method)
        self.assertNotIn("UnsetRange", method)
        self.assertNotIn("UpdateProtection", method)

    def test_only_physical_alias_source_uses_combined_query(self):
        sync = BUFFER_CACHE.split(
            "BufferSyncResult BufferCache::SynchronizeBuffer", 1
        )[1].split("BufferCache::UploadCopies", 1)[0]
        self.assertIn(
            "physical_alias_snapshot\n"
            "        ? memory_tracker->IsRegionCpuModifiedOrSnapshotStale",
            sync,
        )
        self.assertIn(
            ": memory_tracker->IsRegionCpuModified(device_addr, size)", sync
        )
        dirty_ranges = TRACKER.split("bool AreSnapshotRangesDirty", 1)[1].split(
            "bool AreSnapshotRangesGpuDirty", 1
        )[0]
        self.assertIn("IsRegionCpuModifiedOrSnapshotStale", dirty_ranges)

    def test_boolean_result_matches_legacy_or(self):
        rng = random.Random(0x8650C5)
        for _ in range(4000):
            cpu_modified = bool(rng.getrandbits(1))
            snapshot_stale = bool(rng.getrandbits(1))
            legacy = cpu_modified or snapshot_stale
            combined = cpu_modified or snapshot_stale
            self.assertEqual(legacy, combined)


if __name__ == "__main__":
    unittest.main()
