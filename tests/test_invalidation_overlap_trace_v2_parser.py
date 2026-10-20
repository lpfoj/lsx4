import importlib.util
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PARSER_PATH = ROOT / "tools/parse_invalidation_overlap_trace_v2.py"
SPEC = importlib.util.spec_from_file_location("overlap_v2_parser", PARSER_PATH)
PARSER = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(PARSER)


def _record(begin, end, primary, related, argument, size, kind, *, result=True, flags=0):
    commit = kind | flags | (PARSER.FLAG_RESULT if result else 0)
    return (begin, end, primary, related, argument, size, commit)


def _valid_records():
    return [


        _record(90, 90, 30, 6, 60, 1, PARSER.KIND_FRAME_BOUNDARY),
        _record(100, 250, 11, 7, 0x1000, 4096, PARSER.KIND_REQUEST_WAIT),
        _record(150, 150, 11, 21, 7, 0, PARSER.KIND_REQUEST_BATCH),
        _record(150, 200, 21, 7, 7, 1, PARSER.KIND_DRAIN_BATCH),
        _record(300, 300, 31, 7, 70, 1, PARSER.KIND_FRAME_BOUNDARY),
        _record(310, 320, 41, 7, 70, 0, PARSER.KIND_PRESENT),
    ]


def _snapshot(
    records,
    *,
    enabled=0,
    saturated=0,
    dropped=0,
    coverage_exact=1,
    mapping_exact=1,
    active_spans=0,
    left_censored=0,
    right_censored=0,
    mixed_batches=0,
    crossing_batches=0,
):
    capacity = 64
    committed = sum(record[-1] != 0 for record in records)
    header = PARSER.HEADER.pack(
        PARSER.MAGIC,
        PARSER.ABI_VERSION,
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
        mapping_exact,
        PARSER.REQUIRED_FEATURES,
        50,
        50,
        400,
        active_spans,
        left_censored,
        right_censored,
        mixed_batches,
        crossing_batches,
        0,
    )
    return header + b"".join(PARSER.RECORD.pack(*record) for record in records)


class InvalidationOverlapTraceV2ParserTests(unittest.TestCase):
    def test_full_structural_epoch_frame_chain_is_accepted(self):
        result = PARSER.parse_trace_bytes(_snapshot(_valid_records()))
        self.assertTrue(result["coverage_exact"])
        self.assertTrue(result["structural_epoch_comembership_available"])
        self.assertNotIn("causal_frame_attribution_available", result)
        self.assertFalse(result["delay_causality_proven"])
        self.assertTrue(result["instrumentation_perturbed"])
        self.assertFalse(result["production_frametime_usable"])
        self.assertEqual(result["verdict"], "structural-epoch-comembership")
        self.assertEqual(result["first_captured_closed_epoch"], 6)
        self.assertEqual(result["eligible_epochs"], [7])
        epoch = result["structural_epochs"]["7"]
        self.assertEqual(epoch["request_ids"], [11])
        self.assertEqual(epoch["executed_request_ids"], [11])
        self.assertEqual(epoch["batch_ids"], [21])
        self.assertEqual(epoch["frame_token"], 70)
        self.assertEqual(epoch["qualifying_successful_present_ids"], [41])

    def test_saturation_and_uncommitted_slot_are_non_verdict(self):
        records = _valid_records() + [(0, 0, 0, 0, 0, 0, 0)]
        result = PARSER.parse_trace_bytes(
            _snapshot(records, saturated=1, dropped=1, coverage_exact=0)
        )
        self.assertFalse(result["coverage_exact"])
        self.assertEqual(result["uncommitted_slots"], 1)
        self.assertFalse(result["structural_epoch_comembership_available"])
        self.assertEqual(result["verdict"], "non-verdict")

    def test_missing_request_batch_edge_is_non_verdict(self):
        records = [
            record
            for record in _valid_records()
            if (record[-1] & PARSER.KIND_MASK) != PARSER.KIND_REQUEST_BATCH
        ]
        result = PARSER.parse_trace_bytes(_snapshot(records))
        self.assertFalse(result["structural_epoch_comembership_available"])
        self.assertIn("request 11 has no drain-batch edge", result["mapping_errors"])

    def test_missing_successful_present_is_non_verdict(self):
        records = _valid_records()
        records[-1] = _record(
            310, 320, 41, 7, 70, 0, PARSER.KIND_PRESENT, result=False
        )
        result = PARSER.parse_trace_bytes(_snapshot(records))
        self.assertFalse(result["structural_epoch_comembership_available"])
        self.assertIn(
            "epoch 7 has no successful Frame-owned present after its boundary",
            result["mapping_errors"],
        )

    def test_epoch_crossing_batch_is_non_verdict(self):
        records = _valid_records()
        records[3] = _record(
            150,
            200,
            21,
            7,
            8,
            1,
            PARSER.KIND_DRAIN_BATCH,
            flags=PARSER.FLAG_EPOCH_CROSSED,
        )
        result = PARSER.parse_trace_bytes(
            _snapshot(records, mapping_exact=0, crossing_batches=1)
        )
        self.assertFalse(result["structural_epoch_mapping_exact"])
        self.assertFalse(result["structural_epoch_comembership_available"])

    def test_left_censored_or_mixed_generation_is_non_verdict(self):
        for keyword in (
            {"left_censored": 2, "mapping_exact": 0},
            {"right_censored": 2, "mapping_exact": 0},
            {"mixed_batches": 1, "mapping_exact": 0},
        ):
            with self.subTest(keyword=keyword):
                result = PARSER.parse_trace_bytes(_snapshot(_valid_records(), **keyword))
                self.assertFalse(result["structural_epoch_comembership_available"])
                self.assertEqual(result["verdict"], "non-verdict")

    def test_batch_count_or_temporal_guess_cannot_masquerade_as_mapping(self):
        records = _valid_records()
        records[3] = _record(
            150, 200, 21, 7, 7, 2, PARSER.KIND_DRAIN_BATCH
        )
        result = PARSER.parse_trace_bytes(_snapshot(records))
        self.assertFalse(result["structural_epoch_comembership_available"])
        self.assertIn("batch 21 request count 2 != mapped 1", result["mapping_errors"])

    def test_live_or_unquiesced_snapshot_is_non_verdict(self):
        result = PARSER.parse_trace_bytes(
            _snapshot(_valid_records(), enabled=1, active_spans=1)
        )
        self.assertFalse(result["coverage_exact"])
        self.assertFalse(result["structural_epoch_comembership_available"])

    def test_first_captured_closed_epoch_is_quarantined(self):
        records = _valid_records()[1:]
        result = PARSER.parse_trace_bytes(_snapshot(records))
        self.assertFalse(result["structural_epoch_comembership_available"])
        self.assertEqual(
            result["quarantined_epochs"]["7"],
            ["first-captured-closed-epoch"],
        )

    def test_present_before_request_is_non_verdict(self):
        records = _valid_records()
        records[-1] = _record(80, 85, 41, 7, 70, 0, PARSER.KIND_PRESENT)
        result = PARSER.parse_trace_bytes(_snapshot(records))
        self.assertFalse(result["structural_epoch_comembership_available"])
        self.assertIn(
            "epoch 7 has no successful Frame-owned present after its boundary",
            result["mapping_errors"],
        )

    def test_drain_after_boundary_is_non_verdict(self):
        records = _valid_records()
        records[1] = _record(
            100, 330, 11, 7, 0x1000, 4096, PARSER.KIND_REQUEST_WAIT
        )
        records[2] = _record(310, 310, 11, 21, 7, 0, PARSER.KIND_REQUEST_BATCH)
        records[3] = _record(310, 320, 21, 7, 7, 1, PARSER.KIND_DRAIN_BATCH)
        result = PARSER.parse_trace_bytes(_snapshot(records))
        self.assertFalse(result["structural_epoch_comembership_available"])
        self.assertTrue(
            any("violates request<=drain" in error for error in result["mapping_errors"])
        )

    def test_post_stop_present_is_non_verdict(self):
        records = _valid_records()
        records[-1] = _record(410, 420, 41, 7, 70, 0, PARSER.KIND_PRESENT)
        result = PARSER.parse_trace_bytes(_snapshot(records))
        self.assertFalse(result["coverage_exact"])
        self.assertFalse(result["structural_epoch_comembership_available"])
        self.assertTrue(
            any("after capture stop" in error for error in result["invalid_records"])
        )

    def test_enqueue_epoch_must_exactly_equal_drain_epoch(self):
        records = _valid_records()
        records[1] = _record(
            100, 250, 11, 8, 0x1000, 4096, PARSER.KIND_REQUEST_WAIT
        )
        result = PARSER.parse_trace_bytes(_snapshot(records))
        self.assertFalse(result["structural_epoch_comembership_available"])
        self.assertTrue(
            any("enqueue epoch" in error for error in result["mapping_errors"])
        )

    def test_multiple_boundaries_for_epoch_are_ambiguous(self):
        records = _valid_records()
        records.insert(
            -1,
            _record(301, 301, 32, 7, 71, 1, PARSER.KIND_FRAME_BOUNDARY),
        )
        result = PARSER.parse_trace_bytes(_snapshot(records))
        self.assertFalse(result["structural_epoch_comembership_available"])
        self.assertTrue(
            any("concurrent-flip" in error for error in result["mapping_errors"])
        )

    def test_failed_request_cannot_establish_executed_membership(self):
        records = _valid_records()
        records[1] = _record(
            100,
            250,
            11,
            7,
            0x1000,
            4096,
            PARSER.KIND_REQUEST_WAIT,
            result=False,
        )
        result = PARSER.parse_trace_bytes(_snapshot(records))
        self.assertFalse(result["structural_epoch_comembership_available"])
        epoch = result["structural_epochs"]["7"]
        self.assertEqual(epoch["executed_request_ids"], [])
        self.assertEqual(epoch["failed_request_ids_excluded"], [11])

    def test_present_must_carry_boundary_frame_token(self):
        records = _valid_records()
        records[-1] = _record(310, 320, 41, 7, 71, 0, PARSER.KIND_PRESENT)
        result = PARSER.parse_trace_bytes(_snapshot(records))
        self.assertFalse(result["structural_epoch_comembership_available"])
        self.assertTrue(
            any("missing Frame token 71" in error for error in result["mapping_errors"])
        )


if __name__ == "__main__":
    unittest.main()
