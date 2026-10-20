import importlib.util
import struct
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PARSER_PATH = ROOT / "tools/parse_invalidation_overlap_trace.py"
SPEC = importlib.util.spec_from_file_location("overlap_parser", PARSER_PATH)
PARSER = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(PARSER)


def _snapshot(
    records,
    *,
    saturated=0,
    dropped=0,
    coverage_exact=1,
    enabled=0,
    feature_flags=None,
):
    capacity = 64
    committed = sum(record[-1] != 0 for record in records)
    if feature_flags is None:
        feature_flags = PARSER.FEATURE_WAIT | PARSER.FEATURE_DRAIN
    header = PARSER.HEADER.pack(
        PARSER.MAGIC,
        1,
        PARSER.HEADER.size,
        PARSER.RECORD.size,
        PARSER.CLOCK_STEADY_NANOSECONDS,
        capacity,
        len(records),
        committed,
        dropped,
        enabled,
        saturated,
        coverage_exact,
        0,
        feature_flags,
        0,
    )
    return header + b"".join(PARSER.RECORD.pack(*record) for record in records)


class InvalidationOverlapTraceParserTests(unittest.TestCase):
    def test_union_concurrency_and_wait_drain_overlap(self):
        wait_ok = PARSER.KIND_WAIT | PARSER.FLAG_RESULT
        data = _snapshot(
            [
                (0, 10, 0x1000, 4096, wait_ok),
                (5, 15, 0x2000, 8192, wait_ok),
                (8, 12, 0, 0, PARSER.KIND_DRAIN | PARSER.FLAG_RESULT),
                (20, 25, 0, 0, PARSER.KIND_DRAIN | PARSER.FLAG_RESULT),
            ]
        )
        result = PARSER.parse_trace_bytes(data)
        self.assertTrue(result["coverage_exact"])
        self.assertEqual(result["committed_entries"], 4)
        self.assertEqual(result["empty_reserved_slots"], 0)
        self.assertEqual(result["wait"]["summed_ns"], 20)
        self.assertEqual(result["wait"]["union_ns"], 15)
        self.assertEqual(result["wait"]["maximum_concurrency"], 2)
        self.assertEqual(result["drain"]["union_ns"], 9)
        self.assertEqual(result["wait_drain_overlap_ns"], 4)
        self.assertEqual(result["wait_union_outside_drain_ns"], 11)
        self.assertEqual(result["drain_union_without_wait_ns"], 5)
        self.assertEqual(result["combined_union_ns"], 20)

    def test_address_locality_reports_both_completion_and_begin_orders(self):
        wait_ok = PARSER.KIND_WAIT | PARSER.FLAG_RESULT
        data = _snapshot(
            [

                (10, 20, 0x1000, 0x1000, wait_ok),
                (30, 40, 0x5000, 0x1000, wait_ok),
                (20, 50, 0x2000, 0x1000, wait_ok),
                (35, 45, 0, 0, PARSER.KIND_DRAIN | PARSER.FLAG_RESULT),
            ]
        )
        locality = PARSER.parse_trace_bytes(data)["address_locality"]
        self.assertTrue(locality["analysis_exact"])
        self.assertFalse(locality["safe_coalescing_proven"])
        self.assertEqual(locality["analyzable_successful_waits"], 3)
        self.assertEqual(locality["requested_bytes"], 3 * 0x1000)
        self.assertEqual(locality["address_union_bytes"], 3 * 0x1000)
        self.assertEqual(locality["global_spatial_components"][0]["components"], 2)
        slot_order = locality["orders"]["slot_completion"]
        begin_order = locality["orders"]["wait_begin"]
        self.assertEqual(slot_order["within_gap_pairs"]["0"]["count"], 0)
        self.assertEqual(begin_order["within_gap_pairs"]["0"]["count"], 1)
        projection = next(
            item
            for item in begin_order["greedy_grouping_projection"]
            if item["gap_bytes"] == 0 and item["max_span_pages"] == 4
        )
        self.assertEqual(projection["projected_groups"], 2)
        self.assertEqual(projection["projected_request_reduction"], 1)

    def test_address_locality_fails_closed_for_unusable_ranges(self):
        wait_ok = PARSER.KIND_WAIT | PARSER.FLAG_RESULT
        data = _snapshot(
            [
                (1, 2, 0x1000, 0, wait_ok),
                (2, 3, PARSER.UINT64_MAX, 2, wait_ok),
                (
                    3,
                    4,
                    0x2000,
                    0xFFFFFFFF,
                    wait_ok | PARSER.FLAG_SIZE_TRUNCATED,
                ),
                (4, 5, 0x3000, 0x1000, PARSER.KIND_WAIT),
                (5, 6, 0, 0, PARSER.KIND_DRAIN | PARSER.FLAG_RESULT),
            ]
        )
        locality = PARSER.parse_trace_bytes(data)["address_locality"]
        self.assertFalse(locality["analysis_exact"])
        self.assertEqual(locality["analyzable_successful_waits"], 0)
        self.assertEqual(
            locality["excluded"],
            {
                "failed_waits": 1,
                "truncated_sizes": 1,
                "zero_sizes": 1,
                "uint64_end_overflows": 1,
            },
        )

    def test_saturation_fails_closed_and_mapping_stays_pending(self):
        data = _snapshot(
            [(1, 2, 0, 1, PARSER.KIND_WAIT | PARSER.FLAG_RESULT)],
            saturated=1,
            dropped=7,
            coverage_exact=0,
        )
        result = PARSER.parse_trace_bytes(data)
        self.assertFalse(result["coverage_exact"])
        self.assertFalse(result["epoch_mapping"]["available"])
        self.assertFalse(result["causal_frame_attribution_available"])
        self.assertIn("pending", result["epoch_mapping"]["status"])

    def test_rejects_truncated_snapshot(self):
        with self.assertRaises(PARSER.TraceFormatError):
            PARSER.parse_trace_bytes(b"LSX4")

    def test_rejects_header_committing_more_than_capacity(self):
        header = PARSER.HEADER.pack(
            PARSER.MAGIC,
            1,
            PARSER.HEADER.size,
            PARSER.RECORD.size,
            PARSER.CLOCK_STEADY_NANOSECONDS,
            1,
            2,
            0,
            0,
            1,
            0,
            1,
            0,
            PARSER.FEATURE_WAIT | PARSER.FEATURE_DRAIN,
            0,
        )
        with self.assertRaises(PARSER.TraceFormatError):
            PARSER.parse_trace_bytes(header + bytes(PARSER.RECORD.size * 2))

    def test_unknown_record_flag_fails_closed(self):
        data = _snapshot(
            [
                (
                    1,
                    2,
                    0,
                    1,
                    PARSER.KIND_WAIT | PARSER.FLAG_RESULT | (1 << 31),
                )
            ]
        )
        result = PARSER.parse_trace_bytes(data)
        self.assertFalse(result["coverage_exact"])
        self.assertEqual(result["unknown_flag_records"], 1)

    def test_uncommitted_reserved_slot_fails_closed(self):
        data = _snapshot(
            [
                (1, 2, 0, 1, PARSER.KIND_WAIT | PARSER.FLAG_RESULT),
                (0, 0, 0, 0, 0),
            ]
        )
        result = PARSER.parse_trace_bytes(data)
        self.assertFalse(result["coverage_exact"])
        self.assertIn("uncommitted", " ".join(result["warnings"]))

    def test_active_snapshot_cannot_be_exact(self):
        data = _snapshot(
            [(1, 2, 0, 1, PARSER.KIND_WAIT | PARSER.FLAG_RESULT)],
            enabled=1,
        )
        result = PARSER.parse_trace_bytes(data)
        self.assertFalse(result["coverage_exact"])
        self.assertIn("active", " ".join(result["warnings"]))

    def test_rejects_non_boolean_header(self):
        data = _snapshot([], enabled=2)
        with self.assertRaises(PARSER.TraceFormatError):
            PARSER.parse_trace_bytes(data)

    def test_rejects_missing_or_unknown_feature_bits(self):
        for feature_flags in (
            PARSER.FEATURE_WAIT,
            PARSER.FEATURE_WAIT | PARSER.FEATURE_DRAIN | (1 << 63),
        ):
            with self.subTest(feature_flags=feature_flags):
                data = _snapshot([], feature_flags=feature_flags)
                with self.assertRaises(PARSER.TraceFormatError):
                    PARSER.parse_trace_bytes(data)


if __name__ == "__main__":
    unittest.main()
