from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT / "funnel-arm/src/video_core/texture_cache/tile_manager.cpp"
).read_text(encoding="utf-8")


class TileScratchDiagnosticsGateContractTests(unittest.TestCase):
    def test_stats_require_positive_diagnostic_mode_in_production(self):
        start = SOURCE.index("bool ExecutorTileScratchStatsEnabled()")
        end = SOURCE.index("u64 ScratchBucketSize", start)
        gate = SOURCE[start:end]
        self.assertIn("run-diagnostic-tile-scratch-stats", gate)
        self.assertNotIn("IsExecutorLiveQuiet", gate)
        self.assertIn("const bool collect_stats", SOURCE)

    def test_diagnostic_counters_are_gated_but_pool_budget_is_functional(self):
        for counter in (
            "misses.fetch_add",
            "reused.fetch_add",
            "recording_reused.fetch_add",
            "recording_retired.fetch_add",
            "rejected.fetch_add",
            "evicted.fetch_add",
            "created.fetch_add",
        ):
            position = SOURCE.index(counter)
            gate = SOURCE.rfind("if (collect_stats", 0, position)
            self.assertNotEqual(gate, -1, counter)
            self.assertLess(position - gate, 260, counter)

        self.assertIn("pooled_bytes.store(current_bytes + bucket", SOURCE)
        self.assertIn("pooled_bytes.fetch_sub", SOURCE)
        self.assertIn("pooled_buffers.fetch_add", SOURCE)
        self.assertIn("pooled_buffers.fetch_sub", SOURCE)

    def test_detile_dispatch_probe_is_explicitly_gated(self):
        probe = SOURCE.index("[EXECUTOR_DETILE_DISPATCH]")
        gate = SOURCE.rfind("if (ExecutorTileScratchStatsEnabled())", 0, probe)
        self.assertNotEqual(gate, -1)
        self.assertLess(probe - gate, 700)


if __name__ == "__main__":
    unittest.main()
