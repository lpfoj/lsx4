from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
VIDEO = ROOT / "funnel-arm" / "src" / "video_core"
READBACK = (VIDEO / "readback_optimization.cpp").read_text(encoding="utf-8")
READBACK_H = (VIDEO / "readback_optimization.h").read_text(encoding="utf-8")
BUFFER = (VIDEO / "buffer_cache" / "buffer_cache.cpp").read_text(encoding="utf-8")
LIVERPOOL = (VIDEO / "amdgpu" / "liverpool.cpp").read_text(encoding="utf-8")


def between(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


class ReadbackOptimizationStatsGateContractTests(unittest.TestCase):
    def test_gate_is_cached_exact_marker_and_not_environment_driven(self):
        self.assertIn("IsReadbackOptimizationStatsEnabled() noexcept", READBACK_H)
        gate = between(
            READBACK,
            "bool IsReadbackOptimizationStatsEnabled() noexcept",
            "void AccumulateReadbackOptimizationStats",
        )
        self.assertIn("static const bool enabled", gate)
        self.assertIn("run-diagnostic-readback-optimization-stats", gate)
        self.assertIn("::access", gate)
        self.assertNotIn("getenv", gate)

    def test_aggregate_writers_exit_before_touching_atomics(self):
        readback = between(
            READBACK,
            "ReadbackOptimizationStats GetReadbackOptimizationStats",
            "void ResetReadbackOptimizationStats",
        )
        self.assertLess(
            readback.index("if (!IsReadbackOptimizationStatsEnabled())"),
            readback.index(".load"),
        )
        accumulate = between(
            READBACK,
            "void AccumulateReadbackOptimizationStats",
            "void RecordReadbackTimelineRetirement",
        )
        self.assertLess(
            accumulate.index("if (!IsReadbackOptimizationStatsEnabled())"),
            accumulate.index("fetch_add"),
        )
        retirement = between(
            READBACK,
            "void RecordReadbackTimelineRetirement",
            "bool IsReadbackClassificationEnabled",
        )
        self.assertLess(
            retirement.index("!IsReadbackOptimizationStatsEnabled()"),
            retirement.index("exchange"),
        )

    def test_buffer_clocks_and_prefetch_log_are_diagnostic_only(self):
        helper = between(
            BUFFER,
            "ExecutorReadbackDiagnosticStart()",
            "void ExecutorMaybeLogReadbackOptimizationStats",
        )
        self.assertIn("IsReadbackOptimizationStatsEnabled()", helper)
        self.assertIn("IsReadbackClassificationEnabled()", helper)
        self.assertIn("return {};", helper)
        for declaration in (
            "const auto plan_start =",
            "const auto queued_at =",
            "const auto publish_start =",
            "const auto wait_start =",
        ):
            for line in (line for line in BUFFER.splitlines() if declaration in line):
                self.assertIn("ExecutorReadbackDiagnosticStart()", line)
        prefetch_log = between(
            BUFFER,
            "if (IsReadbackOptimizationStatsEnabled()) {\n        static u64 prefetch_batches",
            "// Publish prefetched readbacks",
        )
        self.assertIn("[EXECUTOR_READBACK_PREFETCH]", prefetch_log)

    def test_sidecar_actions_remain_while_only_metrics_are_gated(self):
        record = between(
            LIVERPOOL,
            "void Liverpool::RecordCompletionWriteForExecutor",
            "bool Liverpool::TryResolveCompletionWriteForExecutor",
        )
        self.assertIn("state->completion_writes[address].push_back", record)
        self.assertIn("collect_readback_stats", record)
        self.assertIn("AccumulateReadbackOptimizationStats", record)

        resolve = between(
            LIVERPOOL,
            "bool Liverpool::ResolvePendingCompletionReadForExecutor",
            "void Liverpool::NotifyMemoryPublicationForExecutor",
        )
        self.assertIn("scheduler.WaitRetirement(dependency_tick)", resolve)
        self.assertIn("if (collect_readback_stats)", resolve)
        self.assertIn("executor_pending_read_flushes.fetch_add", resolve)

        publish = between(
            LIVERPOOL,
            "void Liverpool::PublishGpuCompletionBatchForExecutor",
            "void Liverpool::OrderGpuWorkAfterCompletionForExecutor",
        )
        self.assertIn("DeferPriorityOperationAt", publish)
        self.assertIn("for (auto& completion : completions)", publish)
        self.assertIn("completion();", publish)
        self.assertIn("if (collect_readback_stats)", publish)


if __name__ == "__main__":
    unittest.main()
