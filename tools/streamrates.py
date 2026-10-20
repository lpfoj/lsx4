#!/usr/bin/env python3
"""Turn the status stream into per-frame numbers.

The snapshots are cumulative totals, so a single one cannot say what a frame
costs - and totals invite the mistake of dividing a whole session's count by one
window's instructions. Differences between consecutive snapshots do not have that
problem: they are what happened during those two seconds and nothing else.

Only intervals where the counters actually move are used, so the app sitting at a
menu or force-stopped does not dilute the rates.

usage: streamrates.py <stream.txt> [fps]
"""

import json
import sys

SNAPSHOT_INTERVAL_S = 2.0


def snapshots(path):
    raw = open(path, encoding="utf-8", errors="replace").read()
    depth, start = 0, None
    for i, ch in enumerate(raw):
        if ch == "{":
            if depth == 0:
                start = i
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0 and start is not None:
                try:
                    yield json.loads(raw[start:i + 1])
                except ValueError:
                    pass
                start = None


def flatten(o, prefix=""):
    out = {}
    if isinstance(o, dict):
        for k, v in o.items():
            out.update(flatten(v, prefix + "/" + k))
    elif isinstance(o, bool):
        pass
    elif isinstance(o, int):
        out[prefix] = o
    return out


def main():
    path = sys.argv[1]
    fps = float(sys.argv[2]) if len(sys.argv) > 2 else None
    flat = [flatten(s) for s in snapshots(path)]
    if len(flat) < 2:
        print("нужно минимум два снимка")
        return 1

    key_entries = "/runtimeBlocks/tieredJit/entries"
    live = [i for i in range(1, len(flat))
            if flat[i].get(key_entries, 0) > flat[i - 1].get(key_entries, 0)]
    if not live:
        print("движения нет: за весь поток счётчик входов в блоки не вырос,")
        print("значит игра не исполняла гостевой код. Считать нечего.")
        return 1

    span = len(live) * SNAPSHOT_INTERVAL_S
    delta = {}
    for i in live:
        for k, v in flat[i].items():
            prev = flat[i - 1].get(k)
            if prev is not None and v >= prev:
                delta[k] = delta.get(k, 0) + (v - prev)

    print("живых интервалов %d, суммарно %.0f с" % (len(live), span))
    if fps:
        print("кадров за это время примерно %.0f" % (fps * span))
    print()

    interesting = [k for k, v in delta.items() if v > 0]
    interesting.sort(key=lambda k: -delta[k])
    width = max(len(k) for k in interesting[:30])
    header = "%-*s %>14s %>14s" .replace(">", "") % (width, "счётчик", "за секунду", "за кадр")
    print(header)
    for k in interesting[:30]:
        per_s = delta[k] / span
        per_frame = per_s / fps if fps else 0
        print("%-*s %14s %14s" % (width, k, format(int(per_s), ","),
                                  format(int(per_frame), ",") if fps else "-"))


    if fps:
        entries_per_frame = delta.get(key_entries, 0) / span / fps
        print()
        print("входов в блоки на кадр: %s" % format(int(entries_per_frame), ","))
        print("при 975 млн инструкций на кадр это %.1f инструкций на вход," %
              (975e6 / entries_per_frame if entries_per_frame else 0))
        print("то есть столько стоит средний блок целиком - вход, тело и выход.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
