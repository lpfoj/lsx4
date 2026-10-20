import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PIPELINE = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_pipeline_common.cpp"
).read_text(encoding="utf-8")


def between(start: str, end: str) -> str:
    return PIPELINE.split(start, 1)[1].split(end, 1)[0]


class PushConstantTelemetryGateContractTests(unittest.TestCase):
    def test_duplicate_tls_telemetry_reuses_cached_optimization_stats_marker(self):
        gate = between(
            "bool ExecutorVulkanOptimizationStatsEnabled()",
            "#endif\n\n}",
        )
        self.assertIn("static const bool enabled", gate)
        self.assertIn("run-diagnostic-vulkan-optimization-stats", gate)
        self.assertIn("::access", gate)
        self.assertNotIn("getenv", gate)

        gate_definition = PIPELINE.index(
            "bool ExecutorVulkanOptimizationStatsEnabled()"
        )
        android_guard = PIPELINE.rfind("#ifdef __ANDROID__", 0, gate_definition)
        self.assertGreater(android_guard, 0)
        self.assertLess(android_guard, gate_definition)

    def test_only_observational_tls_counters_and_log_are_gated(self):
        bind = between("void Pipeline::BindResources", "std::string Pipeline::GetDebugString")
        telemetry_start = bind.index(
            "if (ExecutorVulkanOptimizationStatsEnabled())"
        )
        telemetry_end = bind.index("#endif", telemetry_start)
        telemetry = bind[telemetry_start:telemetry_end]
        for token in (
            "static thread_local u64 push_attempts",
            "static thread_local u64 push_hits",
            "++push_attempts",
            "push_hits += static_cast<u64>(push_hit)",
            "push_attempts & 8191u",
            "VideoCore::IsExecutorLiveQuiet()",
            "[EXECUTOR_VK_STATE_DEDUP]",
        ):
            self.assertIn(token, telemetry)
        self.assertNotIn("cmdbuf.pushConstants", telemetry)
        self.assertNotIn("scheduler.MarkPushConstants", telemetry)
        self.assertNotIn("push_cache =", telemetry)
        self.assertNotIn("return", telemetry)

    def test_push_constant_emission_and_elision_decision_are_unchanged(self):
        bind = between("void Pipeline::BindResources", "std::string Pipeline::GetDebugString")
        decision = bind.index("const bool push_hit =")
        emission = bind.index("if (!push_hit)")
        push = bind.index("cmdbuf.pushConstants(")
        mark = bind.index("scheduler.MarkPushConstants(bind_point)")
        cache_update = bind.index("push_cache = {")
        consolidated_record = bind.index(
            "VideoCore::RecordVulkanPushConstants(push_hit)"
        )
        telemetry_gate = bind.index(
            "if (ExecutorVulkanOptimizationStatsEnabled())"
        )

        self.assertLess(decision, emission)
        self.assertLess(emission, push)
        self.assertLess(push, mark)
        self.assertLess(mark, cache_update)
        self.assertLess(cache_update, consolidated_record)
        self.assertLess(consolidated_record, telemetry_gate)
        self.assertEqual(bind.count("cmdbuf.pushConstants("), 1)
        self.assertEqual(bind.count("scheduler.MarkPushConstants("), 1)
        self.assertEqual(
            bind.count("VideoCore::RecordVulkanPushConstants(push_hit)"), 1
        )


if __name__ == "__main__":
    unittest.main()
