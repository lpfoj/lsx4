#!/usr/bin/env bash

set -Eeuo pipefail

export MSYS2_ARG_CONV_EXCL='*'

readonly expected_device='10AG263F1B001HM'
readonly device="${ADB_SERIAL:-$expected_device}"
readonly package='app.lsx4.android'
readonly process_name='app.lsx4.android:game'
readonly agentchat='D:/LProj/PetProj/agentchat/agentchat.py'
readonly room='увеличить fps'
readonly target='device:vivo'
readonly title="${1:-}"
readonly warning_wait="${2:-55}"
readonly settle_wait="${3:-25}"
readonly second_settle_wait="${4:-20}"
readonly second_button="${5:-cross}"
readonly keep_running="${BB_KEEP_GAME_RUNNING:-1}"
readonly root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly performance_preflight="$root/scripts/preflight-vivo-performance-test.ps1"
readonly game="/data/user/0/$package/files/lsx4-home/runtime-fs/user/app/$title/eboot.bin"
readonly run_id="${title}-$(date -u +%Y%m%dT%H%M%SZ)-p$BASHPID"
readonly output="$root/.codex-fex-compat/$run_id"
readonly output_windows="$(cygpath -w "$output")"

action_sequence=0

fail() {
    printf 'FEX_TITLE_SMOKE_ERROR %s\n' "$*" >&2
    exit 1
}

guard_device_action() {
    action_sequence=$((action_sequence + 1))
    python3 "$agentchat" event --agent codex --room "$room" \
        --action adb --target "$target" \
        --summary "FEX title smoke $title device action $action_sequence" >/dev/null
    python3 "$agentchat" own --agent codex --room "$room" --area "$target" >/dev/null
}

adb_device() {
    guard_device_action
    adb -s "$device" "$@"
}

run_performance_preflight() {
    local preflight_path="$performance_preflight"
    command -v pwsh >/dev/null 2>&1 || \
        fail 'reason=performance-preflight-pwsh-missing'
    [[ -f "$performance_preflight" ]] || \
        fail "reason=performance-preflight-script-missing path=$performance_preflight"
    if command -v cygpath >/dev/null 2>&1; then
        preflight_path=$(cygpath -w "$performance_preflight") || \
            fail 'reason=performance-preflight-path-conversion-failed'
    fi
    pwsh -NoProfile -NonInteractive -File "$preflight_path" \
        -DeviceId "$expected_device" -AdbTarget "$device" || \
        fail 'reason=performance-preflight-rejected-run'
}

if [[ ! "$title" =~ ^CUSA[0-9]{5}$ ]]; then
    fail "reason=invalid-title value=$title"
fi
for timing in "$warning_wait" "$settle_wait" "$second_settle_wait"; do
    [[ "$timing" =~ ^[0-9]+$ ]] || fail "reason=invalid-wait value=$timing"
    (( timing <= 60 )) || fail "reason=wait-too-long value=$timing maximum=60"
done
[[ "$keep_running" == 0 || "$keep_running" == 1 ]] || \
    fail "reason=invalid-keep-running value=$keep_running"
[[ "$second_button" == cross || "$second_button" == circle ]] || \
    fail "reason=invalid-second-button value=$second_button expected=cross-or-circle"
if [[ "$device" != "$expected_device" &&
      ! "$device" =~ ^adb-10AG263F1B001HM-[A-Za-z0-9]+([[:space:]][(][0-9]+[)])?[.]_adb-tls-connect[.]_tcp$ ]]; then
    fail "reason=wrong-device actual=$device"
fi

mkdir -p "$root/.codex-fex-compat"
mkdir "$output" || fail "reason=run-directory-collision path=$output"

reported_serial=$(adb_device shell getprop ro.serialno 2>/dev/null | tr -d '\r')
[[ "$reported_serial" == "$expected_device" ]] || \
    fail "reason=device-identity-mismatch actual=$reported_serial"
adb_device shell "run-as $package test -f 'files/lsx4-home/runtime-fs/user/app/$title/eboot.bin'" \
    >/dev/null || fail "reason=game-missing path=$game"
preferences=$(adb_device exec-out run-as "$package" cat shared_prefs/lsx4_settings.xml \
    2>/dev/null || true)
grep -Eq 'name="managed_guest_cpu_backend"[[:space:]]+value="1"' <<< "$preferences" || \
    fail 'reason=fex-not-selected expected=managed_guest_cpu_backend:1'

adb_device shell am force-stop "$package" >/dev/null
run_performance_preflight
adb_device shell "run-as $package rm -f \
    files/guest-cpu-backend-status.json \
    files/lsx4-home/translator/last-fex-entry-run.json \
    files/lsx4-home/translator/last-launch-stage.json" >/dev/null
adb_device logcat -b all -c >/dev/null
adb_device shell "am start -n $package/.MainActivity \
    --ez autoload_existing_runtime true --ez autoinit_runtime true \
    --es scan_game_path $game --es launch_game_path $game \
    --es game_platform ps4 --ez fullscreen_render true \
    --ez embedded_aarch64_jit_backend true --ei render_resolution_mode 4" >/dev/null
printf 'FEX_TITLE_SMOKE_LAUNCHED title=%s wait=%s secondButton=%s\n' \
    "$title" "$warning_wait" "$second_button"

sleep 2
# Shader-heavy titles can wrap Android's log buffers long before the final
# sample. Preserve activation/lifecycle evidence while it is still present.
adb_device logcat -b all -d -v threadtime > "$output/logcat-boot.txt"
sleep "$warning_wait"
adb_device exec-out screencap -p > "$output/before-input.png"
adb_device shell 'input swipe 2903 1231 2903 1231 900' >/dev/null
sleep "$settle_wait"
adb_device exec-out screencap -p > "$output/after-first-cross.png"
case "$second_button" in
    cross)
        adb_device shell 'input swipe 2903 1231 2903 1231 300' >/dev/null
        ;;
    circle)
        # CUSA00368's first-boot city-skyline intro ignores Cross/Options.
        # Circle is the title's native cutscene-skip control.  A long hold
        # remains one edge-triggered press while spanning several guest polls.
        adb_device shell 'input swipe 3050 1095 3050 1095 900' >/dev/null
        ;;
esac
sleep "$second_settle_wait"
adb_device exec-out screencap -p > "$output/final.png"

pid=$(adb_device shell "pidof '$process_name' 2>/dev/null || true" \
    2>/dev/null | tr -d '\r[:space:]')
adb_device logcat -b all -d -v threadtime > "$output/logcat-runtime.txt"
{
    cat "$output/logcat-boot.txt"
    cat "$output/logcat-runtime.txt"
} > "$output/logcat.txt"
for artifact in guest-cpu-backend-status.json \
                lsx4-home/translator/last-fex-entry-run.json \
                lsx4-home/translator/last-launch-stage.json; do
    leaf="${artifact##*/}"
    adb_device exec-out run-as "$package" cat "files/$artifact" \
        > "$output/$leaf" 2>/dev/null || :
done

python3 - "$title" "$pid" "$output_windows" "$game" "$second_button" <<'PY'
import json
import math
import pathlib
import re
import struct
import sys
import zlib

title, pid_text, output_text, game_path, second_button = sys.argv[1:]
output = pathlib.Path(output_text)
log = (output / "logcat.txt").read_text(encoding="utf-8", errors="replace")
runtime_log = (output / "logcat-runtime.txt").read_text(
    encoding="utf-8", errors="replace"
)


def read_json(path):
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return {}
    return value if isinstance(value, dict) else {}


def png_metrics(path):
    data = path.read_bytes()
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        return {"valid": False, "reason": "not-png"}
    offset, chunks = 8, []
    width = height = color = None
    while offset + 12 <= len(data):
        length = struct.unpack(">I", data[offset:offset + 4])[0]
        kind = data[offset + 4:offset + 8]
        payload = data[offset + 8:offset + 8 + length]
        offset += 12 + length
        if kind == b"IHDR":
            width, height, depth, color, compression, filtering, interlace = struct.unpack(">IIBBBBB", payload)
            if depth != 8 or color not in (2, 6) or compression or filtering or interlace:
                return {"valid": False, "reason": "unsupported-png"}
        elif kind == b"IDAT":
            chunks.append(payload)
        elif kind == b"IEND":
            break
    if width is None or not chunks:
        return {"valid": False, "reason": "incomplete-png"}
    channels = 3 if color == 2 else 4
    stride = width * channels
    raw = zlib.decompress(b"".join(chunks))
    previous = bytearray(stride)
    cursor = total = total_sq = samples = 0
    step = max(1, width * height // 100000)
    linear = 0
    for _ in range(height):
        filter_type = raw[cursor]
        cursor += 1
        encoded = raw[cursor:cursor + stride]
        cursor += stride
        row = bytearray(stride)
        for index, value in enumerate(encoded):
            left = row[index - channels] if index >= channels else 0
            up = previous[index]
            up_left = previous[index - channels] if index >= channels else 0
            if filter_type == 0:
                predictor = 0
            elif filter_type == 1:
                predictor = left
            elif filter_type == 2:
                predictor = up
            elif filter_type == 3:
                predictor = (left + up) // 2
            elif filter_type == 4:
                probe = left + up - up_left
                distances = (abs(probe - left), abs(probe - up), abs(probe - up_left))
                predictor = (left, up, up_left)[distances.index(min(distances))]
            else:
                return {"valid": False, "reason": f"filter-{filter_type}"}
            row[index] = (value + predictor) & 0xFF
        for pixel in range(0, stride, channels):
            if linear % step == 0:
                red, green, blue = row[pixel:pixel + 3]
                luma = (54 * red + 183 * green + 19 * blue) / 256.0
                total += luma
                total_sq += luma * luma
                samples += 1
            linear += 1
        previous = row
    mean = total / samples
    stddev = math.sqrt(max(0.0, total_sq / samples - mean * mean))
    return {
        "valid": True,
        "width": width,
        "height": height,
        "meanLuma": mean,
        "stddevLuma": stddev,
        "nonBlank": stddev >= 5.0 and mean < 245.0,
    }


fatal_pattern = re.compile(
    r"FATAL EXCEPTION|Fatal signal|signal [0-9]+ \(SIG(?:BUS|SEGV|ILL)\)|"
    r"am_crash|am_anr|ANR in|VK_ERROR_DEVICE_LOST|fex_activation_failed|"
    r"fex-activation-(?:failed|stranded)|fex_mapped_entry_failed|"
    r"fex_hle_prewarm_failed|fex_hle_abi_rejected|"
    r"EXECUTOR_FEX_ENTRY_RUN[^\n]*call=-|"
    r"EXECUTOR_FEX_signal[^\n]*delivery rejected|"
    r"EXECUTOR_FEX_HLE_MANIFEST_LATE_REJECT|"
    r"EXECUTOR_FEX_HLE_CANONICAL_MISS|"
    r"EXECUTOR_HLE_DYNAMIC_TRAMPOLINES[^\n]*result=-|"
    r"EXECUTOR_MODULE_START_FEX[^\n]*result=(?:bind_failed|failed)",
    re.IGNORECASE,
)
live_samples = re.findall(r"\[EXECUTOR_LIVE_FPS\].*", log)
runtime_live_samples = re.findall(r"\[EXECUTOR_LIVE_FPS\].*", runtime_log)
draw_positive = any(re.search(r"(?:draw|builderTotal)=([1-9][0-9]*)", line) for line in live_samples)
present_positive = any(re.search(r"(?:presentReqRate|submitDoneRate)=([1-9][0-9]*|0[.][0-9]*[1-9][0-9]*)", line) for line in live_samples)
metric_live_samples = []
for line in runtime_live_samples:
    frames = re.search(r"\bframes=([0-9]+)", line)
    builder_total = re.search(r"\bbuilderTotal=([0-9]+)", line)
    submit_done_rate = re.search(r"\bsubmitDoneRate=([0-9]+(?:[.][0-9]+)?)", line)
    if frames and builder_total and submit_done_rate:
        metric_live_samples.append(
            {
                "frames": int(frames.group(1)),
                "builderTotal": int(builder_total.group(1)),
                "submitDoneRate": float(submit_done_rate.group(1)),
            }
        )
recent_live_samples = metric_live_samples[-3:]
recent_live_progress = (
    len(recent_live_samples) == 3
    and all(sample["submitDoneRate"] > 0.0 for sample in recent_live_samples)
    and all(
        newer["frames"] > older["frames"]
        and newer["builderTotal"] > older["builderTotal"]
        for older, newer in zip(recent_live_samples, recent_live_samples[1:])
    )
)
backend_status = read_json(output / "guest-cpu-backend-status.json")
backend_status_fex = backend_status.get("effective") == "fex-experimental"
backend_log_fex = bool(
    re.search(r"EXECUTOR_GUEST_CPU_BACKEND.*effective=(?:1|fex-experimental)", log)
)
backend_fex = backend_status_fex or backend_log_fex
launch_stage = read_json(output / "last-launch-stage.json")
normalized_launch_path = str(launch_stage.get("path", "")).replace("\\", "/").rstrip("/")
normalized_game_path = game_path.replace("\\", "/").rstrip("/")
launch_stage_title_bound = normalized_launch_path == normalized_game_path
launch_stage_name = launch_stage.get("stage")
# BuildModuleStatus replaces the in-memory status but deliberately leaves this
# last durable launch checkpoint at build_module_status_begin. A live renderer
# beyond it is positive progress, not a load hang.
launch_stage_ready = (
    launch_stage_title_bound and launch_stage_name == "build_module_status_begin"
)
activation_committed = "[EXECUTOR_FEX_ACTIVATION] stage=commit result=ok" in log
fatal = fatal_pattern.search(log)
screens = {
    name: png_metrics(output / name)
    for name in ("before-input.png", "after-first-cross.png", "final.png")
}
final_nonblank = screens["final.png"].get("nonBlank") is True
process_alive = bool(pid_text)
renderer_active = bool(live_samples) and (draw_positive or present_positive)
activation_inferred = (
    launch_stage_ready
    and backend_status_fex
    and process_alive
    and recent_live_progress
)
activation_evidence = activation_committed or activation_inferred
launchable = (
    process_alive
    and backend_fex
    and activation_evidence
    and not fatal
    and renderer_active
    and final_nonblank
)
summary = {
    "schema": "lsx4-fex-title-smoke/v1",
    "titleId": title,
    "secondButton": second_button,
    "pid": int(pid_text) if pid_text else None,
    "processAlive": process_alive,
    "backendFex": backend_fex,
    "backendStatusFex": backend_status_fex,
    "activationCommitted": activation_committed,
    "activationInferred": activation_inferred,
    "activationEvidence": activation_evidence,
    "activationEvidenceSource": (
        "log-marker" if activation_committed else "durable-stage-and-live-progress"
        if activation_inferred else "none"
    ),
    "launchStage": launch_stage_name,
    "launchStageTitleBound": launch_stage_title_bound,
    "launchStageReady": launch_stage_ready,
    "fatalDetected": bool(fatal),
    "fatalLine": fatal.group(0) if fatal else None,
    "liveFpsSamples": len(live_samples),
    "drawObserved": draw_positive,
    "presentObserved": present_positive,
    "rendererActive": renderer_active,
    "metricLiveFpsSamples": len(metric_live_samples),
    "recentLiveProgress": recent_live_progress,
    "recentLiveSamples": recent_live_samples,
    "screenshots": screens,
    "classification": "launchable-needs-gameplay-route" if launchable else "fex-launch-failed-or-incomplete",
    "playableClaim": False,
}
(output / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
print(
    "FEX_TITLE_SMOKE_RESULT "
    f"title={title} class={summary['classification']} pid={pid_text or 'none'} "
    f"fex={int(backend_fex)} activation={int(activation_evidence)} "
    f"activationSource={summary['activationEvidenceSource']} fatal={int(bool(fatal))} "
    f"liveSamples={len(live_samples)} draw={int(draw_positive)} present={int(present_positive)} "
    f"finalNonBlank={int(final_nonblank)} evidence={output / 'summary.json'}"
)
PY

if [[ "$keep_running" == 0 ]]; then
    adb_device shell am force-stop "$package" >/dev/null
fi
