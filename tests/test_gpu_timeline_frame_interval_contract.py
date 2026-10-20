from dataclasses import replace
import unittest

from tools import analyze_gpu_timeline as timeline


class GpuTimelineFrameIntervalContractTests(unittest.TestCase):
    @staticmethod
    def _timeline_with_long_frame(*, exclude_long_epoch: bool = False):
        parsed = timeline.parse_timeline_text(
            timeline._synthetic_calibrated_timeline()
        )
        long_epoch = 206
        parsed.frames = [
            replace(
                frame,
                cpu_ns=frame.cpu_ns + (64_000_000 if frame.epoch >= long_epoch else 0),
            )
            for frame in parsed.frames
        ]
        if exclude_long_epoch:
            parsed.samples = [
                replace(sample, cpu_submit_end_ns=sample.cpu_submit_end_ns + 1)
                if sample.epoch == long_epoch
                else sample
                for sample in parsed.samples
            ]
        return parsed

    def test_reports_p99_and_strict_long_frame_breakdown(self):
        analysis = timeline.analyze(
            self._timeline_with_long_frame(), trim_epochs=3
        )
        report = timeline.build_report(
            analysis, min_frames=6, long_frame_ms=60.0
        )

        frame_distribution = report["distributions"]["frame_interval_ms"]
        self.assertEqual(frame_distribution["n"], 6)
        self.assertAlmostEqual(frame_distribution["p50"], 16.0)
        self.assertAlmostEqual(frame_distribution["p95"], 64.0)
        self.assertAlmostEqual(frame_distribution["p99"], 76.8)
        self.assertAlmostEqual(frame_distribution["max"], 80.0)
        self.assertIn("p99", report["distributions"]["gpu_union_ms"])

        long_frames = report["long_frames"]
        self.assertEqual(long_frames["count"], 1)
        self.assertEqual(long_frames["strict_gpu_complete"], 1)
        self.assertEqual(long_frames["without_strict_gpu_breakdown"], 0)
        record = long_frames["records"][0]
        self.assertEqual(record["epoch"], 206)
        self.assertAlmostEqual(record["frame_interval_ms"], 80.0)
        self.assertTrue(record["strict_gpu_complete"])
        self.assertEqual(record["excluded_reasons"], [])
        self.assertIsNotNone(record["cpu_record_sum_ms"])
        self.assertIsNotNone(record["cpu_submit_sum_ms"])
        self.assertIsNotNone(record["gpu_union_ms"])
        self.assertIsNotNone(record["gpu_span_ms"])

    def test_long_frame_survives_missing_strict_gpu_breakdown(self):
        analysis = timeline.analyze(
            self._timeline_with_long_frame(exclude_long_epoch=True),
            trim_epochs=3,
        )
        report = timeline.build_report(
            analysis, min_frames=5, long_frame_ms=60.0
        )

        self.assertEqual(report["frames"]["frame_intervals"], 6)
        self.assertEqual(report["frames"]["strict_complete"], 5)
        self.assertEqual(report["distributions"]["frame_interval_ms"]["n"], 6)
        long_frames = report["long_frames"]
        self.assertEqual(long_frames["count"], 1)
        self.assertEqual(long_frames["strict_gpu_complete"], 0)
        self.assertEqual(long_frames["without_strict_gpu_breakdown"], 1)
        record = long_frames["records"][0]
        self.assertFalse(record["strict_gpu_complete"])
        self.assertIn(
            "sample/Q CPU submit timestamps mismatch",
            record["excluded_reasons"],
        )
        self.assertIsNone(record["cpu_record_sum_ms"])
        self.assertIsNone(record["cpu_submit_sum_ms"])
        self.assertIsNone(record["gpu_union_ms"])
        self.assertIsNone(record["gpu_span_ms"])


if __name__ == "__main__":
    unittest.main()
