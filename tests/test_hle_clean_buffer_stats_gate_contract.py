from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT / "funnel-arm/src/video_core/page_manager.cpp"
).read_text(encoding="utf-8")


class HleCleanBufferStatsGateContractTests(unittest.TestCase):
    def test_stats_require_an_exact_cached_marker(self):
        start = SOURCE.index("bool ExecutorHleCleanBufferStatsEnabled()")
        end = SOURCE.index("bool ExecutorGuestInvalidationSourceProfileEnabled()", start)
        gate = SOURCE[start:end]
        self.assertIn("static const bool enabled", gate)
        self.assertIn("run-diagnostic-hle-clean-buffer-stats", gate)
        self.assertNotIn("getenv", gate)

    def test_fast_path_decisions_do_not_depend_on_statistics(self):
        start = SOURCE.index("bool TryPrepareHleCleanBufferWriteForMemset(")
        end = SOURCE.index("void PrepareBulkGuestWrite(", start)
        body = SOURCE[start:end]
        self.assertIn("const bool collect_stats = ExecutorHleCleanBufferStatsEnabled();", body)
        self.assertIn("const u64 ordinal = collect_stats", body)
        self.assertLess(body.index("rasterizer->BeginHleCleanBufferWrite"),
                        body.index("if (collect_stats)"))
        stats = body[body.index("if (collect_stats)") :]
        self.assertIn("counters.active.fetch_add", stats)
        self.assertIn("counters.eligible.fetch_add", stats)
        self.assertIn("counters.avoided_sync.fetch_add", stats)

    def test_reject_and_commit_metrics_are_noops_without_ordinal(self):
        record_start = SOURCE.index("void ExecutorHleCleanBufferRecordFallback(")
        record_end = SOURCE.index("enum class ExecutorBulkInvalidationFirstBlocker", record_start)
        record = SOURCE[record_start:record_end]
        self.assertLess(record.index("if (ordinal == 0)"), record.index("fetch_add"))

        release_start = SOURCE.index("void ReleaseHleCleanBufferWriteTicket(")
        release_end = SOURCE.index("bool TryEnterHleCleanBufferWrite(", release_start)
        release = SOURCE[release_start:release_end]
        self.assertIn("if (committed && ordinal != 0)", release)


if __name__ == "__main__":
    unittest.main()
