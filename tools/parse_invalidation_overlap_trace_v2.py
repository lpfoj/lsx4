#!/usr/bin/env python3
"""Parse LSX4 guest-invalidation structural epoch co-membership ABI v2.

V2 validates explicit identities from a successful synchronous guest
invalidation request to its drain batch and from that batch's fully captured
render epoch to the exact concrete Frame carried through boundary and present.
It never claims that invalidation caused the observed frame delay.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from collections import defaultdict
from pathlib import Path


MAGIC = b"LSX4IOT2"
ABI_VERSION = 2
CLOCK_STEADY_NANOSECONDS = 1
HEADER = struct.Struct("<8sIIIIQQQQIIIIQQQQQQQQQQ")
RECORD = struct.Struct("<QQQQQII")

KIND_REQUEST_WAIT = 1
KIND_REQUEST_BATCH = 2
KIND_DRAIN_BATCH = 3
KIND_FRAME_BOUNDARY = 4
KIND_PRESENT = 5
KNOWN_KINDS = {
    KIND_REQUEST_WAIT,
    KIND_REQUEST_BATCH,
    KIND_DRAIN_BATCH,
    KIND_FRAME_BOUNDARY,
    KIND_PRESENT,
}
KIND_MASK = 0xFF
FLAG_RESULT = 1 << 8
FLAG_SIZE_TRUNCATED = 1 << 9
FLAG_INVALID_INTERVAL = 1 << 10
FLAG_EPOCH_CROSSED = 1 << 11
KNOWN_COMMIT_BITS = (
    KIND_MASK
    | FLAG_RESULT
    | FLAG_SIZE_TRUNCATED
    | FLAG_INVALID_INTERVAL
    | FLAG_EPOCH_CROSSED
)

FEATURE_REQUEST_IDENTITY = 1 << 0
FEATURE_REQUEST_BATCH = 1 << 1
FEATURE_DRAIN_IDENTITY = 1 << 2
FEATURE_FRAME_EPOCHS = 1 << 3
FEATURE_PRESENT_IDENTITY = 1 << 4
FEATURE_CAPTURE_EPOCH = 1 << 5
FEATURE_INSTRUMENTED_PERTURBATION = 1 << 6
FEATURE_FRAME_OWNERSHIP_TOKEN = 1 << 7
REQUIRED_FEATURES = (
    FEATURE_REQUEST_IDENTITY
    | FEATURE_REQUEST_BATCH
    | FEATURE_DRAIN_IDENTITY
    | FEATURE_FRAME_EPOCHS
    | FEATURE_PRESENT_IDENTITY
    | FEATURE_CAPTURE_EPOCH
    | FEATURE_INSTRUMENTED_PERTURBATION
    | FEATURE_FRAME_OWNERSHIP_TOKEN
)


class TraceFormatError(ValueError):
    pass


def _record_dict(slot: int, values: tuple[int, ...]) -> dict[str, int | bool]:
    begin, end, primary, related, argument, size, commit = values
    return {
        "slot": slot,
        "begin_ns": begin,
        "end_ns": end,
        "primary_id": primary,
        "related_id": related,
        "argument": argument,
        "size": size,
        "commit": commit,
        "kind": commit & KIND_MASK,
        "result": bool(commit & FLAG_RESULT),
        "size_truncated": bool(commit & FLAG_SIZE_TRUNCATED),
        "invalid_interval": bool(commit & FLAG_INVALID_INTERVAL),
        "epoch_crossed": bool(commit & FLAG_EPOCH_CROSSED),
    }


def parse_trace_bytes(data: bytes) -> dict[str, object]:
    if len(data) < HEADER.size:
        raise TraceFormatError(
            f"short v2 header: got {len(data)} bytes, need {HEADER.size}"
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
        epoch_mapping_exact_header,
        feature_flags,
        capture_epoch,
        capture_start_ns,
        capture_stop_ns,
        active_spans,
        left_censored_requests,
        right_censored_requests,
        mixed_generation_batches,
        epoch_crossing_batches,
        reserved_zero,
    ) = HEADER.unpack_from(data)

    if magic != MAGIC:
        raise TraceFormatError(f"bad v2 magic: {magic!r}")
    if abi_version != ABI_VERSION:
        raise TraceFormatError(f"unsupported v2 ABI version: {abi_version}")
    if header_size != HEADER.size or record_size != RECORD.size:
        raise TraceFormatError(
            f"v2 layout mismatch: header={header_size}, record={record_size}"
        )
    if clock_id != CLOCK_STEADY_NANOSECONDS:
        raise TraceFormatError(f"unsupported clock id: {clock_id}")
    for name, value in (
        ("enabled", enabled),
        ("saturated", saturated),
        ("coverage_exact", coverage_exact_header),
        ("epoch_mapping_exact", epoch_mapping_exact_header),
    ):
        if value not in (0, 1):
            raise TraceFormatError(f"non-boolean {name}: {value}")
    if capacity_entries == 0 or reserved_entries > capacity_entries:
        raise TraceFormatError("invalid fixed capacity/reserved prefix")
    if committed_entries_header > reserved_entries:
        raise TraceFormatError("committed entries exceed reserved entries")
    if feature_flags != REQUIRED_FEATURES:
        raise TraceFormatError(
            f"v2 requires exactly feature mask 0x{REQUIRED_FEATURES:x}; "
            f"got 0x{feature_flags:x}"
        )
    if capture_epoch == 0 or capture_start_ns == 0:
        raise TraceFormatError("missing capture epoch/start")
    if capture_stop_ns and capture_stop_ns < capture_start_ns:
        raise TraceFormatError("capture stop precedes capture start")
    if reserved_zero != 0:
        raise TraceFormatError("reserved v2 header field is nonzero")
    expected_size = header_size + reserved_entries * record_size
    if len(data) != expected_size:
        raise TraceFormatError(
            f"v2 snapshot size mismatch: got {len(data)}, expected {expected_size}"
        )

    records: list[dict[str, int | bool]] = []
    uncommitted_slots = 0
    unknown_flag_records = 0
    invalid_records: list[str] = []
    for slot in range(reserved_entries):
        values = RECORD.unpack_from(data, header_size + slot * record_size)
        if values[-1] == 0:
            uncommitted_slots += 1
            continue
        record = _record_dict(slot, values)
        records.append(record)
        commit = int(record["commit"])
        kind = int(record["kind"])
        if commit & ~KNOWN_COMMIT_BITS:
            unknown_flag_records += 1
        if kind not in KNOWN_KINDS:
            invalid_records.append(f"slot {slot}: unknown kind {kind}")
            continue
        begin = int(record["begin_ns"])
        end = int(record["end_ns"])
        primary = int(record["primary_id"])
        related = int(record["related_id"])
        argument = int(record["argument"])
        size = int(record["size"])
        if begin < capture_start_ns:
            invalid_records.append(f"slot {slot}: begins before capture")
        if (
            capture_stop_ns
            and kind in (KIND_REQUEST_WAIT, KIND_FRAME_BOUNDARY, KIND_PRESENT)
            and begin > capture_stop_ns
        ):
            invalid_records.append(
                f"slot {slot}: admission-time event begins after capture stop"
            )
        if end < begin or bool(record["invalid_interval"]):
            invalid_records.append(f"slot {slot}: invalid interval")
        if primary == 0:
            invalid_records.append(f"slot {slot}: zero primary identity")
        if kind == KIND_REQUEST_WAIT:
            if commit & FLAG_EPOCH_CROSSED:
                invalid_records.append(f"slot {slot}: WAIT carries epoch-crossed")
            if related == 0:
                invalid_records.append(f"slot {slot}: zero enqueue epoch")
        elif kind == KIND_REQUEST_BATCH:
            if commit & (FLAG_SIZE_TRUNCATED | FLAG_EPOCH_CROSSED):
                invalid_records.append(f"slot {slot}: mapping carries span-only flags")
            if related == 0 or argument == 0 or begin != end or size != 0:
                invalid_records.append(f"slot {slot}: malformed request-batch edge")
            if not bool(record["result"]):
                invalid_records.append(f"slot {slot}: uncommitted mapping result")
        elif kind == KIND_DRAIN_BATCH:
            if related == 0 or argument == 0 or size == 0:
                invalid_records.append(f"slot {slot}: malformed drain batch")
            crossed = related != argument
            if crossed != bool(record["epoch_crossed"]):
                invalid_records.append(f"slot {slot}: inconsistent epoch crossing")
            if not bool(record["result"]):
                invalid_records.append(f"slot {slot}: drain result is false")
        elif kind == KIND_FRAME_BOUNDARY:
            if commit & (FLAG_SIZE_TRUNCATED | FLAG_EPOCH_CROSSED):
                invalid_records.append(f"slot {slot}: boundary carries span-only flags")
            if related == 0 or begin != end or size not in (0, 1):
                invalid_records.append(f"slot {slot}: malformed frame boundary")
            if not bool(record["result"]):
                invalid_records.append(f"slot {slot}: boundary result is false")
        elif kind == KIND_PRESENT:
            if commit & (FLAG_SIZE_TRUNCATED | FLAG_EPOCH_CROSSED):
                invalid_records.append(f"slot {slot}: present carries unrelated flags")
            if related == 0 or argument == 0 or size != 0:
                invalid_records.append(f"slot {slot}: malformed present")

    capture_consistent = len(records) == committed_entries_header
    coverage_exact = bool(
        coverage_exact_header
        and not enabled
        and not saturated
        and dropped_entries == 0
        and active_spans == 0
        and capture_stop_ns != 0
        and capture_consistent
        and uncommitted_slots == 0
        and unknown_flag_records == 0
        and not invalid_records
    )

    by_kind: dict[int, list[dict[str, int | bool]]] = defaultdict(list)
    for record in records:
        by_kind[int(record["kind"])].append(record)
    waits = by_kind[KIND_REQUEST_WAIT]
    edges = by_kind[KIND_REQUEST_BATCH]
    drains = by_kind[KIND_DRAIN_BATCH]
    boundaries = by_kind[KIND_FRAME_BOUNDARY]
    presents = by_kind[KIND_PRESENT]

    mapping_errors: list[str] = []

    def unique(records_for_kind, label):
        result = {}
        for record in records_for_kind:
            identity = int(record["primary_id"])
            if identity in result:
                mapping_errors.append(f"duplicate {label} id {identity}")
            else:
                result[identity] = record
        return result

    waits_by_id = unique(waits, "request")
    drains_by_id = unique(drains, "batch")
    boundaries_by_id = unique(boundaries, "boundary")
    presents_by_id = unique(presents, "present")
    del boundaries_by_id, presents_by_id

    edges_by_request: dict[int, list[dict[str, int | bool]]] = defaultdict(list)
    edges_by_batch: dict[int, list[dict[str, int | bool]]] = defaultdict(list)
    seen_edges: set[tuple[int, int]] = set()
    for edge in edges:
        request_id = int(edge["primary_id"])
        batch_id = int(edge["related_id"])
        pair = (request_id, batch_id)
        if pair in seen_edges:
            mapping_errors.append(f"duplicate request-batch edge {request_id}->{batch_id}")
        seen_edges.add(pair)
        edges_by_request[request_id].append(edge)
        edges_by_batch[batch_id].append(edge)
        wait = waits_by_id.get(request_id)
        drain = drains_by_id.get(batch_id)
        if wait is None:
            mapping_errors.append(f"edge references missing request {request_id}")
            continue
        if drain is None:
            mapping_errors.append(f"edge references missing batch {batch_id}")
            continue
        edge_time = int(edge["begin_ns"])
        if not int(drain["begin_ns"]) <= edge_time <= int(drain["end_ns"]):
            mapping_errors.append(f"edge {request_id}->{batch_id} lies outside drain")
        if int(edge["argument"]) != int(drain["related_id"]):
            mapping_errors.append(f"edge {request_id}->{batch_id} epoch mismatch")
        if int(wait["related_id"]) != int(drain["related_id"]):
            mapping_errors.append(
                f"request {request_id} enqueue epoch does not equal batch {batch_id} epoch"
            )
        if not (
            int(wait["begin_ns"]) <= int(drain["begin_ns"])
            and int(drain["end_ns"]) <= int(wait["end_ns"])
        ):
            mapping_errors.append(f"batch {batch_id} is not enclosed by wait {request_id}")

    for request_id in waits_by_id:
        if request_id not in edges_by_request:
            mapping_errors.append(f"request {request_id} has no drain-batch edge")
    for batch_id, drain in drains_by_id.items():
        if batch_id not in edges_by_batch:
            mapping_errors.append(f"batch {batch_id} has no request edge")
        if bool(drain["epoch_crossed"]):
            mapping_errors.append(f"batch {batch_id} crosses frame epochs")
        expected = int(drain["size"])
        actual = len(edges_by_batch.get(batch_id, ()))
        if expected != actual:
            mapping_errors.append(
                f"batch {batch_id} request count {expected} != mapped {actual}"
            )

    boundaries_by_epoch: dict[int, list[dict[str, int | bool]]] = defaultdict(list)
    presents_by_epoch: dict[int, list[dict[str, int | bool]]] = defaultdict(list)
    boundary_by_frame_token: dict[int, dict[str, int | bool]] = {}
    for boundary in boundaries:
        epoch = int(boundary["related_id"])
        frame_token = int(boundary["argument"])
        boundaries_by_epoch[epoch].append(boundary)
        if frame_token:
            if frame_token in boundary_by_frame_token:
                mapping_errors.append(
                    f"Frame token {frame_token} is owned by multiple boundaries"
                )
            else:
                boundary_by_frame_token[frame_token] = boundary
    for epoch, epoch_boundaries in boundaries_by_epoch.items():
        if len(epoch_boundaries) != 1:
            mapping_errors.append(
                f"epoch {epoch} has {len(epoch_boundaries)} closing boundaries; "
                "concurrent-flip ownership is ambiguous"
            )
    for present in presents:
        epoch = int(present["related_id"])
        frame_token = int(present["argument"])
        presents_by_epoch[epoch].append(present)
        owner = boundary_by_frame_token.get(frame_token)
        if owner is None:
            mapping_errors.append(
                f"present {present['primary_id']} references missing Frame token {frame_token}"
            )
        elif int(owner["related_id"]) != epoch:
            mapping_errors.append(
                f"present {present['primary_id']} Frame token {frame_token} epoch mismatch"
            )

    first_closed_epoch = None
    if boundaries:
        first_boundary = min(
            boundaries, key=lambda item: (int(item["begin_ns"]), int(item["slot"]))
        )
        first_closed_epoch = int(first_boundary["related_id"])

    structural_epochs: dict[int, dict[str, object]] = {}
    quarantined_epochs: dict[int, list[str]] = {}
    mapped_epochs = sorted(
        {
            int(drain["related_id"])
            for drain in drains
            if int(drain["primary_id"]) in edges_by_batch
        }
    )
    for epoch in mapped_epochs:
        quarantine_reasons: list[str] = []
        epoch_errors: list[str] = []
        epoch_boundaries = boundaries_by_epoch.get(epoch, [])
        prior_boundaries = boundaries_by_epoch.get(epoch - 1, []) if epoch else []
        if epoch == first_closed_epoch:
            quarantine_reasons.append("first-captured-closed-epoch")
        elif len(prior_boundaries) != 1:
            message = (
                f"epoch {epoch} lacks one unique prior in-capture boundary for epoch {epoch - 1}"
            )
            mapping_errors.append(message)
            epoch_errors.append(message)
            quarantine_reasons.append("missing-unique-prior-boundary")
        if len(epoch_boundaries) != 1:
            epoch_errors.append("missing-or-ambiguous-closing-boundary")
            quarantine_reasons.append("missing-unique-closing-boundary")

        boundary = epoch_boundaries[0] if len(epoch_boundaries) == 1 else None
        prior_boundary = prior_boundaries[0] if len(prior_boundaries) == 1 else None
        frame_token = int(boundary["argument"]) if boundary is not None else 0
        if boundary is not None and frame_token == 0:
            message = f"epoch {epoch} closing boundary has no concrete Frame token"
            mapping_errors.append(message)
            epoch_errors.append(message)
            quarantine_reasons.append("boundary-without-frame-owner")

        epoch_drains = [
            drain for drain in drains if int(drain["related_id"]) == epoch
        ]
        epoch_edges = [
            edge
            for drain in epoch_drains
            for edge in edges_by_batch.get(int(drain["primary_id"]), ())
        ]
        request_ids = sorted({int(edge["primary_id"]) for edge in epoch_edges})
        executed_request_ids = sorted(
            request_id
            for request_id in request_ids
            if request_id in waits_by_id and bool(waits_by_id[request_id]["result"])
        )
        failed_request_ids = sorted(set(request_ids) - set(executed_request_ids))
        if not executed_request_ids:
            quarantine_reasons.append("no-successful-executed-invalidation")

        epoch_presents = presents_by_epoch.get(epoch, [])
        matching_successful_presents = [
            present
            for present in epoch_presents
            if bool(present["result"]) and int(present["argument"]) == frame_token
        ]
        for present in epoch_presents:
            if boundary is not None and int(present["argument"]) != frame_token:
                message = (
                    f"epoch {epoch} present {present['primary_id']} is not owned by "
                    f"boundary Frame token {frame_token}"
                )
                mapping_errors.append(message)
                epoch_errors.append(message)

        qualifying_presents: list[dict[str, int | bool]] = []
        if boundary is not None:
            boundary_time = int(boundary["begin_ns"])
            qualifying_presents = [
                present
                for present in matching_successful_presents
                if boundary_time <= int(present["begin_ns"]) <= int(present["end_ns"])
            ]
            if executed_request_ids and not qualifying_presents:
                message = (
                    f"epoch {epoch} has no successful Frame-owned present after its boundary"
                )
                mapping_errors.append(message)
                epoch_errors.append(message)

        if boundary is not None and prior_boundary is not None:
            prior_time = int(prior_boundary["begin_ns"])
            boundary_time = int(boundary["begin_ns"])
            for request_id in executed_request_ids:
                wait = waits_by_id[request_id]
                wait_begin = int(wait["begin_ns"])
                wait_end = int(wait["end_ns"])
                if prior_time > wait_begin:
                    message = (
                        f"request {request_id} begins before epoch {epoch} prior boundary"
                    )
                    mapping_errors.append(message)
                    epoch_errors.append(message)
                if wait_end > boundary_time:
                    message = (
                        f"request {request_id} ends after epoch {epoch} closing boundary"
                    )
                    mapping_errors.append(message)
                    epoch_errors.append(message)
                for edge in edges_by_request.get(request_id, ()):
                    drain = drains_by_id.get(int(edge["related_id"]))
                    if drain is None or int(drain["related_id"]) != epoch:
                        continue
                    if not (
                        wait_begin
                        <= int(drain["begin_ns"])
                        <= int(drain["end_ns"])
                        <= wait_end
                        <= boundary_time
                    ):
                        message = (
                            f"request {request_id}/batch {drain['primary_id']} violates "
                            "request<=drain<=request-end<=boundary order"
                        )
                        mapping_errors.append(message)
                        epoch_errors.append(message)

        eligible = bool(
            not quarantine_reasons
            and not epoch_errors
            and executed_request_ids
            and qualifying_presents
        )
        if quarantine_reasons:
            quarantined_epochs[epoch] = sorted(set(quarantine_reasons))
        structural_epochs[epoch] = {
            "eligible": eligible,
            "batch_ids": sorted(int(drain["primary_id"]) for drain in epoch_drains),
            "request_ids": request_ids,
            "executed_request_ids": executed_request_ids,
            "failed_request_ids_excluded": failed_request_ids,
            "prior_boundary_ids": (
                [int(prior_boundary["primary_id"])] if prior_boundary is not None else []
            ),
            "boundary_ids": [int(item["primary_id"]) for item in epoch_boundaries],
            "frame_token": frame_token,
            "present_ids": sorted(int(item["primary_id"]) for item in epoch_presents),
            "qualifying_successful_present_ids": sorted(
                int(item["primary_id"]) for item in qualifying_presents
            ),
            "errors": sorted(set(epoch_errors)),
            "quarantine_reasons": sorted(set(quarantine_reasons)),
        }

    header_mapping_clean = bool(
        epoch_mapping_exact_header
        and left_censored_requests == 0
        and right_censored_requests == 0
        and mixed_generation_batches == 0
        and epoch_crossing_batches == 0
    )
    eligible_epochs = sorted(
        epoch for epoch, report in structural_epochs.items() if report["eligible"]
    )
    structural_epoch_comembership_available = bool(
        coverage_exact
        and header_mapping_clean
        and eligible_epochs
        and not mapping_errors
    )

    warnings: list[str] = []
    if saturated or dropped_entries:
        warnings.append("trace saturated or dropped records; non-verdict")
    if uncommitted_slots:
        warnings.append("reserved prefix contains uncommitted slots; non-verdict")
    if active_spans:
        warnings.append("logical spans did not quiesce; non-verdict")
    if left_censored_requests or right_censored_requests or mixed_generation_batches:
        warnings.append("capture contains an unmapped/mixed request generation; non-verdict")
    if epoch_crossing_batches:
        warnings.append("a drain batch crossed a frame epoch; non-verdict")
    if mapping_errors:
        warnings.append("identity/epoch referential validation failed; non-verdict")
    if quarantined_epochs:
        warnings.append(
            "the first captured closed epoch and any epoch without a unique prior "
            "boundary are quarantined"
        )
    if not structural_epoch_comembership_available:
        warnings.append("no proof-grade structural epoch co-membership verdict")
    warnings.append("structural co-membership is not delay causality")

    return {
        "format": "lsx4-guest-invalidation-overlap-v2",
        "abi_version": abi_version,
        "clock": "steady_clock_nanoseconds",
        "capture_epoch": capture_epoch,
        "capture_start_ns": capture_start_ns,
        "capture_stop_ns": capture_stop_ns,
        "capacity_entries": capacity_entries,
        "reserved_entries": reserved_entries,
        "committed_entries": len(records),
        "committed_entries_header": committed_entries_header,
        "uncommitted_slots": uncommitted_slots,
        "dropped_entries": dropped_entries,
        "enabled_at_snapshot": bool(enabled),
        "saturated": bool(saturated),
        "active_spans": active_spans,
        "coverage_exact": coverage_exact,
        "structural_epoch_mapping_exact": header_mapping_clean,
        "left_censored_requests": left_censored_requests,
        "right_censored_requests": right_censored_requests,
        "mixed_generation_batches": mixed_generation_batches,
        "epoch_crossing_batches": epoch_crossing_batches,
        "invalid_records": invalid_records,
        "events": {
            "request_waits": len(waits),
            "request_batch_edges": len(edges),
            "drain_batches": len(drains),
            "frame_boundaries": len(boundaries),
            "presents": len(presents),
            "successful_presents": sum(bool(item["result"]) for item in presents),
        },
        "mapping_errors": mapping_errors,
        "first_captured_closed_epoch": first_closed_epoch,
        "quarantined_epochs": {
            str(key): value for key, value in quarantined_epochs.items()
        },
        "eligible_epochs": eligible_epochs,
        "structural_epochs": {
            str(key): value for key, value in structural_epochs.items()
        },
        "structural_epoch_comembership_available": (
            structural_epoch_comembership_available
        ),
        "structural_semantics": (
            "successful request identity -> drain batch in the exact enqueue epoch -> "
            "unique closing boundary Frame token -> successful present carrying the same Frame"
        ),
        "delay_causality_proven": False,
        "delay_causality_status": "requires matched intervention/control",
        "instrumentation_perturbed": True,
        "production_frametime_usable": False,
        "production_frametime_status": (
            "use a separate instrumentation-OFF matched capture; v2 timings are diagnostic"
        ),
        "verdict": (
            "structural-epoch-comembership"
            if structural_epoch_comembership_available
            else "non-verdict"
        ),
        "warnings": warnings,
    }


def parse_trace(path: Path) -> dict[str, object]:
    return parse_trace_bytes(path.read_bytes())


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path, help="v2 snapshot produced by LSX4")
    parser.add_argument("--compact", action="store_true")
    parser.add_argument(
        "--require-structural-epoch-comembership",
        action="store_true",
        help="return nonzero unless a fully bounded Frame-owned epoch validates",
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
    if (
        arguments.require_structural_epoch_comembership
        and not result["structural_epoch_comembership_available"]
    ):
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
