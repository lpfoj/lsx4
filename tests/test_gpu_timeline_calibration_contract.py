import re
import unittest

from tools import analyze_gpu_timeline as timeline


class GpuTimelineCalibrationContractTests(unittest.TestCase):
    def test_zero_device_ticks_disable_only_cpu_gpu_gap_classification(self):
        source = timeline._synthetic_calibrated_timeline()
        source = re.sub(r"deviceTicks=\d+", "deviceTicks=0", source)

        parsed = timeline.parse_timeline_text(source)
        analysis = timeline.analyze(parsed, trim_epochs=3)
        report = timeline.build_report(analysis, min_frames=6)

        self.assertTrue(report["frames"]["sufficient"])
        self.assertEqual(report["frames"]["strict_complete"], 6)
        self.assertAlmostEqual(
            report["distributions"]["gpu_union_ms"]["p50"], 0.000010
        )
        self.assertAlmostEqual(
            report["distributions"]["gpu_span_ms"]["p50"], 0.000010
        )
        self.assertNotIn("inter_batch_classification", report)
        self.assertEqual(report["calibrations"]["records"], 12)
        self.assertFalse(report["calibrations"]["available"])
        self.assertIn(
            "deviceTicks values are zero",
            report["calibrations"]["unavailable_reason"],
        )
        self.assertTrue(
            any(
                "Relative GPU duration distributions remain valid" in note
                for note in report["notes"]
            )
        )

    def test_nonzero_device_ticks_keep_classification_available(self):
        parsed = timeline.parse_timeline_text(
            timeline._synthetic_calibrated_timeline()
        )
        analysis = timeline.analyze(parsed, trim_epochs=3)
        report = timeline.build_report(analysis, min_frames=6)

        self.assertEqual(
            report["calibrations"], {"records": 12, "available": True}
        )
        self.assertIn("inter_batch_classification", report)


if __name__ == "__main__":
    unittest.main()
