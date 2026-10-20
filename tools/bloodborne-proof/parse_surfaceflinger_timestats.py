#!/usr/bin/env python3
"""Parse one proof-grade SurfaceFlinger ``--timestats`` capture.

SurfaceFlinger can emit the same layer more than once when the capture spans
different display/render-rate pairs.  Those records are disjoint rate buckets
for one layer generation and must be aggregated.  Records from different full
layer identities, however, must never be combined: that would make a surface
recreation look like one continuous gameplay window.

The parser is deliberately fail closed.  It consumes text only and has no adb
or filesystem side effects other than the optional CLI output file.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import math
import pathlib
import re
import sys
from collections import defaultdict
from decimal import Decimal, InvalidOperation
from typing import Any, Iterable


SCHEMA = "lsx4.surfaceflinger-timestats/v1"
GAME_BLAST_COMPONENT = (
    "SurfaceView[app.lsx4.android/app.lsx4.android.GameActivity](BLAST)"
)
PARENT_COMPONENT_RE = re.compile(
    r"(?:^|\s)app\.lsx4\.android/app\.lsx4\.android\.GameActivity(?:\s|$)"
)
GENERATION_RE = re.compile(r"#(?P<generation>[0-9]+)$")
RATE_RE = re.compile(
    r"^(?P<name>displayRefreshRate|renderRate)\s*=\s*"
    r"(?P<value>[0-9]+(?:\.[0-9]+)?)\s+fps\s*$"
)
INTEGER_FIELD_RE = re.compile(
    r"^(?P<name>totalFrames|droppedFrames)\s*=\s*(?P<value>[0-9]+)\s*$"
)
AVERAGE_FPS_RE = re.compile(
    r"^averageFPS\s*=\s*(?P<value>[0-9]+(?:\.[0-9]+)?)\s*$"
)
HISTOGRAM_TOKEN_RE = re.compile(r"^(?P<bucket>[0-9]+)ms=(?P<count>[0-9]+)$")
STATS_RE = re.compile(r"^(?P<name>statsStart|statsEnd)\s*=\s*(?P<value>[0-9]+)\s*$")


class SurfaceFlingerParseError(ValueError):
    """A capture cannot support a proof-grade frame-time conclusion."""

    def __init__(self, reason: str, detail: str | None = None):
        self.reason = reason
        self.detail = detail
        super().__init__(reason if detail is None else f"{reason}: {detail}")


def _reject(condition: bool, reason: str, detail: str | None = None) -> None:
    if condition:
        raise SurfaceFlingerParseError(reason, detail)


def _parse_decimal(value: str, *, field: str, line_number: int) -> Decimal:
    try:
        parsed = Decimal(value)
    except InvalidOperation as exc:
        raise SurfaceFlingerParseError(
            "invalid_rate", f"line={line_number} field={field} value={value!r}"
        ) from exc
    _reject(
        not parsed.is_finite() or parsed <= 0,
        "invalid_rate",
        f"line={line_number} field={field} value={value!r}",
    )
    return parsed


def _number(decimal: Decimal) -> int | float:
    integral = decimal.to_integral_value()
    return int(integral) if decimal == integral else float(decimal)


@dataclasses.dataclass
class _LayerSection:
    identity: str
    display_refresh_rate: Decimal | None
    render_rate: Decimal | None
    line_number: int
    total_frames: int | None = None
    dropped_frames: int | None = None
    average_fps: Decimal | None = None
    histogram: dict[int, int] | None = None

    @property
    def is_blast(self) -> bool:
        return GAME_BLAST_COMPONENT in self.identity

    @property
    def is_parent(self) -> bool:
        return not self.is_blast and bool(PARENT_COMPONENT_RE.search(self.identity))

    @property
    def generation(self) -> int | None:
        match = GENERATION_RE.search(self.identity)
        return int(match.group("generation")) if match else None


def _parse_histogram(line: str, *, line_number: int) -> dict[int, int]:
    tokens = line.split()
    _reject(not tokens, "empty_present2present_histogram", f"line={line_number}")
    histogram: dict[int, int] = {}
    previous = -1
    for token in tokens:
        match = HISTOGRAM_TOKEN_RE.fullmatch(token)
        _reject(
            match is None,
            "malformed_present2present_histogram",
            f"line={line_number} token={token!r}",
        )
        assert match is not None
        bucket = int(match.group("bucket"))
        count = int(match.group("count"))
        _reject(
            bucket in histogram or bucket <= previous,
            "non_monotonic_present2present_buckets",
            f"line={line_number} bucket={bucket} previous={previous}",
        )
        histogram[bucket] = count
        previous = bucket
    return histogram


def _next_nonempty_line(lines: list[str], start: int) -> tuple[int, str] | None:
    for index in range(start, len(lines)):
        if lines[index].strip():
            return index, lines[index].strip()
    return None


def _collect_sections(text: str) -> tuple[int, int, list[_LayerSection]]:
    lines = text.splitlines()
    stats: dict[str, int] = {}
    sections: list[_LayerSection] = []
    current_display: Decimal | None = None
    current_render: Decimal | None = None
    current: _LayerSection | None = None

    for index, raw_line in enumerate(lines):
        line_number = index + 1
        line = raw_line.strip()
        if not line:
            continue

        stats_match = STATS_RE.fullmatch(line)
        if stats_match:
            name = stats_match.group("name")
            _reject(name in stats, "duplicate_global_stat", f"line={line_number} name={name}")
            stats[name] = int(stats_match.group("value"))
            continue

        rate_match = RATE_RE.fullmatch(line)
        if rate_match:
            name = rate_match.group("name")
            value = _parse_decimal(
                rate_match.group("value"), field=name, line_number=line_number
            )
            current = None
            if name == "displayRefreshRate":
                current_display = value


                current_render = None
            else:
                current_render = value
            continue

        if line.startswith("layerName = "):
            identity = line.split("=", 1)[1].strip()
            _reject(not identity, "empty_layer_identity", f"line={line_number}")
            current = _LayerSection(
                identity=identity,
                display_refresh_rate=current_display,
                render_rate=current_render,
                line_number=line_number,
            )
            sections.append(current)
            continue

        if current is None:
            continue

        integer_match = INTEGER_FIELD_RE.fullmatch(line)
        if integer_match:
            name = integer_match.group("name")
            value = int(integer_match.group("value"))
            if name == "totalFrames":
                _reject(
                    current.total_frames is not None,
                    "duplicate_layer_field",
                    f"line={line_number} identity={current.identity!r} field={name}",
                )
                current.total_frames = value
            else:
                _reject(
                    current.dropped_frames is not None,
                    "duplicate_layer_field",
                    f"line={line_number} identity={current.identity!r} field={name}",
                )
                current.dropped_frames = value
            continue

        average_match = AVERAGE_FPS_RE.fullmatch(line)
        if average_match:
            _reject(
                current.average_fps is not None,
                "duplicate_layer_field",
                f"line={line_number} identity={current.identity!r} field=averageFPS",
            )
            current.average_fps = _parse_decimal(
                average_match.group("value"), field="averageFPS", line_number=line_number
            )
            continue

        if line == "present2present histogram is as below:":
            _reject(
                current.histogram is not None,
                "duplicate_layer_field",
                f"line={line_number} identity={current.identity!r} field=present2present",
            )
            following = _next_nonempty_line(lines, index + 1)
            _reject(
                following is None,
                "missing_present2present_histogram",
                f"line={line_number} identity={current.identity!r}",
            )
            assert following is not None
            histogram_index, histogram_line = following
            current.histogram = _parse_histogram(
                histogram_line, line_number=histogram_index + 1
            )

    _reject("statsStart" not in stats, "missing_stats_start")
    _reject("statsEnd" not in stats, "missing_stats_end")
    _reject(
        stats["statsEnd"] <= stats["statsStart"],
        "invalid_stats_window",
        f"start={stats['statsStart']} end={stats['statsEnd']}",
    )
    return stats["statsStart"], stats["statsEnd"], sections


def _validate_positive_section(section: _LayerSection) -> None:
    detail = f"line={section.line_number} identity={section.identity!r}"
    _reject(section.display_refresh_rate is None, "missing_display_refresh_rate", detail)
    _reject(section.render_rate is None, "missing_render_rate", detail)
    _reject(section.total_frames is None, "missing_total_frames", detail)
    _reject(section.dropped_frames is None, "missing_dropped_frames", detail)
    _reject(section.average_fps is None, "missing_average_fps", detail)
    _reject(section.histogram is None, "missing_present2present_histogram", detail)
    _reject(section.generation is None, "missing_layer_generation", detail)
    assert section.total_frames is not None
    assert section.histogram is not None
    _reject(
        sum(section.histogram.values()) != section.total_frames,
        "present2present_histogram_sum_mismatch",
        (
            f"identity={section.identity!r} rate="
            f"{section.display_refresh_rate}/{section.render_rate} "
            f"histogramSamples={sum(section.histogram.values())} "
            f"totalFrames={section.total_frames}"
        ),
    )


def _nearest_rank_bucket(
    histogram: dict[int, int], quantile: float, *, censor_bucket_ms: int
) -> tuple[int | None, bool]:
    samples = sum(histogram.values())
    _reject(samples <= 0, "empty_aggregate_histogram")
    rank = max(1, math.ceil(quantile * samples))
    cumulative = 0
    for bucket, count in sorted(histogram.items()):
        cumulative += count
        if cumulative >= rank:
            if bucket >= censor_bucket_ms:
                return None, True
            return bucket, False
    raise SurfaceFlingerParseError("quantile_rank_out_of_bounds")


def _fraction(histogram: dict[int, int], predicate: Any) -> tuple[int, float]:
    samples = sum(histogram.values())
    count = sum(value for bucket, value in histogram.items() if predicate(bucket))
    return count, count / samples


def _distribution(histogram: dict[int, int]) -> dict[str, Any]:
    samples = sum(histogram.values())
    _reject(samples <= 0, "empty_aggregate_histogram")
    censor_bucket = 1000
    right_censored_count = histogram.get(censor_bucket, 0)

    quantiles: dict[str, Any] = {}
    for label, quantile in (("p50", 0.50), ("p90", 0.90), ("p95", 0.95), ("p99", 0.99)):
        upper, censored = _nearest_rank_bucket(
            histogram, quantile, censor_bucket_ms=censor_bucket
        )
        quantiles[f"{label}UpperBoundMs"] = upper
        quantiles[f"{label}RightCensored"] = censored

    threshold_specs = (
        ("le33Ms", lambda bucket: bucket <= 33),
        ("le40Ms", lambda bucket: bucket <= 40),
        ("gt40Ms", lambda bucket: bucket > 40),
        ("gt50Ms", lambda bucket: bucket > 50),
        ("ge100Ms", lambda bucket: bucket >= 100),
        ("ge200Ms", lambda bucket: bucket >= 200),
        ("ge500Ms", lambda bucket: bucket >= 500),
        ("ge1000Ms", lambda bucket: bucket >= 1000),
    )
    thresholds: dict[str, Any] = {}
    for label, predicate in threshold_specs:
        count, fraction = _fraction(histogram, predicate)
        thresholds[f"{label}Count"] = count
        thresholds[f"{label}Fraction"] = fraction

    finite_nonzero = [
        bucket for bucket, count in histogram.items() if bucket < censor_bucket and count > 0
    ]
    return {
        "sampleCount": samples,
        "buckets": [
            {"upperBoundMs": bucket, "count": count}
            for bucket, count in sorted(histogram.items())
        ],
        **quantiles,
        **thresholds,
        "rightCensored": right_censored_count > 0,
        "rightCensorLowerBoundMs": censor_bucket if right_censored_count > 0 else None,
        "rightCensoredCount": right_censored_count,
        "largestFiniteNonzeroBucketUpperBoundMs": (
            max(finite_nonzero) if finite_nonzero else None
        ),


        "exactMaximumMs": None,
        "maximumIsExact": False,
    }


def parse_surfaceflinger_timestats(
    text: str, *, expected_layer_identity: str | None = None
) -> dict[str, Any]:
    """Return canonical metrics for exactly one positive gameplay BLAST layer.

    Parent ``GameActivity`` records may coexist in a normal dump, but are never
    selected or added to BLAST frame counts.  Multiple rate-pair sections for
    the same full BLAST identity are aggregated.  A second positive identity is
    rejected even when it happens to carry the same trailing generation number.
    """

    stats_start, stats_end, sections = _collect_sections(text)
    blast_sections = [
        section
        for section in sections
        if section.is_blast and (section.total_frames or 0) > 0
    ]
    _reject(not blast_sections, "no_positive_game_blast_layer")

    for section in blast_sections:
        _validate_positive_section(section)

    identities = {section.identity for section in blast_sections}
    generations = {section.generation for section in blast_sections}
    _reject(
        len(generations) != 1,
        "distinct_positive_blast_generations",
        f"generations={sorted(generations)}",
    )
    _reject(
        len(identities) != 1,
        "distinct_positive_blast_identities",
        f"identities={sorted(identities)!r}",
    )
    identity = next(iter(identities))
    if expected_layer_identity is not None:
        _reject(
            GAME_BLAST_COMPONENT not in expected_layer_identity,
            "expected_identity_is_not_game_blast",
            repr(expected_layer_identity),
        )
        _reject(
            identity != expected_layer_identity,
            "expected_layer_identity_mismatch",
            f"expected={expected_layer_identity!r} actual={identity!r}",
        )

    rate_pairs: set[tuple[Decimal, Decimal]] = set()
    reference_buckets: tuple[int, ...] | None = None
    aggregate_histogram: defaultdict[int, int] = defaultdict(int)
    rate_breakdown: list[dict[str, Any]] = []
    aggregate_frames = 0
    aggregate_dropped = 0
    reported_duration = Decimal(0)

    for section in blast_sections:
        assert section.display_refresh_rate is not None
        assert section.render_rate is not None
        assert section.total_frames is not None
        assert section.dropped_frames is not None
        assert section.average_fps is not None
        assert section.histogram is not None
        pair = (section.display_refresh_rate, section.render_rate)
        _reject(
            pair in rate_pairs,
            "duplicate_layer_rate_pair",
            (
                f"identity={identity!r} display={section.display_refresh_rate} "
                f"render={section.render_rate}"
            ),
        )
        rate_pairs.add(pair)
        bucket_layout = tuple(section.histogram)
        if reference_buckets is None:
            reference_buckets = bucket_layout
        _reject(
            bucket_layout != reference_buckets,
            "histogram_bucket_layout_mismatch",
            f"identity={identity!r} rate={pair}",
        )
        for bucket, count in section.histogram.items():
            aggregate_histogram[bucket] += count
        aggregate_frames += section.total_frames
        aggregate_dropped += section.dropped_frames
        reported_duration += Decimal(section.total_frames) / section.average_fps
        rate_breakdown.append(
            {
                "displayRefreshRateFps": _number(section.display_refresh_rate),
                "renderRateFps": _number(section.render_rate),
                "totalFrames": section.total_frames,
                "droppedFrames": section.dropped_frames,
                "averageFps": float(section.average_fps),
                "present2present": _distribution(section.histogram),
            }
        )

    _reject(aggregate_frames <= 0, "empty_aggregate_frame_count")
    _reject(
        sum(aggregate_histogram.values()) != aggregate_frames,
        "aggregate_histogram_sum_mismatch",
        (
            f"histogramSamples={sum(aggregate_histogram.values())} "
            f"totalFrames={aggregate_frames}"
        ),
    )
    rate_breakdown.sort(
        key=lambda value: (value["displayRefreshRateFps"], value["renderRateFps"])
    )
    parent_records = sum(1 for section in sections if section.is_parent)

    return {
        "schema": SCHEMA,
        "statsStart": stats_start,
        "statsEnd": stats_end,
        "statsDurationSeconds": stats_end - stats_start,
        "layer": {
            "identity": identity,
            "generation": next(iter(generations)),
            "kind": "game-blast-surfaceview",
            "totalFrames": aggregate_frames,
            "droppedFrames": aggregate_dropped,
            "aggregateReportedFps": float(
                Decimal(aggregate_frames) / reported_duration
            ),
            "rateBreakdown": rate_breakdown,
        },
        "present2present": _distribution(dict(aggregate_histogram)),
        "ignoredParentGameActivityRecords": parent_records,
    }


def _read_text(path: str) -> str:
    if path == "-":
        return sys.stdin.read()
    return pathlib.Path(path).read_text(encoding="utf-8", errors="strict")


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", help="SurfaceFlinger timestats file, or - for stdin")
    parser.add_argument("--expected-layer-identity")
    parser.add_argument("--output", help="write canonical JSON here instead of stdout")
    args = parser.parse_args(list(argv) if argv is not None else None)

    try:
        result = parse_surfaceflinger_timestats(
            _read_text(args.input),
            expected_layer_identity=args.expected_layer_identity,
        )
    except (OSError, UnicodeError, SurfaceFlingerParseError) as exc:
        reason = exc.reason if isinstance(exc, SurfaceFlingerParseError) else "input_read_failed"
        payload = {"accepted": False, "reason": reason, "detail": str(exc)}
        print(json.dumps(payload, sort_keys=True), file=sys.stderr)
        return 2

    payload = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        pathlib.Path(args.output).write_text(payload, encoding="utf-8", newline="\n")
    else:
        sys.stdout.write(payload)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
