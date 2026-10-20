#!/usr/bin/env python3
"""Build and execute fail-closed Bloodborne proof campaigns on the Vivo.

The default operation is a side-effect-free dry run.  Device execution needs
``--execute`` and a predeclared expected-state document.  Every direct adb
invocation is guarded by an AgentChat event plus ownership checks for both the
logical Vivo and its physical serial.  The mandatory performance preflight is
run before every phase and again, in cleanup-only mode, immediately before the
measurement window.

This runner intentionally separates production frame-time evidence from FEX
static/dynamic instrumentation.  Instrumented captures are diagnostic only and
can never produce an FPS verdict.  It never clears a pipeline, shader, FEX, or
game cache; cache state is frozen by provenance instead.
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import hashlib
import importlib.util
import json
import os
import pathlib
import re
import struct
import subprocess
import sys
import time
import uuid
import zlib
from collections.abc import Mapping, Sequence
from typing import Any


PLAN_SCHEMA = "lsx4-bloodborne-device-campaign-plan/v1"
RESULT_SCHEMA = "lsx4-bloodborne-device-campaign-result/v1"
ROUTE_SCHEMA = "lsx4-bloodborne-proof-route/v1"
PROOF_SNAPSHOT_SCHEMA = "lsx4-proof-metrics-snapshot/v2"
HUD_COUNTER_SCHEMA = "lsx4-runtime-hud-counters/v1"
VISUAL_ANCHOR_SCHEMA = "lsx4-roi-dhash-scene-anchor/v1"
MOVEMENT_EVIDENCE_SCHEMA = "lsx4-roi-dhash-scene-change/v1"
EXPECTED_SERIAL = "10AG263F1B001HM"
PACKAGE = "app.lsx4.android"
GAME_PROCESS = f"{PACKAGE}:game"
PROOF_RECEIVER = f"{PACKAGE}/.ProofMetricsReceiver"
PROOF_ACTION = f"{PACKAGE}.CAPTURE_PROOF_METRICS"
PROOF_TOKEN_EXTRA = "proof_request_token"
PROOF_ROOT = "files/lsx4-home/runtime/proof-metrics"
MARKER_ROOT = "files/lsx4-home"
OVERLAP_CONTROL_ROOT = "files/lsx4-home/runtime/invalidation-overlap-control"

PMU_EVENTS = (
    "instructions:u",
    "cpu-cycles:u",
    "raw-l1i-cache-refill:u",
    "raw-br-mis-pred-retired:u",
    "page-faults:u",
)





MOVING_MEASUREMENT_SWIPE = {
    "kind": "swipe",
    "x1": 950,
    "y1": 1245,
    "x2": 950,
    "y2": 1060,
    "durationMilliseconds": 14_000,
}
DISPLAY_WIDTH = 3168
DISPLAY_HEIGHT = 1440
MOVING_ROUTE_RELATIVE_PATH = (
    "tools/bloodborne-proof/routes/hunters_dream_forward_swipe_14s.json"
)
MOVING_INPUT_RELATIVE_PATH = (
    "tools/bloodborne-proof/inputs/hunters_dream_forward_swipe_14s.json"
)

AXIS_MARKERS = {
    "MemcpySetTSO": "run-enable-fex-relaxed-memcpyset-tso-ab",
    "VectorTSO": "run-enable-fex-relaxed-vector-tso-ab",
    "HleCleanMemset": "run-enable-hle-clean-buffer-fastpath",
}





BASELINE_BACKEND_MEMORY_CONTRACT = {
    "configureFlags": 0,
    "providedCapabilities": 8191,
    "requiredCapabilities": 4095,
    "forbiddenCapabilities": 0,
    "strongMemoryModel": (
        "FEX software TSO (hardware TSO when enabled by the host OS)"
    ),
    "cacheNamespaceSuffix": "-lookup-throughput-sm-swtso",
}

MEMORY_AXIS_BACKEND_CONTRACTS = {
    "MemcpySetTSO": {
        "configureFlags": 2,
        "providedCapabilities": 8187,
        "requiredCapabilities": 8187,
        "forbiddenCapabilities": 4,
        "strongMemoryModel": "FEX software TSO (MemcpySetTSO relaxed A/B)",
        "cacheNamespaceSuffix": "-ab-mtso0-sm-swtso",
    },
    "VectorTSO": {
        "configureFlags": 1,
        "providedCapabilities": 8187,
        "requiredCapabilities": 8187,
        "forbiddenCapabilities": 4,
        "strongMemoryModel": "FEX software TSO (VectorTSO relaxed A/B)",
        "cacheNamespaceSuffix": "-ab-vtso0-sm-swtso",
    },
}

BACKEND_MEMORY_FIELDS = (
    "configureFlags",
    "providedCapabilities",
    "requiredCapabilities",
    "forbiddenCapabilities",
    "strongMemoryModel",
)

INSTRUMENTATION_MARKERS = {
    "static": "diag-fex-proof-metrics-static",
    "dynamic": "diag-fex-proof-metrics-dynamic",
}
RUNTIME_COUNTER_MARKER = "run-diagnostic-guest-invalidation-profile"

OVERLAP_MARKER = "run-profile-guest-invalidation-overlap"
MULTIBLOCK_MARKER = "run-enable-fex-multiblock"
BASELINE_FORBIDDEN_MARKERS = frozenset({
    "run-enable-fex-buffer-capture-ack",
    "run-enable-fex-deferred-write-epoch",
    "run-enable-bounded-guest-invalidation-drain",
    "run-enable-fex-relaxed-memcpyset-tso-ab",
    "run-enable-fex-relaxed-vector-tso-ab",
    "run-enable-hle-clean-buffer-fastpath",
    MULTIBLOCK_MARKER,
})
MANAGED_MARKERS = frozenset(
    (*AXIS_MARKERS.values(), *INSTRUMENTATION_MARKERS.values(),
     RUNTIME_COUNTER_MARKER, OVERLAP_MARKER, MULTIBLOCK_MARKER)
)

PROOF_METRIC_BITS = {
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

STATIC_REQUIRED_METRICS = frozenset({
    "compiledGuestInstructionSites",
    "publishedNativeBlockBytes",
    "compiledSpillSlots",
    "compiledSpillSites",
    "compiledFillSites",
    "compiledFlagSites",
    "compiledFenceSites",
    "translationUnits",
    "compiledIrBlocks",
    "publishedNativeBlocks",
    "compiledExplicitHelperSites",
    "compiledExplicitFenceDmbSites",
})
DYNAMIC_REQUIRED_METRICS = frozenset(
    set(STATIC_REQUIRED_METRICS) | {"executedIrBlockEntries"}
)
COMPLETE_PANEL_METRICS = frozenset({
    "executedGuestInstructionEntries",
    "executedIrBlockEntries",
    "executedDispatcherEntries",
    "compiledGuestBytes",
    "compiledSpillSites",
    "compiledFillSites",
    "compiledFlagSites",
    "compiledHelperSites",
    "compiledDmbSites",
})
RUNTIME_METRIC_BITS = {
    "guestInvalidationRequests": 1 << 0,
    "guestInvalidationCompletions": 1 << 1,
    "guestInvalidationWaitNs": 1 << 2,
    "ownedGeneratedCodeWriteFaults": 1 << 3,
    "fastmemMisses": 1 << 4,
}
RUNTIME_INVALIDATION_REQUIRED = frozenset({
    "guestInvalidationRequests",
    "guestInvalidationCompletions",
    "guestInvalidationWaitNs",
})
RUNTIME_FAULT_REQUIRED = frozenset(
    set(RUNTIME_INVALIDATION_REQUIRED) | {"ownedGeneratedCodeWriteFaults"}
)
RUNTIME_COMPLETE_PANEL = frozenset(RUNTIME_METRIC_BITS)

FEX_UNPROVABLE_TOTALS = frozenset({
    "executedGuestInstructionEntries",
    "executedDispatcherEntries",
    "compiledGuestBytes",
    "compiledHelperSites",
    "compiledDmbSites",
})

WIRELESS_ALIAS_RE = re.compile(
    r"^adb-10AG263F1B001HM-[A-Za-z0-9_-]+\._adb-tls-connect\._tcp"
    r"(?: \([0-9]+\))?$"
)
SAFE_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,95}$")
PROOF_TOKEN_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$")
LAYER_GENERATION_RE = re.compile(r"#(?P<generation>[0-9]+)$")
DHASH_1024_RE = re.compile(r"^[0-9a-f]{256}$")
OVERLAP_HEADER_V2 = struct.Struct("<8sIIIIQQQQIIIIQQQQQQQQQQ")
OVERLAP_RECORD_V2 = struct.Struct("<QQQQQII")
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


class CampaignRejected(RuntimeError):
    """A planned or captured campaign cannot support a conclusion."""

    def __init__(self, reason: str, detail: str | None = None):
        self.reason = reason
        self.detail = detail
        super().__init__(reason if detail is None else f"{reason}: {detail}")


def reject_if(condition: bool, reason: str, detail: str | None = None) -> None:
    if condition:
        raise CampaignRejected(reason, detail)


def canonical_json(value: Any) -> bytes:
    return (json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ) + "\n").encode("utf-8")


def canonical_sha256(value: Any) -> str:
    return hashlib.sha256(canonical_json(value)).hexdigest()


def _paeth_predictor(left: int, above: int, upper_left: int) -> int:
    estimate = left + above - upper_left
    left_distance = abs(estimate - left)
    above_distance = abs(estimate - above)
    upper_left_distance = abs(estimate - upper_left)
    if left_distance <= above_distance and left_distance <= upper_left_distance:
        return left
    if above_distance <= upper_left_distance:
        return above
    return upper_left


def decode_screencap_png(payload: bytes) -> tuple[int, int, int, bytes]:
    """Decode the strict 8-bit RGB/RGBA PNG subset emitted by screencap.

    A campaign must not silently hand a malformed or differently encoded image
    to a permissive external decoder.  The route freezes the capture geometry;
    unsupported PNG features reject the phase rather than weakening the visual
    scene gate.
    """

    reject_if(not payload.startswith(PNG_SIGNATURE), "scene-anchor-png-signature-invalid")
    cursor = len(PNG_SIGNATURE)
    ihdr: bytes | None = None
    compressed: list[bytes] = []
    saw_iend = False
    while cursor < len(payload):
        reject_if(cursor + 12 > len(payload), "scene-anchor-png-chunk-truncated")
        length = struct.unpack_from(">I", payload, cursor)[0]
        cursor += 4
        chunk_type = payload[cursor:cursor + 4]
        cursor += 4
        reject_if(cursor + length + 4 > len(payload),
                  "scene-anchor-png-chunk-truncated")
        chunk = payload[cursor:cursor + length]
        cursor += length
        expected_crc = struct.unpack_from(">I", payload, cursor)[0]
        cursor += 4
        actual_crc = zlib.crc32(chunk_type)
        actual_crc = zlib.crc32(chunk, actual_crc) & 0xFFFFFFFF
        reject_if(actual_crc != expected_crc, "scene-anchor-png-crc-invalid",
                  chunk_type.decode("ascii", "replace"))
        if chunk_type == b"IHDR":
            reject_if(ihdr is not None, "scene-anchor-png-ihdr-duplicate")
            ihdr = chunk
        elif chunk_type == b"IDAT":
            compressed.append(chunk)
        elif chunk_type == b"IEND":
            reject_if(length != 0, "scene-anchor-png-iend-invalid")
            saw_iend = True
            reject_if(cursor != len(payload), "scene-anchor-png-trailing-data")
            break
        elif chunk_type and 65 <= chunk_type[0] <= 90:
            raise CampaignRejected(
                "scene-anchor-png-critical-chunk-unsupported",
                chunk_type.decode("ascii", "replace"),
            )
    reject_if(ihdr is None or len(ihdr) != 13,
              "scene-anchor-png-ihdr-invalid")
    reject_if(not compressed or not saw_iend, "scene-anchor-png-incomplete")
    width, height, bit_depth, color_type, compression, filtering, interlace = (
        struct.unpack(">IIBBBBB", ihdr)
    )
    reject_if(width <= 0 or height <= 0, "scene-anchor-png-geometry-invalid")
    reject_if(bit_depth != 8 or color_type not in {2, 6} or compression != 0
              or filtering != 0 or interlace != 0,
              "scene-anchor-png-format-unsupported",
              f"depth={bit_depth} color={color_type} interlace={interlace}")
    channels = 3 if color_type == 2 else 4
    row_bytes = width * channels
    expected_size = height * (row_bytes + 1)
    try:
        filtered = zlib.decompress(b"".join(compressed))
    except zlib.error as error:
        raise CampaignRejected("scene-anchor-png-deflate-invalid", str(error)) from error
    reject_if(len(filtered) != expected_size, "scene-anchor-png-size-mismatch",
              f"expected={expected_size} actual={len(filtered)}")

    decoded = bytearray(height * row_bytes)
    previous = bytearray(row_bytes)
    source = 0
    for row_index in range(height):
        filter_type = filtered[source]
        source += 1
        reject_if(filter_type > 4, "scene-anchor-png-filter-unsupported",
                  str(filter_type))
        current = bytearray(filtered[source:source + row_bytes])
        source += row_bytes
        for index in range(row_bytes):
            left = current[index - channels] if index >= channels else 0
            above = previous[index]
            upper_left = previous[index - channels] if index >= channels else 0
            if filter_type == 1:
                current[index] = (current[index] + left) & 0xFF
            elif filter_type == 2:
                current[index] = (current[index] + above) & 0xFF
            elif filter_type == 3:
                current[index] = (current[index] + ((left + above) // 2)) & 0xFF
            elif filter_type == 4:
                current[index] = (
                    current[index] + _paeth_predictor(left, above, upper_left)
                ) & 0xFF
        offset = row_index * row_bytes
        decoded[offset:offset + row_bytes] = current
        previous = current
    return width, height, channels, bytes(decoded)


def roi_dhash_1024(png: bytes, anchor: Mapping[str, Any]) -> str:
    """Return a 1024-bit (32x32 comparisons) box-filtered ROI dHash."""

    width, height, channels, pixels = decode_screencap_png(png)
    reject_if(width != anchor.get("captureWidth") or
              height != anchor.get("captureHeight"),
              "scene-anchor-capture-geometry-mismatch",
              f"expected={anchor.get('captureWidth')}x{anchor.get('captureHeight')} "
              f"actual={width}x{height}")
    roi = anchor.get("roi")
    reject_if(not isinstance(roi, Mapping), "scene-anchor-roi-invalid")
    try:
        x = int(roi["x"])
        y = int(roi["y"])
        roi_width = int(roi["width"])
        roi_height = int(roi["height"])
    except (KeyError, TypeError, ValueError) as error:
        raise CampaignRejected("scene-anchor-roi-invalid", str(error)) from error
    reject_if(any(isinstance(roi.get(name), bool)
                  for name in ("x", "y", "width", "height")) or
              x < 0 or y < 0 or roi_width < 33 or roi_height < 32 or
              x + roi_width > width or y + roi_height > height,
              "scene-anchor-roi-invalid", repr(dict(roi)))

    luminance: list[list[int]] = []
    row_stride = width * channels
    for grid_y in range(32):
        y0 = y + (grid_y * roi_height) // 32
        y1 = y + ((grid_y + 1) * roi_height) // 32
        row_values: list[int] = []
        for grid_x in range(33):
            x0 = x + (grid_x * roi_width) // 33
            x1 = x + ((grid_x + 1) * roi_width) // 33
            total = 0
            count = (x1 - x0) * (y1 - y0)
            for pixel_y in range(y0, y1):
                offset = pixel_y * row_stride + x0 * channels
                for _pixel_x in range(x0, x1):
                    red, green, blue = pixels[offset:offset + 3]
                    total += 77 * red + 150 * green + 29 * blue
                    offset += channels
            reject_if(count <= 0, "scene-anchor-roi-bin-empty")
            row_values.append(total // count)
        luminance.append(row_values)

    bits = 0
    for row in luminance:
        for index in range(32):
            bits = (bits << 1) | int(row[index] > row[index + 1])
    return f"{bits:0256x}"


def dhash_hamming_distance(first: str, second: str) -> int:
    reject_if(DHASH_1024_RE.fullmatch(first) is None or
              DHASH_1024_RE.fullmatch(second) is None,
              "scene-anchor-descriptor-invalid")
    return (int(first, 16) ^ int(second, 16)).bit_count()


def validate_visual_anchor(anchor: Mapping[str, Any], label: str) -> None:
    reject_if(anchor.get("schema") != VISUAL_ANCHOR_SCHEMA,
              "scene-anchor-schema-invalid", label)
    reject_if(anchor.get("algorithm") != "roi-dhash-1024-box-luma-v1",
              "scene-anchor-algorithm-invalid", label)
    for name, expected_value in (("captureWidth", 3168), ("captureHeight", 1440)):
        value = anchor.get(name)
        reject_if(isinstance(value, bool) or not isinstance(value, int)
                  or value != expected_value,
                  "scene-anchor-capture-geometry-invalid", f"{label}.{name}")
    descriptor = anchor.get("descriptorHex")
    reject_if(not isinstance(descriptor, str) or
              DHASH_1024_RE.fullmatch(descriptor) is None,
              "scene-anchor-descriptor-invalid", label)
    evidence_sha = anchor.get("sourceEvidenceSha256")
    reject_if(not isinstance(evidence_sha, str) or
              re.fullmatch(r"[0-9a-f]{64}", evidence_sha) is None,
              "scene-anchor-evidence-hash-invalid", label)
    threshold = anchor.get("maximumHammingDistance")
    reject_if(isinstance(threshold, bool) or not isinstance(threshold, int)
              or threshold < 0 or threshold > 256,
              "scene-anchor-threshold-invalid", label)


    roi = anchor.get("roi")
    reject_if(not isinstance(roi, Mapping) or set(roi) != {"x", "y", "width", "height"},
              "scene-anchor-roi-invalid", label)
    values = [roi.get(name) for name in ("x", "y", "width", "height")]
    reject_if(any(isinstance(value, bool) or not isinstance(value, int)
                  for value in values), "scene-anchor-roi-invalid", label)
    x, y, roi_width, roi_height = values
    reject_if(x < 0 or y < 0 or roi_width < 33 or roi_height < 32 or
              x + roi_width > DISPLAY_WIDTH or y + roi_height > DISPLAY_HEIGHT,
              "scene-anchor-roi-invalid", label)


def validate_measurement_input(
    input_spec: Mapping[str, Any], duration_seconds: int,
) -> None:
    """Validate the only two measurement inputs without interpreting them.

    ``static`` carries no hidden fields.  ``swipe`` is the frozen virtual-left-
    stick forward gesture and must span the whole PMU/SF window.  In
    particular, Cross coordinates cannot be smuggled into the post-gameplay
    portion of a plan.
    """

    reject_if(not isinstance(input_spec, Mapping),
              "route-measurement-input-invalid")
    kind = input_spec.get("kind")
    reject_if(kind not in {"static", "swipe"},
              "route-measurement-input-invalid")
    if kind == "static":
        reject_if(set(input_spec) != {"kind"},
                  "route-static-input-fields-invalid")
        return

    expected_fields = set(MOVING_MEASUREMENT_SWIPE)
    reject_if(set(input_spec) != expected_fields,
              "route-measurement-swipe-fields-invalid")
    for field in expected_fields - {"kind"}:
        value = input_spec.get(field)
        reject_if(isinstance(value, bool) or not isinstance(value, int),
                  "route-measurement-swipe-field-invalid", field)
    x1, y1 = int(input_spec["x1"]), int(input_spec["y1"])
    x2, y2 = int(input_spec["x2"]), int(input_spec["y2"])
    reject_if(not (0 <= x1 < DISPLAY_WIDTH and 0 <= x2 < DISPLAY_WIDTH and
                   0 <= y1 < DISPLAY_HEIGHT and 0 <= y2 < DISPLAY_HEIGHT),
              "route-measurement-swipe-coordinate-out-of-bounds")
    swipe_ms = int(input_spec["durationMilliseconds"])
    reject_if(swipe_ms <= 0 or swipe_ms > 60_000,
              "route-measurement-swipe-duration-invalid")
    reject_if(swipe_ms != duration_seconds * 1000,
              "route-measurement-swipe-window-duration-mismatch")
    reject_if(dict(input_spec) != MOVING_MEASUREMENT_SWIPE,
              "route-measurement-swipe-contract-invalid")


def validate_movement_evidence(value: Mapping[str, Any]) -> None:
    reject_if(not isinstance(value, Mapping) or set(value) != {
        "schema", "algorithm", "captureWidth", "captureHeight", "roi",
        "minimumHammingDistance",
    }, "route-movement-evidence-fields-invalid")
    reject_if(value.get("schema") != MOVEMENT_EVIDENCE_SCHEMA or
              value.get("algorithm") != "roi-dhash-1024-box-luma-v1",
              "route-movement-evidence-schema-invalid")
    reject_if(value.get("captureWidth") != DISPLAY_WIDTH or
              value.get("captureHeight") != DISPLAY_HEIGHT,
              "route-movement-evidence-capture-geometry-invalid")
    roi = value.get("roi")
    reject_if(not isinstance(roi, Mapping) or
              set(roi) != {"x", "y", "width", "height"},
              "route-movement-evidence-roi-invalid")
    roi_values = [roi.get(name) for name in ("x", "y", "width", "height")]
    reject_if(any(isinstance(item, bool) or not isinstance(item, int)
                  for item in roi_values),
              "route-movement-evidence-roi-invalid")
    x, y, width, height = roi_values
    reject_if(x < 0 or y < 0 or width < 33 or height < 32 or
              x + width > DISPLAY_WIDTH or y + height > DISPLAY_HEIGHT,
              "route-movement-evidence-roi-invalid")
    minimum = value.get("minimumHammingDistance")
    reject_if(isinstance(minimum, bool) or not isinstance(minimum, int) or
              minimum < 12 or minimum > 256,
              "route-movement-evidence-distance-invalid")


def movement_scene_change_metadata(
    before_descriptor: str, after_descriptor: str,
    evidence: Mapping[str, Any],
) -> dict[str, Any]:
    validate_movement_evidence(evidence)
    distance = dhash_hamming_distance(before_descriptor, after_descriptor)
    minimum = int(evidence["minimumHammingDistance"])
    return {
        "schema": MOVEMENT_EVIDENCE_SCHEMA,
        "algorithm": evidence["algorithm"],
        "roi": dict(evidence["roi"]),
        "beforeDescriptorHex": before_descriptor,
        "afterDescriptorHex": after_descriptor,
        "hammingDistance": distance,
        "minimumHammingDistance": minimum,
        "sceneChangeProven": distance >= minimum,
        "routeTraversalDistanceClaimed": False,
        "fpsVerdictAllowed": False,
    }


def simpleperf_attach_argv(
    adb: str, target: str, pid: int, duration_seconds: int,
) -> list[str]:
    """Build the *candidate* non-root existing-process ``simpleperf`` command.

    The proof parser is the availability gate: any stderr diagnostic rejects
    the window.  On the current retail Vivo firmware this shell ``--app -p``
    form still emits ``Running child command in app uid isn't allowed`` and is
    therefore unavailable for production evidence.  Running under ``run-as``
    is also forbidden, while shell ``-p`` without ``--app`` fails
    ``perf_event_open``.  Keep this builder for devices whose kernel policy
    genuinely permits the attach, but never interpret returned counter rows as
    valid merely because the process exits zero.

    ``-p`` is the exact existing game process selector; there is deliberately
    no positional workload after the options.
    """

    reject_if(isinstance(pid, bool) or not isinstance(pid, int) or pid <= 0,
              "simpleperf-attach-pid-invalid", repr(pid))
    reject_if(
        isinstance(duration_seconds, bool)
        or not isinstance(duration_seconds, int)
        or duration_seconds <= 0,
        "simpleperf-duration-invalid",
        repr(duration_seconds),
    )
    return [
        adb, "-s", target, "shell", "/system/bin/simpleperf", "stat",
        "--csv", "--app", PACKAGE, "-p", str(pid),
        "--duration", str(duration_seconds), "-e", ",".join(PMU_EVENTS),
    ]


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def single_file_inventory_sha256(path: pathlib.Path) -> str:
    reject_if(path.is_symlink() or not path.is_file(),
              "moving-inventory-file-invalid", str(path))
    entry = {
        "root": "root0",
        "path": path.name,
        "type": "file",
        "size": path.stat().st_size,
        "sha256": sha256_file(path),
    }
    return canonical_sha256([entry])


def validate_moving_campaign_inventory(
    route: Mapping[str, Any], route_path: pathlib.Path,
    expected_state: Mapping[str, Any],
) -> pathlib.Path | None:
    """Bind a moving plan to its reviewed route/input host inventories."""

    if route["measurement"]["input"]["kind"] != "swipe":
        return None
    repo_root = pathlib.Path(__file__).resolve().parents[2]
    canonical_route = repo_root / MOVING_ROUTE_RELATIVE_PATH
    reject_if(route_path.resolve() != canonical_route.resolve(),
              "moving-route-path-not-canonical", str(route_path))
    input_path = repo_root / MOVING_INPUT_RELATIVE_PATH
    input_artifact = load_json_object(input_path, "moving-input")
    gesture = input_artifact.get("gesture")
    declared_input = (
        {"kind": input_artifact.get("kind"), **dict(gesture)}
        if isinstance(gesture, Mapping) else None
    )
    reject_if(declared_input != route["measurement"]["input"] or
              input_artifact.get("crossAllowed") is not False or
              input_artifact.get("requiresGameplayAndVisualGate") is not True or
              input_artifact.get("measurementDurationSeconds") !=
              route["measurement"]["durationSeconds"] or
              input_artifact.get("movementEvidenceRequired") is not True,
              "moving-input-artifact-contract-invalid")

    inventories = expected_state.get("inventories")
    reject_if(not isinstance(inventories, Mapping),
              "moving-expected-inventories-missing")
    for name, relative_path, local_path in (
        ("route", MOVING_ROUTE_RELATIVE_PATH, canonical_route),
        ("input", MOVING_INPUT_RELATIVE_PATH, input_path),
    ):
        spec = inventories.get(name)
        reject_if(not isinstance(spec, Mapping) or
                  spec.get("source") != "host" or
                  spec.get("roots") != [relative_path],
                  "moving-expected-inventory-root-mismatch", name)
        reject_if(spec.get("sha256") != single_file_inventory_sha256(local_path),
                  "moving-expected-inventory-hash-mismatch", name)
    return input_path


def load_json_object(path: pathlib.Path, label: str) -> dict[str, Any]:
    reject_if(path.is_symlink() or not path.is_file(), f"{label}-file-invalid", str(path))
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise CampaignRejected(f"{label}-unreadable", str(error)) from error
    reject_if(not isinstance(value, dict), f"{label}-not-object")
    return value


def validate_route(route: Mapping[str, Any]) -> None:
    reject_if(route.get("schema") != ROUTE_SCHEMA, "route-schema-invalid")
    for field in (
        "id", "titleId", "titleVersion", "package", "launcherComponent",
        "gameComponent", "gameProcess", "proofMetricsReceiver", "gamePath", "device", "launch",
        "visualAnchors", "gameplayGate", "surface", "measurement", "thermal", "cachePolicy",
        "verdictPolicy",
    ):
        reject_if(field not in route, "route-field-missing", field)
    reject_if(route["package"] != PACKAGE, "route-package-invalid", str(route["package"]))
    reject_if(route["gameProcess"] != GAME_PROCESS, "route-process-invalid")
    receiver = route["proofMetricsReceiver"]
    reject_if(not isinstance(receiver, Mapping), "route-proof-receiver-invalid")
    reject_if(receiver.get("component") != PROOF_RECEIVER or
              receiver.get("action") != PROOF_ACTION or
              receiver.get("tokenExtra") != PROOF_TOKEN_EXTRA or
              receiver.get("requiredPermission") != "android.permission.DUMP" or
              receiver.get("process") != ":game",
              "route-proof-receiver-contract-invalid")
    device = route["device"]
    reject_if(not isinstance(device, Mapping), "route-device-invalid")
    reject_if(device.get("roSerial") != EXPECTED_SERIAL, "route-device-serial-invalid")
    try:
        re.compile(str(device.get("adbTargetRegex", "")))
    except re.error as error:
        raise CampaignRejected("route-adb-regex-invalid", str(error)) from error

    launch = route["launch"]
    reject_if(not isinstance(launch, Mapping), "route-launch-invalid")
    actions = launch.get("actions")
    reject_if(not isinstance(actions, list), "route-actions-invalid")
    crosses = [action for action in actions
               if isinstance(action, Mapping) and action.get("kind") == "cross-hold"]
    reject_if(len(actions) != 1 or len(crosses) != 1,
              "route-must-have-exactly-one-cross")
    expected_anchor_sequence = ("hunters-dream-world",)
    for index, (cross, expected_anchor_id) in enumerate(
            zip(crosses, expected_anchor_sequence, strict=True)):
        reject_if(cross.get("x") != 2903 or cross.get("y") != 1231,
                  "route-cross-coordinate-invalid", str(index))
        hold = cross.get("holdMilliseconds")
        reject_if(isinstance(hold, bool) or not isinstance(hold, int) or hold < 300,
                  "route-cross-hold-too-short", str(index))
        reject_if(cross.get("stateAnchorId") != expected_anchor_id,
                  "route-state-anchor-sequence-invalid", str(index))
        for name, minimum in (("stateGateTimeoutSeconds", 10),
                              ("stateGatePollSeconds", 1)):
            value = cross.get(name)
            reject_if(isinstance(value, bool) or not isinstance(value, int)
                      or value < minimum,
                      "route-state-gate-timing-invalid", f"{index}.{name}")

    anchors = route["visualAnchors"]
    reject_if(not isinstance(anchors, Mapping) or
              set(anchors) != set(expected_anchor_sequence),
              "route-visual-anchor-set-invalid")
    for anchor_id, anchor in anchors.items():
        reject_if(not isinstance(anchor, Mapping),
                  "scene-anchor-invalid", anchor_id)
        validate_visual_anchor(anchor, anchor_id)
    gate = route["gameplayGate"]
    reject_if(not isinstance(gate, Mapping), "route-gameplay-gate-invalid")
    reject_if(gate.get("forbidAdditionalCross") is not True,
              "route-additional-cross-not-forbidden")
    reject_if(gate.get("requiredConsecutivePasses", 0) < 2,
              "route-gameplay-gate-too-weak")
    reject_if(gate.get("minimumHavokPercent", 0) < 20,
              "route-havok-gate-too-weak")
    reject_if(gate.get("minimumGpuCommPercent", 0) < 8,
              "route-gpucomm-gate-too-weak")
    reject_if(gate.get("sceneAnchorId") != "hunters-dream-world",
              "route-gameplay-scene-anchor-invalid")
    draw_floor = gate.get("minimumDrawCallsPerSecond")
    reject_if(isinstance(draw_floor, bool) or not isinstance(draw_floor, (int, float))
              or draw_floor < 6000,
              "route-gameplay-draw-floor-too-weak")
    draw_probe_seconds = gate.get("drawProbeSeconds")
    reject_if(isinstance(draw_probe_seconds, bool) or
              not isinstance(draw_probe_seconds, int) or draw_probe_seconds < 2,
              "route-gameplay-draw-probe-invalid")
    for name, minimum in (("finalSceneRecheckTimeoutSeconds", 5),
                          ("finalSceneRecheckPollSeconds", 1)):
        value = gate.get(name)
        reject_if(isinstance(value, bool) or not isinstance(value, int)
                  or value < minimum,
                  "route-gameplay-scene-recheck-invalid", name)

    measurement = route["measurement"]
    reject_if(not isinstance(measurement, Mapping), "route-measurement-invalid")
    duration = measurement.get("durationSeconds")
    reject_if(isinstance(duration, bool) or not isinstance(duration, int) or duration < 10,
              "route-measurement-duration-invalid")
    input_spec = measurement.get("input")
    validate_measurement_input(input_spec, duration)
    movement_evidence = measurement.get("movementEvidence")
    if input_spec["kind"] == "swipe":
        reject_if(measurement.get("inputArtifact") != MOVING_INPUT_RELATIVE_PATH,
                  "route-moving-input-artifact-invalid")
        validate_movement_evidence(movement_evidence)
    else:
        reject_if(movement_evidence is not None or
                  measurement.get("inputArtifact") is not None,
                  "route-static-movement-evidence-forbidden")
    surface = route["surface"]
    reject_if(not isinstance(surface, Mapping) or
              surface.get("requiredComponent") !=
              "SurfaceView[app.lsx4.android/app.lsx4.android.GameActivity](BLAST)" or
              isinstance(surface.get("minimumGeneration"), bool) or
              not isinstance(surface.get("minimumGeneration"), int) or
              surface["minimumGeneration"] < 1,
              "route-surface-contract-invalid")

    cache_policy = route["cachePolicy"]
    reject_if(not isinstance(cache_policy, Mapping), "route-cache-policy-invalid")
    reject_if(cache_policy.get("mode") != "preserve-matched-inventory" or
              cache_policy.get("allowDeletion") is not False,
              "route-cache-policy-not-preserving")
    required_inventories = set(cache_policy.get("requireProvenanceInventories", []))
    reject_if(not {"pipelineCache", "fexCache"}.issubset(required_inventories),
              "route-cache-inventory-incomplete")
    thermal = route["thermal"]
    reject_if(not isinstance(thermal, Mapping), "route-thermal-invalid")
    maximum_status = thermal.get("maximumStatus")
    reject_if(isinstance(maximum_status, bool) or
              not isinstance(maximum_status, int) or
              maximum_status < 0 or maximum_status > 2,
              "route-thermal-status-gate-too-weak")
    minimum_battery = thermal.get("minimumBatteryLevelPercent")
    reject_if(isinstance(minimum_battery, bool) or
              not isinstance(minimum_battery, int) or
              minimum_battery < 20 or minimum_battery > 100,
              "route-battery-gate-too-weak")
    policy_floors = thermal.get("minimumCpuPolicyMaxKHz")
    reject_if(not isinstance(policy_floors, Mapping) or
              set(policy_floors) != {"0", "6"} or
              any(isinstance(value, bool) or not isinstance(value, int)
                  for value in policy_floors.values()) or
              policy_floors.get("0", 0) < 1785600 or
              policy_floors.get("6", 0) < 1632000,
              "route-cpu-policy-floor-too-weak")
    reject_if(thermal.get("requireEqualExternalPowerAcrossPhases") is not True,
              "route-external-power-band-not-required")
    reject_if(thermal.get("requireEqualCpuPolicyLimitsAcrossPhases") is not True,
              "route-cpu-policy-band-not-required")
    reject_if(thermal.get("requireSurfaceTemperatureForCampaign") is not True,
              "route-surface-temperature-band-not-required")
    maximum_delta = thermal.get("maximumPhaseStartSurfaceDeltaC")
    reject_if(isinstance(maximum_delta, bool) or
              not isinstance(maximum_delta, (int, float)) or
              maximum_delta <= 0 or maximum_delta > 3,
              "route-surface-temperature-band-invalid")
    verdict = route["verdictPolicy"]
    reject_if(not isinstance(verdict, Mapping) or verdict.get("hudAverageAccepted") is not False,
              "route-hud-average-not-forbidden")
    reject_if(verdict.get("productionAndInstrumentedWindowsMayBeCompared") is not False,
              "route-instrumentation-comparison-not-forbidden")


def validate_adb_target(route: Mapping[str, Any], adb_target: str) -> None:
    reject_if(not adb_target or not WIRELESS_ALIAS_RE.fullmatch(adb_target),
              "adb-target-not-exact-vivo-alias", adb_target)
    expected_regex = re.compile(str(route["device"]["adbTargetRegex"]))
    reject_if(not expected_regex.fullmatch(adb_target),
              "adb-target-route-mismatch", adb_target)


def validate_pmu_capabilities(value: Mapping[str, Any]) -> None:
    reject_if(value.get("schema") != "lsx4-pmu-negative-control/v1",
              "pmu-capability-schema-invalid")
    reject_if(value.get("accepted") is not True, "pmu-capability-not-accepted")
    reject_if(value.get("l1iValidated") is not True, "pmu-l1i-unvalidated")
    reject_if(value.get("branchValidated") is not True, "pmu-branch-unvalidated")
    reject_if(value.get("dmbPmuValidated") is not False,
              "pmu-dmb-capability-unexpected")




    reject_if(value.get("dmbMetricSource") not in {
        "fex-exact-runtime-counter", "unavailable"
    }, "dmb-capability-declaration-invalid")


def phase_labels(kind: str, order: str | None) -> list[str]:
    if kind == "axis":
        reject_if(order not in {"ABA", "BAB"}, "axis-order-invalid", str(order))
        return list(order)
    reject_if(order is not None, "order-only-valid-for-axis")
    return ["D" if kind in {
        "static", "dynamic", "runtime", "runtime-faults", "overlap"
    } else "A"]


def desired_markers(kind: str, axis: str | None, variant: str) -> list[str]:
    enabled: set[str] = set()
    if kind == "axis":
        reject_if(axis not in AXIS_MARKERS, "axis-invalid", str(axis))
        if variant == "B":
            enabled.add(AXIS_MARKERS[str(axis)])
    else:
        reject_if(axis is not None, "axis-only-valid-for-axis")
    if kind in INSTRUMENTATION_MARKERS:
        enabled.add(INSTRUMENTATION_MARKERS[kind])
    if kind in {"runtime", "runtime-faults"}:
        enabled.add(RUNTIME_COUNTER_MARKER)
    if kind == "overlap":
        enabled.add(OVERLAP_MARKER)
    reject_if(MULTIBLOCK_MARKER in enabled, "multiblock-quarantine-violated")
    return sorted(enabled)


def _action(kind: str, **values: Any) -> dict[str, Any]:
    uses_adb = bool(values.pop("_adb", False))
    result = {"kind": kind}
    result.update(values)
    if uses_adb:
        result["adbTargetPolicy"] = "exact-plan-alias"
        result["agentChatGate"] = [
            "event",
            "own:device:vivo",
            f"own:adb:{EXPECTED_SERIAL}",
        ]
    return result


def _canonical_baseline_backend_status(base: Mapping[str, Any]) -> dict[str, Any]:
    backend = base.get("backend")
    reject_if(not isinstance(backend, Mapping), "expected-backend-missing")
    status = backend.get("status")
    reject_if(not isinstance(status, Mapping),
              "expected-backend-status-payload-missing")
    payload = json.loads(json.dumps(status))
    status_sha = backend.get("statusSha256")
    reject_if(not isinstance(status_sha, str) or
              canonical_sha256(payload) != status_sha,
              "expected-backend-status-payload-hash-mismatch")
    for field in BACKEND_MEMORY_FIELDS:
        wanted = BASELINE_BACKEND_MEMORY_CONTRACT[field]
        got = payload.get(field)
        if isinstance(wanted, int):
            reject_if(isinstance(got, bool) or not isinstance(got, int),
                      "expected-baseline-backend-field-type-invalid", field)
        reject_if(got != wanted, "expected-baseline-backend-contract-mismatch",
                  f"{field}: expected={wanted!r} actual={got!r}")
    cache_namespace = payload.get("cacheNamespace")
    baseline_suffix = BASELINE_BACKEND_MEMORY_CONTRACT["cacheNamespaceSuffix"]
    reject_if(not isinstance(cache_namespace, str) or
              not cache_namespace.endswith(baseline_suffix),
              "expected-baseline-backend-cache-namespace-mismatch",
              repr(cache_namespace))
    return payload


def _phase_backend_expected(base: Mapping[str, Any], axis: str | None,
                            variant: str) -> dict[str, Any]:
    backend = base.get("backend")
    reject_if(not isinstance(backend, Mapping), "expected-backend-missing")
    result = json.loads(json.dumps(backend))
    baseline = _canonical_baseline_backend_status(base)
    payload = json.loads(json.dumps(baseline))
    contract = MEMORY_AXIS_BACKEND_CONTRACTS.get(str(axis))
    if variant == "B" and contract is not None:
        software_tso_suffix = "-sm-swtso"
        cache_namespace = str(payload["cacheNamespace"])
        reject_if(not cache_namespace.endswith(software_tso_suffix),
                  "expected-baseline-backend-cache-namespace-mismatch",
                  repr(cache_namespace))
        cache_base = cache_namespace[:-len(software_tso_suffix)]
        for field in BACKEND_MEMORY_FIELDS:
            payload[field] = contract[field]
        payload["cacheNamespace"] = (
            cache_base + str(contract["cacheNamespaceSuffix"])
        )




    required = result.get("required")
    reject_if(not isinstance(required, dict), "expected-backend-required-missing")
    for field in (*BACKEND_MEMORY_FIELDS, "cacheNamespace"):
        if field in required:
            required[field] = payload[field]
    result["status"] = payload
    result["statusSha256"] = canonical_sha256(payload)
    return result


def _phase_expected_state(base: Mapping[str, Any], enabled: Sequence[str],
                          axis: str | None, variant: str) -> dict[str, Any]:

    expected = json.loads(json.dumps(base))
    expected["backend"] = _phase_backend_expected(base, axis, variant)
    markers = expected.get("markers")
    reject_if(not isinstance(markers, dict), "expected-markers-missing")
    allowlist = markers.get("allowlist")
    present = markers.get("present")
    reject_if(not isinstance(allowlist, list) or not isinstance(present, list),
              "expected-marker-lists-invalid")
    reject_if(not MANAGED_MARKERS.issubset(set(allowlist)),
              "expected-marker-allowlist-incomplete",
              ",".join(sorted(MANAGED_MARKERS - set(allowlist))))
    unmanaged_present = set(present) - MANAGED_MARKERS
    phase_present = sorted(unmanaged_present | set(enabled))
    reject_if(MULTIBLOCK_MARKER in phase_present, "multiblock-quarantine-violated")
    markers["present"] = phase_present
    markers["presentSha256"] = canonical_sha256(phase_present)
    return expected


def build_plan(
    *,
    route: Mapping[str, Any],
    expected_state: Mapping[str, Any],
    pmu_capabilities: Mapping[str, Any],
    route_path: pathlib.Path,
    expected_state_path: pathlib.Path,
    pmu_capabilities_path: pathlib.Path,
    output_root: pathlib.Path,
    campaign_id: str,
    adb_target: str,
    kind: str,
    axis: str | None = None,
    order: str | None = None,
    duration_seconds: int | None = None,
    require_complete_panel: bool = False,
) -> dict[str, Any]:
    validate_route(route)
    validate_adb_target(route, adb_target)
    validate_pmu_capabilities(pmu_capabilities)
    moving_input_path = validate_moving_campaign_inventory(
        route, route_path, expected_state
    )
    reject_if(not SAFE_ID_RE.fullmatch(campaign_id), "campaign-id-invalid", campaign_id)
    reject_if(kind not in {
        "production", "axis", "static", "dynamic", "runtime",
        "runtime-faults", "overlap",
    },
              "campaign-kind-invalid", kind)
    reject_if(kind == "axis" and axis not in AXIS_MARKERS, "axis-invalid", str(axis))
    reject_if(kind != "axis" and axis is not None, "axis-only-valid-for-axis")
    reject_if(kind in {
        "static", "dynamic", "runtime", "runtime-faults", "overlap"
    } and order is not None,
              "instrumented-order-forbidden")
    if kind == "runtime-faults":
        markers = expected_state.get("markers")
        present = markers.get("present", []) if isinstance(markers, Mapping) else []
        reject_if("run-live-quiet" in present,
                  "runtime-fault-proof-requires-live-quiet-off")
    markers = expected_state.get("markers")
    present = markers.get("present", []) if isinstance(markers, Mapping) else []
    forbidden_present = BASELINE_FORBIDDEN_MARKERS.intersection(present)
    reject_if(bool(forbidden_present), "baseline-forbidden-marker-present",
              ",".join(sorted(forbidden_present)))
    labels = phase_labels(kind, order)
    duration = duration_seconds or int(route["measurement"]["durationSeconds"])
    reject_if(duration < 10 or duration > 60, "measurement-duration-out-of-range")
    movement_evidence = route["measurement"].get("movementEvidence")

    route_sha = sha256_file(route_path)
    expected_sha = sha256_file(expected_state_path)
    pmu_sha = sha256_file(pmu_capabilities_path)
    artifact_dir = output_root.resolve() / campaign_id
    plan: dict[str, Any] = {
        "schema": PLAN_SCHEMA,
        "campaignId": campaign_id,
        "artifactDir": str(artifact_dir),
        "kind": kind,
        "axis": axis,
        "order": order,
        "device": {"adbTarget": adb_target, "roSerial": EXPECTED_SERIAL},
        "inputs": {
            "route": {"path": str(route_path.resolve()), "sha256": route_sha},
            "expectedState": {
                "path": str(expected_state_path.resolve()), "sha256": expected_sha,
            },
            "pmuCapabilities": {
                "path": str(pmu_capabilities_path.resolve()), "sha256": pmu_sha,
            },
        },
        "invariants": {
            "readOnlyPreMutationBootstrapRequired": True,
            "preflightBeforeEveryPhase": True,
            "cleanupPreflightBeforeEveryWindow": True,
            "agentChatEventAndOwnBeforeEveryAdb": True,
            "freshPidAndStarttime": True,
            "exactSurfaceIdentityAndGeneration": True,
            "cacheDeletionAllowed": False,
            "multiblock": "quarantined-off",
            "hudAverageAccepted": False,
            "productionInstrumentation": "off",
            "instrumentedFpsVerdictAllowed": False,
            "artifactDirectoryReuseAllowed": False,
            "postGameplayCrossAllowed": False,
            "measurementInputStartsAfterGameplayAndVisualGate": True,
            "measurementInputSha256": canonical_sha256(
                route["measurement"]["input"]
            ),
            "movementSceneChangeRequired": (
                route["measurement"]["input"]["kind"] == "swipe"
            ),
            "movementEvidenceSha256": (
                canonical_sha256(movement_evidence)
                if movement_evidence is not None else None
            ),
            "movingExpectedInventoryBound": moving_input_path is not None,
            "measurementInputArtifactSha256": (
                sha256_file(moving_input_path)
                if moving_input_path is not None else None
            ),
        },
        "pmu": {
            "events": list(PMU_EVENTS),
            "attach": "existing-fresh-game-pid",
            "capabilitiesSha256": pmu_sha,
            "dmbEvidence": "unavailable-until-authoritative-executed-total",
            "rawDmbPmuAccepted": False,
        },
        "phases": [],
    }
    if moving_input_path is not None:
        plan["inputs"]["measurementInput"] = {
            "path": str(moving_input_path.resolve()),
            "sha256": sha256_file(moving_input_path),
        }
    for index, label in enumerate(labels, start=1):
        enabled = desired_markers(kind, axis, label)
        phase_id = f"{index:02d}-{label}"
        mode = kind if kind in {"static", "dynamic"} else "off"
        proof_required = sorted(
            COMPLETE_PANEL_METRICS if require_complete_panel
            else (STATIC_REQUIRED_METRICS if mode == "static"
                  else DYNAMIC_REQUIRED_METRICS if mode == "dynamic" else ())
        )
        runtime_mode = (
            "dynamic-diagnostic"
            if kind in {"runtime", "runtime-faults"} else "off"
        )
        runtime_required = sorted(
            RUNTIME_COMPLETE_PANEL if require_complete_panel
            else RUNTIME_INVALIDATION_REQUIRED if kind == "runtime"
            else RUNTIME_FAULT_REQUIRED if kind == "runtime-faults" else ()
        )
        token_prefix = f"{campaign_id}-{phase_id}"



        proof_tokens = {
            "start": f"{token_prefix}-start", "end": f"{token_prefix}-end"
        }
        reject_if(any(PROOF_TOKEN_RE.fullmatch(token) is None
                      for token in proof_tokens.values()),
                  "proof-token-invalid", token_prefix)
        gate_token_prefix = canonical_sha256({
            "campaignId": campaign_id, "phaseId": phase_id,
            "purpose": "gameplay-draw-gate",
        })[:24]
        gameplay_gate_tokens = {
            "start": f"gate-{gate_token_prefix}-start",
            "end": f"gate-{gate_token_prefix}-end",
        }
        reject_if(any(PROOF_TOKEN_RE.fullmatch(token) is None
                      for token in gameplay_gate_tokens.values()),
                  "proof-token-invalid", gate_token_prefix)
        phase_expected = _phase_expected_state(expected_state, enabled, axis, label)
        phase = {
            "phaseId": phase_id,
            "variant": label,
            "instrumentationMode": mode,
            "runtimeInstrumentationMode": runtime_mode,
            "fpsVerdictAllowed": kind in {"production", "axis"},
            "enabledManagedMarkers": enabled,
            "disabledManagedMarkers": sorted(MANAGED_MARKERS - set(enabled)),
            "expectedState": phase_expected,
            "proofMetricsRequired": proof_required,
            "runtimeMetricsRequired": runtime_required,
            "proofTokens": proof_tokens,
            "gameplayGateProofTokens": gameplay_gate_tokens,
            "proofSnapshotTransport": {
                "kind": "shell-broadcast",
                "component": PROOF_RECEIVER,
                "action": PROOF_ACTION,
                "requiredPermission": "android.permission.DUMP",
                "targetProcess": GAME_PROCESS,
                "activityLaunchAllowed": False,
            },
            "actions": [
                _action("force-stop-preparation", _adb=True),
                _action("thermal-cooldown", _adb=True,
                        maximumStatus=route["thermal"]["maximumStatus"],
                        timeoutSeconds=route["thermal"]["cooldownTimeoutSeconds"]),
                _action("device-identity", _adb=True,
                        expectedRoSerial=EXPECTED_SERIAL),
                _action("mandatory-performance-preflight", cleanupOnly=False),
                _action("set-and-verify-exact-marker-state", _adb=True,
                        enabled=enabled,
                        disabled=sorted(MANAGED_MARKERS - set(enabled))),
                _action("launch-game", _adb=True,
                        component=route["launcherComponent"],
                        gamePath=route["gamePath"]),
                _action("wait", seconds=route["launch"]["settleBeforeInputSeconds"]),
                _action("one-cross-state-anchored-route", _adb=True,
                        actions=list(route["launch"]["actions"]),
                        visualAnchorIds=list(route["visualAnchors"])),
                _action("hunters-dream-gameplay-gate", _adb=True,
                        **dict(route["gameplayGate"]),
                        proofTokens=gameplay_gate_tokens,
                        hudCountersAreFpsVerdict=False),
                _action("capture-fresh-process-identity", _adb=True,
                        process=route["gameProcess"]),
                _action("capture-exact-surface-identity", _adb=True,
                        requiredComponent=route["surface"]["requiredComponent"]),
                _action("capture-read-only-provenance",
                        expectedStateArtifact=f"phases/{phase_id}/expected-state.json"),
                _action("mandatory-performance-preflight", cleanupOnly=True),
                _action("measurement-window", _adb=True,
                        durationSeconds=duration,
                        attachPid="fresh-game-pid",
                        simpleperfLaunchContext="shell",
                        simpleperfAppScope=PACKAGE,
                        simpleperfChildWorkload=False,
                        simpleperfEvents=list(PMU_EVENTS),
                         surfaceSource="SurfaceFlinger-timestats-exact-BLAST-generation",
                         input=dict(route["measurement"]["input"]),
                         movementEvidence=(
                             dict(movement_evidence)
                             if movement_evidence is not None else None
                         ),
                        proofMetricsMode=mode,
                        proofMetricsRequired=proof_required,
                        runtimeMetricsMode=runtime_mode,
                        runtimeMetricsRequired=runtime_required,
                        overlapMode="causal-v2-instrumented" if kind == "overlap" else "off",
                        proofTokens=proof_tokens),
                _action("verify-same-process-and-surface", _adb=True),
                _action("capture-read-only-provenance-post",
                        expectedStateArtifact=f"phases/{phase_id}/expected-state.json"),
                _action("validate-phase-fail-closed"),
                _action("force-stop-phase-end", _adb=True),
            ],
        }
        if kind == "overlap":
            marker_index = next(
                i for i, action in enumerate(phase["actions"])
                if action["kind"] == "set-and-verify-exact-marker-state"
            )
            phase["actions"].insert(
                marker_index + 1,
                _action("prepare-causal-overlap-v2-control", _adb=True,
                        controlRoot=OVERLAP_CONTROL_ROOT,
                        selector="abi-v2", trace="trace-v2.bin"),
            )
        plan["phases"].append(phase)
    validate_plan(plan)
    return plan


def validate_plan(plan: Mapping[str, Any]) -> None:
    reject_if(plan.get("schema") != PLAN_SCHEMA, "plan-schema-invalid")
    phases = plan.get("phases")
    reject_if(not isinstance(phases, list) or not phases, "plan-phases-empty")
    seen_tokens: set[str] = set()
    start_status = plan["invariants"]
    reject_if(start_status.get("cacheDeletionAllowed") is not False,
              "plan-cache-deletion-enabled")
    reject_if(start_status.get("postGameplayCrossAllowed") is not False or
              start_status.get("measurementInputStartsAfterGameplayAndVisualGate")
              is not True,
              "plan-post-gameplay-input-invariant-invalid")
    for phase in phases:
        actions = phase.get("actions")
        reject_if(not isinstance(actions, list), "plan-actions-invalid")
        reject_if(any(not isinstance(action, Mapping) for action in actions),
                  "plan-action-invalid")
        kinds = [action.get("kind") for action in actions]
        reject_if(kinds.count("mandatory-performance-preflight") != 2,
                  "plan-preflight-count-invalid", str(phase.get("phaseId")))
        for required_kind in (
            "one-cross-state-anchored-route",
            "hunters-dream-gameplay-gate",
            "measurement-window",
        ):
            reject_if(kinds.count(required_kind) != 1,
                      "plan-gameplay-measurement-action-count-invalid",
                      required_kind)
        route_index = kinds.index("one-cross-state-anchored-route")
        gameplay_index = kinds.index("hunters-dream-gameplay-gate")
        measurement_index = kinds.index("measurement-window")
        reject_if(not route_index < gameplay_index < measurement_index,
                  "plan-gameplay-measurement-order-invalid")
        gameplay_action = actions[gameplay_index]
        reject_if(gameplay_action.get("forbidAdditionalCross") is not True or
                  gameplay_action.get("sceneAnchorId") != "hunters-dream-world",
                  "plan-gameplay-gate-contract-invalid")
        for action in actions[gameplay_index + 1:]:
            encoded_action = json.dumps(action, sort_keys=True).lower()
            reject_if("cross" in encoded_action,
                      "plan-cross-after-gameplay-gate",
                      str(action.get("kind")))
        measurement_action = actions[measurement_index]
        measurement_duration = measurement_action.get("durationSeconds")
        reject_if(isinstance(measurement_duration, bool) or
                  not isinstance(measurement_duration, int),
                  "plan-measurement-duration-invalid")
        validate_measurement_input(
            measurement_action.get("input"), measurement_duration
        )
        reject_if(
            canonical_sha256(measurement_action["input"]) !=
            start_status.get("measurementInputSha256"),
            "plan-measurement-input-hash-mismatch",
        )
        movement_evidence = measurement_action.get("movementEvidence")
        moving = measurement_action["input"]["kind"] == "swipe"
        reject_if(start_status.get("movementSceneChangeRequired") is not moving,
                  "plan-movement-evidence-invariant-invalid")
        if moving:
            reject_if(start_status.get("movingExpectedInventoryBound") is not True,
                      "plan-moving-expected-inventory-not-bound")
            validate_movement_evidence(movement_evidence)
            reject_if(
                canonical_sha256(movement_evidence) !=
                start_status.get("movementEvidenceSha256"),
                "plan-movement-evidence-hash-mismatch",
            )
            declared_input = plan.get("inputs", {}).get("measurementInput")
            reject_if(not isinstance(declared_input, Mapping) or
                      declared_input.get("sha256") !=
                      start_status.get("measurementInputArtifactSha256"),
                      "plan-moving-input-artifact-not-frozen")
        else:
            reject_if(movement_evidence is not None or
                      start_status.get("movementEvidenceSha256") is not None or
                      start_status.get("movingExpectedInventoryBound") is not False or
                      start_status.get("measurementInputArtifactSha256") is not None or
                      plan.get("inputs", {}).get("measurementInput") is not None,
                      "plan-static-movement-evidence-present")
        cleanup_indices = [i for i, action in enumerate(actions)
                           if action.get("kind") == "mandatory-performance-preflight"
                           and action.get("cleanupOnly") is True]
        reject_if(len(cleanup_indices) != 1 or cleanup_indices[0] >= measurement_index,
                  "plan-cleanup-preflight-order-invalid")
        reject_if(MULTIBLOCK_MARKER in phase.get("enabledManagedMarkers", []),
                  "multiblock-quarantine-violated")
        enabled = set(phase.get("enabledManagedMarkers", []))
        axes_enabled = enabled.intersection(AXIS_MARKERS.values())
        reject_if(len(axes_enabled) > 1, "plan-multiple-axis-markers")
        instrumented = enabled.intersection(INSTRUMENTATION_MARKERS.values())
        reject_if(len(instrumented) > 1, "plan-mixed-instrumentation")
        mode = phase.get("instrumentationMode")
        reject_if(mode == "off" and instrumented, "plan-production-instrumented")
        runtime_mode = phase.get("runtimeInstrumentationMode")
        runtime_marker_enabled = RUNTIME_COUNTER_MARKER in enabled
        reject_if((runtime_mode == "dynamic-diagnostic") != runtime_marker_enabled,
                  "plan-runtime-instrumentation-marker-mismatch")
        any_diagnostic = bool(instrumented or runtime_marker_enabled or
                              OVERLAP_MARKER in enabled)
        reject_if(any_diagnostic and phase.get("fpsVerdictAllowed") is not False,
                  "plan-instrumented-fps-verdict")
        if plan.get("kind") in {"production", "axis"}:
            reject_if(any_diagnostic, "plan-production-diagnostic-marker")
        tokens = phase.get("proofTokens") or {}
        for token in tokens.values():
            reject_if(token in seen_tokens, "plan-proof-token-reused", token)
            reject_if(not PROOF_TOKEN_RE.fullmatch(token), "plan-proof-token-invalid", token)
            seen_tokens.add(token)
        for action in actions:
            if action.get("adbTargetPolicy") is not None:
                reject_if(action.get("agentChatGate") != [
                    "event", "own:device:vivo", f"own:adb:{EXPECTED_SERIAL}"
                ], "plan-adb-gate-missing", str(action.get("kind")))
    encoded = json.dumps(plan, sort_keys=True).lower()
    for forbidden in ("rm -rf", "pm clear", "clear application data", "delete cache"):
        reject_if(forbidden in encoded, "plan-blind-cache-operation", forbidden)


def parse_proc_stat(text: str) -> tuple[int, str, int]:
    value = text.strip()
    first_space = value.find(" ")
    close = value.rfind(")")
    reject_if(first_space <= 0 or close <= first_space, "proc-stat-malformed")
    try:
        pid = int(value[:first_space])
    except ValueError as error:
        raise CampaignRejected("proc-stat-pid-invalid") from error
    comm = value[first_space + 2:close]
    tail = value[close + 2:].split()
    reject_if(len(tail) < 20, "proc-stat-fields-missing")
    try:
        starttime = int(tail[19])
    except ValueError as error:
        raise CampaignRejected("proc-stat-starttime-invalid") from error
    reject_if(pid <= 0 or starttime <= 0 or not comm, "proc-stat-identity-invalid")
    return pid, comm, starttime


def select_exact_game_surface(text: str, required_component: str) -> dict[str, Any]:
    matches = [line.strip() for line in text.splitlines()
               if required_component in line and line.strip()]
    reject_if(len(matches) != 1, "surface-identity-not-unique", repr(matches))
    identity = matches[0]
    generation_match = LAYER_GENERATION_RE.search(identity)
    reject_if(generation_match is None, "surface-generation-missing", identity)
    return {"identity": identity, "generation": int(generation_match.group("generation"))}


def parse_preflight_surface_max_c(text: str) -> float | None:
    match = re.search(r"surface=\[([^\]]*)\]", text, re.IGNORECASE)
    if match is None or match.group(1).strip().lower() == "unavailable":
        return None
    values = [float(value) for value in re.findall(
        r"=(-?[0-9]+(?:\.[0-9]+)?)C\b", match.group(1), re.IGNORECASE
    )]
    return max(values) if values else None


def parse_preflight_admission(text: str) -> dict[str, Any]:
    match = re.search(
        r"batteryLevel=(?P<battery>\d+)%\s+"
        r"externalPower=(?P<power>true|false)\s+"
        r"cpuPolicies=\["
        r"policy0=(?P<p0cur>\d+)/(?P<p0min>\d+)/(?P<p0max>\d+)KHz,"
        r"policy6=(?P<p6cur>\d+)/(?P<p6min>\d+)/(?P<p6max>\d+)KHz\]",
        text, re.IGNORECASE,
    )
    reject_if(match is None, "preflight-admission-evidence-unavailable")
    assert match is not None
    policies = {
        "0": {
            "currentKHz": int(match.group("p0cur")),
            "minimumKHz": int(match.group("p0min")),
            "maximumKHz": int(match.group("p0max")),
        },
        "6": {
            "currentKHz": int(match.group("p6cur")),
            "minimumKHz": int(match.group("p6min")),
            "maximumKHz": int(match.group("p6max")),
        },
    }
    reject_if(any(value <= 0 for policy in policies.values()
                  for value in policy.values()),
              "preflight-cpu-policy-value-invalid")
    return {
        "batteryLevelPercent": int(match.group("battery")),
        "externalPower": match.group("power").lower() == "true",
        "cpuPolicies": policies,
    }


def parse_overlap_v2(data: bytes) -> dict[str, Any]:
    reject_if(len(data) < OVERLAP_HEADER_V2.size, "overlap-v2-header-truncated")
    fields = OVERLAP_HEADER_V2.unpack_from(data)
    header = {
        "magic": fields[0], "abiVersion": fields[1], "headerSize": fields[2],
        "recordSize": fields[3], "clockId": fields[4], "capacity": fields[5],
        "reserved": fields[6], "committed": fields[7], "dropped": fields[8],
        "enabled": fields[9], "saturated": fields[10], "coverageExact": fields[11],
        "epochMappingExact": fields[12], "featureFlags": fields[13],
        "captureEpoch": fields[14], "captureStartNs": fields[15],
        "captureStopNs": fields[16], "activeSpans": fields[17],
        "leftCensoredRequests": fields[18], "rightCensoredRequests": fields[19],
        "mixedGenerationBatches": fields[20], "epochCrossingBatches": fields[21],
        "reservedZero": fields[22],
    }
    reject_if(header["magic"] != b"LSX4IOT2" or header["abiVersion"] != 2,
              "overlap-v2-identity-invalid")
    reject_if(header["headerSize"] != OVERLAP_HEADER_V2.size or
              header["recordSize"] != OVERLAP_RECORD_V2.size or
              header["clockId"] != 1,
              "overlap-v2-layout-invalid")
    reject_if(header["featureFlags"] != 0x7F,
              "overlap-v2-feature-set-unrecognized", hex(header["featureFlags"]))
    for field in (
        "dropped", "enabled", "saturated", "activeSpans",
        "leftCensoredRequests", "rightCensoredRequests",
        "mixedGenerationBatches", "epochCrossingBatches", "reservedZero",
    ):
        reject_if(header[field] != 0, "overlap-v2-inexact-header", field)
    reject_if(header["coverageExact"] != 1 or header["epochMappingExact"] != 1,
              "overlap-v2-coverage-inexact")
    reject_if(header["reserved"] <= 0 or
              header["reserved"] != header["committed"] or
              header["reserved"] > header["capacity"],
              "overlap-v2-record-count-invalid")
    reject_if(header["captureEpoch"] != header["captureStartNs"] or
              header["captureStartNs"] <= 0 or
              header["captureStopNs"] <= header["captureStartNs"],
              "overlap-v2-capture-boundary-invalid")
    expected_size = header["headerSize"] + header["reserved"] * header["recordSize"]
    reject_if(len(data) != expected_size, "overlap-v2-file-size-invalid",
              f"expected={expected_size} actual={len(data)}")

    records: dict[int, list[dict[str, int]]] = {kind: [] for kind in range(1, 6)}
    known_commit_mask = 0xFF | (0xF << 8)
    for index in range(header["reserved"]):
        offset = header["headerSize"] + index * header["recordSize"]
        begin, end, primary, related, argument, size, commit = (
            OVERLAP_RECORD_V2.unpack_from(data, offset)
        )
        kind = commit & 0xFF
        reject_if(commit == 0, "overlap-v2-uncommitted-record", str(index))
        reject_if(commit & ~known_commit_mask, "overlap-v2-unknown-record-flags", str(index))
        reject_if(kind not in records, "overlap-v2-record-kind-invalid", str(kind))
        reject_if(commit & ((1 << 9) | (1 << 10) | (1 << 11)),
                  "overlap-v2-record-inexact", str(index))


        reject_if(begin < header["captureStartNs"] or
                  begin > header["captureStopNs"] or end < begin,
                  "overlap-v2-record-outside-capture", str(index))
        reject_if(primary == 0, "overlap-v2-primary-id-zero", str(index))
        records[kind].append({
            "begin": begin, "end": end, "primary": primary, "related": related,
            "argument": argument, "size": size, "commit": commit,
        })

    def unique_by(items: Sequence[Mapping[str, int]], field: str,
                  reason: str) -> dict[int, Mapping[str, int]]:
        result: dict[int, Mapping[str, int]] = {}
        for item in items:
            key = item[field]
            reject_if(key in result, reason, str(key))
            result[key] = item
        return result

    waits = unique_by(records[1], "primary", "overlap-v2-duplicate-request")
    mappings = unique_by(records[2], "primary", "overlap-v2-duplicate-request-map")
    drains = unique_by(records[3], "primary", "overlap-v2-duplicate-drain-batch")
    boundaries = unique_by(records[4], "related", "overlap-v2-duplicate-frame-epoch")
    presents = unique_by(records[5], "related", "overlap-v2-duplicate-present-epoch")
    batch_map_counts: dict[int, int] = {}
    causal_pairs = 0
    causal_epochs: set[int] = set()
    for request_id, mapping in mappings.items():
        wait = waits.get(request_id)
        drain = drains.get(mapping["related"])
        reject_if(wait is None, "overlap-v2-map-without-request", str(request_id))
        reject_if(drain is None, "overlap-v2-map-without-drain", str(mapping["related"]))
        assert wait is not None and drain is not None
        batch_map_counts[mapping["related"]] = batch_map_counts.get(mapping["related"], 0) + 1
        epoch = mapping["argument"]
        reject_if(epoch == 0 or drain["related"] != epoch or drain["argument"] != epoch,
                  "overlap-v2-batch-epoch-mismatch", str(request_id))
        reject_if(mapping["begin"] != mapping["end"] or
                  mapping["begin"] != drain["begin"] or
                  not (wait["begin"] <= mapping["begin"] <= wait["end"]),
                  "overlap-v2-request-batch-time-mismatch", str(request_id))
        boundary = boundaries.get(epoch)
        present = presents.get(epoch)
        if boundary is None or present is None:
            continue
        if not (drain["end"] <= boundary["begin"] <= present["begin"]):
            continue
        if not (wait["commit"] & (1 << 8)) or not (present["commit"] & (1 << 8)):
            continue
        causal_pairs += 1
        causal_epochs.add(epoch)
    for batch_id, drain in drains.items():
        reject_if(batch_map_counts.get(batch_id, 0) != drain["size"],
                  "overlap-v2-batch-cardinality-mismatch", str(batch_id))
    reject_if(causal_pairs <= 0, "overlap-v2-no-complete-causal-membership")
    return {
        "schema": "lsx4-invalidation-overlap/v2",
        "instrumented": True,
        "productionFrametime": False,
        "delayCausalityClaimed": False,
        "causalMembershipExact": True,
        "causalPairs": causal_pairs,
        "causalEpochs": len(causal_epochs),
        "recordCounts": {str(kind): len(items) for kind, items in records.items()},
        "header": header,
    }


def validate_runtime_counters(
    runtime: Mapping[str, Any], *, mode: str, pid: int,
    required_metrics: Sequence[str], previous: Mapping[str, Any] | None = None,
) -> None:
    reject_if(runtime.get("schema") != "lsx4-runtime-proof-counters/v1",
              "runtime-counter-schema-invalid")
    reject_if(runtime.get("source") != "funnel-runtime-diagnostic-v1",
              "runtime-counter-source-invalid")
    reject_if(runtime.get("effectiveBackend") not in {"fex-experimental", "FEX"},
              "runtime-counter-backend-not-fex")
    reject_if(runtime.get("pid") != pid, "runtime-counter-pid-mismatch")
    reject_if(runtime.get("instrumentationState") != mode,
              "runtime-counter-mode-mismatch")
    reject_if(runtime.get("queryStatus") != 0 or runtime.get("status") != 0,
              "runtime-counter-query-failed")
    reject_if(runtime.get("abiVersion") != 1 or runtime.get("structSize") != 256,
              "runtime-counter-abi-invalid")
    requested = runtime.get("requestedMask")
    valid = runtime.get("validMask")
    unavailable = runtime.get("unavailableMask")
    exact = runtime.get("exactMask")
    sampled = runtime.get("sampledMask")
    static = runtime.get("staticMask")
    dynamic = runtime.get("dynamicMask")
    perturbed = runtime.get("perturbedMask")
    attempted = runtime.get("attemptedNotRetiredMask")
    capture_flags = runtime.get("captureFlags")
    saturated = runtime.get("saturated")
    for name, value in (
        ("requested", requested), ("valid", valid), ("unavailable", unavailable),
        ("exact", exact), ("sampled", sampled), ("static", static),
        ("dynamic", dynamic), ("perturbed", perturbed), ("attempted", attempted),
        ("captureFlags", capture_flags), ("saturated", saturated),
    ):
        reject_if(isinstance(value, bool) or not isinstance(value, int) or value < 0,
                  "runtime-counter-mask-invalid", name)
    all_mask = sum(RUNTIME_METRIC_BITS.values())
    reject_if(requested != all_mask, "runtime-counter-request-mask-incomplete")
    reject_if((valid | unavailable) != requested or valid & unavailable,
              "runtime-counter-availability-partition-invalid")
    reject_if(exact != valid or sampled != 0 or static != 0,
              "runtime-counter-semantic-mask-invalid")
    values = runtime.get("values")
    reject_if(not isinstance(values, Mapping), "runtime-counter-values-missing")
    for metric, bit in RUNTIME_METRIC_BITS.items():
        value = values.get(metric)
        reject_if(isinstance(value, bool) or not isinstance(value, int) or value < 0,
                  "runtime-counter-value-invalid", metric)
        reject_if(bool(unavailable & bit) and value != 0,
                  "runtime-counter-unavailable-nonzero", metric)
    if mode == "off":
        reject_if(valid != 0 or unavailable != requested or dynamic != 0 or
                  perturbed != 0 or attempted != 0 or capture_flags != 0 or
                  saturated != 0 or runtime.get("epoch") != 0 or
                  runtime.get("sequence") != 0,
                  "runtime-counter-off-state-invalid")
        reject_if(required_metrics, "runtime-counter-required-while-off")
        return
    reject_if(mode != "dynamic-diagnostic", "runtime-counter-mode-invalid", mode)
    required_dynamic_mask = sum(
        RUNTIME_METRIC_BITS[name] for name in RUNTIME_INVALIDATION_REQUIRED
    )
    supported_mask = all_mask & ~RUNTIME_METRIC_BITS["fastmemMisses"]
    reject_if((valid & required_dynamic_mask) != required_dynamic_mask or
              valid & ~supported_mask,
              "runtime-counter-supported-mask-invalid")
    reject_if(dynamic != valid or perturbed != valid,
              "runtime-counter-dynamic-mask-invalid")
    expected_attempted = (
        RUNTIME_METRIC_BITS["guestInvalidationRequests"] |
        RUNTIME_METRIC_BITS["ownedGeneratedCodeWriteFaults"]
    ) & valid
    reject_if(attempted != expected_attempted,
              "runtime-counter-attempt-semantics-invalid")
    expected_capture_flags = 0x3
    if valid & RUNTIME_METRIC_BITS["ownedGeneratedCodeWriteFaults"]:
        expected_capture_flags |= 0x4
    reject_if(capture_flags != expected_capture_flags,
              "runtime-counter-capture-flags-invalid",
              f"{capture_flags} != {expected_capture_flags}")
    reject_if(saturated != 0, "runtime-counter-saturated")
    reject_if(isinstance(runtime.get("epoch"), bool) or
              not isinstance(runtime.get("epoch"), int) or
              runtime["epoch"] <= 0 or
              isinstance(runtime.get("sequence"), bool) or
              not isinstance(runtime.get("sequence"), int) or
              runtime["sequence"] <= 0,
              "runtime-counter-live-identity-missing")
    for metric in required_metrics:
        bit = RUNTIME_METRIC_BITS.get(metric)
        reject_if(bit is None, "runtime-counter-name-unknown", metric)
        reject_if(unavailable & bit, "runtime-counter-unavailable", metric)
        reject_if(not (valid & bit), "runtime-counter-not-valid", metric)



    fastmem = RUNTIME_METRIC_BITS["fastmemMisses"]
    reject_if(not (unavailable & fastmem) or valid & fastmem,
              "runtime-fastmem-not-explicitly-unavailable")
    if previous is not None:
        for field in ("instrumentationState", "requestedMask", "validMask",
                      "unavailableMask", "exactMask", "dynamicMask",
                      "perturbedMask", "attemptedNotRetiredMask", "captureFlags"):
            reject_if(runtime.get(field) != previous.get(field),
                      "runtime-counter-snapshot-mismatch", field)
        reject_if(runtime.get("epoch") != previous.get("epoch"),
                  "runtime-counter-epoch-mixed")
        reject_if(not isinstance(runtime.get("sequence"), int) or
                  runtime["sequence"] <= previous.get("sequence", -1),
                  "runtime-counter-sequence-stale")
        previous_values = previous.get("values")
        reject_if(not isinstance(previous_values, Mapping),
                  "runtime-counter-start-values-missing")
        for metric in required_metrics:
            reject_if(values[metric] < previous_values.get(metric, 0),
                      "runtime-counter-regressed", metric)


def validate_hud_counters(hud: Mapping[str, Any], *, pid: int) -> None:
    reject_if(hud.get("schema") != HUD_COUNTER_SCHEMA,
              "proof-hud-schema-invalid")
    reject_if(hud.get("source") != "runtimeHudStats-exact-cumulative",
              "proof-hud-source-invalid")
    reject_if(hud.get("pid") != pid, "proof-hud-pid-mismatch")
    before = hud.get("bootBeforeNs")
    after = hud.get("bootAfterNs")
    reject_if(isinstance(before, bool) or not isinstance(before, int) or
              isinstance(after, bool) or not isinstance(after, int) or
              before <= 0 or after < before,
              "proof-hud-clock-invalid")
    indices = hud.get("indices")
    expected_indices = {
        "generatedBlocks": 0,
        "drawCalls": 6,
        "submitCalls": 8,
        "presentCalls": 10,
    }
    reject_if(indices != expected_indices, "proof-hud-index-contract-invalid")
    value_count = hud.get("valueCount")
    values = hud.get("rawValues")
    counters = hud.get("counters")
    reject_if(isinstance(value_count, bool) or not isinstance(value_count, int)
              or value_count < 11 or not isinstance(values, list)
              or len(values) != value_count,
              "proof-hud-values-invalid")
    reject_if(any(isinstance(value, bool) or not isinstance(value, int) or value < 0
                  for value in values), "proof-hud-value-invalid")
    reject_if(not isinstance(counters, Mapping) or set(counters) != set(expected_indices),
              "proof-hud-counters-invalid")
    for name, index in expected_indices.items():
        reject_if(counters.get(name) != values[index],
                  "proof-hud-counter-index-mismatch", name)


def hud_draw_rate_lower_bound(start: Mapping[str, Any],
                              end: Mapping[str, Any], *, pid: int) -> dict[str, Any]:
    validate_hud_counters(start, pid=pid)
    validate_hud_counters(end, pid=pid)
    start_counters = start["counters"]
    end_counters = end["counters"]
    deltas: dict[str, int] = {}
    for name in start_counters:
        delta = int(end_counters[name]) - int(start_counters[name])
        reject_if(delta < 0, "proof-hud-counter-regressed", name)
        deltas[name] = delta
    reject_if(int(end["bootBeforeNs"]) <= int(start["bootAfterNs"]),
              "proof-hud-snapshot-order-invalid")
    maximum_interval_ns = int(end["bootAfterNs"]) - int(start["bootBeforeNs"])
    reject_if(maximum_interval_ns <= 0, "proof-hud-interval-invalid")
    draw_lower_bound = deltas["drawCalls"] * 1_000_000_000.0 / maximum_interval_ns
    return {
        "source": "runtimeHudStats-exact-cumulative",
        "deltas": deltas,
        "maximumCounterIntervalNs": maximum_interval_ns,
        "drawCallsPerSecondLowerBound": draw_lower_bound,
        "fpsVerdictAllowed": False,
    }


def validate_proof_snapshot(
    snapshot: Mapping[str, Any], *, mode: str, token: str, pid: int,
    process_starttime: int,
    required_metrics: Sequence[str], runtime_mode: str,
    runtime_required_metrics: Sequence[str],
    previous: Mapping[str, Any] | None = None,
) -> None:
    reject_if(snapshot.get("schema") != PROOF_SNAPSHOT_SCHEMA,
              "proof-snapshot-schema-invalid")
    reject_if(snapshot.get("requestToken") != token, "proof-snapshot-token-mismatch")
    reject_if(snapshot.get("pid") != pid, "proof-snapshot-pid-mismatch")
    reject_if(snapshot.get("processStarttime") != process_starttime,
              "proof-snapshot-starttime-mismatch")
    boot_before = snapshot.get("bootBeforeNs")
    boot_after = snapshot.get("bootAfterNs")
    reject_if(isinstance(boot_before, bool) or not isinstance(boot_before, int) or
              isinstance(boot_after, bool) or not isinstance(boot_after, int) or
              boot_before <= 0 or boot_after < boot_before,
              "proof-snapshot-boot-boundary-invalid")
    fex = snapshot.get("fex")
    runtime = snapshot.get("runtime")
    hud = snapshot.get("hud")
    reject_if(not isinstance(fex, Mapping), "proof-snapshot-fex-missing")
    reject_if(not isinstance(runtime, Mapping), "proof-snapshot-runtime-missing")
    reject_if(not isinstance(hud, Mapping), "proof-snapshot-hud-missing")
    validate_hud_counters(hud, pid=pid)
    nested_boundaries = (
        fex.get("bootBeforeNs"), fex.get("bootAfterNs"),
        runtime.get("bootBeforeNs"), runtime.get("bootAfterNs"),
        hud.get("bootBeforeNs"), hud.get("bootAfterNs"),
    )
    reject_if(any(isinstance(value, bool) or not isinstance(value, int)
                  for value in nested_boundaries),
              "proof-snapshot-nested-boundary-invalid")
    reject_if(not (
        boot_before <= nested_boundaries[0] <= nested_boundaries[1] <=
        nested_boundaries[2] <= nested_boundaries[3] <=
        nested_boundaries[4] <= nested_boundaries[5] <= boot_after
    ), "proof-snapshot-nested-boundary-order-invalid")
    reject_if(fex.get("schema") != "lsx4-fex-proof-metrics/v1",
              "proof-fex-schema-invalid")
    reject_if(fex.get("source") != "fex-runtime-diagnostic-v1",
              "proof-fex-source-invalid")
    reject_if(fex.get("effectiveBackend") not in {"fex-experimental", "FEX"},
              "proof-snapshot-backend-not-fex")
    reject_if(fex.get("pid") != pid, "proof-fex-pid-mismatch")
    reject_if(fex.get("instrumentationState") != mode,
              "proof-snapshot-mode-mismatch")
    reject_if(fex.get("queryStatus") != 0 or fex.get("status") != 0,
              "proof-snapshot-query-failed")
    reject_if(fex.get("abiVersion") != 1 or fex.get("structSize") != 256,
              "proof-snapshot-abi-invalid")
    requested = fex.get("requestedMask")
    valid = fex.get("validMask")
    unavailable = fex.get("unavailableMask")
    exact = fex.get("exactMask")
    sampled = fex.get("sampledMask")
    static = fex.get("staticMask")
    dynamic = fex.get("dynamicMask")
    perturbed = fex.get("perturbedMask")
    attempted = fex.get("attemptedNotRetiredMask")
    capture_flags = fex.get("captureFlags")
    for name, value in (("requested", requested), ("valid", valid),
                        ("unavailable", unavailable), ("exact", exact),
                        ("sampled", sampled), ("static", static),
                        ("dynamic", dynamic), ("perturbed", perturbed),
                        ("attempted", attempted),
                        ("captureFlags", capture_flags)):
        reject_if(isinstance(value, bool) or not isinstance(value, int) or value < 0,
                  "proof-snapshot-mask-invalid", name)
    all_fex_mask = sum(PROOF_METRIC_BITS.values())
    reject_if(requested != all_fex_mask,
              "proof-snapshot-request-mask-incomplete")
    reject_if(valid & unavailable, "proof-snapshot-valid-unavailable-overlap")
    reject_if((valid | unavailable) != requested,
              "proof-snapshot-availability-partition-invalid")
    reject_if(valid & ~requested, "proof-snapshot-unrequested-valid")
    reject_if(exact & ~valid, "proof-snapshot-exact-without-valid")
    reject_if(sampled != 0, "proof-snapshot-sampled-metrics-forbidden")
    reject_if(exact != valid, "proof-snapshot-exact-mask-invalid")
    reject_if((static | dynamic) != valid or static & dynamic,
              "proof-snapshot-kind-partition-invalid")
    reject_if((perturbed | attempted) & ~valid,
              "proof-snapshot-semantic-mask-outside-valid")
    unavailable_names = {
        name for name, bit in PROOF_METRIC_BITS.items() if unavailable & bit
    }
    reject_if(not FEX_UNPROVABLE_TOTALS.issubset(unavailable_names),
              "proof-snapshot-unprovable-total-advertised",
              ",".join(sorted(FEX_UNPROVABLE_TOTALS - unavailable_names)))
    reject_if(attempted != 0, "proof-snapshot-false-instruction-entry-claim")
    if mode == "off":
        reject_if(valid != 0 or unavailable != requested or
                  capture_flags != 0 or static != 0 or dynamic != 0 or
                  perturbed != 0,
                  "proof-snapshot-off-state-invalid")
    elif mode == "static":
        reject_if(dynamic != 0 or perturbed != 0 or static != valid,
                  "proof-snapshot-static-semantics-invalid")
        reject_if(capture_flags & ~0x3 or not (capture_flags & 0x2),
                  "proof-snapshot-capture-flags-invalid")
    elif mode == "dynamic":
        block_bit = PROOF_METRIC_BITS["executedIrBlockEntries"]
        reject_if(dynamic != block_bit or perturbed != block_bit or
                  static != (valid & ~block_bit),
                  "proof-snapshot-dynamic-semantics-invalid")
        reject_if(capture_flags & ~0x3 or not (capture_flags & 0x2),
                  "proof-snapshot-capture-flags-invalid")
    else:
        raise CampaignRejected("proof-snapshot-mode-invalid", mode)
    values = fex.get("values")
    reject_if(not isinstance(values, Mapping), "proof-snapshot-values-missing")
    for metric, bit in PROOF_METRIC_BITS.items():
        value = values.get(metric)
        reject_if(isinstance(value, bool) or not isinstance(value, int) or value < 0,
                  "proof-metric-value-invalid", metric)
        reject_if(bool(unavailable & bit) and value != 0,
                  "proof-metric-unavailable-nonzero", metric)
    for metric in required_metrics:
        bit = PROOF_METRIC_BITS.get(metric)
        reject_if(bit is None, "proof-metric-name-unknown", metric)
        reject_if(not (requested & bit), "proof-metric-not-requested", metric)
        reject_if(unavailable & bit, "proof-metric-unavailable", metric)
        reject_if(not (valid & bit), "proof-metric-not-valid", metric)
        reject_if(not (exact & bit), "proof-metric-not-exact", metric)
    if previous is not None:
        previous_fex = previous.get("fex")
        reject_if(not isinstance(previous_fex, Mapping), "proof-start-fex-missing")
        for field in ("instrumentationState", "requestedMask", "validMask",
                      "unavailableMask", "exactMask", "staticMask", "dynamicMask",
                      "perturbedMask", "attemptedNotRetiredMask", "captureFlags"):
            reject_if(fex.get(field) != previous_fex.get(field),
                      "proof-snapshot-pair-mismatch", field)
        reject_if(fex.get("epoch") != previous_fex.get("epoch"),
                  "proof-snapshot-epoch-mixed")
        reject_if(not isinstance(fex.get("sequence"), int) or
                  fex["sequence"] <= previous_fex.get("sequence", -1),
                  "proof-snapshot-sequence-stale")
        previous_values = previous_fex.get("values")
        reject_if(not isinstance(previous_values, Mapping), "proof-start-values-missing")
        for metric in required_metrics:
            reject_if(values[metric] < previous_values.get(metric, 0),
                      "proof-metric-regressed", metric)
    previous_runtime = previous.get("runtime") if previous is not None else None
    reject_if(previous_runtime is not None and not isinstance(previous_runtime, Mapping),
              "runtime-counter-start-missing")
    if previous is not None:
        previous_hud = previous.get("hud")
        reject_if(not isinstance(previous_hud, Mapping),
                  "proof-hud-start-missing")
        hud_draw_rate_lower_bound(previous_hud, hud, pid=pid)
    validate_runtime_counters(
        runtime, mode=runtime_mode, pid=pid,
        required_metrics=runtime_required_metrics,
        previous=previous_runtime,
    )


@dataclasses.dataclass(frozen=True)
class CommandResult:
    argv: tuple[str, ...]
    returncode: int
    stdout: bytes
    stderr: bytes


def require_unambiguous_absence(result: CommandResult, *, label: str) -> None:
    if result.returncode == 0:
        raise CampaignRejected("preexisting-artifact", label)
    reject_if(result.returncode != 1 or bool(result.stdout.strip()) or
              bool(result.stderr.strip()),
              "artifact-absence-probe-ambiguous",
              f"{label}: rc={result.returncode} "
              f"stdout={result.stdout[:200]!r} stderr={result.stderr[:200]!r}")


class HostCommands:
    def run(self, argv: Sequence[str], *, timeout: float = 120.0,
            allow_failure: bool = False) -> CommandResult:
        try:
            completed = subprocess.run(
                list(argv), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                timeout=timeout, check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            raise CampaignRejected("command-execution-failed", f"{argv!r}: {error}") from error
        result = CommandResult(tuple(argv), completed.returncode,
                               completed.stdout, completed.stderr)
        if completed.returncode != 0 and not allow_failure:
            raise CampaignRejected(
                "command-returned-nonzero",
                f"rc={completed.returncode} argv={list(argv)!r} "
                f"stderr={completed.stderr.decode('utf-8', 'replace')[:500]!r}",
            )
        return result


class GuardedAdb:
    def __init__(self, host: HostCommands, *, adb: str, target: str,
                 agentchat: pathlib.Path, agent: str, room: str):
        self.host = host
        self.adb = adb
        self.target = target
        self.agentchat = agentchat
        self.agent = agent
        self.room = room
        self.sequence = 0

    def _agentchat(self, args: Sequence[str]) -> None:
        self.host.run([sys.executable, str(self.agentchat), *args], timeout=30)

    def gate(self, summary: str) -> None:
        self.sequence += 1
        self._agentchat([
            "event", "--agent", self.agent, "--room", self.room,
            "--action", "adb-bloodborne-proof",
            "--target", "device:vivo",
            "--summary", f"campaign adb #{self.sequence}: {summary}",
        ])
        for area in ("device:vivo", f"adb:{EXPECTED_SERIAL}"):
            self._agentchat([
                "own", "--agent", self.agent, "--room", self.room,
                "--area", area,
            ])

    def run(self, args: Sequence[str], *, summary: str, timeout: float = 120.0,
            allow_failure: bool = False) -> CommandResult:
        self.gate(summary)
        return self.host.run(
            [self.adb, "-s", self.target, *args], timeout=timeout,
            allow_failure=allow_failure,
        )


class ExclusiveStore:
    def __init__(self, root: pathlib.Path):
        reject_if(root.exists(), "artifact-directory-already-exists", str(root))
        root.mkdir(parents=True, exist_ok=False)
        self.root = root

    def write(self, relative: str, data: bytes) -> pathlib.Path:
        path = self.root / relative
        reject_if(path.is_absolute() and self.root not in path.parents,
                  "artifact-path-escaped", str(path))
        path.parent.mkdir(parents=True, exist_ok=True)
        try:
            with path.open("xb") as stream:
                stream.write(data)
        except FileExistsError as error:
            raise CampaignRejected("artifact-file-already-exists", relative) from error
        return path

    def json(self, relative: str, value: Any) -> pathlib.Path:
        return self.write(relative, canonical_json(value))

    def manifest(self) -> dict[str, str]:
        result: dict[str, str] = {}
        for path in sorted(self.root.rglob("*")):
            if path.is_file() and path.name not in {"manifest.json", "SEALED.json"}:
                result[path.relative_to(self.root).as_posix()] = sha256_file(path)
        return result


class CampaignExecutor:
    def __init__(self, plan: Mapping[str, Any], *, route: Mapping[str, Any],
                 store: ExclusiveStore, adb: GuardedAdb, host: HostCommands,
                 repo_root: pathlib.Path, agentchat: pathlib.Path,
                 agent: str, room: str):
        self.plan = plan
        self.route = route
        self.store = store
        self.adb = adb
        self.host = host
        self.repo_root = repo_root
        self.agentchat = agentchat
        self.agent = agent
        self.room = room
        self.phase_start_thermal: list[int] = []
        self.phase_start_surface_c: list[float] = []
        self.phase_start_external_power: list[bool] = []
        self.phase_start_cpu_policy_limits: list[tuple[int, int, int, int]] = []
        self._last_overlap_ready = ""

    @staticmethod
    def _text(result: CommandResult) -> str:
        return result.stdout.decode("utf-8", "strict").replace("\r", "")

    def _identity(self) -> None:
        result = self.adb.run(["shell", "getprop", "ro.serialno"],
                              summary="verify physical Vivo identity")
        actual = self._text(result).strip()
        reject_if(actual != EXPECTED_SERIAL, "device-ro-serial-mismatch", actual)

    def _force_stop(self, summary: str) -> None:
        self.adb.run(["shell", "am", "force-stop", PACKAGE], summary=summary)

    def _thermal_status(self) -> int:




        dump = self.adb.run(
            ["shell", "dumpsys", "thermalservice"],
            summary="read thermal status for matched phase band",
            allow_failure=True,
        )
        dump_text = self._text(dump)
        values = re.findall(r"(?m)^Thermal Status:\s*([0-6])\s*$", dump_text)
        reject_if(len(set(values)) > 1, "thermal-status-ambiguous", repr(values))
        if dump.returncode == 0 and len(set(values)) == 1:
            return int(values[0])

        fallback = self.adb.run(
            ["shell", "cmd", "thermalservice", "get-status"],
            summary="fallback read thermal status for matched phase band",
            allow_failure=True,
        )
        text = self._text(fallback).strip().lower()
        match = re.fullmatch(r"(?:thermal status:\s*)?([0-6])", text)
        names = {"none": 0, "light": 1, "moderate": 2, "severe": 3,
                 "critical": 4, "emergency": 5, "shutdown": 6}
        if fallback.returncode == 0 and match:
            return int(match.group(1))
        if fallback.returncode == 0:
            matched_names = [value for name, value in names.items()
                             if re.fullmatch(rf"(?:thermal status:\s*)?{name}", text)]
            if len(matched_names) == 1:
                return matched_names[0]
        raise CampaignRejected(
            "thermal-status-unavailable",
            f"dumpsys-rc={dump.returncode} cmd-rc={fallback.returncode} "
            f"dumpsys={dump_text[:200]!r} cmd={text[:200]!r}",
        )

    def _cooldown(self) -> int:
        thermal = self.route["thermal"]
        deadline = time.monotonic() + int(thermal["cooldownTimeoutSeconds"])
        while True:
            status = self._thermal_status()
            if status <= int(thermal["maximumStatus"]):
                return status
            reject_if(time.monotonic() >= deadline, "thermal-cooldown-timeout", str(status))
            time.sleep(min(60, int(thermal["cooldownPollSeconds"])))

    def _preflight(self, cleanup_only: bool) -> str:


        self.adb.gate("mandatory Vivo performance preflight")
        thermal = self.route["thermal"]
        argv = [




            "pwsh", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
            str(self.repo_root / "scripts" / "preflight-vivo-performance-test.ps1"),
            "-DeviceId", EXPECTED_SERIAL,
            "-AdbTarget", self.plan["device"]["adbTarget"],
            "-MinimumCpuIdlePercent", str(thermal["minimumCpuIdlePercent"]),
            "-MinimumAvailableMemoryMb", str(thermal["minimumAvailableMemoryMb"]),
            "-MaximumThermalStatus", str(thermal["maximumStatus"]),
            "-MaximumSurfaceTemperatureC", str(thermal["maximumSurfaceC"]),
            "-MinimumBatteryLevelPercent",
            str(thermal["minimumBatteryLevelPercent"]),
            "-MinimumPolicy0MaxKHz",
            str(thermal["minimumCpuPolicyMaxKHz"]["0"]),
            "-MinimumPolicy6MaxKHz",
            str(thermal["minimumCpuPolicyMaxKHz"]["6"]),
        ]
        if cleanup_only:
            argv.append("-CleanupOnly")
        result = self.host.run(argv, timeout=180)
        text = self._text(result)
        reject_if("PASSED" not in text, "performance-preflight-not-passed")
        return text

    def _set_markers(self, enabled: Sequence[str]) -> str:
        enabled_set = set(enabled)
        reject_if(MULTIBLOCK_MARKER in enabled_set, "multiblock-quarantine-violated")
        paths = [f"{MARKER_ROOT}/{name}" for name in sorted(MANAGED_MARKERS)]
        desired_paths = [f"{MARKER_ROOT}/{name}" for name in sorted(enabled_set)]


        script = "set -eu; mkdir -p 'files/lsx4-home'; "
        script += "rm -f " + " ".join(f"'{path}'" for path in paths) + "; "
        if desired_paths:
            script += "touch " + " ".join(f"'{path}'" for path in desired_paths) + "; "
        script += "for p in " + " ".join(f"'{path}'" for path in paths) + "; do "
        script += "if [ -e \"$p\" ]; then basename \"$p\"; fi; done"
        result = self.adb.run(
            ["exec-out", "run-as", PACKAGE, "sh", "-c", script],
            summary="set exact single-axis marker state",
        )
        actual = sorted(line.strip() for line in self._text(result).splitlines() if line.strip())
        reject_if(actual != sorted(enabled_set), "marker-state-verification-failed",
                  f"expected={sorted(enabled_set)!r} actual={actual!r}")
        return "\n".join(actual) + ("\n" if actual else "")

    def _prepare_overlap_v2(self) -> None:
        names = ("arm", "ready", "ready.tmp", "stop", "done", "error",
                 "trace-v1.bin", "trace-v2.bin", "abi-v2")
        paths = [f"{OVERLAP_CONTROL_ROOT}/{name}" for name in names]
        script = (
            f"set -eu; mkdir -p '{OVERLAP_CONTROL_ROOT}'; rm -f " +
            " ".join(f"'{path}'" for path in paths) + "; " +
            f"touch '{OVERLAP_CONTROL_ROOT}/abi-v2'; " +
            f"test -f '{OVERLAP_CONTROL_ROOT}/abi-v2'"
        )
        self.adb.run(
            ["shell", "run-as", PACKAGE, "sh", "-c", script],
            summary="prepare explicit ABI-v2 overlap selector and no stale trace",
        )

    def _wait_app_file(self, path: str, *, timeout_seconds: float,
                       summary: str) -> None:
        deadline = time.monotonic() + timeout_seconds
        while True:
            result = self.adb.run(
                ["shell", "run-as", PACKAGE, "test", "-s", path],
                summary=summary, allow_failure=True,
            )
            if result.returncode == 0:
                return
            reject_if(time.monotonic() >= deadline, "app-file-wait-timeout", path)
            time.sleep(0.25)

    def _arm_overlap_v2(self) -> str:
        selector = f"{OVERLAP_CONTROL_ROOT}/abi-v2"
        selector_probe = self.adb.run(
            ["shell", "run-as", PACKAGE, "test", "!", "-e", selector],
            summary="verify ABI-v2 selector was consumed by existing runtime",
            allow_failure=True,
        )
        reject_if(selector_probe.returncode != 0,
                  "overlap-v2-selector-not-consumed")
        self.adb.run(
            ["shell", "run-as", PACKAGE, "touch", f"{OVERLAP_CONTROL_ROOT}/arm"],
            summary="arm causal overlap v2 capture",
        )
        ready_path = f"{OVERLAP_CONTROL_ROOT}/ready"
        self._wait_app_file(ready_path, timeout_seconds=10,
                            summary="wait for overlap v2 ready")
        ready = self.adb.run(
            ["exec-out", "run-as", PACKAGE, "cat", ready_path],
            summary="read overlap v2 ready contract",
        )
        text = self._text(ready)
        for token in (
            "enabled=1", "abi=2", "causalMembership=1", "delayCausality=0",
            "perturbed=1", "productionFrametime=0",
        ):
            reject_if(token not in text, "overlap-v2-ready-contract-invalid", token)
        self._last_overlap_ready = text
        return text

    def _stop_overlap_v2(self, phase_dir: pathlib.Path) -> dict[str, Any]:
        self.adb.run(
            ["shell", "run-as", PACKAGE, "touch", f"{OVERLAP_CONTROL_ROOT}/stop"],
            summary="stop causal overlap v2 capture",
        )
        done_path = f"{OVERLAP_CONTROL_ROOT}/done"
        self._wait_app_file(done_path, timeout_seconds=10,
                            summary="wait for quiesced overlap v2 trace")
        done_result = self.adb.run(
            ["exec-out", "run-as", PACKAGE, "cat", done_path],
            summary="read overlap v2 quiescence contract",
        )
        done = self._text(done_result)
        for token in (
            "dumped=1", "exact=1", "structuralMappingExact=1",
            "causalVerdict=parser-required", "saturated=0", "droppedEntries=0",
            "activeSpans=0", "timedOut=0",
        ):
            reject_if(token not in done, "overlap-v2-done-contract-invalid", token)
        trace = self.adb.run(
            ["exec-out", "run-as", PACKAGE, "cat",
             f"{OVERLAP_CONTROL_ROOT}/trace-v2.bin"],
            summary="pull quiesced immutable overlap v2 trace",
            timeout=120,
        )
        reject_if(not trace.stdout, "overlap-v2-trace-empty")
        prefix = phase_dir.relative_to(self.store.root).as_posix()
        self.store.write(f"{prefix}/overlap-ready.txt",
                         self._last_overlap_ready.encode("utf-8"))
        self.store.write(f"{prefix}/overlap-done.txt", done_result.stdout)
        self.store.write(f"{prefix}/overlap-trace-v2.bin", trace.stdout)
        parsed = parse_overlap_v2(trace.stdout)
        self.store.json(f"{prefix}/overlap-v2.json", parsed)
        return parsed

    def _capture_visual_anchor(self, anchor_id: str, phase_dir: pathlib.Path,
                               attempt: int) -> dict[str, Any]:
        anchors = self.route["visualAnchors"]
        anchor = anchors.get(anchor_id)
        reject_if(not isinstance(anchor, Mapping),
                  "scene-anchor-missing", anchor_id)
        result = self.adb.run(
            ["exec-out", "screencap", "-p"],
            summary=f"capture read-only visual scene anchor {anchor_id}",
            timeout=60,
        )
        reject_if(bool(result.stderr.strip()), "scene-anchor-screencap-stderr",
                  result.stderr.decode("utf-8", "replace")[:500])
        observed = roi_dhash_1024(result.stdout, anchor)
        expected = str(anchor["descriptorHex"])
        distance = dhash_hamming_distance(observed, expected)
        return {
            "anchorId": anchor_id,
            "attempt": attempt,
            "algorithm": anchor["algorithm"],
            "roi": dict(anchor["roi"]),
            "expectedDescriptorHex": expected,
            "observedDescriptorHex": observed,
            "hammingDistance": distance,
            "maximumHammingDistance": int(anchor["maximumHammingDistance"]),
            "screenshotSha256": hashlib.sha256(result.stdout).hexdigest(),
            "matched": distance <= int(anchor["maximumHammingDistance"]),
            "png": result.stdout,
        }

    def _wait_visual_anchor(self, anchor_id: str, phase_dir: pathlib.Path,
                            *, timeout_seconds: int,
                            poll_seconds: int, label: str) -> dict[str, Any]:
        deadline = time.monotonic() + timeout_seconds
        attempts: list[dict[str, Any]] = []
        last: dict[str, Any] | None = None
        while True:
            last = self._capture_visual_anchor(anchor_id, phase_dir, len(attempts) + 1)
            png = last.pop("png")
            attempts.append(last)
            if last["matched"]:
                prefix = phase_dir.relative_to(self.store.root).as_posix()
                self.store.write(f"{prefix}/{label}-matched.png", png)
                result = {
                    "schema": "lsx4-visual-scene-gate/v1",
                    "accepted": True,
                    "anchorId": anchor_id,
                    "sourceEvidenceSha256": self.route["visualAnchors"][anchor_id][
                        "sourceEvidenceSha256"
                    ],
                    "attempts": attempts,
                    "fpsVerdictAllowed": False,
                }
                self.store.json(f"{prefix}/{label}.json", result)
                return result
            if time.monotonic() >= deadline:
                prefix = phase_dir.relative_to(self.store.root).as_posix()
                self.store.write(f"{prefix}/{label}-last-rejected.png", png)
                self.store.json(f"{prefix}/{label}.json", {
                    "schema": "lsx4-visual-scene-gate/v1",
                    "accepted": False,
                    "anchorId": anchor_id,
                    "attempts": attempts,
                    "fpsVerdictAllowed": False,
                })
                raise CampaignRejected("scene-anchor-timeout", anchor_id)
            time.sleep(poll_seconds)

    def _capture_movement_frame(
        self, evidence: Mapping[str, Any], phase_dir: pathlib.Path, label: str,
    ) -> dict[str, Any]:
        result = self.adb.run(
            ["exec-out", "screencap", "-p"],
            summary=f"capture read-only moving-route frame {label}",
            timeout=60,
        )
        reject_if(bool(result.stderr.strip()),
                  "movement-screencap-stderr",
                  result.stderr.decode("utf-8", "replace")[:500])
        descriptor = roi_dhash_1024(result.stdout, evidence)
        prefix = phase_dir.relative_to(self.store.root).as_posix()
        self.store.write(f"{prefix}/movement-{label}.png", result.stdout)
        return {
            "label": label,
            "descriptorHex": descriptor,
            "screenshotSha256": hashlib.sha256(result.stdout).hexdigest(),
        }

    def _verify_movement_scene_change(
        self, evidence: Mapping[str, Any], before: Mapping[str, Any],
        after: Mapping[str, Any], phase_dir: pathlib.Path,
    ) -> dict[str, Any]:
        result = movement_scene_change_metadata(
            str(before["descriptorHex"]), str(after["descriptorHex"]), evidence
        )
        result["beforeScreenshotSha256"] = before["screenshotSha256"]
        result["afterScreenshotSha256"] = after["screenshotSha256"]
        prefix = phase_dir.relative_to(self.store.root).as_posix()
        self.store.json(f"{prefix}/movement-scene-change.json", result)
        reject_if(result["sceneChangeProven"] is not True,
                  "movement-scene-change-below-floor",
                  f"distance={result['hammingDistance']} "
                  f"minimum={result['minimumHammingDistance']}")
        return result

    def _launch_route(self, phase_dir: pathlib.Path) -> list[dict[str, Any]]:
        self.adb.run([
            "shell", "am", "start", "-n", self.route["launcherComponent"],
            "--es", "launch_game_path", self.route["gamePath"],
            "--es", "game_platform", "ps4",
        ], summary="launch frozen Bloodborne route")
        time.sleep(int(self.route["launch"]["settleBeforeInputSeconds"]))
        gates: list[dict[str, Any]] = []
        route_actions = self.route["launch"]["actions"]
        for index, cross in enumerate(route_actions, start=1):
            self.adb.run([
                "shell", "input", "swipe", str(cross["x"]), str(cross["y"]),
                str(cross["x"]), str(cross["y"]), str(cross["holdMilliseconds"]),
            ], summary=f"route Cross hold {index}/{len(route_actions)}")
            gates.append(self._wait_visual_anchor(
                str(cross["stateAnchorId"]), phase_dir,
                timeout_seconds=int(cross["stateGateTimeoutSeconds"]),
                poll_seconds=int(cross["stateGatePollSeconds"]),
                label=f"route-state-{index:02d}-{cross['stateAnchorId']}",
            ))
        return gates

    def _pid(self) -> int:
        result = self.adb.run(["shell", "pidof", GAME_PROCESS],
                              summary="resolve unique current game pid")
        values = self._text(result).split()
        reject_if(len(values) != 1 or not values[0].isdigit(),
                  "game-pid-not-unique", repr(values))
        return int(values[0])

    def _process_identity(self) -> dict[str, Any]:
        pid = self._pid()
        stat = self.adb.run(
            ["exec-out", "run-as", PACKAGE, "cat", f"/proc/{pid}/stat"],
            summary="capture game pid starttime",
        )
        parsed_pid, comm, starttime = parse_proc_stat(self._text(stat))
        reject_if(parsed_pid != pid, "proc-stat-pid-mismatch")
        cmdline = self.adb.run(
            ["exec-out", "run-as", PACKAGE, "cat", f"/proc/{pid}/cmdline"],
            summary="capture game process cmdline",
        ).stdout.replace(b"\x00", b" ").decode("utf-8", "strict").strip()
        reject_if(cmdline != GAME_PROCESS, "game-cmdline-mismatch", cmdline)
        return {"pid": pid, "starttime": starttime, "comm": comm, "cmdline": cmdline}

    def _surface_identity(self) -> dict[str, Any]:
        result = self.adb.run(["shell", "dumpsys", "SurfaceFlinger", "--list"],
                              summary="capture exact BLAST layer identity")
        surface = select_exact_game_surface(
            self._text(result), self.route["surface"]["requiredComponent"]
        )
        reject_if(surface["generation"] < int(self.route["surface"]["minimumGeneration"]),
                  "surface-generation-below-route-minimum",
                  str(surface["generation"]))
        return surface

    def _role_times(self, pid: int) -> tuple[int, int]:
        gate = self.route["gameplayGate"]
        script = (
            "set -eu; havok=0; gpu=0; "
            f"for t in /proc/{pid}/task/*; do "
            "n=$(cat \"$t/comm\" 2>/dev/null || true); "
            "set -- $(cat \"$t/schedstat\" 2>/dev/null || echo '0 0 0'); "
            f"case \"$n\" in *'{gate['havokThreadPattern']}'*) havok=$((havok+$1));; esac; "
            f"case \"$n\" in *'{gate['gpuCommThreadPattern']}'*) gpu=$((gpu+$1));; esac; "
            "done; printf '%s %s\\n' \"$havok\" \"$gpu\""
        )
        result = self.adb.run(["shell", "sh", "-c", script],
                              summary="sample gameplay role schedstat")
        values = self._text(result).split()
        reject_if(len(values) != 2 or not all(value.isdigit() for value in values),
                  "gameplay-role-sample-invalid", repr(values))
        return int(values[0]), int(values[1])

    def _gameplay_gate(self, phase: Mapping[str, Any], phase_dir: pathlib.Path,
                       identity: Mapping[str, Any]) -> dict[str, Any]:
        gate = self.route["gameplayGate"]
        pid = int(identity["pid"])
        timeout = int(self.route["launch"]["postCrossGateTimeoutSeconds"])
        deadline = time.monotonic() + timeout
        required = int(gate["requiredConsecutivePasses"])
        probe_seconds = int(gate["probeSeconds"])
        streak = 0
        probes: list[dict[str, float]] = []
        while time.monotonic() < deadline:
            before = self._role_times(pid)
            time.sleep(probe_seconds)
            after = self._role_times(pid)
            havok = max(0.0, (after[0] - before[0]) / (probe_seconds * 10_000_000.0))
            gpu = max(0.0, (after[1] - before[1]) / (probe_seconds * 10_000_000.0))
            passed = (havok >= float(gate["minimumHavokPercent"]) and
                      gpu >= float(gate["minimumGpuCommPercent"]))
            probes.append({"havokPercent": havok, "gpuCommPercent": gpu,
                           "passed": passed})
            streak = streak + 1 if passed else 0
            if streak >= required:
                logcat = self.adb.run(
                    ["shell", "logcat", "-b", "all", "-d", "-v", "brief"],
                    summary="verify runtime gameplay-state log",
                )
                reject_if(not re.search(str(gate["requiredLogRegex"]),
                                        self._text(logcat)),
                          "gameplay-log-gate-missing")
                scene = self._wait_visual_anchor(
                    str(gate["sceneAnchorId"]), phase_dir,
                    timeout_seconds=int(gate["finalSceneRecheckTimeoutSeconds"]),
                    poll_seconds=int(gate["finalSceneRecheckPollSeconds"]),
                    label="gameplay-scene-recheck",
                )
                tokens = phase["gameplayGateProofTokens"]
                start = self._proof_snapshot(
                    str(tokens["start"]), phase_dir, "gameplay-draw-start", identity
                )
                validate_proof_snapshot(
                    start, mode=str(phase["instrumentationMode"]),
                    token=str(tokens["start"]), pid=pid,
                    process_starttime=int(identity["starttime"]),
                    required_metrics=phase["proofMetricsRequired"],
                    runtime_mode=str(phase["runtimeInstrumentationMode"]),
                    runtime_required_metrics=phase["runtimeMetricsRequired"],
                )
                time.sleep(int(gate["drawProbeSeconds"]))
                end = self._proof_snapshot(
                    str(tokens["end"]), phase_dir, "gameplay-draw-end", identity
                )
                validate_proof_snapshot(
                    end, mode=str(phase["instrumentationMode"]),
                    token=str(tokens["end"]), pid=pid,
                    process_starttime=int(identity["starttime"]),
                    required_metrics=phase["proofMetricsRequired"],
                    runtime_mode=str(phase["runtimeInstrumentationMode"]),
                    runtime_required_metrics=phase["runtimeMetricsRequired"],
                    previous=start,
                )
                draw = hud_draw_rate_lower_bound(start["hud"], end["hud"], pid=pid)
                reject_if(draw["drawCallsPerSecondLowerBound"] <=
                          float(gate["minimumDrawCallsPerSecond"]),
                          "gameplay-draw-floor-not-proven",
                          f"lowerBound={draw['drawCallsPerSecondLowerBound']:.3f}")
                return {
                    "probes": probes,
                    "scene": scene,
                    "draw": draw,
                    "runtimeLogGate": str(gate["requiredLogRegex"]),
                    "fpsVerdictAllowed": False,
                }
        raise CampaignRejected("hunters-dream-gameplay-gate-timeout")

    def _capture_provenance(self, phase_dir: pathlib.Path,
                            expected_path: pathlib.Path, label: str) -> None:
        output = phase_dir / label
        argv = [
            sys.executable,
            str(self.repo_root / "tools" / "bloodborne-proof" / "capture_provenance.py"),
            "--expected", str(expected_path), "--output", str(output),
            "--serial", self.plan["device"]["adbTarget"],
            "--agentchat", str(self.agentchat), "--agent", self.agent,
            "--room", self.room,
        ]
        self.host.run(argv, timeout=300)
        reject_if(not (output / "provenance.json").is_file(),
                  "provenance-artifact-missing")

    def _proof_snapshot(self, token: str, phase_dir: pathlib.Path,
                        label: str, expected_identity: Mapping[str, Any]) -> dict[str, Any]:
        before_identity = self._process_identity()
        reject_if(before_identity != expected_identity,
                  "proof-snapshot-process-changed-before-request", label)
        path = f"{PROOF_ROOT}/snapshot-{token}.json"
        stale = self.adb.run(
            ["shell", "run-as", PACKAGE, "test", "-e", path],
            summary=f"prove proof token {token} is fresh before broadcast",
            allow_failure=True,
        )
        try:
            require_unambiguous_absence(stale, label=token)
        except CampaignRejected as error:
            if error.reason == "preexisting-artifact":
                raise CampaignRejected("proof-snapshot-token-already-exists", token) from error
            raise
        self.adb.run([
            "shell", "am", "broadcast", "-n", PROOF_RECEIVER,
            "-a", PROOF_ACTION,
            "--es", PROOF_TOKEN_EXTRA, token,
        ], summary=f"request {label} proof snapshot in existing :game process")
        deadline = time.monotonic() + 15
        while True:
            probe = self.adb.run(
                ["shell", "run-as", PACKAGE, "test", "-s", path],
                summary=f"wait for immutable proof token {token}",
                allow_failure=True,
            )
            if probe.returncode == 0:
                break
            reject_if(time.monotonic() >= deadline, "proof-snapshot-timeout", token)
            time.sleep(0.5)
        result = self.adb.run(
            ["exec-out", "run-as", PACKAGE, "cat", path],
            summary=f"read immutable proof token {token}",
        )
        after_identity = self._process_identity()
        reject_if(after_identity != before_identity,
                  "proof-snapshot-process-changed-during-request", label)
        self.store.write(
            f"{phase_dir.relative_to(self.store.root).as_posix()}/{label}-proof.json",
            result.stdout,
        )
        try:
            value = json.loads(result.stdout.decode("utf-8", "strict"))
        except (UnicodeError, json.JSONDecodeError) as error:
            raise CampaignRejected("proof-snapshot-json-invalid", token) from error
        reject_if(not isinstance(value, dict), "proof-snapshot-not-object")
        return value

    def _device_uptime_ns(self) -> int:
        result = self.adb.run(["shell", "cat", "/proc/uptime"],
                              summary="capture synchronized device boottime boundary")
        try:
            seconds = float(self._text(result).split()[0])
        except (ValueError, IndexError) as error:
            raise CampaignRejected("device-uptime-invalid") from error
        reject_if(seconds <= 0, "device-uptime-invalid")
        return int(seconds * 1_000_000_000)

    def _measurement(self, phase: Mapping[str, Any], phase_dir: pathlib.Path,
                     identity: Mapping[str, Any], surface: Mapping[str, Any]) -> dict[str, Any]:
        pid = int(identity["pid"])
        measurement_action = next(
            action for action in phase["actions"]
            if action["kind"] == "measurement-window"
        )
        duration = int(measurement_action["durationSeconds"])
        mode = str(phase["instrumentationMode"])
        required_metrics = list(phase["proofMetricsRequired"])
        runtime_mode = str(phase["runtimeInstrumentationMode"])
        runtime_required = list(phase["runtimeMetricsRequired"])
        tokens = phase.get("proofTokens")

        assert isinstance(tokens, Mapping)
        self.adb.run(["shell", "dumpsys", "SurfaceFlinger", "--timestats", "-enable"],
                     summary="enable SurfaceFlinger timestats")
        proof_start = self._proof_snapshot(
            str(tokens["start"]), phase_dir, "start", identity
        )
        validate_proof_snapshot(
            proof_start, mode=mode, token=str(tokens["start"]), pid=pid,
            process_starttime=int(identity["starttime"]),
            required_metrics=required_metrics, runtime_mode=runtime_mode,
            runtime_required_metrics=runtime_required,
        )



        self.adb.run(["shell", "dumpsys", "SurfaceFlinger", "--timestats", "-clear"],
                     summary="clear only SurfaceFlinger measurement statistics")
        self.adb.run(["shell", "dumpsys", "SurfaceFlinger", "--latency-clear",
                      str(surface["identity"])],
                     summary="clear exact BLAST layer latency history")
        self.adb.run(["shell", "logcat", "-b", "all", "-c"],
                     summary="clear logcat immediately before PMU and frame window")

        overlap_v2 = self.plan["kind"] == "overlap"
        if overlap_v2:
            self._arm_overlap_v2()
        input_spec = measurement_action["input"]
        validate_measurement_input(input_spec, duration)
        movement_contract = measurement_action.get("movementEvidence")
        movement_before: dict[str, Any] | None = None
        if input_spec["kind"] == "swipe":
            validate_movement_evidence(movement_contract)
            movement_before = self._capture_movement_frame(
                movement_contract, phase_dir, "before"
            )


        self.adb.gate("simpleperf attach to existing Bloodborne game pid")
        if input_spec["kind"] == "swipe":
            self.adb.gate("matched non-Cross gameplay measurement swipe")
        device_start = self._device_uptime_ns()
        host_start = time.monotonic_ns()
        simpleperf_argv = simpleperf_attach_argv(
            self.adb.adb, self.adb.target, pid, duration
        )
        input_process: subprocess.Popen[bytes] | None = None
        try:
            simpleperf_process = subprocess.Popen(
                simpleperf_argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            if input_spec["kind"] == "swipe":
                input_process = subprocess.Popen([
                    self.adb.adb, "-s", self.adb.target, "shell", "input", "swipe",
                    str(input_spec["x1"]), str(input_spec["y1"]),
                    str(input_spec["x2"]), str(input_spec["y2"]),
                    str(input_spec["durationMilliseconds"]),
                ], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            stdout, stderr = simpleperf_process.communicate(timeout=duration + 20)
            if input_process is not None:
                input_stdout, input_stderr = input_process.communicate(timeout=10)
                self.store.write(
                    f"{phase_dir.relative_to(self.store.root).as_posix()}/input.stdout",
                    input_stdout,
                )
                self.store.write(
                    f"{phase_dir.relative_to(self.store.root).as_posix()}/input.stderr",
                    input_stderr,
                )
                reject_if(input_process.returncode != 0,
                          "measurement-input-returned-nonzero",
                          input_stderr.decode("utf-8", "replace")[:500])
            completed = subprocess.CompletedProcess(
                simpleperf_argv, simpleperf_process.returncode, stdout, stderr
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            raise CampaignRejected("simpleperf-execution-failed", str(error)) from error
        host_end = time.monotonic_ns()
        device_end = self._device_uptime_ns()
        overlap = self._stop_overlap_v2(phase_dir) if overlap_v2 else None
        movement: dict[str, Any] | None = None
        if movement_before is not None:
            movement_after = self._capture_movement_frame(
                movement_contract, phase_dir, "after"
            )
            movement = self._verify_movement_scene_change(
                movement_contract, movement_before, movement_after, phase_dir
            )
        self.store.write(
            f"{phase_dir.relative_to(self.store.root).as_posix()}/simpleperf.csv",
            completed.stdout,
        )
        self.store.write(
            f"{phase_dir.relative_to(self.store.root).as_posix()}/simpleperf.stderr",
            completed.stderr,
        )
        reject_if(completed.returncode != 0, "simpleperf-returned-nonzero",
                  completed.stderr.decode("utf-8", "replace")[:500])

        timestats = self.adb.run(
            ["shell", "dumpsys", "SurfaceFlinger", "--timestats", "-dump",
             "--maxlayers", "128"],
            summary="dump exact-generation SurfaceFlinger timestats",
        )
        latency = self.adb.run(
            ["shell", "dumpsys", "SurfaceFlinger", "--latency",
             str(surface["identity"])],
            summary="dump exact BLAST layer latency",
        )
        logcat = self.adb.run(
            ["shell", "logcat", "-b", "all", "-d", "-v", "threadtime"],
            summary="capture proof-window health log",
        )
        proof_end = self._proof_snapshot(
            str(tokens["end"]), phase_dir, "end", identity
        )
        validate_proof_snapshot(
            proof_end, mode=mode, token=str(tokens["end"]), pid=pid,
            process_starttime=int(identity["starttime"]),
            required_metrics=required_metrics, runtime_mode=runtime_mode,
            runtime_required_metrics=runtime_required, previous=proof_start,
        )
        reject_if(int(proof_start["bootAfterNs"]) > device_start or
                  int(proof_end["bootBeforeNs"]) < device_end,
                  "proof-counter-envelope-does-not-contain-pmu-window")
        prefix = phase_dir.relative_to(self.store.root).as_posix()
        self.store.write(f"{prefix}/surfaceflinger-timestats.txt", timestats.stdout)
        self.store.write(f"{prefix}/surfaceflinger-latency.txt", latency.stdout)
        self.store.write(f"{prefix}/window-logcat.txt", logcat.stdout)

        proof_contract = _load_local_module(
            self.repo_root / "tools" / "bloodborne-proof" / "proof_contract.py",
            "bloodborne_proof_contract_runtime",
        )
        try:
            pmu = proof_contract.parse_simpleperf_csv(
                completed.stdout.decode("utf-8", "strict"),
                completed.stderr.decode("utf-8", "strict"),
            )
        except Exception as error:
            raise CampaignRejected("simpleperf-artifact-rejected", str(error)) from error
        try:
            proof_delta = proof_contract.combined_proof_delta(
                proof_start, proof_end
            )
        except Exception as error:
            raise CampaignRejected("combined-proof-pair-rejected", str(error)) from error
        try:
            frame_latency = proof_contract.parse_surfaceflinger_latency(
                latency.stdout.decode("utf-8", "strict")
            )
        except Exception as error:
            raise CampaignRejected("surfaceflinger-latency-rejected", str(error)) from error
        try:
            derived_metrics = proof_contract.derive_metrics(
                pmu, frame_latency, proof_delta["runtime"], proof_delta["fex"]
            )
        except Exception as error:
            raise CampaignRejected("derived-metrics-rejected", str(error)) from error
        sf_parser = _load_local_module(
            self.repo_root / "tools" / "bloodborne-proof" /
            "parse_surfaceflinger_timestats.py",
            "bloodborne_surfaceflinger_runtime",
        )
        try:
            frames = sf_parser.parse_surfaceflinger_timestats(
                timestats.stdout.decode("utf-8", "strict"),
                expected_layer_identity=str(surface["identity"]),
            )
        except Exception as error:
            raise CampaignRejected("surfaceflinger-artifact-rejected", str(error)) from error
        fatal_patterns = (
            r"FATAL EXCEPTION", r"SIG(?:SEGV|ABRT|BUS)", r"VK_ERROR_DEVICE_LOST",
            r"EXECUTOR_VK_PRESENT_TERMINAL", r"ANR in app\.lsx4\.android",
        )
        log_text = logcat.stdout.decode("utf-8", "replace")
        fatal = [pattern for pattern in fatal_patterns if re.search(pattern, log_text, re.I)]
        reject_if(bool(fatal), "window-health-log-failed", ",".join(fatal))
        return {
            "boundary": {
                "deviceBootStartNs": device_start,
                "deviceBootEndNs": device_end,
                "hostMonotonicStartNs": host_start,
                "hostMonotonicEndNs": host_end,
            },
            "pmu": pmu,
            "frames": frames,
            "surface": surface,
            "process": identity,
            "proofStart": proof_start,
            "proofEnd": proof_end,
            "proofDelta": proof_delta,
            "derivedMetrics": derived_metrics,
            "proofSemantics": {
                "counterEnvelopeContainsPmuWindow": True,
                "counterAndFrameWindowsIdentical": False,
                "crossSourceRatiosAllowed": False,
                "fexSnapshotCoherence": (
                    "bounded-relaxed-no-global-seqlock"
                    if int(proof_start["fex"]["captureFlags"]) & 0x1
                    else "no-relaxed-multifield-flag"
                ),
                "runtimeCountersPerturbed": runtime_mode != "off",
                "runtimeRequestsAreAttemptedNotRetired": runtime_mode != "off",
            },
            "frameLatency": frame_latency,
            "movementEvidence": movement,
            "overlap": overlap,
            "fpsVerdictAllowed": phase["fpsVerdictAllowed"],
            "hudAverageUsed": False,
        }

    def run(self) -> dict[str, Any]:
        self.store.json("plan.json", self.plan)
        for name, item in self.plan["inputs"].items():
            path = pathlib.Path(str(item["path"]))
            reject_if(not path.is_file() or path.is_symlink(),
                      "campaign-input-file-invalid", name)
            reject_if(sha256_file(path) != item["sha256"],
                      "campaign-input-changed-after-plan", name)






        baseline_expected = pathlib.Path(
            str(self.plan["inputs"]["expectedState"]["path"])
        )
        self._capture_provenance(
            self.store.root, baseline_expected, "pre-mutation-bootstrap"
        )
        baseline_provenance = (
            self.store.root / "pre-mutation-bootstrap" / "provenance.json"
        )
        reject_if(not baseline_provenance.is_file(),
                  "pre-mutation-bootstrap-provenance-missing")
        self.store.json("pre-mutation-bootstrap-pass.json", {
            "schema": "lsx4-bloodborne-pre-mutation-bootstrap/v1",
            "accepted": True,
            "readOnly": True,
            "completedBeforeForceStop": True,
            "completedBeforeMarkerMutation": True,
            "expectedStateSha256": self.plan["inputs"]["expectedState"]["sha256"],
            "provenanceSha256": sha256_file(baseline_provenance),
        })
        results: list[dict[str, Any]] = []
        for phase in self.plan["phases"]:
            phase_id = phase["phaseId"]
            phase_dir = self.store.root / "phases" / phase_id
            phase_dir.mkdir(parents=True, exist_ok=False)
            expected_path = phase_dir / "expected-state.json"
            expected_path.write_bytes(canonical_json(phase["expectedState"]))

            self._force_stop(f"prepare phase {phase_id} from stopped app")
            thermal_status = self._cooldown()
            self.phase_start_thermal.append(thermal_status)
            self._identity()
            preflight = self._preflight(False)
            surface_c = parse_preflight_surface_max_c(preflight)
            admission = parse_preflight_admission(preflight)
            self.phase_start_external_power.append(admission["externalPower"])
            policies = admission["cpuPolicies"]
            self.phase_start_cpu_policy_limits.append((
                policies["0"]["minimumKHz"], policies["0"]["maximumKHz"],
                policies["6"]["minimumKHz"], policies["6"]["maximumKHz"],
            ))
            reject_if(
                self.route["thermal"].get("requireSurfaceTemperatureForCampaign") is True
                and surface_c is None,
                "phase-start-surface-temperature-unavailable",
            )
            if surface_c is not None:
                self.phase_start_surface_c.append(surface_c)
            self.store.write(f"phases/{phase_id}/preflight.txt", preflight.encode("utf-8"))
            markers = self._set_markers(phase["enabledManagedMarkers"])
            self.store.write(f"phases/{phase_id}/markers.txt", markers.encode("utf-8"))
            if self.plan["kind"] == "overlap":
                self._prepare_overlap_v2()
            self.adb.run(["shell", "logcat", "-b", "all", "-c"],
                         summary="clear logs before canonical route")
            route_gates = self._launch_route(phase_dir)
            identity = self._process_identity()
            gameplay_gate = self._gameplay_gate(phase, phase_dir, identity)
            surface = self._surface_identity()
            self.store.json(f"phases/{phase_id}/gameplay-gate.json", {
                "routeId": self.route["id"], "crossHolds": 1,
                "additionalCrossHolds": 0,
                "routeStateGates": route_gates,
                "gameplayGate": gameplay_gate,
                "sceneContract": self.route["gameplayGate"]["expectedScene"],
                "fpsVerdictAllowed": False,
            })
            self._capture_provenance(phase_dir, expected_path, "provenance-pre")
            identity = self._process_identity()
            reject_if(identity["pid"] <= 0 or identity["starttime"] <= 0,
                      "fresh-process-identity-invalid")
            surface = self._surface_identity()
            cleanup = self._preflight(True)
            self.store.write(f"phases/{phase_id}/preflight-cleanup.txt",
                             cleanup.encode("utf-8"))
            window = self._measurement(phase, phase_dir, identity, surface)
            final_identity = self._process_identity()
            final_surface = self._surface_identity()
            reject_if(final_identity != identity, "game-process-identity-changed")
            reject_if(final_surface != surface, "game-surface-generation-changed")
            self._capture_provenance(phase_dir, expected_path, "provenance-post")
            self.store.json(f"phases/{phase_id}/window.json", window)
            results.append({
                "phaseId": phase_id, "variant": phase["variant"],
                "thermalStartStatus": thermal_status,
                "surfaceStartMaxC": surface_c,
                "batteryStartLevelPercent": admission["batteryLevelPercent"],
                "externalPowerAtStart": admission["externalPower"],
                "cpuPoliciesAtStart": admission["cpuPolicies"],
                "window": window,
            })
            self._force_stop(f"end phase {phase_id}")

        if self.route["thermal"].get("requireEqualStartStatusAcrossPhases"):
            reject_if(len(set(self.phase_start_thermal)) > 1,
                      "phase-start-thermal-band-mismatch",
                      repr(self.phase_start_thermal))
        if len(self.phase_start_surface_c) > 1:
            allowed_delta = float(
                self.route["thermal"]["maximumPhaseStartSurfaceDeltaC"]
            )
            actual_delta = max(self.phase_start_surface_c) - min(self.phase_start_surface_c)
            reject_if(actual_delta > allowed_delta,
                      "phase-start-surface-temperature-band-mismatch",
                      f"delta={actual_delta:.2f}C allowed={allowed_delta:.2f}C")
        if self.route["thermal"].get("requireEqualExternalPowerAcrossPhases"):
            reject_if(len(set(self.phase_start_external_power)) > 1,
                      "phase-start-external-power-mismatch",
                      repr(self.phase_start_external_power))
        if self.route["thermal"].get("requireEqualCpuPolicyLimitsAcrossPhases"):
            reject_if(len(set(self.phase_start_cpu_policy_limits)) > 1,
                      "phase-start-cpu-policy-limits-mismatch",
                      repr(self.phase_start_cpu_policy_limits))
        result = {
            "schema": RESULT_SCHEMA,
            "campaignId": self.plan["campaignId"],
            "accepted": True,
            "kind": self.plan["kind"],
            "axis": self.plan["axis"],
            "order": self.plan["order"],
            "phases": results,
            "claims": {
                "hudAverageUsed": False,
                "instrumentedFpsVerdict": False,
                "cacheDeleted": False,
                "multiblockEnabled": False,
            },
        }
        self.store.json("result.json", result)
        manifest = {"schema": "lsx4-bloodborne-campaign-manifest/v1",
                    "files": self.store.manifest()}
        self.store.json("manifest.json", manifest)
        self.store.json("SEALED.json", {
            "schema": "lsx4-bloodborne-campaign-seal/v1",
            "manifestSha256": canonical_sha256(manifest),
            "sealedUtc": dt.datetime.now(dt.timezone.utc).isoformat(),
        })
        return result


def _load_local_module(path: pathlib.Path, name: str) -> Any:
    spec = importlib.util.spec_from_file_location(name, path)
    reject_if(spec is None or spec.loader is None, "local-module-load-failed", str(path))
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def default_campaign_id(kind: str, axis: str | None) -> str:
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    suffix = uuid.uuid4().hex[:10]
    axis_part = f"-{axis.lower()}" if axis else ""
    return f"bb-{kind}{axis_part}-{stamp}-{suffix}"


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    root = pathlib.Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--route", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parent /
                        "routes" / "hunters_dream_forward_swipe_14s.json")
    parser.add_argument("--expected-state", required=True, type=pathlib.Path)
    parser.add_argument("--pmu-capabilities", required=True, type=pathlib.Path)
    parser.add_argument("--output-root", type=pathlib.Path,
                        default=root / ".codex-bb-proof" / "campaigns")
    parser.add_argument("--campaign-id")
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--adb-target", required=True)
    parser.add_argument("--kind", choices=(
        "production", "axis", "static", "dynamic", "runtime",
        "runtime-faults", "overlap",
    ),
                        default="production")
    parser.add_argument("--axis", choices=tuple(AXIS_MARKERS))
    parser.add_argument("--order", choices=("ABA", "BAB"))
    parser.add_argument("--duration-seconds", type=int)
    parser.add_argument("--require-complete-panel", action="store_true")
    parser.add_argument("--agentchat", type=pathlib.Path,
                        default=root.parent / "agentchat" / "agentchat.py")
    parser.add_argument("--agent", default="codex")
    parser.add_argument("--room", default="увеличить fps")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--dry-run", action="store_true",
                      help="print the command plan and do not touch the device (default)")
    mode.add_argument("--execute", action="store_true",
                      help="execute the immutable plan on the declared Vivo")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    store: ExclusiveStore | None = None
    try:
        route = load_json_object(args.route, "route")
        expected = load_json_object(args.expected_state, "expected-state")
        pmu = load_json_object(args.pmu_capabilities, "pmu-capabilities")
        campaign_id = args.campaign_id or default_campaign_id(args.kind, args.axis)
        plan = build_plan(
            route=route, expected_state=expected, pmu_capabilities=pmu,
            route_path=args.route, expected_state_path=args.expected_state,
            pmu_capabilities_path=args.pmu_capabilities,
            output_root=args.output_root, campaign_id=campaign_id,
            adb_target=args.adb_target, kind=args.kind, axis=args.axis,
            order=args.order, duration_seconds=args.duration_seconds,
            require_complete_panel=args.require_complete_panel,
        )
        if not args.execute:
            print(json.dumps({"dryRun": True, "plan": plan},
                             ensure_ascii=False, indent=2, sort_keys=True))
            return 0
        reject_if(not args.agentchat.is_file(), "agentchat-script-missing",
                  str(args.agentchat))
        artifact_dir = pathlib.Path(plan["artifactDir"])
        store = ExclusiveStore(artifact_dir)
        host = HostCommands()
        adb = GuardedAdb(
            host, adb=args.adb, target=args.adb_target,
            agentchat=args.agentchat, agent=args.agent, room=args.room,
        )
        executor = CampaignExecutor(
            plan, route=route, store=store, adb=adb, host=host,
            repo_root=pathlib.Path(__file__).resolve().parents[2],
            agentchat=args.agentchat, agent=args.agent, room=args.room,
        )
        result = executor.run()
        print(json.dumps({"accepted": True, "artifactDir": str(artifact_dir),
                          "result": result}, ensure_ascii=False, sort_keys=True))
        return 0
    except CampaignRejected as error:
        payload = {"accepted": False, "reason": error.reason, "detail": error.detail}
        if store is not None:
            try:
                store.json("REJECTED.json", payload)
            except CampaignRejected:
                pass
        print(json.dumps(payload, ensure_ascii=False, sort_keys=True), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
