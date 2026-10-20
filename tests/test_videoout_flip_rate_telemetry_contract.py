import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VIDEO_OUT = (
    ROOT / "funnel-arm/src/core/libraries/videoout/video_out.cpp"
).read_text(encoding="utf-8")


class VideoOutFlipRateTelemetryContractTests(unittest.TestCase):
    def test_android_records_guest_cadence_without_changing_it(self):
        body = VIDEO_OUT.split(
            "s32 PS4_SYSV_ABI sceVideoOutSetFlipRate", 1
        )[1].split("s32 PS4_SYSV_ABI sceVideoOutIsFlipPending", 1)[0]
        self.assertIn("[EXECUTOR_VIDEOOUT_FLIP_RATE]", body)
        self.assertIn("Config::vblankFreq()", body)
        self.assertIn("static_cast<double>(rate + 1)", body)
        self.assertEqual(body.count("driver->GetPort(handle)->flip_rate = rate;"), 1)
        self.assertNotIn("sleep", body)


if __name__ == "__main__":
    unittest.main()
