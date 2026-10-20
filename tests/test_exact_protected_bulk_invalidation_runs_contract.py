import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "funnel-arm/src/video_core/page_manager.cpp").read_text(
    encoding="utf-8"
)


class ExactProtectedBulkInvalidationRunsContractTests(unittest.TestCase):
    def test_candidate_is_fresh_process_marker_gated(self):
        self.assertIn(
            "run-enable-exact-protected-bulk-invalidation-runs", SOURCE
        )
        self.assertIn(
            "if (ExecutorExactProtectedBulkInvalidationRunsEnabled())", SOURCE
        )

    def test_candidate_keeps_one_fifo_handoff_for_exact_runs(self):
        self.assertIn(
            "InvalidateMemoryBatchFromGuestThread(\n                    protected_runs)",
            SOURCE,
        )
        self.assertIn(
            "for (const auto& [run_begin, run_size] : protected_runs)", SOURCE
        )
        self.assertIn(
            "ReleaseDeferredWriteProtectionRange(run_begin, run_size)", SOURCE
        )

    def test_candidate_does_not_replace_the_baseline_full_span_path(self):
        marker_branch = SOURCE.index(
            "if (ExecutorExactProtectedBulkInvalidationRunsEnabled())"
        )
        baseline_scan = SOURCE.index(
            "size_t first_invalidated_page = first_page", marker_branch
        )
        baseline_invalidation = SOURCE.index(
            "rasterizer->InvalidateMemoryFromGuestThread(", baseline_scan
        )
        self.assertLess(marker_branch, baseline_scan)
        self.assertLess(baseline_scan, baseline_invalidation)

    def test_clean_measurement_does_not_pay_diagnostic_atomics(self):
        candidate = SOURCE[
            SOURCE.index("if (ExecutorExactProtectedBulkInvalidationRunsEnabled())") :
            SOURCE.index("size_t first_invalidated_page = first_page")
        ]
        profile_gate = candidate.index(
            "if (ExecutorGuestInvalidationSourceProfileEnabled())"
        )
        first_counter = candidate.index("exact_run_calls")
        self.assertLess(profile_gate, first_counter)


if __name__ == "__main__":
    unittest.main()
