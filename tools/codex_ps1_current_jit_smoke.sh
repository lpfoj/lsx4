#!/usr/bin/env bash

set -Eeuo pipefail

export MSYS2_ARG_CONV_EXCL='*'

readonly expected_device='10AG263F1B001HM'
readonly device="${ADB_SERIAL:-$expected_device}"
readonly package='app.lsx4.android'
readonly process_name='app.lsx4.android:game'
readonly title='CUSA42759'
readonly agentchat='D:/LProj/PetProj/agentchat/agentchat.py'
readonly room='увеличить fps'
readonly target='device:vivo'
readonly root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly performance_preflight="$root/scripts/preflight-vivo-performance-test.ps1"
readonly app0="/data/user/0/$package/files/lsx4-home/runtime-fs/user/app/$title"
readonly game="$app0/eboot.bin"
readonly run_id="${title}-$(date -u +%Y%m%dT%H%M%SZ)-ps1-current-p$BASHPID"
readonly output="$root/.codex-fex-compat/$run_id"
readonly output_windows="$(cygpath -w "$output")"

action_sequence=0
backend_restore_required=0

fail() {
    printf 'PS1_CURRENT_SMOKE_ERROR %s\n' "$*" >&2
    exit 1
}

guard_device_action() {
    action_sequence=$((action_sequence + 1))
    python3 "$agentchat" event --agent codex --room "$room" \
        --action adb --target "$target" \
        --summary "PS1 current-JIT smoke $title device action $action_sequence" >/dev/null
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

restore_fex_backend() {
    local status=$?
    trap - EXIT
    set +e
    if (( backend_restore_required )); then
        adb_device shell am force-stop "$package" >/dev/null
        adb_device shell "run-as $package sed -i \
            '/name=\"managed_guest_cpu_backend\"/ s/value=\"0\"/value=\"1\"/' \
            shared_prefs/lsx4_settings.xml" >/dev/null
        local restored
        restored=$(adb_device exec-out run-as "$package" cat \
            shared_prefs/lsx4_settings.xml 2>/dev/null || true)
        if grep -Eq 'name="managed_guest_cpu_backend"[[:space:]]+value="1"' \
                <<< "$restored"; then
            printf 'PS1_CURRENT_SMOKE_BACKEND_RESTORED value=1\n'
        else
            printf 'PS1_CURRENT_SMOKE_ERROR reason=backend-restore-failed\n' >&2
            status=1
        fi
    fi
    exit "$status"
}
trap restore_fex_backend EXIT

if [[ "$device" != "$expected_device" &&
      ! "$device" =~ ^adb-10AG263F1B001HM-[A-Za-z0-9]+[.]_adb-tls-connect[.]_tcp$ ]]; then
    fail "reason=wrong-device actual=$device"
fi

mkdir -p "$root/.codex-fex-compat"
mkdir "$output" || fail "reason=run-directory-collision path=$output"

reported_serial=$(adb_device shell getprop ro.serialno 2>/dev/null | tr -d '\r')
[[ "$reported_serial" == "$expected_device" ]] ||
    fail "reason=device-identity-mismatch actual=$reported_serial"

for required in \
    eboot.bin \
    conf/PS1HD/package-ps4.conf \
    data \
    assets/PS1HD/bios \
    assets/common/PS1FullScreen.png; do
    adb_device shell "run-as $package test -e 'files/lsx4-home/runtime-fs/user/app/$title/$required'" \
        >/dev/null || fail "reason=game-component-missing path=$app0/$required"
done

preferences=$(adb_device exec-out run-as "$package" cat \
    shared_prefs/lsx4_settings.xml 2>/dev/null || true)
grep -Eq 'name="managed_guest_cpu_backend"[[:space:]]+value="1"' <<< "$preferences" ||
    fail 'reason=fex-not-selected-before-test expected=managed_guest_cpu_backend:1'

adb_device shell am force-stop "$package" >/dev/null
run_performance_preflight
backend_restore_required=1
adb_device shell "run-as $package sed -i \
    '/name=\"managed_guest_cpu_backend\"/ s/value=\"1\"/value=\"0\"/' \
    shared_prefs/lsx4_settings.xml" >/dev/null
preferences=$(adb_device exec-out run-as "$package" cat \
    shared_prefs/lsx4_settings.xml 2>/dev/null || true)
grep -Eq 'name="managed_guest_cpu_backend"[[:space:]]+value="0"' <<< "$preferences" ||
    fail 'reason=current-jit-selection-failed expected=managed_guest_cpu_backend:0'

adb_device shell "run-as $package rm -f files/guest-cpu-backend-status.json" >/dev/null
adb_device logcat -b all -c >/dev/null
adb_device shell "am start -n $package/.MainActivity \
    --ez autoload_existing_runtime true --ez autoinit_runtime true \
    --es scan_game_path $game --es launch_game_path $game \
    --es game_platform ps4 --ez fullscreen_render true \
    --ez embedded_aarch64_jit_backend true --ei render_resolution_mode 4" >/dev/null
printf 'PS1_CURRENT_SMOKE_LAUNCHED title=%s backend=current-aarch64-jit\n' "$title"

sleep 12
adb_device exec-out screencap -p > "$output/boot.png"
# Advance the native PS1 title far enough to prove that input and emulation
# continue together rather than merely presenting a static boot frame.
for _ in 1 2 3; do
    adb_device shell 'input swipe 2903 1231 2903 1231 120' >/dev/null
    sleep 1
done
sleep 9
adb_device exec-out screencap -p > "$output/final.png"

pid=$(adb_device shell "pidof '$process_name' 2>/dev/null || true" \
    2>/dev/null | tr -d '\r[:space:]')
adb_device logcat -b all -d -v threadtime > "$output/logcat.txt"
adb_device exec-out run-as "$package" cat files/guest-cpu-backend-status.json \
    > "$output/guest-cpu-backend-status.json" 2>/dev/null || :

python3 - "$pid" "$output_windows" <<'PY'
import json
import pathlib
import re
import statistics
import sys

pid_text, output_text = sys.argv[1:]
output = pathlib.Path(output_text)
log = (output / "logcat.txt").read_text(encoding="utf-8", errors="replace")
try:
    backend = json.loads(
        (output / "guest-cpu-backend-status.json").read_text(encoding="utf-8")
    )
except (OSError, UnicodeError, json.JSONDecodeError):
    backend = {}

samples = [
    (float(fps), float(core_ms), float(budget_ms), int(presents))
    for fps, core_ms, budget_ms, presents in re.findall(
        r"\[PS1_DIRECT_METRIC\] fps=([0-9.]+) coreMs=([0-9.]+) "
        r"budgetMs=([0-9.]+) presents=([0-9]+)",
        log,
    )
]
fatal = next(
    (
        line
        for line in log.splitlines()
        if "FATAL EXCEPTION" in line or "Fatal signal" in line
    ),
    None,
)
requested = backend.get("requested")
effective = backend.get("effective")
launch = "[PS1_DIRECT] launch app0=" in log
core = "[PS1_DIRECT] core=SwanStation" in log
jit_result = "AArch64 JIT launch result: 0" in log
audio = "[PS1_DIRECT] AAudio start result=0" in log
fps_median = statistics.median(sample[0] for sample in samples) if samples else 0.0
core_median = statistics.median(sample[1] for sample in samples) if samples else 0.0
budget_median = statistics.median(sample[2] for sample in samples) if samples else 0.0
present_delta = samples[-1][3] - samples[0][3] if len(samples) >= 2 else 0
pngs_valid = all(
    path.read_bytes().startswith(b"\x89PNG\r\n\x1a\n")
    for path in (output / "boot.png", output / "final.png")
)
playable = all(
    (
        bool(pid_text),
        fatal is None,
        requested == "current-aarch64-jit",
        effective == "current-aarch64-jit",
        launch,
        core,
        jit_result,
        audio,
        len(samples) >= 4,
        fps_median >= 58.0,
        present_delta >= 180,
        core_median < budget_median,
        pngs_valid,
    )
)
summary = {
    "schema": "lsx4-ps1-current-jit-smoke/v1",
    "titleId": "CUSA42759",
    "pid": int(pid_text) if pid_text.isdigit() else None,
    "processAlive": bool(pid_text),
    "requestedBackend": requested,
    "effectiveBackend": effective,
    "directLaunch": launch,
    "coreSwanStation": core,
    "jitLaunchResultOk": jit_result,
    "audioStarted": audio,
    "fatalDetected": fatal is not None,
    "fatalLine": fatal,
    "metricSamples": len(samples),
    "medianFps": fps_median,
    "medianCoreMs": core_median,
    "medianBudgetMs": budget_median,
    "presentDelta": present_delta,
    "screenshotsValid": pngs_valid,
    "classification": "playable" if playable else "regression",
}
(output / "summary.json").write_text(
    json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
)
print(
    "PS1_CURRENT_SMOKE_RESULT "
    f"class={summary['classification']} pid={summary['pid']} "
    f"samples={len(samples)} medianFps={fps_median:.2f} "
    f"presentDelta={present_delta} fatal={int(fatal is not None)} "
    f"evidence={output}"
)
if not playable:
    raise SystemExit(1)
PY
