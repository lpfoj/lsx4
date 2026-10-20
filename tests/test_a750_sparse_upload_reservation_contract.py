import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUFFER_CACHE = ROOT / "funnel-arm" / "src" / "video_core" / "buffer_cache"


class A750SparseUploadReservationContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cache = (BUFFER_CACHE / "buffer_cache.cpp").read_text(encoding="utf-8")
        cls.tracker = (BUFFER_CACHE / "memory_tracker.h").read_text(encoding="utf-8")
        cls.region = (BUFFER_CACHE / "region_manager.h").read_text(encoding="utf-8")

    def test_region_measurement_is_strictly_read_only(self):
        body = self.region.split("void ForEachUploadRangeReadOnly", 1)[1].split(
            "void ForEachUploadRange(", 1
        )[0]
        for required in (
            "RegionBits(snapshot_stale, start_page, end_page)",
            "RegionBits(cpu, start_page, end_page)",
            "upload_mask.SetRange(start_page, end_page)",
            "func(cpu_addr + start * TRACKER_BYTES_PER_PAGE",
        ):
            self.assertIn(required, body)
        for mutation in (
            "UnsetRange",
            "UpdateProtection",
            "MarkSnapshotStale",
            "SetSnapshotOwnershipStamp",
            "ChangeRegionState",
        ):
            self.assertNotIn(mutation, body)

    def test_tracker_measurement_locks_primary_and_alias_managers(self):
        body = self.tracker.split("SnapshotTransactionResult MeasureUploadRanges", 1)[
            1
        ].split("SnapshotTransactionResult ForEachUploadRange(", 1)[0]
        self.assertGreaterEqual(body.count("IteratePages<true>"), 2)
        self.assertIn("snapshot_alias_ranges", body)
        self.assertIn("manager->lock.lock()", body)
        self.assertIn("(*it)->lock.unlock()", body)
        self.assertIn("HasGpuDirtyUploadOverlap", body)
        self.assertIn("alias_source_dirty", body)
        self.assertIn("ForEachUploadRangeReadOnly", body)
        self.assertLess(body.index("manager->lock.lock()"), body.index("gpu_conflict"))
        self.assertLess(body.index("alias_source_dirty"), body.index("ForEachUploadRangeReadOnly"))
        for forbidden in (
            "TryTrackDescriptorSnapshotWrite",
            "UntrackDescriptorSnapshotWrite",
            "MarkSnapshotStale",
            "ChangeRegionState",
            "UpdateProtection",
        ):
            self.assertNotIn(forbidden, body)

    def test_exact_a750_fex_measures_before_the_only_mutating_transaction(self):
        body = self.cache.split(
            "BufferCache::BufferSyncResult BufferCache::SynchronizeBuffer", 1
        )[1].split("vk::Buffer BufferCache::UploadCopies", 1)[0]
        gate = body.split("const bool sparse_fex_upload_reservation", 1)[1].split(
            "const bool ownership_transaction_needed", 1
        )[0]
        self.assertIn("exact_proprietary_adreno750", gate)
        self.assertIn("fex_buffer_capture || fex_snapshot_boundary", gate)
        self.assertIn("!sparse_fex_upload_reservation", gate)
        self.assertIn("reserve_upload_storage(upload_capacity)", gate)

        transaction = body.split(
            "for (u32 attempt = 0; attempt < MaxSnapshotTransactionAttempts", 1
        )[1].split("const auto action = DecideSnapshotTransactionAction", 1)[0]
        self.assertEqual(transaction.count("memory_tracker->ForEachUploadRange("), 1)
        self.assertEqual(transaction.count("memory_tracker->MeasureUploadRanges("), 1)
        self.assertLess(
            transaction.index("memory_tracker->MeasureUploadRanges("),
            transaction.index("memory_tracker->ForEachUploadRange("),
        )
        self.assertLess(
            transaction.index("reserve_upload_storage(\n                                    measured_upload_bytes"),
            transaction.index("memory_tracker->ForEachUploadRange("),
        )
        self.assertNotIn("first sparse FEX pass", transaction)

    def test_plan_growth_retries_without_releasing_the_fex_capture(self):
        body = self.cache.split(
            "BufferCache::BufferSyncResult BufferCache::SynchronizeBuffer", 1
        )[1].split("vk::Buffer BufferCache::UploadCopies", 1)[0]
        retry = body.split(
            "sparse_fex_upload_reservation && sparse_upload_plan_changed", 1
        )[1].split("const auto action = DecideSnapshotTransactionAction", 1)[0]
        self.assertIn("!upload_range_overflow", retry)
        self.assertIn("continue;", retry)
        self.assertNotIn("release_fex_buffer_capture", retry)
        retry_start = body.index(
            "sparse_fex_upload_reservation && sparse_upload_plan_changed"
        )
        self.assertLess(
            retry_start,
            body.index("release_fex_buffer_capture();", retry_start),
        )

        mutating = self.tracker.split("SnapshotTransactionResult ForEachUploadRange(", 1)[
            1
        ].split("template <typename SnapshotFunc>", 1)[0]
        failed = mutating.split("if (!upload_succeeded)", 1)[1].split(
            "if (!finish_descriptor_snapshot())", 1
        )[0]
        self.assertIn("SnapshotTransactionResult::CopyFailed", failed)
        self.assertNotIn("publish_gpu_ownership", failed)


if __name__ == "__main__":
    unittest.main()
