#!/usr/bin/env python3
"""Validate that device PMU events distinguish known positive controls."""

from __future__ import annotations

import argparse
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from proof_contract import ProofRejected, parse_simpleperf_csv  # noqa: E402


EVENTS = (
    "instructions:u",
    "cpu-cycles:u",
    "raw-l1i-cache-refill:u",
    "raw-br-mis-pred-retired:u",
    "raw-dmb-spec:u",
)


def read_pair(directory: pathlib.Path, name: str) -> dict:
    return parse_simpleperf_csv(
        (directory / f"{name}.csv").read_text(encoding="utf-8"),
        (directory / f"{name}.err").read_text(encoding="utf-8"),
        EVENTS,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=pathlib.Path)
    parser.add_argument("--iterations", type=int, required=True)
    parser.add_argument("--require-dmb", action="store_true")
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    try:
        control = read_pair(args.directory, "control")["events"]
        dmb = read_pair(args.directory, "dmb")["events"]
        branch = read_pair(args.directory, "branch")["events"]
        icache = read_pair(args.directory, "icache")["events"]
        dmb_delta = dmb["raw-dmb-spec:u"] - control["raw-dmb-spec:u"]
        dmb_validated = dmb_delta >= args.iterations * 0.90
        if args.require_dmb and not dmb_validated:
            raise ProofRejected(
                "dmb-pmu-negative-control-failed",
                detail=f"delta={dmb_delta} expected>={args.iterations * 0.90:.0f}",
            )
        branch_delta = (
            branch["raw-br-mis-pred-retired:u"]
            - control["raw-br-mis-pred-retired:u"]
        )
        if branch_delta <= max(10_000, args.iterations // 20):
            raise ProofRejected("branch-pmu-negative-control-failed")
        l1i_delta = (
            icache["raw-l1i-cache-refill:u"]
            - control["raw-l1i-cache-refill:u"]
        )
        if l1i_delta <= max(10_000, args.iterations // 100):
            raise ProofRejected("l1i-pmu-negative-control-failed")
    except (OSError, ProofRejected) as exc:
        print(json.dumps({"accepted": False, "reason": str(exc)}))
        return 2
    payload = {
        "accepted": True,
        "schema": "lsx4-pmu-negative-control/v1",
        "iterations": args.iterations,
        "dmbDelta": dmb_delta,
        "dmbPmuValidated": dmb_validated,
        "dmbMetricRequiredFallback": None if dmb_validated else "unavailable",
        "dmbUnavailableReason": (
            None
            if dmb_validated
            else "raw-dmb-spec failed the device positive control and the adapter has no generic executed-DMB observer"
        ),
        "branchMissDelta": branch_delta,
        "l1iRefillDelta": l1i_delta,
        "branchMissControl": control["raw-br-mis-pred-retired:u"],
        "branchMissPositive": branch["raw-br-mis-pred-retired:u"],
        "l1iValidated": True,
        "branchValidated": True,
        "dmbMetricSource": (
            "pmu-raw-dmb-spec" if dmb_validated else "unavailable"
        ),
    }
    encoded = json.dumps(payload, indent=2, sort_keys=True)
    if args.output:
        args.output.write_text(encoded + "\n", encoding="utf-8")
    print(encoded)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
