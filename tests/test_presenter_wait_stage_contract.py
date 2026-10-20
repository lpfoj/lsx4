import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PRESENTER = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_presenter.cpp"
).read_text(encoding="utf-8")


class PresenterWaitStageContractTests(unittest.TestCase):
    def test_composited_present_waits_at_first_real_consumers(self):
        anchor = PRESENTER.index('WaitForFrameReady(frame, "present_frame_ready")')
        body = PRESENTER[anchor - 700 : anchor + 700]
        self.assertIn("swapchain.GetImageAcquiredSemaphore(), 1,", body)
        self.assertIn("vk::PipelineStageFlagBits::eColorAttachmentOutput", body)
        self.assertIn("frame->ready_semaphore, frame->ready_tick,", body)
        self.assertIn("vk::PipelineStageFlagBits::eFragmentShader", body)


if __name__ == "__main__":
    unittest.main()
