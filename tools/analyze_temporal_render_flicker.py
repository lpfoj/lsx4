#!/usr/bin/env python3
"""Find transient large-area renderer corruption in a video or PNG series.

The acceptance use case is a fixed in-game camera.  A temporal median rejects
short flashes and becomes the reference frame.  Every decoded frame is then
compared with that reference inside a configurable gameplay ROI.  The report
records both the total changed area and the largest connected changed region;
the latter keeps particles and HUD animation from hiding a one-frame missing
wall, floor, or attachment.
"""

from __future__ import annotations

import argparse
import json
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable

import cv2
import numpy as np


@dataclass(frozen=True)
class FrameMetric:
    frame: int
    seconds: float
    changed_fraction: float
    largest_component_fraction: float
    bright_fraction: float
    dark_fraction: float
    desaturated_fraction: float


def _parse_roi(value: str) -> tuple[float, float, float, float]:
    try:
        left, top, right, bottom = (float(part) for part in value.split(","))
    except (TypeError, ValueError) as exc:
        raise argparse.ArgumentTypeError("ROI must be left,top,right,bottom") from exc
    if not (0.0 <= left < right <= 1.0 and 0.0 <= top < bottom <= 1.0):
        raise argparse.ArgumentTypeError("ROI coordinates must satisfy 0 <= min < max <= 1")
    return left, top, right, bottom


def _open_video(path: Path) -> cv2.VideoCapture:
    capture = cv2.VideoCapture(str(path))
    if not capture.isOpened():
        raise RuntimeError(f"cannot open video: {path}")
    return capture


def _resize(frame: np.ndarray, width: int) -> np.ndarray:
    if frame.shape[1] == width:
        return frame
    height = max(1, round(frame.shape[0] * width / frame.shape[1]))
    return cv2.resize(frame, (width, height), interpolation=cv2.INTER_AREA)


def _crop(frame: np.ndarray, roi: tuple[float, float, float, float]) -> np.ndarray:
    height, width = frame.shape[:2]
    left, top, right, bottom = roi
    x0, x1 = round(left * width), round(right * width)
    y0, y1 = round(top * height), round(bottom * height)
    return frame[y0:y1, x0:x1]


def build_temporal_median(
    video: Path,
    frame_count: int,
    analysis_width: int,
    roi: tuple[float, float, float, float],
    max_samples: int,
) -> np.ndarray:
    if frame_count <= 0:
        raise RuntimeError("video contains no frames")
    sample_indices = np.unique(
        np.linspace(0, frame_count - 1, min(frame_count, max_samples), dtype=np.int64)
    )
    wanted = set(int(index) for index in sample_indices)
    samples: list[np.ndarray] = []
    capture = _open_video(video)
    index = 0
    while wanted:
        ok, frame = capture.read()
        if not ok:
            break
        if index in wanted:
            samples.append(_crop(_resize(frame, analysis_width), roi))
            wanted.remove(index)
        index += 1
    capture.release()
    if not samples:
        raise RuntimeError("video decoder returned no reference samples")
    return np.median(np.stack(samples, axis=0), axis=0).astype(np.uint8)


def measure_frame(
    frame: np.ndarray,
    reference: np.ndarray,
    threshold: int,
) -> tuple[float, float, float, float, float]:
    delta = cv2.absdiff(frame, reference)
    maximum_delta = delta.max(axis=2)
    changed = (maximum_delta >= threshold).astype(np.uint8)
    kernel = np.ones((3, 3), dtype=np.uint8)
    changed = cv2.morphologyEx(changed, cv2.MORPH_OPEN, kernel)
    changed = cv2.morphologyEx(changed, cv2.MORPH_CLOSE, kernel)

    pixel_count = changed.size
    changed_fraction = float(np.count_nonzero(changed)) / pixel_count
    component_count, _, stats, _ = cv2.connectedComponentsWithStats(changed, connectivity=8)
    largest = 0 if component_count <= 1 else int(stats[1:, cv2.CC_STAT_AREA].max())

    frame_luma = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY).astype(np.int16)
    reference_luma = cv2.cvtColor(reference, cv2.COLOR_BGR2GRAY).astype(np.int16)
    luma_delta = frame_luma - reference_luma
    bright_fraction = float(np.count_nonzero(luma_delta >= threshold)) / pixel_count
    dark_fraction = float(np.count_nonzero(luma_delta <= -threshold)) / pixel_count

    frame_saturation = cv2.cvtColor(frame, cv2.COLOR_BGR2HSV)[:, :, 1].astype(np.int16)
    reference_saturation = cv2.cvtColor(reference, cv2.COLOR_BGR2HSV)[:, :, 1].astype(np.int16)
    desaturated_fraction = (
        float(np.count_nonzero(reference_saturation - frame_saturation >= threshold))
        / pixel_count
    )
    return (
        changed_fraction,
        largest / pixel_count,
        bright_fraction,
        dark_fraction,
        desaturated_fraction,
    )


def _series_paths(source: Path) -> list[Path]:
    extensions = {".png", ".jpg", ".jpeg", ".webp"}
    return sorted(
        path for path in source.iterdir()
        if path.is_file() and path.suffix.lower() in extensions
    )


def analyze_series(
    source: Path,
    analysis_width: int,
    roi: tuple[float, float, float, float],
    threshold: int,
    max_reference_samples: int,
    series_fps: float,
) -> tuple[dict[str, float | int], np.ndarray, list[FrameMetric]]:
    paths = _series_paths(source)
    if not paths:
        raise RuntimeError(f"image series contains no supported frames: {source}")
    first = cv2.imread(str(paths[0]), cv2.IMREAD_COLOR)
    if first is None:
        raise RuntimeError(f"cannot decode series frame: {paths[0]}")
    source_height, source_width = first.shape[:2]
    sample_indices = np.unique(
        np.linspace(
            0, len(paths) - 1, min(len(paths), max_reference_samples), dtype=np.int64
        )
    )
    samples: list[np.ndarray] = []
    for index in sample_indices:
        frame = cv2.imread(str(paths[int(index)]), cv2.IMREAD_COLOR)
        if frame is None:
            raise RuntimeError(f"cannot decode series frame: {paths[int(index)]}")
        samples.append(_crop(_resize(frame, analysis_width), roi))
    reference = np.median(np.stack(samples, axis=0), axis=0).astype(np.uint8)

    metrics: list[FrameMetric] = []
    for frame_index, path in enumerate(paths):
        source_frame = cv2.imread(str(path), cv2.IMREAD_COLOR)
        if source_frame is None:
            raise RuntimeError(f"cannot decode series frame: {path}")
        frame = _crop(_resize(source_frame, analysis_width), roi)
        metrics.append(
            FrameMetric(
                frame_index,
                frame_index / series_fps,
                *measure_frame(frame, reference, threshold),
            )
        )
    metadata: dict[str, float | int] = {
        "fps": series_fps,
        "decoded_frames": len(metrics),
        "reported_frames": len(paths),
        "duration_seconds": len(metrics) / series_fps,
        "source_width": source_width,
        "source_height": source_height,
        "analysis_width": analysis_width,
        "threshold": threshold,
    }
    return metadata, reference, metrics


def analyze_video(
    video: Path,
    analysis_width: int,
    roi: tuple[float, float, float, float],
    threshold: int,
    max_reference_samples: int,
) -> tuple[dict[str, float | int], np.ndarray, list[FrameMetric]]:
    capture = _open_video(video)
    fps = float(capture.get(cv2.CAP_PROP_FPS))
    frame_count = int(capture.get(cv2.CAP_PROP_FRAME_COUNT))
    source_width = int(capture.get(cv2.CAP_PROP_FRAME_WIDTH))
    source_height = int(capture.get(cv2.CAP_PROP_FRAME_HEIGHT))
    capture.release()
    if fps <= 0:
        raise RuntimeError("video reports an invalid frame rate")

    reference = build_temporal_median(
        video, frame_count, analysis_width, roi, max_reference_samples
    )
    metrics: list[FrameMetric] = []
    capture = _open_video(video)
    frame_index = 0
    while True:
        ok, source = capture.read()
        if not ok:
            break
        frame = _crop(_resize(source, analysis_width), roi)
        values = measure_frame(frame, reference, threshold)
        metrics.append(
            FrameMetric(frame_index, frame_index / fps, *values)
        )
        frame_index += 1
    capture.release()

    metadata: dict[str, float | int] = {
        "fps": fps,
        "decoded_frames": len(metrics),
        "reported_frames": frame_count,
        "duration_seconds": len(metrics) / fps,
        "source_width": source_width,
        "source_height": source_height,
        "analysis_width": analysis_width,
        "threshold": threshold,
    }
    return metadata, reference, metrics


def group_events(metrics: Iterable[FrameMetric], minimum_component: float) -> list[list[FrameMetric]]:
    selected = [m for m in metrics if m.largest_component_fraction >= minimum_component]
    events: list[list[FrameMetric]] = []
    for metric in selected:
        if not events or metric.frame > events[-1][-1].frame + 1:
            events.append([metric])
        else:
            events[-1].append(metric)
    return events


def _read_frames(source: Path, indices: set[int]) -> dict[int, np.ndarray]:
    result: dict[int, np.ndarray] = {}
    if source.is_dir():
        paths = _series_paths(source)
        for index in sorted(indices):
            if index >= len(paths):
                continue
            frame = cv2.imread(str(paths[index]), cv2.IMREAD_COLOR)
            if frame is not None:
                result[index] = frame
        return result

    capture = _open_video(source)
    frame_index = 0
    while indices:
        ok, frame = capture.read()
        if not ok:
            break
        if frame_index in indices:
            result[frame_index] = frame
            indices.remove(frame_index)
        frame_index += 1
    capture.release()
    return result


def write_event_images(
    source: Path,
    output_dir: Path,
    metrics: list[FrameMetric],
    event_limit: int,
) -> list[dict[str, object]]:
    top = sorted(metrics, key=lambda item: item.largest_component_fraction, reverse=True)[
        :event_limit
    ]
    requested: set[int] = set()
    decoded_count = len(metrics)
    for metric in top:
        requested.update(
            index for index in (metric.frame - 1, metric.frame, metric.frame + 1)
            if 0 <= index < decoded_count
        )
    frames = _read_frames(source, set(requested))
    output_dir.mkdir(parents=True, exist_ok=True)

    records: list[dict[str, object]] = []
    sheet_tiles: list[np.ndarray] = []
    for rank, metric in enumerate(top, start=1):
        record: dict[str, object] = {"rank": rank, **asdict(metric), "images": []}
        for index in (metric.frame - 1, metric.frame, metric.frame + 1):
            frame = frames.get(index)
            if frame is None:
                continue
            name = f"event-{rank:02d}-frame-{index:06d}.jpg"
            cv2.imwrite(str(output_dir / name), frame, [cv2.IMWRITE_JPEG_QUALITY, 94])
            record["images"].append(name)
            tile = _resize(frame, 640)
            label = (
                f"rank {rank} frame {index}  "
                f"peak={metric.largest_component_fraction:.2%}"
            )
            cv2.putText(tile, label, (12, 28), cv2.FONT_HERSHEY_SIMPLEX, 0.65,
                        (255, 255, 255), 2, cv2.LINE_AA)
            sheet_tiles.append(tile)
        records.append(record)

    if sheet_tiles:
        width = max(tile.shape[1] for tile in sheet_tiles)
        height = max(tile.shape[0] for tile in sheet_tiles)
        columns = 3
        rows = (len(sheet_tiles) + columns - 1) // columns
        sheet = np.zeros((rows * height, columns * width, 3), dtype=np.uint8)
        for index, tile in enumerate(sheet_tiles):
            y, x = divmod(index, columns)
            sheet[y * height:y * height + tile.shape[0],
                  x * width:x * width + tile.shape[1]] = tile
        cv2.imwrite(str(output_dir / "contact-sheet.jpg"), sheet,
                    [cv2.IMWRITE_JPEG_QUALITY, 92])
    return records


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="video file or ordered image directory")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--analysis-width", type=int, default=800)
    parser.add_argument("--roi", type=_parse_roi, default=_parse_roi("0.07,0.12,0.93,0.93"))
    parser.add_argument("--threshold", type=int, default=42)
    parser.add_argument("--minimum-component", type=float, default=0.015)
    parser.add_argument("--max-reference-samples", type=int, default=91)
    parser.add_argument("--event-limit", type=int, default=8)
    parser.add_argument(
        "--series-fps",
        type=float,
        default=1.0,
        help="capture rate used for an image-directory source",
    )
    parser.add_argument(
        "--fail-on-events",
        action="store_true",
        help="return exit status 1 when at least one event crosses the threshold",
    )
    args = parser.parse_args()

    if not args.source.exists() or not (args.source.is_file() or args.source.is_dir()):
        parser.error(f"source does not exist: {args.source}")
    if args.analysis_width < 64 or not 1 <= args.threshold <= 255:
        parser.error("analysis-width must be >= 64 and threshold must be 1..255")
    if args.series_fps <= 0:
        parser.error("series-fps must be positive")

    output_dir = args.output_dir or args.source.with_name(f"{args.source.stem}-flicker")
    if args.source.is_dir():
        metadata, reference, metrics = analyze_series(
            args.source,
            args.analysis_width,
            args.roi,
            args.threshold,
            args.max_reference_samples,
            args.series_fps,
        )
    else:
        metadata, reference, metrics = analyze_video(
            args.source,
            args.analysis_width,
            args.roi,
            args.threshold,
            args.max_reference_samples,
        )
    output_dir.mkdir(parents=True, exist_ok=True)
    cv2.imwrite(str(output_dir / "temporal-median.jpg"), reference,
                [cv2.IMWRITE_JPEG_QUALITY, 94])

    grouped = group_events(metrics, args.minimum_component)
    event_images = write_event_images(args.source, output_dir, metrics, args.event_limit)
    report = {
        "source": str(args.source.resolve()),
        "metadata": metadata,
        "roi": args.roi,
        "minimum_component": args.minimum_component,
        "event_group_count": len(grouped),
        "event_frame_count": sum(len(event) for event in grouped),
        "event_ranges": [
            {
                "first_frame": event[0].frame,
                "last_frame": event[-1].frame,
                "first_seconds": event[0].seconds,
                "last_seconds": event[-1].seconds,
                "peak_component_fraction": max(
                    metric.largest_component_fraction for metric in event
                ),
            }
            for event in grouped
        ],
        "top_frames": event_images,
        "frames": [asdict(metric) for metric in metrics],
    }
    (output_dir / "report.json").write_text(
        json.dumps(report, indent=2), encoding="utf-8"
    )
    summary = {
        "source": str(args.source),
        "frames": len(metrics),
        "fps": metadata["fps"],
        "events": len(grouped),
        "event_frames": report["event_frame_count"],
        "max_component_fraction": max(
            (metric.largest_component_fraction for metric in metrics), default=0.0
        ),
        "output_dir": str(output_dir),
    }
    print(json.dumps(summary, indent=2))
    return 1 if args.fail_on_events and grouped else 0


if __name__ == "__main__":
    raise SystemExit(main())
