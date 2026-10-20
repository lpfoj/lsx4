from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_presenter.cpp"
).read_text(encoding="utf-8")


class PresenterLiveStatsGateContractTests(unittest.TestCase):
    def test_quiet_present_keeps_runtime_inputs_but_skips_diagnostics(self):
        start = SOURCE.index("static void ExecutorLiveDirectPresentStats(")
        end = SOURCE.index("bool CanBlitToSwapchain(", start)
        body = SOURCE[start:end]

        counter = body.index("g_executor_live_present_count.fetch_add")
        adpf = body.index("AndroidPerformanceHint::MaintainSessionThreads()")
        gate = body.index("if (VideoCore::IsExecutorLiveQuiet())")
        clock = body.index("Clock::now()")
        mutex = body.index("std::scoped_lock lock{stats_mutex}")

        self.assertLess(counter, gate)
        self.assertLess(adpf, gate)
        self.assertLess(gate, clock)
        self.assertLess(gate, mutex)
        self.assertIn("[EXECUTOR_LIVE_FPS]", body[gate:])


if __name__ == "__main__":
    unittest.main()
