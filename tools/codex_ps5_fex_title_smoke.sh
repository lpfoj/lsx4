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
readonly boot_wait="${2:-12}"
readonly runtime_wait="${3:-20}"
readonly keep_running="${PS5_KEEP_GAME_RUNNING:-1}"
readonly root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly performance_preflight="$root/scripts/preflight-vivo-performance-test.ps1"
readonly run_id="${title}-$(date -u +%Y%m%dT%H%M%SZ)-ps5-fex-p$BASHPID"
readonly output="$root/.codex-fex-compat/$run_id"
readonly output_windows="$(cygpath -w "$output")"

action_sequence=0

fail() {
    printf 'PS5_FEX_TITLE_SMOKE_ERROR %s\n' "$*" >&2
    exit 1
}

guard_device_action() {
    action_sequence=$((action_sequence + 1))
    python3 "$agentchat" event --agent codex --room "$room" \
        --action adb --target "$target" \
        --summary "PS5 FEX title smoke $title device action $action_sequence" >/dev/null
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

[[ "$title" =~ ^PPSA[0-9]{5}$ ]] || fail "reason=invalid-title value=$title"
for timing in "$boot_wait" "$runtime_wait"; do
    [[ "$timing" =~ ^[0-9]+$ ]] || fail "reason=invalid-wait value=$timing"
    (( timing <= 60 )) || fail "reason=wait-too-long value=$timing maximum=60"
done
[[ "$keep_running" == 0 || "$keep_running" == 1 ]] || \
    fail "reason=invalid-keep-running value=$keep_running"
if [[ "$device" != "$expected_device" &&
      ! "$device" =~ ^adb-10AG263F1B001HM-[A-Za-z0-9]+[.]_adb-tls-connect[.]_tcp$ ]]; then
    fail "reason=wrong-device actual=$device"
fi

mkdir -p "$root/.codex-fex-compat"
mkdir "$output" || fail "reason=run-directory-collision path=$output"

reported_serial=$(adb_device shell getprop ro.serialno 2>/dev/null | tr -d '\r')
[[ "$reported_serial" == "$expected_device" ]] || \
    fail "reason=device-identity-mismatch actual=$reported_serial"

game_relative=''
for candidate in \
    "files/lsx4-home/games/$title" \
    "files/lsx4-home/runtime-fs/user/app/$title"; do
    if adb_device shell "run-as $package test -f '$candidate/eboot.bin' -a -f '$candidate/sce_sys/param.json'" \
            >/dev/null 2>&1; then
        game_relative="$candidate"
        break
    fi
done
[[ -n "$game_relative" ]] || fail "reason=game-missing title=$title"
readonly game="/data/user/0/$package/$game_relative"

preferences=$(adb_device exec-out run-as "$package" cat shared_prefs/lsx4_settings.xml \
    2>/dev/null || true)
grep -Eq 'name="managed_guest_cpu_backend"[[:space:]]+value="1"' <<< "$preferences" || \
    fail 'reason=fex-not-selected expected=managed_guest_cpu_backend:1'

adb_device shell am force-stop "$package" >/dev/null
run_performance_preflight
adb_device shell "run-as $package rm -f files/guest-cpu-backend-status.json" >/dev/null
adb_device logcat -b all -c >/dev/null
adb_device shell "am start -n $package/.MainActivity \
    --ez autoload_existing_runtime true --ez autoinit_runtime true \
    --es scan_game_path $game --es launch_game_path $game \
    --es game_platform ps5 --ez fullscreen_render true \
    --ez embedded_aarch64_jit_backend true --ei render_resolution_mode 4" >/dev/null
printf 'PS5_FEX_TITLE_SMOKE_LAUNCHED title=%s root=%s bootWait=%s runtimeWait=%s\n' \
    "$title" "$game" "$boot_wait" "$runtime_wait"

sleep 3
adb_device logcat -b all -d -v threadtime > "$output/logcat-boot.txt"
sleep "$boot_wait"
adb_device exec-out screencap -p > "$output/boot.png"
# Match the existing landscape controller's Cross location without depending on
# key injection support in the title.
adb_device shell 'input swipe 2903 1231 2903 1231 300' >/dev/null
sleep "$runtime_wait"
adb_device exec-out screencap -p > "$output/final.png"

pid=$(adb_device shell "pidof '$process_name' 2>/dev/null || true" \
    2>/dev/null | tr -d '\r[:space:]')
adb_device logcat -b all -d -v threadtime > "$output/logcat-runtime.txt"
{
    cat "$output/logcat-boot.txt"
    cat "$output/logcat-runtime.txt"
} > "$output/logcat.txt"
for artifact in guest-cpu-backend-status.json lsx4-home/ui-smoke.log; do
    leaf="${artifact##*/}"
    adb_device exec-out run-as "$package" cat "files/$artifact" \
        > "$output/$leaf" 2>/dev/null || :
done

python3 - "$title" "$pid" "$output_windows" "$game" <<'PY'
import json
import pathlib
import re
import sys

title, pid_text, output_text, game_path = sys.argv[1:]
output = pathlib.Path(output_text)
log = (output / "logcat.txt").read_text(encoding="utf-8", errors="replace")

try:
    backend = json.loads(
        (output / "guest-cpu-backend-status.json").read_text(encoding="utf-8")
    )
except (OSError, UnicodeError, json.JSONDecodeError):
    backend = {}

fatal_pattern = re.compile(
    r"FATAL EXCEPTION|Fatal signal|signal [0-9]+ \(SIG(?:BUS|SEGV|ILL)\)|"
    r"am_crash|am_anr|ANR in|VK_ERROR_DEVICE_LOST|"
    r"EXECUTOR_GUEST_CPU_BACKEND_BLOCKED|Aborting automation: runtime failed to load|"
    r"LSX4-PS5-FEX[^\n]*fatal|PS5_FEX_FATAL|"
    r"PS5 FEX guest execution failed|fex_activation_failed|"
    r"fex-activation-(?:failed|stranded)|fex_mapped_entry_failed|"
    r"fex_hle_(?:prewarm_failed|abi_rejected)",
    re.IGNORECASE,
)
fatal = fatal_pattern.search(log)
backend_fex = (
    backend.get("platform") == "ps5"
    and backend.get("effective") == "fex-experimental"
    and backend.get("supported") is not False
)
backend_log_fex = bool(
    re.search(
        r"EXECUTOR_GUEST_CPU_BACKEND[^\n]*effective=(?:FEX_EXPERIMENTAL|1)"
        r"[^\n]*\"platform\":\"ps5\"",
        log,
        re.IGNORECASE,
    )
)
fex_active = backend_fex or backend_log_fex
launch_attempted = bool(
    re.search(r"PS5 runtime status before launch:|PS5 launch result:|LSX4-PS5", log)
)
adapter_active = bool(
    re.search(r"LSX4-PS5-FEX|EXECUTOR_FEX_ACTIVATION|fex-2608|fex2608", log, re.I)
)
process_alive = bool(pid_text)
screens = {}
for name in ("boot.png", "final.png"):
    path = output / name
    data = path.read_bytes() if path.is_file() else b""
    screens[name] = {
        "validPng": data.startswith(b"\x89PNG\r\n\x1a\n"),
        "bytes": len(data),
    }
evidence_ok = (
    process_alive
    and fex_active
    and launch_attempted
    and adapter_active
    and fatal is None
    and screens["final.png"]["validPng"]
)
summary = {
    "schema": "lsx4-ps5-fex-title-smoke/v1",
    "titleId": title,
    "gamePath": game_path,
    "pid": int(pid_text) if pid_text else None,
    "processAlive": process_alive,
    "backendFex": fex_active,
    "backendStatus": backend,
    "launchAttempted": launch_attempted,
    "adapterActive": adapter_active,
    "fatalDetected": fatal is not None,
    "fatalLine": fatal.group(0) if fatal else None,
    "screenshots": screens,
    "classification": (
        "ps5-fex-active-needs-gameplay-route"
        if evidence_ok
        else "ps5-fex-launch-failed-or-incomplete"
    ),
    "playableClaim": False,
}
(output / "summary.json").write_text(
    json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
)
print(
    "PS5_FEX_TITLE_SMOKE_RESULT "
    f"title={title} class={summary['classification']} pid={pid_text or 'none'} "
    f"fex={int(fex_active)} launch={int(launch_attempted)} "
    f"adapter={int(adapter_active)} fatal={int(fatal is not None)} "
    f"evidence={output / 'summary.json'}"
)
PY

if [[ "$keep_running" == 0 ]]; then
    adb_device shell am force-stop "$package" >/dev/null
fi
