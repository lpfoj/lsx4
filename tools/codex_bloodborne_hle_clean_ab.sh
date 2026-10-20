#!/usr/bin/env bash

set -Eeuo pipefail

export MSYS2_ARG_CONV_EXCL='*'

readonly root_shell_pid="$BASHPID"

readonly expected_device='10AG263F1B001HM'
readonly device="${ADB_SERIAL:-$expected_device}"
readonly package='app.lsx4.android'
readonly game_process='app.lsx4.android:game'
readonly game='/data/user/0/app.lsx4.android/files/lsx4-home/runtime-fs/user/app/CUSA03173/eboot.bin'
readonly runtime_home='files/lsx4-home'
readonly hle_clean_marker="$runtime_home/run-enable-hle-clean-buffer-fastpath"
readonly hle_clean_stats_marker="$runtime_home/run-diagnostic-hle-clean-buffer-stats"
readonly async_shader_disable_marker="$runtime_home/run-disable-async-shader-retranslation"
readonly agentchat_py='D:/LProj/PetProj/agentchat/agentchat.py'
readonly room='увеличить fps'
readonly window="${1:-30}"
readonly requested_tag="${2:-hle-clean-buffer-aba}"
readonly script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly repo_root="$(cd "$script_dir/.." && pwd)"
readonly workspace_root="$(cd "$repo_root/.." && pwd)"
readonly pmu_driver="$script_dir/codex_bloodborne_pmu.sh"
readonly performance_preflight="$repo_root/scripts/preflight-vivo-performance-test.ps1"
readonly warmup_seconds="${BB_HLE_CLEAN_AB_WARMUP_WAIT:-20}"
readonly save_seed_discovery_only="${BB_SAVE_SEED_DISCOVERY_ONLY:-0}"
readonly minimum_wait_reduction_pct="${BB_HLE_MIN_WAIT_REDUCTION_PCT:-10}"
readonly minimum_sync_coverage='0.50'
readonly process_identity_timeout="${BB_PROCESS_IDENTITY_TIMEOUT:-360}"
readonly save_space_multiplier=8
readonly save_space_margin_kib=65536
readonly save_root="$runtime_home/native/savedata"
readonly save_parent="$save_root/1000/CUSA00207"
readonly save_leaf='SPRJ0005'
readonly save_path="$save_parent/$save_leaf"

run_id=''
tag=''
run_dir=''
windows_file=''
save_evidence_dir=''
device_save_state_dir=''
device_save_archive=''
device_save_archive_hash=''

baseline_a1_pid=''
clean_b_pid=''
baseline_a2_pid=''
final_handoff_pid=''
final_handoff_starttime=''
last_pid=''
last_process_identity=''
active_mode='unknown'
device_session_started=0
cleanup_complete=0
cleanup_running=0
run_dir_sealed=0
save_seed_prepared=0
save_restore_sequence=0
process_observer_pid=''
process_observer_raw=''
process_observer_status=125
declare -A window_open=()
declare -A window_mode=()
declare -A window_artifact_sha=()
declare -A phase_pid=()
declare -A phase_starttime=()

readonly experiment_markers=(
    "$hle_clean_marker"
    "$hle_clean_stats_marker"
    "$runtime_home/run-enable-fex-buffer-capture-ack"
    "$runtime_home/run-enable-fex-deferred-write-epoch"
    "$runtime_home/run-enable-bounded-guest-invalidation-drain"
    "$runtime_home/run-force-fex-software-tso"
    "$runtime_home/run-enable-fex-multiblock"
    "$runtime_home/run-enable-fex-relaxed-vector-tso-ab"
    "$runtime_home/run-enable-fex-relaxed-memcpyset-tso-ab"
    "$runtime_home/run-enable-fex-low-memory-cache"
    "$runtime_home/run-profile-guest-invalidation-sources"
    "$runtime_home/run-profile-guest-invalidation-overlap"
    "$runtime_home/run-disable-adpf-40hz-target"
    "$runtime_home/run-use-vivo-moderate-cpu-floor"
    "$runtime_home/run-use-vivo-performance-hint-4241"
    "$runtime_home/run-disable-vivo-moderate-cpu-floor"
    "$runtime_home/diag-jit-helper-perf-map"
    "$runtime_home/run-enable-stable-canonical-streaming"
    "$runtime_home/run-disable-render-effects-bundle"
    "$runtime_home/run-force-internal-resolution-640x360"
    "$runtime_home/run-disable-vk-robustness"
    "$runtime_home/run-force-rectlist-first-vertex-zero"
    "$runtime_home/run-disable-audio-output"
    "$runtime_home/run-force-interpret-audio"
    "$runtime_home/run-force-interpret-audio-thread2"
    "$runtime_home/run-force-interpret-all"
    "$runtime_home/run-disable-mspace-free-reuse"
    "$runtime_home/run-enable-memset-vtable-guard"
    "$runtime_home/run-disable-imt-fix"
    'files/run-enable-icall-bind-detour'
    'files/run-enable-icall-slot-fill'
    'files/run-enable-legacy-patches'
)

readonly graphics_baseline_markers=(
    "$runtime_home/run-disable-dynamic-shadows"
    "$runtime_home/run-disable-ssao"
    "$runtime_home/run-disable-motion-blur"
    "$runtime_home/run-disable-dof"
    "$runtime_home/run-disable-anti-aliasing"
    "$runtime_home/run-disable-chromatic-aberration"
)

fail() {
    echo "HLE_CLEAN_AB_ERROR $*" >&2
    return 1
}

guard_device_action() {
    local action="${1:-adb}"
    python3 "$agentchat_py" event --agent codex --room "$room" \
        --action "$action" --target 'device:vivo' \
        --summary "Bloodborne HLE clean buffer A/B/A: $action" >/dev/null || return 1
    python3 "$agentchat_py" own --agent codex --room "$room" \
        --area 'device:vivo' >/dev/null
}

posix_shell_quote() {
    local value="${1-}"
    printf "'"
    while [[ "$value" == *"'"* ]]; do
        printf "%s'\"'\"'" "${value%%\'*}"
        value="${value#*\'}"
    done
    printf "%s'" "$value"
}

adb_device() {
    local subcommand="${1:-unknown}"
    guard_device_action "adb-$subcommand" || return 1
    if [[ "$#" -ge 2 &&
          ( "$1" == shell || "$1" == exec-out ) &&
          "$2" == run-as && "${4:-}" == sh && "${5:-}" == -c ]]; then
        if [[ "$#" -ne 6 || -z "${3:-}" ]]; then
            echo "ADB_REMOTE_SH_C_INVALID transport=$1 argc=$#" >&2
            return 64
        fi
        local transport="$1" quoted_package quoted_script
        quoted_package=$(posix_shell_quote "$3") || return 64
        quoted_script=$(posix_shell_quote "$6") || return 64
        set -- "$transport" "run-as $quoted_package sh -c $quoted_script"
    fi
    adb -s "$device" "$@"
}

adb_device_background() {
    local action="$1"
    shift
    guard_device_action "$action" || return 1
    adb -s "$device" "$@" &
    process_observer_pid=$!
}

run_performance_preflight() {
    local preflight_path="$performance_preflight"
    command -v pwsh >/dev/null 2>&1 || {
        fail 'reason=performance-preflight-pwsh-missing'
        return 1
    }
    [[ -f "$performance_preflight" ]] || {
        fail "reason=performance-preflight-script-missing path=$performance_preflight"
        return 1
    }
    if command -v cygpath >/dev/null 2>&1; then
        preflight_path=$(cygpath -w "$performance_preflight") || return 1
    fi
    pwsh -NoProfile -NonInteractive -File "$preflight_path" \
        -DeviceId "$expected_device" -AdbTarget "$device"
}

verify_vivo_device_identity() {
    local reported_serial
    if ! reported_serial=$(adb_device shell getprop ro.serialno 2>/dev/null); then
        fail "reason=device-identity-query-failed adbTarget=$device"
        return 1
    fi
    reported_serial=${reported_serial//$'\r'/}
    if [[ "$reported_serial" != "$expected_device" ]]; then
        fail "reason=device-identity-mismatch adbTarget=$device expected=$expected_device actual=$reported_serial"
        return 1
    fi
    echo "VIVO_DEVICE_IDENTITY_OK adbTarget=$device ro.serialno=$reported_serial"
}

host_path() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -m "$1"
    else
        printf '%s\n' "$1"
    fi
}

initialize_run_directory() {
    local utc nonce
    utc=$(date -u +%Y%m%dT%H%M%SZ)
    nonce=$(printf '%05d%05d' "$RANDOM" "$RANDOM")
    run_id="${requested_tag}-${utc}-p$$-${nonce}"
    tag="$run_id"
    run_dir="$repo_root/.codex-bb-runs/$run_id"
    windows_file="$run_dir/windows.jsonl"
    save_evidence_dir="$run_dir/save-seed"
    device_save_state_dir="$save_root/.codex-hle-clean-aba/$run_id"
    device_save_archive="$device_save_state_dir/original.tar"
    device_save_archive_hash="$device_save_state_dir/original.tar.sha256"

    mkdir -p "$repo_root/.codex-bb-runs"
    if ! mkdir "$run_dir"; then
        fail "reason=run-namespace-collision path=$run_dir"
        return 1
    fi
    mkdir "$save_evidence_dir"
    python3 - "$(host_path "$windows_file")" \
        "$(host_path "$run_dir/save-restores.jsonl")" <<'PY'
import os
import pathlib
import sys

for value in sys.argv[1:]:
    path = pathlib.Path(value)
    fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    os.close(fd)
PY

    python3 - \
        "$(host_path "$run_dir/manifest.json")" \
        "$(host_path "$repo_root")" \
        "$(host_path "$workspace_root")" \
        "$(host_path "${BASH_SOURCE[0]}")" \
        "$(host_path "$pmu_driver")" \
        "$run_id" "$requested_tag" "$device" "$game" "$window" "$warmup_seconds" \
        "$save_path" "$device_save_state_dir" "$minimum_wait_reduction_pct" \
        "$minimum_sync_coverage" "$save_space_multiplier" \
        "$save_space_margin_kib" <<'PY'
import datetime
import hashlib
import json
import os
import pathlib
import subprocess
import sys

(
    manifest_path,
    repo_text,
    workspace_text,
    harness_text,
    pmu_text,
    run_id,
    requested_tag,
    device,
    game,
    window,
    warmup,
    save_path,
    device_save_state_dir,
    minimum_wait_reduction_pct,
    minimum_sync_coverage,
    save_space_multiplier,
    save_space_margin_kib,
) = sys.argv[1:]
repo = pathlib.Path(repo_text)
workspace = pathlib.Path(workspace_text)


def sha256(path):
    path = pathlib.Path(path)
    if not path.is_file():
        return None
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_capture(path):
    path = pathlib.Path(path)
    if not (path / ".git").exists():
        return {"path": str(path), "present": False}

    def run(*args):
        completed = subprocess.run(
            ["git", "-C", str(path), *args],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        return completed.returncode, completed.stdout

    head_rc, head = run("rev-parse", "HEAD")
    status_rc, status = run("status", "--porcelain=v1", "--untracked-files=all")
    diff_rc, diff = run("diff", "--binary", "HEAD", "--")
    cached_rc, cached = run("diff", "--binary", "--cached", "--")
    return {
        "path": str(path),
        "present": True,
        "head": head.decode("ascii", "replace").strip() if head_rc == 0 else None,
        "dirty": bool(status.strip()) if status_rc == 0 else None,
        "statusSha256": hashlib.sha256(status).hexdigest() if status_rc == 0 else None,
        "worktreeDiffSha256": hashlib.sha256(diff).hexdigest() if diff_rc == 0 else None,
        "indexDiffSha256": hashlib.sha256(cached).hexdigest() if cached_rc == 0 else None,
    }


artifact_paths = {
    "apkDebug": repo / "android-app/app/build/outputs/apk/debug/app-debug.apk",
    "executor": repo / "build/android-arm64-release/liblsx4_executor_android.so",
    "fexManifest": workspace / ".fex-2608-artifacts/android-api26-arm64/manifest.json",
    "harness": pathlib.Path(harness_text),
    "pmuDriver": pathlib.Path(pmu_text),
}
artifacts = {
    name: {"path": str(path.resolve()), "sha256": sha256(path), "present": path.is_file()}
    for name, path in artifact_paths.items()
}
fex_manifest_path = artifact_paths["fexManifest"]
if fex_manifest_path.is_file():
    try:
        fex = json.loads(fex_manifest_path.read_text(encoding="utf-8"))
        for role in ("adapter", "bridge", "fexcore"):
            entry = fex.get("artifacts", {}).get(role, {})
            path = pathlib.Path(entry.get("path", ""))
            artifacts[f"fex-{role}"] = {
                "path": str(path.resolve()) if str(path) else None,
                "manifestSha256": entry.get("sha256"),
                "sha256": sha256(path) if str(path) else None,
                "present": path.is_file() if str(path) else False,
            }
    except (OSError, ValueError, TypeError):
        artifacts["fexManifestParseError"] = True

environment_keys = (
    "BB_WARNING_WAIT",
    "BB_OFFLINE_WAIT",
    "BB_CONTINUE_WAIT",
    "BB_DOOR_WAIT",
    "BB_DOOR_SETTLE_WAIT",
    "BB_ROLE_PROBE_WAIT",
    "BB_LIVENESS_TAIL_SECONDS",
    "BB_SCENE_WORK_TOLERANCE_PCT",
    "BB_SCENE_DRAW_TOLERANCE_PCT",
    "BB_SCENE_SHADER_TOLERANCE_PCT",
)
manifest = {
    "schema": "lsx4-bloodborne-hle-clean-diagnostic-aba/v3",
    "runId": run_id,
    "requestedTag": requested_tag,
    "createdUtc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "hostDirectory": str(pathlib.Path(manifest_path).parent),
    "device": {"expectedSerial": device, "package": "app.lsx4.android"},
    "target": {"executable": game, "platform": "ps4"},
    "sequence": [
        {"phase": "baseline-a1", "hleClean": 0, "diagnosticStats": 1},
        {"phase": "clean-b", "hleClean": 1, "diagnosticStats": 1},
        {"phase": "baseline-a2", "hleClean": 0, "diagnosticStats": 1},
    ],
    "diagnosticOnly": True,
    "fpsVerdictAllowed": False,
    "productionHandoff": {
        "separateFreshProcess": True,
        "afterSaveOriginalRestore": True,
        "hleClean": 0,
        "asyncShaderRetranslation": "default-on",
        "graphicsAllSix": "off",
        "exactGameplayProofRequired": True,
        "causalWindow": False,
        "diagnosticStats": 0,
    },
    "causalInferenceBoundary": {
        "measuredConfiguration": "async-shader-retranslation-disabled",
        "productionConfiguration": "async-shader-retranslation-default-on",
        "productionTransferProven": False,
        "productionValidationRequired": True,
    },
    "processIdentityContract": {
        "source": "/proc/<pid>/stat field 22",
        "requiredPerPhase": True,
        "uniquePair": ["pid", "starttime"],
    },
    "constantSafetyControls": {
        "asyncShaderRetranslation": {
            "status": "approved-required-in-causal-phases",
            "runtimeMarker": "files/lsx4-home/run-disable-async-shader-retranslation",
            "nativeEnvironment": "EXECUTOR_DISABLE_ASYNC_SHADER_RETRANSLATION=1",
            "phaseProofRequired": True,
            "experimentalAxis": False,
            "productionHandoffState": "absent-default-on",
        }
    },
    "diagnosticInstrumentation": {
        "hleCleanBufferStats": {
            "runtimeMarker": "files/lsx4-home/run-diagnostic-hle-clean-buffer-stats",
            "constantInCausalPhases": True,
            "productionHandoffState": "absent",
            "fpsVerdictAllowed": False,
        }
    },
    "settings": {
        "measurementWindowSeconds": int(window),
        "preMeasurementWarmupSeconds": int(warmup),
        "strictPhaseGates": 1,
        "verifyCanonicalFex": 1,
        "profileInvalidationSources": 0,
        "profileInvalidationOverlap": 0,
        "perfMap": 0,
        "record": 0,
        "measurementInput": "forward",
        "graphicsOverride": 1,
        "graphicsAllSix": "off",
        "adpfTargetMs": 25,
        "vivoHint": 4241,
        "mechanismEvidenceAcceptance": {
            "minimumGuestInvalidationWaitReductionPct": float(minimum_wait_reduction_pct),
            "minimumCommittedSyncShare": float(minimum_sync_coverage),
            "baselineReference": "mean(baseline-a1,baseline-a2)",
            "fpsVerdictAllowed": False,
        },
        "environmentOverrides": {key: os.environ.get(key) for key in environment_keys},
    },
    "saveSeed": {
        "mode": "required",
        "runtimeMount": {"user": "1000", "title": "CUSA00207", "directory": "SPRJ0005"},
        "appPrivateRelativePath": save_path,
        "deviceEvidenceDirectory": device_save_state_dir,
        "forceStopAndPidVerificationRequired": True,
        "atomicSwap": True,
        "originalBackupRetained": True,
        "freeSpaceGate": {
            "sourceSizeCommand": "du -sk",
            "availableSpaceCommand": "df -k",
            "minimumMultiplier": int(save_space_multiplier),
            "fixedMarginKiB": int(save_space_margin_kib),
        },
    },
    "artifacts": artifacts,
    "repositories": {
        "lsx4": git_capture(repo),
        "funnel": git_capture(repo / "funnel-arm"),
    },
}
path = pathlib.Path(manifest_path)
with path.open("x", encoding="utf-8", newline="\n") as stream:
    json.dump(manifest, stream, ensure_ascii=False, indent=2, sort_keys=True)
    stream.write("\n")
    stream.flush()
    os.fsync(stream.fileno())
PY
    echo "RUN_NAMESPACE_CREATED id=$run_id dir=$run_dir"
    echo "HOST_MANIFEST_AVAILABLE path=$run_dir/manifest.json"
}

append_window_state() {
    local phase="$1" mode="$2" state="$3" reason="${4:-}" status="${5:-0}" line="${6:-0}" command="${7:-}"
    local artifact_path="${8:-}" artifact_sha="${9:-}"
    local process_pid="${10:-}" process_starttime="${11:-}"
    python3 - "$(host_path "$windows_file")" "$run_id" "$phase" "$mode" "$state" \
        "$reason" "$status" "$line" "$command" "$artifact_path" "$artifact_sha" \
        "$process_pid" "$process_starttime" <<'PY'
import datetime
import json
import os
import sys

(
    path, run_id, phase, mode, state, reason, status, line, command,
    artifact_path, artifact_sha, process_pid, process_starttime,
) = sys.argv[1:]
entry = {
    "schema": "lsx4-bloodborne-window/v1",
    "runId": run_id,
    "phase": phase,
    "hleClean": int(mode),
    "diagnosticOnly": True,
    "diagnosticStats": 1,
    "fpsVerdictAllowed": False,
    "state": state,
    "utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
}
if reason:
    entry["reason"] = reason
if status != "0":
    entry["exitStatus"] = int(status)
if line != "0":
    entry["line"] = int(line)
if command:
    entry["command"] = command
if artifact_path:
    entry["measurementArtifact"] = artifact_path
if artifact_sha:
    entry["measurementArtifactSha256"] = artifact_sha
if process_pid or process_starttime:
    if not process_pid.isdigit() or not process_starttime.isdigit():
        raise SystemExit("invalid process identity")
    entry["processIdentity"] = {"pid": int(process_pid), "starttime": int(process_starttime)}
payload = (json.dumps(entry, ensure_ascii=False, sort_keys=True) + "\n").encode("utf-8")
fd = os.open(path, os.O_WRONLY | os.O_APPEND)
try:
    remaining = memoryview(payload)
    while remaining:
        written = os.write(fd, remaining)
        if written <= 0:
            raise OSError("short append to windows.jsonl")
        remaining = remaining[written:]
    os.fsync(fd)
finally:
    os.close(fd)
PY
}

begin_window() {
    local phase="$1" mode="$2"
    if [[ "${window_open[$phase]:-}" == 1 ]]; then
        fail "phase=$phase reason=duplicate-window-start"
        return 1
    fi
    append_window_state "$phase" "$mode" started '' 0 0 '' "$phase/hle-clean-window.json"
    window_open["$phase"]=1
    window_mode["$phase"]="$mode"
}

terminal_window() {
    local phase="$1" state="$2" reason="${3:-}"
    [[ "${window_open[$phase]:-}" == 1 ]] || return 0
    append_window_state "$phase" "${window_mode[$phase]}" "$state" "$reason" 0 0 '' \
        "$phase/hle-clean-window.json" "${window_artifact_sha[$phase]:-}" \
        "${phase_pid[$phase]:-}" "${phase_starttime[$phase]:-}"
    window_open["$phase"]=0
}

reject_open_windows() {
    local reason="${1:-aborted}" status="${2:-0}" line="${3:-0}" command="${4:-}" phase
    for phase in baseline-a1 clean-b baseline-a2; do
        if [[ "${window_open[$phase]:-}" == 1 ]]; then
            if append_window_state "$phase" "${window_mode[$phase]}" rejected "$reason" \
                "$status" "$line" "$command" "$phase/hle-clean-window.json" \
                "${window_artifact_sha[$phase]:-}" "${phase_pid[$phase]:-}" \
                "${phase_starttime[$phase]:-}"; then
                window_open["$phase"]=0
            fi
        fi
    done
}

collect_phase_artifacts() {
    local phase="$1" source target prefix
    local phase_dir="$run_dir/$phase"
    prefix=".codex-bb-$(phase_tag "$phase")-"
    mkdir -p "$phase_dir"
    shopt -s nullglob
    local sources=("$repo_root/$prefix"*)
    shopt -u nullglob
    for source in "${sources[@]}"; do
        target="$phase_dir/${source##*/$prefix}"
        if [[ -e "$target" ]]; then
            fail "phase=$phase reason=immutable-artifact-collision target=$target"
            return 1
        fi
        mv -- "$source" "$target"
    done
}

validate_measurement_window_artifact() {
    local phase="$1" mode="$2" diagnostic_expected="${3:-1}" artifact sha
    artifact="$(phase_file "$phase" 'hle-clean-window.json')"
    if [[ ! -f "$artifact" ]]; then
        fail "phase=$phase reason=measurement-window-json-missing path=$artifact"
        return 1
    fi
    python3 - "$(host_path "$artifact")" "$mode" "$minimum_sync_coverage" \
        "$diagnostic_expected" <<'PY'
import json
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
expected_mode = int(sys.argv[2])
minimum_coverage = float(sys.argv[3])
diagnostic_expected = bool(int(sys.argv[4]))
value = json.loads(path.read_text(encoding="utf-8"))
if value.get("mode") != expected_mode:
    raise SystemExit(f"measurement mode mismatch: {value.get('mode')!r} != {expected_mode}")
if value.get("diagnosticOnly") is not diagnostic_expected:
    raise SystemExit("measurement diagnostic classification mismatch")
if diagnostic_expected and value.get("fpsVerdictAllowed") is not False:
    raise SystemExit("diagnostic measurement permits an FPS verdict")
if type(value.get("frames")) is not int or value["frames"] <= 0:
    raise SystemExit("measurement frame count is not positive")
guest = value.get("guestInvalidation")
if not isinstance(guest, dict) or guest.get("requests", 0) <= 0 or guest.get("waitMsPerFrame", -1) < 0:
    raise SystemExit("guest invalidation measurement is incomplete")
hle = value.get("hleClean")
if not isinstance(hle, dict):
    raise SystemExit("HLE clean measurement is missing")
if expected_mode == 0:
    if hle.get("snapshots") != 0 or hle.get("boundaryCommitted") != 0:
        raise SystemExit("baseline HLE clean counters are not absent")
else:
    if hle.get("coverageAccepted") is not True:
        raise SystemExit("clean-B coverage was not accepted")
    coverage = hle.get("committedSyncShare")
    declared = hle.get("minimumCommittedSyncShare")
    if not isinstance(coverage, (int, float)) or coverage < minimum_coverage:
        raise SystemExit(f"clean-B coverage below harness floor: {coverage!r}")
    if not isinstance(declared, (int, float)) or declared < minimum_coverage:
        raise SystemExit(f"PMU coverage threshold below harness floor: {declared!r}")
PY
    sha=$(sha256sum "$artifact" | awk '{print $1}') || return 1
    if [[ ! "$sha" =~ ^[0-9a-fA-F]{64}$ ]]; then
        fail "phase=$phase reason=measurement-window-json-hash-invalid value=$sha"
        return 1
    fi
    window_artifact_sha["$phase"]="$sha"
    echo "MEASUREMENT_WINDOW_PROVENANCE phase=$phase mode=$mode sha256=$sha artifact=$phase/hle-clean-window.json"
}

start_process_identity_observer() {
    local phase="$1"
    process_observer_raw="$run_dir/$phase/proc-stat.raw.tmp"
    process_observer_status=125
    if [[ -e "$process_observer_raw" ]]; then
        fail "phase=$phase reason=process-observer-artifact-collision path=$process_observer_raw"
        return 1
    fi
    adb_device_background "adb-proc-starttime-$phase" shell \
        "observer_attempt=0; while [ \"\$observer_attempt\" -lt '$process_identity_timeout' ]; do \
            observed_pid=\$(pidof '$game_process' 2>/dev/null || true); \
            observed_pid=\${observed_pid%% *}; \
            if [ -n \"\$observed_pid\" ] && \
               run-as '$package' cat \"/proc/\$observed_pid/stat\" 2>/dev/null; then \
                exit 0; \
            fi; \
            observer_attempt=\$((observer_attempt + 1)); sleep 1; \
        done; exit 124" > "$process_observer_raw" 2>/dev/null
    echo "PROCESS_IDENTITY_OBSERVER_STARTED phase=$phase hostPid=$process_observer_pid timeout=$process_identity_timeout"
}

stop_process_identity_observer() {
    if [[ -z "$process_observer_pid" ]]; then
        process_observer_status=125
        return 0
    fi
    if [[ -s "$process_observer_raw" ]]; then
        if wait "$process_observer_pid"; then
            process_observer_status=0
        else
            process_observer_status=$?
        fi
    elif kill -0 "$process_observer_pid" 2>/dev/null; then
        kill "$process_observer_pid" 2>/dev/null || true
        wait "$process_observer_pid" 2>/dev/null || true
        process_observer_status=124
    elif wait "$process_observer_pid"; then
        process_observer_status=0
    else
        process_observer_status=$?
    fi
    process_observer_pid=''
}

validate_process_identity() {
    local phase="$1" expected_pid="$2" raw final identity observed_pid starttime
    raw="$run_dir/$phase/proc-stat.raw.tmp"
    final="$run_dir/$phase/proc-stat.raw"
    if (( process_observer_status != 0 )) || [[ ! -s "$raw" || -e "$final" ]]; then
        fail "phase=$phase reason=process-starttime-observer-failed status=$process_observer_status raw=$raw"
        return 1
    fi
    tr -d '\r' < "$raw" > "$final"
    rm -f -- "$raw"
    identity=$(python3 - "$(host_path "$final")" \
        "$(host_path "$run_dir/$phase/process-identity.json")" "$expected_pid" <<'PY'
import json
import os
import pathlib
import sys

raw_path = pathlib.Path(sys.argv[1])
destination = pathlib.Path(sys.argv[2])
expected_pid = int(sys.argv[3])
raw = raw_path.read_text(encoding="utf-8").strip()
right = raw.rfind(")")
left = raw.find("(")
if left <= 0 or right <= left:
    raise SystemExit("malformed /proc/<pid>/stat")
pid = int(raw[:left].strip())
fields_after_comm = raw[right + 1 :].split()
# fields_after_comm[0] is proc field 3 (state); field 22 is therefore index 19.
if len(fields_after_comm) <= 19:
    raise SystemExit("/proc/<pid>/stat does not contain field 22")
starttime = int(fields_after_comm[19])
if pid != expected_pid or pid <= 0 or starttime <= 0:
    raise SystemExit(
        f"process identity mismatch expectedPid={expected_pid} observedPid={pid} starttime={starttime}"
    )
value = {
    "schema": "lsx4-process-identity/v1",
    "pid": pid,
    "procStatField22Starttime": starttime,
    "source": f"/proc/{pid}/stat",
}
with destination.open("x", encoding="utf-8", newline="\n") as stream:
    json.dump(value, stream, indent=2, sort_keys=True)
    stream.write("\n")
    stream.flush()
    os.fsync(stream.fileno())
print(pid, starttime)
PY
    ) || return 1
    read -r observed_pid starttime <<< "$identity"
    if [[ ! "$observed_pid" =~ ^[0-9]+$ || ! "$starttime" =~ ^[0-9]+$ ]]; then
        fail "phase=$phase reason=process-starttime-parse-invalid value=$identity"
        return 1
    fi
    phase_pid["$phase"]="$observed_pid"
    phase_starttime["$phase"]="$starttime"
    echo "PROCESS_IDENTITY_PROVEN phase=$phase pid=$observed_pid starttime=$starttime source=/proc/$observed_pid/stat:22"
}

seal_run_directory() {
    local status="${1:-1}"
    (( run_dir_sealed == 0 )) || return 0
    [[ -n "$run_dir" && -d "$run_dir" ]] || return 0
    reject_open_windows "process-exit-$status"
    python3 - "$(host_path "$run_dir")" "$status" <<'PY'
import datetime
import hashlib
import json
import os
import pathlib
import stat
import sys

root = pathlib.Path(sys.argv[1])
status = int(sys.argv[2])
result = root / "result.json"
with result.open("x", encoding="utf-8", newline="\n") as stream:
    json.dump(
        {
            "schema": "lsx4-bloodborne-hle-clean-aba-result/v1",
            "exitStatus": status,
            "sealedUtc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        },
        stream,
        indent=2,
        sort_keys=True,
    )
    stream.write("\n")
    stream.flush()
    os.fsync(stream.fileno())

index_path = root / "SHA256SUMS"
entries = []
for path in sorted(root.rglob("*")):
    if not path.is_file() or path == index_path:
        continue
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    entries.append(f"{digest}  {path.relative_to(root).as_posix()}\n")
with index_path.open("x", encoding="utf-8", newline="\n") as stream:
    stream.writelines(entries)
    stream.flush()
    os.fsync(stream.fileno())

for path in sorted(root.rglob("*"), reverse=True):
    try:
        path.chmod(path.stat().st_mode & ~(stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH))
    except OSError:
        pass
try:
    root.chmod(root.stat().st_mode & ~(stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH))
except OSError:
    pass
PY
    run_dir_sealed=1
    echo "RUN_NAMESPACE_SEALED id=$run_id status=$status index=$run_dir/SHA256SUMS"
}

phase_tag() {
    printf '%s-%s\n' "$tag" "$1"
}

phase_file() {
    local phase="$1"
    local suffix="$2"
    printf '%s/%s/%s\n' "$run_dir" "$phase" "$suffix"
}

append_save_event() {
    local phase="$1" state="$2" reason="${3:-}" inventory_sha="${4:-}" archive_sha="${5:-}" displaced="${6:-}"
    python3 - "$(host_path "$run_dir/save-restores.jsonl")" "$run_id" "$phase" "$state" \
        "$reason" "$inventory_sha" "$archive_sha" "$displaced" <<'PY'
import datetime
import json
import os
import sys

path, run_id, phase, state, reason, inventory_sha, archive_sha, displaced = sys.argv[1:]
entry = {
    "schema": "lsx4-bloodborne-save-restore/v1",
    "runId": run_id,
    "phase": phase,
    "state": state,
    "utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
}
for key, value in (
    ("reason", reason),
    ("inventorySha256", inventory_sha),
    ("archiveSha256", archive_sha),
    ("displacedPath", displaced),
):
    if value:
        entry[key] = value
payload = (json.dumps(entry, ensure_ascii=False, sort_keys=True) + "\n").encode("utf-8")
fd = os.open(path, os.O_WRONLY | os.O_APPEND)
try:
    remaining = memoryview(payload)
    while remaining:
        written = os.write(fd, remaining)
        if written <= 0:
            raise OSError("short append to save-restores.jsonl")
        remaining = remaining[written:]
    os.fsync(fd)
finally:
    os.close(fd)
PY
}

force_stop_and_verify_no_app_pid() {
    local reason="$1" pids='' attempt
    adb_device shell am force-stop "$package" >/dev/null || return 1
    for attempt in 1 2 3 4 5; do
        pids=$(adb_device shell \
            "pidof '$package' 2>/dev/null || true; pidof '$game_process' 2>/dev/null || true" \
            2>/dev/null | tr -d '\r[:space:]') || return 1
        [[ -z "$pids" ]] && break
        sleep 1
    done
    if [[ -n "$pids" ]]; then
        fail "reason=save-seed-force-stop-pid-still-live phase=$reason pids=$pids"
        return 1
    fi
    echo "SAVE_SEED_PROCESS_QUIESCENT phase=$reason package=$package"
}

probe_save_seed_contract() {
    local special symlink resolved
    adb_device shell run-as "$package" sh -c \
        "set -eu; test -d '$save_root'; test -d '$save_parent'; test -d '$save_path'; \
        test ! -L '$save_root'; test ! -L '$save_parent'; test ! -L '$save_path'; \
        for command_name in tar sha256sum find sort mv mkdir rmdir du df awk tail; do \
            command -v \"\$command_name\" >/dev/null; \
        done; cd '$save_parent'; tar -cf /dev/null '$save_leaf'" >/dev/null || {
        fail "reason=save-seed-runtime-contract-unavailable path=$save_path"
        return 1
    }
    symlink=$(adb_device shell run-as "$package" sh -c \
        "find '$save_path' -type l -print | head -n 1" 2>/dev/null | tr -d '\r') || return 1
    if [[ -n "$symlink" ]]; then
        fail "reason=save-seed-symlink-rejected path=$symlink"
        return 1
    fi
    special=$(adb_device shell run-as "$package" sh -c \
        "find '$save_path' ! -type d ! -type f -print | head -n 1" 2>/dev/null | tr -d '\r') || return 1
    if [[ -n "$special" ]]; then
        fail "reason=save-seed-special-file-rejected path=$special"
        return 1
    fi
    resolved=$(adb_device shell run-as "$package" sh -c \
        "set -eu; app_root=\$(pwd -P); save_absolute=\$(cd '$save_path' && pwd -P); \
        printf '%s\\n%s\\n' \"\$app_root\" \"\$save_absolute\"" \
        2>/dev/null | tr -d '\r') || return 1
    python3 - "$(host_path "$save_evidence_dir/runtime-mount-contract.json")" \
        "$resolved" "$package" "$save_path" <<'PY'
import json
import os
import pathlib
import sys

destination = pathlib.Path(sys.argv[1])
lines = sys.argv[2].splitlines()
package = sys.argv[3]
relative = sys.argv[4]
if len(lines) != 2:
    raise SystemExit(f"runtime mount discovery malformed: {lines!r}")
app_root, save_absolute = lines
expected_root = f"/data/user/0/{package}"
expected_save = f"{expected_root}/{relative}"
if app_root != expected_root or save_absolute != expected_save:
    raise SystemExit(
        f"runtime mount mismatch root={app_root!r} save={save_absolute!r} "
        f"expectedRoot={expected_root!r} expectedSave={expected_save!r}"
    )
value = {
    "schema": "lsx4-save-runtime-mount/v1",
    "appDataRoot": app_root,
    "saveAbsolutePath": save_absolute,
    "appPrivateRelativePath": relative,
    "runtimeMount": {"user": "1000", "title": "CUSA00207", "directory": "SPRJ0005"},
}
with destination.open("x", encoding="utf-8", newline="\n") as stream:
    json.dump(value, stream, indent=2, sort_keys=True)
    stream.write("\n")
    stream.flush()
    os.fsync(stream.fileno())
PY
    echo "SAVE_SEED_DISCOVERY_OK user=1000 title=CUSA00207 dir=$save_leaf path=$save_path commands=tar,sha256sum,find,sort,mv,mkdir,rmdir,du,df,awk,tail"
}

check_save_seed_free_space() {
    local values source_kib available_kib required_kib
    values=$(adb_device shell run-as "$package" sh -c \
        "set -eu; source_kib=\$(du -sk '$save_path' | awk '{print \$1}'); \
        available_kib=\$(df -k '$save_root' | awk 'END {print \$(NF-2)}'); \
        printf '%s\\n%s\\n' \"\$source_kib\" \"\$available_kib\"" \
        2>/dev/null | tr -d '\r') || return 1
    source_kib=$(sed -n '1p' <<< "$values")
    available_kib=$(sed -n '2p' <<< "$values")
    if [[ ! "$source_kib" =~ ^[0-9]+$ || ! "$available_kib" =~ ^[0-9]+$ ]]; then
        fail "reason=save-seed-free-space-probe-invalid sourceKiB=$source_kib availableKiB=$available_kib"
        return 1
    fi
    (( source_kib < 1 )) && source_kib=1
    required_kib=$((source_kib * save_space_multiplier + save_space_margin_kib))
    python3 - "$(host_path "$save_evidence_dir/free-space.json")" \
        "$source_kib" "$available_kib" "$required_kib" "$save_space_multiplier" \
        "$save_space_margin_kib" <<'PY'
import json
import os
import pathlib
import sys

destination = pathlib.Path(sys.argv[1])
source_kib, available_kib, required_kib, multiplier, margin_kib = map(int, sys.argv[2:])
value = {
    "schema": "lsx4-save-free-space-gate/v1",
    "sourceKiB": source_kib,
    "availableKiB": available_kib,
    "requiredKiB": required_kib,
    "minimumMultiplier": multiplier,
    "fixedMarginKiB": margin_kib,
    "accepted": available_kib >= required_kib,
}
with destination.open("x", encoding="utf-8", newline="\n") as stream:
    json.dump(value, stream, indent=2, sort_keys=True)
    stream.write("\n")
    stream.flush()
    os.fsync(stream.fileno())
PY
    if (( available_kib < required_kib )); then
        fail "reason=save-seed-free-space-insufficient sourceKiB=$source_kib availableKiB=$available_kib requiredKiB=$required_kib multiplier=$save_space_multiplier marginKiB=$save_space_margin_kib"
        return 1
    fi
    echo "SAVE_SEED_FREE_SPACE_OK sourceKiB=$source_kib availableKiB=$available_kib requiredKiB=$required_kib multiplier=$save_space_multiplier marginKiB=$save_space_margin_kib"
}

capture_save_inventory() {
    local base="$1" leaf="$2" output="$3" temporary
    temporary="$output.tmp-$$"
    if [[ -e "$output" || -e "$temporary" ]]; then
        fail "reason=save-inventory-host-collision output=$output"
        return 1
    fi
    if ! adb_device exec-out run-as "$package" sh -c \
        "set -eu; cd '$base'; find '$leaf' -type f -print | sort | \
        while IFS= read -r save_file; do sha256sum \"\$save_file\"; done" \
        > "$temporary"; then
        return 1
    fi
    tr -d '\r' < "$temporary" > "$output"
    rm -f -- "$temporary"
    python3 - "$(host_path "$output")" "$leaf" <<'PY'
import pathlib
import re
import sys

path = pathlib.Path(sys.argv[1])
leaf = sys.argv[2]
lines = path.read_text(encoding="utf-8").splitlines()
names = []
for line in lines:
    match = re.fullmatch(r"([0-9a-fA-F]{64})[ \t]+([A-Za-z0-9._/-]+)", line)
    if match is None:
        raise SystemExit(f"unsafe or malformed save inventory line: {line!r}")
    name = match.group(2)
    if name != leaf and not name.startswith(leaf + "/"):
        raise SystemExit(f"save inventory escaped leaf: {name!r}")
    if "/../" in f"/{name}/" or name.startswith("/"):
        raise SystemExit(f"unsafe save inventory path: {name!r}")
    names.append(name)
if names != sorted(names) or len(names) != len(set(names)):
    raise SystemExit("save inventory is not sorted and unique")
PY
    sha256sum "$output" | awk '{print $1}'
}

validate_save_archive_listing() {
    local listing="$save_evidence_dir/original-tar-list.txt" temporary="$save_evidence_dir/original-tar-list.txt.tmp-$$"
    if [[ -e "$listing" || -e "$temporary" ]]; then
        fail "reason=save-tar-list-host-collision path=$listing"
        return 1
    fi
    adb_device exec-out run-as "$package" tar -tf "$device_save_archive" > "$temporary" || return 1
    tr -d '\r' < "$temporary" > "$listing"
    rm -f -- "$temporary"
    python3 - "$(host_path "$listing")" "$save_leaf" <<'PY'
import pathlib
import re
import sys

path = pathlib.Path(sys.argv[1])
leaf = sys.argv[2]
entries = path.read_text(encoding="utf-8").splitlines()
if not entries:
    raise SystemExit("save archive listing is empty")
for entry in entries:
    normalized = entry.rstrip("/")
    if not re.fullmatch(r"[A-Za-z0-9._/-]+", normalized):
        raise SystemExit(f"unsafe save archive member: {entry!r}")
    if normalized != leaf and not normalized.startswith(leaf + "/"):
        raise SystemExit(f"save archive member escaped leaf: {entry!r}")
    if normalized.startswith("/") or "/../" in f"/{normalized}/":
        raise SystemExit(f"unsafe save archive member: {entry!r}")
PY
}

device_archive_sha256() {
    local actual recorded
    actual=$(adb_device shell run-as "$package" sha256sum "$device_save_archive" \
        2>/dev/null | tr -d '\r' | awk '{print $1}') || return 1
    recorded=$(adb_device shell run-as "$package" sh -c \
        "cat '$device_save_archive_hash'" 2>/dev/null | tr -d '\r' | awk '{print $1}') || return 1
    if [[ ! "$actual" =~ ^[0-9a-fA-F]{64}$ || "$actual" != "$recorded" ]]; then
        fail "reason=save-seed-archive-hash-mismatch actual=$actual recorded=$recorded"
        return 1
    fi
    printf '%s\n' "$actual"
}

prepare_save_seed() {
    local original_inventory_sha archive_sha device_root
    force_stop_and_verify_no_app_pid prepare
    probe_save_seed_contract
    check_save_seed_free_space
    adb_device shell run-as "$package" sh -c \
        "set -eu; umask 077; mkdir -p '$save_root/.codex-hle-clean-aba'; \
        test ! -e '$device_save_state_dir'; mkdir '$device_save_state_dir'" >/dev/null || return 1
    original_inventory_sha=$(capture_save_inventory "$save_parent" "$save_leaf" \
        "$save_evidence_dir/original-files.sha256") || return 1
    adb_device shell run-as "$package" sh -c \
        "set -eu; umask 077; device_root=\$(pwd -P); \
        test ! -e '$device_save_archive'; test ! -e '$device_save_archive.tmp'; \
        cd '$save_parent'; \
        tar -cf \"\$device_root/$device_save_archive.tmp\" '$save_leaf'; \
        sha256sum \"\$device_root/$device_save_archive.tmp\" > \
            \"\$device_root/$device_save_archive_hash.tmp\"; \
        mv \"\$device_root/$device_save_archive.tmp\" \"\$device_root/$device_save_archive\"; \
        mv \"\$device_root/$device_save_archive_hash.tmp\" \
            \"\$device_root/$device_save_archive_hash\"" >/dev/null || return 1
    archive_sha=$(device_archive_sha256) || return 1
    validate_save_archive_listing
    printf '%s  %s\n' "$archive_sha" "$device_save_archive" > \
        "$save_evidence_dir/device-original-tar.sha256"
    append_save_event seed prepared '' "$original_inventory_sha" "$archive_sha" \
        "$device_save_state_dir"
    save_seed_prepared=1
    echo "SAVE_SEED_PREPARED path=$save_path inventorySha256=$original_inventory_sha archiveSha256=$archive_sha retained=$device_save_archive"
}

restore_save_seed() {
    local phase="$1" stage displaced staged_inventory live_inventory original_inventory archive_sha expected_archive_sha
    (( save_seed_prepared == 1 )) || {
        fail "phase=$phase reason=save-seed-not-prepared"
        return 1
    }
    (( save_restore_sequence += 1 ))
    stage="$save_parent/.codex-stage-$run_id-$save_restore_sequence-$phase"
    displaced="$device_save_state_dir/displaced-$save_restore_sequence-$phase"
    append_save_event "$phase" started '' '' '' "$displaced"
    force_stop_and_verify_no_app_pid "$phase" || {
        append_save_event "$phase" rejected force-stop-failed
        return 1
    }
    archive_sha=$(device_archive_sha256) || {
        append_save_event "$phase" rejected archive-hash-mismatch
        return 1
    }
    expected_archive_sha=$(awk 'NR == 1 {print $1}' \
        "$save_evidence_dir/device-original-tar.sha256") || return 1
    if [[ ! "$expected_archive_sha" =~ ^[0-9a-fA-F]{64}$ ||
          "$archive_sha" != "$expected_archive_sha" ]]; then
        append_save_event "$phase" rejected host-anchored-archive-hash-mismatch '' \
            "$archive_sha" "$displaced"
        fail "phase=$phase reason=save-seed-host-anchored-archive-hash-mismatch expected=$expected_archive_sha actual=$archive_sha"
        return 1
    fi
    adb_device shell run-as "$package" sh -c \
        "set -eu; device_root=\$(pwd -P); \
        test ! -e '$stage'; test ! -e '$displaced'; mkdir '$stage'; \
        cd '$stage'; tar -xf \"\$device_root/$device_save_archive\"; \
        test -d '$save_leaf'" >/dev/null || {
        append_save_event "$phase" rejected staging-extract-failed '' "$archive_sha" "$displaced"
        return 1
    }
    staged_inventory=$(capture_save_inventory "$stage" "$save_leaf" \
        "$save_evidence_dir/$save_restore_sequence-$phase-staged-files.sha256") || {
        append_save_event "$phase" rejected staged-inventory-failed '' "$archive_sha" "$displaced"
        return 1
    }
    original_inventory=$(sha256sum "$save_evidence_dir/original-files.sha256" | awk '{print $1}')
    if [[ "$staged_inventory" != "$original_inventory" ]]; then
        append_save_event "$phase" rejected staged-inventory-mismatch "$staged_inventory" "$archive_sha" "$displaced"
        fail "phase=$phase reason=save-seed-staged-inventory-mismatch expected=$original_inventory actual=$staged_inventory"
        return 1
    fi
    adb_device shell run-as "$package" sh -c \
        "set -eu; had_live=0; \
        if [ -e '$save_path' ]; then \
            test -d '$save_path'; mv '$save_path' '$displaced'; had_live=1; \
        fi; \
        if mv '$stage/$save_leaf' '$save_path'; then \
            rmdir '$stage' 2>/dev/null || true; \
        else \
            if [ \"\$had_live\" = 1 ] && [ ! -e '$save_path' ]; then \
                mv '$displaced' '$save_path'; \
            fi; \
            exit 1; \
        fi" >/dev/null || {
        append_save_event "$phase" rejected atomic-swap-failed "$staged_inventory" "$archive_sha" "$displaced"
        return 1
    }
    force_stop_and_verify_no_app_pid "$phase-postswap" || {
        append_save_event "$phase" rejected post-swap-pid-check-failed "$staged_inventory" "$archive_sha" "$displaced"
        return 1
    }
    live_inventory=$(capture_save_inventory "$save_parent" "$save_leaf" \
        "$save_evidence_dir/$save_restore_sequence-$phase-live-files.sha256") || {
        append_save_event "$phase" rejected live-inventory-failed "$staged_inventory" "$archive_sha" "$displaced"
        return 1
    }
    if [[ "$live_inventory" != "$original_inventory" ]]; then
        append_save_event "$phase" rejected live-inventory-mismatch "$live_inventory" "$archive_sha" "$displaced"
        fail "phase=$phase reason=save-seed-live-inventory-mismatch expected=$original_inventory actual=$live_inventory"
        return 1
    fi
    append_save_event "$phase" committed '' "$live_inventory" "$archive_sha" "$displaced"
    echo "SAVE_SEED_RESTORED phase=$phase inventorySha256=$live_inventory archiveSha256=$archive_sha displaced=$displaced atomic=true"
}

enable_constant_safety_controls() {
    local phase="$1"
    adb_device shell run-as "$package" sh -c \
        "set -eu; mkdir -p '$runtime_home'; : > '$async_shader_disable_marker'; \
        test -f '$async_shader_disable_marker'" >/dev/null || return 1
    echo "CONSTANT_SAFETY_MARKER phase=$phase asyncShaderRetranslation=disabled marker=present"
}

assert_constant_safety_controls() {
    local phase="$1"
    adb_device shell run-as "$package" test -f "$async_shader_disable_marker" >/dev/null || {
        fail "phase=$phase reason=constant-safety-marker-missing marker=$async_shader_disable_marker"
        return 1
    }
    echo "CONSTANT_SAFETY_MARKER_PROVEN phase=$phase asyncShaderRetranslation=disabled marker=present"
}

remove_constant_safety_controls() {
    adb_device shell run-as "$package" rm -f "$async_shader_disable_marker" >/dev/null || return 1
    assert_constant_safety_controls_absent
}

assert_constant_safety_controls_absent() {
    adb_device shell run-as "$package" sh -c \
        "test ! -e '$async_shader_disable_marker'" >/dev/null || {
        fail "reason=constant-safety-marker-not-removed marker=$async_shader_disable_marker"
        return 1
    }
    echo 'PRODUCTION_SAFETY_DEFAULT asyncShaderRetranslation=default-on marker=absent'
}

remove_experiment_markers() {
    adb_device shell "run-as $package mkdir -p '$runtime_home' && \
        run-as $package rm -f ${experiment_markers[*]}" >/dev/null
}

assert_experiment_markers_absent() {
    local probe='' marker
    for marker in "${experiment_markers[@]}"; do
        probe+="if [ -e '$marker' ]; then echo '$marker'; fi; "
    done
    local active
    active=$(adb_device shell "run-as $package sh -c \"$probe\"" 2>/dev/null | tr -d '\r') || return 1
    if [[ -n "$active" ]]; then
        fail "reason=confounding-marker-active markers=${active//$'\n'/,}"
        return 1
    fi
}

assert_graphics_baseline() {
    local probe='' marker
    for marker in "${graphics_baseline_markers[@]}"; do
        probe+="if [ ! -e '$marker' ]; then echo '$marker'; fi; "
    done
    local missing
    missing=$(adb_device shell "run-as $package sh -c \"$probe\"" 2>/dev/null | tr -d '\r') || return 1
    if [[ -n "$missing" ]]; then
        fail "reason=graphics-baseline-marker-missing markers=${missing//$'\n'/,}"
        return 1
    fi
    echo 'GRAPHICS_BASELINE_OK allSix=OFF markers=6/6'
}

launch_exact_baseline() {
    adb_device shell "am start -n $package/.MainActivity \
        --ez autoload_existing_runtime true --ez autoinit_runtime true \
        --es scan_game_path $game --es launch_game_path $game \
        --es game_platform ps4 --ez fullscreen_render true \
        --ez embedded_aarch64_jit_backend true --ei render_resolution_mode 4 \
        --ez profile_graphics_override true \
        --ez profile_disable_dynamic_shadows true \
        --ez profile_disable_ssao true \
        --ez profile_disable_motion_blur true \
        --ez profile_disable_depth_of_field true \
        --ez profile_disable_anti_aliasing true \
        --ez profile_disable_chromatic_aberration true" >/dev/null
}

restore_baseline_running_best_effort() {
    set +e
    echo 'CLEANUP_BEGIN target=baseline-exact-running marker=absent allSix=OFF'
    local state current_pid attempt
    state=$(adb_device get-state 2>/dev/null | tr -d '\r[:space:]')
    if [[ "$state" != device ]]; then
        echo "CLEANUP_DEVICE_UNAVAILABLE serial=$device"
        return 0
    fi
    if (( save_seed_prepared == 1 )); then
        if ! restore_save_seed cleanup; then
            echo "CLEANUP_SAVE_SEED_RESTORE_FAILED retained=$device_save_archive" >&2
        else
            echo "CLEANUP_SAVE_SEED_RESTORED retained=$device_save_archive"
        fi
    fi
    remove_experiment_markers || true
    remove_constant_safety_controls || true
    assert_experiment_markers_absent || true
    current_pid=$(adb_device shell "pidof $game_process" 2>/dev/null | tr -d '\r[:space:]')
    if [[ "$active_mode" == production && -n "$current_pid" ]]; then
        assert_graphics_baseline || return 1
        echo "CLEANUP_KEEP_RUNNING_PRODUCTION_BASELINE pid=$current_pid exactGame=$game asyncShaderRetranslation=default-on"
        return 0
    fi
    adb_device shell am force-stop "$package" >/dev/null 2>&1 || true
    sleep 1
    remove_experiment_markers || true
    launch_exact_baseline || true
    current_pid=''
    for attempt in 1 2 3; do
        sleep 5
        current_pid=$(adb_device shell "pidof $game_process" 2>/dev/null | tr -d '\r[:space:]')
        [[ -n "$current_pid" ]] && break
        launch_exact_baseline || true
    done
    if [[ -z "$current_pid" ]]; then
        echo 'CLEANUP_BASELINE_RELAUNCH_FAILED' >&2
        return 1
    fi
    active_mode=production
    assert_experiment_markers_absent || return 1
    assert_graphics_baseline || return 1
    echo "CLEANUP_BASELINE_RUNNING pid=$current_pid exactGame=$game marker=absent allSix=OFF"
}

on_err() {
    local status="$1" line="$2" command="$3"
    [[ "$BASHPID" == "$root_shell_pid" ]] || return "$status"
    trap - ERR
    reject_open_windows shell-error "$status" "$line" "${command:0:240}" || true
    return "$status"
}

on_exit() {
    local status=$?
    trap - ERR EXIT INT TERM
    set +e
    stop_process_identity_observer || true
    if (( device_session_started == 1 && cleanup_complete == 0 && cleanup_running == 0 )); then
        cleanup_running=1
        restore_baseline_running_best_effort || true
    fi
    reject_open_windows "process-exit-$status" || true
    if ! seal_run_directory "$status"; then
        echo "RUN_NAMESPACE_SEAL_FAILED id=$run_id" >&2
        (( status == 0 )) && status=70
    fi
    exit "$status"
}

trap 'on_err "$?" "$LINENO" "$BASH_COMMAND"' ERR
trap on_exit EXIT
trap 'exit 130' INT TERM

run_phase() {
    local phase="$1"
    local mode="$2"
    local keep_running="$3"
    local driver_log driver_status tee_status
    restore_save_seed "$phase"
    enable_constant_safety_controls "$phase"
    if ! mkdir "$run_dir/$phase"; then
        fail "phase=$phase reason=immutable-phase-directory-collision path=$run_dir/$phase"
        return 1
    fi
    driver_log="$(phase_file "$phase" 'driver.txt')"
    begin_window "$phase" "$mode"
    start_process_identity_observer "$phase"
    echo "PHASE_BEGIN name=$phase hleClean=$mode diagnosticStats=1 diagnosticOnly=true fpsVerdictAllowed=false route=iosefka-door-forward camera=unchanged warmup=$warmup_seconds window=$window"
    set +e
    (
        trap - ERR
        set -Eeuo pipefail
        cd "$repo_root"
        env \
            ADB_SERIAL="$device" \
            BB_HLE_CLEAN_BUFFER_FASTPATH="$mode" \
            BB_HLE_CLEAN_DIAGNOSTIC_STATS=1 \
            BB_DISABLE_ASYNC_SHADER_RETRANSLATION=1 \
            BB_MIN_HLE_SYNC_COVERAGE="$minimum_sync_coverage" \
            BB_STRICT_PHASE_GATES=1 \
            BB_VERIFY_CANONICAL_FEX=1 \
            BB_KEEP_GAME_RUNNING="$keep_running" \
            BB_PROFILE_INVALIDATION_SOURCES=0 \
            BB_PROFILE_INVALIDATION_OVERLAP=0 \
            ENABLE_PERF_MAP=0 \
            ENABLE_RECORD=0 \
            BB_GRAPHICS_OVERRIDE=1 \
            BB_DISABLE_DYNAMIC_SHADOWS=true \
            BB_DISABLE_SSAO=true \
            BB_DISABLE_MOTION_BLUR=true \
            BB_DISABLE_DEPTH_OF_FIELD=true \
            BB_DISABLE_ANTI_ALIASING=true \
            BB_DISABLE_CHROMATIC_ABERRATION=true \
            BB_MEASUREMENT_INPUT=forward \
            BB_PRE_MEASURE_WARMUP="$warmup_seconds" \
            BB_WARNING_WAIT="${BB_WARNING_WAIT:-64}" \
            BB_OFFLINE_WAIT="${BB_OFFLINE_WAIT:-30}" \
            BB_CONTINUE_WAIT="${BB_CONTINUE_WAIT:-22}" \
            BB_DOOR_WAIT="${BB_DOOR_WAIT:-65}" \
            BB_DOOR_SETTLE_WAIT="${BB_DOOR_SETTLE_WAIT:-5}" \
            BB_ROLE_PROBE_WAIT="${BB_ROLE_PROBE_WAIT:-3}" \
            BB_LIVENESS_TAIL_SECONDS="${BB_LIVENESS_TAIL_SECONDS:-3}" \
            bash "$pmu_driver" "$window" "$(phase_tag "$phase")"
    ) | tee "$driver_log"
    local pipeline_status=("${PIPESTATUS[@]}")
    set -e
    stop_process_identity_observer
    driver_status="${pipeline_status[0]:-125}"
    tee_status="${pipeline_status[1]:-125}"
    collect_phase_artifacts "$phase" || {
        terminal_window "$phase" rejected artifact-collection-failed
        return 1
    }
    if (( driver_status != 0 || tee_status != 0 )); then
        terminal_window "$phase" rejected "driver-status-$driver_status-tee-status-$tee_status"
        fail "phase=$phase reason=driver-failed driverStatus=$driver_status teeStatus=$tee_status"
        return 1
    fi
    validate_measurement_window_artifact "$phase" "$mode" || {
        terminal_window "$phase" rejected measurement-window-json-invalid
        return 1
    }
    assert_constant_safety_controls_absent || {
        terminal_window "$phase" rejected constant-safety-marker-not-cleaned
        return 1
    }

    local pid
    pid=$(awk '/^DONE pid=/{sub(/^DONE pid=/, ""); split($0,a," "); value=a[1]} END{print value}' "$driver_log")
    if [[ -z "$pid" ]]; then
        terminal_window "$phase" rejected missing-done-pid
        fail "phase=$phase reason=missing-done-pid"
        return 1
    fi
    validate_process_identity "$phase" "$pid" || {
        terminal_window "$phase" rejected process-starttime-not-proven
        return 1
    }
    local process_identity="$pid:${phase_starttime[$phase]}"
    if [[ -n "$last_process_identity" && "$process_identity" == "$last_process_identity" ]]; then
        terminal_window "$phase" rejected process-identity-not-fresh
        fail "phase=$phase reason=process-identity-not-fresh previous=$last_process_identity actual=$process_identity"
        return 1
    fi
    for token in \
        'STRICT_CONFOUNDERS=none GRAPHICS_RESET=1 TARGET_ALL_SIX=off ADPF=25ms VIVO=performance-hint-4241' \
        'POST_LAUNCH_GRAPHICS_BASELINE ALL_SIX=off markers=6/6' \
        'STRICT_ROOT_BOOST_SETTING=disabled' \
        "HLE_CLEAN_MARKER_STATE expected=$mode verified=1" \
        'HLE_CLEAN_STATS_MARKER_STATE expected=1 verified=1 diagnosticOnly=1' \
        "HLE_CLEAN_PROCESS_ENV expected=$mode verified=1" \
        'CANONICAL_FEX_RUNTIME_VERIFIED' \
        'CANONICAL_FEX_STATUS_OK configureFlags=0 faultSignalOwner=unselected signalMode=split selectionLocked=true' \
        'ASYNC_SHADER_RETRANSLATION_MARKER_STATE expectedDisabled=1 verified=1' \
        'POST_LAUNCH_ASYNC_SHADER_RETRANSLATION_MARKER expectedDisabled=1 verified=1' \
        'POST_LAUNCH_HLE_CLEAN_STATS_MARKER expected=1 verified=1 diagnosticOnly=1' \
        'ASYNC_SHADER_RETRANSLATION_LOG_OK disabled=1 queue=off' \
        'ASYNC_SHADER_RETRANSLATION_MARKER_CLEAN=1' \
        "STRICT_PHASE_OK mode=$mode" \
        'HLE_CLEAN_MARKER_CLEAN=1' \
        'HLE_CLEAN_STATS_MARKER_CLEAN=1'; do
        if ! grep -Fq "$token" "$driver_log"; then
            terminal_window "$phase" rejected "driver-contract-token-missing"
            fail "phase=$phase reason=driver-contract-token-missing token=$token"
            return 1
        fi
    done
    if [[ "$mode" == 1 ]]; then
        grep -Fq 'HLE_CLEAN_COUNTERS_OK' "$driver_log" || {
            terminal_window "$phase" rejected fastpath-counters-not-proven
            fail "phase=$phase reason=fastpath-counters-not-proven"
            return 1
        }
    else
        grep -Fq 'HLE_CLEAN_LOG_BASELINE_OK enabled=0 counters=absent' "$driver_log" || {
            terminal_window "$phase" rejected baseline-log-contract-not-proven
            fail "phase=$phase reason=baseline-log-contract-not-proven"
            return 1
        }
    fi
    if ! grep -Eq '\[EXECUTOR_ASYNC_SHADER_RETRANSLATION\] source=sentinel .*enabled=0 disabled=1' \
            "$driver_log" ||
       ! grep -Fq "[EXECUTOR_ASYNC_SHADER_RETRANSLATION_CONFIG] disabled=1 fastTierAllowed=0 hleGuardRequested=$mode hleGuardEnabled=$mode" \
            "$driver_log"; then
        terminal_window "$phase" rejected constant-safety-runtime-proof-missing
        fail "phase=$phase reason=constant-safety-runtime-proof-missing expected=sentinel-disabled+pipeline-consumer-disabled"
        return 1
    fi
    if [[ "$mode" == 1 ]] &&
       ! grep -Eq '\[EXECUTOR_HLE_CLEAN_BUFFER_FASTPATH\].*asyncShaderRetranslationDisabled=1' \
            "$driver_log"; then
        terminal_window "$phase" rejected hle-safety-coupling-proof-missing
        fail "phase=$phase reason=hle-safety-coupling-proof-missing"
        return 1
    fi
    # Only publish the process mode after every phase proof passed.  If a
    # driver/token check aborts while switching clean -> baseline, cleanup must
    # not mistake the still-running clean process for a proven baseline.
    active_mode="$([[ "$mode" == 0 ]] && echo baseline || echo clean)"
    last_pid="$pid"
    last_process_identity="$process_identity"
    case "$phase" in
    baseline-a1) baseline_a1_pid="$pid" ;;
    clean-b) clean_b_pid="$pid" ;;
    baseline-a2) baseline_a2_pid="$pid" ;;
    esac
    echo "PHASE_DONE name=$phase pid=$pid hleClean=$mode diagnosticStats=1 diagnosticOnly=true fpsVerdictAllowed=false fresh=true windowState=pending-aba"
}

run_production_handoff() {
    local phase='production-handoff' driver_log driver_status tee_status pid current_pid
    local process_identity causal_identity
    restore_save_seed "$phase"
    remove_experiment_markers
    remove_constant_safety_controls
    assert_experiment_markers_absent
    assert_constant_safety_controls_absent
    if ! mkdir "$run_dir/$phase"; then
        fail "phase=$phase reason=immutable-phase-directory-collision path=$run_dir/$phase"
        return 1
    fi
    driver_log="$(phase_file "$phase" 'driver.txt')"
    start_process_identity_observer "$phase"
    echo "PRODUCTION_HANDOFF_BEGIN hleClean=0 asyncShaderRetranslation=default-on save=original-restored allSix=OFF"
    set +e
    (
        trap - ERR
        set -Eeuo pipefail
        cd "$repo_root"
        env \
            ADB_SERIAL="$device" \
            BB_HLE_CLEAN_BUFFER_FASTPATH=0 \
            BB_HLE_CLEAN_DIAGNOSTIC_STATS=0 \
            BB_DISABLE_ASYNC_SHADER_RETRANSLATION=0 \
            BB_MIN_HLE_SYNC_COVERAGE="$minimum_sync_coverage" \
            BB_STRICT_PHASE_GATES=1 \
            BB_VERIFY_CANONICAL_FEX=1 \
            BB_KEEP_GAME_RUNNING=1 \
            BB_PROFILE_INVALIDATION_SOURCES=0 \
            BB_PROFILE_INVALIDATION_OVERLAP=0 \
            ENABLE_PERF_MAP=0 \
            ENABLE_RECORD=0 \
            BB_GRAPHICS_OVERRIDE=1 \
            BB_DISABLE_DYNAMIC_SHADOWS=true \
            BB_DISABLE_SSAO=true \
            BB_DISABLE_MOTION_BLUR=true \
            BB_DISABLE_DEPTH_OF_FIELD=true \
            BB_DISABLE_ANTI_ALIASING=true \
            BB_DISABLE_CHROMATIC_ABERRATION=true \
            BB_MEASUREMENT_INPUT=forward \
            BB_PRE_MEASURE_WARMUP="$warmup_seconds" \
            BB_WARNING_WAIT="${BB_WARNING_WAIT:-64}" \
            BB_OFFLINE_WAIT="${BB_OFFLINE_WAIT:-30}" \
            BB_CONTINUE_WAIT="${BB_CONTINUE_WAIT:-22}" \
            BB_DOOR_WAIT="${BB_DOOR_WAIT:-65}" \
            BB_DOOR_SETTLE_WAIT="${BB_DOOR_SETTLE_WAIT:-5}" \
            BB_ROLE_PROBE_WAIT="${BB_ROLE_PROBE_WAIT:-3}" \
            BB_LIVENESS_TAIL_SECONDS="${BB_LIVENESS_TAIL_SECONDS:-3}" \
            bash "$pmu_driver" "$window" "$(phase_tag "$phase")"
    ) | tee "$driver_log"
    local pipeline_status=("${PIPESTATUS[@]}")
    set -e
    stop_process_identity_observer
    driver_status="${pipeline_status[0]:-125}"
    tee_status="${pipeline_status[1]:-125}"
    collect_phase_artifacts "$phase"
    if (( driver_status != 0 || tee_status != 0 )); then
        fail "phase=$phase reason=driver-failed driverStatus=$driver_status teeStatus=$tee_status"
        return 1
    fi
    validate_measurement_window_artifact "$phase" 0 0
    pid=$(awk '/^DONE pid=/{sub(/^DONE pid=/, ""); split($0,a," "); value=a[1]} END{print value}' "$driver_log")
    if [[ -z "$pid" ]]; then
        fail "phase=$phase reason=missing-done-pid"
        return 1
    fi
    validate_process_identity "$phase" "$pid"
    process_identity="$pid:${phase_starttime[$phase]}"
    for causal_identity in \
        "${phase_pid[baseline-a1]}:${phase_starttime[baseline-a1]}" \
        "${phase_pid[clean-b]}:${phase_starttime[clean-b]}" \
        "${phase_pid[baseline-a2]}:${phase_starttime[baseline-a2]}"; do
        if [[ "$process_identity" == "$causal_identity" ]]; then
            fail "phase=$phase reason=handoff-process-identity-not-fresh actual=$process_identity causal=$causal_identity"
            return 1
        fi
    done
    for token in \
        'STRICT_CONFOUNDERS=none GRAPHICS_RESET=1 TARGET_ALL_SIX=off ADPF=25ms VIVO=performance-hint-4241' \
        'POST_LAUNCH_GRAPHICS_BASELINE ALL_SIX=off markers=6/6' \
        'STRICT_ROOT_BOOST_SETTING=disabled' \
        'HLE_CLEAN_MARKER_STATE expected=0 verified=1' \
        'HLE_CLEAN_STATS_MARKER_STATE expected=0 verified=1 diagnosticOnly=0' \
        'HLE_CLEAN_PROCESS_ENV expected=0 verified=1' \
        'CANONICAL_FEX_RUNTIME_VERIFIED' \
        'CANONICAL_FEX_STATUS_OK configureFlags=0 faultSignalOwner=unselected signalMode=split selectionLocked=true' \
        'ASYNC_SHADER_RETRANSLATION_MARKER_STATE expectedDisabled=0 verified=1' \
        'POST_LAUNCH_ASYNC_SHADER_RETRANSLATION_MARKER expectedDisabled=0 verified=1' \
        'POST_LAUNCH_HLE_CLEAN_STATS_MARKER expected=0 verified=1 diagnosticOnly=0' \
        'ASYNC_SHADER_RETRANSLATION_LOG_OK disabled=0 queue=async' \
        'ASYNC_SHADER_RETRANSLATION_MARKER_CLEAN=1' \
        'STRICT_PHASE_OK mode=0' \
        'HLE_CLEAN_LOG_BASELINE_OK enabled=0 counters=absent' \
        'HLE_CLEAN_MARKER_CLEAN=1' \
        'HLE_CLEAN_STATS_MARKER_CLEAN=1'; do
        if ! grep -Fq "$token" "$driver_log"; then
            fail "phase=$phase reason=driver-contract-token-missing token=$token"
            return 1
        fi
    done
    if grep -Eq '\[EXECUTOR_ASYNC_SHADER_RETRANSLATION\] source=sentinel .*enabled=0 disabled=1' \
            "$driver_log" ||
       ! grep -Fq '[EXECUTOR_ASYNC_SHADER_RETRANSLATION_CONFIG] disabled=0 fastTierAllowed=1 hleGuardRequested=0 hleGuardEnabled=0' \
            "$driver_log"; then
        fail "phase=$phase reason=production-async-shader-default-not-proven"
        return 1
    fi
    assert_experiment_markers_absent
    assert_constant_safety_controls_absent
    assert_graphics_baseline
    current_pid=$(adb_device shell "pidof '$game_process' 2>/dev/null || true" \
        2>/dev/null | tr -d '\r[:space:]') || return 1
    if [[ "$current_pid" != "$pid" ]]; then
        fail "phase=$phase reason=final-exact-game-not-running expected=$pid actual=$current_pid"
        return 1
    fi
    final_handoff_pid="$pid"
    final_handoff_starttime="${phase_starttime[$phase]}"
    active_mode=production
    python3 - \
        "$(host_path "$run_dir/production-handoff.json")" \
        "$final_handoff_pid" "$final_handoff_starttime" \
        "$(host_path "$driver_log")" \
        "$(host_path "$(phase_file "$phase" 'hle-clean-window.json')")" <<'PY'
import datetime
import hashlib
import json
import os
import pathlib
import sys

destination = pathlib.Path(sys.argv[1])
pid = int(sys.argv[2])
starttime = int(sys.argv[3])
driver = pathlib.Path(sys.argv[4])
window = pathlib.Path(sys.argv[5])
value = {
    "schema": "lsx4-bloodborne-production-handoff/v1",
    "createdUtc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "processIdentity": {"pid": pid, "starttime": starttime},
    "exactGameRunning": True,
    "gameplayProof": "STRICT_PHASE_OK mode=0",
    "hleClean": 0,
    "diagnosticStats": 0,
    "asyncShaderRetranslation": "default-on",
    "graphicsAllSix": "off",
    "saveOriginalRestoredBeforeLaunch": True,
    "artifacts": {
        "driver": {
            "path": "production-handoff/driver.txt",
            "sha256": hashlib.sha256(driver.read_bytes()).hexdigest(),
        },
        "measurementWindow": {
            "path": "production-handoff/hle-clean-window.json",
            "sha256": hashlib.sha256(window.read_bytes()).hexdigest(),
        },
    },
}
with destination.open("x", encoding="utf-8", newline="\n") as stream:
    json.dump(value, stream, indent=2, sort_keys=True)
    stream.write("\n")
    stream.flush()
    os.fsync(stream.fileno())
PY
    echo "PRODUCTION_HANDOFF_OK pid=$final_handoff_pid starttime=$final_handoff_starttime freshFromCausal=true hleClean=0 diagnosticStats=0 asyncShaderRetranslation=default-on allSix=OFF exactGameplay=true exactGame=$game"
}

scene_metric() {
    local phase="$1" key="$2"
    awk -v key="$key" '{for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]==key) print a[2]}}' \
        "$(phase_file "$phase" 'scene.txt')"
}

compare_scene_metric() {
    local key="$1" tolerance="$2" a1 b a2
    a1=$(scene_metric baseline-a1 "$key")
    b=$(scene_metric clean-b "$key")
    a2=$(scene_metric baseline-a2 "$key")
    if ! awk -v a="$a1" -v b="$b" -v c="$a2" -v t="$tolerance" '
        BEGIN {
            mean=(a+c)/2; adrift=(a>c?a-c:c-a); bdrift=(b>mean?b-mean:mean-b)
            if (mean == 0) exit !(a == 0 && b == 0 && c == 0)
            exit !(mean > 0 && 100*adrift/mean <= t && 100*bdrift/mean <= t)
        }'; then
        fail "reason=aba-workload-mismatch metric=$key a1=$a1 b=$b a2=$a2 tolerancePct=$tolerance"
        return 1
    fi
}

compare_phase_contracts() {
    local a1_identity="${phase_pid[baseline-a1]:-}:${phase_starttime[baseline-a1]:-}"
    local b_identity="${phase_pid[clean-b]:-}:${phase_starttime[clean-b]:-}"
    local a2_identity="${phase_pid[baseline-a2]:-}:${phase_starttime[baseline-a2]:-}"
    if [[ -z "$baseline_a1_pid" || -z "$clean_b_pid" || -z "$baseline_a2_pid" ||
          "$a1_identity" == *: || "$b_identity" == *: || "$a2_identity" == *: ||
          "$a1_identity" == "$b_identity" || "$a1_identity" == "$a2_identity" ||
          "$b_identity" == "$a2_identity" ]]; then
        fail "reason=aba-process-identities-not-unique a1=$a1_identity b=$b_identity a2=$a2_identity"
        return 1
    fi
    compare_scene_metric builderUnitsPerFrame "${BB_SCENE_WORK_TOLERANCE_PCT:-15}"
    compare_scene_metric builderDrawPerFrame "${BB_SCENE_DRAW_TOLERANCE_PCT:-15}"
    compare_scene_metric builderShaderPerFrame "${BB_SCENE_SHADER_TOLERANCE_PCT:-25}"
    python3 - \
        "$(host_path "$(phase_file baseline-a1 'end-graphics.json')")" \
        "$(host_path "$(phase_file clean-b 'end-graphics.json')")" \
        "$(host_path "$(phase_file baseline-a2 'end-graphics.json')")" <<'PY'
import json
import pathlib
import sys

a1, b, a2 = [json.loads(pathlib.Path(path).read_text(encoding="utf-8")) for path in sys.argv[1:4]]
if (a1["width"], a1["height"]) != (b["width"], b["height"]) or (a1["width"], a1["height"]) != (a2["width"], a2["height"]):
    raise SystemExit("A/B/A screenshot dimensions differ")
def l1(left, right):
    return sum(abs(x-y) for x, y in zip(left, right))
baseline = [(x+y)/2 for x, y in zip(a1["histogram"], a2["histogram"])]
a_drift = l1(a1["histogram"], a2["histogram"])
b_drift = l1(b["histogram"], baseline)
if a_drift > 0.40 or b_drift > 0.45:
    raise SystemExit(f"graphics histogram mismatch a={a_drift:.6f} b={b_drift:.6f}")
mean = (a1["mean_luma"] + a2["mean_luma"])/2
if abs(b["mean_luma"] - mean) > max(18.0, mean*0.35):
    raise SystemExit("clean-B graphics luminance mismatch")
baseline_white = (a1["white_fraction"] + a2["white_fraction"])/2
white_limit = max(0.20, baseline_white*3.0 + 0.02)
if b["white_fraction"] > white_limit:
    raise SystemExit(
        f"clean-B white/missing-geometry gate failed baseline={baseline_white:.6f} "
        f"clean={b['white_fraction']:.6f} limit={white_limit:.6f}"
    )
print(f"GRAPHICS_ABA_OK aHistogramL1={a_drift:.6f} bHistogramL1={b_drift:.6f}")
PY
    local role a1_hash b_hash a2_hash
    for role in executor adapter bridge fexcore; do
        a1_hash=$(awk -v role="$role" '$1 == "role="role {for(i=1;i<=NF;i++) if($i ~ /^loadedSha256=/){split($i,a,"="); print a[2]}}' \
            "$(phase_file baseline-a1 'loaded-fex-hashes.txt')")
        b_hash=$(awk -v role="$role" '$1 == "role="role {for(i=1;i<=NF;i++) if($i ~ /^loadedSha256=/){split($i,a,"="); print a[2]}}' \
            "$(phase_file clean-b 'loaded-fex-hashes.txt')")
        a2_hash=$(awk -v role="$role" '$1 == "role="role {for(i=1;i<=NF;i++) if($i ~ /^loadedSha256=/){split($i,a,"="); print a[2]}}' \
            "$(phase_file baseline-a2 'loaded-fex-hashes.txt')")
        if [[ -z "$a1_hash" || "$a1_hash" != "$b_hash" || "$a1_hash" != "$a2_hash" ]]; then
            fail "reason=aba-loaded-hash-mismatch role=$role a1=$a1_hash b=$b_hash a2=$a2_hash"
            return 1
        fi
    done
    echo "ABA_PROVENANCE_OK roles=executor,adapter,bridge,fexcore configureFlags=0 processIdentities=$a1_identity,$b_identity,$a2_identity"
}

evaluate_mechanism_evidence() {
    python3 - \
        "$(host_path "$(phase_file baseline-a1 'hle-clean-window.json')")" \
        "$(host_path "$(phase_file clean-b 'hle-clean-window.json')")" \
        "$(host_path "$(phase_file baseline-a2 'hle-clean-window.json')")" \
        "$(host_path "$(phase_file baseline-a1 'sf.txt')")" \
        "$(host_path "$(phase_file clean-b 'sf.txt')")" \
        "$(host_path "$(phase_file baseline-a2 'sf.txt')")" \
        "$(host_path "$(phase_file baseline-a1 'process-identity.json')")" \
        "$(host_path "$(phase_file clean-b 'process-identity.json')")" \
        "$(host_path "$(phase_file baseline-a2 'process-identity.json')")" \
        "$(host_path "$run_dir/decision.json")" \
        "$minimum_wait_reduction_pct" "$minimum_sync_coverage" <<'PY'
import datetime
import hashlib
import json
import os
import pathlib
import re
import sys

window_paths = [pathlib.Path(value) for value in sys.argv[1:4]]
sf_paths = [pathlib.Path(value) for value in sys.argv[4:7]]
identity_paths = [pathlib.Path(value) for value in sys.argv[7:10]]
destination = pathlib.Path(sys.argv[10])
minimum_wait_reduction = float(sys.argv[11])
minimum_sync_coverage = float(sys.argv[12])
phase_names = ("baseline-a1", "clean-b", "baseline-a2")


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sf_metric(path):
    start = None
    end = None
    frames = 0
    game = False
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("statsStart = ") and start is None:
            start = int(line.split("=", 1)[1].strip())
        elif line.startswith("statsEnd = ") and end is None:
            end = int(line.split("=", 1)[1].strip())
        elif line.startswith("layerName = "):
            game = "SurfaceView[app.lsx4.android/app.lsx4.android.GameActivity](BLAST)" in line
        elif game and line.startswith("totalFrames = "):
            frames += int(line.split("=", 1)[1].strip())
    if start is None or end is None or end <= start or frames <= 0:
        raise ValueError(f"invalid SurfaceFlinger measurement in {path}: start={start} end={end} frames={frames}")
    seconds = end - start
    return {"frames": frames, "seconds": seconds, "fps": frames / seconds}


windows = [json.loads(path.read_text(encoding="utf-8")) for path in window_paths]
surface = [sf_metric(path) for path in sf_paths]
identities = [json.loads(path.read_text(encoding="utf-8")) for path in identity_paths]
wait = [value["guestInvalidation"]["waitMsPerFrame"] for value in windows]
if any(not isinstance(value, (int, float)) or value < 0 for value in wait):
    raise SystemExit("invalid guest invalidation waitMsPerFrame")
for value in windows:
    if value.get("diagnosticOnly") is not True or value.get("fpsVerdictAllowed") is not False:
        raise SystemExit("causal window is not diagnostic-only with FPS verdict disabled")
coverage = windows[1]["hleClean"].get("committedSyncShare")
coverage_accepted = windows[1]["hleClean"].get("coverageAccepted") is True

baseline_wait = (wait[0] + wait[2]) / 2.0
baseline_fps = (surface[0]["fps"] + surface[2]["fps"]) / 2.0
wait_reduction = ((baseline_wait - wait[1]) * 100.0 / baseline_wait) if baseline_wait > 0 else float("-inf")
fps_improvement = ((surface[1]["fps"] - baseline_fps) * 100.0 / baseline_fps) if baseline_fps > 0 else float("-inf")
reasons = []
if not coverage_accepted or not isinstance(coverage, (int, float)) or coverage < minimum_sync_coverage:
    reasons.append("coverage-below-floor")
if wait_reduction < minimum_wait_reduction:
    reasons.append("wait-reduction-below-threshold")
accepted = not reasons

decision = {
    "schema": "lsx4-bloodborne-hle-clean-mechanism-decision/v2",
    "createdUtc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "validAba": True,
    "diagnosticOnly": True,
    "fpsVerdictAllowed": False,
    "mechanismEvidenceAccepted": accepted,
    "optimizationAccepted": False,
    "productionEligible": False,
    "causalInferenceBoundary": {
        "measuredConfiguration": "async-shader-retranslation-disabled",
        "productionConfiguration": "async-shader-retranslation-default-on",
        "productionTransferProven": False,
        "productionValidationRequired": True,
    },
    "reasons": reasons,
    "thresholds": {
        "minimumCommittedSyncShare": minimum_sync_coverage,
        "minimumGuestInvalidationWaitReductionPct": minimum_wait_reduction,
        "baselineReference": "mean(baseline-a1,baseline-a2)",
        "fpsVerdictAllowed": False,
    },
    "measurements": {
        phase: {
            "guestInvalidationWaitMsPerFrame": wait[index],
            "surfaceFps": surface[index]["fps"],
            "surfaceFrames": surface[index]["frames"],
            "surfaceSeconds": surface[index]["seconds"],
            "windowArtifact": f"{phase}/hle-clean-window.json",
            "windowArtifactSha256": sha256(window_paths[index]),
            "surfaceArtifact": f"{phase}/sf.txt",
            "surfaceArtifactSha256": sha256(sf_paths[index]),
            "processIdentity": {
                "pid": identities[index]["pid"],
                "starttime": identities[index]["procStatField22Starttime"],
            },
            "processIdentityArtifact": f"{phase}/process-identity.json",
            "processIdentityArtifactSha256": sha256(identity_paths[index]),
        }
        for index, phase in enumerate(phase_names)
    },
    "derived": {
        "baselineMeanGuestInvalidationWaitMsPerFrame": baseline_wait,
        "cleanBGuestInvalidationWaitReductionPct": wait_reduction if baseline_wait > 0 else None,
        "baselineMeanSurfaceFps": baseline_fps,
        "cleanBSurfaceFpsImprovementPct": fps_improvement if baseline_fps > 0 else None,
        "cleanBCommittedSyncShare": coverage,
        "surfaceFpsObservationOnly": True,
    },
}
with destination.open("x", encoding="utf-8", newline="\n") as stream:
    json.dump(decision, stream, indent=2, sort_keys=True)
    stream.write("\n")
    stream.flush()
    os.fsync(stream.fileno())

print(
    "VALID_ABA "
    f"baselineWaitMsPerFrame={baseline_wait:.6f} cleanWaitMsPerFrame={wait[1]:.6f} "
    f"baselineSfFps={baseline_fps:.6f} cleanSfFps={surface[1]['fps']:.6f}"
)
if accepted:
    print(
        "MECHANISM_EVIDENCE_ACCEPTED "
        f"waitReductionPct={wait_reduction:.6f} minimumWaitReductionPct={minimum_wait_reduction:.6f} "
        f"surfaceFpsObservationPct={fps_improvement:.6f} fpsVerdictAllowed=false "
        f"committedSyncShare={coverage:.6f} minimumCommittedSyncShare={minimum_sync_coverage:.6f} "
        "optimizationAccepted=false productionEligible=false productionTransfer=unproven"
    )
    raise SystemExit(0)
print(
    "MECHANISM_EVIDENCE_REJECTED "
    f"reasons={','.join(reasons)} waitReductionPct={wait_reduction:.6f} "
    f"minimumWaitReductionPct={minimum_wait_reduction:.6f} "
    f"surfaceFpsObservationPct={fps_improvement:.6f} fpsVerdictAllowed=false "
    f"committedSyncShare={coverage!r} minimumCommittedSyncShare={minimum_sync_coverage:.6f} "
    "optimizationAccepted=false productionEligible=false productionTransfer=unproven"
)
raise SystemExit(62)
PY
}

if [[ "$device" != "$expected_device" &&
      ! "$device" =~ ^adb-10AG263F1B001HM-[A-Za-z0-9]+[.]_adb-tls-connect[.]_tcp$ ]]; then
    fail "reason=wrong-device expected=$expected_device orStrictWirelessAlias=true actual=$device"
    exit 2
fi
if [[ ! "$window" =~ ^[0-9]+$ ]] || (( window < 12 )); then
    fail "reason=invalid-window value=$window minimum=12"
    exit 2
fi
if [[ ! "$warmup_seconds" =~ ^[0-9]+$ ]] || (( warmup_seconds < 5 )); then
    fail "reason=invalid-warmup value=$warmup_seconds minimum=5"
    exit 2
fi
if [[ ! "$requested_tag" =~ ^[A-Za-z0-9._-]+$ ]] || (( ${#requested_tag} > 48 )); then
    fail "reason=invalid-tag value=$requested_tag maxLength=48"
    exit 2
fi
if [[ "$save_seed_discovery_only" != 0 && "$save_seed_discovery_only" != 1 ]]; then
    fail "reason=invalid-boolean name=BB_SAVE_SEED_DISCOVERY_ONLY value=$save_seed_discovery_only"
    exit 2
fi
for threshold_spec in \
    "BB_HLE_MIN_WAIT_REDUCTION_PCT:$minimum_wait_reduction_pct"; do
    threshold_name="${threshold_spec%%:*}"
    threshold_value="${threshold_spec#*:}"
    if ! awk -v value="$threshold_value" \
        'BEGIN { exit !(value ~ /^[0-9]+([.][0-9]+)?$/ && value >= 0 && value <= 100) }'; then
        fail "reason=invalid-percentage name=$threshold_name value=$threshold_value range=0..100"
        exit 2
    fi
done
if [[ ! "$process_identity_timeout" =~ ^[0-9]+$ ]] ||
   (( process_identity_timeout < 60 || process_identity_timeout > 900 )); then
    fail "reason=invalid-process-identity-timeout value=$process_identity_timeout range=60..900"
    exit 2
fi
if [[ ! -f "$pmu_driver" ]]; then
    fail "reason=pmu-driver-missing path=$pmu_driver"
    exit 2
fi

cd "$repo_root"
initialize_run_directory
if [[ "$(adb_device get-state 2>/dev/null | tr -d '\r[:space:]')" != device ]]; then
    fail "reason=physical-vivo-unavailable serial=$device"
    exit 3
fi
if ! verify_vivo_device_identity; then
    exit 4
fi
device_session_started=1
if [[ "$save_seed_discovery_only" == 1 ]]; then
    force_stop_and_verify_no_app_pid discovery-only
    run_performance_preflight
    probe_save_seed_contract
    check_save_seed_free_space
    discovery_inventory=$(capture_save_inventory "$save_parent" "$save_leaf" \
        "$save_evidence_dir/discovery-files.sha256")
    echo "SAVE_SEED_DISCOVERY_ONLY_OK inventorySha256=$discovery_inventory noSaveMutation=true"
    exit 0
fi
prepare_save_seed
run_performance_preflight
# Fresh-process A/B/A. The only experimental input changes 0 -> 1 -> 0.
run_phase baseline-a1 0 0
run_phase clean-b 1 0
run_phase baseline-a2 0 1
active_mode=baseline

compare_phase_contracts
assert_experiment_markers_absent
assert_graphics_baseline
a2_live_pid=$(adb_device shell "pidof '$game_process' 2>/dev/null || true" 2>/dev/null | tr -d '\r[:space:]')
if [[ "$a2_live_pid" != "$baseline_a2_pid" ]]; then
    fail "reason=baseline-a2-not-running-before-decision expected=$baseline_a2_pid actual=$a2_live_pid"
    exit 61
fi
if evaluate_mechanism_evidence; then
    mechanism_evidence_accepted=1
    decision_status=0
else
    mechanism_evidence_accepted=0
    decision_status=$?
    reject_open_windows diagnostic-mechanism-thresholds-not-met
    (( decision_status == 0 )) && decision_status=62
fi
run_production_handoff
if (( mechanism_evidence_accepted == 0 )); then
    cleanup_complete=1
    echo "DONE runId=$run_id evidenceDir=$run_dir diagnosticOnly=true fpsVerdictAllowed=false mechanismEvidence=REJECTED optimization=NOT_EVALUATED productionEligible=false productionTransfer=unproven decision=$run_dir/decision.json finalHandoffPid=$final_handoff_pid finalHandoffStarttime=$final_handoff_starttime hleClean=0 diagnosticStats=0 asyncShaderRetranslation=default-on markerClean=true allSix=OFF finalGameRunning=true exactGameplay=true exactGame=$game" >&2
    exit "$decision_status"
fi
terminal_window baseline-a1 accepted diagnostic-mechanism-and-final-contracts-passed
terminal_window clean-b accepted diagnostic-mechanism-and-final-contracts-passed
terminal_window baseline-a2 accepted diagnostic-mechanism-and-final-contracts-passed

cleanup_complete=1
echo "DONE runId=$run_id evidenceDir=$run_dir diagnosticOnly=true fpsVerdictAllowed=false mechanismEvidence=ACCEPTED optimization=NOT_EVALUATED productionEligible=false productionTransfer=unproven sequence=baseline-a1:0,clean-b:1,baseline-a2:0 causalPids=$baseline_a1_pid,$clean_b_pid,$baseline_a2_pid finalHandoffPid=$final_handoff_pid finalHandoffStarttime=$final_handoff_starttime hleClean=0 diagnosticStats=0 asyncShaderRetranslation=default-on markerClean=true allSix=OFF ADPF=25ms Vivo=4241 finalMode=production-baseline finalGameRunning=true exactGameplay=true exactGame=$game"
