#!/usr/bin/env python3
"""Parse LSX4 guest-invalidation overlap trace ABI v1.

V1 measures the interval union of synchronous producer WAIT spans and GpuComm
DRAIN callbacks. It deliberately has no frame-boundary/present mapping, so this
tool never reports a per-frame or causal FPS attribution.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from collections import Counter
from pathlib import Path
from typing import Iterable


MAGIC = b"LSX4IOT1"
ABI_VERSION = 1
CLOCK_STEADY_NANOSECONDS = 1
HEADER = struct.Struct("<8sIIIIQQQQIIIIQQ")
RECORD = struct.Struct("<QQQII")

KIND_WAIT = 1
KIND_DRAIN = 2
KIND_FRAME_BOUNDARY = 3
KIND_PRESENT = 4
KIND_MASK = 0xFF
FLAG_RESULT = 1 << 8
FLAG_SIZE_TRUNCATED = 1 << 9
FLAG_INVALID_INTERVAL = 1 << 10
KNOWN_COMMIT_BITS = (
    KIND_MASK | FLAG_RESULT | FLAG_SIZE_TRUNCATED | FLAG_INVALID_INTERVAL
)

FEATURE_WAIT = 1 << 0
FEATURE_DRAIN = 1 << 1
FEATURE_FRAME_BOUNDARY = 1 << 2
FEATURE_PRESENT = 1 << 3
REQUIRED_FEATURES = FEATURE_WAIT | FEATURE_DRAIN
KNOWN_FEATURES = (
    FEATURE_WAIT | FEATURE_DRAIN | FEATURE_FRAME_BOUNDARY | FEATURE_PRESENT
)

UINT64_MAX = (1 << 64) - 1
PAGE_BYTES = 4096
LOCALITY_GAPS_BYTES = (0, PAGE_BYTES, 16 * PAGE_BYTES)
PROJECTION_MAX_SPAN_PAGES = (4, 8, 16)


class TraceFormatError(ValueError):
    pass


def _merge(intervals: Iterable[tuple[int, int]]) -> list[tuple[int, int]]:
    ordered = sorted((begin, end) for begin, end in intervals if end > begin)
    merged: list[list[int]] = []
    for begin, end in ordered:
        if not merged or begin > merged[-1][1]:
            merged.append([begin, end])
        else:
            merged[-1][1] = max(merged[-1][1], end)
    return [(begin, end) for begin, end in merged]


def _duration(intervals: Iterable[tuple[int, int]]) -> int:
    return sum(end - begin for begin, end in intervals)


def _intersection_duration(
    left: list[tuple[int, int]], right: list[tuple[int, int]]
) -> int:
    left_index = 0
    right_index = 0
    total = 0
    while left_index < len(left) and right_index < len(right):
        begin = max(left[left_index][0], right[right_index][0])
        end = min(left[left_index][1], right[right_index][1])
        if end > begin:
            total += end - begin
        if left[left_index][1] <= right[right_index][1]:
            left_index += 1
        else:
            right_index += 1
    return total


def _maximum_concurrency(intervals: Iterable[tuple[int, int]]) -> int:

    points = []
    for begin, end in intervals:
        if end > begin:
            points.append((begin, 1))
            points.append((end, -1))
    current = 0
    maximum = 0
    for _, delta in sorted(points, key=lambda point: (point[0], point[1])):
        current += delta
        maximum = max(maximum, current)
    return maximum


def _span_summary(intervals: list[tuple[int, int]]) -> dict[str, int | float]:
    merged = _merge(intervals)
    summed_ns = _duration(intervals)
    union_ns = _duration(merged)
    return {
        "count": len(intervals),
        "summed_ns": summed_ns,
        "union_ns": union_ns,
        "summed_ms": summed_ns / 1_000_000.0,
        "union_ms": union_ns / 1_000_000.0,
        "maximum_concurrency": _maximum_concurrency(intervals),
    }


def _percent(numerator: int, denominator: int) -> float:
    return 0.0 if denominator == 0 else 100.0 * numerator / denominator


def _range_distance(left: tuple[int, int], right: tuple[int, int]) -> int:
    left_begin, left_end = left
    right_begin, right_end = right
    if left_end < right_begin:
        return right_begin - left_end
    if right_end < left_begin:
        return left_begin - right_end
    return 0


def _spatial_components(
    ranges: list[tuple[int, int]], gap_bytes: int
) -> dict[str, int | float]:
    if not ranges:
        return {
            "gap_bytes": gap_bytes,
            "components": 0,
            "largest_component_requests": 0,
            "envelope_bytes": 0,
            "extra_gap_bytes": 0,
        }
    ordered = sorted(ranges)
    component_begin, component_end = ordered[0]
    component_requests = 1
    components = 0
    largest_component_requests = 0
    envelope_bytes = 0
    for begin, end in ordered[1:]:
        if begin <= component_end + gap_bytes:
            component_end = max(component_end, end)
            component_requests += 1
            continue
        components += 1
        largest_component_requests = max(
            largest_component_requests, component_requests
        )
        envelope_bytes += component_end - component_begin
        component_begin, component_end = begin, end
        component_requests = 1
    components += 1
    largest_component_requests = max(largest_component_requests, component_requests)
    envelope_bytes += component_end - component_begin
    union_bytes = _duration(_merge(ranges))
    return {
        "gap_bytes": gap_bytes,
        "components": components,
        "largest_component_requests": largest_component_requests,
        "envelope_bytes": envelope_bytes,
        "extra_gap_bytes": envelope_bytes - union_bytes,
    }


def _greedy_projection(
    ordered_ranges: list[tuple[int, int]], gap_bytes: int, max_span_bytes: int
) -> dict[str, int | float]:
    if not ordered_ranges:
        return {
            "gap_bytes": gap_bytes,
            "max_span_pages": max_span_bytes // PAGE_BYTES,
            "projected_groups": 0,
            "projected_request_reduction": 0,
            "projected_request_reduction_percent": 0.0,
            "largest_group_requests": 0,
            "oversized_single_ranges": 0,
        }
    groups = 1
    group_begin, group_end = ordered_ranges[0]
    group_requests = 1
    largest_group_requests = 1
    oversized_single_ranges = int(group_end - group_begin > max_span_bytes)
    for begin, end in ordered_ranges[1:]:
        if end - begin > max_span_bytes:
            oversized_single_ranges += 1
        envelope_begin = min(group_begin, begin)
        envelope_end = max(group_end, end)
        can_join = (
            _range_distance((group_begin, group_end), (begin, end)) <= gap_bytes
            and envelope_end - envelope_begin <= max_span_bytes
        )
        if can_join:
            group_begin, group_end = envelope_begin, envelope_end
            group_requests += 1
            largest_group_requests = max(largest_group_requests, group_requests)
            continue
        groups += 1
        group_begin, group_end = begin, end
        group_requests = 1
    reduction = len(ordered_ranges) - groups
    return {
        "gap_bytes": gap_bytes,
        "max_span_pages": max_span_bytes // PAGE_BYTES,
        "projected_groups": groups,
        "projected_request_reduction": reduction,
        "projected_request_reduction_percent": _percent(
            reduction, len(ordered_ranges)
        ),
        "largest_group_requests": largest_group_requests,
        "oversized_single_ranges": oversized_single_ranges,
    }


def _ordered_locality_summary(
    records: list[tuple[int, int, int, int]], order: str
) -> dict[str, object]:
    if order == "slot_completion":
        ordered = sorted(records, key=lambda record: record[0])
        description = (
            "record reservation order after each wait completed; exact for slots, "
            "not producer invocation order"
        )
    elif order == "wait_begin":
        ordered = sorted(records, key=lambda record: (record[1], record[0]))
        description = (
            "wait begin timestamp with slot tie-break; a cross-thread chronology "
            "heuristic without thread identity"
        )
    else:
        raise ValueError(f"unsupported locality order: {order}")
    ranges = [(begin, end) for _, _, begin, end in ordered]
    pair_count = max(0, len(ranges) - 1)
    overlapping_pairs = 0
    touching_pairs = 0
    exact_duplicate_pairs = 0
    forward_pairs = 0
    backward_pairs = 0
    within_gap = {gap: 0 for gap in LOCALITY_GAPS_BYTES}
    distances = []
    for left, right in zip(ranges, ranges[1:]):
        overlap = min(left[1], right[1]) - max(left[0], right[0])
        if overlap > 0:
            overlapping_pairs += 1
        elif overlap == 0 and (left[1] == right[0] or right[1] == left[0]):
            touching_pairs += 1
        if left == right:
            exact_duplicate_pairs += 1
        if right[0] >= left[0]:
            forward_pairs += 1
        else:
            backward_pairs += 1
        distance = _range_distance(left, right)
        distances.append(distance)
        for gap in LOCALITY_GAPS_BYTES:
            within_gap[gap] += distance <= gap
    projections = [
        _greedy_projection(ranges, gap, pages * PAGE_BYTES)
        for gap in LOCALITY_GAPS_BYTES
        for pages in PROJECTION_MAX_SPAN_PAGES
    ]
    return {
        "order": order,
        "description": description,
        "requests": len(ranges),
        "consecutive_pairs": pair_count,
        "overlapping_pairs": overlapping_pairs,
        "touching_pairs": touching_pairs,
        "exact_duplicate_pairs": exact_duplicate_pairs,
        "forward_pairs": forward_pairs,
        "backward_pairs": backward_pairs,
        "within_gap_pairs": {
            str(gap): {
                "count": within_gap[gap],
                "percent": _percent(within_gap[gap], pair_count),
            }
            for gap in LOCALITY_GAPS_BYTES
        },
        "consecutive_gap_bytes": {
            "minimum": min(distances, default=0),
            "maximum": max(distances, default=0),
        },
        "greedy_grouping_projection": projections,
    }


def _address_locality_summary(
    records: list[tuple[int, int, int, int]],
    *,
    coverage_exact: bool,
    failed_waits: int,
    excluded_truncated: int,
    excluded_zero_size: int,
    excluded_overflow: int,
) -> dict[str, object]:
    ranges = [(begin, end) for _, _, begin, end in records]
    sizes = sorted(end - begin for begin, end in ranges)
    requested_bytes = sum(sizes)
    union_bytes = _duration(_merge(ranges))
    size_counts = Counter(sizes)

    def percentile(numerator: int, denominator: int) -> int:
        if not sizes:
            return 0
        index = ((len(sizes) - 1) * numerator) // denominator
        return sizes[index]

    analysis_exact = bool(
        coverage_exact
        and failed_waits == 0
        and excluded_truncated == 0
        and excluded_zero_size == 0
        and excluded_overflow == 0
    )
    return {
        "scope": (
            "whole trace window; successful WAIT address ranges only; no frame, "
            "producer-thread, request-to-drain, or dependency identity"
        ),
        "analysis_exact": analysis_exact,
        "safe_coalescing_proven": False,
        "interpretation": (
            "locality and greedy projections are necessary-opportunity diagnostics "
            "only; they do not prove that delaying, widening, or merging invalidations "
            "is safe or removes the synchronous dependency"
        ),
        "analyzable_successful_waits": len(records),
        "excluded": {
            "failed_waits": failed_waits,
            "truncated_sizes": excluded_truncated,
            "zero_sizes": excluded_zero_size,
            "uint64_end_overflows": excluded_overflow,
        },
        "requested_bytes": requested_bytes,
        "address_union_bytes": union_bytes,
        "repeated_or_overlapping_bytes": requested_bytes - union_bytes,
        "unique_exact_ranges": len(set(ranges)),
        "page_aligned_addresses": sum(begin % PAGE_BYTES == 0 for begin, _ in ranges),
        "page_multiple_sizes": sum(size % PAGE_BYTES == 0 for size in sizes),
        "size_bytes": {
            "minimum": min(sizes, default=0),
            "median_lower": percentile(1, 2),
            "p90_lower": percentile(9, 10),
            "maximum": max(sizes, default=0),
            "most_common": [
                {"size": size, "count": count}
                for size, count in size_counts.most_common(8)
            ],
        },
        "global_spatial_components": [
            _spatial_components(ranges, gap) for gap in LOCALITY_GAPS_BYTES
        ],
        "orders": {
            order: _ordered_locality_summary(records, order)
            for order in ("slot_completion", "wait_begin")
        },
    }


def parse_trace_bytes(data: bytes) -> dict[str, object]:
    if len(data) < HEADER.size:
        raise TraceFormatError(
            f"short header: got {len(data)} bytes, need {HEADER.size}"
        )
    (
        magic,
        abi_version,
        header_size,
        record_size,
        clock_id,
        capacity_entries,
        reserved_entries,
        committed_entries_header,
        dropped_entries,
        enabled,
        saturated,
        coverage_exact_header,
        epoch_mapping_exact,
        feature_flags,
        reserved_zero,
    ) = HEADER.unpack_from(data)
    if magic != MAGIC:
        raise TraceFormatError(f"bad magic: {magic!r}")
    if abi_version != ABI_VERSION:
        raise TraceFormatError(f"unsupported ABI version: {abi_version}")
    if header_size != HEADER.size or record_size != RECORD.size:
        raise TraceFormatError(
            f"layout mismatch: header={header_size}, record={record_size}"
        )
    if clock_id != CLOCK_STEADY_NANOSECONDS:
        raise TraceFormatError(f"unsupported clock id: {clock_id}")
    for name, value in (
        ("enabled", enabled),
        ("saturated", saturated),
        ("coverage_exact", coverage_exact_header),
        ("epoch_mapping_exact", epoch_mapping_exact),
    ):
        if value not in (0, 1):
            raise TraceFormatError(f"non-boolean {name}: {value}")
    if capacity_entries == 0:
        raise TraceFormatError("fixed capacity is zero")
    if reserved_entries > capacity_entries:
        raise TraceFormatError("reserved entries exceed fixed capacity")
    if committed_entries_header > reserved_entries:
        raise TraceFormatError("committed entries exceed reserved entries")
    if feature_flags & ~KNOWN_FEATURES:
        raise TraceFormatError(
            f"unknown v1 feature bits: 0x{feature_flags & ~KNOWN_FEATURES:x}"
        )
    if feature_flags & REQUIRED_FEATURES != REQUIRED_FEATURES:
        raise TraceFormatError("v1 snapshot is missing WAIT or DRAIN feature")
    if epoch_mapping_exact and (
        feature_flags & (FEATURE_FRAME_BOUNDARY | FEATURE_PRESENT)
        != (FEATURE_FRAME_BOUNDARY | FEATURE_PRESENT)
    ):
        raise TraceFormatError("exact epoch mapping lacks frame/present features")
    expected_size = header_size + reserved_entries * record_size
    if len(data) != expected_size:
        raise TraceFormatError(
            f"snapshot size mismatch: got {len(data)}, expected {expected_size}"
        )
    if reserved_zero != 0:
        raise TraceFormatError("reserved header field is nonzero")

    intervals: dict[int, list[tuple[int, int]]] = {
        KIND_WAIT: [],
        KIND_DRAIN: [],
        KIND_FRAME_BOUNDARY: [],
        KIND_PRESENT: [],
    }
    committed = 0
    invalid_records = 0
    unknown_kind_records = 0
    unknown_flag_records = 0
    successful_waits = 0
    failed_waits = 0
    truncated_sizes = 0
    address_records: list[tuple[int, int, int, int]] = []
    address_excluded_truncated = 0
    address_excluded_zero_size = 0
    address_excluded_overflow = 0
    first_ns: int | None = None
    last_ns: int | None = None
    for slot in range(reserved_entries):
        offset = header_size + slot * record_size
        begin_ns, end_ns, argument, size, commit = RECORD.unpack_from(
            data, offset
        )
        if commit == 0:
            continue
        committed += 1
        if commit & ~KNOWN_COMMIT_BITS:
            unknown_flag_records += 1
        kind = commit & KIND_MASK
        if commit & FLAG_SIZE_TRUNCATED:
            truncated_sizes += 1
        if kind == KIND_WAIT:
            if commit & FLAG_RESULT:
                successful_waits += 1
                if commit & FLAG_SIZE_TRUNCATED:
                    address_excluded_truncated += 1
                elif size == 0:
                    address_excluded_zero_size += 1
                elif argument > UINT64_MAX - size:
                    address_excluded_overflow += 1
                elif not (end_ns < begin_ns or commit & FLAG_INVALID_INTERVAL):
                    address_records.append(
                        (slot, begin_ns, argument, argument + size)
                    )
            else:
                failed_waits += 1
        if kind not in intervals:
            unknown_kind_records += 1
            continue
        if end_ns < begin_ns or commit & FLAG_INVALID_INTERVAL:
            invalid_records += 1
            continue
        intervals[kind].append((begin_ns, end_ns))
        first_ns = begin_ns if first_ns is None else min(first_ns, begin_ns)
        last_ns = end_ns if last_ns is None else max(last_ns, end_ns)

    wait_intervals = intervals[KIND_WAIT]
    drain_intervals = intervals[KIND_DRAIN]
    wait_merged = _merge(wait_intervals)
    drain_merged = _merge(drain_intervals)
    wait_union_ns = _duration(wait_merged)
    drain_union_ns = _duration(drain_merged)
    overlap_ns = _intersection_duration(wait_merged, drain_merged)
    combined_union_ns = _duration(_merge(wait_intervals + drain_intervals))
    capture_consistent = committed == committed_entries_header
    coverage_exact = bool(
        coverage_exact_header
        and not saturated
        and not enabled
        and dropped_entries == 0
        and capture_consistent
        and committed == reserved_entries
        and invalid_records == 0
        and unknown_kind_records == 0
        and unknown_flag_records == 0
    )
    mapping_features_present = bool(
        feature_flags & (FEATURE_FRAME_BOUNDARY | FEATURE_PRESENT)
    )
    mapping_events_present = bool(
        intervals[KIND_FRAME_BOUNDARY] or intervals[KIND_PRESENT]
    )
    epoch_mapping_available = bool(
        epoch_mapping_exact and mapping_features_present and mapping_events_present
    )

    warnings = []
    if not capture_consistent:
        warnings.append("snapshot changed while it was copied")
    if committed != reserved_entries:
        warnings.append("reserved prefix contains uncommitted slots")
    if saturated or dropped_entries:
        warnings.append("trace saturated; interval coverage is incomplete")
    if invalid_records or unknown_kind_records or unknown_flag_records:
        warnings.append("trace contains invalid or unknown committed records")
    if enabled:
        warnings.append("snapshot was taken while recording was active")
    if not epoch_mapping_available:
        warnings.append(
            "ABI v1 has no exact frame-boundary/present mapping; do not infer causal frame cost"
        )

    return {
        "format": "lsx4-guest-invalidation-overlap-v1",
        "abi_version": abi_version,
        "clock": "steady_clock_nanoseconds",
        "capacity_entries": capacity_entries,
        "reserved_entries": reserved_entries,
        "committed_entries": committed,
        "committed_entries_header": committed_entries_header,
        "empty_reserved_slots": reserved_entries - committed,
        "dropped_entries": dropped_entries,
        "enabled_at_snapshot": bool(enabled),
        "saturated": bool(saturated),
        "capture_consistent": capture_consistent,
        "unknown_flag_records": unknown_flag_records,
        "coverage_exact": coverage_exact,
        "trace_window_ns": 0 if first_ns is None else last_ns - first_ns,
        "wait": {
            **_span_summary(wait_intervals),
            "successful": successful_waits,
            "failed": failed_waits,
            "truncated_sizes": truncated_sizes,
        },
        "address_locality": _address_locality_summary(
            address_records,
            coverage_exact=coverage_exact,
            failed_waits=failed_waits,
            excluded_truncated=address_excluded_truncated,
            excluded_zero_size=address_excluded_zero_size,
            excluded_overflow=address_excluded_overflow,
        ),
        "drain": _span_summary(drain_intervals),
        "wait_drain_overlap_ns": overlap_ns,
        "wait_drain_overlap_ms": overlap_ns / 1_000_000.0,
        "wait_union_outside_drain_ns": wait_union_ns - overlap_ns,
        "drain_union_without_wait_ns": drain_union_ns - overlap_ns,
        "wait_union_overlapped_percent": (
            0.0 if wait_union_ns == 0 else 100.0 * overlap_ns / wait_union_ns
        ),
        "drain_union_overlapped_percent": (
            0.0 if drain_union_ns == 0 else 100.0 * overlap_ns / drain_union_ns
        ),
        "combined_union_ns": combined_union_ns,
        "combined_union_ms": combined_union_ns / 1_000_000.0,
        "epoch_mapping": {
            "exact": bool(epoch_mapping_exact),
            "available": epoch_mapping_available,
            "frame_boundary_events": len(intervals[KIND_FRAME_BOUNDARY]),
            "present_events": len(intervals[KIND_PRESENT]),
            "status": (
                "available"
                if epoch_mapping_available
                else "pending: v1 records WAIT+DRAIN only"
            ),
        },
        "causal_frame_attribution_available": False,
        "warnings": warnings,
    }


def parse_trace(path: Path) -> dict[str, object]:
    return parse_trace_bytes(path.read_bytes())


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path, help="v1 snapshot produced by the ABI")
    parser.add_argument("--compact", action="store_true", help="emit one-line JSON")
    parser.add_argument(
        "--require-exact",
        action="store_true",
        help="return nonzero when the fixed buffer saturated or capture raced",
    )
    arguments = parser.parse_args(argv)
    try:
        result = parse_trace(arguments.trace)
    except (OSError, TraceFormatError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(
        json.dumps(
            result,
            ensure_ascii=False,
            indent=None if arguments.compact else 2,
            sort_keys=True,
        )
    )
    if arguments.require_exact and not result["coverage_exact"]:
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
