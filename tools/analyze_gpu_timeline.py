#!/usr/bin/env python3
"""Strict offline analyzer for LSX4 Vulkan GPU timeline schemas 4 and 5."""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


KV_RE = re.compile(r"([A-Za-z][A-Za-z0-9]*)=([^\s]+)")
KINDS = {"draw", "present", "repeat"}


class TimelineError(ValueError):
    pass


@dataclass(frozen=True)
class Frame:
    boundary: int
    epoch: int
    cpu_ns: int
    flip_arg: int
    is_eop: bool


@dataclass(frozen=True)
class QueueSubmit:
    sequence: int
    kind: str
    first_tick: int
    last_tick: int
    cpu_begin_ns: int
    cpu_end_ns: int
    logical: int
    command_buffers: int
    result: int
    forced: bool
    empty_fence: bool
    queue_present: bool

    @property
    def benign_without_timestamp(self) -> bool:
        return self.empty_fence or self.queue_present


@dataclass(frozen=True)
class Calibration:
    sequence: int
    cpu_before_ns: int
    cpu_after_ns: int
    device_ticks: int
    host_timestamp: int
    max_deviation_ns: int

    @property
    def cpu_midpoint_ns(self) -> float:
        return (self.cpu_before_ns + self.cpu_after_ns) / 2.0

    @property
    def uncertainty_ns(self) -> float:
        return self.max_deviation_ns + (
            self.cpu_after_ns - self.cpu_before_ns
        ) / 2.0


@dataclass(frozen=True)
class Sample:
    scheduler: str
    kind: str
    epoch: int
    submit: int
    logical_tick: int
    physical_sequence: int
    physical_index: int
    begin: int
    end: int
    valid_bits: int
    period_ns: float
    cpu_record_begin_ns: int
    cpu_record_end_ns: int
    cpu_submit_begin_ns: int
    cpu_submit_end_ns: int
    attempts: int
    recorded: int
    collected: int
    ring_full_skips: int
    not_ready_polls: int


@dataclass(frozen=True)
class Interval:
    sample: Sample
    begin_tick: int
    end_tick: int


@dataclass
class FrameMetrics:
    epoch: int
    boundary: int
    sample_count: int
    queue_submit_count: int
    gpu_union_ms: float
    gpu_span_ms: float
    gpu_internal_gap_ms: float
    gpu_gap_before_ms: float | None
    cpu_record_sum_ms: float
    cpu_submit_sum_ms: float
    kind_lower_bound_ms: dict[str, float]
    first_tick: int
    last_tick: int
    first_sequence: int
    last_sequence: int


@dataclass(frozen=True)
class FrameInterval:
    epoch: int
    boundary: int
    flip_arg: int
    is_eop: bool
    interval_ms: float


@dataclass
class ParsedTimeline:
    header: dict[str, str]
    frames: list[Frame]
    queues: list[QueueSubmit]
    calibrations: list[Calibration]
    samples: list[Sample]


@dataclass(frozen=True)
class InterBatchClassification:
    definitely_host_late_ms: float = 0.0
    definitely_host_late_count: int = 0
    prequeued_gap_ms: float = 0.0
    prequeued_gap_count: int = 0
    ambiguous_ms: float = 0.0
    ambiguous_count: int = 0


@dataclass
class Analysis:
    schema: int
    valid_bits: int
    period_ns: float
    parsed_frames: int
    eligible_after_trim: int
    frame_intervals: list[FrameInterval]
    complete_frames: list[FrameMetrics]
    complete_metrics_by_epoch: dict[int, FrameMetrics]
    epoch_exclusion_reasons: dict[int, tuple[str, ...]]
    excluded_reasons: Counter[str]
    global_notes: list[str]
    calibration_count: int
    inter_batch_classification: InterBatchClassification | None
    inter_batch_classification_unavailable_reason: str | None


def _kv(line: str) -> dict[str, str]:
    return dict(KV_RE.findall(line))


def _need(values: dict[str, str], names: Iterable[str], line_no: int) -> None:
    missing = [name for name in names if name not in values]
    if missing:
        raise TimelineError(f"line {line_no}: missing fields: {', '.join(missing)}")


def _integer(values: dict[str, str], name: str, line_no: int) -> int:
    try:
        return int(values[name], 0)
    except ValueError as exc:
        raise TimelineError(f"line {line_no}: invalid integer {name}={values[name]!r}") from exc


def _boolean(values: dict[str, str], name: str, line_no: int) -> bool:
    value = _integer(values, name, line_no)
    if value not in (0, 1):
        raise TimelineError(f"line {line_no}: {name} must be 0 or 1")
    return bool(value)


def parse_timeline_text(text: str) -> ParsedTimeline:
    header: dict[str, str] | None = None
    frames: list[Frame] = []
    queues: list[QueueSubmit] = []
    calibrations: list[Calibration] = []
    samples: list[Sample] = []

    for line_no, raw_line in enumerate(text.splitlines(), 1):
        line = raw_line.strip()
        if not line:
            continue
        values = _kv(line)
        if line.startswith("schema="):
            if header is not None:
                raise TimelineError(f"line {line_no}: duplicate header")
            _need(values, ("schema", "samples", "frames", "queueSubmits", "droppedSamples", "currentEpoch"), line_no)
            header = values
            continue
        if line.startswith("F "):
            _need(values, ("boundary", "epoch", "cpuNs", "flipArg", "isEop"), line_no)
            frames.append(Frame(
                boundary=_integer(values, "boundary", line_no),
                epoch=_integer(values, "epoch", line_no),
                cpu_ns=_integer(values, "cpuNs", line_no),
                flip_arg=_integer(values, "flipArg", line_no),
                is_eop=_boolean(values, "isEop", line_no),
            ))
            continue
        if line.startswith("Q "):
            _need(values, ("sequence", "kind", "firstTick", "lastTick", "cpuBeginNs", "cpuEndNs", "logical", "commandBuffers", "result", "forced", "emptyFence", "queuePresent"), line_no)
            kind = values["kind"]
            if kind not in KINDS:
                raise TimelineError(f"line {line_no}: unknown scheduler kind {kind!r}")
            queues.append(QueueSubmit(
                sequence=_integer(values, "sequence", line_no),
                kind=kind,
                first_tick=_integer(values, "firstTick", line_no),
                last_tick=_integer(values, "lastTick", line_no),
                cpu_begin_ns=_integer(values, "cpuBeginNs", line_no),
                cpu_end_ns=_integer(values, "cpuEndNs", line_no),
                logical=_integer(values, "logical", line_no),
                command_buffers=_integer(values, "commandBuffers", line_no),
                result=_integer(values, "result", line_no),
                forced=_boolean(values, "forced", line_no),
                empty_fence=_boolean(values, "emptyFence", line_no),
                queue_present=_boolean(values, "queuePresent", line_no),
            ))
            continue
        if line.startswith("C "):
            _need(values, ("sequence", "cpuBeforeNs", "cpuAfterNs", "deviceTicks", "hostTimestamp", "maxDeviationNs"), line_no)
            calibrations.append(Calibration(
                sequence=_integer(values, "sequence", line_no),
                cpu_before_ns=_integer(values, "cpuBeforeNs", line_no),
                cpu_after_ns=_integer(values, "cpuAfterNs", line_no),
                device_ticks=_integer(values, "deviceTicks", line_no),
                host_timestamp=_integer(values, "hostTimestamp", line_no),
                max_deviation_ns=_integer(values, "maxDeviationNs", line_no),
            ))
            continue
        if line.startswith("S "):
            _need(values, ("scheduler", "kind", "epoch", "submit", "logicalTick", "physicalSequence", "physicalIndex", "begin", "end", "validBits", "periodNs", "cpuRecordBeginNs", "cpuRecordEndNs", "cpuSubmitBeginNs", "cpuSubmitEndNs", "attempts", "recorded", "collected", "ringFullSkips", "notReadyPolls"), line_no)
            kind = values["kind"]
            if kind not in KINDS:
                raise TimelineError(f"line {line_no}: unknown scheduler kind {kind!r}")
            try:
                period_ns = float(values["periodNs"])
            except ValueError as exc:
                raise TimelineError(f"line {line_no}: invalid periodNs={values['periodNs']!r}") from exc
            samples.append(Sample(
                scheduler=values["scheduler"],
                kind=kind,
                epoch=_integer(values, "epoch", line_no),
                submit=_integer(values, "submit", line_no),
                logical_tick=_integer(values, "logicalTick", line_no),
                physical_sequence=_integer(values, "physicalSequence", line_no),
                physical_index=_integer(values, "physicalIndex", line_no),
                begin=_integer(values, "begin", line_no),
                end=_integer(values, "end", line_no),
                valid_bits=_integer(values, "validBits", line_no),
                period_ns=period_ns,
                cpu_record_begin_ns=_integer(values, "cpuRecordBeginNs", line_no),
                cpu_record_end_ns=_integer(values, "cpuRecordEndNs", line_no),
                cpu_submit_begin_ns=_integer(values, "cpuSubmitBeginNs", line_no),
                cpu_submit_end_ns=_integer(values, "cpuSubmitEndNs", line_no),
                attempts=_integer(values, "attempts", line_no),
                recorded=_integer(values, "recorded", line_no),
                collected=_integer(values, "collected", line_no),
                ring_full_skips=_integer(values, "ringFullSkips", line_no),
                not_ready_polls=_integer(values, "notReadyPolls", line_no),
            ))
            continue
        raise TimelineError(f"line {line_no}: unknown record type")

    if header is None:
        raise TimelineError("missing schema header")
    numeric_header: dict[str, int] = {}
    common_header_fields = (
        "schema", "samples", "frames", "queueSubmits", "droppedSamples", "currentEpoch"
    )
    for field in common_header_fields:
        try:
            numeric_header[field] = int(header[field], 0)
        except ValueError as exc:
            raise TimelineError(f"invalid header {field}={header[field]!r}") from exc
    schema = numeric_header["schema"]
    if schema not in (4, 5):
        raise TimelineError(f"unsupported schema={schema}; expected schema=4 or schema=5")
    if schema == 5:
        if "calibrations" not in header:
            raise TimelineError("schema=5 header is missing calibrations")
        try:
            numeric_header["calibrations"] = int(header["calibrations"], 0)
        except ValueError as exc:
            raise TimelineError(
                f"invalid header calibrations={header['calibrations']!r}"
            ) from exc
    elif calibrations:
        raise TimelineError("schema=4 cannot contain C calibration records")
    expected = {
        "samples": len(samples),
        "frames": len(frames),
        "queueSubmits": len(queues),
    }
    if schema == 5:
        expected["calibrations"] = len(calibrations)
    for field, actual in expected.items():
        declared = numeric_header[field]
        if declared != actual:
            raise TimelineError(f"header {field}={declared}, parsed {actual}; file is incomplete")
    return ParsedTimeline(header, frames, queues, calibrations, samples)


def _mark(reasons: dict[int, set[str]], epochs: Iterable[int], reason: str) -> None:
    for epoch in epochs:
        reasons[epoch].add(reason)


def _merge(intervals: Sequence[tuple[int, int]]) -> tuple[int, int, int]:
    if not intervals:
        return 0, 0, 0
    ordered = sorted(intervals)
    first = ordered[0][0]
    merged_end = ordered[0][1]
    union = 0
    for begin, end in ordered[1:]:
        if begin <= merged_end:
            merged_end = max(merged_end, end)
        else:
            union += merged_end - first
            first, merged_end = begin, end
    union += merged_end - first
    span = max(end for _, end in ordered) - min(begin for begin, _ in ordered)
    return union, span, span - union


def _percentile(values: Sequence[float], percentile: float) -> float:
    if not values:
        return math.nan
    ordered = sorted(values)
    position = (len(ordered) - 1) * percentile
    low = math.floor(position)
    high = math.ceil(position)
    if low == high:
        return ordered[low]
    fraction = position - low
    return ordered[low] * (1.0 - fraction) + ordered[high] * fraction


def _unwrap_samples(samples: Sequence[Sample], reasons: dict[int, set[str]]) -> tuple[list[Interval], int, float]:
    if not samples:
        raise TimelineError("no timestamp samples")
    domains = {(sample.valid_bits, sample.period_ns) for sample in samples}
    valid_bits_set = {item[0] for item in domains}
    if len(valid_bits_set) != 1:
        raise TimelineError("timestamp validBits differ; cross-domain GPU ticks cannot be merged")
    valid_bits = next(iter(valid_bits_set))
    if valid_bits <= 0 or valid_bits > 64:
        raise TimelineError(f"invalid validBits={valid_bits}")
    periods = [sample.period_ns for sample in samples]
    period_ns = periods[0]
    if not math.isfinite(period_ns) or period_ns <= 0:
        raise TimelineError(f"invalid timestamp period {period_ns}")
    if any(not math.isclose(period, period_ns, rel_tol=1e-9, abs_tol=1e-12) for period in periods[1:]):
        raise TimelineError("timestamp periodNs differ; GPU tick domains cannot be merged")

    modulus = 1 << valid_bits
    half_modulus = modulus >> 1
    ordered = sorted(samples, key=lambda sample: (sample.physical_sequence, sample.physical_index, sample.scheduler, sample.submit))
    intervals: list[Interval] = []
    previous_begin: int | None = None
    seen_positions: set[tuple[int, int]] = set()
    for sample in ordered:
        position = (sample.physical_sequence, sample.physical_index)
        if position in seen_positions:
            _mark(reasons, (sample.epoch,), "duplicate physicalSequence/index")
        seen_positions.add(position)
        mask = modulus - 1
        raw_begin = sample.begin & mask
        raw_end = sample.end & mask
        duration = (raw_end - raw_begin) & mask
        if duration >= half_modulus:
            _mark(reasons, (sample.epoch,), "ambiguous timestamp duration across wrap")
        if previous_begin is None:
            begin_tick = raw_begin
        elif raw_begin >= (previous_begin % modulus):
            begin_tick = previous_begin - (previous_begin % modulus) + raw_begin
        else:
            begin_tick = previous_begin - (previous_begin % modulus) + modulus + raw_begin
        if previous_begin is not None and begin_tick < previous_begin:
            raise AssertionError("timestamp unwrapping regressed")
        end_tick = begin_tick + duration
        intervals.append(Interval(sample, begin_tick, end_tick))
        previous_begin = begin_tick
    return intervals, valid_bits, period_ns


def _map_gpu_tick_to_cpu_ns(
    tick: int,
    sequence: int,
    calibrations: Sequence[Calibration],
    valid_bits: int,
    period_ns: float,
) -> tuple[float, float]:
    """Map a GPU tick to steady_clock using the nearest queue calibration.

    Device timestamps use only ``valid_bits`` and may wrap.  A signed modular
    delta relative to the calibration keeps the conversion correct across the
    wrap nearest to that calibration.  Calibrations are emitted every few
    physical queue submissions, so queue sequence is the unambiguous nearest
    calibration selector.
    """
    calibration = min(
        calibrations,
        key=lambda item: (abs(item.sequence - sequence), item.sequence),
    )
    modulus = 1 << valid_bits
    half_modulus = modulus >> 1
    mask = modulus - 1
    delta_ticks = ((tick & mask) - (calibration.device_ticks & mask)) & mask
    if delta_ticks >= half_modulus:
        delta_ticks -= modulus
    return (
        calibration.cpu_midpoint_ns + delta_ticks * period_ns,
        calibration.uncertainty_ns,
    )


def _classify_inter_batch_gaps(
    intervals: Sequence[Interval],
    queues: Sequence[QueueSubmit],
    calibrations: Sequence[Calibration],
    valid_bits: int,
    period_ns: float,
    complete_epochs: set[int],
) -> InterBatchClassification:
    """Classify positive gaps between timestamped physical queue batches."""
    queue_by_sequence = {submit.sequence: submit for submit in queues}
    batches: list[tuple[int, int, int, set[int]]] = []
    for interval in intervals:
        sequence = interval.sample.physical_sequence
        if batches and batches[-1][0] == sequence:
            old_sequence, begin, end, epochs = batches[-1]
            epochs.add(interval.sample.epoch)
            batches[-1] = (
                old_sequence,
                min(begin, interval.begin_tick),
                max(end, interval.end_tick),
                epochs,
            )
        else:
            batches.append((
                sequence,
                interval.begin_tick,
                interval.end_tick,
                {interval.sample.epoch},
            ))

    host_late_ms = 0.0
    host_late_count = 0
    prequeued_ms = 0.0
    prequeued_count = 0
    ambiguous_ms = 0.0
    ambiguous_count = 0
    for previous, current in zip(batches, batches[1:]):
        previous_sequence, _, previous_end, previous_epochs = previous
        current_sequence, current_begin, _, current_epochs = current
        if not previous_epochs or not current_epochs:
            continue
        if not previous_epochs.issubset(complete_epochs) or not current_epochs.issubset(complete_epochs):
            continue
        if current_sequence <= previous_sequence:
            continue
        gap_ticks = current_begin - previous_end
        if gap_ticks <= 0:
            continue
        next_submit = queue_by_sequence.get(current_sequence)
        if next_submit is None:
            continue

        mapped_end_ns, uncertainty_ns = _map_gpu_tick_to_cpu_ns(
            previous_end,
            previous_sequence,
            calibrations,
            valid_bits,
            period_ns,
        )
        gap_ms = gap_ticks * period_ns / 1_000_000.0
        if next_submit.cpu_begin_ns > mapped_end_ns + uncertainty_ns:
            host_late_count += 1
            host_late_ms += gap_ms
        elif next_submit.cpu_begin_ns < mapped_end_ns - uncertainty_ns:
            prequeued_count += 1
            prequeued_ms += gap_ms
        else:
            ambiguous_count += 1
            ambiguous_ms += gap_ms

    return InterBatchClassification(
        definitely_host_late_ms=host_late_ms,
        definitely_host_late_count=host_late_count,
        prequeued_gap_ms=prequeued_ms,
        prequeued_gap_count=prequeued_count,
        ambiguous_ms=ambiguous_ms,
        ambiguous_count=ambiguous_count,
    )


def analyze(parsed: ParsedTimeline, trim_epochs: int = 3) -> Analysis:
    if trim_epochs < 0:
        raise TimelineError("trim_epochs must be non-negative")
    frames = sorted(parsed.frames, key=lambda frame: frame.boundary)
    queues = sorted(parsed.queues, key=lambda submit: submit.sequence)
    calibrations = sorted(parsed.calibrations, key=lambda item: item.sequence)
    samples = list(parsed.samples)
    reasons: dict[int, set[str]] = defaultdict(set)
    notes: list[str] = []
    schema = int(parsed.header["schema"], 0)

    frame_epochs = [frame.epoch for frame in frames]
    if len(set(frame_epochs)) != len(frame_epochs):
        raise TimelineError("duplicate F epoch")
    if len({frame.boundary for frame in frames}) != len(frames):
        raise TimelineError("duplicate F boundary")
    frame_interval_by_epoch: dict[int, FrameInterval] = {}
    for previous, current in zip(frames, frames[1:]):
        contiguous = (
            current.boundary == previous.boundary + 1
            and current.epoch == previous.epoch + 1
        )
        if not contiguous:
            _mark(reasons, (previous.epoch, current.epoch), "non-contiguous F boundary/epoch")
        if current.cpu_ns <= previous.cpu_ns:
            _mark(reasons, (previous.epoch, current.epoch), "non-increasing F cpuNs")
        if contiguous and current.cpu_ns > previous.cpu_ns:
            frame_interval_by_epoch[current.epoch] = FrameInterval(
                epoch=current.epoch,
                boundary=current.boundary,
                flip_arg=current.flip_arg,
                is_eop=current.is_eop,
                interval_ms=(current.cpu_ns - previous.cpu_ns) / 1_000_000.0,
            )

    if int(parsed.header["droppedSamples"], 0) != 0:
        raise TimelineError("droppedSamples is non-zero; no strict completeness proof is possible")
    if (
        len(samples) >= 16384
        or len(frames) >= 2048
        or len(queues) >= 16384
        or (schema >= 5 and len(calibrations) >= 4096)
    ):
        raise TimelineError(
            f"timeline storage reached a schema={schema} fixed capacity; tail completeness is unknown"
        )
    try:
        current_epoch = int(parsed.header["currentEpoch"], 0)
    except ValueError as exc:
        raise TimelineError(f"invalid currentEpoch={parsed.header['currentEpoch']!r}") from exc
    if frames and current_epoch != frames[-1].epoch + 1:
        raise TimelineError(
            f"currentEpoch={current_epoch} does not immediately follow last closed F epoch={frames[-1].epoch}"
        )
    if not queues:
        raise TimelineError("no Q records")
    queue_by_sequence: dict[int, QueueSubmit] = {}
    unowned_work_sequences: list[int] = []
    for submit in queues:
        if submit.sequence <= 0:
            raise TimelineError("Q sequence must be positive")
        if submit.sequence in queue_by_sequence:
            raise TimelineError(f"duplicate Q sequence={submit.sequence}")
        queue_by_sequence[submit.sequence] = submit
        if submit.cpu_end_ns < submit.cpu_begin_ns:
            raise TimelineError(f"Q sequence={submit.sequence} has negative CPU submit delta")
    for previous, current in zip(queues, queues[1:]):
        if current.sequence != previous.sequence + 1:
            raise TimelineError(
                f"Q sequence gap {previous.sequence}->{current.sequence}; queuePresent/emptyFence must be explicit Q records"
            )

    seen_calibration_sequences: set[int] = set()
    for calibration in calibrations:
        if calibration.sequence <= 0:
            raise TimelineError("C sequence must be positive")
        if calibration.sequence in seen_calibration_sequences:
            raise TimelineError(f"duplicate C sequence={calibration.sequence}")
        seen_calibration_sequences.add(calibration.sequence)
        if calibration.sequence not in queue_by_sequence:
            raise TimelineError(
                f"C sequence={calibration.sequence} references missing Q sequence"
            )
        if calibration.cpu_before_ns < 0 or calibration.cpu_after_ns < 0:
            raise TimelineError(
                f"C sequence={calibration.sequence} has negative CPU timestamp"
            )
        if calibration.cpu_after_ns < calibration.cpu_before_ns:
            raise TimelineError(
                f"C sequence={calibration.sequence} has negative CPU calibration interval"
            )
        if calibration.device_ticks < 0 or calibration.host_timestamp < 0:
            raise TimelineError(
                f"C sequence={calibration.sequence} has negative calibrated timestamp"
            )
        if calibration.max_deviation_ns < 0:
            raise TimelineError(
                f"C sequence={calibration.sequence} has negative maxDeviationNs"
            )
    for previous, current in zip(calibrations, calibrations[1:]):
        if current.cpu_before_ns < previous.cpu_before_ns:
            raise TimelineError("C cpuBeforeNs regresses with queue sequence")

    samples_by_scheduler: dict[str, list[Sample]] = defaultdict(list)
    for sample in samples:
        samples_by_scheduler[sample.scheduler].append(sample)
        if sample.kind == "draw" and sample.physical_sequence <= 0:
            _mark(reasons, (sample.epoch,), "draw sample has physicalSequence=0")
        if sample.recorded < sample.collected:
            _mark(reasons, (sample.epoch,), "recorded counter below collected")
        if sample.cpu_record_end_ns < sample.cpu_record_begin_ns:
            _mark(reasons, (sample.epoch,), "negative CPU record delta")
        if sample.cpu_submit_end_ns < sample.cpu_submit_begin_ns:
            _mark(reasons, (sample.epoch,), "negative sample CPU submit delta")
    for scheduler_samples in samples_by_scheduler.values():
        ordered_scheduler = sorted(scheduler_samples, key=lambda sample: sample.collected)
        for previous, current in zip(ordered_scheduler, ordered_scheduler[1:]):
            if current.collected != previous.collected + 1:
                _mark(reasons, (previous.epoch, current.epoch), "non-contiguous collected counter")
            if current.ring_full_skips != previous.ring_full_skips:
                low, high = sorted((previous.epoch, current.epoch))
                _mark(reasons, range(low, high + 1), "ringFullSkips delta")

    intervals, valid_bits, period_ns = _unwrap_samples(samples, reasons)
    samples_by_q: dict[int, list[Sample]] = defaultdict(list)
    intervals_by_epoch: dict[int, list[Interval]] = defaultdict(list)
    positions_by_epoch: dict[int, list[int]] = defaultdict(list)
    for position, interval in enumerate(intervals):
        sample = interval.sample
        intervals_by_epoch[sample.epoch].append(interval)
        positions_by_epoch[sample.epoch].append(position)
        samples_by_q[sample.physical_sequence].append(sample)
        submit = queue_by_sequence.get(sample.physical_sequence)
        if submit is None:
            _mark(reasons, (sample.epoch,), "sample references missing Q sequence")
            continue
        if submit.result != 0:
            _mark(reasons, (sample.epoch,), "Vulkan queue submit failed")
        if submit.kind != sample.kind:
            _mark(reasons, (sample.epoch,), "sample/Q scheduler kind mismatch")
        if sample.physical_index >= submit.logical:
            _mark(reasons, (sample.epoch,), "physicalIndex outside Q logical range")
        if submit.logical == 1 and sample.logical_tick != submit.first_tick:
            _mark(reasons, (sample.epoch,), "sample logicalTick mismatches Q tick")
        if sample.cpu_submit_begin_ns != submit.cpu_begin_ns or sample.cpu_submit_end_ns != submit.cpu_end_ns:
            _mark(reasons, (sample.epoch,), "sample/Q CPU submit timestamps mismatch")

    direct_q_owner: dict[int, int] = {}
    for submit in queues:
        linked = samples_by_q.get(submit.sequence, [])
        linked_epochs = {sample.epoch for sample in linked}
        if len(linked_epochs) > 1:
            _mark(reasons, linked_epochs, "one Q sequence straddles epochs")
        elif linked_epochs:
            direct_q_owner[submit.sequence] = next(iter(linked_epochs))
        linked_positions = [sample.physical_index for sample in linked]
        if len(linked_positions) != len(set(linked_positions)):
            _mark(reasons, linked_epochs, "duplicate physicalIndex in Q")
        if submit.command_buffers > 0 and len(linked) != submit.command_buffers:
            adjacent = linked_epochs or set(frame_epochs)
            if linked_epochs:
                _mark(reasons, adjacent, "Q command buffer lacks timestamp sample")
        if not linked and not submit.benign_without_timestamp and submit.command_buffers > 0:
            unowned_work_sequences.append(submit.sequence)

    q_owner = dict(direct_q_owner)
    previous_owner: int | None = None
    for submit in queues:
        owner = direct_q_owner.get(submit.sequence)
        if owner is not None:
            previous_owner = owner
        elif submit.benign_without_timestamp and previous_owner is not None:
            q_owner[submit.sequence] = previous_owner
    for submit in queues:
        owner = q_owner.get(submit.sequence)
        if submit.result != 0 and owner is not None:
            _mark(reasons, (owner,), "Vulkan queue submit/present failed")
    if unowned_work_sequences:
        preview = ",".join(str(sequence) for sequence in unowned_work_sequences[:8])
        suffix = ",..." if len(unowned_work_sequences) > 8 else ""
        notes.append(
            f"untimestamped regular GPU-work Q records={len(unowned_work_sequences)} sequences={preview}{suffix}; affected epochs are excluded"
        )

    ordered_epochs = [interval.sample.epoch for interval in intervals]
    for epoch, positions in positions_by_epoch.items():
        if positions[-1] - positions[0] + 1 != len(positions):
            _mark(reasons, (epoch,), "epoch samples are interleaved/straddled")
    for previous, current in zip(ordered_epochs, ordered_epochs[1:]):
        if current < previous:
            _mark(reasons, (previous, current), "GPU physical order regresses across epochs")

    sampled_epochs = sorted(intervals_by_epoch)
    for previous_epoch, current_epoch in zip(sampled_epochs, sampled_epochs[1:]):
        previous_intervals = intervals_by_epoch[previous_epoch]
        current_intervals = intervals_by_epoch[current_epoch]
        previous_last_sequence = max(item.sample.physical_sequence for item in previous_intervals)
        current_first_sequence = min(item.sample.physical_sequence for item in current_intervals)
        if previous_epoch + 1 != current_epoch:
            _mark(reasons, (previous_epoch, current_epoch), "missing sampled epoch")
        if current_first_sequence <= previous_last_sequence:
            _mark(reasons, (previous_epoch, current_epoch), "epoch physical Q ranges overlap")
            continue
        for sequence in range(previous_last_sequence + 1, current_first_sequence):
            submit = queue_by_sequence.get(sequence)
            if submit is None:
                _mark(reasons, (previous_epoch, current_epoch), "Q gap between epochs")
            elif not submit.benign_without_timestamp:
                _mark(reasons, (previous_epoch, current_epoch), "untimestamped GPU-work Q between epochs")

    frame_by_epoch = {frame.epoch: frame for frame in frames}
    period_ms = period_ns / 1_000_000.0
    metrics_by_epoch: dict[int, FrameMetrics] = {}
    for epoch, epoch_intervals in intervals_by_epoch.items():
        frame = frame_by_epoch.get(epoch)
        if frame is None:
            reasons[epoch].add("sample epoch has no closing F record")
            continue
        if not any(interval.sample.kind == "draw" for interval in epoch_intervals):
            reasons[epoch].add("epoch has no draw sample")
        first_sequence = min(interval.sample.physical_sequence for interval in epoch_intervals)
        last_sequence = max(interval.sample.physical_sequence for interval in epoch_intervals)
        for sequence in range(first_sequence, last_sequence + 1):
            submit = queue_by_sequence.get(sequence)
            if submit is None:
                reasons[epoch].add("missing Q inside epoch sequence range")
            elif sequence not in samples_by_q and not submit.benign_without_timestamp:
                reasons[epoch].add("untimestamped GPU-work Q inside epoch")
            elif sequence in samples_by_q and q_owner.get(sequence) != epoch:
                reasons[epoch].add("Q inside epoch owned by another epoch")

        raw_intervals = [(item.begin_tick, item.end_tick) for item in epoch_intervals]
        union_ticks, span_ticks, gap_ticks = _merge(raw_intervals)
        kind_lower_bound_ms: dict[str, float] = {}
        for kind in sorted(KINDS):
            kind_intervals = [
                (item.begin_tick, item.end_tick)
                for item in epoch_intervals
                if item.sample.kind == kind
            ]
            kind_union, _, _ = _merge(kind_intervals)
            kind_lower_bound_ms[kind] = kind_union * period_ms
        cpu_record_sum_ns = sum(
            item.sample.cpu_record_end_ns - item.sample.cpu_record_begin_ns
            for item in epoch_intervals
        )
        owned_submits = [submit for submit in queues if q_owner.get(submit.sequence) == epoch]
        cpu_submit_sum_ns = sum(submit.cpu_end_ns - submit.cpu_begin_ns for submit in owned_submits)
        metrics_by_epoch[epoch] = FrameMetrics(
            epoch=epoch,
            boundary=frame.boundary,
            sample_count=len(epoch_intervals),
            queue_submit_count=len(owned_submits),
            gpu_union_ms=union_ticks * period_ms,
            gpu_span_ms=span_ticks * period_ms,
            gpu_internal_gap_ms=gap_ticks * period_ms,
            gpu_gap_before_ms=None,
            cpu_record_sum_ms=cpu_record_sum_ns / 1_000_000.0,
            cpu_submit_sum_ms=cpu_submit_sum_ns / 1_000_000.0,
            kind_lower_bound_ms=kind_lower_bound_ms,
            first_tick=min(begin for begin, _ in raw_intervals),
            last_tick=max(end for _, end in raw_intervals),
            first_sequence=first_sequence,
            last_sequence=last_sequence,
        )

    for previous, current in zip(frames, frames[1:]):
        previous_metrics = metrics_by_epoch.get(previous.epoch)
        current_metrics = metrics_by_epoch.get(current.epoch)
        if previous_metrics is None or current_metrics is None:
            continue
        if current_metrics.first_tick < previous_metrics.last_tick:
            _mark(reasons, (previous.epoch, current.epoch), "GPU intervals overlap adjacent epochs")
        else:
            current_metrics.gpu_gap_before_ms = (
                current_metrics.first_tick - previous_metrics.last_tick
            ) * period_ms

    for previous, current in zip(frames, frames[1:]):
        current_metrics = metrics_by_epoch.get(current.epoch)
        if current_metrics is not None and reasons.get(previous.epoch):
            current_metrics.gpu_gap_before_ms = None

    trimmed_frames = frames[trim_epochs:len(frames) - trim_epochs if trim_epochs else None]
    frame_intervals = [
        frame_interval_by_epoch[frame.epoch]
        for frame in trimmed_frames
        if frame.epoch in frame_interval_by_epoch
    ]
    complete: list[FrameMetrics] = []
    complete_metrics_by_epoch: dict[int, FrameMetrics] = {}
    epoch_exclusion_reasons: dict[int, tuple[str, ...]] = {}
    excluded_reasons: Counter[str] = Counter()
    for frame in trimmed_frames:
        frame_metrics = metrics_by_epoch.get(frame.epoch)
        epoch_reasons = reasons.get(frame.epoch, set())
        if frame_metrics is None:
            epoch_reasons = set(epoch_reasons)
            epoch_reasons.add("no measurable epoch metrics")
        if epoch_reasons:
            excluded_reasons.update(epoch_reasons)
        else:
            complete.append(frame_metrics)
            complete_metrics_by_epoch[frame.epoch] = frame_metrics
        epoch_exclusion_reasons[frame.epoch] = tuple(sorted(epoch_reasons))

    inter_batch_classification: InterBatchClassification | None = None
    inter_batch_classification_unavailable_reason: str | None = None
    zero_only_device_calibration = (
        bool(calibrations)
        and all(calibration.device_ticks == 0 for calibration in calibrations)
        and any(sample.begin != 0 or sample.end != 0 for sample in samples)
    )
    if zero_only_device_calibration:
        inter_batch_classification_unavailable_reason = (
            "all calibration deviceTicks values are zero while timestamp query "
            "ticks are non-zero"
        )
        notes.append(
            "Calibrated host-late/prequeued inter-batch attribution is unavailable: "
            f"{inter_batch_classification_unavailable_reason}. Relative GPU duration "
            "distributions remain valid because they do not require CPU/GPU clock-domain mapping."
        )
    elif calibrations:
        inter_batch_classification = _classify_inter_batch_gaps(
            intervals,
            queues,
            calibrations,
            valid_bits,
            period_ns,
            {frame.epoch for frame in complete},
        )
        notes.append(
            "Calibrated inter-batch attribution is conservative: the GPU endpoint uses the nearest queue calibration midpoint and uncertainty=maxDeviation+(cpuAfter-cpuBefore)/2."
        )
    else:
        if schema == 4:
            notes.append(
                "CPU record/submit values are summed CPU API intervals; GPU ticks are not calibrated to CLOCK_MONOTONIC, so CPU/GPU overlap is not inferred."
            )
        else:
            notes.append(
                "CPU record/submit values are summed CPU API intervals; no calibrated timestamps are present, so CPU/GPU overlap is not inferred."
            )
    notes.append(
        "Scheduler-kind values are per-kind merged timestamp coverage lower bounds and are not additive when intervals overlap."
    )
    notes.append(
        "frame_interval_ms is the CLOCK_MONOTONIC interval between contiguous F flip-boundary records; it is reported for all eligible intervals independently of strict GPU timestamp completeness."
    )
    notes.append(
        "cpu_record_sum_ms and cpu_submit_sum_ms are summed host API intervals; cpu_submit_sum_ms is not GPU queue residence and neither value alone is total CPU frame preparation."
    )
    return Analysis(
        schema=schema,
        valid_bits=valid_bits,
        period_ns=period_ns,
        parsed_frames=len(frames),
        eligible_after_trim=len(trimmed_frames),
        frame_intervals=frame_intervals,
        complete_frames=complete,
        complete_metrics_by_epoch=complete_metrics_by_epoch,
        epoch_exclusion_reasons=epoch_exclusion_reasons,
        excluded_reasons=excluded_reasons,
        global_notes=notes,
        calibration_count=len(calibrations),
        inter_batch_classification=inter_batch_classification,
        inter_batch_classification_unavailable_reason=(
            inter_batch_classification_unavailable_reason
        ),
    )


def _distribution(values: Sequence[float]) -> dict[str, float | int]:
    return {
        "n": len(values),
        "mean": sum(values) / len(values),
        "p50": _percentile(values, 0.50),
        "p90": _percentile(values, 0.90),
        "p95": _percentile(values, 0.95),
        "p99": _percentile(values, 0.99),
        "max": max(values),
    }


def build_report(
    analysis: Analysis,
    min_frames: int,
    long_frame_ms: float = 60.0,
) -> dict[str, object]:
    if not math.isfinite(long_frame_ms) or long_frame_ms <= 0:
        raise TimelineError("long_frame_ms must be finite and positive")
    frames = analysis.complete_frames
    frame_intervals = analysis.frame_intervals
    report: dict[str, object] = {
        "schema": analysis.schema,
        "tick_domain": {
            "valid_bits": analysis.valid_bits,
            "period_ns": analysis.period_ns,
        },
        "frames": {
            "parsed": analysis.parsed_frames,
            "eligible_after_trim": analysis.eligible_after_trim,
            "frame_intervals": len(frame_intervals),
            "strict_complete": len(frames),
            "minimum_required": min_frames,
            "sufficient": len(frames) >= min_frames,
        },
        "excluded_reasons": dict(analysis.excluded_reasons.most_common()),
        "notes": analysis.global_notes,
    }
    distributions: dict[str, dict[str, float | int]] = {}
    report["distributions"] = distributions
    if frame_intervals:
        distributions["frame_interval_ms"] = _distribution(
            [frame.interval_ms for frame in frame_intervals]
        )

    long_frame_records: list[dict[str, object]] = []
    strict_long_frame_count = 0
    for frame in frame_intervals:
        if frame.interval_ms <= long_frame_ms:
            continue
        metrics = analysis.complete_metrics_by_epoch.get(frame.epoch)
        strict_complete = metrics is not None
        strict_long_frame_count += int(strict_complete)
        long_frame_records.append({
            "boundary": frame.boundary,
            "epoch": frame.epoch,
            "flip_arg": frame.flip_arg,
            "is_eop": frame.is_eop,
            "frame_interval_ms": frame.interval_ms,
            "strict_gpu_complete": strict_complete,
            "excluded_reasons": list(
                analysis.epoch_exclusion_reasons.get(frame.epoch, ())
            ),
            "cpu_record_sum_ms": (
                metrics.cpu_record_sum_ms if metrics is not None else None
            ),
            "cpu_submit_sum_ms": (
                metrics.cpu_submit_sum_ms if metrics is not None else None
            ),
            "gpu_union_ms": metrics.gpu_union_ms if metrics is not None else None,
            "gpu_span_ms": metrics.gpu_span_ms if metrics is not None else None,
            "gpu_internal_gap_ms": (
                metrics.gpu_internal_gap_ms if metrics is not None else None
            ),
            "gpu_gap_before_ms": (
                metrics.gpu_gap_before_ms if metrics is not None else None
            ),
            "timestamp_samples": metrics.sample_count if metrics is not None else None,
            "queue_submits": (
                metrics.queue_submit_count if metrics is not None else None
            ),
        })
    report["long_frames"] = {
        "threshold_ms": long_frame_ms,
        "comparison": ">",
        "count": len(long_frame_records),
        "strict_gpu_complete": strict_long_frame_count,
        "without_strict_gpu_breakdown": (
            len(long_frame_records) - strict_long_frame_count
        ),
        "records": long_frame_records,
    }
    if analysis.schema >= 5:
        calibration_report: dict[str, object] = {
            "records": analysis.calibration_count,
            "available": analysis.inter_batch_classification is not None,
        }
        if analysis.inter_batch_classification_unavailable_reason is not None:
            calibration_report["unavailable_reason"] = (
                analysis.inter_batch_classification_unavailable_reason
            )
        report["calibrations"] = calibration_report
        if analysis.inter_batch_classification is not None:
            classification = analysis.inter_batch_classification
            report["inter_batch_classification"] = {
                "definitely_host_late_ms": classification.definitely_host_late_ms,
                "definitely_host_late_count": classification.definitely_host_late_count,
                "prequeued_gap_ms": classification.prequeued_gap_ms,
                "prequeued_gap_count": classification.prequeued_gap_count,
                "ambiguous_ms": classification.ambiguous_ms,
                "ambiguous_count": classification.ambiguous_count,
            }
    if len(frames) < min_frames:
        report["error"] = (
            f"only {len(frames)} strict-complete frames; at least {min_frames} are required for percentile reporting"
        )
        return report

    metrics = {
        "gpu_union_ms": [frame.gpu_union_ms for frame in frames],
        "gpu_span_ms": [frame.gpu_span_ms for frame in frames],
        "gpu_internal_gap_ms": [frame.gpu_internal_gap_ms for frame in frames],
        "gpu_gap_before_ms": [
            frame.gpu_gap_before_ms
            for frame in frames
            if frame.gpu_gap_before_ms is not None
        ],
        "cpu_record_sum_ms": [frame.cpu_record_sum_ms for frame in frames],
        "cpu_submit_sum_ms": [frame.cpu_submit_sum_ms for frame in frames],
        "timestamp_samples_per_frame": [float(frame.sample_count) for frame in frames],
        "queue_submits_per_frame": [float(frame.queue_submit_count) for frame in frames],
    }
    distributions.update({
        name: _distribution(values) for name, values in metrics.items() if values
    })
    report["scheduler_kind_lower_bound_ms"] = {
        kind: _distribution([frame.kind_lower_bound_ms[kind] for frame in frames])
        for kind in sorted(KINDS)
    }
    return report


def _format_distribution(name: str, values: dict[str, float | int]) -> str:
    return (
        f"{name:<29} n={values['n']:>4} mean={values['mean']:>9.3f} "
        f"p50={values['p50']:>9.3f} p90={values['p90']:>9.3f} "
        f"p95={values['p95']:>9.3f} p99={values['p99']:>9.3f} "
        f"max={values['max']:>9.3f}"
    )


def _format_optional_ms(value: object) -> str:
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return f"{float(value):.3f}"
    return "n/a"


def print_human(report: dict[str, object]) -> None:
    tick = report["tick_domain"]
    frames = report["frames"]
    assert isinstance(tick, dict) and isinstance(frames, dict)
    print(
        f"GPU timeline schema={report['schema']} validBits={tick['valid_bits']} "
        f"periodNs={tick['period_ns']}"
    )
    print(
        "frames: "
        f"parsed={frames['parsed']} eligible-after-trim={frames['eligible_after_trim']} "
        f"frame-intervals={frames.get('frame_intervals', 0)} "
        f"strict-complete={frames['strict_complete']} required={frames['minimum_required']}"
    )
    excluded = report.get("excluded_reasons", {})
    if isinstance(excluded, dict) and excluded:
        print("excluded:")
        for reason, count in excluded.items():
            print(f"  {count:>4}  {reason}")
    if "error" in report:
        print(f"ERROR: {report['error']}")
    else:
        print("per-frame distributions:")
        distributions = report["distributions"]
        assert isinstance(distributions, dict)
        for name, values in distributions.items():
            assert isinstance(values, dict)
            print(_format_distribution(name, values))
        print("scheduler-kind timestamp coverage lower bounds (non-additive):")
        kind_values = report["scheduler_kind_lower_bound_ms"]
        assert isinstance(kind_values, dict)
        for kind, values in kind_values.items():
            assert isinstance(values, dict)
            print(_format_distribution(kind, values))
    long_frames = report.get("long_frames")
    if isinstance(long_frames, dict):
        print(
            f"long flip-boundary intervals {long_frames['comparison']}"
            f"{long_frames['threshold_ms']:.3f} ms: count={long_frames['count']} "
            f"strict-gpu-complete={long_frames['strict_gpu_complete']} "
            f"without-breakdown={long_frames['without_strict_gpu_breakdown']}"
        )
        records = long_frames.get("records", [])
        if isinstance(records, list) and records:
            print(
                "  boundary epoch frame-ms cpu-record cpu-submit gpu-union "
                "gpu-span gpu-internal-gap gpu-gap-before"
            )
            for record in records:
                assert isinstance(record, dict)
                print(
                    f"  {record['boundary']:>8} {record['epoch']:>5} "
                    f"{_format_optional_ms(record['frame_interval_ms']):>8} "
                    f"{_format_optional_ms(record['cpu_record_sum_ms']):>10} "
                    f"{_format_optional_ms(record['cpu_submit_sum_ms']):>10} "
                    f"{_format_optional_ms(record['gpu_union_ms']):>9} "
                    f"{_format_optional_ms(record['gpu_span_ms']):>8} "
                    f"{_format_optional_ms(record['gpu_internal_gap_ms']):>16} "
                    f"{_format_optional_ms(record['gpu_gap_before_ms']):>14}"
                )
                reasons = record.get("excluded_reasons", [])
                if isinstance(reasons, list) and reasons:
                    print(f"           excluded: {', '.join(str(item) for item in reasons)}")
    calibrations = report.get("calibrations")
    if isinstance(calibrations, dict):
        print(
            "calibrations: "
            f"records={calibrations['records']} available={calibrations['available']}"
        )
        unavailable_reason = calibrations.get("unavailable_reason")
        if isinstance(unavailable_reason, str):
            print(
                "calibrated inter-batch host/prequeued classification: "
                f"unavailable ({unavailable_reason})"
            )
    classification = report.get("inter_batch_classification")
    if isinstance(classification, dict):
        print("calibrated inter-batch positive GPU gaps:")
        print(
            "  definitely-host-late "
            f"count={classification['definitely_host_late_count']} "
            f"total-ms={classification['definitely_host_late_ms']:.3f}"
        )
        print(
            "  prequeued            "
            f"count={classification['prequeued_gap_count']} "
            f"total-ms={classification['prequeued_gap_ms']:.3f}"
        )
        print(
            "  ambiguous            "
            f"count={classification['ambiguous_count']} "
            f"total-ms={classification['ambiguous_ms']:.3f}"
        )
    print("caveats:")
    for note in report.get("notes", []):
        print(f"  - {note}")


def _synthetic_timeline() -> str:
    frame_count = 12
    first_epoch = 100
    frames: list[str] = []
    queues: list[str] = []
    samples: list[str] = []
    for index in range(frame_count):
        epoch = first_epoch + index
        draw_sequence = index * 2 + 1
        present_sequence = draw_sequence + 1
        begin = (240 + index * 15) & 0xFF
        end = (begin + 10) & 0xFF
        frames.append(
            f"F boundary={500 + index} epoch={epoch} cpuNs={1_000_000 + index * 16_000_000} flipArg={index} isEop=1"
        )
        queues.append(
            f"Q sequence={draw_sequence} kind=draw firstTick={1000 + index} lastTick={1000 + index} "
            f"cpuBeginNs={2000 + index * 100} cpuEndNs={2020 + index * 100} logical=1 "
            "commandBuffers=1 result=0 forced=1 emptyFence=0 queuePresent=0"
        )
        queues.append(
            f"Q sequence={present_sequence} kind=present firstTick=0 lastTick=0 "
            f"cpuBeginNs={2030 + index * 100} cpuEndNs={2040 + index * 100} logical=0 "
            "commandBuffers=0 result=0 forced=1 emptyFence=0 queuePresent=1"
        )
        samples.append(
            f"S scheduler=0x1 kind=draw epoch={epoch} submit={index + 1} logicalTick={1000 + index} "
            f"physicalSequence={draw_sequence} physicalIndex=0 begin={begin} end={end} validBits=8 "
            f"periodNs=1.0 cpuRecordBeginNs={1000 + index * 100} cpuRecordEndNs={1050 + index * 100} "
            f"cpuSubmitBeginNs={2000 + index * 100} cpuSubmitEndNs={2020 + index * 100} "
            f"attempts={index + 1} recorded={index + 1} collected={index + 1} ringFullSkips=0 notReadyPolls=0"
        )
    header = (
        f"schema=4 samples={frame_count} frames={frame_count} queueSubmits={frame_count * 2} "
        f"droppedSamples=0 currentEpoch={first_epoch + frame_count}"
    )
    return "\n".join([header, *frames, *queues, *samples, ""])


def _synthetic_calibrated_timeline() -> str:
    frame_count = 12
    first_epoch = 200
    base_cpu_ns = 1_000_000
    frames: list[str] = []
    queues: list[str] = []
    calibrations: list[str] = []
    samples: list[str] = []
    for index in range(frame_count):
        epoch = first_epoch + index
        draw_sequence = index * 2 + 1
        present_sequence = draw_sequence + 1
        unwrapped_begin = 240 + index * 15
        begin = unwrapped_begin & 0xFF
        end = (begin + 10) & 0xFF
        mapped_begin_ns = base_cpu_ns + unwrapped_begin
        if index == 0:
            queue_begin_ns = mapped_begin_ns - 3
        else:
            previous_end_ns = base_cpu_ns + (240 + (index - 1) * 15 + 10)
            if index % 3 == 1:
                queue_begin_ns = previous_end_ns + 2
            elif index % 3 == 2:
                queue_begin_ns = previous_end_ns - 2
            else:
                queue_begin_ns = previous_end_ns
        queue_end_ns = queue_begin_ns + 1
        frames.append(
            f"F boundary={700 + index} epoch={epoch} cpuNs={2_000_000 + index * 16_000_000} flipArg={index} isEop=1"
        )
        queues.append(
            f"Q sequence={draw_sequence} kind=draw firstTick={3000 + index} lastTick={3000 + index} "
            f"cpuBeginNs={queue_begin_ns} cpuEndNs={queue_end_ns} logical=1 "
            "commandBuffers=1 result=0 forced=1 emptyFence=0 queuePresent=0"
        )
        queues.append(
            f"Q sequence={present_sequence} kind=present firstTick=0 lastTick=0 "
            f"cpuBeginNs={queue_end_ns + 1} cpuEndNs={queue_end_ns + 2} logical=0 "
            "commandBuffers=0 result=0 forced=1 emptyFence=0 queuePresent=1"
        )
        calibrations.append(
            f"C sequence={draw_sequence} cpuBeforeNs={mapped_begin_ns} "
            f"cpuAfterNs={mapped_begin_ns} deviceTicks={begin} "
            f"hostTimestamp={mapped_begin_ns + 1234} maxDeviationNs=0"
        )
        samples.append(
            f"S scheduler=0x2 kind=draw epoch={epoch} submit={index + 1} logicalTick={3000 + index} "
            f"physicalSequence={draw_sequence} physicalIndex=0 begin={begin} end={end} validBits=8 "
            f"periodNs=1.0 cpuRecordBeginNs={mapped_begin_ns - 20} cpuRecordEndNs={mapped_begin_ns - 10} "
            f"cpuSubmitBeginNs={queue_begin_ns} cpuSubmitEndNs={queue_end_ns} "
            f"attempts={index + 1} recorded={index + 1} collected={index + 1} ringFullSkips=0 notReadyPolls=0"
        )
    header = (
        f"schema=5 samples={frame_count} frames={frame_count} queueSubmits={frame_count * 2} "
        f"calibrations={frame_count} droppedSamples=0 currentEpoch={first_epoch + frame_count}"
    )
    return "\n".join([header, *frames, *queues, *calibrations, *samples, ""])


def self_test() -> None:
    parsed = parse_timeline_text(_synthetic_timeline())
    analysis = analyze(parsed, trim_epochs=3)
    report = build_report(analysis, min_frames=6)
    assert report["frames"]["strict_complete"] == 6
    distributions = report["distributions"]
    assert math.isclose(distributions["frame_interval_ms"]["p99"], 16.0)
    assert math.isclose(distributions["gpu_union_ms"]["p50"], 0.000010)
    assert math.isclose(distributions["gpu_span_ms"]["p95"], 0.000010)
    assert math.isclose(distributions["gpu_internal_gap_ms"]["max"], 0.0)
    assert math.isclose(distributions["gpu_gap_before_ms"]["p50"], 0.000005)
    assert math.isclose(distributions["cpu_record_sum_ms"]["mean"], 0.000050)
    assert math.isclose(distributions["cpu_submit_sum_ms"]["mean"], 0.000030)
    assert report["long_frames"]["count"] == 0
    assert "calibrations" not in report
    assert "inter_batch_classification" not in report

    parsed_calibrated = parse_timeline_text(_synthetic_calibrated_timeline())
    calibrated_analysis = analyze(parsed_calibrated, trim_epochs=3)
    calibrated_report = build_report(calibrated_analysis, min_frames=6)
    assert calibrated_report["schema"] == 5
    assert calibrated_report["calibrations"] == {
        "records": 12,
        "available": True,
    }
    classification = calibrated_report["inter_batch_classification"]
    assert classification["definitely_host_late_count"] == 2
    assert math.isclose(classification["definitely_host_late_ms"], 0.000010)
    assert classification["prequeued_gap_count"] == 2
    assert math.isclose(classification["prequeued_gap_ms"], 0.000010)
    assert classification["ambiguous_count"] == 1
    assert math.isclose(classification["ambiguous_ms"], 0.000005)

    bad_header = _synthetic_calibrated_timeline().replace(
        "calibrations=12", "calibrations=11", 1
    )
    try:
        parse_timeline_text(bad_header)
    except TimelineError as exc:
        assert "header calibrations=11, parsed 12" in str(exc)
    else:
        raise AssertionError("schema=5 calibration count mismatch was accepted")
    print("analyze_gpu_timeline.py self-test: PASS")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Analyze LSX4 runtime/gpu-timeline.txt schema=4/5. Default reporting "
            "requires at least 100 strict-complete epochs after trimming."
        )
    )
    parser.add_argument("timeline", nargs="?", type=Path, help="path to gpu-timeline.txt")
    parser.add_argument("--min-frames", type=int, default=100)
    parser.add_argument("--trim-epochs", type=int, default=3)
    parser.add_argument(
        "--long-frame-ms",
        type=float,
        default=60.0,
        help="list flip-boundary intervals strictly greater than this threshold",
    )
    parser.add_argument("--json", action="store_true", dest="as_json")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)

    if args.self_test:
        self_test()
        return 0
    if args.timeline is None:
        parser.error("timeline path is required unless --self-test is used")
    if args.min_frames <= 0:
        parser.error("--min-frames must be positive")
    if not math.isfinite(args.long_frame_ms) or args.long_frame_ms <= 0:
        parser.error("--long-frame-ms must be finite and positive")
    try:
        parsed = parse_timeline_text(args.timeline.read_text(encoding="utf-8"))
        analysis = analyze(parsed, trim_epochs=args.trim_epochs)
        report = build_report(
            analysis,
            min_frames=args.min_frames,
            long_frame_ms=args.long_frame_ms,
        )
    except (OSError, TimelineError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    if args.as_json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print_human(report)
    return 0 if report["frames"]["sufficient"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
