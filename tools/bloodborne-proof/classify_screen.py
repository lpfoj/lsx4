#!/usr/bin/env python3
"""Classify the small set of Bloodborne startup screens needed by the route.

The classifier deliberately uses structural brightness only.  It does not ship
or compare any game image/template and it writes no output image.  Android's
temporary screencap is deleted by the caller after each classification.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import cv2
import numpy as np


def _separated_peaks(values: np.ndarray, floor: float, distance: int) -> list[int]:
    peaks: list[int] = []
    for index in np.argsort(values)[::-1]:
        value = float(values[index])
        if value < floor:
            break
        candidate = int(index)
        if all(abs(candidate - prior) >= distance for prior in peaks):
            peaks.append(candidate)
    return sorted(peaks)


def classify(path: Path) -> dict[str, object]:
    image = cv2.imread(str(path), cv2.IMREAD_COLOR)
    if image is None or image.ndim != 3:
        raise ValueError(f"unreadable PNG: {path}")

    height, width = image.shape[:2]
    gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)




    logo = gray[
        int(height * 0.27) : int(height * 0.54),
        int(width * 0.22) : int(width * 0.78),
    ]
    logo_bright_fraction = float(np.mean(logo > 150))
    title_visible = logo_bright_fraction >= 0.05

    center = gray[
        int(height * 0.20) : int(height * 0.80),
        int(width * 0.25) : int(width * 0.75),
    ]
    center_bright_fraction = float(np.mean(center > 100))




    x0, x1 = int(width * 0.455), int(width * 0.545)
    y0, y1 = int(height * 0.58), int(height * 0.82)
    menu_bgr = image[y0:y1, x0:x1]
    menu_gray = gray[y0:y1, x0:x1]
    channel_max = menu_bgr.max(axis=2)
    channel_min = menu_bgr.min(axis=2)
    text_mask = ((menu_gray > 130) & ((channel_max - channel_min) < 80)).astype(
        np.float32
    )
    projection = np.convolve(text_mask.sum(axis=1), np.ones(9) / 9, mode="same")
    peaks = _separated_peaks(projection, floor=20.0, distance=max(24, height // 48))

    if title_visible and len(peaks) >= 4:
        state = "continue-menu"
    elif title_visible and len(peaks) >= 2:
        state = "online-menu"
    elif title_visible:
        state = "title-transition"
    elif center_bright_fraction >= 0.004:



        state = "startup-clickable"
    else:
        state = "dark-transition"

    return {
        "schema": "lsx4.bloodborne-screen-classifier/v1",
        "state": state,
        "width": width,
        "height": height,
        "logoBrightFraction": round(logo_bright_fraction, 6),
        "centerBrightFraction": round(center_bright_fraction, 6),
        "menuTextRows": len(peaks),
        "menuRowY": [int(y0 + peak) for peak in peaks],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    result = classify(args.image)
    if args.json:
        print(json.dumps(result, separators=(",", ":")))
    else:
        print(result["state"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
