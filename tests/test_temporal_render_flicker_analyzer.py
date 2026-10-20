import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

import cv2
import numpy as np


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "analyze_temporal_render_flicker.py"
SPEC = importlib.util.spec_from_file_location("temporal_render_flicker", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
ANALYZER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = ANALYZER
SPEC.loader.exec_module(ANALYZER)


class TemporalRenderFlickerAnalyzerTests(unittest.TestCase):
    def test_large_connected_corruption_is_distinguished_from_small_motion(self):
        reference = np.full((180, 320, 3), (72, 91, 116), dtype=np.uint8)

        particle = reference.copy()
        particle[30:34, 90:94] = (255, 255, 255)
        particle_metrics = ANALYZER.measure_frame(particle, reference, threshold=42)

        broken_surface = reference.copy()
        broken_surface[55:150, 35:285] = (18, 18, 18)
        broken_metrics = ANALYZER.measure_frame(
            broken_surface, reference, threshold=42
        )

        self.assertLess(particle_metrics[1], 0.001)
        self.assertGreater(broken_metrics[1], 0.40)
        self.assertGreater(broken_metrics[3], 0.40)

    def test_event_grouping_preserves_separate_one_frame_flashes(self):
        metric = ANALYZER.FrameMetric
        values = [
            metric(7, 0.7, 0.1, 0.08, 0.0, 0.08, 0.0),
            metric(8, 0.8, 0.1, 0.09, 0.0, 0.09, 0.0),
            metric(10, 1.0, 0.1, 0.07, 0.0, 0.07, 0.0),
        ]
        events = ANALYZER.group_events(values, minimum_component=0.05)
        self.assertEqual([[7, 8], [10]], [[m.frame for m in event] for event in events])

    def test_ordered_png_series_uses_all_frames_and_declared_rate(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            for index in range(5):
                frame = np.full((90, 160, 3), (60, 80, 100), dtype=np.uint8)
                if index == 2:
                    frame[25:75, 30:130] = (240, 20, 240)
                self.assertTrue(cv2.imwrite(str(source / f"frame-{index:03d}.png"), frame))
            metadata, _, metrics = ANALYZER.analyze_series(
                source,
                analysis_width=160,
                roi=(0.0, 0.0, 1.0, 1.0),
                threshold=42,
                max_reference_samples=5,
                series_fps=2.0,
            )
            self.assertEqual(5, metadata["decoded_frames"])
            self.assertEqual(2.0, metadata["fps"])
            self.assertEqual(1.0, metrics[2].seconds)
            self.assertGreater(metrics[2].largest_component_fraction, 0.30)


if __name__ == "__main__":
    unittest.main()
