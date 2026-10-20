import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PRESENTER = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_presenter.cpp"
).read_text(encoding="utf-8")


class VulkanPresentFenceTimeoutContractTests(unittest.TestCase):
    def test_android_timeout_preserves_frame_ownership_without_terminal_poison(self):
        wait = PRESENTER.split("bool Presenter::WaitForFence", 1)[1].split(
            "bool Presenter::WaitForFrameReady", 1
        )[0]
        android = wait.split("#ifdef __ANDROID__", 1)[1].split("#else", 1)[0]
        timeout = android.split("if (result == vk::Result::eTimeout)", 1)[1].split(
            "MarkTerminal(result, where);", 1
        )[0]

        self.assertIn("for (;;)", android)
        self.assertIn("++timeout_slices", timeout)
        self.assertIn("[EXECUTOR_VK_FENCE_STALL]", timeout)
        self.assertIn("continue;", timeout)
        self.assertNotIn("MarkTerminal", timeout)
        self.assertNotIn("return false", timeout)
        self.assertNotIn("MaxWaits", android)

    def test_real_vulkan_error_remains_terminal(self):
        wait = PRESENTER.split("bool Presenter::WaitForFence", 1)[1].split(
            "bool Presenter::WaitForFrameReady", 1
        )[0]
        android = wait.split("#ifdef __ANDROID__", 1)[1].split("#else", 1)[0]
        timeout_end = android.index("MarkTerminal(result, where);")
        self.assertLess(android.index("if (result == vk::Result::eTimeout)"), timeout_end)
        self.assertIn("MarkTerminal(result, where);\n        return false;", android)


if __name__ == "__main__":
    unittest.main()
