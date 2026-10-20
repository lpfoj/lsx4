import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
PAGE_MANAGER_H = (ROOT / "funnel-arm/src/video_core/page_manager.h").read_text(
    encoding="utf-8"
)
PAGE_MANAGER_CPP = (
    ROOT / "funnel-arm/src/video_core/page_manager.cpp"
).read_text(encoding="utf-8")
RUNTIME = (ROOT / "src/executor/native_runtime_api.cpp").read_text(
    encoding="utf-8"
)


class FusedFastBulkGuestWriteLeaseContractTests(unittest.TestCase):
    def test_candidate_is_additive_with_exact_bloodborne_snapdragon_default(self):
        self.assertIn(
            "run-enable-fused-fast-bulk-guest-write-lease", RUNTIME
        )
        self.assertIn(
            'unsetenv("EXECUTOR_FUSED_FAST_BULK_GUEST_WRITE_LEASE")',
            RUNTIME,
        )
        self.assertIn("strong_snapdragon_cpu_bundle", RUNTIME)
        self.assertIn("fused_fast_bulk_guest_write_lease_requested ||", RUNTIME)
        self.assertIn("if (!fused_fast_bulk_lease)", RUNTIME)
        self.assertIn("PrepareBulkGuestWrite(\n                            guest_address", RUNTIME)
        self.assertIn("EnterBulkGuestWrite(\n                            guest_address", RUNTIME)

    def test_public_transaction_has_exact_fallback_contract(self):
        self.assertIn("TryBeginFastBulkGuestWrite", PAGE_MANAGER_H)
        body = PAGE_MANAGER_CPP.split(
            "bool TryBeginFastBulkGuestWrite(const VAddr address,", 1
        )[1].split("#endif", 1)[0]
        self.assertIn("ExecutorFastBulkGuestWriteEnabled()", body)
        self.assertIn("ExecutorFusedFastBulkGuestWriteLeaseEnabled()", body)
        self.assertIn("FexWriteEpochs().enabled.load", body)
        self.assertIn("Common::RangeLockGuard", body)
        self.assertIn("ActualWriteProtected(page)", body)
        self.assertIn("descriptor_snapshot_write_watcher", body)

    def test_prevalidation_precedes_all_state_publication(self):
        body = PAGE_MANAGER_CPP.split(
            "bool TryBeginFastBulkGuestWrite(const VAddr address,", 1
        )[1].split("#endif", 1)[0]
        protected = body.index("ActualWriteProtected(page)")
        descriptor = body.index("descriptor_snapshot_write_watcher")
        publish = body.index("UpdateBulkWriterCount(page, true)")
        self.assertLess(protected, publish)
        self.assertLess(descriptor, publish)
        self.assertIn("CloseFexDeferredWriteBypass(page)", body)
        self.assertNotIn("Protect(", body)

    def test_lease_teardown_remains_the_existing_path(self):
        lease = RUNTIME.split("struct ExecutorFexGuestMemoryLease final", 1)[1]
        lease = lease.split("int32_t ExecutorFexAcquireGuestMemoryLease", 1)[0]
        self.assertIn("PageManager::LeaveBulkGuestWrite(address, size)", lease)
        self.assertIn("VideoCore::EndBulkGuestWrite()", lease)


if __name__ == "__main__":
    unittest.main()
