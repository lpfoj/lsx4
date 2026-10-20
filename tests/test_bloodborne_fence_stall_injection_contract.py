import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_presenter.cpp"
).read_text(encoding="utf-8")


class BloodborneFenceStallInjectionContractTests(unittest.TestCase):
    def test_injection_is_marker_only_bounded_and_default_off(self):
        start = SOURCE.index("bool ExecutorInjectPresentFenceTimeout()")
        end = SOURCE.index("enum class PresenterCpuStage", start)
        helper = SOURCE[start:end]
        self.assertIn("run-diagnostic-vk-inject-present-fence-timeouts", helper)
        self.assertIn("static const bool enabled", helper)
        self.assertIn("if (!enabled)", helper)
        self.assertIn("static std::atomic<u32> remaining{8}", helper)
        self.assertIn("usleep(250'000)", helper)
        self.assertIn("proofOnly=1", helper)
        self.assertNotIn("getenv", helper)

    def test_injected_timeout_uses_the_real_nonterminal_timeout_path(self):
        start = SOURCE.index("bool Presenter::WaitForFence")
        end = SOURCE.index("bool Presenter::WaitForFrameReady", start)
        wait = SOURCE[start:end]
        injection = wait.index("ExecutorInjectPresentFenceTimeout()")
        timeout = wait.index("if (result == vk::Result::eTimeout)")
        terminal = wait.index("MarkTerminal(result, where);")
        self.assertLess(injection, timeout)
        self.assertLess(timeout, terminal)
        timeout_body = wait[timeout:terminal]
        self.assertIn("continue;", timeout_body)
        self.assertNotIn("MarkTerminal", timeout_body)

    def test_real_driver_error_is_not_masked_by_injection(self):
        start = SOURCE.index("bool Presenter::WaitForFence")
        end = SOURCE.index("bool Presenter::WaitForFrameReady", start)
        wait = SOURCE[start:end]
        self.assertIn(
            ": instance.GetDevice().waitForFences(fence, false, WaitSlice);", wait
        )
        self.assertIn("MarkTerminal(result, where);\n        return false;", wait)


if __name__ == "__main__":
    unittest.main()
