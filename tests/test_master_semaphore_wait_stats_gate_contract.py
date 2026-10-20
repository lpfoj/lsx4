from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_master_semaphore.cpp"
).read_text(encoding="utf-8")


class MasterSemaphoreWaitStatsGateContractTests(unittest.TestCase):
    def test_wait_measurement_is_explicit_opt_in(self):
        gate_start = SOURCE.index("bool ExecutorTimelineWaitStatsEnabled()")
        gate_end = SOURCE.index("#endif", gate_start)
        gate = SOURCE[gate_start:gate_end]
        self.assertIn("static const bool enabled", gate)
        self.assertIn("run-diagnostic-vk-timeline-wait-stats", gate)

        start = SOURCE.index("void MasterSemaphore::Wait(u64 tick)")
        body = SOURCE[start:]
        wait_call = body.index("waitSemaphores(&wait_info, ANDROID_WAIT_SLICE)")
        stats = body.index("if (executor_wait_stats_enabled)", wait_call)
        self.assertLess(wait_call, stats)
        measured = body[stats:]
        self.assertIn("steady_clock::now() - executor_wait_begin", measured)
        self.assertIn("executor_wait_total_ns.fetch_add", measured)
        self.assertIn("[EXECUTOR_VK_TIMELINE_WAIT]", measured)
        self.assertLess(
            stats,
            body.index("if (wait_result != vk::Result::eSuccess)", stats),
        )

    def test_timeout_and_terminal_handling_remain_unconditional(self):
        start = SOURCE.index("void MasterSemaphore::Wait(u64 tick)")
        body = SOURCE[start:]
        stats = body.index("if (executor_wait_stats_enabled)",
                           body.index("waitSemaphores(&wait_info, ANDROID_WAIT_SLICE)"))
        self.assertLess(body.index("[EXECUTOR_VK_TIMELINE_STALL]"), stats)
        self.assertGreater(body.index("MarkTerminal(wait_result, \"timeline_wait\")"), stats)


if __name__ == "__main__":
    unittest.main()
