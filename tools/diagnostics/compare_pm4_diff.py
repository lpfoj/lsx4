#!/usr/bin/env python3
"""Find the first semantic divergence between two LSX4 one-frame PM4 traces."""

from __future__ import annotations

import argparse
import dataclasses
import difflib
import pathlib
import shlex
import sys
from collections import Counter


@dataclasses.dataclass(frozen=True)
class Record:
    kind: str
    fields: dict[str, str]
    line: int
    raw: str


def parse(path: pathlib.Path) -> list[Record]:
    records: list[Record] = []
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not raw.strip():
            continue
        words = shlex.split(raw, posix=True)
        kind = words[0]
        fields: dict[str, str] = {}
        for word in words[1:]:
            if "=" in word:
                key, value = word.split("=", 1)
                fields[key] = value
        records.append(Record(kind, fields, number, raw))
    if not records or records[0].kind != "H":
        raise ValueError(f"{path}: missing lsx4.pm4-diff header")
    if records[0].fields.get("schema") != "lsx4.pm4-diff.v1":
        raise ValueError(f"{path}: unsupported schema {records[0].fields.get('schema')!r}")
    return records


def values(record: Record, keys: tuple[str, ...]) -> tuple[str, ...]:
    return tuple(record.fields.get(key, "-") for key in keys)


ROOT_KEYS = ("dcbDw", "rawDcb", "ownedDcb", "dcbMatch", "ccbDw", "rawCcb",
             "ownedCcb", "ccbMatch", "physicalAliasing")
IB_KEYS = ("phase", "engine", "depth", "dw", "hash", "gpuModified")
PACKET_KEYS = ("engine", "depth", "type", "opcode", "dw", "hash", "prefix")
DRAW_KEYS = ("depth", "opcode", "prefix", "shaders", "user", "attachments", "draw",
             "regs")
CE_KEYS = ("offset", "bytes", "hash")


def stream(records: list[Record], kind: str, keys: tuple[str, ...]) -> list[tuple[str, ...]]:
    return [values(record, keys) for record in records if record.kind == kind]


def record_stream(records: list[Record], kind: str) -> list[Record]:
    return [record for record in records if record.kind == kind]


def first_mismatch(left: list[tuple[str, ...]], right: list[tuple[str, ...]]) -> int | None:
    common = min(len(left), len(right))
    for index in range(common):
        if left[index] != right[index]:
            return index
    return common if len(left) != len(right) else None


def describe(record: Record | None, keys: tuple[str, ...]) -> str:
    if record is None:
        return "<end-of-stream>"
    selected = " ".join(f"{key}={record.fields.get(key, '-')}" for key in keys)
    return f"line {record.line}: {selected}"


def relative_epoch(records: list[Record], field: str) -> list[int]:
    result: list[int] = []
    base: int | None = None
    for record in records:
        if field not in record.fields:
            continue
        current = int(record.fields[field], 0)
        if base is None:
            base = current
        result.append(current - base)
    return result


def report_stream(label: str, left_records: list[Record], right_records: list[Record],
                  kind: str, keys: tuple[str, ...]) -> bool:
    left = stream(left_records, kind, keys)
    right = stream(right_records, kind, keys)
    mismatch = first_mismatch(left, right)
    print(f"{label}: left={len(left)} right={len(right)}", end="")
    if mismatch is None:
        print(" exact")
        return False
    print(f" first-divergence={mismatch + 1}")
    left_source = record_stream(left_records, kind)
    right_source = record_stream(right_records, kind)
    lrec = left_source[mismatch] if mismatch < len(left_source) else None
    rrec = right_source[mismatch] if mismatch < len(right_source) else None
    print(f"  left  {describe(lrec, keys)}")
    print(f"  right {describe(rrec, keys)}")



    matcher = difflib.SequenceMatcher(a=left, b=right, autojunk=False)
    for tag, i1, i2, j1, j2 in matcher.get_opcodes():
        if tag != "equal" and (i1 <= mismatch < i2 or j1 <= mismatch < j2 or
                               (i1 == i2 == mismatch) or (j1 == j2 == mismatch)):
            print(f"  edit  {tag} left[{i1 + 1}:{i2}] right[{j1 + 1}:{j2}]")
            break
    return True


def report_root_internal_mismatch(label: str, records: list[Record]) -> bool:
    mismatch = False
    for ordinal, record in enumerate(record_stream(records, "R"), 1):
        dcb_bad = record.fields.get("dcbMatch") == "0"
        ccb_bad = record.fields.get("ccbMatch") == "0"
        if dcb_bad or ccb_bad:
            print(f"{label}: raw-vs-owned mismatch at root {ordinal} line {record.line} "
                  f"dcbMatch={record.fields.get('dcbMatch')} "
                  f"ccbMatch={record.fields.get('ccbMatch')}")
            mismatch = True
            break
    return mismatch


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare current-JIT and FEX one-frame pre-Vulkan PM4 traces")
    parser.add_argument("left", type=pathlib.Path)
    parser.add_argument("right", type=pathlib.Path)
    args = parser.parse_args()

    try:
        left = parse(args.left)
        right = parse(args.right)
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    print(f"left : {args.left} backend={left[0].fields.get('backend', '?')}")
    print(f"right: {args.right} backend={right[0].fields.get('backend', '?')}")
    print("counts left =", dict(Counter(record.kind for record in left)))
    print("counts right=", dict(Counter(record.kind for record in right)))

    left_internal = report_root_internal_mismatch("left", left)
    right_internal = report_root_internal_mismatch("right", right)
    root_diff = report_stream("root DCB/CCB", left, right, "R", ROOT_KEYS)
    ib_diff = report_stream("indirect buffers", left, right, "I", IB_KEYS)
    packet_diff = report_stream("PM4 packets", left, right, "P", PACKET_KEYS)
    ce_diff = report_stream("CE dumps", left, right, "C", CE_KEYS)
    draw_diff = report_stream("pre-Vulkan draws", left, right, "D", DRAW_KEYS)

    left_alias = relative_epoch(left, "aliasEpoch")
    right_alias = relative_epoch(right, "aliasEpoch")
    left_writes = relative_epoch(left, "writeEpoch")
    right_writes = relative_epoch(right, "writeEpoch")
    print(f"relative epochs: alias leftMax={max(left_alias, default=0)} "
          f"rightMax={max(right_alias, default=0)}; write leftMax={max(left_writes, default=0)} "
          f"rightMax={max(right_writes, default=0)}")

    if left_internal or right_internal:
        print("verdict: PM4 changed between guest submit pointer and owned snapshot")
    elif root_diff:
        print("verdict: divergence already exists in guest-submitted DCB/CCB (before parsing/Vulkan)")
    elif ib_diff:
        print("verdict: roots match; first divergence is an indirect-buffer snapshot/encounter")
    elif packet_diff:
        print("verdict: buffer snapshots match until a different PM4 packet/prefix")
    elif ce_diff:
        print("verdict: PM4 matches; CE-produced descriptor/SRT data diverges")
    elif draw_diff:
        print("verdict: PM4 matches; accumulated GNM register/SRT state diverges before Vulkan")
    else:
        print("verdict: pre-Vulkan PM4 and draw state are identical; investigate post-PM4 resource contents")
    return 1 if any((left_internal, right_internal, root_diff, ib_diff, packet_diff,
                     ce_diff, draw_diff)) else 0


if __name__ == "__main__":
    raise SystemExit(main())
