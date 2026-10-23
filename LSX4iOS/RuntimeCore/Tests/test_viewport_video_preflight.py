"""Reject recording configurations before any physical-device operation."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class ViewportVideoPreflightTests(unittest.TestCase):
    def reject(self, window, extra=()):
        probe = Path(__file__).resolve().parents[1] / "device_probe.py"
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "must-not-exist"
            result = subprocess.run([
                sys.executable, str(probe), "--device", "unused-offline-test",
                "--app", "/nonexistent.app", "--output", str(output),
                "--game", "Games/fixture/eboot.bin", "--game-stage", "run",
                "--seconds", "300", "--video-window", window, *extra,
            ], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertIn("video window" if not extra else "--video-window", result.stderr)
            self.assertFalse(output.exists(), "Recording preflight contacted the device")

    def test_invalid_duration_rate_and_nonfinite_windows(self):
        for window in ("0 181 10", "0 20 16", "0 20 0", "-1 20 10",
                       "290 296 10", "10 10 10", "nan 10 10", "0 inf 10",
                       "0 10 nan", "0 10", "0 10 10 extra"):
            with self.subTest(window=window):
                self.reject(window)

    def test_still_capture_and_video_cannot_compete_for_one_frame_callback(self):
        self.reject("10 80 10", ("--capture-window", "10 20 1"))


if __name__ == "__main__":
    unittest.main()
