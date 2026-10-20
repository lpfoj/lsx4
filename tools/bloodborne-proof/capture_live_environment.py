#!/usr/bin/env python3
"""Capture one timestamped, read-only Vivo environment snapshot."""

from __future__ import annotations

import argparse
import datetime as dt
import importlib.util
import json
import pathlib
import re
import sys
from typing import Any


MODULE_PATH = pathlib.Path(__file__).with_name("capture_provenance.py")
SPEC = importlib.util.spec_from_file_location("bloodborne_live_provenance", MODULE_PATH)
proof = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = proof
SPEC.loader.exec_module(proof)


STATUS_NAMES = {
    0: "NONE", 1: "LIGHT", 2: "MODERATE", 3: "SEVERE",
    4: "CRITICAL", 5: "EMERGENCY", 6: "SHUTDOWN",
}


def parse_temperatures(text: str) -> dict[str, Any]:
    proof.reject_if("Current temperatures from HAL:" not in text,
                    "current-hal-temperatures-missing")
    section = text.split("Current temperatures from HAL:", 1)[1]
    section = section.split("Current cooling devices from HAL:", 1)[0]
    entries = []
    pattern = re.compile(
        r"Temperature\{mValue=(?P<value>-?[0-9.]+), mType=(?P<type>\d+), "
        r"mName=(?P<name>[^,}]+), mStatus=(?P<status>\d+)\}"
    )
    for match in pattern.finditer(section):
        status = int(match.group("status"))
        sensor_type = int(match.group("type"))
        entries.append({
            "name": match.group("name"),
            "type": sensor_type,
            "value": float(match.group("value")),


            "unit": "celsius" if sensor_type in {0, 1, 2, 3, 4, 5, 9}
                    else "hal-native",
            "status": status,
            "statusName": STATUS_NAMES.get(status, "UNKNOWN"),
        })
    proof.reject_if(not entries, "current-hal-temperatures-empty")
    skin = [entry for entry in entries if entry["name"] == "skin"]
    cpu = [entry for entry in entries if entry["type"] == 0]
    gpu = [entry for entry in entries if entry["type"] == 1]
    proof.reject_if(len(skin) != 1 or not cpu or not gpu,
                    "required-current-temperature-missing")
    return {
        "skin": skin[0],
        "cpuMax": max(cpu, key=lambda entry: entry["value"]),
        "gpuMax": max(gpu, key=lambda entry: entry["value"]),
        "allCurrentHal": entries,
    }


def parse_cpu_frequencies(text: str) -> list[dict[str, Any]]:
    pattern = re.compile(
        r"(?m)^(?P<path>/\S+/policy(?P<policy>\d+)) affected_cpus=(?P<cpus>[^\n]+)\n"
        r"scaling_cur_freq=(?P<current>\d+)\n"
        r"scaling_min_freq=(?P<minimum>\d+)\n"
        r"scaling_max_freq=(?P<maximum>\d+)$"
    )
    values = [{
        "policy": int(match.group("policy")),
        "cpus": match.group("cpus").strip().split(),
        "currentKHz": int(match.group("current")),
        "minKHz": int(match.group("minimum")),
        "maxKHz": int(match.group("maximum")),
    } for match in pattern.finditer(text.strip())]
    proof.reject_if(not values, "cpu-frequency-parse-failed")
    return values


def exact_line(text: str, pattern: str, label: str) -> str:
    values = re.findall(pattern, text, flags=re.MULTILINE)
    proof.reject_if(len(set(values)) != 1, f"{label}-missing-or-ambiguous")
    return values[0]


def summarize_environment(environment: dict[str, Any]) -> dict[str, Any]:
    battery = environment["battery"]["text"]
    power = environment["power"]["text"]
    thermal_text = environment["thermal"]["text"]
    gpu_freq_text = environment["gpuFreq"]["text"].strip()
    gpu_status = exact_line(gpu_freq_text, r"\bstatus=(available|unavailable)\b",
                            "gpu-frequency-status")
    powered = {
        name: exact_line(
            battery, rf"^\s*{re.escape(label)} powered:\s*(true|false)\s*$",
            f"battery-{name}-powered",
        ) == "true"
        for name, label in (
            ("ac", "AC"), ("usb", "USB"), ("wireless", "Wireless"),
            ("dock", "Dock"),
        )
    }
    return {
        "cpuFrequencies": parse_cpu_frequencies(environment["cpuFreq"]["text"]),
        "gpuFrequency": {
            "status": gpu_status,
            "evidence": gpu_freq_text,
            "proxyUsed": False,
        },
        "thermal": {
            "globalStatus": int(exact_line(
                thermal_text, r"^Thermal Status:\s*(\d+)\s*$", "thermal-status"
            )),
            **parse_temperatures(thermal_text),
        },
        "battery": {
            "levelPercent": int(exact_line(
                battery, r"^\s*level:\s*(\d+)\s*$", "battery-level"
            )),
            "voltageMv": int(exact_line(
                battery, r"^\s*voltage:\s*(\d+)\s*$", "battery-voltage"
            )),
            "status": int(exact_line(
                battery, r"^\s*status:\s*(\d+)\s*$", "battery-status"
            )),
            "powered": powered,
            "externalPower": any(powered.values()),
        },
        "power": {
            "wakefulness": exact_line(
                power, r"^\s*mWakefulness=(\w+)\s*$", "wakefulness"
            ),
            "batterySaver": exact_line(
                power, r"^\s*Battery Saver is currently:\s*(ON|OFF)\s*$",
                "battery-saver",
            ),
        },
    }


def collect(adb: Any) -> dict[str, Any]:
    captured_utc_start = dt.datetime.now(dt.timezone.utc).isoformat()
    serial = adb.text("serial", ["shell", "getprop", "ro.serialno"]).strip()
    proof.reject_if(serial != "10AG263F1B001HM", "physical-serial-mismatch", serial)
    boot_id = adb.text("boot-id", ["shell", "cat", "/proc/sys/kernel/random/boot_id"]).strip()
    pid_values = adb.text("pidof-pre", ["shell", "pidof", "app.lsx4.android:game"]).split()
    proof.reject_if(len(pid_values) != 1 or not pid_values[0].isdigit(),
                    "game-pid-not-unique", repr(pid_values))
    game_pid = int(pid_values[0])
    stat_pre = adb.text("proc-stat-pre", [
        "exec-out", "run-as", "app.lsx4.android", "cat", f"/proc/{game_pid}/stat",
    ])
    parsed_pid, _comm, start_ticks = proof.parse_proc_stat(stat_pre)
    proof.reject_if(parsed_pid != game_pid, "proc-pid-mismatch")
    uptime_text = adb.text("device-uptime", ["shell", "cat", "/proc/uptime"]).strip()
    uptime_seconds = float(uptime_text.split()[0])
    clock_ticks = int(adb.text("clock-ticks", ["shell", "getconf", "CLK_TCK"]).strip())
    process_uptime = uptime_seconds - (start_ticks / clock_ticks)
    proof.reject_if(process_uptime <= 0, "process-uptime-invalid")

    environment = proof.capture_environment(adb, "snapshot")
    pid_post = adb.text("pidof-post", ["shell", "pidof", "app.lsx4.android:game"]).strip()
    stat_post = adb.text("proc-stat-post", [
        "exec-out", "run-as", "app.lsx4.android", "cat", f"/proc/{game_pid}/stat",
    ])
    _post_pid, _post_comm, post_start_ticks = proof.parse_proc_stat(stat_post)
    proof.reject_if(pid_post != str(game_pid) or post_start_ticks != start_ticks,
                    "process-identity-changed-during-snapshot")

    result = {
        "schema": "lsx4-bloodborne-live-environment/v1",
        "capturedUtcStart": captured_utc_start,
        "capturedUtcEnd": dt.datetime.now(dt.timezone.utc).isoformat(),
        "device": {"serial": serial, "bootId": boot_id},
        "process": {
            "pid": game_pid,
            "startTicks": start_ticks,
            "clockTicksPerSecond": clock_ticks,
            "deviceUptimeSeconds": uptime_seconds,
            "processUptimeSeconds": round(process_uptime, 2),
        },
        **summarize_environment(environment),
    }
    return result


def collect_readiness(adb: Any) -> dict[str, Any]:
    """Collect lightweight admission evidence without requiring or launching a game."""

    captured_utc_start = dt.datetime.now(dt.timezone.utc).isoformat()
    serial = adb.text("serial", ["shell", "getprop", "ro.serialno"]).strip()
    proof.reject_if(serial != "10AG263F1B001HM", "physical-serial-mismatch", serial)
    boot_id = adb.text("boot-id", [
        "shell", "cat", "/proc/sys/kernel/random/boot_id",
    ]).strip()
    device_uptime = float(adb.text(
        "device-uptime", ["shell", "cat", "/proc/uptime"],
    ).split()[0])
    game_pid_text = adb.text(
        "game-pid",
        proof.adb_remote_shell_command(
            ["shell"], "pidof app.lsx4.android:game 2>/dev/null || true"
        ),
        allow_empty=True,
    ).strip()
    proof.reject_if(game_pid_text and not re.fullmatch(r"\d+", game_pid_text),
                    "game-pid-ambiguous", game_pid_text)
    activity = adb.text("activity", ["shell", "dumpsys", "activity", "activities"])
    top_resumed = exact_line(
        activity, r"^\s*topResumedActivity=(ActivityRecord\{[^\n]+)\s*$",
        "top-resumed-activity",
    )
    environment = proof.capture_environment(adb, "readiness")
    return {
        "schema": "lsx4-bloodborne-vivo-readiness/v1",
        "capturedUtcStart": captured_utc_start,
        "capturedUtcEnd": dt.datetime.now(dt.timezone.utc).isoformat(),
        "device": {
            "serial": serial,
            "bootId": boot_id,
            "uptimeSeconds": device_uptime,
        },
        "foreground": {
            "topResumedActivity": top_resumed,
            "gameProcessPresent": bool(game_pid_text),
            "gamePid": int(game_pid_text) if game_pid_text else None,
        },
        **summarize_environment(environment),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--agentchat", required=True, type=pathlib.Path)
    parser.add_argument("--agent", default="codex")
    parser.add_argument("--room", default="увеличить fps")
    parser.add_argument("--readiness", action="store_true",
                        help="allow absent game PID and collect only admission evidence")
    args = parser.parse_args()
    store = proof.ArtifactStore(args.output)
    runner = proof.CommandRunner()
    gate = proof.AgentChatGate(
        runner, args.agentchat, agent=args.agent, room=args.room,
        additional_areas=("adb:10AG263F1B001HM",),
    )
    adb = proof.AdbReader(runner, gate, store, args.adb, args.serial)
    snapshot = collect_readiness(adb) if args.readiness else collect(adb)
    store.json("snapshot.json", snapshot)
    store.json("manifest.json", {
        "schema": "lsx4-bloodborne-artifact-manifest/v1",
        "artifacts": store.manifest(),
    })
    print(json.dumps({"accepted": True, "output": str(args.output), **snapshot},
                     ensure_ascii=False, sort_keys=True))
    store.seal()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
