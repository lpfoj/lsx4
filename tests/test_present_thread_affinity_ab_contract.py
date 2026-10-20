import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DRIVER = (
    ROOT / "funnel-arm/src/core/libraries/videoout/driver.cpp"
).read_text(encoding="utf-8")


class PresentThreadAffinityAbContractTests(unittest.TestCase):
    def test_affinity_is_an_explicit_file_marker_ab_only(self):
        helper = DRIVER.split(
            "static void ExecutorApplyPresentThreadAffinityAb()", 1
        )[1].split("static bool ExecutorTraceLiveWide()", 1)[0]

        self.assertIn("run-ab-present-thread-graphics-affinity", helper)
        gate = re.search(
            r"if \(::access\(MarkerPath, F_OK\) != 0\) \{\s*return;\s*\}",
            helper,
        )
        self.assertIsNotNone(gate)
        self.assertLess(
            gate.end(), helper.index("Common::ApplyCurrentThreadAffinity(")
        )

    def test_enabled_ab_uses_only_the_existing_graphics_role(self):
        helper = DRIVER.split(
            "static void ExecutorApplyPresentThreadAffinityAb()", 1
        )[1].split("static bool ExecutorTraceLiveWide()", 1)[0]

        self.assertIn("Common::ThreadAffinityClass::GraphicsDriver", helper)
        self.assertIn("[EXECUTOR_PRESENT_THREAD_AFFINITY_AB]", helper)
        self.assertNotIn("LatencyPerformance", helper)
        self.assertNotIn("GuestCompute", helper)

    def test_present_thread_applies_ab_once_before_entering_its_loop(self):
        present_thread = DRIVER.split(
            "void VideoOutDriver::PresentThread(std::stop_token token)", 1
        )[1]
        call = present_thread.index("ExecutorApplyPresentThreadAffinityAb();")
        timer = present_thread.index("Common::AccurateTimer timer{vblank_period};")
        loop = present_thread.index("while (!token.stop_requested())")

        self.assertLess(call, timer)
        self.assertLess(call, loop)
        self.assertEqual(present_thread.count("ExecutorApplyPresentThreadAffinityAb();"), 1)


if __name__ == "__main__":
    unittest.main()
