#!/usr/bin/env bash

set -Eeuo pipefail

# Git Bash must not rewrite Android absolute paths passed through adb.
export MSYS2_ARG_CONV_EXCL='*'

readonly expected_device='10AG263F1B001HM'
readonly device="${ADB_SERIAL:-$expected_device}"
readonly package='app.lsx4.android'
readonly game_process='app.lsx4.android:game'
readonly game='/data/user/0/app.lsx4.android/files/lsx4-home/runtime-fs/user/app/CUSA03173/eboot.bin'
readonly runtime_home='files/lsx4-home'
readonly relaxed_memcpy_marker="$runtime_home/run-enable-fex-relaxed-memcpyset-tso-ab"
readonly agentchat_py='D:/LProj/PetProj/agentchat/agentchat.py'
readonly room='увеличить fps'
readonly window="${1:-45}"
readonly tag="${2:-fex-memory-ab-r85}"

readonly script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly repo_root="$(cd "$script_dir/.." && pwd)"
readonly workspace_root="$(cd "$repo_root/.." && pwd)"
readonly output_dir="${BB_FEX_MEMORY_AB_OUTPUT_DIR:-$repo_root}"
readonly manifest="$workspace_root/.fex-2608-artifacts/android-api26-arm64/manifest.json"
readonly verifier="$repo_root/tools/fex/verify_android_fex_artifacts.py"

phase_logcat_pid=''
active_process_flags='-1'
last_pid=''
baseline_a1_pid=''
relaxed_b_pid=''
baseline_a2_pid=''
cleanup_complete=0
cleanup_running=0
device_session_started=0
launched_pid=''

# These are performance/correctness experiments, not the six known-good
# Bloodborne effect-disable markers. Exactly one entry in this list may exist:
# the phase-selected MemcpySetTSO A/B marker.
readonly unsafe_marker_paths=(
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

declare -A local_artifact_path
declare -A local_artifact_sha
declare -A local_manifest_sha

fail() {
    echo "FEX_MEMORY_AB_ERROR $*" >&2
    return 1
}

host_path() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -m "$1"
    else
        printf '%s\n' "$1"
    fi
}

artifact_file() {
    local phase="$1"
    local suffix="$2"
    printf '%s/.codex-bb-%s-%s-%s\n' "$output_dir" "$tag" "$phase" "$suffix"
}

guard_device_action() {
    local action="${1:-adb}"
    python3 "$agentchat_py" event --agent codex --room "$room" \
        --action "$action" --target 'device:vivo' \
        --summary "Bloodborne FEX memory A/B/A: $action" >/dev/null || return 1
    python3 "$agentchat_py" own --agent codex --room "$room" \
        --area 'device:vivo' >/dev/null
}

adb_device() {
    local subcommand="${1:-unknown}"
    guard_device_action "adb-$subcommand" || return 1
    adb -s "$device" "$@"
}

stop_phase_logcat() {
    if [[ -n "$phase_logcat_pid" ]]; then
        kill "$phase_logcat_pid" >/dev/null 2>&1 || true
        wait "$phase_logcat_pid" >/dev/null 2>&1 || true
        phase_logcat_pid=''
    fi
}

start_phase_logcat() {
    local phase="$1"
    local phase_log
    phase_log="$(artifact_file "$phase" 'logcat.txt')"
    if ! adb_device logcat -c >/dev/null; then
        fail "phase=$phase reason=logcat-clear-failed"
        return 1
    fi
    adb_device logcat -v threadtime > "$phase_log" 2>&1 &
    phase_logcat_pid=$!
    sleep 1
    if ! kill -0 "$phase_logcat_pid" >/dev/null 2>&1; then
        wait "$phase_logcat_pid" >/dev/null 2>&1 || true
        phase_logcat_pid=''
        fail "phase=$phase reason=logcat-reader-exited"
        return 1
    fi
}

stop_game() {
    adb_device shell am force-stop "$package" >/dev/null
    sleep 1
    local remaining
    remaining="$(adb_device shell "pidof $game_process" 2>/dev/null | tr -d '\r[:space:]' || true)"
    if [[ -n "$remaining" ]]; then
        fail "reason=force-stop-did-not-remove-game pid=$remaining"
        return 1
    fi
    active_process_flags='-1'
}

remove_unsafe_markers() {
    if ! adb_device shell "run-as $package mkdir -p $runtime_home" >/dev/null; then
        fail 'reason=runtime-home-create-failed'
        return 1
    fi
    if ! adb_device shell "run-as $package rm -f ${unsafe_marker_paths[*]}" >/dev/null; then
        fail 'reason=unsafe-marker-remove-failed'
        return 1
    fi
}

active_unsafe_markers() {
    local probe=''
    local path
    for path in "${unsafe_marker_paths[@]}"; do
        probe+="if [ -f $path ]; then echo $path; elif [ -e $path ]; then echo NONREGULAR:$path; fi; "
    done
    adb_device shell "run-as $package sh -c '$probe'" 2>/dev/null | tr -d '\r'
}

assert_unsafe_marker_state() {
    local expected="${1:-}"
    local active
    active="$(active_unsafe_markers)" || {
        fail 'reason=unsafe-marker-probe-failed'
        return 1
    }
    if [[ "$active" != "$expected" ]]; then
        fail "reason=unsafe-marker-state expected=${expected:-none} actual=${active//$'\n'/,}"
        return 1
    fi
}

set_phase_marker_state() {
    local selected="${1:-}"
    remove_unsafe_markers || return 1
    if [[ -n "$selected" ]]; then
        if [[ "$selected" != "$relaxed_memcpy_marker" ]]; then
            fail "reason=unsupported-selected-marker marker=$selected"
            return 1
        fi
        if ! adb_device shell "run-as $package touch $selected" >/dev/null; then
            fail "reason=selected-marker-create-failed marker=$selected"
            return 1
        fi
    fi
    assert_unsafe_marker_state "$selected"
}

assert_graphics_baseline() {
    local expression=''
    local marker
    for marker in "${graphics_baseline_markers[@]}"; do
        expression+="test -f $marker && "
    done
    expression+='true'
    if ! adb_device shell "run-as $package sh -c '$expression'" >/dev/null 2>&1; then
        fail 'reason=graphics-baseline-markers-not-all-active'
        return 1
    fi
}

assert_root_boost_disabled() {
    local preferences
    preferences="$(adb_device exec-out run-as "$package" \
        cat shared_prefs/lsx4_settings.xml 2>/dev/null || true)"
    if grep -Eq 'name="managed_oem_root_performance"[[:space:]]+value="true"' \
            <<< "$preferences"; then
        fail 'reason=managed-oem-root-performance-enabled'
        return 1
    fi
}

launch_through_handoff() {
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

launch_fresh_process() {
    local phase="$1"
    local expected_flags="$2"
    local attempt pid=''
    launched_pid=''
    for attempt in 1 2 3; do
        launch_through_handoff
        sleep 5
        pid="$(adb_device shell "pidof $game_process" 2>/dev/null | tr -d '\r[:space:]' || true)"
        if [[ -n "$pid" ]]; then
            break
        fi
        echo "HANDOFF_RETRY phase=$phase attempt=$attempt" >&2
    done
    if [[ -z "$pid" ]]; then
        fail "phase=$phase reason=no-game-process-after-handoff"
        return 1
    fi
    if [[ -n "$last_pid" && "$pid" == "$last_pid" ]]; then
        fail "phase=$phase reason=pid-not-fresh previous=$last_pid actual=$pid"
        return 1
    fi
    active_process_flags="$expected_flags"
    launched_pid="$pid"
}

navigate_to_saved_static_scene() {
    local phase="$1"
    local start_time
    start_time="$(date +%s)"
    sleep "${BB_WARNING_WAIT:-64}"
    adb_device shell 'input swipe 2903 1231 2903 1231 1000' >/dev/null
    echo "CROSS_WARNING phase=$phase t=$(( $(date +%s) - start_time ))"
    sleep "${BB_OFFLINE_WAIT:-30}"
    adb_device shell 'input swipe 2903 1231 2903 1231 1000' >/dev/null
    echo "CROSS_OFFLINE phase=$phase t=$(( $(date +%s) - start_time ))"
    sleep "${BB_CONTINUE_WAIT:-22}"
    adb_device shell 'input swipe 2903 1231 2903 1231 1000' >/dev/null
    echo "CROSS_CONTINUE phase=$phase t=$(( $(date +%s) - start_time ))"
    sleep "${BB_DOOR_WAIT:-65}"
    adb_device shell 'input swipe 2903 1231 2903 1231 1000' >/dev/null
    echo "CROSS_DOOR phase=$phase t=$(( $(date +%s) - start_time ))"
    sleep "${BB_DOOR_SETTLE_WAIT:-5}"

    # The same deterministic move is made before every fresh process is
    # sampled. The measurement itself is entirely static.
    local position_seconds="${BB_POSITION_WINDOW:-30}"
    adb_device shell "input swipe 950 1245 950 1060 $((position_seconds * 1000))" \
        >/dev/null 2>&1
    sleep "${BB_POSITION_SETTLE_WAIT:-5}"
    echo "STATIC_SCENE_POSITIONED phase=$phase positionSeconds=$position_seconds"

    local warmup_seconds="${BB_FEX_MEMORY_AB_WARMUP_WAIT:-20}"
    sleep "$warmup_seconds"
    echo "WARMUP_DONE phase=$phase seconds=$warmup_seconds"
}

assert_pid_and_foreground() {
    local phase="$1"
    local expected_pid="$2"
    local state_file current_pid
    state_file="$(artifact_file "$phase" 'foreground.txt')"
    adb_device shell "pidof $game_process
        dumpsys window | grep -m1 -E 'mCurrentFocus=.*GameActivity|mFocusedApp=.*GameActivity'
        dumpsys activity activities | grep -m1 -E 'topResumedActivity=.*GameActivity|mResumedActivity=.*GameActivity'" \
        2>/dev/null | tr -d '\r' > "$state_file"
    current_pid="$(head -n1 "$state_file" | tr -d '[:space:]')"
    if [[ "$current_pid" != "$expected_pid" ]]; then
        fail "phase=$phase reason=pid-changed expected=$expected_pid actual=$current_pid"
        return 1
    fi
    if ! grep -Eq 'app\.lsx4\.android(/app\.lsx4\.android|/)\.GameActivity' "$state_file"; then
        fail "phase=$phase reason=game-not-foreground"
        return 1
    fi
}

assert_gameplay_identity() {
    local phase="$1"
    local phase_log
    phase_log="$(artifact_file "$phase" 'logcat.txt')"
    if ! grep -q 'gameState loading=false gameplay=true' "$phase_log" ||
       ! grep -q 'serial=CUSA03173' "$phase_log" ||
       ! grep -q 'target=960x544' "$phase_log"; then
        fail "phase=$phase reason=exact-gameplay-identity-not-observed title=CUSA03173 target=960x544"
        return 1
    fi
    local live_samples
    live_samples="$(grep -c '\[EXECUTOR_LIVE_FPS\]' "$phase_log" || true)"
    if (( live_samples < ${BB_MIN_WARM_LIVE_SAMPLES:-8} )); then
        fail "phase=$phase reason=warmup-insufficient liveSamples=$live_samples"
        return 1
    fi
}

assert_platform_policy_contract() {
    local phase="$1"
    local phase_log
    phase_log="$(artifact_file "$phase" 'logcat.txt')"
    if grep -Eq 'rootBoost .*applied=true' "$phase_log"; then
        fail "phase=$phase reason=confounding-root-boost-applied"
        return 1
    fi
    if ! grep -Fq '[EXECUTOR_ADPF_TARGET] aggressive40Hz=1 targetNs=25000000' \
            "$phase_log"; then
        fail "phase=$phase reason=default-adpf-25ms-policy-not-observed"
        return 1
    fi
    if ! grep -Eq 'vivo mode=performance-hint-4241 transport=binder result=(acquired|renewed)' \
            "$phase_log"; then
        fail "phase=$phase reason=default-vivo-4241-policy-not-observed"
        return 1
    fi
    if grep -Eq 'vivo mode=moderate-cpu-floor .*result=(acquired|renewed)|result=(acquire-failed|renewal-failed|release-failed|fallback-acquired|release-retry-exhausted)' \
            "$phase_log"; then
        fail "phase=$phase reason=platform-policy-failure-or-confound"
        return 1
    fi
}

pull_backend_status() {
    local phase="$1"
    local status_file
    status_file="$(artifact_file "$phase" 'guest-cpu-backend-status.json')"
    if ! adb_device exec-out run-as "$package" cat files/guest-cpu-backend-status.json \
            > "$status_file" 2>/dev/null; then
        fail "phase=$phase reason=backend-status-pull-failed"
        return 1
    fi
    if [[ ! -s "$status_file" ]]; then
        fail "phase=$phase reason=backend-status-empty"
        return 1
    fi
}

validate_backend_status() {
    local phase="$1"
    local expected_flags="$2"
    local expected_suffix="$3"
    local expected_model="$4"
    local status_file meta_file
    status_file="$(artifact_file "$phase" 'guest-cpu-backend-status.json')"
    meta_file="$(artifact_file "$phase" 'backend-provenance.txt')"
    python3 - "$(host_path "$status_file")" "$expected_flags" "$expected_suffix" \
        "$expected_model" > "$meta_file" <<'PY'
import json
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
expected_flags = int(sys.argv[2])
expected_suffix = sys.argv[3]
expected_model = sys.argv[4]
status = json.loads(path.read_text(encoding="utf-8"))

def exact_int(name):
    value = status.get(name)
    if type(value) is not int:
        raise SystemExit(f"{name} must be an exact JSON integer, got {value!r}")
    return value

if status.get("effective") != "fex-2608-experimental":
    raise SystemExit(f"effective backend mismatch: {status.get('effective')!r}")
if status.get("supported") is not True or status.get("fallbackReason") != "none":
    raise SystemExit(
        f"FEX backend rejected/fell back: supported={status.get('supported')!r} "
        f"fallback={status.get('fallbackReason')!r}"
    )
flags = exact_int("configureFlags")
provided = exact_int("providedCapabilities")
required = exact_int("requiredCapabilities")
forbidden = exact_int("forbiddenCapabilities")
if flags != expected_flags:
    raise SystemExit(f"configureFlags mismatch: expected {expected_flags}, got {flags}")
strong_cap = 1 << 2
ab_override_cap = 1 << 12
if (provided & ab_override_cap) == 0:
    raise SystemExit(f"capability bit12 absent: provided=0x{provided:x}")
if expected_flags == 0:
    if (provided & strong_cap) == 0 or (required & strong_cap) == 0:
        raise SystemExit(
            f"baseline capability bit2 absent: provided=0x{provided:x} required=0x{required:x}"
        )
    if forbidden & strong_cap:
        raise SystemExit(f"baseline unexpectedly forbids capability bit2: 0x{forbidden:x}")
elif expected_flags == 2:
    if provided & strong_cap or required & strong_cap:
        raise SystemExit(
            f"relaxed phase still advertises/requires bit2: provided=0x{provided:x} required=0x{required:x}"
        )
    if (forbidden & strong_cap) == 0:
        raise SystemExit(f"relaxed phase does not forbid bit2: 0x{forbidden:x}")
    if (required & ab_override_cap) == 0:
        raise SystemExit(f"relaxed phase does not require bit12: 0x{required:x}")
else:
    raise SystemExit(f"unsupported expected flags {expected_flags}")
model = status.get("strongMemoryModel")
if model != expected_model:
    raise SystemExit(f"strongMemoryModel mismatch: expected {expected_model!r}, got {model!r}")
cache = status.get("cacheNamespace")
if not isinstance(cache, str) or not cache.endswith(expected_suffix):
    raise SystemExit(
        f"cache namespace suffix mismatch: expected {expected_suffix!r}, got {cache!r}"
    )
cache_base = cache[: -len(expected_suffix)] if expected_suffix else cache
if not cache_base.startswith("fex-2608-ps4-v3-"):
    raise SystemExit(f"unexpected cache namespace base: {cache_base!r}")
print("effective=fex-2608-experimental")
print(f"configureFlags={flags}")
print(f"providedCapabilities=0x{provided:x}")
print(f"requiredCapabilities=0x{required:x}")
print(f"forbiddenCapabilities=0x{forbidden:x}")
print(f"strongMemoryModel={model}")
print(f"cacheNamespace={cache}")
print(f"cacheBase={cache_base}")
print(f"cacheSuffix={expected_suffix}")
print("cacheSuffixRole=provenance-only")
PY
}

assert_memory_ab_log_contract() {
    local phase="$1"
    local expected_mode="$2"
    local expected_hex_flags="$3"
    local expected_strong="$4"
    local phase_log expected_line ab_lines
    phase_log="$(artifact_file "$phase" 'logcat.txt')"
    expected_line="[EXECUTOR_FEX_MEMORY_MODEL_AB] mode=$expected_mode flags=$expected_hex_flags strongExpected=$expected_strong"
    if ! grep -Fq "$expected_line" "$phase_log"; then
        fail "phase=$phase reason=memory-ab-log-missing expected=$expected_line"
        return 1
    fi
    ab_lines="$(grep -F '[EXECUTOR_FEX_MEMORY_MODEL_AB]' "$phase_log" || true)"
    if grep -Fv "$expected_line" <<< "$ab_lines" | grep -q .; then
        fail "phase=$phase reason=conflicting-memory-ab-log"
        return 1
    fi
    # The force-software marker is deliberately forbidden by this experiment.
    # The actual effective software model is proven by the exact cache suffix.
    if ! grep -Fq '[EXECUTOR_FEX_STRONG_MEMORY_PREFLIGHT] forceSoftware=0' "$phase_log"; then
        fail "phase=$phase reason=unexpected-force-software-policy"
        return 1
    fi
    if ! grep -Fq '[EXECUTOR_FEX_LOOKUP_CACHE] requestedLowMemory=0 effectiveLowMemory=0 dynamicL1=0 disableL2=0' \
            "$phase_log"; then
        fail "phase=$phase reason=lookup-cache-policy-not-throughput-default"
        return 1
    fi
}

manifest_value() {
    local role="$1"
    local field="$2"
    python3 -c 'import json, pathlib, sys; value=json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))["artifacts"][sys.argv[2]][sys.argv[3]]; print(pathlib.Path(value).resolve().as_posix() if sys.argv[3] == "path" else value)' \
        "$(host_path "$manifest")" "$role" "$field"
}

sha256_local() {
    sha256sum "$1" | awk '{print $1}'
}

verify_host_artifacts() {
    if [[ ! -f "$manifest" || ! -f "$verifier" ]]; then
        fail "reason=canonical-fex-manifest-missing path=$manifest"
        return 1
    fi
    if ! python3 "$(host_path "$verifier")" --manifest "$(host_path "$manifest")"; then
        fail 'reason=canonical-fex-manifest-verification-failed'
        return 1
    fi

    local_artifact_path[executor]="$repo_root/build/android-arm64-release/liblsx4_executor_android.so"
    local_artifact_path[adapter]="$(manifest_value adapter path)"
    local_artifact_path[bridge]="$(manifest_value bridge path)"
    local_artifact_path[fexcore]="$(manifest_value fexcore path)"
    local_manifest_sha[adapter]="$(manifest_value adapter sha256)"
    local_manifest_sha[bridge]="$(manifest_value bridge sha256)"
    local_manifest_sha[fexcore]="$(manifest_value fexcore sha256)"

    local role expected_name
    for role in executor adapter bridge fexcore; do
        if [[ ! -f "${local_artifact_path[$role]}" ]]; then
            fail "reason=canonical-artifact-missing role=$role path=${local_artifact_path[$role]}"
            return 1
        fi
        local_artifact_sha[$role]="$(sha256_local "${local_artifact_path[$role]}")"
        if [[ "$role" != executor &&
              "${local_artifact_sha[$role]}" != "${local_manifest_sha[$role]}" ]]; then
            fail "reason=canonical-manifest-hash-mismatch role=$role manifest=${local_manifest_sha[$role]} actual=${local_artifact_sha[$role]}"
            return 1
        fi
    done
    for role in executor adapter bridge fexcore; do
        case "$role" in
        executor) expected_name='liblsx4_executor_android.so' ;;
        adapter) expected_name='liblsx4_fex_backend_v3.so' ;;
        bridge) expected_name='liblsx4_fex2608_context_bridge.so' ;;
        fexcore) expected_name='libFEXCore.so' ;;
        esac
        if [[ "${local_artifact_path[$role]##*/}" != "$expected_name" ]]; then
            fail "reason=canonical-artifact-name-mismatch role=$role expected=$expected_name actual=${local_artifact_path[$role]##*/}"
            return 1
        fi
    done
    echo "HOST_ARTIFACTS_VERIFIED executor=${local_artifact_sha[executor]} adapter=${local_artifact_sha[adapter]} bridge=${local_artifact_sha[bridge]} fexcore=${local_artifact_sha[fexcore]}"
}

verify_loaded_artifacts() {
    local phase="$1"
    local pid="$2"
    local maps_file provenance_file
    maps_file="$(artifact_file "$phase" 'maps.txt')"
    provenance_file="$(artifact_file "$phase" 'loaded-artifacts.txt')"
    if ! adb_device shell "run-as $package cat /proc/$pid/maps" \
            2>/dev/null | tr -d '\r' > "$maps_file"; then
        fail "phase=$phase reason=maps-read-failed pid=$pid"
        return 1
    fi
    : > "$provenance_file"
    local role basename loaded_path loaded_sha
    for role in executor adapter bridge fexcore; do
        case "$role" in
        executor) basename='liblsx4_executor_android.so' ;;
        adapter) basename='liblsx4_fex_backend_v3.so' ;;
        bridge) basename='liblsx4_fex2608_context_bridge.so' ;;
        fexcore) basename='libFEXCore.so' ;;
        esac
        loaded_path="$(awk -v name="$basename" 'index($0, name) {print $6; exit}' "$maps_file")"
        if [[ -z "$loaded_path" ]]; then
            fail "phase=$phase reason=loaded-artifact-not-mapped role=$role name=$basename"
            return 1
        fi
        loaded_sha="$(adb_device shell "run-as $package sha256sum '$loaded_path'" \
            2>/dev/null | tr -d '\r' | awk '{print $1}')"
        if [[ -z "$loaded_sha" || "$loaded_sha" != "${local_artifact_sha[$role]}" ]]; then
            fail "phase=$phase reason=loaded-artifact-hash-mismatch role=$role loaded=$loaded_sha local=${local_artifact_sha[$role]} path=$loaded_path"
            return 1
        fi
        printf 'role=%s loadedPath=%s loadedSha256=%s localPath=%s localSha256=%s\n' \
            "$role" "$loaded_path" "$loaded_sha" "${local_artifact_path[$role]}" \
            "${local_artifact_sha[$role]}" >> "$provenance_file"
    done
}

write_png_metrics() {
    local image="$1"
    local metrics="$2"
    python3 - "$(host_path "$image")" "$(host_path "$metrics")" <<'PY'
import json
import math
import pathlib
import struct
import sys
import zlib

source = pathlib.Path(sys.argv[1])
destination = pathlib.Path(sys.argv[2])
data = source.read_bytes()
if not data.startswith(b"\x89PNG\r\n\x1a\n"):
    raise SystemExit("screenshot is not a PNG")
offset = 8
idat = []
width = height = bit_depth = color_type = None
while offset + 12 <= len(data):
    length = struct.unpack(">I", data[offset : offset + 4])[0]
    kind = data[offset + 4 : offset + 8]
    payload = data[offset + 8 : offset + 8 + length]
    offset += 12 + length
    if kind == b"IHDR":
        width, height, bit_depth, color_type, compression, filtering, interlace = struct.unpack(
            ">IIBBBBB", payload
        )
        if compression != 0 or filtering != 0 or interlace != 0:
            raise SystemExit("unsupported PNG encoding")
    elif kind == b"IDAT":
        idat.append(payload)
    elif kind == b"IEND":
        break
if width is None or not idat:
    raise SystemExit("PNG is missing IHDR/IDAT")
if bit_depth != 8 or color_type not in (2, 6):
    raise SystemExit(f"unsupported screenshot PNG type depth={bit_depth} type={color_type}")
channels = 3 if color_type == 2 else 4
stride = width * channels
raw = zlib.decompress(b"".join(idat))
if len(raw) != height * (stride + 1):
    raise SystemExit("PNG decompressed size mismatch")

previous = bytearray(stride)
rows = []
cursor = 0
for _ in range(height):
    filter_type = raw[cursor]
    cursor += 1
    encoded = raw[cursor : cursor + stride]
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
            p = left + up - up_left
            pa, pb, pc = abs(p - left), abs(p - up), abs(p - up_left)
            predictor = left if pa <= pb and pa <= pc else (up if pb <= pc else up_left)
        else:
            raise SystemExit(f"unsupported PNG filter {filter_type}")
        row[index] = (value + predictor) & 0xFF
    rows.append(row)
    previous = row

sample_step = max(1, (width * height) // 250000)
histogram = [0] * 16
total = total_sq = 0.0
black = white = samples = 0
linear = 0
for row in rows:
    for offset in range(0, stride, channels):
        if linear % sample_step == 0:
            r, g, b = row[offset], row[offset + 1], row[offset + 2]
            luma = (54 * r + 183 * g + 19 * b) / 256.0
            histogram[min(15, int(luma) // 16)] += 1
            total += luma
            total_sq += luma * luma
            black += luma < 12
            white += luma > 245
            samples += 1
        linear += 1
if width < 1000 or height < 500 or samples < 10000:
    raise SystemExit(f"implausible screenshot geometry/samples: {width}x{height} n={samples}")
mean = total / samples
variance = max(0.0, total_sq / samples - mean * mean)
stddev = math.sqrt(variance)
if stddev < 5.0 or black / samples > 0.985 or white / samples > 0.95:
    raise SystemExit(
        f"blank/corrupt graphics gate: mean={mean:.3f} stddev={stddev:.3f} "
        f"black={black / samples:.6f} white={white / samples:.6f}"
    )
metrics = {
    "width": width,
    "height": height,
    "samples": samples,
    "mean_luma": mean,
    "stddev_luma": stddev,
    "black_fraction": black / samples,
    "white_fraction": white / samples,
    "histogram": [value / samples for value in histogram],
}
destination.write_text(json.dumps(metrics, sort_keys=True) + "\n", encoding="utf-8")
print(
    f"GRAPHICS_PNG_OK path={source.name} size={width}x{height} "
    f"mean={mean:.3f} stddev={stddev:.3f} black={black / samples:.6f} "
    f"white={white / samples:.6f}"
)
PY
}

capture_and_validate_screenshot() {
    local phase="$1"
    local point="$2"
    local image metrics
    image="$(artifact_file "$phase" "$point.png")"
    metrics="$(artifact_file "$phase" "$point-graphics.json")"
    if ! adb_device exec-out screencap -p > "$image"; then
        fail "phase=$phase reason=screenshot-capture-failed point=$point"
        return 1
    fi
    write_png_metrics "$image" "$metrics"
}

annotation() {
    local phase="$1"
    local point="$2"
    adb_device shell 'for z in /sys/class/thermal/thermal_zone*; do
        if [ "$(cat "$z/type" 2>/dev/null)" = tz_shell ]; then
            echo "skin=$(cat "$z/temp")"
            break
        fi
    done
    dumpsys thermalservice | grep -m1 "Thermal Status"
    for cpu in 0 6 7; do
        base=/sys/devices/system/cpu/cpu${cpu}/cpufreq
        echo "cpu${cpu}_cur=$(cat "$base/scaling_cur_freq" 2>/dev/null)"
        echo "cpu${cpu}_min=$(cat "$base/scaling_min_freq" 2>/dev/null)"
        echo "cpu${cpu}_max=$(cat "$base/scaling_max_freq" 2>/dev/null)"
    done
    gpu=/sys/class/kgsl/kgsl-3d0/devfreq
    echo "gpu_cur=$(cat "$gpu/cur_freq" 2>/dev/null)"
    echo "gpu_min=$(cat "$gpu/min_freq" 2>/dev/null)"
    echo "gpu_max=$(cat "$gpu/max_freq" 2>/dev/null)"
    echo "gpu_busy=$(cat /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage 2>/dev/null)"' \
        > "$(artifact_file "$phase" "$point-annotation.txt")" 2>/dev/null
}

game_surface_frames_from_file() {
    awk '
        /^layerName = / {
            in_game = ($0 ~ /SurfaceView\[app\.lsx4\.android\/app\.lsx4\.android\.GameActivity\]\(BLAST\)/)
        }
        in_game && /^totalFrames = / { frames += $3 }
        END { print frames + 0 }
    ' "$1"
}

game_dropped_frames_from_file() {
    awk '
        /^layerName = / {
            in_game = ($0 ~ /SurfaceView\[app\.lsx4\.android\/app\.lsx4\.android\.GameActivity\]\(BLAST\)/)
        }
        in_game && /^droppedFrames = / { frames += $3 }
        END { print frames + 0 }
    ' "$1"
}

write_builder_scene_metrics() {
    local phase="$1"
    local measure_log metrics_file
    measure_log="$(artifact_file "$phase" 'measure-logcat.txt')"
    metrics_file="$(artifact_file "$phase" 'scene.txt')"
    if ! awk '
        /\[EXECUTOR_LIVE_FPS\]/ && /builderTotal=/ {
            frames = total = draws = shaders = ""
            for (i = 1; i <= NF; ++i) {
                if ($i ~ /^frames=/) { split($i, a, "="); frames = a[2] + 0 }
                if ($i ~ /^builderTotal=/) { split($i, a, "="); total = a[2] + 0 }
                if ($i ~ /^builderDraw=/) { split($i, a, "="); draws = a[2] + 0 }
                if ($i ~ /^builderShader=/) { split($i, a, "="); shaders = a[2] + 0 }
            }
            if (frames != "" && total != "") {
                if (samples == 0) {
                    first_frames = frames
                    first_total = total
                    first_draws = draws
                    first_shaders = shaders
                }
                last_frames = frames
                last_total = total
                last_draws = draws
                last_shaders = shaders
                ++samples
            }
        }
        END {
            frame_delta = last_frames - first_frames
            total_delta = last_total - first_total
            draw_delta = last_draws - first_draws
            shader_delta = last_shaders - first_shaders
            if (samples < 2 || frame_delta <= 0 || total_delta <= 0) exit 2
            printf "samples=%d nativeFrames=%d builderTotal=%d builderDraw=%d builderShader=%d builderUnitsPerFrame=%.6f builderDrawPerFrame=%.6f builderShaderPerFrame=%.6f\n",
                samples, frame_delta, total_delta, draw_delta, shader_delta,
                total_delta / frame_delta, draw_delta / frame_delta,
                shader_delta / frame_delta
        }
    ' "$measure_log" > "$metrics_file"; then
        fail "phase=$phase reason=no-gameplay-builder-work"
        return 1
    fi
    local units
    units="$(awk '{for(i=1;i<=NF;i++) if($i ~ /^builderUnitsPerFrame=/) {split($i,a,"="); print a[2]}}' "$metrics_file")"
    if ! awk -v value="$units" -v minimum="${BB_MIN_BUILDER_UNITS_PER_FRAME:-400}" \
            'BEGIN { exit !(value + 0 >= minimum + 0) }'; then
        fail "phase=$phase reason=light-or-menu-workload builderUnitsPerFrame=$units"
        return 1
    fi
}

assert_no_crash_freeze_log() {
    local phase="$1"
    local phase_log
    phase_log="$(artifact_file "$phase" 'logcat.txt')"
    local fatal_pattern='FATAL EXCEPTION|Fatal signal|signal [0-9]+ \(SIG(BUS|SEGV)\)|am_crash|am_anr|ANR in|Application Not Responding|fex-activation-(failed|stranded)|fallback=current'
    if grep -Eqi "$fatal_pattern" \
            "$phase_log"; then
        grep -Ei "$fatal_pattern" \
            "$phase_log" | tail -40 >&2 || true
        fail "phase=$phase reason=crash-anr-or-fallback-log"
        return 1
    fi
}

measure_static_phase() {
    local phase="$1"
    local pid="$2"
    local phase_log pmu_out pmu_err sf_file sf_tail window_file
    phase_log="$(artifact_file "$phase" 'logcat.txt')"
    pmu_out="$(artifact_file "$phase" 'pmu.out')"
    pmu_err="$(artifact_file "$phase" 'pmu.err')"
    sf_file="$(artifact_file "$phase" 'sf.txt')"
    sf_tail="$(artifact_file "$phase" 'sf-tail.txt')"
    window_file="$(artifact_file "$phase" 'window.txt')"

    assert_pid_and_foreground "$phase" "$pid"
    capture_and_validate_screenshot "$phase" 'static-start'
    annotation "$phase" 'start'

    if ! adb_device shell 'dumpsys SurfaceFlinger --timestats -enable' >/dev/null 2>&1 ||
       ! adb_device shell 'dumpsys SurfaceFlinger --timestats -clear' >/dev/null 2>&1; then
        fail "phase=$phase reason=surfaceflinger-timestats-setup-failed"
        return 1
    fi

    local phase_log_start wall_start wall_end
    phase_log_start="$(wc -l < "$phase_log")"
    wall_start="$(date +%s%3N)"
    if ! adb_device shell "run-as $package /system/bin/simpleperf stat -p $pid \
        --duration $window -e instructions:u,cpu-cycles:u,task-clock,context-switches,cpu-migrations,page-faults" \
        > "$pmu_out" 2> "$pmu_err"; then
        fail "phase=$phase reason=simpleperf-command-failed"
        return 1
    fi
    wall_end="$(date +%s%3N)"
    if ! grep -q 'Total test time:' "$pmu_out" ||
       grep -Eqi '<not counted>|not supported|permission denied' "$pmu_out" "$pmu_err"; then
        fail "phase=$phase reason=pmu-coverage-invalid"
        return 1
    fi

    if ! adb_device shell 'dumpsys SurfaceFlinger --timestats -dump --maxlayers 8' \
            > "$sf_file" 2>/dev/null; then
        fail "phase=$phase reason=surfaceflinger-dump-failed"
        return 1
    fi
    local frames dropped sf_start sf_end sf_seconds minimum_frames
    frames="$(game_surface_frames_from_file "$sf_file")"
    dropped="$(game_dropped_frames_from_file "$sf_file")"
    sf_start="$(awk '/^statsStart = / {print $3; exit}' "$sf_file")"
    sf_end="$(awk '/^statsEnd = / {print $3; exit}' "$sf_file")"
    if [[ -z "$sf_start" || -z "$sf_end" ]]; then
        fail "phase=$phase reason=surfaceflinger-window-missing"
        return 1
    fi
    sf_seconds=$((sf_end - sf_start))
    if (( sf_seconds < window - 3 || sf_seconds > window + 8 )); then
        fail "phase=$phase reason=surfaceflinger-window-invalid seconds=$sf_seconds expected=$window"
        return 1
    fi
    minimum_frames=$((window * ${BB_MIN_PHASE_FPS:-5}))
    if (( frames < minimum_frames )); then
        fail "phase=$phase reason=surface-frame-count-too-low frames=$frames minimum=$minimum_frames"
        return 1
    fi
    if (( dropped != 0 )); then
        fail "phase=$phase reason=surfaceflinger-dropped-frames count=$dropped"
        return 1
    fi

    local tail_seconds="${BB_LIVENESS_TAIL_SECONDS:-3}"
    sleep "$tail_seconds"
    if ! adb_device shell 'dumpsys SurfaceFlinger --timestats -dump --maxlayers 8' \
            > "$sf_tail" 2>/dev/null; then
        fail "phase=$phase reason=surfaceflinger-tail-dump-failed"
        return 1
    fi
    local tail_frames
    tail_frames="$(game_surface_frames_from_file "$sf_tail")"
    if (( tail_frames <= frames )); then
        fail "phase=$phase reason=zero-fps-frozen-tail before=$frames after=$tail_frames"
        return 1
    fi

    sleep 1
    tail -n "+$((phase_log_start + 1))" "$phase_log" \
        > "$(artifact_file "$phase" 'measure-logcat.txt')"
    write_builder_scene_metrics "$phase"
    capture_and_validate_screenshot "$phase" 'static-end'
    annotation "$phase" 'end'
    assert_pid_and_foreground "$phase" "$pid"
    assert_no_crash_freeze_log "$phase"
    printf 'wallStartMs=%s wallEndMs=%s wallMs=%s sfStart=%s sfEnd=%s sfSeconds=%s surfaceFrames=%s surfaceFps=%.6f droppedFrames=%s tailFrames=%s\n' \
        "$wall_start" "$wall_end" "$((wall_end - wall_start))" "$sf_start" "$sf_end" \
        "$sf_seconds" "$frames" "$(awk -v f="$frames" -v s="$sf_seconds" 'BEGIN {print f/s}')" \
        "$dropped" "$tail_frames" > "$window_file"
}

run_phase() {
    local phase="$1"
    local expected_flags="$2"
    local selected_marker="$3"
    local expected_mode="$4"
    local expected_hex_flags="$5"
    local expected_strong="$6"
    local expected_suffix="$7"
    local expected_model="$8"
    local pid

    echo "PHASE_BEGIN name=$phase configureFlags=$expected_flags selectedMarker=${selected_marker:-none}"
    stop_phase_logcat
    stop_game
    set_phase_marker_state "$selected_marker"
    assert_root_boost_disabled
    start_phase_logcat "$phase"
    launch_fresh_process "$phase" "$expected_flags"
    pid="$launched_pid"
    if [[ -n "$last_pid" && "$pid" == "$last_pid" ]]; then
        fail "phase=$phase reason=fresh-pid-contract-failed pid=$pid"
        return 1
    fi
    navigate_to_saved_static_scene "$phase"
    sleep 1
    assert_pid_and_foreground "$phase" "$pid"
    assert_unsafe_marker_state "$selected_marker"
    assert_graphics_baseline
    assert_root_boost_disabled
    assert_gameplay_identity "$phase"
    assert_platform_policy_contract "$phase"
    pull_backend_status "$phase"
    validate_backend_status "$phase" "$expected_flags" "$expected_suffix" "$expected_model"
    assert_memory_ab_log_contract "$phase" "$expected_mode" "$expected_hex_flags" "$expected_strong"
    verify_loaded_artifacts "$phase" "$pid"
    measure_static_phase "$phase" "$pid"
    stop_phase_logcat
    assert_unsafe_marker_state "$selected_marker"
    last_pid="$pid"
    case "$phase" in
    baseline-a1) baseline_a1_pid="$pid" ;;
    memcpyset-relaxed-b) relaxed_b_pid="$pid" ;;
    baseline-a2) baseline_a2_pid="$pid" ;;
    esac
    echo "PHASE_DONE name=$phase pid=$pid configureFlags=$expected_flags staticScene=true"
}

scene_metric() {
    local phase="$1"
    local key="$2"
    awk -v key="$key" '{
        for (i = 1; i <= NF; ++i) {
            split($i, value, "=")
            if (value[1] == key) print value[2]
        }
    }' "$(artifact_file "$phase" 'scene.txt')"
}

compare_aba_scene_metric() {
    local key="$1"
    local tolerance="$2"
    local a1 b a2
    a1="$(scene_metric baseline-a1 "$key")"
    b="$(scene_metric memcpyset-relaxed-b "$key")"
    a2="$(scene_metric baseline-a2 "$key")"
    if ! awk -v a="$a1" -v b="$b" -v c="$a2" -v tolerance="$tolerance" '
        BEGIN {
            mean_a = (a + c) / 2
            drift_a = a > c ? a - c : c - a
            drift_b = b > mean_a ? b - mean_a : mean_a - b
            if (mean_a == 0) exit !(b == 0)
            exit !((100 * drift_a / mean_a) <= tolerance &&
                   (100 * drift_b / mean_a) <= tolerance)
        }'; then
        fail "reason=aba-workload-mismatch metric=$key baselineA1=$a1 relaxedB=$b baselineA2=$a2 tolerancePct=$tolerance"
        return 1
    fi
}

compare_cache_provenance() {
    local a1 b a2
    a1="$(awk -F= '$1 == "cacheBase" {print substr($0, index($0, "=") + 1)}' \
        "$(artifact_file baseline-a1 'backend-provenance.txt')")"
    b="$(awk -F= '$1 == "cacheBase" {print substr($0, index($0, "=") + 1)}' \
        "$(artifact_file memcpyset-relaxed-b 'backend-provenance.txt')")"
    a2="$(awk -F= '$1 == "cacheBase" {print substr($0, index($0, "=") + 1)}' \
        "$(artifact_file baseline-a2 'backend-provenance.txt')")"
    if [[ -z "$a1" || "$a1" != "$b" || "$a1" != "$a2" ]]; then
        fail "reason=cache-namespace-base-mismatch baselineA1=$a1 relaxedB=$b baselineA2=$a2"
        return 1
    fi
    echo "CACHE_PROVENANCE_OK base=$a1 suffixRole=provenance-only"
}

compare_screenshot_graphics() {
    local a1 b a2
    a1="$(artifact_file baseline-a1 'static-end-graphics.json')"
    b="$(artifact_file memcpyset-relaxed-b 'static-end-graphics.json')"
    a2="$(artifact_file baseline-a2 'static-end-graphics.json')"
    python3 - "$(host_path "$a1")" "$(host_path "$b")" "$(host_path "$a2")" \
        "${BB_GRAPHICS_BASELINE_HISTOGRAM_L1_MAX:-0.40}" \
        "${BB_GRAPHICS_RELAXED_HISTOGRAM_L1_MAX:-0.45}" <<'PY'
import json
import pathlib
import sys

a1, b, a2 = [json.loads(pathlib.Path(value).read_text(encoding="utf-8")) for value in sys.argv[1:4]]
a_limit = float(sys.argv[4])
b_limit = float(sys.argv[5])
if (a1["width"], a1["height"]) != (b["width"], b["height"]) or (
    a1["width"], a1["height"]
) != (a2["width"], a2["height"]):
    raise SystemExit("A/B/A screenshot dimensions differ")

def l1(left, right):
    return sum(abs(x - y) for x, y in zip(left["histogram"], right["histogram"]))

mean_a_hist = {
    "histogram": [(x + y) / 2 for x, y in zip(a1["histogram"], a2["histogram"])]
}
a_drift = l1(a1, a2)
b_drift = l1(b, mean_a_hist)
if a_drift > a_limit or b_drift > b_limit:
    raise SystemExit(
        f"static-scene graphics histogram mismatch: A drift={a_drift:.6f}/{a_limit} "
        f"B drift={b_drift:.6f}/{b_limit}"
    )
mean_luma_a = (a1["mean_luma"] + a2["mean_luma"]) / 2
if abs(b["mean_luma"] - mean_luma_a) > max(18.0, mean_luma_a * 0.35):
    raise SystemExit(
        f"relaxed graphics luminance mismatch: baseline={mean_luma_a:.3f} "
        f"relaxed={b['mean_luma']:.3f}"
    )
mean_white_a = (a1["white_fraction"] + a2["white_fraction"]) / 2
white_limit = max(0.20, mean_white_a * 3.0 + 0.02)
if b["white_fraction"] > white_limit:
    raise SystemExit(
        f"relaxed screenshot white/missing-geometry gate failed: "
        f"baseline={mean_white_a:.6f} relaxed={b['white_fraction']:.6f} limit={white_limit:.6f}"
    )
print(
    f"GRAPHICS_ABA_OK aHistogramL1={a_drift:.6f} bHistogramL1={b_drift:.6f} "
    f"baselineMean={mean_luma_a:.3f} relaxedMean={b['mean_luma']:.3f}"
)
PY
}

assert_cross_phase_contract() {
    if [[ -z "$baseline_a1_pid" || -z "$relaxed_b_pid" || -z "$baseline_a2_pid" ||
          "$baseline_a1_pid" == "$relaxed_b_pid" ||
          "$baseline_a1_pid" == "$baseline_a2_pid" ||
          "$relaxed_b_pid" == "$baseline_a2_pid" ]]; then
        fail "reason=aba-pids-not-unique a1=$baseline_a1_pid b=$relaxed_b_pid a2=$baseline_a2_pid"
        return 1
    fi
    compare_aba_scene_metric builderUnitsPerFrame "${BB_SCENE_WORK_TOLERANCE_PCT:-15}"
    compare_aba_scene_metric builderDrawPerFrame "${BB_SCENE_DRAW_TOLERANCE_PCT:-15}"
    compare_aba_scene_metric builderShaderPerFrame "${BB_SCENE_SHADER_TOLERANCE_PCT:-25}"
    compare_screenshot_graphics
    compare_cache_provenance
}

restore_baseline_running_best_effort() {
    set +e
    echo 'CLEANUP_BEGIN target=baseline-running markers=none'
    remove_unsafe_markers
    assert_unsafe_marker_state ''
    local current_pid
    current_pid="$(adb_device shell "pidof $game_process" 2>/dev/null | tr -d '\r[:space:]')"
    if [[ "$active_process_flags" == 0 && -n "$current_pid" ]]; then
        echo "CLEANUP_KEEP_RUNNING_BASELINE pid=$current_pid"
        return 0
    fi
    if [[ -n "$current_pid" ]]; then
        adb_device shell am force-stop "$package" >/dev/null 2>&1
        sleep 1
    fi
    launch_through_handoff
    local attempt
    current_pid=''
    for attempt in 1 2 3; do
        sleep 5
        current_pid="$(adb_device shell "pidof $game_process" 2>/dev/null | tr -d '\r[:space:]')"
        [[ -n "$current_pid" ]] && break
        launch_through_handoff
    done
    if [[ -z "$current_pid" ]]; then
        echo 'CLEANUP_BASELINE_RELAUNCH_FAILED' >&2
        return 1
    fi
    active_process_flags=0
    assert_unsafe_marker_state ''
    echo "CLEANUP_BASELINE_RUNNING pid=$current_pid exactGame=$game"
}

on_exit() {
    local status=$?
    trap - EXIT INT TERM
    stop_phase_logcat
    if (( device_session_started == 1 && cleanup_running == 0 && cleanup_complete == 0 )); then
        cleanup_running=1
        restore_baseline_running_best_effort || true
    fi
    exit "$status"
}

trap on_exit EXIT
trap 'exit 130' INT TERM

if [[ "$device" != "$expected_device" ]]; then
    fail "reason=wrong-device expected=$expected_device actual=$device"
    exit 2
fi
if [[ ! "$window" =~ ^[0-9]+$ ]] || (( window < 12 )); then
    fail "reason=invalid-window value=$window minimum=12"
    exit 2
fi
if [[ ! "$tag" =~ ^[A-Za-z0-9._-]+$ ]]; then
    fail "reason=invalid-tag value=$tag"
    exit 2
fi
mkdir -p "$output_dir"

verify_host_artifacts
if [[ "$(adb_device get-state 2>/dev/null | tr -d '\r[:space:]')" != device ]]; then
    fail "reason=physical-vivo-unavailable serial=$device"
    exit 3
fi
device_session_started=1
assert_root_boost_disabled

# Strict fresh-process A/B/A. The only changed runtime input is configureFlags
# 0 -> 2 -> 0, selected before each new process starts.
run_phase baseline-a1 0 '' baseline 0x0 1 \
    '-lookup-throughput-sm-swtso' \
    'FEX software TSO (hardware TSO when enabled by the host OS)'
run_phase memcpyset-relaxed-b 2 "$relaxed_memcpy_marker" memcpyset-relaxed 0x2 0 \
    '-lookup-throughput-ab-mtso0-sm-swtso' \
    'FEX software TSO (MemcpySetTSO relaxed A/B)'
run_phase baseline-a2 0 '' baseline 0x0 1 \
    '-lookup-throughput-sm-swtso' \
    'FEX software TSO (hardware TSO when enabled by the host OS)'

assert_cross_phase_contract
assert_unsafe_marker_state ''
assert_pid_and_foreground baseline-a2 "$baseline_a2_pid"
pull_backend_status baseline-a2-final
validate_backend_status baseline-a2-final 0 '-lookup-throughput-sm-swtso' \
    'FEX software TSO (hardware TSO when enabled by the host OS)'
if [[ "$(adb_device shell "pidof $game_process" 2>/dev/null | tr -d '\r[:space:]')" != "$baseline_a2_pid" ]]; then
    fail "reason=final-exact-game-not-running expectedPid=$baseline_a2_pid"
    exit 61
fi

cleanup_complete=1
echo "DONE sequence=baseline-a1:0,memcpyset-relaxed-b:2,baseline-a2:0 freshPids=$baseline_a1_pid,$relaxed_b_pid,$baseline_a2_pid finalMode=baseline finalGameRunning=true exactGame=$game"
cat "$(artifact_file baseline-a1 'window.txt')"
cat "$(artifact_file memcpyset-relaxed-b 'window.txt')"
cat "$(artifact_file baseline-a2 'window.txt')"
