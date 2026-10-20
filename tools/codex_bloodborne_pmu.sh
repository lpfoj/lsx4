#!/usr/bin/env bash

set -u

export MSYS2_ARG_CONV_EXCL='*'

expected_device='10AG263F1B001HM'
device="${ADB_SERIAL:-$expected_device}"
window="${1:-14}"
tag="${2:-epochfix}"
game='/data/user/0/app.lsx4.android/files/lsx4-home/runtime-fs/user/app/CUSA03173/eboot.bin'
agentchat='D:/LProj/PetProj/agentchat/agentchat.py'
agent_room='увеличить fps'
agent_target='device:vivo'
profile_root='files/lsx4-home'
overlap_control_dir="$profile_root/runtime/invalidation-overlap-control"
overlap_trace_device="$overlap_control_dir/trace-v1.bin"
hle_clean_marker="$profile_root/run-enable-hle-clean-buffer-fastpath"
hle_clean_mode="${BB_HLE_CLEAN_BUFFER_FASTPATH:-}"
hle_clean_stats_marker="$profile_root/run-diagnostic-hle-clean-buffer-stats"
hle_clean_stats_mode="${BB_HLE_CLEAN_DIAGNOSTIC_STATS:-0}"
async_shader_disable_marker="$profile_root/run-disable-async-shader-retranslation"
async_shader_disable_mode="${BB_DISABLE_ASYNC_SHADER_RETRANSLATION:-0}"
strict_phase_gates="${BB_STRICT_PHASE_GATES:-0}"
adaptive_phase_gates="${BB_ADAPTIVE_PHASE_GATES:-0}"
skip_renderer_log_gate="${BB_NAV_SKIP_RENDERER_LOG_GATE:-0}"
keep_game_running="${BB_KEEP_GAME_RUNNING:-0}"
verify_canonical_fex="${BB_VERIFY_CANONICAL_FEX:-0}"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
workspace_root="$(cd "$repo_root/.." && pwd)"
fex_manifest="$workspace_root/.fex-2608-artifacts/android-api26-arm64/manifest.json"
fex_verifier="$repo_root/tools/fex/verify_android_fex_artifacts.py"
performance_preflight="$repo_root/scripts/preflight-vivo-performance-test.ps1"
action_sequence=0
navigation_stable_distance="${BB_NAV_STABLE_DISTANCE_MAX:-0.025}"
navigation_change_distance="${BB_NAV_CHANGE_DISTANCE_MIN:-0.060}"

graphics_six_markers=(
    "$profile_root/run-disable-dynamic-shadows"
    "$profile_root/run-disable-ssao"
    "$profile_root/run-disable-motion-blur"
    "$profile_root/run-disable-dof"
    "$profile_root/run-disable-anti-aliasing"
    "$profile_root/run-disable-chromatic-aberration"
)

strict_forbidden_markers=(
    "$profile_root/run-enable-fex-buffer-capture-ack"
    "$profile_root/run-enable-fex-deferred-write-epoch"
    "$profile_root/run-enable-bounded-guest-invalidation-drain"
    "$profile_root/run-force-fex-software-tso"
    "$profile_root/run-enable-fex-multiblock"
    "$profile_root/run-enable-fex-relaxed-vector-tso-ab"
    "$profile_root/run-enable-fex-relaxed-memcpyset-tso-ab"
    "$profile_root/run-enable-fex-low-memory-cache"
    "$profile_root/run-profile-guest-invalidation-sources"
    "$profile_root/run-profile-guest-invalidation-overlap"
    "$profile_root/run-disable-adpf-40hz-target"
    "$profile_root/run-use-vivo-moderate-cpu-floor"
    "$profile_root/run-use-vivo-performance-hint-4241"
    "$profile_root/run-disable-vivo-moderate-cpu-floor"
    "$profile_root/diag-jit-helper-perf-map"
    "$profile_root/run-enable-stable-canonical-streaming"
    "$profile_root/run-disable-render-effects-bundle"
    "$profile_root/run-force-internal-resolution-640x360"
    "$profile_root/run-disable-vk-robustness"
    "$profile_root/run-force-rectlist-first-vertex-zero"
    "$profile_root/run-disable-audio-output"
    "$profile_root/run-force-interpret-audio"
    "$profile_root/run-force-interpret-audio-thread2"
    "$profile_root/run-force-interpret-all"
    "$profile_root/run-disable-mspace-free-reuse"
    "$profile_root/run-enable-memset-vtable-guard"
    "$profile_root/run-disable-imt-fix"
    'files/run-enable-icall-bind-detour'
    'files/run-enable-icall-slot-fill'
    'files/run-enable-legacy-patches'
)

if [[ "$device" != "$expected_device" &&
      ! "$device" =~ ^adb-10AG263F1B001HM-[A-Za-z0-9()[:space:].]+[.]_adb-tls-connect[.]_tcp$ ]]; then
    echo "INVALID_DEVICE=$device EXPECTED=$expected_device OR_STRICT_WIRELESS_ALIAS"
    exit 23
fi
for boolean_spec in \
    "BB_STRICT_PHASE_GATES:$strict_phase_gates" \
    "BB_ADAPTIVE_PHASE_GATES:$adaptive_phase_gates" \
    "BB_NAV_SKIP_RENDERER_LOG_GATE:$skip_renderer_log_gate" \
    "BB_KEEP_GAME_RUNNING:$keep_game_running" \
    "BB_VERIFY_CANONICAL_FEX:$verify_canonical_fex" \
    "BB_HLE_CLEAN_DIAGNOSTIC_STATS:$hle_clean_stats_mode" \
    "BB_DISABLE_ASYNC_SHADER_RETRANSLATION:$async_shader_disable_mode"; do
    boolean_name="${boolean_spec%%:*}"
    boolean_value="${boolean_spec#*:}"
    if [[ "$boolean_value" != 0 && "$boolean_value" != 1 ]]; then
        echo "INVALID_BOOLEAN=$boolean_name:$boolean_value"
        exit 41
    fi
done
if [[ "$adaptive_phase_gates" == 1 ]]; then
    for integer_spec in \
        "BB_NAV_BOOT_TIMEOUT:${BB_NAV_BOOT_TIMEOUT:-180}" \
        "BB_NAV_PHASE_TIMEOUT:${BB_NAV_PHASE_TIMEOUT:-180}" \
        "BB_NAV_POLL_SECONDS:${BB_NAV_POLL_SECONDS:-3}" \
        "BB_NAV_STABLE_SAMPLES:${BB_NAV_STABLE_SAMPLES:-2}" \
        "BB_WARNING_WAIT:${BB_WARNING_WAIT:-64}" \
        "BB_OFFLINE_WAIT:${BB_OFFLINE_WAIT:-30}" \
        "BB_CONTINUE_WAIT:${BB_CONTINUE_WAIT:-22}" \
        "BB_DOOR_WAIT:${BB_DOOR_WAIT:-65}" \
        "BB_DOOR_SETTLE_WAIT:${BB_DOOR_SETTLE_WAIT:-5}"; do
        integer_name="${integer_spec%%:*}"
        integer_value="${integer_spec#*:}"
        if [[ ! "$integer_value" =~ ^[0-9]+$ ]]; then
            echo "INVALID_ADAPTIVE_INTEGER=$integer_name:$integer_value"
            exit 70
        fi
    done
    if (( ${BB_NAV_BOOT_TIMEOUT:-180} == 0 ||
          ${BB_NAV_PHASE_TIMEOUT:-180} == 0 ||
          ${BB_NAV_POLL_SECONDS:-3} == 0 ||
          ${BB_NAV_STABLE_SAMPLES:-2} == 0 )); then
        echo 'INVALID_ADAPTIVE_ZERO=timeout,poll,and-stable-samples-must-be-positive'
        exit 70
    fi
    for distance_spec in \
        "BB_NAV_STABLE_DISTANCE_MAX:$navigation_stable_distance" \
        "BB_NAV_CHANGE_DISTANCE_MIN:$navigation_change_distance"; do
        distance_name="${distance_spec%%:*}"
        distance_value="${distance_spec#*:}"
        if [[ ! "$distance_value" =~ ^(0([.][0-9]+)?|1([.]0+)?)$ ]]; then
            echo "INVALID_ADAPTIVE_DISTANCE=$distance_name:$distance_value"
            exit 70
        fi
    done
fi
if [[ -n "$hle_clean_mode" && "$hle_clean_mode" != 0 && "$hle_clean_mode" != 1 ]]; then
    echo "INVALID_HLE_CLEAN_MODE=$hle_clean_mode"
    exit 42
fi
if [[ "$strict_phase_gates" == 1 && -z "$hle_clean_mode" ]]; then
    echo 'STRICT_PHASE_REQUIRES_HLE_CLEAN_MODE=0-or-1'
    exit 43
fi
if [[ "$hle_clean_mode" == 1 && "$async_shader_disable_mode" != 1 ]]; then
    echo 'HLE_CLEAN_REQUIRES_ASYNC_SHADER_RETRANSLATION_DISABLED=1'
    exit 66
fi
if [[ "$hle_clean_stats_mode" == 1 &&
      ( "$strict_phase_gates" != 1 || -z "$hle_clean_mode" ||
        "$async_shader_disable_mode" != 1 ) ]]; then
    echo 'HLE_CLEAN_DIAGNOSTIC_STATS_REQUIRES=strict-phase,hlemode-0-or-1,async-disabled'
    exit 71
fi
if [[ "$strict_phase_gates" == 1 ]]; then
    set -o pipefail
fi
if [[ "$keep_game_running" == 1 && "$hle_clean_mode" != 0 ]]; then
    echo 'KEEP_RUNNING_REQUIRES_BASELINE_HLE_CLEAN_MODE=0'
    exit 44
fi
if [[ ! "$window" =~ ^[1-9][0-9]*$ ]]; then
    echo "INVALID_WINDOW=$window"
    exit 24
fi
if [[ "${BB_PROFILE_INVALIDATION_OVERLAP:-0}" == 1 && "$window" -gt 18 ]]; then
    echo "INVALID_OVERLAP_WINDOW=$window MAX=18"
    exit 25
fi

if [[ "${BB_PROFILE_INVALIDATION_SOURCES:-0}" == 1 &&
      "${BB_PROFILE_INVALIDATION_OVERLAP:-0}" == 1 ]]; then
    echo 'INVALID_PROFILE_COMBINATION=guest-invalidation-sources,guest-invalidation-overlap'
    exit 22
fi

guard_device_action() {
    action_sequence=$((action_sequence + 1))
    python3 "$agentchat" event --agent codex --room "$agent_room" \
        --action adb --target "$agent_target" \
        --summary "codex_bloodborne_pmu device action $action_sequence" \
        >/dev/null || {
        echo 'EVENT_FAILED' >&2
        return 3
    }
    python3 "$agentchat" own --agent codex --room "$agent_room" \
        --area "$agent_target" >/dev/null || {
        echo 'OWN_FAILED' >&2
        return 3
    }
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
    guard_device_action || return $?
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
    guard_device_action || return $?
    adb -s "$device" "$@" &
    background_adb_pid=$!
}

run_performance_preflight() {
    local mode="${1:-full}"
    local preflight_path="$performance_preflight"
    if ! command -v pwsh >/dev/null 2>&1; then
        echo 'VIVO_PERFORMANCE_PREFLIGHT_FAILED reason=pwsh-missing' >&2
        return 1
    fi
    if [[ ! -f "$performance_preflight" ]]; then
        echo "VIVO_PERFORMANCE_PREFLIGHT_FAILED reason=script-missing path=$performance_preflight" >&2
        return 1
    fi
    if command -v cygpath >/dev/null 2>&1; then
        preflight_path=$(cygpath -w "$performance_preflight") || return 1
    fi
    local -a preflight_args=(
        -NoProfile -NonInteractive -File "$preflight_path"
        -DeviceId "$expected_device" -AdbTarget "$device"
    )
    case "$mode" in
        full) ;;
        cleanup-only) preflight_args+=(-CleanupOnly) ;;
        *)
            echo "VIVO_PERFORMANCE_PREFLIGHT_FAILED reason=invalid-mode value=$mode" >&2
            return 1
            ;;
    esac
    pwsh "${preflight_args[@]}"
}

verify_vivo_device_identity() {
    local reported_serial
    if ! reported_serial=$(adb_device shell getprop ro.serialno 2>/dev/null); then
        echo "VIVO_DEVICE_IDENTITY_QUERY_FAILED adbTarget=$device" >&2
        return 1
    fi
    reported_serial=${reported_serial//$'\r'/}
    if [[ "$reported_serial" != "$expected_device" ]]; then
        echo "VIVO_DEVICE_IDENTITY_MISMATCH adbTarget=$device expected=$expected_device actual=$reported_serial" >&2
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

stop_game() {
    if ! adb_device shell am force-stop app.lsx4.android >/dev/null 2>&1; then
        echo 'STOP_GAME_FORCE_STOP_FAILED' >&2
        return 1
    fi
    local attempt pid
    for attempt in {1..10}; do
        if ! pid=$(adb_device shell \
                'pidof app.lsx4.android app.lsx4.android:game || true' \
                2>/dev/null | tr -d '\r[:space:]'); then
            echo 'STOP_GAME_PID_QUERY_FAILED' >&2
            return 1
        fi
        if [[ -z "$pid" ]]; then
            echo 'STOP_GAME_VERIFIED pids=absent'
            return 0
        fi
        sleep 0.2
    done
    echo "STOP_GAME_PID_STILL_RUNNING pid=$pid" >&2
    return 1
}

cleanup_profile_markers() {
    adb_device shell "run-as app.lsx4.android rm -f \
        '$profile_root/run-profile-guest-invalidation-sources' \
        '$profile_root/run-profile-guest-invalidation-overlap' \
        '$hle_clean_stats_marker' \
        '$overlap_control_dir/arm' \
        '$overlap_control_dir/ready' \
        '$overlap_control_dir/ready.tmp' \
        '$overlap_control_dir/stop' \
        '$overlap_control_dir/done' \
        '$overlap_control_dir/error' \
        '$overlap_control_dir/trace-v1.bin' \
        '$overlap_control_dir/trace-v1.bin.tmp'" >/dev/null 2>&1 || true
    if [[ -n "$hle_clean_mode" ]]; then
        adb_device shell "run-as app.lsx4.android rm -f '$hle_clean_marker'" \
            >/dev/null 2>&1 || true
    fi
    adb_device shell \
        "run-as app.lsx4.android rm -f '$async_shader_disable_marker'" \
        >/dev/null 2>&1 || true
    if ! adb_device shell \
            "run-as app.lsx4.android test ! -e '$hle_clean_stats_marker'" \
            >/dev/null; then
        echo 'HLE_CLEAN_STATS_MARKER_CLEANUP_FAILED'
        return 1
    fi
    echo 'HLE_CLEAN_STATS_MARKER_CLEAN=1'
}

set_async_shader_retranslation_marker_state() {
    if ! adb_device shell "run-as app.lsx4.android mkdir -p '$profile_root' && \
        run-as app.lsx4.android rm -f '$async_shader_disable_marker'" >/dev/null; then
        echo 'ASYNC_SHADER_RETRANSLATION_MARKER_RESET_FAILED'
        return 1
    fi
    if [[ "$async_shader_disable_mode" == 1 ]]; then
        if ! adb_device shell \
                "run-as app.lsx4.android touch '$async_shader_disable_marker'" >/dev/null ||
           ! adb_device shell \
                "run-as app.lsx4.android test -f '$async_shader_disable_marker'" >/dev/null; then
            echo 'ASYNC_SHADER_RETRANSLATION_MARKER_ENABLE_FAILED'
            return 1
        fi
    elif ! adb_device shell \
            "run-as app.lsx4.android test ! -e '$async_shader_disable_marker'" >/dev/null; then
        echo 'ASYNC_SHADER_RETRANSLATION_MARKER_DISABLE_FAILED'
        return 1
    fi
    echo "ASYNC_SHADER_RETRANSLATION_MARKER_STATE expectedDisabled=$async_shader_disable_mode verified=1"
}

set_hle_clean_marker_state() {
    [[ -z "$hle_clean_mode" ]] && return 0
    if ! adb_device shell "run-as app.lsx4.android mkdir -p '$profile_root' && \
        run-as app.lsx4.android rm -f '$hle_clean_marker'" >/dev/null; then
        echo 'HLE_CLEAN_MARKER_RESET_FAILED'
        return 1
    fi
    if [[ "$hle_clean_mode" == 1 ]]; then
        if ! adb_device shell "run-as app.lsx4.android touch '$hle_clean_marker'" >/dev/null; then
            echo 'HLE_CLEAN_MARKER_CREATE_FAILED'
            return 1
        fi
        if ! adb_device shell "run-as app.lsx4.android test -f '$hle_clean_marker'" >/dev/null; then
            echo 'HLE_CLEAN_MARKER_ENABLE_VERIFY_FAILED'
            return 1
        fi
    elif ! adb_device shell "run-as app.lsx4.android test ! -e '$hle_clean_marker'" >/dev/null; then
        echo 'HLE_CLEAN_MARKER_DISABLE_VERIFY_FAILED'
        return 1
    fi
    echo "HLE_CLEAN_MARKER_STATE expected=$hle_clean_mode verified=1"
}

set_hle_clean_stats_marker_state() {
    if ! adb_device shell "run-as app.lsx4.android mkdir -p '$profile_root' && \
        run-as app.lsx4.android rm -f '$hle_clean_stats_marker'" >/dev/null; then
        echo 'HLE_CLEAN_STATS_MARKER_RESET_FAILED'
        return 1
    fi
    if [[ "$hle_clean_stats_mode" == 1 ]]; then
        if ! adb_device shell \
                "run-as app.lsx4.android touch '$hle_clean_stats_marker'" >/dev/null ||
           ! adb_device shell \
                "run-as app.lsx4.android test -f '$hle_clean_stats_marker'" >/dev/null; then
            echo 'HLE_CLEAN_STATS_MARKER_ENABLE_FAILED'
            return 1
        fi
    elif ! adb_device shell \
            "run-as app.lsx4.android test ! -e '$hle_clean_stats_marker'" >/dev/null; then
        echo 'HLE_CLEAN_STATS_MARKER_DISABLE_FAILED'
        return 1
    fi
    echo "HLE_CLEAN_STATS_MARKER_STATE expected=$hle_clean_stats_mode verified=1 diagnosticOnly=$hle_clean_stats_mode"
}

remove_and_verify_strict_confounders() {
    [[ "$strict_phase_gates" != 1 ]] && return 0
    local paths=("${strict_forbidden_markers[@]}" "${graphics_six_markers[@]}" \
                 "$async_shader_disable_marker")
    if ! adb_device shell "run-as app.lsx4.android rm -f ${paths[*]}" >/dev/null; then
        echo 'STRICT_CONFOUNDER_REMOVE_FAILED'
        return 1
    fi
    local probe='' path
    for path in "${paths[@]}"; do
        probe+="if [ -e '$path' ]; then echo '$path'; fi; "
    done
    local active
    active=$(adb_device shell "run-as app.lsx4.android sh -c \"$probe\"" \
        2>/dev/null | tr -d '\r') || {
        echo 'STRICT_CONFOUNDER_PROBE_FAILED'
        return 1
    }
    if [[ -n "$active" ]]; then
        echo "STRICT_CONFOUNDERS_ACTIVE=$(echo "$active" | paste -sd, -)"
        return 1
    fi
    echo 'STRICT_CONFOUNDERS=none GRAPHICS_RESET=1 TARGET_ALL_SIX=off ADPF=25ms VIVO=performance-hint-4241'
}

assert_strict_runtime_marker_state() {
    [[ "$strict_phase_gates" != 1 ]] && return 0
    local probe='' path
    for path in "${strict_forbidden_markers[@]}"; do
        probe+="if [ -e '$path' ]; then echo '$path'; fi; "
    done
    local active
    active=$(adb_device shell "run-as app.lsx4.android sh -c \"$probe\"" \
        2>/dev/null | tr -d '\r') || return 1
    if [[ -n "$active" ]]; then
        echo "POST_LAUNCH_STRICT_CONFOUNDERS=$(echo "$active" | paste -sd, -)"
        return 1
    fi
    probe=''
    for path in "${graphics_six_markers[@]}"; do
        probe+="if [ ! -e '$path' ]; then echo '$path'; fi; "
    done
    local missing
    missing=$(adb_device shell "run-as app.lsx4.android sh -c \"$probe\"" \
        2>/dev/null | tr -d '\r') || return 1
    if [[ -n "$missing" ]]; then
        echo "POST_LAUNCH_GRAPHICS_BASELINE_MISSING=$(echo "$missing" | paste -sd, -)"
        return 1
    fi
    echo 'POST_LAUNCH_GRAPHICS_BASELINE ALL_SIX=off markers=6/6'
    if [[ "$async_shader_disable_mode" == 1 ]]; then
        adb_device shell \
            "run-as app.lsx4.android test -f '$async_shader_disable_marker'" >/dev/null || return 1
    else
        adb_device shell \
            "run-as app.lsx4.android test ! -e '$async_shader_disable_marker'" >/dev/null || return 1
    fi
    echo "POST_LAUNCH_ASYNC_SHADER_RETRANSLATION_MARKER expectedDisabled=$async_shader_disable_mode verified=1"
    if [[ "$hle_clean_mode" == 1 ]]; then
        adb_device shell "run-as app.lsx4.android test -f '$hle_clean_marker'" >/dev/null || return 1
    else
        adb_device shell "run-as app.lsx4.android test ! -e '$hle_clean_marker'" >/dev/null || return 1
    fi
    echo "POST_LAUNCH_HLE_CLEAN_MARKER expected=$hle_clean_mode verified=1"
    if [[ "$hle_clean_stats_mode" == 1 ]]; then
        adb_device shell \
            "run-as app.lsx4.android test -f '$hle_clean_stats_marker'" >/dev/null || return 1
    else
        adb_device shell \
            "run-as app.lsx4.android test ! -e '$hle_clean_stats_marker'" >/dev/null || return 1
    fi
    echo "POST_LAUNCH_HLE_CLEAN_STATS_MARKER expected=$hle_clean_stats_mode verified=1 diagnosticOnly=$hle_clean_stats_mode"
}

assert_strict_root_boost_disabled() {
    [[ "$strict_phase_gates" != 1 ]] && return 0
    local preferences
    if ! preferences=$(adb_device exec-out run-as app.lsx4.android sh -c \
            'if [ -f shared_prefs/lsx4_settings.xml ]; then cat shared_prefs/lsx4_settings.xml; fi' \
            2>/dev/null); then
        echo 'STRICT_ROOT_BOOST_SETTING_PROBE_FAILED'
        return 1
    fi
    if grep -Eq 'name="managed_oem_root_performance"[[:space:]]+value="true"' \
            <<< "$preferences"; then
        echo 'STRICT_ROOT_BOOST_SETTING_ENABLED'
        return 1
    fi
    echo 'STRICT_ROOT_BOOST_SETTING=disabled'
}

manifest_value() {
    local role="$1"
    local field="$2"
    python3 -c 'import json,pathlib,sys; value=json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))["artifacts"][sys.argv[2]][sys.argv[3]]; print(pathlib.Path(value).resolve().as_posix() if sys.argv[3] == "path" else value)' \
        "$(host_path "$fex_manifest")" "$role" "$field"
}

verify_canonical_fex_host() {
    [[ "$verify_canonical_fex" != 1 ]] && return 0
    if [[ ! -f "$fex_manifest" || ! -f "$fex_verifier" ]] ||
       ! python3 "$(host_path "$fex_verifier")" --manifest \
            "$(host_path "$fex_manifest")"; then
        echo 'CANONICAL_FEX_HOST_VERIFICATION_FAILED'
        return 1
    fi
    local role path expected actual
    for role in adapter bridge fexcore; do
        path=$(manifest_value "$role" path) || return 1
        expected=$(manifest_value "$role" sha256) || return 1
        actual=$(sha256sum "$path" | awk '{print $1}') || return 1
        if [[ -z "$actual" || "$actual" != "$expected" ]]; then
            echo "CANONICAL_FEX_HOST_HASH_MISMATCH role=$role expected=$expected actual=$actual"
            return 1
        fi
    done
    echo 'CANONICAL_FEX_HOST_VERIFIED roles=adapter,bridge,fexcore'
}

verify_canonical_fex_runtime() {
    local pid="$1"
    [[ "$verify_canonical_fex" != 1 ]] && return 0
    local maps_file=".codex-bb-$tag-maps.txt"
    local hashes_file=".codex-bb-$tag-loaded-fex-hashes.txt"
    if ! adb_device shell "run-as app.lsx4.android cat /proc/$pid/maps" \
            2>/dev/null | tr -d '\r' > "$maps_file"; then
        echo 'CANONICAL_FEX_MAPS_CAPTURE_FAILED'
        return 1
    fi
    : > "$hashes_file"
    local role basename local_path loaded_path local_sha loaded_sha
    for role in executor adapter bridge fexcore; do
        case "$role" in
        executor)
            basename='liblsx4_executor_android.so'
            local_path="$repo_root/build/android-arm64-release/$basename"
            ;;
        adapter)
            basename='liblsx4_fex_backend_v3.so'
            local_path=$(manifest_value adapter path) || return 1
            ;;
        bridge)
            basename='liblsx4_fex2608_context_bridge.so'
            local_path=$(manifest_value bridge path) || return 1
            ;;
        fexcore)
            basename='libFEXCore.so'
            local_path=$(manifest_value fexcore path) || return 1
            ;;
        esac
        if [[ ! -f "$local_path" ]]; then
            echo "CANONICAL_FEX_LOCAL_ARTIFACT_MISSING role=$role path=$local_path"
            return 1
        fi
        local_sha=$(sha256sum "$local_path" | awk '{print $1}') || return 1
        loaded_path=$(awk -v name="$basename" 'index($0,name){print $6; exit}' "$maps_file")
        if [[ -z "$loaded_path" ]]; then
            echo "CANONICAL_FEX_RUNTIME_ARTIFACT_MISSING role=$role library=$basename"
            return 1
        fi
        loaded_sha=$(adb_device shell "run-as app.lsx4.android sha256sum '$loaded_path'" \
            2>/dev/null | tr -d '\r' | awk '{print $1}')
        if [[ -z "$loaded_sha" || "$loaded_sha" != "$local_sha" ]]; then
            echo "CANONICAL_FEX_RUNTIME_HASH_MISMATCH role=$role loaded=$loaded_sha local=$local_sha"
            return 1
        fi
        printf 'role=%s loadedPath=%s loadedSha256=%s localPath=%s localSha256=%s\n' \
            "$role" "$loaded_path" "$loaded_sha" "$local_path" "$local_sha" \
            >> "$hashes_file"
    done
    echo "CANONICAL_FEX_RUNTIME_VERIFIED pid=$pid roles=executor,adapter,bridge,fexcore"
}

capture_and_validate_fex_status() {
    [[ "$verify_canonical_fex" != 1 ]] && return 0
    local status_file=".codex-bb-$tag-guest-cpu-backend-status.json"
    local initial_status_file=".codex-bb-$tag-guest-cpu-backend-status-initial.json"
    local status_log=".codex-bb-$tag-fex-status-logcat.txt"
    if ! adb_device exec-out run-as app.lsx4.android cat \
            files/guest-cpu-backend-status.json > "$initial_status_file" 2>/dev/null ||
       [[ ! -s "$initial_status_file" ]]; then
        echo 'CANONICAL_FEX_STATUS_CAPTURE_FAILED'
        return 1
    fi
    if ! adb_device logcat -b all -d -v threadtime > "$status_log" 2>/dev/null ||
       [[ ! -s "$status_log" ]]; then
        echo 'CANONICAL_FEX_STATUS_LOG_CAPTURE_FAILED'
        return 1
    fi
    python3 - "$initial_status_file" "$status_file" "$status_log" <<'PY'
import json
import pathlib
import sys

initial_path = pathlib.Path(sys.argv[1])
status_path = pathlib.Path(sys.argv[2])
log_path = pathlib.Path(sys.argv[3])
initial = json.loads(initial_path.read_text(encoding="utf-8"))
log = log_path.read_text(encoding="utf-8", errors="replace")
prefix = "EXECUTOR_GUEST_CPU_BACKEND phase=initialized status="
initialized = [line.split(prefix, 1)[1].strip() for line in log.splitlines() if prefix in line]
if not initialized:
    raise SystemExit("initialized FEX status missing from runtime log")
status = json.loads(initialized[-1])
status_path.write_text(json.dumps(status, sort_keys=True) + "\n", encoding="utf-8")
if initial.get("requested") != "fex-experimental" or initial.get("effective") != "fex-experimental":
    raise SystemExit("initial Android FEX selection mismatch")
if status.get("requested") != "fex-experimental" or status.get("effective") != "fex-experimental":
    raise SystemExit(f"effective backend mismatch: {status.get('effective')!r}")
if status.get("supported") is not True or status.get("fallbackReason") != "none":
    raise SystemExit("FEX unsupported or fallback active")
for name in ("configureFlags", "providedCapabilities", "requiredCapabilities", "forbiddenCapabilities"):
    if type(status.get(name)) is not int:
        raise SystemExit(f"{name} is not an exact integer")
if status["configureFlags"] != 0:
    raise SystemExit(f"noncanonical configureFlags={status['configureFlags']}")
strong = 1 << 2
ab_override = 1 << 12
if not status["providedCapabilities"] & strong or not status["requiredCapabilities"] & strong:
    raise SystemExit("strong-memory capability bit2 absent")
if not status["providedCapabilities"] & ab_override:
    raise SystemExit("A/B capability bit12 absent")
if status["forbiddenCapabilities"] & strong:
    raise SystemExit("strong-memory capability bit2 forbidden")
if status.get("strongMemoryModel") != "FEX software TSO (hardware TSO when enabled by the host OS)":
    raise SystemExit(f"strongMemoryModel mismatch: {status.get('strongMemoryModel')!r}")
if status.get("faultSignalOwner") != "unselected":
    raise SystemExit(f"faultSignalOwner mismatch: {status.get('faultSignalOwner')!r}")
if status.get("selectionLocked") is not True:
    raise SystemExit("FEX selection is not locked after initialization")
cache = status.get("cacheNamespace")
if not isinstance(cache, str) or not cache.startswith("fex-2608-ps4-v3-") or not cache.endswith("-lookup-throughput-sm-swtso"):
    raise SystemExit(f"cache namespace mismatch: {cache!r}")
required_runtime_lines = (
    "[EXECUTOR_FEX_SIGNAL_MODE] owner=split shadps4=SIGSEGV,SIGBUS fex=SIGILL jit=0",
    "[EXECUTOR_FEX_bridge] full FEX runtime: LSX4 signal owner, guest delivery, and thread wake active",
)
for required in required_runtime_lines:
    if required not in log:
        raise SystemExit(f"canonical FEX runtime proof missing: {required}")
print("CANONICAL_FEX_STATUS_OK configureFlags=0 faultSignalOwner=unselected signalMode=split selectionLocked=true")
PY
}

game_surface_frames_from_file() {
    awk '
        /^layerName = / {
            game = ($0 ~ /SurfaceView\[app\.lsx4\.android\/app\.lsx4\.android\.GameActivity\]\(BLAST\)/)
        }
        game && /^totalFrames = / { total += $3 }
        END { print total + 0 }
    ' "$1"
}

game_dropped_frames_from_file() {
    awk '
        /^layerName = / {
            game = ($0 ~ /SurfaceView\[app\.lsx4\.android\/app\.lsx4\.android\.GameActivity\]\(BLAST\)/)
        }
        game && /^droppedFrames = / { total += $3 }
        END { print total + 0 }
    ' "$1"
}

game_max_present_gap_ms_from_file() {
    awk '
        /^layerName = / {
            game = ($0 ~ /SurfaceView\[app\.lsx4\.android\/app\.lsx4\.android\.GameActivity\]\(BLAST\)/)
        }
        game && /^present2present histogram is as below:/ {
            if (getline <= 0) exit 2
            maximum = 0
            for (i = 1; i <= NF; ++i) {
                split($i, pair, "=")
                bucket = pair[1]
                sub(/ms$/, "", bucket)
                if ((pair[2] + 0) > 0 && (bucket + 0) > maximum) maximum = bucket + 0
            }
            print maximum
            found = 1
            exit
        }
        END { if (!found) exit 2 }
    ' "$1"
}

write_builder_scene_metrics() {
    local input="$1"
    local output="$2"
    awk '
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
                    first_frames = frames; first_total = total
                    first_draws = draws; first_shaders = shaders
                }
                last_frames = frames; last_total = total
                last_draws = draws; last_shaders = shaders
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
                total_delta / frame_delta, draw_delta / frame_delta, shader_delta / frame_delta
        }
    ' "$input" > "$output"
}

validate_png_image() {
    local image="$1"
    local metrics="$2"
    python3 - "$image" "$metrics" <<'PY'
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
    raise SystemExit("screenshot is not PNG")
offset, idat = 8, []
width = height = depth = color = None
while offset + 12 <= len(data):
    length = struct.unpack(">I", data[offset:offset + 4])[0]
    kind = data[offset + 4:offset + 8]
    payload = data[offset + 8:offset + 8 + length]
    offset += 12 + length
    if kind == b"IHDR":
        width, height, depth, color, compression, filtering, interlace = struct.unpack(">IIBBBBB", payload)
        if compression or filtering or interlace:
            raise SystemExit("unsupported PNG encoding")
    elif kind == b"IDAT":
        idat.append(payload)
    elif kind == b"IEND":
        break
if width is None or not idat or depth != 8 or color not in (2, 6):
    raise SystemExit("unsupported or incomplete PNG")
channels = 3 if color == 2 else 4
stride = width * channels
raw = zlib.decompress(b"".join(idat))
if len(raw) != height * (stride + 1):
    raise SystemExit("PNG decompressed size mismatch")
previous = bytearray(stride)
histogram = [0] * 16
total = total_sq = 0.0
black = white = samples = 0
central_histogram = [0] * 16
central_total = central_total_sq = 0.0
central_black = central_samples = 0
central_x0, central_x1 = int(width * 0.12), int(width * 0.88)
central_y0, central_y1 = int(height * 0.15), int(height * 0.85)
cursor = linear = 0
step = max(1, (width * height) // 250000)
for y in range(height):
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
            p = left + up - up_left
            pa, pb, pc = abs(p - left), abs(p - up), abs(p - up_left)
            predictor = left if pa <= pb and pa <= pc else up if pb <= pc else up_left
        else:
            raise SystemExit(f"unsupported PNG filter {filter_type}")
        row[index] = (value + predictor) & 0xFF
    for pixel in range(0, stride, channels):
        if linear % step == 0:
            r, g, b = row[pixel], row[pixel + 1], row[pixel + 2]
            luma = (54 * r + 183 * g + 19 * b) / 256.0
            histogram[min(15, int(luma) // 16)] += 1
            total += luma
            total_sq += luma * luma
            black += luma < 12
            white += luma > 245
            samples += 1
            x = pixel // channels
            if central_x0 <= x < central_x1 and central_y0 <= y < central_y1:
                central_histogram[min(15, int(luma) // 16)] += 1
                central_total += luma
                central_total_sq += luma * luma
                central_black += luma < 12
                central_samples += 1
        linear += 1
    previous = row
if width < 1000 or height < 500 or samples < 10000:
    raise SystemExit(f"implausible screenshot {width}x{height} samples={samples}")
mean = total / samples
stddev = math.sqrt(max(0.0, total_sq / samples - mean * mean))
if central_samples < 1000:
    raise SystemExit(f"implausible central screenshot samples={central_samples}")
central_mean = central_total / central_samples
central_stddev = math.sqrt(
    max(0.0, central_total_sq / central_samples - central_mean * central_mean)
)
if stddev < 5.0 or black / samples > 0.985 or white / samples > 0.95:
    raise SystemExit(
        f"blank/corrupt graphics gate mean={mean:.3f} stddev={stddev:.3f} "
        f"black={black / samples:.6f} white={white / samples:.6f}"
    )
result = {
    "width": width, "height": height, "samples": samples,
    "mean_luma": mean, "stddev_luma": stddev,
    "black_fraction": black / samples, "white_fraction": white / samples,
    "histogram": [value / samples for value in histogram],
    "central_mean_luma": central_mean, "central_stddev_luma": central_stddev,
    "central_black_fraction": central_black / central_samples,
    "central_histogram": [value / central_samples for value in central_histogram],
}
destination.write_text(json.dumps(result, sort_keys=True) + "\n", encoding="utf-8")
print(f"GRAPHICS_PNG_OK size={width}x{height} mean={mean:.3f} stddev={stddev:.3f}")
PY
}

validate_hle_clean_log_contract() {
    local log_file="$1"
    local measure_log="${2:-$1}"
    local measured_frames="${3:-0}"
    if [[ "$hle_clean_mode" == 0 ]]; then
        if grep -Eq '\[EXECUTOR_HLE_CLEAN_BUFFER_FASTPATH\].*(enabled=1|attempts=)' "$log_file"; then
            echo 'HLE_CLEAN_LOG_ENABLED_IN_BASELINE'
            return 1
        fi
    elif ! grep -Eq '\[EXECUTOR_HLE_CLEAN_BUFFER_FASTPATH\] source=sentinel .*enabled=1 .*scope=memset-only .*maxPages=16' \
                 "$log_file"; then
        echo 'HLE_CLEAN_STARTUP_LOG_MISSING'
        return 1
    fi
    python3 - "$measure_log" "$measured_frames" "$hle_clean_mode" \
        "$hle_clean_stats_mode" ".codex-bb-$tag-hle-clean-window.json" \
        "${BB_MIN_HLE_SYNC_COVERAGE:-0.50}" <<'PY'
import json
import math
import pathlib
import re
import sys

log_path = pathlib.Path(sys.argv[1])
frames = int(sys.argv[2])
mode = int(sys.argv[3])
diagnostic_stats_mode = int(sys.argv[4])
destination = pathlib.Path(sys.argv[5])
minimum_coverage = float(sys.argv[6])
if frames <= 0:
    raise SystemExit(f"invalid measured frame count: {frames}")
if diagnostic_stats_mode not in (0, 1):
    raise SystemExit(f"invalid diagnostic stats mode: {diagnostic_stats_mode}")
if not 0.0 <= minimum_coverage <= 1.0:
    raise SystemExit(f"invalid BB_MIN_HLE_SYNC_COVERAGE: {minimum_coverage}")
lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()

def snapshots(marker, required, floating=()):
    result = []
    for line in lines:
        if marker not in line:
            continue
        parsed = {}
        for key in required:
            pattern = rf"(?:^|\s){re.escape(key)}=([0-9]+(?:\.[0-9]+)?)(?:\s|$)"
            match = re.search(pattern, line)
            if not match:
                raise SystemExit(f"counter {key} missing from {marker} line")
            parsed[key] = float(match.group(1)) if key in floating else int(match.group(1))
        result.append(parsed)
    return result

guest_required = ("requests", "waitTotalMs", "waitMaxMs")
guest = snapshots(
    "[EXECUTOR_GUEST_INVALIDATION]", guest_required,
    floating=("waitTotalMs", "waitMaxMs"),
)
if len(guest) < 2:
    raise SystemExit(f"guest invalidation measurement snapshots insufficient: {len(guest)}")
guest_first, guest_last = guest[0], guest[-1]
guest_requests = guest_last["requests"] - guest_first["requests"]
guest_wait_ms = guest_last["waitTotalMs"] - guest_first["waitTotalMs"]
if guest_requests <= 0 or guest_wait_ms < 0:
    raise SystemExit(
        f"guest invalidation measurement delta invalid: requests={guest_requests} waitMs={guest_wait_ms}"
    )

hle_required = (
    "attempts", "eligible", "committed", "avoidedSync", "fallbackSize", "fallbackAlias",
    "fallbackMapping", "fallbackPage", "fallbackTexture", "fallbackShader",
    "fallbackRegionMissing", "fallbackRegionBusy", "fallbackRegionGpuDirty",
    "fallbackPostvalidate", "fallbackNested",
    "pages", "bytes", "active", "activeMax",
)
hle = snapshots("[EXECUTOR_HLE_CLEAN_BUFFER_FASTPATH]", hle_required)
metrics = {
    "mode": mode,
    "diagnosticOnly": diagnostic_stats_mode == 1,
    "fpsVerdictAllowed": diagnostic_stats_mode == 0,
    "frames": frames,
    "measurementLog": str(log_path),
    "guestInvalidation": {
        "snapshots": len(guest),
        "requests": guest_requests,
        "waitTotalMs": guest_wait_ms,
        "waitMsPerFrame": guest_wait_ms / frames,
        "waitMaxCumulativeMs": guest_last["waitMaxMs"],
    },
}

print(
    "GUEST_INVALIDATION_WINDOW_OK "
    f"snapshots={len(guest)} requests={guest_requests} waitTotalMs={guest_wait_ms:.3f} "
    f"waitMsPerFrame={guest_wait_ms / frames:.6f} "
    f"waitMaxCumulativeMs={guest_last['waitMaxMs']:.3f}"
)
if diagnostic_stats_mode == 0:
    if hle:
        raise SystemExit("HLE clean counters present while diagnostic stats are disabled")
    metrics["hleClean"] = {
        "snapshots": 0,
        "diagnosticStats": False,
        "boundaryCommitted": 0,
        "committedSyncShare": 0.0,
    }
    destination.write_text(json.dumps(metrics, sort_keys=True) + "\n", encoding="utf-8")
    if mode == 0:
        print("HLE_CLEAN_LOG_BASELINE_OK enabled=0 counters=absent diagnosticStats=0")
    else:
        print("HLE_CLEAN_LOG_PERFORMANCE_OK enabled=1 counters=absent diagnosticStats=0")
    raise SystemExit(0)

if mode == 0:
    if hle:
        raise SystemExit("HLE clean counters present in diagnostic baseline measurement window")
    metrics["hleClean"] = {
        "snapshots": 0,
        "diagnosticStats": True,
        "firstActive": 0,
        "endActive": 0,
        "eligibleDelta": 0,
        "avoidedSyncDelta": 0,
        "committedDelta": 0,
        "abortedOrUnaccounted": 0,
        "boundaryCommitted": 0,
        "committedSyncShare": 0.0,
        "minimumCommittedSyncShare": minimum_coverage,
        "coverageAccepted": False,
    }
    destination.write_text(json.dumps(metrics, sort_keys=True) + "\n", encoding="utf-8")
    print("HLE_CLEAN_LOG_BASELINE_OK enabled=0 counters=absent diagnosticStats=1")
    raise SystemExit(0)

if len(hle) < 2:
    raise SystemExit(f"HLE clean measurement snapshots insufficient: {len(hle)}")
first, last = hle[0], hle[-1]
for key in hle_required:
    if key == "active":
        continue
    if last[key] < first[key]:
        raise SystemExit(f"HLE clean counter regressed: {key} {first[key]} -> {last[key]}")
delta = {key: last[key] - first[key] for key in hle_required}
first_active = first["active"]
end_active = last["active"]
eligible_delta = delta["eligible"]
avoided_sync_delta = delta["avoidedSync"]
committed_delta = delta["committed"]
if first_active != 0 or end_active != 0:
    raise SystemExit(
        "measurement boundary has active HLE tickets: "
        f"firstActive={first_active} endActive={end_active}"
    )
if eligible_delta != avoided_sync_delta:
    raise SystemExit(
        "measurement prepared counter mismatch: "
        f"eligibleDelta={eligible_delta} avoidedSyncDelta={avoided_sync_delta}"
    )
aborted_or_unaccounted = (
    first_active + eligible_delta - committed_delta - end_active
)
if aborted_or_unaccounted != 0:
    raise SystemExit(
        "measurement has aborted/unaccounted HLE tickets: "
        f"value={aborted_or_unaccounted} eligibleDelta={eligible_delta} "
        f"committedDelta={committed_delta} firstActive={first_active} "
        f"endActive={end_active}"
    )
boundary_committed = committed_delta - first_active + end_active
if not (delta["attempts"] >= eligible_delta and boundary_committed > 0):
    raise SystemExit(f"measurement counter ordering invalid: {delta}")
page_delta = delta["pages"]
if not (boundary_committed <= page_delta <= boundary_committed * 16):
    raise SystemExit(f"measurement committed page bound invalid: {delta}")
if delta["bytes"] <= 0 or last["activeMax"] <= 0 or end_active > last["activeMax"]:
    raise SystemExit(f"measurement counter liveness/concurrency invalid: {delta}")
fallback_total = sum(value for key, value in delta.items() if key.startswith("fallback"))
classification_skew = delta["attempts"] - delta["eligible"] - fallback_total
skew_limit = max(64, math.ceil(delta["attempts"] * 0.01))
if abs(classification_skew) > skew_limit:
    raise SystemExit(
        f"measurement attempt classification skew invalid: skew={classification_skew} "
        f"limit={skew_limit} delta={delta}"
    )
denominator = guest_requests + boundary_committed
coverage = boundary_committed / denominator if denominator else 0.0
eligible_ratio = eligible_delta / delta["attempts"]
committed_ratio = boundary_committed / delta["attempts"]
shader_contention_ratio = delta["fallbackShader"] / delta["attempts"]
metrics["hleClean"] = {
    "snapshots": len(hle),
    "diagnosticStats": True,
    "deltas": delta,
    "firstActive": first_active,
    "endActive": end_active,
    "eligibleDelta": eligible_delta,
    "avoidedSyncDelta": avoided_sync_delta,
    "committedDelta": committed_delta,
    "abortedOrUnaccounted": aborted_or_unaccounted,
    "boundaryCommitted": boundary_committed,
    "fallbackTotal": fallback_total,
    "classificationSkew": classification_skew,
    "eligiblePerAttempt": eligible_ratio,
    "committedPerAttempt": committed_ratio,
    "shaderContentionPerAttempt": shader_contention_ratio,
    "committedSyncShare": coverage,
    "minimumCommittedSyncShare": minimum_coverage,
    "coverageAccepted": coverage >= minimum_coverage,
}
destination.write_text(json.dumps(metrics, sort_keys=True) + "\n", encoding="utf-8")
print(
    "HLE_CLEAN_COUNTERS_OK "
    + " ".join(f"delta_{key}={delta[key]}" for key in hle_required)
    + f" firstActive={first_active} endActive={end_active} "
    + f"abortedOrUnaccounted={aborted_or_unaccounted} "
    + f"boundaryCommitted={boundary_committed} fallbackTotal={fallback_total} "
    + f"classificationSkew={classification_skew}"
)
print(
    "HLE_CLEAN_WINDOW_COVERAGE "
    f"eligiblePerAttempt={eligible_ratio:.6f} committedPerAttempt={committed_ratio:.6f} "
    f"shaderContentionPerAttempt={shader_contention_ratio:.6f} "
    f"committedSyncShare={coverage:.6f} minimum={minimum_coverage:.6f} "
    f"actualSync={guest_requests} boundaryCommitted={boundary_committed}"
)
if coverage < minimum_coverage:
    raise SystemExit(
        f"HLE clean committed coverage below threshold: {coverage:.6f} < {minimum_coverage:.6f}"
    )
PY
    local counter_status=$?
    if (( counter_status != 0 )); then
        return "$counter_status"
    fi
    echo "HLE_CLEAN_PROCESS_ENV expected=$hle_clean_mode verified=1 proof=consumer-getenv-or-marker-contract"
}

validate_async_shader_retranslation_log_contract() {
    local log_file="$1"
    local expected_fast_tier=$((1 - async_shader_disable_mode))
    local expected_hle_guard="$hle_clean_mode"
    local config_token="[EXECUTOR_ASYNC_SHADER_RETRANSLATION_CONFIG] disabled=$async_shader_disable_mode fastTierAllowed=$expected_fast_tier hleGuardRequested=$expected_hle_guard hleGuardEnabled=$expected_hle_guard"
    if ! grep -Fq "$config_token" "$log_file"; then
        echo "ASYNC_SHADER_RETRANSLATION_CONFIG_LOG_MISSING expected=$config_token"
        return 1
    fi
    if [[ "$async_shader_disable_mode" == 1 ]]; then
        if ! grep -Eq '\[EXECUTOR_ASYNC_SHADER_RETRANSLATION\] source=sentinel .*enabled=0 disabled=1' \
                "$log_file"; then
            echo 'ASYNC_SHADER_RETRANSLATION_DISABLE_LOG_MISSING'
            return 1
        fi
        echo 'ASYNC_SHADER_RETRANSLATION_LOG_OK disabled=1 queue=off'
        return 0
    fi
    if grep -Eq '\[EXECUTOR_ASYNC_SHADER_RETRANSLATION\].*disabled=1' "$log_file" ||
       ! grep -Eq '\[EXECUTOR_ASYNC_PIPELINE_CONFIG\].*shaderRetranslation=async' \
            "$log_file"; then
        echo 'ASYNC_SHADER_RETRANSLATION_DEFAULT_LOG_INVALID'
        return 1
    fi
    echo 'ASYNC_SHADER_RETRANSLATION_LOG_OK disabled=0 queue=async'
}

validate_strict_phase_artifacts() {
    [[ "$strict_phase_gates" != 1 ]] && return 0
    local log_file=".codex-bb-$tag-logcat.txt"
    local measure_log=".codex-bb-$tag-measure-logcat.txt"
    local pmu_out=".codex-bb-$tag-pmu.out"
    local pmu_err=".codex-bb-$tag-pmu.err"
    local sf_file=".codex-bb-$tag-sf.txt"
    local sf_tail=".codex-bb-$tag-sf-tail.txt"
    local scene_file=".codex-bb-$tag-scene.txt"
    local start_image=".codex-bb-$tag-start.png"
    local start_graphics=".codex-bb-$tag-start-graphics.json"
    local image=".codex-bb-$tag-end.png"
    local graphics=".codex-bb-$tag-end-graphics.json"

    if [[ "${ENABLE_RECORD:-0}" == 1 ]] ||
       ! grep -q 'Total test time:' "$pmu_out" ||
       grep -Eqi '<not counted>|not supported|permission denied' "$pmu_out" "$pmu_err"; then
        echo 'STRICT_PMU_COVERAGE_INVALID'
        return 1
    fi
    local frames dropped tail_frames tail_dropped sf_start sf_end sf_seconds minimum_frames
    local tail_increment minimum_tail_frames max_present_gap
    frames=$(game_surface_frames_from_file "$sf_file")
    dropped=$(game_dropped_frames_from_file "$sf_file")
    tail_frames=$(game_surface_frames_from_file "$sf_tail")
    tail_dropped=$(game_dropped_frames_from_file "$sf_tail")
    sf_start=$(awk '/^statsStart = / {print $3; exit}' "$sf_file")
    sf_end=$(awk '/^statsEnd = / {print $3; exit}' "$sf_file")
    if [[ -z "$sf_start" || -z "$sf_end" ]]; then
        echo 'STRICT_SF_WINDOW_MISSING'
        return 1
    fi
    sf_seconds=$((sf_end - sf_start))
    if (( sf_seconds < window - 3 || sf_seconds > window + 8 )); then
        echo "STRICT_SF_WINDOW_INVALID seconds=$sf_seconds expected=$window"
        return 1
    fi
    minimum_frames=$((window * ${BB_MIN_PHASE_FPS:-5}))
    if (( frames < minimum_frames )); then
        echo "STRICT_SF_FRAME_COUNT_LOW frames=$frames minimum=$minimum_frames"
        return 1
    fi
    if (( dropped != 0 || tail_dropped != 0 )); then
        echo "STRICT_SF_DROPPED_FRAMES first=$dropped tail=$tail_dropped"
        return 1
    fi
    if (( tail_frames <= frames )); then
        echo "STRICT_ZERO_FPS_FREEZE before=$frames after=$tail_frames"
        return 1
    fi
    tail_increment=$((tail_frames - frames))
    minimum_tail_frames=$((${strict_tail_seconds:-3} * ${BB_MIN_TAIL_FPS:-5}))
    if (( tail_increment < minimum_tail_frames )); then
        echo "STRICT_TAIL_FRAME_RATE_LOW frames=$tail_increment minimum=$minimum_tail_frames"
        return 1
    fi
    if ! max_present_gap=$(game_max_present_gap_ms_from_file "$sf_file"); then
        echo 'STRICT_PRESENT_GAP_HISTOGRAM_MISSING'
        return 1
    fi
    if (( max_present_gap > ${BB_MAX_PRESENT_GAP_MS:-500} )); then
        echo "STRICT_PRESENT_GAP_TOO_HIGH maxMs=$max_present_gap limitMs=${BB_MAX_PRESENT_GAP_MS:-500}"
        return 1
    fi
    if ! grep -q 'gameState loading=false gameplay=true' "$log_file" ||
       ! grep -q 'serial=CUSA03173' "$log_file" ||
       ! grep -q 'target=960x544' "$log_file"; then
        echo 'STRICT_GAMEPLAY_IDENTITY_MISSING title=CUSA03173 target=960x544'
        return 1
    fi
    if (( $(grep -c '\[EXECUTOR_LIVE_FPS\]' "$log_file" || true) < ${BB_MIN_WARM_LIVE_SAMPLES:-8} )); then
        echo 'STRICT_WARMUP_LIVE_SAMPLES_LOW'
        return 1
    fi
    if ! grep -Eq '\[EXECUTOR_ADPF_TARGET\].*aggressive40Hz=1.*targetNs=25000000.*sessionActive=1.*updateResult=0' "$log_file" ||
       ! grep -Eq 'vivo mode=performance-hint-4241 transport=binder result=(acquired|renewed)' "$log_file"; then
        echo 'STRICT_PLATFORM_POLICY_MISSING expected=ADPF25,Vivo4241'
        return 1
    fi
    if grep -Eq 'rootBoost .*applied=true|vivo mode=moderate-cpu-floor .*result=(acquired|renewed)|result=(acquire-failed|renewal-failed|release-failed|fallback-acquired|release-retry-exhausted)' \
            "$log_file"; then
        echo 'STRICT_PLATFORM_POLICY_CONFOUND_OR_FAILURE'
        return 1
    fi
    local fatal_pattern='FATAL EXCEPTION|Fatal signal|signal [0-9]+ \(SIG(BUS|SEGV)\)|am_crash|am_anr|ANR in|Application Not Responding|fex-activation-(failed|stranded)|fallback=current|VK_ERROR_DEVICE_LOST'
    if grep -Eqi "$fatal_pattern" "$log_file"; then
        grep -Ei "$fatal_pattern" "$log_file" | tail -40 >&2 || true
        echo 'STRICT_CRASH_ANR_DEVICE_LOST_OR_FALLBACK'
        return 1
    fi
    if ! write_builder_scene_metrics "$measure_log" "$scene_file"; then
        echo 'STRICT_NO_HEAVY_GAMEPLAY_WORKLOAD'
        return 1
    fi
    local builder_units
    builder_units=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^builderUnitsPerFrame=/){split($i,a,"="); print a[2]}}' "$scene_file")
    if ! awk -v value="$builder_units" -v minimum="${BB_MIN_BUILDER_UNITS_PER_FRAME:-400}" \
            'BEGIN { exit !(value + 0 >= minimum + 0) }'; then
        echo "STRICT_LIGHT_WORKLOAD builderUnitsPerFrame=$builder_units"
        return 1
    fi
    validate_png_image "$start_image" "$start_graphics" || return 1
    validate_png_image "$image" "$graphics" || return 1
    validate_async_shader_retranslation_log_contract "$log_file" || return 1
    validate_hle_clean_log_contract "$log_file" "$measure_log" "$frames" || return 1
    printf 'STRICT_PHASE_OK mode=%s asyncShaderRetranslationDisabled=%s sfSeconds=%s frames=%s dropped=%s tailFrames=%s tailIncrement=%s maxPresentGapMs=%s builderUnitsPerFrame=%s\n' \
        "$hle_clean_mode" "$async_shader_disable_mode" "$sf_seconds" "$frames" "$dropped" "$tail_frames" \
        "$tail_increment" "$max_present_gap" "$builder_units"
}

wait_device_file() {
    local path="$1"
    local attempts="$2"
    adb_device shell "run-as app.lsx4.android sh -c '
        i=0
        while [ ! -f \"$path\" ] && [ \$i -lt $attempts ]; do
            i=\$((i + 1))
            sleep 1
        done
        test -f \"$path\"'" >/dev/null 2>&1
}

navigation_last_metrics=''

assert_navigation_target() {
    local expected_pid="$1"
    local phase="$2"
    local state_file=".codex-bb-$tag-nav-$phase-state.txt"
    local state actual_pid
    if ! state=$(adb_device shell \
            "printf 'pid='; pidof app.lsx4.android:game 2>/dev/null || true; printf '\n'; \
             dumpsys window | grep -m1 -E 'mCurrentFocus=.*GameActivity|mFocusedApp=.*GameActivity'; \
             dumpsys activity activities | grep -m1 -E 'topResumedActivity=.*GameActivity|mResumedActivity=.*GameActivity'" \
            2>/dev/null); then
        echo "NAV_TARGET_QUERY_FAILED phase=$phase" >&2
        return 1
    fi
    state=${state//$'\r'/}
    printf '%s\n' "$state" > "$state_file"
    actual_pid=$(awk -F= '/^pid=/{print $2; exit}' "$state_file" | awk '{print $1}')
    if [[ -z "$actual_pid" || "$actual_pid" != "$expected_pid" ]]; then
        echo "NAV_PID_CHANGED phase=$phase expected=$expected_pid actual=${actual_pid:-absent}" >&2
        return 1
    fi
    if ! grep -Eq 'app[.]lsx4[.]android(/app[.]lsx4[.]android|/)[.]GameActivity' \
            "$state_file"; then
        echo "NAV_GAME_NOT_FOREGROUND phase=$phase pid=$actual_pid artifact=$state_file" >&2
        return 1
    fi
}

wait_navigation_game_process() {
    local timeout_seconds="$1"
    local deadline=$(( $(date +%s) + timeout_seconds ))
    local candidate=''
    while (( $(date +%s) < deadline )); do
        candidate=$(adb_device shell 'pidof app.lsx4.android:game 2>/dev/null || true' \
            2>/dev/null | tr -d '\r' | awk '{print $1}')
        if [[ -n "$candidate" ]] && assert_navigation_target "$candidate" boot; then
            game_pid="$candidate"
            echo "NAV_GAME_PROCESS_READY pid=$game_pid t=$(( $(date +%s) - start_time ))"
            return 0
        fi
        sleep "${BB_NAV_POLL_SECONDS:-3}"
    done
    echo "NAV_GAME_PROCESS_TIMEOUT seconds=$timeout_seconds" >&2
    return 1
}

wait_navigation_renderer() {
    local expected_pid="$1"
    local timeout_seconds="$2"
    local deadline=$(( $(date +%s) + timeout_seconds ))
    local boot_log=".codex-bb-$tag-nav-boot-logcat.txt"
    local live_samples loading_ready
    while (( $(date +%s) < deadline )); do
        assert_navigation_target "$expected_pid" renderer || return 1
        if adb_device logcat -b all -d -v brief > "$boot_log" 2>/dev/null; then
            if grep -Eqi 'FATAL EXCEPTION|Fatal signal|am_crash|am_anr|VK_ERROR_DEVICE_LOST' \
                    "$boot_log"; then
                echo "NAV_RENDERER_FATAL artifact=$boot_log" >&2
                return 1
            fi
            live_samples=$(grep -c '\[EXECUTOR_LIVE_FPS\]' "$boot_log" || true)
            loading_ready=0
            grep -q 'LSX4-PlatformPerf: gameState loading=false gameplay=true' \
                "$boot_log" && loading_ready=1
            echo "NAV_RENDERER_PROBE pid=$expected_pid loading=$loading_ready liveSamples=$live_samples"
            if (( loading_ready == 1 && live_samples >= 2 )); then
                echo "NAV_RENDERER_READY pid=$expected_pid liveSamples=$live_samples t=$(( $(date +%s) - start_time ))"
                return 0
            fi
        fi
        sleep "${BB_NAV_POLL_SECONDS:-3}"
    done
    echo "NAV_RENDERER_TIMEOUT pid=$expected_pid seconds=$timeout_seconds artifact=$boot_log" >&2
    return 1
}

navigation_wait_minimum() {
    local expected_pid="$1"
    local since_epoch="$2"
    local minimum_seconds="$3"
    local phase="$4"
    local deadline=$(( since_epoch + minimum_seconds ))
    local now remaining chunk
    while :; do
        now=$(date +%s)
        (( now >= deadline )) && break
        assert_navigation_target "$expected_pid" "$phase-minimum" || return 1
        remaining=$(( deadline - now ))
        chunk=$(( remaining > 10 ? 10 : remaining ))
        sleep "$chunk"
    done
    assert_navigation_target "$expected_pid" "$phase-minimum-done" || return 1
    echo "NAV_MINIMUM_REACHED phase=$phase seconds=$minimum_seconds t=$(( $(date +%s) - start_time ))"
}

navigation_png_metrics() {
    python3 - "$1" "$2" <<'PY'
import json
import pathlib
import sys

try:
    from PIL import Image, ImageStat
except ImportError:
    raise SystemExit(3)

source = pathlib.Path(sys.argv[1])
destination = pathlib.Path(sys.argv[2])
with Image.open(source) as image:
    image.load()
    if image.format != "PNG" or image.width < 1000 or image.height < 500:
        raise SystemExit("invalid navigation screenshot")
    luma = image.convert("L")
    central = luma.crop((
        int(image.width * 0.12), int(image.height * 0.15),
        int(image.width * 0.88), int(image.height * 0.85),
    ))
histogram = central.histogram()
samples = central.width * central.height
coarse = [sum(histogram[index:index + 16]) / samples for index in range(0, 256, 16)]
stats = ImageStat.Stat(central)
result = {
    "width": image.width,
    "height": image.height,
    "central_mean_luma": stats.mean[0],
    "central_stddev_luma": stats.stddev[0],
    "central_black_fraction": sum(histogram[:12]) / samples,
    "central_histogram": coarse,
}
destination.write_text(json.dumps(result, sort_keys=True) + "\n", encoding="utf-8")
PY
}

navigation_metrics_nonblank() {
    python3 - "$1" <<'PY'
import json
import pathlib
import sys

value = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
mean = float(value["central_mean_luma"])
stddev = float(value["central_stddev_luma"])
black = float(value["central_black_fraction"])
ready = int(stddev >= 5.0 and black <= 0.965)
print(f"{ready} {mean:.6f} {stddev:.6f} {black:.6f}")
PY
}

navigation_metrics_distance() {
    python3 - "$1" "$2" <<'PY'
import json
import pathlib
import sys

left = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
right = json.loads(pathlib.Path(sys.argv[2]).read_text(encoding="utf-8"))
left_hist = left["central_histogram"]
right_hist = right["central_histogram"]
if len(left_hist) != 16 or len(right_hist) != 16:
    raise SystemExit("invalid central histogram")
variation = sum(abs(float(a) - float(b)) for a, b in zip(left_hist, right_hist)) / 2.0
mean_delta = abs(float(left["central_mean_luma"]) - float(right["central_mean_luma"])) / 255.0
print(f"{0.8 * variation + 0.2 * mean_delta:.9f}")
PY
}

wait_for_navigation_screen() {
    local phase="$1"
    local expected_pid="$2"
    local timeout_seconds="$3"
    local reference_metrics="${4:-}"
    local require_change="${5:-0}"
    local probe_image=".codex-bb-$tag-nav-$phase-probe.png"
    local probe_metrics=".codex-bb-$tag-nav-$phase-probe.json"
    local previous_metrics=".codex-bb-$tag-nav-$phase-previous.json"
    local validate_log=".codex-bb-$tag-nav-$phase-validate.txt"
    local ready_image=".codex-bb-$tag-nav-$phase-ready.png"
    local ready_metrics=".codex-bb-$tag-nav-$phase-ready.json"
    local deadline=$(( $(date +%s) + timeout_seconds ))
    local stable_required="${BB_NAV_STABLE_SAMPLES:-2}"
    local stable_count=0 change_seen=$(( require_change == 0 ? 1 : 0 ))
    local have_previous=0 attempt=0
    local nonblank_info nonblank mean stddev black previous_distance reference_distance
    while (( $(date +%s) < deadline )); do
        attempt=$((attempt + 1))
        assert_navigation_target "$expected_pid" "$phase" || return 1
        if ! adb_device exec-out screencap -p > "$probe_image" 2>/dev/null; then
            echo "NAV_FRAME_INVALID phase=$phase attempt=$attempt artifact=$probe_image detail=screencap-failed"
            stable_count=0
            sleep "${BB_NAV_POLL_SECONDS:-3}"
            continue
        fi
        if ! navigation_png_metrics "$probe_image" "$probe_metrics" > "$validate_log" 2>&1 &&
           ! validate_png_image "$probe_image" "$probe_metrics" > "$validate_log" 2>&1; then
            echo "NAV_FRAME_INVALID phase=$phase attempt=$attempt artifact=$probe_image detail=$(tail -1 "$validate_log" 2>/dev/null)"
            stable_count=0
            sleep "${BB_NAV_POLL_SECONDS:-3}"
            continue
        fi
        if ! nonblank_info=$(navigation_metrics_nonblank "$probe_metrics"); then
            echo "NAV_METRICS_INVALID phase=$phase attempt=$attempt artifact=$probe_metrics" >&2
            return 1
        fi
        read -r nonblank mean stddev black <<< "$nonblank_info"
        previous_distance='-'
        reference_distance='-'
        if (( have_previous == 1 )); then
            previous_distance=$(navigation_metrics_distance "$previous_metrics" "$probe_metrics") || return 1
        fi
        if [[ -n "$reference_metrics" && -s "$reference_metrics" ]]; then
            reference_distance=$(navigation_metrics_distance "$reference_metrics" "$probe_metrics") || return 1
            if awk -v value="$reference_distance" -v minimum="$navigation_change_distance" \
                    'BEGIN { exit !(value >= minimum) }'; then
                change_seen=1
            fi
        fi
        if [[ "$nonblank" == 1 && "$previous_distance" != '-' ]] &&
           awk -v value="$previous_distance" -v maximum="$navigation_stable_distance" \
               'BEGIN { exit !(value <= maximum) }'; then
            stable_count=$((stable_count + 1))
        else
            stable_count=0
        fi
        echo "NAV_FRAME phase=$phase attempt=$attempt nonblank=$nonblank mean=$mean stddev=$stddev black=$black previousDistance=$previous_distance referenceDistance=$reference_distance changed=$change_seen stable=$stable_count/$stable_required"
        if [[ "$nonblank" == 1 ]] && (( change_seen == 1 && stable_count >= stable_required )); then
            cp -f "$probe_image" "$ready_image"
            cp -f "$probe_metrics" "$ready_metrics"
            navigation_last_metrics="$ready_metrics"
            echo "NAV_PHASE_READY phase=$phase image=$ready_image metrics=$ready_metrics t=$(( $(date +%s) - start_time ))"
            return 0
        fi
        cp -f "$probe_metrics" "$previous_metrics"
        have_previous=1
        sleep "${BB_NAV_POLL_SECONDS:-3}"
    done
    cp -f "$probe_image" ".codex-bb-$tag-nav-$phase-timeout.png" 2>/dev/null || true
    cp -f "$probe_metrics" ".codex-bb-$tag-nav-$phase-timeout.json" 2>/dev/null || true
    echo "NAV_PHASE_TIMEOUT phase=$phase seconds=$timeout_seconds changed=$change_seen stable=$stable_count/$stable_required" >&2
    return 1
}

navigation_press_cross() {
    local phase="$1"
    local expected_pid="$2"
    assert_navigation_target "$expected_pid" "$phase-before-input" || return 1
    adb_device shell 'input swipe 2903 1231 2903 1231 1000' >/dev/null || return 1
    sleep 1
    assert_navigation_target "$expected_pid" "$phase-after-input" || return 1
}

cleanup() {
    cleanup_profile_markers || true
    stop_game || true
}

verify_canonical_fex_host || exit 51
verify_vivo_device_identity || exit 69
trap cleanup EXIT
stop_game || exit 65
run_performance_preflight full || exit 72
cleanup_profile_markers
remove_and_verify_strict_confounders || exit 45
set_async_shader_retranslation_marker_state || exit 67
set_hle_clean_marker_state || exit 46
set_hle_clean_stats_marker_state || exit 71
assert_strict_root_boost_disabled || exit 64
adb_device shell input keyevent KEYCODE_WAKEUP >/dev/null 2>&1 || true
adb_device shell input keyevent 82 >/dev/null 2>&1 || true
if [[ "${ENABLE_PERF_MAP:-0}" == 1 ]]; then
    adb_device shell \
        "run-as app.lsx4.android sh -c 'mkdir -p files/lsx4-home; touch files/lsx4-home/diag-jit-helper-perf-map'" \
        >/dev/null
else
    adb_device shell \
        "run-as app.lsx4.android rm -f files/lsx4-home/diag-jit-helper-perf-map" \
        >/dev/null 2>&1 || true
fi
if [[ "${BB_PROFILE_INVALIDATION_SOURCES:-0}" == 1 ]]; then
    adb_device shell \
        "run-as app.lsx4.android touch files/lsx4-home/run-profile-guest-invalidation-sources" \
        >/dev/null || {
        echo 'SOURCE_PROFILE_MARKER_FAILED'
        exit 26
    }
else
    adb_device shell \
        "run-as app.lsx4.android rm -f files/lsx4-home/run-profile-guest-invalidation-sources" \
        >/dev/null 2>&1 || true
fi
if [[ "${BB_PROFILE_INVALIDATION_OVERLAP:-0}" == 1 ]]; then
    adb_device shell \
        "run-as app.lsx4.android mkdir -p '$overlap_control_dir'" \
        >/dev/null || {
        echo 'OVERLAP_CONTROL_DIR_FAILED'
        exit 27
    }
    adb_device shell \
        "run-as app.lsx4.android touch files/lsx4-home/run-profile-guest-invalidation-overlap" \
        >/dev/null || {
        echo 'OVERLAP_PROFILE_MARKER_FAILED'
        exit 28
    }
else
    adb_device shell \
        "run-as app.lsx4.android rm -f files/lsx4-home/run-profile-guest-invalidation-overlap" \
        >/dev/null 2>&1 || true
fi
marker_listing=$(adb_device shell 'run-as app.lsx4.android ls -1 files/lsx4-home' \
    2>/dev/null | tr -d '\r' | grep -E '^(run-|diag-)' || true)
echo "MARKERS=$(echo "$marker_listing" | paste -sd, -)"
forbidden_marker_listing=$(echo "$marker_listing" | grep -E \
    '^(run-enable-fex-buffer-capture-ack|run-enable-fex-deferred-write-epoch|run-enable-bounded-guest-invalidation-drain|run-force-fex-software-tso|run-enable-fex-relaxed-vector-tso-ab|run-enable-fex-relaxed-memcpyset-tso-ab)$' || true)
if [[ -n "$forbidden_marker_listing" ]]; then
    echo "FORBIDDEN_MARKERS=$(echo "$forbidden_marker_listing" | paste -sd, -)"
    exit 19
fi
if [[ "$hle_clean_mode" != 1 ]] &&
   echo "$marker_listing" | grep -Fxq 'run-enable-hle-clean-buffer-fastpath'; then
    echo 'FORBIDDEN_UNMANAGED_HLE_CLEAN_MARKER=run-enable-hle-clean-buffer-fastpath'
    exit 19
fi
if [[ "$hle_clean_stats_mode" != 1 ]] &&
   echo "$marker_listing" | grep -Fxq 'run-diagnostic-hle-clean-buffer-stats'; then
    echo 'FORBIDDEN_UNMANAGED_HLE_CLEAN_STATS_MARKER=run-diagnostic-hle-clean-buffer-stats'
    exit 19
fi
if [[ "$async_shader_disable_mode" != 1 ]] &&
   echo "$marker_listing" | grep -Fxq 'run-disable-async-shader-retranslation'; then
    echo 'FORBIDDEN_UNMANAGED_ASYNC_SHADER_RETRANSLATION_MARKER=run-disable-async-shader-retranslation'
    exit 19
fi
unexpected_diag=$(echo "$marker_listing" | grep '^diag-' | \
    grep -Ev '(\.off|\.diag-off)$' || true)
if [[ -n "$unexpected_diag" ]]; then
    echo "UNEXPECTED_DIAGNOSTIC_MARKERS=$(echo "$unexpected_diag" | paste -sd, -)"
    exit 6
fi
adb_device logcat -b all -c

start_time=$(date +%s)
graphics_override_args=''
if [[ "${BB_GRAPHICS_OVERRIDE:-0}" == 1 ]]; then
    graphics_override_args='--ez profile_graphics_override true'
    for override_spec in \
        'BB_DISABLE_DYNAMIC_SHADOWS:profile_disable_dynamic_shadows' \
        'BB_DISABLE_SSAO:profile_disable_ssao' \
        'BB_DISABLE_MOTION_BLUR:profile_disable_motion_blur' \
        'BB_DISABLE_DEPTH_OF_FIELD:profile_disable_depth_of_field' \
        'BB_DISABLE_ANTI_ALIASING:profile_disable_anti_aliasing' \
        'BB_DISABLE_CHROMATIC_ABERRATION:profile_disable_chromatic_aberration'; do
        override_variable="${override_spec%%:*}"
        override_extra="${override_spec#*:}"
        override_value="${!override_variable:-false}"
        if [[ "$override_value" != true && "$override_value" != false ]]; then
            echo "INVALID_GRAPHICS_OVERRIDE=$override_variable:$override_value"
            exit 21
        fi
        graphics_override_args+=" --ez $override_extra $override_value"
    done
fi
adb_device shell "am start -n app.lsx4.android/.MainActivity \
    --ez autoload_existing_runtime true --ez autoinit_runtime true \
    --es scan_game_path $game --es launch_game_path $game \
    --es game_platform ps4 --ez fullscreen_render true \
    --ez embedded_aarch64_jit_backend true --ei render_resolution_mode 4 \
    $graphics_override_args" >/dev/null
echo 'LAUNCHED t=0'

if [[ "$adaptive_phase_gates" == 1 ]]; then
    echo "NAV_CONFIG adaptive=1 bootTimeout=${BB_NAV_BOOT_TIMEOUT:-180} phaseTimeout=${BB_NAV_PHASE_TIMEOUT:-180} pollSeconds=${BB_NAV_POLL_SECONDS:-3} stableSamples=${BB_NAV_STABLE_SAMPLES:-2} stableDistance=$navigation_stable_distance changeDistance=$navigation_change_distance"
    wait_navigation_game_process "${BB_NAV_BOOT_TIMEOUT:-180}" || exit 71
    if [[ "$skip_renderer_log_gate" == 1 ]]; then
        echo 'NAV_RENDERER_GATE skipped=1 reason=explicit-screenshot-gated-route'
    else
        wait_navigation_renderer "$game_pid" "${BB_NAV_BOOT_TIMEOUT:-180}" || exit 71
    fi
    navigation_wait_minimum "$game_pid" "$start_time" "${BB_WARNING_WAIT:-64}" warning || exit 71
    wait_for_navigation_screen warning "$game_pid" "${BB_NAV_PHASE_TIMEOUT:-180}" || exit 71
    warning_metrics="$navigation_last_metrics"
    navigation_press_cross warning "$game_pid" || exit 71
    echo "CROSS_WARNING t=$(( $(date +%s) - start_time )) adaptive=1"
    warning_cross_time=$(date +%s)

    navigation_wait_minimum "$game_pid" "$warning_cross_time" \
        "${BB_OFFLINE_WAIT:-30}" offline || exit 71
    wait_for_navigation_screen offline "$game_pid" "${BB_NAV_PHASE_TIMEOUT:-180}" \
        "$warning_metrics" 1 || exit 71
    offline_metrics="$navigation_last_metrics"
    navigation_press_cross offline "$game_pid" || exit 71
    echo "CROSS_OFFLINE t=$(( $(date +%s) - start_time )) adaptive=1"
    offline_cross_time=$(date +%s)

    navigation_wait_minimum "$game_pid" "$offline_cross_time" \
        "${BB_CONTINUE_WAIT:-22}" continue || exit 71
    wait_for_navigation_screen continue "$game_pid" "${BB_NAV_PHASE_TIMEOUT:-180}" \
        "$offline_metrics" 1 || exit 71
    continue_metrics="$navigation_last_metrics"
    navigation_press_cross continue "$game_pid" || exit 71
    echo "CROSS_CONTINUE t=$(( $(date +%s) - start_time )) adaptive=1"
else
    sleep "${BB_WARNING_WAIT:-64}"
    adb_device shell 'input swipe 2903 1231 2903 1231 1000' >/dev/null
    echo "CROSS_WARNING t=$(( $(date +%s) - start_time ))"
    sleep "${BB_OFFLINE_WAIT:-30}"
    adb_device shell 'input swipe 2903 1231 2903 1231 1000' >/dev/null
    echo "CROSS_OFFLINE t=$(( $(date +%s) - start_time ))"
    sleep "${BB_CONTINUE_WAIT:-22}"
    adb_device shell 'input swipe 2903 1231 2903 1231 1000' >/dev/null
    echo "CROSS_CONTINUE t=$(( $(date +%s) - start_time ))"
    game_pid=$(adb_device shell 'pidof app.lsx4.android:game' | tr -d '\r')
fi

if [[ -z "$game_pid" ]]; then
    echo 'NO_PROCESS'
    exit 4
fi
if ! assert_strict_runtime_marker_state; then
    echo 'STRICT_RUNTIME_MARKER_STATE_FAILED'
    exit 47
fi
assert_strict_root_boost_disabled || exit 64
if [[ "$strict_phase_gates" == 1 ]]; then
    if ! adb_device exec-out run-as app.lsx4.android cat "/proc/$game_pid/environ" \
            2>/dev/null | tr '\0' '\n' > ".codex-bb-$tag-environ.txt"; then
        echo 'PROCESS_ENVIRONMENT_CAPTURE_FAILED'
        exit 48
    fi
    if [[ "$hle_clean_mode" == 1 ]]; then
        # /proc/PID/environ describes the exec-time environment range and is
        # only advisory for a later libc setenv().  The enabled consumer is
        # proven below by attempts>0: that counter is behind the cached getenv
        # gate in PageManager.
        proc_env_visible=0
        grep -Fxq 'EXECUTOR_HLE_CLEAN_BUFFER_FASTPATH=1' \
            ".codex-bb-$tag-environ.txt" && proc_env_visible=1
        echo "HLE_CLEAN_PROCESS_ENV expected=1 verified=pending-counter procInitialVisible=$proc_env_visible"
    elif grep -Eq '^EXECUTOR_HLE_CLEAN_BUFFER_FASTPATH=(1|true|on)$' \
            ".codex-bb-$tag-environ.txt"; then
        echo 'HLE_CLEAN_PROCESS_ENV_ENABLED_IN_BASELINE'
        exit 50
    else
        echo 'HLE_CLEAN_PROCESS_ENV expected=0 verified=1 proof=marker-absent,consumer-inactive'
    fi
fi
post_launch_markers=$(adb_device shell 'run-as app.lsx4.android ls -1 files/lsx4-home' \
    2>/dev/null | tr -d '\r' | grep -E '^(run-|diag-)' || true)
echo "POST_LAUNCH_MARKERS=$(echo "$post_launch_markers" | paste -sd, -)"
post_launch_forbidden=$(echo "$post_launch_markers" | grep -E \
    '^(run-enable-fex-buffer-capture-ack|run-enable-fex-deferred-write-epoch|run-enable-bounded-guest-invalidation-drain|run-force-fex-software-tso|run-enable-fex-relaxed-vector-tso-ab|run-enable-fex-relaxed-memcpyset-tso-ab)$' || true)
if [[ -n "$post_launch_forbidden" ]]; then
    echo "POST_LAUNCH_FORBIDDEN_MARKERS=$(echo "$post_launch_forbidden" | paste -sd, -)"
    exit 19
fi
if [[ "$hle_clean_mode" != 1 ]] &&
   echo "$post_launch_markers" | grep -Fxq 'run-enable-hle-clean-buffer-fastpath'; then
    echo 'POST_LAUNCH_FORBIDDEN_UNMANAGED_HLE_CLEAN_MARKER=run-enable-hle-clean-buffer-fastpath'
    exit 19
fi
if [[ "$hle_clean_stats_mode" != 1 ]] &&
   echo "$post_launch_markers" | grep -Fxq 'run-diagnostic-hle-clean-buffer-stats'; then
    echo 'POST_LAUNCH_FORBIDDEN_UNMANAGED_HLE_CLEAN_STATS_MARKER=run-diagnostic-hle-clean-buffer-stats'
    exit 19
fi
if [[ "$async_shader_disable_mode" != 1 ]] &&
   echo "$post_launch_markers" | grep -Fxq 'run-disable-async-shader-retranslation'; then
    echo 'POST_LAUNCH_FORBIDDEN_UNMANAGED_ASYNC_SHADER_RETRANSLATION_MARKER=run-disable-async-shader-retranslation'
    exit 19
fi
loaded_so=$(adb_device shell "run-as app.lsx4.android cat /proc/$game_pid/maps" \
    2>/dev/null | tr -d '\r' | awk '/liblsx4_executor_android.so/{print $6; exit}')
loaded_sha=$(adb_device shell "run-as app.lsx4.android sha256sum '$loaded_so'" \
    2>/dev/null | tr -d '\r' | awk '{print $1}')
local_sha=$(sha256sum build/android-arm64-release/liblsx4_executor_android.so \
    2>/dev/null | awk '{print $1}')
echo "RUNTIME_SO=$loaded_so SHA_LOADED=$loaded_sha SHA_LOCAL=$local_sha"
if [[ -z "$loaded_sha" || "$loaded_sha" != "$local_sha" ]]; then
    echo 'RUNTIME_HASH_MISMATCH'
    exit 7
fi
verify_canonical_fex_runtime "$game_pid" || exit 52
capture_and_validate_fex_status || exit 53

role_run_ns() {
    local pattern="$1"
    adb_device shell "total=0
    for t in /proc/$game_pid/task/*; do
        n=\$(cat \$t/comm 2>/dev/null)
        case \"\$n\" in
            *'$pattern'*)
                set -- \$(cat \$t/schedstat 2>/dev/null)
                total=\$((total + \$1))
                ;;
        esac
    done
    echo \$total" 2>/dev/null | tr -d '\r'
}

# Current save starts at Iosefka's Clinic. Open the door before sampling so
# every run measures the same streaming boundary while the hunter moves.
door_settle_wait="${BB_DOOR_SETTLE_WAIT:-5}"
if [[ "$adaptive_phase_gates" == 1 ]]; then
    door_wait_start=$(date +%s)
    navigation_wait_minimum "$game_pid" "$door_wait_start" \
        "${BB_DOOR_WAIT:-65}" gameplay || exit 71
    wait_for_navigation_screen gameplay "$game_pid" "${BB_NAV_PHASE_TIMEOUT:-180}" \
        "$continue_metrics" 1 || exit 71
    navigation_press_cross open-door "$game_pid" || exit 71
    echo "CROSS_OPEN_DOOR t=$(( $(date +%s) - start_time )) adaptive=1"
    door_cross_time=$(date +%s)
    navigation_wait_minimum "$game_pid" "$door_cross_time" "$door_settle_wait" \
        door-settle || exit 71
else
    sleep "${BB_DOOR_WAIT:-65}"
    adb_device shell 'input swipe 2903 1231 2903 1231 1000' >/dev/null
    echo "CROSS_OPEN_DOOR t=$(( $(date +%s) - start_time ))"
    sleep "$door_settle_wait"
fi

havok_start=$(role_run_ns HavokWorker)
gpucomm_start=$(role_run_ns GpuComm)
role_probe_wait="${BB_ROLE_PROBE_WAIT:-3}"
sleep "$role_probe_wait"
havok_end=$(role_run_ns HavokWorker)
gpucomm_end=$(role_run_ns GpuComm)
role_probe_divisor=$(( role_probe_wait > 0 ? role_probe_wait * 10000000 : 10000000 ))
havok_pct=$(( (havok_end - havok_start) / role_probe_divisor ))
gpucomm_pct=$(( (gpucomm_end - gpucomm_start) / role_probe_divisor ))
echo "GAMEPLAY_PROBE havok=$havok_pct gpucomm=$gpucomm_pct t=$(( $(date +%s) - start_time ))"

pre_measure_warmup="${BB_PRE_MEASURE_WARMUP:-0}"
if [[ ! "$pre_measure_warmup" =~ ^[0-9]+$ ]]; then
    echo "INVALID_PRE_MEASURE_WARMUP=$pre_measure_warmup"
    exit 54
fi
if (( pre_measure_warmup > 0 )); then
    sleep "$pre_measure_warmup"
fi
echo "WARMUP_DONE seconds=$pre_measure_warmup route=iosefka-door camera=unchanged"

if [[ "${BB_PROFILE_INVALIDATION_OVERLAP:-0}" == 1 ]]; then
    adb_device shell \
        "run-as app.lsx4.android touch '$overlap_control_dir/arm'" \
        >/dev/null || {
        echo 'OVERLAP_ARM_FAILED'
        exit 29
    }
    if ! wait_device_file "$overlap_control_dir/ready" 20; then
        echo 'OVERLAP_READY_TIMEOUT'
        exit 30
    fi
    overlap_ready=$(adb_device exec-out run-as app.lsx4.android cat \
        "$overlap_control_dir/ready" 2>/dev/null | tr -d '\r')
    echo "OVERLAP_READY=$overlap_ready"
    if [[ "$overlap_ready" != *'enabled=1'* ]]; then
        echo 'OVERLAP_NOT_ENABLED'
        exit 31
    fi
fi

# Android Studio can recreate process-tracker during the several-minute route.
# Re-clean immediately before opening the actual measurement window; CleanupOnly
# deliberately leaves the running LSX4 game untouched.
run_performance_preflight cleanup-only || exit 72

if ! adb_device exec-out screencap -p > ".codex-bb-$tag-start.png" 2>/dev/null; then
    if [[ "$strict_phase_gates" == 1 ]]; then
        echo 'STRICT_START_SCREENSHOT_CAPTURE_FAILED'
        exit 60
    fi
fi
adb_device shell 'dumpsys SurfaceFlinger --timestats -enable' >/dev/null 2>&1
adb_device shell 'dumpsys SurfaceFlinger --timestats -clear' >/dev/null 2>&1
adb_device logcat -b all -d -v threadtime \
    > ".codex-bb-$tag-startup-logcat.txt" 2>/dev/null
adb_device logcat -b all -c
measurement_input="${BB_MEASUREMENT_INPUT:-forward}"
echo "MEASURE pid=$game_pid t=$(( $(date +%s) - start_time )) window=$window input=$measurement_input"

analog_pid=''
if [[ "$measurement_input" == forward ]]; then
    adb_device_background shell \
        "input swipe 950 1245 950 1060 $((window * 1000))" >/dev/null 2>&1 || exit $?
    analog_pid="$background_adb_pid"
elif [[ "$measurement_input" != static ]]; then
    echo "INVALID_MEASUREMENT_INPUT=$measurement_input"
    exit 20
fi
if [[ "${ENABLE_RECORD:-0}" == 1 ]]; then
    profile_device_file="files/.codex-bb-$tag.data"
    record_extra=()
    if [[ "${ENABLE_OFFCPU:-0}" == 1 ]]; then
        record_extra+=(--trace-offcpu)
    fi
    adb_device shell "run-as app.lsx4.android rm -f '$profile_device_file'" \
        >/dev/null 2>&1 || true
    adb_device shell "run-as app.lsx4.android /system/bin/simpleperf record -p $game_pid \
        --duration $window -g ${record_extra[*]} -o '$profile_device_file'" \
        > ".codex-bb-$tag-profile.out" 2> ".codex-bb-$tag-profile.err"
else
    adb_device shell "run-as app.lsx4.android /system/bin/simpleperf stat -p $game_pid \
        --duration $window -e instructions:u,cpu-cycles:u,task-clock,context-switches,cpu-migrations,page-faults" \
        > ".codex-bb-$tag-pmu.out" 2> ".codex-bb-$tag-pmu.err"
fi
if [[ -n "$analog_pid" ]]; then
    wait "$analog_pid" || true
fi

if [[ "${BB_PROFILE_INVALIDATION_OVERLAP:-0}" == 1 ]]; then
    adb_device shell \
        "run-as app.lsx4.android touch '$overlap_control_dir/stop'" \
        >/dev/null || {
        echo 'OVERLAP_STOP_FAILED'
        exit 32
    }
    if ! wait_device_file "$overlap_control_dir/done" 30; then
        echo 'OVERLAP_DONE_TIMEOUT'
        exit 33
    fi
    overlap_done=$(adb_device exec-out run-as app.lsx4.android cat \
        "$overlap_control_dir/done" 2>/dev/null | tr -d '\r')
    echo "OVERLAP_DONE=$overlap_done"
    if [[ "$overlap_done" != *'dumped=1'* ]]; then
        overlap_error=$(adb_device exec-out run-as app.lsx4.android cat \
            "$overlap_control_dir/error" 2>/dev/null | tr -d '\r' || true)
        echo "OVERLAP_DUMP_FAILED=$overlap_error"
        exit 34
    fi
    overlap_local_bin=".codex-bb-$tag-invalidation-overlap.bin"
    overlap_local_json=".codex-bb-$tag-invalidation-overlap.json"
    if ! adb_device exec-out run-as app.lsx4.android cat "$overlap_trace_device" \
        > "$overlap_local_bin" 2>/dev/null || [[ ! -s "$overlap_local_bin" ]]; then
        echo 'OVERLAP_PULL_FAILED'
        exit 35
    fi
    if ! python3 tools/parse_invalidation_overlap_trace.py \
        --require-exact "$overlap_local_bin" > "$overlap_local_json"; then
        echo 'OVERLAP_PARSE_INEXACT'
        cat "$overlap_local_json" 2>/dev/null || true
        exit 36
    fi
    if ! python3 -c \
        'import json,sys; d=json.load(open(sys.argv[1], encoding="utf-8")); sys.exit(0 if d["committed_entries"] > 0 else 1)' \
        "$overlap_local_json"; then
        echo 'OVERLAP_EMPTY_TRACE'
        exit 37
    fi
    echo "OVERLAP_TRACE=$overlap_local_bin JSON=$overlap_local_json"
fi

if [[ "${BB_REPLAY_COLD_BOUNDARY:-0}" == 1 ]]; then
    adb_device shell 'dumpsys SurfaceFlinger --timestats -dump --maxlayers 8' \
        > ".codex-bb-$tag-first-sf.txt" 2>/dev/null
    return_window="${BB_RETURN_WINDOW:-$window}"
    adb_device shell "input swipe 950 1060 950 1245 $((return_window * 1000))" \
        >/dev/null 2>&1
    sleep "${BB_REPLAY_SETTLE_WAIT:-1}"
    adb_device shell 'dumpsys SurfaceFlinger --timestats -clear' >/dev/null 2>&1
    adb_device_background shell \
        "input swipe 950 1245 950 1060 $((window * 1000))" >/dev/null 2>&1 || exit $?
    replay_analog_pid="$background_adb_pid"
    adb_device shell "run-as app.lsx4.android /system/bin/simpleperf stat -p $game_pid \
        --duration $window -e instructions:u,cpu-cycles:u,task-clock,context-switches,cpu-migrations,page-faults" \
        > ".codex-bb-$tag-replay-pmu.out" 2> ".codex-bb-$tag-replay-pmu.err"
    wait "$replay_analog_pid" || true
fi

if [[ "${ENABLE_RECORD:-0}" == 1 ]]; then
    adb_device exec-out run-as app.lsx4.android cat "$profile_device_file" \
        > ".codex-bb-$tag.data" 2>/dev/null || true
    adb_device shell "run-as app.lsx4.android rm -f '$profile_device_file'" \
        >/dev/null 2>&1 || true
fi

adb_device shell 'dumpsys SurfaceFlinger --timestats -dump --maxlayers 8' \
    > ".codex-bb-$tag-sf.txt" 2>/dev/null
if [[ "$strict_phase_gates" == 1 ]]; then
    strict_tail_seconds="${BB_LIVENESS_TAIL_SECONDS:-3}"
    if [[ ! "$strict_tail_seconds" =~ ^[1-9][0-9]*$ ]]; then
        echo "INVALID_LIVENESS_TAIL=$strict_tail_seconds"
        exit 55
    fi
    sleep "$strict_tail_seconds"
    if ! adb_device shell 'dumpsys SurfaceFlinger --timestats -dump --maxlayers 8' \
            > ".codex-bb-$tag-sf-tail.txt" 2>/dev/null; then
        echo 'STRICT_SF_TAIL_CAPTURE_FAILED'
        exit 56
    fi
    final_pid=$(adb_device shell 'pidof app.lsx4.android:game' 2>/dev/null | \
        tr -d '\r[:space:]')
    if [[ "$final_pid" != "$game_pid" ]]; then
        echo "STRICT_PID_CHANGED expected=$game_pid actual=$final_pid"
        exit 57
    fi
    if ! adb_device shell \
            "dumpsys window | grep -m1 -E 'mCurrentFocus=.*GameActivity|mFocusedApp=.*GameActivity'; \
             dumpsys activity activities | grep -m1 -E 'topResumedActivity=.*GameActivity|mResumedActivity=.*GameActivity'" \
            > ".codex-bb-$tag-foreground.txt" 2>/dev/null ||
       ! grep -Eq 'app\.lsx4\.android(/app\.lsx4\.android|/)\.GameActivity' \
            ".codex-bb-$tag-foreground.txt"; then
        echo 'STRICT_GAME_NOT_FOREGROUND'
        exit 58
    fi
    assert_strict_runtime_marker_state || {
        echo 'STRICT_FINAL_MARKER_STATE_FAILED'
        exit 59
    }
fi
adb_device exec-out run-as app.lsx4.android cat files/jit-status.json \
    > ".codex-bb-$tag-jit-status.json" 2>/dev/null || true
adb_device shell "for z in /sys/class/thermal/thermal_zone*; do
    [ \"\$(cat \$z/type 2>/dev/null)\" = tz_shell ] && cat \$z/temp && break
done
cat /sys/devices/system/cpu/cpu6/cpufreq/scaling_cur_freq \
    /sys/devices/system/cpu/cpu7/cpufreq/scaling_cur_freq 2>/dev/null" \
    > ".codex-bb-$tag-annotation.txt" 2>/dev/null
if ! adb_device exec-out screencap -p > ".codex-bb-$tag-end.png" 2>/dev/null; then
    if [[ "$strict_phase_gates" == 1 ]]; then
        echo 'STRICT_SCREENSHOT_CAPTURE_FAILED'
        exit 60
    fi
fi

if [[ "$strict_phase_gates" == 1 ]]; then
    sleep 1
fi
adb_device logcat -b all -d -v threadtime \
    > ".codex-bb-$tag-measure-logcat.txt" 2>/dev/null
cat ".codex-bb-$tag-startup-logcat.txt" ".codex-bb-$tag-measure-logcat.txt" \
    > ".codex-bb-$tag-logcat.txt"
if [[ "$strict_phase_gates" == 1 ]]; then
    validate_strict_phase_artifacts || exit 61
fi
if [[ "${ENABLE_PERF_MAP:-0}" == 1 ]]; then
    adb_device exec-out run-as app.lsx4.android cat \
        "/data/data/app.lsx4.android/perf-$game_pid.map" \
        > "perf-$game_pid-$tag.map" 2>/dev/null || true
fi
cleanup_profile_markers
if ! cleanup_root_listing=$(adb_device shell \
    "run-as app.lsx4.android ls -1 '$profile_root'" 2>/dev/null); then
    echo 'PROFILE_CLEANUP_ROOT_VERIFY_FAILED'
    exit 39
fi
if ! cleanup_control_listing=$(adb_device shell \
    "run-as app.lsx4.android sh -c 'if [ -d \"$overlap_control_dir\" ]; then ls -1 \"$overlap_control_dir\"; fi'" \
    2>/dev/null); then
    echo 'PROFILE_CLEANUP_CONTROL_VERIFY_FAILED'
    exit 40
fi
remaining_profile_markers=$(echo "$cleanup_root_listing" | tr -d '\r' | \
    grep -E '^run-profile-guest-invalidation-(sources|overlap)$' || true)
remaining_control_markers=$(echo "$cleanup_control_listing" | tr -d '\r' | \
    grep -E '^(arm|ready|ready\.tmp|stop|done|error|trace-v1\.bin|trace-v1\.bin\.tmp)$' || true)
if [[ -n "$remaining_profile_markers" || -n "$remaining_control_markers" ]]; then
    echo "PROFILE_CLEANUP_FAILED root=$(echo "$remaining_profile_markers" | paste -sd,) control=$(echo "$remaining_control_markers" | paste -sd,)"
    exit 38
fi
echo 'PROFILE_MARKERS_CLEAN=1'
if [[ -n "$hle_clean_mode" ]] &&
   ! adb_device shell "run-as app.lsx4.android test ! -e '$hle_clean_marker'" >/dev/null; then
    echo 'HLE_CLEAN_MARKER_CLEANUP_FAILED'
    exit 63
fi
[[ -n "$hle_clean_mode" ]] && echo 'HLE_CLEAN_MARKER_CLEAN=1'
if ! adb_device shell \
        "run-as app.lsx4.android test ! -e '$hle_clean_stats_marker'" >/dev/null; then
    echo 'HLE_CLEAN_STATS_MARKER_CLEANUP_FAILED'
    exit 71
fi
if ! adb_device shell \
        "run-as app.lsx4.android test ! -e '$async_shader_disable_marker'" >/dev/null; then
    echo 'ASYNC_SHADER_RETRANSLATION_MARKER_CLEANUP_FAILED'
    exit 68
fi
echo 'ASYNC_SHADER_RETRANSLATION_MARKER_CLEAN=1'
if [[ "$keep_game_running" == 1 ]]; then
    final_running_pid=$(adb_device shell 'pidof app.lsx4.android:game' 2>/dev/null | \
        tr -d '\r[:space:]')
    if [[ "$final_running_pid" != "$game_pid" ]]; then
        echo "KEEP_RUNNING_PID_MISMATCH expected=$game_pid actual=$final_running_pid"
        exit 62
    fi
    echo "KEEP_RUNNING_BASELINE pid=$game_pid marker=absent exactGame=$game"
else
stop_game || exit 65
trap - EXIT
fi
if [[ "$keep_game_running" == 1 ]]; then
    trap - EXIT
fi
map_bytes=0
if [[ -f "perf-$game_pid-$tag.map" ]]; then
    map_bytes=$(wc -c < "perf-$game_pid-$tag.map")
fi
echo "DONE pid=$game_pid wall=$(( $(date +%s) - start_time )) map_bytes=$map_bytes hleClean=$hle_clean_mode asyncShaderRetranslationDisabled=$async_shader_disable_mode keepRunning=$keep_game_running"
if [[ "${ENABLE_RECORD:-0}" == 1 ]]; then
    cat ".codex-bb-$tag-profile.out" ".codex-bb-$tag-profile.err"
else
    cat ".codex-bb-$tag-pmu.out" ".codex-bb-$tag-pmu.err"
fi
grep -A8 -B2 'GameActivity.*BLAST' ".codex-bb-$tag-sf.txt" | head -20 || true
cat ".codex-bb-$tag-annotation.txt"
