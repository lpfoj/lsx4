#!/usr/bin/env python3
"""Fail-closed parsers and validators for Bloodborne performance evidence.

This module has no adb side effects.  It consumes immutable artifacts created
by a capture runner and refuses to derive ratios from partial, stale, mixed or
unsynchronised data.  In particular, native-current-JIT counters are never a
valid denominator for an effective FEX run.
"""

from __future__ import annotations

import csv
import dataclasses
import hashlib
import json
import math
import pathlib
import re
import statistics
from collections.abc import Iterable, Mapping, Sequence
from typing import Any


SCHEMA = "lsx4-bloodborne-proof-window/v1"
CAMPAIGN_SCHEMA = "lsx4-bloodborne-proof-campaign/v1"
PROOF_SNAPSHOT_SCHEMA = "lsx4-proof-metrics-snapshot/v2"
HUD_COUNTER_SCHEMA = "lsx4-runtime-hud-counters/v1"

REQUIRED_PMU_EVENTS = (
    "instructions:u",
    "cpu-cycles:u",
    "raw-l1i-cache-refill:u",
    "raw-br-mis-pred-retired:u",
    "page-faults:u",
)

FEX_PROOF_METRIC_BITS = {
    "executedGuestInstructionEntries": 1 << 0,
    "executedIrBlockEntries": 1 << 1,
    "executedDispatcherEntries": 1 << 2,
    "compiledGuestInstructionSites": 1 << 3,
    "compiledGuestBytes": 1 << 4,
    "publishedNativeBlockBytes": 1 << 5,
    "compiledSpillSlots": 1 << 6,
    "compiledSpillSites": 1 << 7,
    "compiledFillSites": 1 << 8,
    "compiledHelperSites": 1 << 9,
    "compiledFlagSites": 1 << 10,
    "compiledFenceSites": 1 << 11,
    "compiledDmbSites": 1 << 12,
    "translationUnits": 1 << 13,
    "compiledIrBlocks": 1 << 14,
    "publishedNativeBlocks": 1 << 15,
    "compiledExplicitHelperSites": 1 << 16,
    "compiledExplicitFenceDmbSites": 1 << 17,
}
FEX_PROOF_REQUESTED_MASK = (1 << len(FEX_PROOF_METRIC_BITS)) - 1
FEX_PROOF_DYNAMIC_FIELDS = {"executedIrBlockEntries"}
FEX_PROOF_UNAVAILABLE_TOTALS = {
    "executedGuestInstructionEntries",
    "executedDispatcherEntries",
    "compiledGuestBytes",
    "compiledHelperSites",
    "compiledDmbSites",
}

RUNTIME_PROOF_METRIC_NAMES = (
    "guestInvalidationRequests",
    "guestInvalidationCompletions",
    "guestInvalidationWaitNs",
    "ownedGeneratedCodeWriteFaults",
    "fastmemMisses",
)
RUNTIME_PROOF_METRIC_BITS = {
    name: (1 << index)
    for index, name in enumerate(RUNTIME_PROOF_METRIC_NAMES)
}
RUNTIME_PROOF_REQUESTED_MASK = (1 << len(RUNTIME_PROOF_METRIC_BITS)) - 1
RUNTIME_PROOF_SUPPORTED_MASK = (
    RUNTIME_PROOF_REQUESTED_MASK
    & ~RUNTIME_PROOF_METRIC_BITS["fastmemMisses"]
)
RUNTIME_PROOF_REQUIRED_DYNAMIC_MASK = (
    RUNTIME_PROOF_METRIC_BITS["guestInvalidationRequests"]
    | RUNTIME_PROOF_METRIC_BITS["guestInvalidationCompletions"]
    | RUNTIME_PROOF_METRIC_BITS["guestInvalidationWaitNs"]
)
RUNTIME_PROOF_ATTEMPTED_MASK = (
    RUNTIME_PROOF_METRIC_BITS["guestInvalidationRequests"]
    | RUNTIME_PROOF_METRIC_BITS["ownedGeneratedCodeWriteFaults"]
)

SIMPLEPERF_FATAL = re.compile(
    r"(?:^|\s)(?:simpleperf\s+)?[EFW]\s|"
    r"not supported|not counted|permission denied|operation not permitted|"
    r"running child command.*isn't allowed|failed to|cannot |error:",
    re.IGNORECASE,
)

SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
PROOF_TOKEN_RE = re.compile(r"^[A-Za-z0-9._-]{1,80}$")


class ProofRejected(RuntimeError):
    """Raised when an artifact cannot support the claimed conclusion."""

    def __init__(self, reason: str, *, detail: str | None = None):
        self.reason = reason
        self.detail = detail
        message = reason if detail is None else f"{reason}: {detail}"
        super().__init__(message)


def _reject(condition: bool, reason: str, detail: str | None = None) -> None:
    if condition:
        raise ProofRejected(reason, detail=detail)


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_json_sha256(value: Any) -> str:
    payload = json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def parse_simpleperf_csv(
    stdout: str,
    stderr: str,
    required_events: Sequence[str] = REQUIRED_PMU_EVENTS,
) -> dict[str, Any]:
    """Parse one simpleperf ``stat --csv`` window.

    Any diagnostic on stderr is retained, but known warning/error forms reject
    the whole window.  Missing, duplicated, multiplexed-to-zero, unsupported or
    non-numeric events are also fatal.  This deliberately rejects all historic
    LSX4 captures affected by simpleperf's app-uid child-command error.
    """

    bad_stderr = [line.strip() for line in stderr.splitlines() if line.strip()]
    fatal_lines = [line for line in bad_stderr if SIMPLEPERF_FATAL.search(line)]
    _reject(bool(fatal_lines), "simpleperf-stderr-error", " | ".join(fatal_lines))

    counts: dict[str, int] = {}
    total_seconds: float | None = None
    for row in csv.reader(stdout.splitlines()):
        if not row:
            continue
        row = [cell.strip() for cell in row]
        if row[0] == "Total test time":
            _reject(len(row) < 3, "simpleperf-total-time-malformed", repr(row))
            try:
                total_seconds = float(row[1])
            except ValueError as exc:
                raise ProofRejected("simpleperf-total-time-nonnumeric", detail=row[1]) from exc
            continue
        if row[0].startswith("Performance counter statistics"):
            continue
        if len(row) < 2:
            continue
        raw_count, event = row[0].replace(" ", ""), row[1]
        if event not in required_events:
            continue
        _reject(event in counts, "simpleperf-duplicate-event", event)
        _reject(
            raw_count in {"", "<notcounted>", "<notsupported>"},
            "simpleperf-event-unavailable",
            event,
        )
        try:
            count = int(raw_count)
        except ValueError as exc:
            raise ProofRejected(
                "simpleperf-event-nonnumeric", detail=f"{event}={raw_count}"
            ) from exc
        _reject(count < 0, "simpleperf-negative-event", event)
        counts[event] = count

    missing = [event for event in required_events if event not in counts]
    _reject(bool(missing), "simpleperf-missing-events", ",".join(missing))
    _reject(total_seconds is None, "simpleperf-total-time-missing")
    assert total_seconds is not None
    _reject(not math.isfinite(total_seconds) or total_seconds <= 0,
            "simpleperf-total-time-invalid", str(total_seconds))
    _reject(counts["instructions:u"] <= 0, "simpleperf-zero-host-instructions")
    _reject(counts["cpu-cycles:u"] <= 0, "simpleperf-zero-cycles")
    return {
        "events": counts,
        "totalSeconds": total_seconds,
        "stderrLines": bad_stderr,
    }


def _proof_int(value: Mapping[str, Any], name: str) -> int:
    raw = value.get(name)
    _reject(isinstance(raw, bool) or not isinstance(raw, int),
            "fex-proof-field-invalid", name)
    _reject(raw < 0, "fex-proof-field-negative", name)
    return raw


def parse_fex_proof_snapshot(value: Mapping[str, Any]) -> dict[str, Any]:
    """Validate the adapter-owned proof ABI without inventing missing metrics."""

    _reject(value.get("schema") != "lsx4-fex-proof-metrics/v1",
            "fex-proof-schema-invalid", str(value.get("schema")))
    _reject(value.get("source") != "fex-runtime-diagnostic-v1",
            "fex-proof-source-invalid", str(value.get("source")))
    _reject(value.get("effectiveBackend") not in {"FEX", "fex-experimental"},
            "fex-proof-backend-not-fex", str(value.get("effectiveBackend")))
    _reject(value.get("queryStatus") != 0 or value.get("status") != 0,
            "fex-proof-query-failed")
    _reject(_proof_int(value, "abiVersion") != 1, "fex-proof-abi-invalid")
    _reject(_proof_int(value, "structSize") < 256, "fex-proof-struct-truncated")
    before_ns = _proof_int(value, "bootBeforeNs")
    after_ns = _proof_int(value, "bootAfterNs")
    _reject(before_ns == 0 or after_ns < before_ns, "fex-proof-clock-invalid")

    mode = value.get("instrumentationState")
    _reject(mode not in {"off", "static", "dynamic"},
            "fex-proof-mode-invalid", str(mode))
    requested = _proof_int(value, "requestedMask")
    valid = _proof_int(value, "validMask")
    unavailable = _proof_int(value, "unavailableMask")
    exact = _proof_int(value, "exactMask")
    sampled = _proof_int(value, "sampledMask")
    static = _proof_int(value, "staticMask")
    dynamic = _proof_int(value, "dynamicMask")
    perturbed = _proof_int(value, "perturbedMask")
    attempted = _proof_int(value, "attemptedNotRetiredMask")
    _reject(requested != FEX_PROOF_REQUESTED_MASK,
            "fex-proof-requested-mask-drift", str(requested))
    _reject((valid & unavailable) != 0 or (valid | unavailable) != requested,
            "fex-proof-availability-partition-invalid")
    _reject((valid & ~exact) != 0, "fex-proof-valid-not-exact")
    _reject(sampled != 0, "fex-proof-sampled-metric-present")
    _reject((static | dynamic) != valid or (static & dynamic) != 0,
            "fex-proof-kind-partition-invalid")
    _reject((perturbed | attempted) & ~valid,
            "fex-proof-semantic-mask-outside-valid")

    values = value.get("values")
    _reject(not isinstance(values, Mapping), "fex-proof-values-missing")
    parsed_values = {name: _proof_int(values, name) for name in FEX_PROOF_METRIC_BITS}
    unavailable_fields = {
        name for name, bit in FEX_PROOF_METRIC_BITS.items() if unavailable & bit
    }
    valid_fields = {
        name for name, bit in FEX_PROOF_METRIC_BITS.items() if valid & bit
    }
    exact_fields = {
        name for name, bit in FEX_PROOF_METRIC_BITS.items() if exact & bit
    }
    static_fields = {
        name for name, bit in FEX_PROOF_METRIC_BITS.items() if static & bit
    }
    dynamic_fields = {
        name for name, bit in FEX_PROOF_METRIC_BITS.items() if dynamic & bit
    }
    perturbed_fields = {
        name for name, bit in FEX_PROOF_METRIC_BITS.items() if perturbed & bit
    }
    _reject(not FEX_PROOF_UNAVAILABLE_TOTALS.issubset(unavailable_fields),
            "fex-proof-unprovable-total-was-advertised",
            ",".join(sorted(FEX_PROOF_UNAVAILABLE_TOTALS - unavailable_fields)))
    _reject(attempted != 0, "fex-proof-false-instruction-entry-claim")
    if mode == "off":
        _reject(valid != 0 or static != 0 or dynamic != 0 or perturbed != 0,
                "fex-proof-off-advertises-values")
    elif mode == "static":
        _reject(dynamic != 0 or perturbed != 0,
                "fex-proof-static-has-dynamic-values")
    else:
        block_bit = FEX_PROOF_METRIC_BITS["executedIrBlockEntries"]
        _reject((dynamic & block_bit) == 0 or (perturbed & block_bit) == 0,
                "fex-proof-dynamic-block-counter-missing")

    return {
        "mode": mode,
        "epoch": _proof_int(value, "epoch"),
        "sequence": _proof_int(value, "sequence"),
        "pid": _proof_int(value, "pid"),
        "bootBeforeNs": before_ns,
        "bootAfterNs": after_ns,
        "requestedMask": requested,
        "validMask": valid,
        "unavailableMask": unavailable,
        "exactMask": exact,
        "staticMask": static,
        "dynamicMask": dynamic,
        "perturbedMask": perturbed,
        "values": parsed_values,
        "validFields": sorted(valid_fields),
        "exactFields": sorted(exact_fields),
        "staticFields": sorted(static_fields),
        "dynamicFields": sorted(dynamic_fields),
        "perturbedFields": sorted(perturbed_fields),
        "unavailableFields": sorted(unavailable_fields),
    }


def fex_proof_delta(start: Mapping[str, Any], end: Mapping[str, Any]) -> dict[str, Any]:
    lhs = parse_fex_proof_snapshot(start)
    rhs = parse_fex_proof_snapshot(end)
    for field in (
        "mode", "epoch", "pid", "requestedMask", "validMask",
        "unavailableMask", "exactMask", "staticMask", "dynamicMask",
        "perturbedMask",
    ):
        _reject(lhs[field] != rhs[field], "fex-proof-snapshot-mismatch", field)
    _reject(rhs["sequence"] <= lhs["sequence"], "fex-proof-sequence-stale")
    deltas: dict[str, int] = {}
    for name in lhs["validFields"]:
        delta = rhs["values"][name] - lhs["values"][name]
        _reject(delta < 0, "fex-proof-counter-regressed", name)
        deltas[name] = delta
    if lhs["mode"] == "dynamic":
        _reject(deltas.get("executedIrBlockEntries", 0) <= 0,
                "fex-proof-ir-block-denominator-zero")
    return {
        "mode": lhs["mode"],
        "values": deltas,
        "validFields": lhs["validFields"],
        "exactFields": lhs["exactFields"],
        "staticFields": lhs["staticFields"],
        "dynamicFields": lhs["dynamicFields"],
        "perturbedFields": lhs["perturbedFields"],
        "unavailableFields": lhs["unavailableFields"],
        "perturbedMask": lhs["perturbedMask"],
    }


def parse_runtime_counter_snapshot(value: Mapping[str, Any]) -> dict[str, Any]:
    _reject(value.get("schema") != "lsx4-runtime-proof-counters/v1",
            "runtime-counter-schema-invalid")
    _reject(value.get("source") != "funnel-runtime-diagnostic-v1",
            "runtime-counter-source-invalid")
    _reject(value.get("effectiveBackend") not in {"FEX", "fex-experimental"},
            "runtime-counter-backend-not-fex")
    _reject(value.get("queryStatus") != 0 or value.get("status") != 0,
            "runtime-counter-query-failed")
    _reject(_proof_int(value, "abiVersion") != 1, "runtime-counter-abi-invalid")
    _reject(_proof_int(value, "structSize") < 256,
            "runtime-counter-struct-truncated")
    before_ns = _proof_int(value, "bootBeforeNs")
    after_ns = _proof_int(value, "bootAfterNs")
    _reject(before_ns == 0 or after_ns < before_ns, "runtime-counter-clock-invalid")
    mode = value.get("instrumentationState")
    _reject(mode not in {"off", "dynamic-diagnostic"},
            "runtime-counter-mode-invalid", str(mode))

    requested = _proof_int(value, "requestedMask")
    valid = _proof_int(value, "validMask")
    unavailable = _proof_int(value, "unavailableMask")
    exact = _proof_int(value, "exactMask")
    sampled = _proof_int(value, "sampledMask")
    static = _proof_int(value, "staticMask")
    dynamic = _proof_int(value, "dynamicMask")
    perturbed = _proof_int(value, "perturbedMask")
    attempted = _proof_int(value, "attemptedNotRetiredMask")
    saturated = _proof_int(value, "saturated")
    _reject(requested != RUNTIME_PROOF_REQUESTED_MASK,
            "runtime-counter-requested-mask-drift", str(requested))
    _reject((valid & unavailable) != 0 or (valid | unavailable) != requested,
            "runtime-counter-availability-partition-invalid")
    _reject((valid & ~exact) != 0, "runtime-counter-valid-not-exact")
    _reject(sampled != 0 or static != 0, "runtime-counter-kind-invalid")
    _reject(dynamic != valid, "runtime-counter-dynamic-mask-invalid")
    _reject((perturbed | attempted) & ~valid,
            "runtime-counter-semantic-mask-outside-valid")
    _reject(saturated != 0, "runtime-counter-saturated")

    values = value.get("values")
    _reject(not isinstance(values, Mapping), "runtime-counters-missing")
    parsed_values = {
        name: _proof_int(values, name) for name in RUNTIME_PROOF_METRIC_BITS
    }
    valid_fields = {
        name for name, bit in RUNTIME_PROOF_METRIC_BITS.items() if valid & bit
    }
    unavailable_fields = {
        name for name, bit in RUNTIME_PROOF_METRIC_BITS.items()
        if unavailable & bit
    }
    _reject("fastmemMisses" not in unavailable_fields,
            "runtime-counter-fastmem-falsely-advertised")
    for name in unavailable_fields:
        _reject(parsed_values[name] != 0,
                "runtime-counter-unavailable-nonzero", name)

    epoch = _proof_int(value, "epoch")
    sequence = _proof_int(value, "sequence")
    capture_flags = _proof_int(value, "captureFlags")
    if mode == "off":
        _reject(valid != 0 or dynamic != 0 or perturbed != 0 or attempted != 0,
                "runtime-counter-off-advertises-values")
        _reject(unavailable != requested or capture_flags != 0,
                "runtime-counter-off-contract-invalid")
        _reject(epoch != 0 or sequence != 0,
                "runtime-counter-off-has-live-epoch")
    else:
        _reject((valid & RUNTIME_PROOF_REQUIRED_DYNAMIC_MASK) !=
                RUNTIME_PROOF_REQUIRED_DYNAMIC_MASK or
                (valid & ~RUNTIME_PROOF_SUPPORTED_MASK) != 0,
                "runtime-counter-supported-mask-invalid", str(valid))
        _reject(perturbed != valid,
                "runtime-counter-perturbation-mask-invalid")
        _reject(attempted != (RUNTIME_PROOF_ATTEMPTED_MASK & valid),
                "runtime-counter-attempted-mask-invalid")
        expected_capture_flags = 0x3
        if "ownedGeneratedCodeWriteFaults" in valid_fields:
            expected_capture_flags |= 0x4
        _reject(capture_flags != expected_capture_flags,
                "runtime-counter-capture-flags-invalid",
                f"{capture_flags} != {expected_capture_flags}")
        _reject(epoch == 0 or sequence == 0,
                "runtime-counter-live-identity-missing")
    return {
        "mode": mode,
        "epoch": epoch,
        "sequence": sequence,
        "pid": _proof_int(value, "pid"),
        "bootBeforeNs": before_ns,
        "bootAfterNs": after_ns,
        "requestedMask": requested,
        "validMask": valid,
        "unavailableMask": unavailable,
        "perturbedMask": perturbed,
        "values": parsed_values,
        "validFields": sorted(valid_fields),
        "unavailableFields": sorted(unavailable_fields),
    }


def runtime_counter_delta(start: Mapping[str, Any], end: Mapping[str, Any]) -> dict[str, Any]:
    lhs = parse_runtime_counter_snapshot(start)
    rhs = parse_runtime_counter_snapshot(end)
    for field in ("mode", "epoch", "pid", "requestedMask", "validMask", "unavailableMask"):
        _reject(lhs[field] != rhs[field], "runtime-counter-snapshot-mismatch", field)
    if lhs["mode"] == "dynamic-diagnostic":
        _reject(rhs["sequence"] <= lhs["sequence"],
                "runtime-counter-sequence-stale")
    else:
        _reject(lhs["sequence"] != 0 or rhs["sequence"] != 0,
                "runtime-counter-off-sequence-invalid")
    deltas: dict[str, int] = {}
    for name in lhs["validFields"]:
        delta = rhs["values"][name] - lhs["values"][name]
        _reject(delta < 0, "runtime-counter-regressed", name)
        deltas[name] = delta
    return {
        "mode": lhs["mode"],
        "values": deltas,
        "validFields": lhs["validFields"],
        "unavailableFields": lhs["unavailableFields"],
        "perturbedMask": lhs["perturbedMask"],
    }


def parse_hud_counter_snapshot(value: Mapping[str, Any]) -> dict[str, Any]:
    """Validate exact cumulative runtime-HUD counters used only for scene gates."""

    _reject(value.get("schema") != HUD_COUNTER_SCHEMA, "hud-counter-schema-invalid")
    _reject(value.get("source") != "runtimeHudStats-exact-cumulative",
            "hud-counter-source-invalid")
    pid = _proof_int(value, "pid")
    before = _proof_int(value, "bootBeforeNs")
    after = _proof_int(value, "bootAfterNs")
    _reject(pid <= 0 or before <= 0 or after < before,
            "hud-counter-identity-invalid")
    expected_indices = {
        "generatedBlocks": 0, "drawCalls": 6,
        "submitCalls": 8, "presentCalls": 10,
    }
    _reject(value.get("indices") != expected_indices,
            "hud-counter-index-contract-invalid")
    count = _proof_int(value, "valueCount")
    raw = value.get("rawValues")
    counters = value.get("counters")
    _reject(count < 11 or not isinstance(raw, list) or len(raw) != count,
            "hud-counter-values-invalid")
    _reject(any(isinstance(item, bool) or not isinstance(item, int) or item < 0
                for item in raw), "hud-counter-value-invalid")
    _reject(not isinstance(counters, Mapping) or set(counters) != set(expected_indices),
            "hud-counter-map-invalid")
    for name, index in expected_indices.items():
        _reject(counters.get(name) != raw[index],
                "hud-counter-index-mismatch", name)
    return {
        "pid": pid,
        "bootBeforeNs": before,
        "bootAfterNs": after,
        "counters": dict(counters),
    }


def parse_combined_proof_snapshot(value: Mapping[str, Any]) -> dict[str, Any]:
    """Validate one receiver-produced, same-process composite snapshot."""

    _reject(value.get("schema") != PROOF_SNAPSHOT_SCHEMA,
            "proof-snapshot-schema-invalid")
    token = value.get("requestToken")
    _reject(not isinstance(token, str) or PROOF_TOKEN_RE.fullmatch(token) is None,
            "proof-snapshot-token-invalid")
    pid = _proof_int(value, "pid")
    process_starttime = _proof_int(value, "processStarttime")
    boot_before = _proof_int(value, "bootBeforeNs")
    boot_after = _proof_int(value, "bootAfterNs")
    _reject(pid <= 0 or process_starttime <= 0,
            "proof-snapshot-process-identity-invalid")
    _reject(boot_before == 0 or boot_after < boot_before,
            "proof-snapshot-clock-invalid")
    fex_raw = value.get("fex")
    runtime_raw = value.get("runtime")
    hud_raw = value.get("hud")
    _reject(not isinstance(fex_raw, Mapping) or not isinstance(runtime_raw, Mapping)
            or not isinstance(hud_raw, Mapping),
            "proof-snapshot-payload-missing")
    fex = parse_fex_proof_snapshot(fex_raw)
    runtime = parse_runtime_counter_snapshot(runtime_raw)
    hud = parse_hud_counter_snapshot(hud_raw)
    for name, nested in (("fex", fex), ("runtime", runtime), ("hud", hud)):
        _reject(nested["pid"] != pid, "proof-snapshot-nested-pid-mismatch", name)
        _reject(nested["bootBeforeNs"] < boot_before or
                nested["bootAfterNs"] > boot_after,
                "proof-snapshot-nested-clock-outside-envelope", name)
    return {
        "requestToken": token,
        "pid": pid,
        "processStarttime": process_starttime,
        "bootBeforeNs": boot_before,
        "bootAfterNs": boot_after,
        "fexRaw": fex_raw,
        "runtimeRaw": runtime_raw,
        "hudRaw": hud_raw,
        "fex": fex,
        "runtime": runtime,
        "hud": hud,
    }


def combined_proof_delta(start: Mapping[str, Any], end: Mapping[str, Any]) -> dict[str, Any]:
    lhs = parse_combined_proof_snapshot(start)
    rhs = parse_combined_proof_snapshot(end)
    for field in ("pid", "processStarttime"):
        _reject(lhs[field] != rhs[field], "proof-snapshot-pair-mismatch", field)
    _reject(lhs["requestToken"] == rhs["requestToken"],
            "proof-snapshot-token-reused")
    _reject(rhs["bootBeforeNs"] <= lhs["bootAfterNs"],
            "proof-snapshot-order-invalid")
    _reject(rhs["hud"]["bootBeforeNs"] <= lhs["hud"]["bootAfterNs"],
            "hud-counter-snapshot-order-invalid")
    hud_deltas = {
        name: rhs["hud"]["counters"][name] - lhs["hud"]["counters"][name]
        for name in lhs["hud"]["counters"]
    }
    _reject(any(value < 0 for value in hud_deltas.values()),
            "hud-counter-regressed")
    return {
        "pid": lhs["pid"],
        "processStarttime": lhs["processStarttime"],
        "bootStartNs": lhs["bootBeforeNs"],
        "bootEndNs": rhs["bootAfterNs"],
        "fex": fex_proof_delta(lhs["fexRaw"], rhs["fexRaw"]),
        "runtime": runtime_counter_delta(lhs["runtimeRaw"], rhs["runtimeRaw"]),
        "hud": {
            "values": hud_deltas,
            "fpsVerdictAllowed": False,
        },
    }


def _percentile_nearest_rank(values: Sequence[float], percentile: float) -> float:
    _reject(not values, "frametime-empty")
    ordered = sorted(values)
    rank = max(1, math.ceil(percentile * len(ordered)))
    return ordered[rank - 1]


def parse_surfaceflinger_latency(text: str) -> dict[str, Any]:
    """Parse ``dumpsys SurfaceFlinger --latency <exact-layer>`` output."""

    rows = [line.strip() for line in text.splitlines() if line.strip()]
    _reject(not rows, "sf-latency-empty")
    try:
        refresh_period_ns = int(rows[0])
    except ValueError as exc:
        raise ProofRejected("sf-refresh-period-invalid", detail=rows[0]) from exc
    _reject(refresh_period_ns <= 0, "sf-refresh-period-invalid", str(refresh_period_ns))

    presents: list[int] = []
    for line in rows[1:]:
        fields = line.split()
        if len(fields) != 3:
            continue
        try:
            desired, actual, presented = (int(field) for field in fields)
        except ValueError:
            continue
        if 0 in {desired, actual, presented}:
            continue
        if max(desired, actual, presented) >= (1 << 63) - 1:
            continue
        presents.append(presented)

    _reject(len(presents) < 3, "sf-latency-insufficient-frames", str(len(presents)))
    _reject(any(b <= a for a, b in zip(presents, presents[1:])),
            "sf-latency-nonmonotonic")
    intervals_ms = [(b - a) / 1_000_000.0 for a, b in zip(presents, presents[1:])]
    _reject(any(value <= 0 or not math.isfinite(value) for value in intervals_ms),
            "sf-frametime-invalid")
    total_seconds = (presents[-1] - presents[0]) / 1_000_000_000.0
    _reject(total_seconds <= 0, "sf-window-duration-invalid")
    p50 = _percentile_nearest_rank(intervals_ms, 0.50)
    p95 = _percentile_nearest_rank(intervals_ms, 0.95)
    p99 = _percentile_nearest_rank(intervals_ms, 0.99)
    one_percent_low_fps = 1000.0 / p99
    return {
        "refreshPeriodNs": refresh_period_ns,
        "firstPresentNs": presents[0],
        "lastPresentNs": presents[-1],
        "presentedFrames": len(presents),
        "intervalCount": len(intervals_ms),
        "durationSeconds": total_seconds,
        "averageFps": len(intervals_ms) / total_seconds,
        "p50Ms": p50,
        "p95Ms": p95,
        "p99Ms": p99,
        "onePercentLowFps": one_percent_low_fps,
        "over40ms": sum(value > 40.0 for value in intervals_ms),
        "over100ms": sum(value > 100.0 for value in intervals_ms),
        "maxMs": max(intervals_ms),
        "intervalsMs": intervals_ms,
    }


def validate_surface_identity(value: Mapping[str, Any]) -> None:
    required = ("layerName", "layerId", "generation", "uid", "displayId")
    for name in required:
        _reject(value.get(name) in {None, ""}, "surface-identity-missing", name)
    layer_name = str(value["layerName"])
    _reject("app.lsx4.android" not in layer_name or "GameActivity" not in layer_name,
            "surface-identity-not-game", layer_name)


def validate_window_boundaries(value: Mapping[str, Any]) -> None:
    _reject(value.get("schema") != SCHEMA, "window-schema-invalid")
    for name in ("runId", "windowId", "phase", "pid", "processStarttime"):
        _reject(value.get(name) in {None, ""}, "window-identity-missing", name)
    boundary = value.get("boundary")
    _reject(not isinstance(boundary, Mapping), "window-boundary-missing")
    required = (
        "deviceBootStartNs",
        "deviceBootEndNs",
        "hostMonotonicStartNs",
        "hostMonotonicEndNs",
    )
    parsed: dict[str, int] = {}
    for name in required:
        raw = boundary.get(name)
        _reject(isinstance(raw, bool) or not isinstance(raw, int),
                "window-boundary-invalid", name)
        parsed[name] = raw
    _reject(parsed["deviceBootEndNs"] <= parsed["deviceBootStartNs"],
            "window-device-time-invalid")
    _reject(parsed["hostMonotonicEndNs"] <= parsed["hostMonotonicStartNs"],
            "window-host-time-invalid")
    device_duration = parsed["deviceBootEndNs"] - parsed["deviceBootStartNs"]
    host_duration = parsed["hostMonotonicEndNs"] - parsed["hostMonotonicStartNs"]
    skew = abs(device_duration - host_duration)
    allowed = max(50_000_000, int(host_duration * 0.005))
    _reject(skew > allowed, "window-clock-duration-skew", f"{skew}>{allowed}")


PROVENANCE_EQUAL_PATHS = (
    "device.serial",
    "device.soc",
    "device.board",
    "build.gitCommit",
    "build.apkSha256",
    "build.apkVersionCode",
    "build.signerSha256",
    "libraries.executorSha256",
    "libraries.fexAdapterSha256",
    "libraries.fexBridgeSha256",
    "libraries.fexCoreSha256",
    "backend.requested",
    "backend.effective",
    "backend.fallbackReason",
    "vulkan.requested",
    "vulkan.effective",
    "vulkan.vendorId",
    "vulkan.deviceId",
    "vulkan.driverVersion",
    "vulkan.driverBuild",
    "vulkan.icdSha256",
    "vulkan.pipelineCacheUuid",
    "settings.sha256",
    "patches.sha256",
    "route.sha256",
    "input.sha256",
    "save.sha256",
    "cache.seedSha256",
    "cache.pipelineInventorySha256",
    "cache.fexInventorySha256",
    "display.mode",
    "display.refreshHz",
    "pmuCapabilities.sha256",
)


def _nested_get(value: Mapping[str, Any], path: str) -> Any:
    current: Any = value
    for part in path.split("."):
        if not isinstance(current, Mapping) or part not in current:
            raise ProofRejected("provenance-field-missing", detail=path)
        current = current[part]
    return current


def validate_provenance(value: Mapping[str, Any]) -> None:
    for path in PROVENANCE_EQUAL_PATHS:
        current = _nested_get(value, path)
        _reject(current in {None, "", "unknown"}, "provenance-field-empty", path)
        if path.endswith("Sha256") or path.endswith("sha256"):
            _reject(not isinstance(current, str) or not SHA256_RE.fullmatch(current),
                    "provenance-hash-invalid", path)
    _reject(_nested_get(value, "device.serial") != "10AG263F1B001HM",
            "provenance-wrong-device")
    _reject(_nested_get(value, "backend.effective") != "FEX",
            "provenance-effective-backend-not-fex")
    capabilities = value.get("pmuCapabilities")
    _reject(not isinstance(capabilities, Mapping), "pmu-capabilities-missing")
    _reject(capabilities.get("l1iValidated") is not True,
            "pmu-l1i-not-validated")
    _reject(capabilities.get("branchValidated") is not True,
            "pmu-branch-not-validated")
    if capabilities.get("dmbPmuValidated") is not True:
        _reject(capabilities.get("dmbMetricSource") != "unavailable",
                "dmb-metric-source-falsely-advertised")


def compare_provenance(
    reference: Mapping[str, Any],
    candidate: Mapping[str, Any],
    *,
    allowed_differences: Iterable[str] = (),
) -> None:
    validate_provenance(reference)
    validate_provenance(candidate)
    allowed = set(allowed_differences)
    for path in PROVENANCE_EQUAL_PATHS:
        if path in allowed:
            continue
        lhs, rhs = _nested_get(reference, path), _nested_get(candidate, path)
        _reject(lhs != rhs, "provenance-mismatch", f"{path}: {lhs!r} != {rhs!r}")

    ref_markers = reference.get("markers")
    candidate_markers = candidate.get("markers")
    _reject(not isinstance(ref_markers, Mapping) or not isinstance(candidate_markers, Mapping),
            "provenance-markers-missing")
    marker_deltas = {
        name
        for name in set(ref_markers) | set(candidate_markers)
        if ref_markers.get(name) != candidate_markers.get(name)
    }
    unexpected = marker_deltas - allowed
    _reject(bool(unexpected), "provenance-marker-mismatch", ",".join(sorted(unexpected)))


def derive_metrics(
    pmu: Mapping[str, Any],
    frames: Mapping[str, Any],
    runtime: Mapping[str, Any],
    fex: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    events = pmu["events"]
    frame_count = frames["intervalCount"]
    _reject(frame_count <= 0, "frame-denominator-zero")
    host_instructions = events["instructions:u"]
    _reject(host_instructions <= 0, "host-instruction-denominator-zero")
    host_cycles = events["cpu-cycles:u"]
    host_l1i_misses = events["raw-l1i-cache-refill:u"]
    host_branch_misses = events["raw-br-mis-pred-retired:u"]
    total_seconds = pmu["totalSeconds"]
    result: dict[str, Any] = {





        "hostInstructions": host_instructions,
        "hostCycles": host_cycles,
        "hostInstructionsPerSecond": host_instructions / total_seconds,
        "hostCyclesPerSecond": host_cycles / total_seconds,
        "hostInstructionsPerFrame": host_instructions / frame_count,
        "hostCyclesPerFrame": host_cycles / frame_count,
        "hostCyclesPerInstruction": host_cycles / host_instructions,
        "hostInstructionsPerCycle": host_instructions / host_cycles,
        "hostL1iMisses": host_l1i_misses,
        "hostBranchMisses": host_branch_misses,
        "hostL1iMissesPerSecond": host_l1i_misses / total_seconds,
        "hostBranchMissesPerSecond": host_branch_misses / total_seconds,
        "hostL1iMissesPerFrame": host_l1i_misses / frame_count,
        "hostBranchMissesPerFrame": host_branch_misses / frame_count,
        "l1iMissesPerMillionHostInstructions":
            host_l1i_misses * 1_000_000.0 / host_instructions,
        "branchMissesPerMillionHostInstructions":
            host_branch_misses * 1_000_000.0 / host_instructions,
        "pageFaultsPerFrame": events["page-faults:u"] / frame_count,
        "unavailable": [
            "cyclesPerGuestInstruction",
            "l1iMissesPerGuestInstruction",
            "branchMissesPerGuestInstruction",
            "dispatcherEntriesPerGuestInstruction",
            "genericHelperCallsPerGuestInstruction",
            "executedDmbPerGuestInstruction",
        ],
    }
    runtime_values = runtime.get("values", {})
    runtime_metric_names = {
        "guestInvalidationRequests": "invalidationRequestsPerFrame",
        "guestInvalidationCompletions": "invalidationCompletionsPerFrame",
        "ownedGeneratedCodeWriteFaults": "ownedGeneratedCodeWriteFaultsPerFrame",
    }
    for field, metric_name in runtime_metric_names.items():
        if field in runtime_values:
            result[metric_name] = runtime_values[field] / frame_count
        else:
            result["unavailable"].append(metric_name)
    if "guestInvalidationWaitNs" in runtime_values:
        result["invalidationWaitMsPerFrame"] = (
            runtime_values["guestInvalidationWaitNs"] / 1_000_000.0 / frame_count
        )
    else:
        result["unavailable"].append("invalidationWaitMsPerFrame")
    result["unavailable"].append("fastmemMissesPerFrame")




    static_ratio_specs = (
        ("staticNativeBytesPerCompiledGuestInstructionSite",
         ("publishedNativeBlockBytes",), "compiledGuestInstructionSites"),
        ("staticSpillFillSitesPerCompiledGuestInstructionSite",
         ("compiledSpillSites", "compiledFillSites"),
         "compiledGuestInstructionSites"),
        ("staticAllocatedSpillSlotsPerCompiledGuestInstructionSite",
         ("compiledSpillSlots",), "compiledGuestInstructionSites"),
        ("staticAllocatedSpillSlotsPerTranslationUnit",
         ("compiledSpillSlots",), "translationUnits"),
        ("staticFlagSitesPerCompiledGuestInstructionSite",
         ("compiledFlagSites",), "compiledGuestInstructionSites"),
        ("staticExplicitHelperSitesPerCompiledGuestInstructionSite",
         ("compiledExplicitHelperSites",), "compiledGuestInstructionSites"),
        ("staticExplicitFenceDmbSitesPerCompiledGuestInstructionSite",
         ("compiledExplicitFenceDmbSites",),
         "compiledGuestInstructionSites"),
        ("staticIrBlocksPerTranslationUnit",
         ("compiledIrBlocks",), "translationUnits"),
        ("staticNativeBytesPerPublishedBlock",
         ("publishedNativeBlockBytes",), "publishedNativeBlocks"),
    )
    if fex is not None and fex.get("mode") == "static":
        eligible_fields = (
            set(fex.get("validFields", ())) &
            set(fex.get("exactFields", ())) &
            set(fex.get("staticFields", ()))
        )
        fex_values = fex.get("values", {})
        for metric_name, numerators, denominator in static_ratio_specs:
            required_fields = set(numerators) | {denominator}
            denominator_value = fex_values.get(denominator)
            if (not required_fields.issubset(eligible_fields) or
                    isinstance(denominator_value, bool) or
                    not isinstance(denominator_value, int) or
                    denominator_value <= 0):
                result["unavailable"].append(metric_name)
                continue
            result[metric_name] = (
                sum(int(fex_values[name]) for name in numerators) /
                denominator_value
            )
        result["staticCompileShapeMetricSource"] = "exact-static-fex-delta"
        result["staticCompileShapeRatiosProductionEligible"] = False
        result["staticCompileShapeRatiosPerturbed"] = False
        result["staticSpillSlotSemantics"] = (
            "sum-of-post-ra-header-spill-slots-not-executed-spills"
        )
    else:
        result["unavailable"].extend(
            metric_name for metric_name, _, _ in static_ratio_specs
        )

    dynamic_ratio_names = (
        "instrumentedCyclesPerIrBlockEntry",
        "instrumentedHostInstructionsPerIrBlockEntry",
        "instrumentedL1iMissesPerKIrBlockEntries",
        "instrumentedBranchMissesPerKIrBlockEntries",
    )
    if fex is not None and fex.get("mode") == "dynamic":
        denominator_field = "executedIrBlockEntries"
        denominator_sets = (
            set(fex.get("validFields", ())),
            set(fex.get("exactFields", ())),
            set(fex.get("dynamicFields", ())),
            set(fex.get("perturbedFields", ())),
        )
        _reject(any(denominator_field not in fields for fields in denominator_sets),
                "fex-proof-ir-block-semantic-contract-invalid")
        block_entries = fex["values"].get("executedIrBlockEntries", 0)
        _reject(block_entries <= 0, "fex-proof-ir-block-denominator-zero")
        result["instrumentedCyclesPerIrBlockEntry"] = (
            host_cycles / block_entries
        )
        result["instrumentedHostInstructionsPerIrBlockEntry"] = (
            host_instructions / block_entries
        )
        result["instrumentedL1iMissesPerKIrBlockEntries"] = (
            host_l1i_misses * 1000.0 / block_entries
        )
        result["instrumentedBranchMissesPerKIrBlockEntries"] = (
            host_branch_misses * 1000.0 / block_entries
        )
        result["instrumentedRatioMetricSource"] = (
            "simpleperf-host-events/exact-dynamic-fex-ir-block-entries-"
            "bounded-counter-envelope"
        )
        result["instrumentedRatioWindowAlignment"] = (
            "fex-counter-envelope-contains-pmu-window-not-identical"
        )
        result["instrumentedRatiosProductionEligible"] = False
        result["instrumentedRatiosPerturbed"] = True
    else:
        result["unavailable"].extend(dynamic_ratio_names)
    return result


def validate_overlap(value: Mapping[str, Any], window: Mapping[str, Any]) -> None:
    _reject(value.get("schema") != "lsx4-invalidation-overlap/v2",
            "overlap-not-causal-v2", str(value.get("schema")))
    _reject(value.get("windowId") != window.get("windowId"), "overlap-window-mismatch")
    _reject(value.get("pid") != window.get("pid"), "overlap-pid-mismatch")
    _reject(value.get("processStarttime") != window.get("processStarttime"),
            "overlap-starttime-mismatch")
    for field in ("droppedRecords", "uncommittedRecords", "saturatedRecords"):
        _reject(value.get(field) != 0, "overlap-incomplete", field)
    _reject(value.get("causalPairs", 0) <= 0, "overlap-no-causal-pairs")
    _reject(value.get("boundaryStartSequence") is None or
            value.get("boundaryEndSequence") is None,
            "overlap-boundary-sequence-missing")
    _reject(value["boundaryEndSequence"] <= value["boundaryStartSequence"],
            "overlap-boundary-sequence-invalid")


def validate_health(value: Mapping[str, Any]) -> None:
    for field in (
        "fatalSignals",
        "deviceLost",
        "blackFrames",
        "corruptFrames",
        "freezeGapsOver100ms",
        "audioContractFailures",
    ):
        _reject(value.get(field) != 0, "health-gate-failed", field)
    _reject(value.get("foreground") is not True, "health-not-foreground")
    _reject(value.get("tailProgress") is not True, "health-no-tail-progress")


def validate_heavy_route_frames(
    frames: Mapping[str, Any],
    *,
    minimum_intervals: int = 300,
    minimum_average_fps: float = 25.0,
    maximum_p99_ms: float = 40.0,
) -> None:
    _reject(frames.get("intervalCount", 0) < minimum_intervals,
            "heavy-route-too-short")
    _reject(frames.get("averageFps", 0.0) < minimum_average_fps,
            "heavy-route-average-below-25", str(frames.get("averageFps")))
    _reject(frames.get("p99Ms", math.inf) > maximum_p99_ms,
            "heavy-route-p99-over-40ms", str(frames.get("p99Ms")))
    _reject(frames.get("over100ms", 1) != 0,
            "heavy-route-freeze-over-100ms", str(frames.get("over100ms")))


def validate_window_bundle(bundle: Mapping[str, Any]) -> dict[str, Any]:
    window = bundle.get("window")
    _reject(not isinstance(window, Mapping), "window-missing")
    validate_window_boundaries(window)
    provenance = bundle.get("provenance")
    _reject(not isinstance(provenance, Mapping), "provenance-missing")
    validate_provenance(provenance)
    surface = bundle.get("surface")
    _reject(not isinstance(surface, Mapping), "surface-missing")
    validate_surface_identity(surface)
    _reject(surface.get("generation") != window.get("surfaceGeneration"),
            "surface-generation-window-mismatch")
    health = bundle.get("health")
    _reject(not isinstance(health, Mapping), "health-missing")
    validate_health(health)

    pmu_raw = bundle.get("pmu")
    _reject(not isinstance(pmu_raw, Mapping), "pmu-missing")
    pmu = parse_simpleperf_csv(
        str(pmu_raw.get("stdout", "")), str(pmu_raw.get("stderr", ""))
    )
    proof_start, proof_end = bundle.get("proofStart"), bundle.get("proofEnd")
    _reject(not isinstance(proof_start, Mapping) or not isinstance(proof_end, Mapping),
            "proof-snapshot-pair-missing")
    proof_delta = combined_proof_delta(proof_start, proof_end)
    _reject(proof_delta["pid"] != window.get("pid") or
            proof_delta["processStarttime"] != window.get("processStarttime"),
            "proof-snapshot-window-process-mismatch")
    runtime = proof_delta["runtime"]
    fex = proof_delta["fex"]
    claim_class = bundle.get("claimClass", "production")
    if claim_class == "production":
        _reject(fex["mode"] != "off" or runtime["mode"] != "off",
                "production-window-instrumented")
    elif claim_class == "diagnostic-fex-proof":
        _reject(fex["mode"] not in {"static", "dynamic"} or
                runtime["mode"] != "off",
                "diagnostic-fex-window-axis-invalid")
    elif claim_class in {"diagnostic-runtime-proof", "diagnostic-hle-proof"}:
        _reject(fex["mode"] != "off" or
                runtime["mode"] != "dynamic-diagnostic",
                "diagnostic-runtime-window-axis-invalid")
    elif claim_class == "diagnostic-overlap":
        _reject(fex["mode"] != "off" or runtime["mode"] != "off",
                "diagnostic-overlap-window-axis-invalid")
    else:
        raise ProofRejected("claim-class-invalid", detail=str(claim_class))
    latency = bundle.get("surfaceLatency")
    _reject(not isinstance(latency, str), "sf-latency-missing")
    frames = parse_surfaceflinger_latency(latency)
    boundary = window["boundary"]
    tolerance_ns = 100_000_000
    _reject(proof_delta["bootStartNs"] < boundary["deviceBootStartNs"] - tolerance_ns or
            proof_delta["bootEndNs"] > boundary["deviceBootEndNs"] + tolerance_ns,
            "proof-snapshot-outside-window")
    _reject(frames["firstPresentNs"] < boundary["deviceBootStartNs"] - tolerance_ns,
            "sf-frames-before-window")
    _reject(frames["lastPresentNs"] > boundary["deviceBootEndNs"] + tolerance_ns,
            "sf-frames-after-window")
    pmu_duration_ns = int(pmu["totalSeconds"] * 1_000_000_000)
    device_duration_ns = boundary["deviceBootEndNs"] - boundary["deviceBootStartNs"]
    _reject(abs(pmu_duration_ns - device_duration_ns) > max(150_000_000, int(device_duration_ns * 0.01)),
            "pmu-window-duration-mismatch")

    metrics = derive_metrics(pmu, frames, runtime, fex)
    overlap = bundle.get("overlap")
    if overlap is not None:
        _reject(not isinstance(overlap, Mapping), "overlap-invalid")
        validate_overlap(overlap, window)
    _reject(claim_class == "diagnostic-overlap" and overlap is None,
            "diagnostic-overlap-artifact-missing")
    return {
        "claimClass": claim_class,
        "pmu": pmu,
        "runtime": runtime,
        "fexProof": fex,
        "frames": frames,
        "metrics": metrics,
        "fullGuestMetricPanelProven": not bool(metrics["unavailable"]),
        "productionFrameTimeEligible": claim_class == "production",
    }


def validate_axis_campaign(campaign: Mapping[str, Any]) -> dict[str, Any]:
    _reject(campaign.get("schema") != CAMPAIGN_SCHEMA, "campaign-schema-invalid")
    axis = campaign.get("axis")
    allowed_axes = {
        "MemcpySetTSO": "run-enable-fex-relaxed-memcpyset-tso-ab",
        "VectorTSO": "run-enable-fex-relaxed-vector-tso-ab",
        "HleCleanMemset": "run-enable-hle-clean-buffer-fastpath",
    }
    _reject(axis not in allowed_axes, "campaign-axis-invalid", str(axis))
    order = campaign.get("order")
    _reject(order not in {"ABA", "BAB"}, "campaign-order-invalid", str(order))
    phases = campaign.get("phases")
    _reject(not isinstance(phases, Sequence) or isinstance(phases, (str, bytes)) or len(phases) != 3,
            "campaign-phase-count-invalid")

    parsed = [validate_window_bundle(phase) for phase in phases]
    expected = ["A", "B", "A"] if order == "ABA" else ["B", "A", "B"]
    actual = [phase["window"].get("variant") for phase in phases]
    _reject(actual != expected, "campaign-phase-order-mismatch", repr(actual))
    marker = allowed_axes[axis]
    for index in range(1, len(phases)):
        compare_provenance(
            phases[0]["provenance"],
            phases[index]["provenance"],
            allowed_differences={marker},
        )
    for phase, label in zip(phases, expected):
        markers = phase["provenance"]["markers"]
        expected_state = label == "B"
        _reject(markers.get(marker) is not expected_state,
                "campaign-axis-marker-state-invalid", f"{label}:{marker}")
        other_experiment_markers = {
            value for key, value in allowed_axes.items() if key != axis
        }
        enabled_other = [name for name in other_experiment_markers if markers.get(name)]
        _reject(bool(enabled_other), "campaign-confounded-marker", ",".join(enabled_other))

    return {"axis": axis, "order": order, "phases": parsed}


def load_and_validate(path: pathlib.Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        value = json.load(stream)
    _reject(not isinstance(value, Mapping), "top-level-not-object")
    if value.get("schema") == CAMPAIGN_SCHEMA:
        return validate_axis_campaign(value)
    return validate_window_bundle(value)


def main(argv: Sequence[str] | None = None) -> int:
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("artifact", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args(argv)
    try:
        result = load_and_validate(args.artifact)
    except (OSError, json.JSONDecodeError, ProofRejected) as exc:
        print(json.dumps({"accepted": False, "reason": str(exc)}, ensure_ascii=False))
        return 2
    payload = {"accepted": True, "result": result}
    encoded = json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True)
    if args.output:
        args.output.write_text(encoded + "\n", encoding="utf-8")
    print(encoded)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
