from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
NATIVE = (ROOT / "src/executor/native_runtime_api.cpp").read_text(
    encoding="utf-8"
)
PAGE_MANAGER = (
    ROOT / "funnel-arm/src/video_core/page_manager.cpp"
).read_text(encoding="utf-8")


class UnwatchedBulkGuestWriteFastPathContractTests(unittest.TestCase):
    def test_runtime_marker_is_default_off_and_sets_only_the_general_gate(self):
        self.assertIn(
            "run-enable-unwatched-bulk-guest-write-fastpath", NATIVE
        )
        self.assertIn(
            'setenv("EXECUTOR_ENABLE_UNWATCHED_BULK_GUEST_WRITE_FAST_PATH", "1", 1)',
            NATIVE,
        )
        self.assertIn(
            'unsetenv("EXECUTOR_ENABLE_UNWATCHED_BULK_GUEST_WRITE_FAST_PATH")',
            NATIVE,
        )
        self.assertIn("scope=steady-state-unwatched default=off", NATIVE)
        marker_scope = NATIVE.split(
            "const auto unwatched_bulk_guest_write_fastpath_sentinel", 1
        )[1].split("const auto suballocate_bda_buffers_sentinel", 1)[0]
        for title_token in ("CUSA", "Bloodborne", "TitleId", "title_id"):
            self.assertNotIn(title_token, marker_scope)

    def test_steady_state_admission_remains_fail_closed(self):
        admission = PAGE_MANAGER.split(
            "bool TryEnterUnwatchedBulkGuestWrite", 1
        )[1].split("bool LeaveUnwatchedBulkGuestWrite", 1)[0]
        for token in (
            "initial_gpu_mapping_registration_complete.load(",
            "ExecutorFexBufferCaptureAckEnabled()",
            "!ExecutorUnwatchedBulkGuestWriteFastPathEnabled()",
            "executor_unwatched_bulk_write_active",
            "executor_tracked_bulk_write_gate_active",
            "unwatched_bulk_write_gate.load(std::memory_order_acquire)",
            "active_unwatched_bulk_writers.fetch_add",
            "completed_during_admission",
            "tracked_bulk_writers.load(std::memory_order_acquire)",
            "WriteWatcherHazard(page)",
            "ActualWriteProtected(page)",
            "WriteFaultBypassed(page)",
            "FexDeferredWriteBypassed(page)",
        ):
            self.assertIn(token, admission)
        self.assertGreaterEqual(
            admission.count(
                "unwatched_bulk_write_gate.load(std::memory_order_acquire)"
            ),
            3,
        )
        self.assertGreaterEqual(
            admission.count(
                "tracked_bulk_writers.load(std::memory_order_acquire)"
            ),
            2,
        )

    def test_mapping_registration_and_capture_take_an_exclusive_cutoff(self):
        cutoff = PAGE_MANAGER.split("struct UnwatchedBulkWriteCutoff", 1)[1].split(
            "void CompleteInitialGpuMappingRegistration", 1
        )[0]
        self.assertIn("unwatched_bulk_write_gate_mutex", cutoff)
        self.assertIn("unwatched_bulk_write_gate.fetch_add", cutoff)
        self.assertIn("active_unwatched_bulk_writers.load", cutoff)
        self.assertIn("tracked_bulk_writers.load", cutoff)

        registration = PAGE_MANAGER.split(
            "void CompleteInitialGpuMappingRegistration", 1
        )[1].split("bool InitialGpuMappingRegistrationComplete", 1)[0]
        self.assertLess(
            registration.index("UnwatchedBulkWriteCutoff cutoff"),
            registration.index("initial_gpu_mapping_registration_complete.store"),
        )

        capture = PAGE_MANAGER.split(
            "bool PageManager::MayHaveFexDeferredBufferCapture", 1
        )[1].split(
            "PageManager::CurrentFexDeferredBufferCaptureDirtyPages", 1
        )[0]
        self.assertIn(
            "owner->active_unwatched_bulk_writers.load(std::memory_order_acquire)",
            capture,
        )

    def test_leave_publishes_before_releasing_global_writer_count(self):
        leave = PAGE_MANAGER.split(
            "bool LeaveUnwatchedBulkGuestWrite", 1
        )[1].split("template <typename Callback>", 1)[0]
        self.assertIn(
            "active_unwatched_bulk_writers.fetch_sub", leave
        )
        self.assertIn("std::memory_order_release", leave)


if __name__ == "__main__":
    unittest.main()
