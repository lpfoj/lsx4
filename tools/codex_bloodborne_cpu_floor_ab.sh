#!/usr/bin/env bash

set -u

export MSYS2_ARG_CONV_EXCL='*'

device="${ADB_SERIAL:-10AG263F1B001HM}"
window="${1:-60}"
tag="${2:-vivo-policy-ab-r84}"
game='/data/user/0/app.lsx4.android/files/lsx4-home/runtime-fs/user/app/CUSA03173/eboot.bin'
home='files/lsx4-home'
policy_marker="$home/run-use-vivo-moderate-cpu-floor"
legacy_hint_marker="$home/run-use-vivo-performance-hint-4241"
legacy_disable_marker="$home/run-disable-vivo-moderate-cpu-floor"
agentchat_py='D:/LProj/PetProj/agentchat/agentchat.py'
logcat_pid=''

guard_device_action() {
    local action="${1:-adb}"
    python3 "$agentchat_py" event --agent codex --room "увеличить fps" \
        --action "$action" --target "device:vivo" \
        --summary "Bloodborne Vivo policy A/B/A: $action" >/dev/null || return 1
    python3 "$agentchat_py" own --agent codex --room "увеличить fps" \
        --area "device:vivo" >/dev/null
}

adb_device() {
    guard_device_action "adb-$1" || return 1
    adb -s "$device" "$@"
}

remove_policy_markers_best_effort() {
    adb_device shell "run-as app.lsx4.android rm -f '$policy_marker' '$legacy_hint_marker' '$legacy_disable_marker'" \
        >/dev/null 2>&1 || true
}

ensure_policy_markers_absent() {
    if ! adb_device shell "run-as app.lsx4.android rm -f '$policy_marker' '$legacy_hint_marker' '$legacy_disable_marker'" \
        >/dev/null 2>&1; then
        echo 'POLICY_MARKER_REMOVE_FAILED'
        return 1
    fi
    if ! adb_device shell \
        "run-as app.lsx4.android sh -c 'test ! -e \"$policy_marker\" && test ! -e \"$legacy_hint_marker\" && test ! -e \"$legacy_disable_marker\"'" \
        >/dev/null 2>&1; then
        echo 'POLICY_MARKER_STILL_PRESENT'
        return 1
    fi
}

ensure_unsafe_markers_absent() {
    local marker
    for marker in \
        run-enable-fex-buffer-capture-ack \
        run-enable-fex-deferred-write-epoch \
        run-enable-bounded-guest-invalidation-drain \
        run-force-fex-software-tso \
        run-enable-fex-multiblock \
        run-enable-fex-relaxed-vector-tso-ab \
        run-enable-fex-relaxed-memcpyset-tso-ab \
        run-profile-guest-invalidation-sources \
        run-profile-guest-invalidation-overlap \
        run-disable-adpf-40hz-target; do
        if ! adb_device shell \
            "run-as app.lsx4.android sh -c 'test ! -e \"$home/$marker\"'" \
            >/dev/null 2>&1; then
            echo "UNSAFE_OR_CONFOUNDING_MARKER_PRESENT marker=$marker"
            return 1
        fi
    done
}

ensure_root_boost_disabled() {
    local preferences
    preferences=$(adb_device exec-out run-as app.lsx4.android \
        cat shared_prefs/lsx4_settings.xml 2>/dev/null || true)
    if grep -Eq 'name="managed_oem_root_performance"[[:space:]]+value="true"' \
            <<< "$preferences"; then
        echo 'CONFOUNDING_ROOT_BOOST_ENABLED preference=managed_oem_root_performance'
        return 1
    fi
}

assert_no_policy_failures() {
    local context="${1:-unknown}"
    local log_file=".codex-bb-$tag-logcat.txt"
    if grep -Eq 'result=(acquire-failed|renewal-failed|release-failed|fallback-acquired|release-retry-scheduled|release-retry-exhausted|release-pending-expired|acquire-blocked)' \
            "$log_file"; then
        echo "POLICY_FAILURE_OBSERVED context=$context"
        grep -E 'LSX4-PlatformPerf.*result=(acquire-failed|renewal-failed|release-failed|fallback-acquired|release-retry-scheduled|release-retry-exhausted|release-pending-expired|acquire-blocked)' \
            "$log_file" | tail -20
        return 1
    fi
}

assert_policy_marker_state() {
    local expected_mode="$1"
    local expression
    if [[ "$expected_mode" == moderate-cpu-floor ]]; then
        expression="test -f \"$policy_marker\" && test ! -e \"$legacy_hint_marker\" && test ! -e \"$legacy_disable_marker\""
    else
        expression="test ! -e \"$policy_marker\" && test ! -e \"$legacy_hint_marker\" && test ! -e \"$legacy_disable_marker\""
    fi
    if ! adb_device shell \
        "run-as app.lsx4.android sh -c '$expression'" >/dev/null 2>&1; then
        echo "POLICY_MARKER_STATE_INVALID expectedMode=$expected_mode"
        return 1
    fi
}

stop_game() {
    adb_device shell am force-stop app.lsx4.android >/dev/null 2>&1 || true
}

cleanup() {
    if [[ -n "$logcat_pid" ]]; then
        kill "$logcat_pid" >/dev/null 2>&1 || true
        wait "$logcat_pid" >/dev/null 2>&1 || true
        logcat_pid=''
    fi
    remove_policy_markers_best_effort
    stop_game
}

trap cleanup EXIT

annotation() {
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
    echo "gpu_busy=$(cat /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage 2>/dev/null)"
    echo "low_power=$(settings get global low_power 2>/dev/null)"
    echo "power_save_type_system=$(settings get system power_save_type 2>/dev/null)"
    echo "power_save_type_global=$(settings get global power_save_type 2>/dev/null)"
    cmd game mode app.lsx4.android 2>/dev/null | sed "s/^/game_mode=/"'
}

sample_frequencies() {
    local phase="$1"
    adb_device shell "end=\$((\$(date +%s) + $window))
    while [ \"\$(date +%s)\" -lt \"\$end\" ]; do
        printf '%s ' \"\$(date +%s%3N)\"
        cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq \
            /sys/devices/system/cpu/cpu6/cpufreq/scaling_cur_freq \
            /sys/devices/system/cpu/cpu7/cpufreq/scaling_cur_freq \
            /sys/class/kgsl/kgsl-3d0/devfreq/cur_freq \
            /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage 2>/dev/null | tr '\n' ' '
        for z in /sys/class/thermal/thermal_zone*; do
            if [ \"\$(cat \"\$z/type\" 2>/dev/null)\" = tz_shell ]; then
                cat \"\$z/temp\"
                break
            fi
        done
        sleep 1
    done" > ".codex-bb-$tag-$phase-freq.txt" 2>/dev/null
}

foreground_state() {
    adb_device shell 'pidof app.lsx4.android:game
    dumpsys window | grep -m1 -E "mCurrentFocus=.*GameActivity|mFocusedApp=.*GameActivity"
    dumpsys activity activities | grep -m1 -E "topResumedActivity=.*GameActivity|mResumedActivity=.*GameActivity"' \
        2>/dev/null | tr -d '\r'
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

write_builder_scene_metrics() {
    local phase="$1"
    local log_file=".codex-bb-$tag-$phase-logcat.txt"
    local metrics_file=".codex-bb-$tag-$phase-scene.txt"
    if ! awk '
        /\[EXECUTOR_LIVE_FPS\]/ {
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
    ' "$log_file" > "$metrics_file"; then
        echo "PHASE_INVALID_NO_GAMEPLAY_WORK name=$phase"
        return 1
    fi
    local builder_units_per_frame
    builder_units_per_frame=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^builderUnitsPerFrame=/) {split($i,a,"="); print a[2]}}' \
        "$metrics_file")
    if ! awk -v value="$builder_units_per_frame" \
            -v minimum="${BB_MIN_BUILDER_UNITS_PER_FRAME:-400}" \
            'BEGIN { exit !(value + 0 >= minimum + 0) }'; then
        echo "PHASE_INVALID_LIGHT_OR_MENU_WORKLOAD name=$phase builderUnitsPerFrame=$builder_units_per_frame"
        return 1
    fi
}

measure_phase() {
    local phase="$1"
    local expected_mode="$2"
    local phase_wall_start_ms
    local phase_log_start_line
    phase_log_start_line=$(wc -l < ".codex-bb-$tag-logcat.txt")
    assert_policy_marker_state "$expected_mode" || exit 33
    annotation > ".codex-bb-$tag-$phase-start.txt" 2>/dev/null
    adb_device exec-out run-as app.lsx4.android cat files/jit-status.json \
        > ".codex-bb-$tag-$phase-jit-status-start.json" 2>/dev/null || true
    if ! adb_device shell 'dumpsys SurfaceFlinger --timestats -clear' \
        >/dev/null 2>&1; then
        echo "PHASE_INVALID_TIMESTATS_CLEAR name=$phase"
        exit 19
    fi
    phase_wall_start_ms=$(date +%s%3N)
    sample_frequencies "$phase" &
    local freq_pid=$!
    if ! adb_device shell "run-as app.lsx4.android /system/bin/simpleperf stat -p $game_pid \
        --duration $window -e instructions:u,cpu-cycles:u,task-clock,context-switches,cpu-migrations,page-faults" \
        > ".codex-bb-$tag-$phase-pmu.out" 2> ".codex-bb-$tag-$phase-pmu.err"; then
        echo "PHASE_INVALID_PMU_COMMAND name=$phase"
        exit 34
    fi
    wait "$freq_pid" || true
    if ! grep -q 'Total test time:' ".codex-bb-$tag-$phase-pmu.out"; then
        echo "PHASE_INVALID_PMU name=$phase"
        cat ".codex-bb-$tag-$phase-pmu.out" ".codex-bb-$tag-$phase-pmu.err"
        exit 9
    fi
    if grep -Eqi '<not counted>|not supported|permission denied' \
            ".codex-bb-$tag-$phase-pmu.out" ".codex-bb-$tag-$phase-pmu.err"; then
        echo "PHASE_INVALID_PMU_COVERAGE name=$phase"
        exit 35
    fi
    if ! adb_device shell 'dumpsys SurfaceFlinger --timestats -dump --maxlayers 8' \
        > ".codex-bb-$tag-$phase-sf.txt" 2>/dev/null; then
        echo "PHASE_INVALID_TIMESTATS_DUMP name=$phase"
        exit 20
    fi
    local game_surface_frames
    game_surface_frames=$(game_surface_frames_from_file ".codex-bb-$tag-$phase-sf.txt")
    local phase_wall_end_ms
    phase_wall_end_ms=$(date +%s%3N)
    local sf_start sf_end sf_seconds
    sf_start=$(awk '/^statsStart = / {print $3; exit}' ".codex-bb-$tag-$phase-sf.txt")
    sf_end=$(awk '/^statsEnd = / {print $3; exit}' ".codex-bb-$tag-$phase-sf.txt")
    sf_seconds=$((sf_end - sf_start))
    if (( sf_seconds < window - 3 || sf_seconds > window + 8 )); then
        echo "PHASE_INVALID_SF_WINDOW name=$phase seconds=$sf_seconds expected=$window"
        exit 36
    fi
    echo "wallStartMs=$phase_wall_start_ms wallEndMs=$phase_wall_end_ms wallMs=$((phase_wall_end_ms - phase_wall_start_ms)) sfStart=$sf_start sfEnd=$sf_end sfSeconds=$sf_seconds surfaceFrames=$game_surface_frames" \
        > ".codex-bb-$tag-$phase-window.txt"
    local minimum_game_surface_frames=$((window * ${BB_MIN_PHASE_FPS:-5}))
    if (( game_surface_frames < minimum_game_surface_frames )); then
        echo "PHASE_INVALID_SURFACE_FRAMES name=$phase frames=$game_surface_frames minimum=$minimum_game_surface_frames"
        exit 12
    fi
    local tail_seconds="${BB_LIVENESS_TAIL_SECONDS:-3}"
    sleep "$tail_seconds"
    if ! adb_device shell 'dumpsys SurfaceFlinger --timestats -dump --maxlayers 8' \
        > ".codex-bb-$tag-$phase-sf-tail.txt" 2>/dev/null; then
        echo "PHASE_INVALID_TIMESTATS_TAIL name=$phase"
        exit 21
    fi
    local tail_game_surface_frames
    tail_game_surface_frames=$(game_surface_frames_from_file ".codex-bb-$tag-$phase-sf-tail.txt")
    if (( tail_game_surface_frames <= game_surface_frames )); then
        echo "PHASE_INVALID_FROZEN_TAIL name=$phase before=$game_surface_frames after=$tail_game_surface_frames tailSeconds=$tail_seconds"
        exit 22
    fi
    annotation > ".codex-bb-$tag-$phase-end.txt" 2>/dev/null
    adb_device exec-out run-as app.lsx4.android cat files/jit-status.json \
        > ".codex-bb-$tag-$phase-jit-status-end.json" 2>/dev/null || true
    tail -n "+$((phase_log_start_line + 1))" ".codex-bb-$tag-logcat.txt" \
        > ".codex-bb-$tag-$phase-logcat.txt"
    assert_policy_marker_state "$expected_mode" || exit 37
    if grep -q 'vivo transition=switch' ".codex-bb-$tag-$phase-logcat.txt"; then
        echo "PHASE_INVALID_POLICY_SWITCH name=$phase expectedMode=$expected_mode"
        exit 38
    fi
    if ! grep -q "vivo mode=$expected_mode transport=binder result=renewed" \
            ".codex-bb-$tag-$phase-logcat.txt"; then
        echo "PHASE_INVALID_POLICY_NOT_RENEWED name=$phase expectedMode=$expected_mode"
        exit 39
    fi
    local unexpected_mode=performance-hint-4241
    if [[ "$expected_mode" == performance-hint-4241 ]]; then
        unexpected_mode=moderate-cpu-floor
    fi
    if grep -Eq "vivo mode=$unexpected_mode .*result=(acquired|renewed)" \
            ".codex-bb-$tag-$phase-logcat.txt"; then
        echo "PHASE_INVALID_UNEXPECTED_POLICY name=$phase expectedMode=$expected_mode"
        exit 40
    fi
    write_builder_scene_metrics "$phase" || exit 41
    foreground_state > ".codex-bb-$tag-$phase-foreground.txt"
    current_pid=$(head -n1 ".codex-bb-$tag-$phase-foreground.txt" | tr -d '[:space:]')
    if [[ "$current_pid" != "$game_pid" ]]; then
        echo "PHASE_INVALID_PID name=$phase expected=$game_pid actual=$current_pid"
        exit 10
    fi
    if ! grep -Eq 'app\.lsx4\.android(/app\.lsx4\.android|/)\.GameActivity' \
        ".codex-bb-$tag-$phase-foreground.txt"; then
        echo "PHASE_INVALID_NOT_FOREGROUND name=$phase"
        cat ".codex-bb-$tag-$phase-foreground.txt"
        exit 8
    fi
    if ! adb_device exec-out screencap -p > ".codex-bb-$tag-$phase-scene.png"; then
        echo "PHASE_INVALID_SCREENSHOT name=$phase"
        exit 45
    fi
    assert_no_policy_failures "phase-$phase-final" || exit 46
    echo "PHASE_DONE name=$phase alive=$game_pid surface_frames=$game_surface_frames tail_frames=$tail_game_surface_frames"
}

launch_through_handoff() {
    adb_device shell "am start -n app.lsx4.android/.MainActivity \
        --ez autoload_existing_runtime true --ez autoinit_runtime true \
        --es scan_game_path $game --es launch_game_path $game \
        --es game_platform ps4 --ez fullscreen_render true \
        --ez embedded_aarch64_jit_backend true --ei render_resolution_mode 4" >/dev/null
}

if ! ensure_policy_markers_absent || ! ensure_unsafe_markers_absent \
        || ! ensure_root_boost_disabled; then
    exit 5
fi
stop_game
adb_device shell input keyevent KEYCODE_WAKEUP >/dev/null 2>&1 || true
adb_device shell input keyevent 82 >/dev/null 2>&1 || true
if ! adb_device logcat -c; then
    echo 'LOGCAT_CLEAR_FAILED'
    exit 23
fi
adb_device logcat -v threadtime > ".codex-bb-$tag-logcat.txt" 2>/dev/null &
logcat_pid=$!

start_time=$(date +%s)
game_pid=''
for launch_attempt in 1 2 3; do
    launch_through_handoff
    sleep 5
    game_pid=$(adb_device shell 'pidof app.lsx4.android:game' | tr -d '\r')
    if [[ -n "$game_pid" ]]; then
        echo "LAUNCHED attempt=$launch_attempt pid=$game_pid t=$(( $(date +%s) - start_time ))"
        break
    fi
    echo "HANDOFF_RETRY attempt=$launch_attempt t=$(( $(date +%s) - start_time ))"
done
if [[ -z "$game_pid" ]]; then
    echo 'NO_PROCESS_AFTER_HANDOFF_RETRIES'
    exit 4
fi

sleep "${BB_WARNING_WAIT:-64}"
adb_device shell 'input swipe 2903 1231 2903 1231 1000' >/dev/null
sleep "${BB_OFFLINE_WAIT:-30}"
adb_device shell 'input swipe 2903 1231 2903 1231 1000' >/dev/null
sleep "${BB_CONTINUE_WAIT:-22}"
adb_device shell 'input swipe 2903 1231 2903 1231 1000' >/dev/null

menu_pid=$(adb_device shell 'pidof app.lsx4.android:game' | tr -d '\r')
if [[ -z "$menu_pid" || "$menu_pid" != "$game_pid" ]]; then
    echo 'NO_PROCESS'
    exit 4
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

sleep "${BB_DOOR_WAIT:-65}"
adb_device shell 'input swipe 2903 1231 2903 1231 1000' >/dev/null
sleep "${BB_DOOR_SETTLE_WAIT:-5}"

# Position once, then leave both A/B phases completely stationary.  This
# avoids the scene mismatch caused by low-FPS game-time movement.
position_window="${BB_POSITION_WINDOW:-30}"
adb_device shell "input swipe 950 1245 950 1060 $((position_window * 1000))" >/dev/null 2>&1
sleep "${BB_POSITION_SETTLE_WAIT:-5}"

if ! grep -q 'gameState loading=false gameplay=true' ".codex-bb-$tag-logcat.txt" \
        || ! grep -q 'serial=CUSA03173' ".codex-bb-$tag-logcat.txt" \
        || ! grep -q 'target=960x544' ".codex-bb-$tag-logcat.txt"; then
    echo 'GAMEPLAY_IDENTITY_NOT_OBSERVED title=CUSA03173 target=960x544'
    exit 42
fi
if grep -Eq 'rootBoost .*applied=true' ".codex-bb-$tag-logcat.txt"; then
    echo 'CONFOUNDING_ROOT_BOOST_APPLIED'
    exit 43
fi

if ! adb_device shell 'dumpsys SurfaceFlinger --timestats -enable' \
    >/dev/null 2>&1; then
    echo 'TIMESTATS_ENABLE_FAILED'
    exit 24
fi
if ! assert_no_policy_failures hint-a1-preflight; then
    echo 'HINT_A1_INVALID_EARLY_STATE'
    exit 13
fi
if ! grep -Eq 'vivo mode=performance-hint-4241 transport=binder result=(acquired|renewed)' \
    ".codex-bb-$tag-logcat.txt"; then
    echo 'HINT_A1_ACQUIRE_NOT_OBSERVED'
    exit 14
fi
measure_phase hint-a1 performance-hint-4241
if grep -Eq 'vivo mode=(moderate-cpu-floor|performance-hint-4241).*result=(acquire-failed|renewal-failed|release-failed|fallback-acquired)' \
    ".codex-bb-$tag-hint-a1-logcat.txt"; then
    echo 'HINT_A1_INVALID_DURING_PHASE'
    exit 16
fi
assert_no_policy_failures before-moderate-marker || exit 47

to_moderate_log_line=$(wc -l < ".codex-bb-$tag-logcat.txt")
if ! adb_device shell "run-as app.lsx4.android sh -c 'touch $policy_marker'" \
    >/dev/null 2>&1; then
    echo 'MODERATE_MARKER_CREATE_FAILED'
    exit 17
fi
if ! adb_device shell \
    "run-as app.lsx4.android sh -c 'test -f \"$policy_marker\"'" \
    >/dev/null 2>&1; then
    echo 'MODERATE_MARKER_NOT_PRESENT'
    exit 18
fi
echo "MODERATE_MARKER_CREATED t=$(( $(date +%s) - start_time ))"
sleep "${BB_POLICY_SWITCH_WAIT:-45}"
tail -n "+$((to_moderate_log_line + 1))" ".codex-bb-$tag-logcat.txt" \
    > ".codex-bb-$tag-to-moderate-transition-log.txt"
assert_no_policy_failures after-moderate-transition-settle || exit 48

transition_order() {
    local file="$1"
    local from="$2"
    local to="$3"
    local prefix="vivo transition=switch from=$from to=$to order="
    local detach_line release_line released_line acquire_line commit_line acquired_line
    local detach release released acquire commit acquired old_handle new_handle
    detach_line=$(grep -n -m1 "${prefix}detach-old" "$file")
    old_handle=$(sed -n 's/.*oldHandle=\([0-9][0-9]*\).*/\1/p' <<< "$detach_line")
    release_line=$(grep -n -m1 "${prefix}release-old oldHandle=$old_handle" "$file")
    released_line=$(grep -n -m1 \
        "vivo mode=$from transport=binder result=released handle=$old_handle " "$file")
    acquire_line=$(grep -n -m1 \
        "${prefix}acquire-desired oldHandle=$old_handle" "$file")
    commit_line=$(grep -n -m1 \
        "${prefix}commit-desired oldHandle=$old_handle" "$file")
    new_handle=$(sed -n 's/.*newHandle=\([0-9][0-9]*\).*/\1/p' <<< "$commit_line")
    acquired_line=$(grep -n -m1 \
        "vivo mode=$to transport=binder result=acquired handle=$new_handle" "$file")
    detach=${detach_line%%:*}
    release=${release_line%%:*}
    released=${released_line%%:*}
    acquire=${acquire_line%%:*}
    commit=${commit_line%%:*}
    acquired=${acquired_line%%:*}
    if [[ -z "$detach" || -z "$release" || -z "$released" || -z "$acquire" \
            || -z "$commit" || -z "$acquired" || -z "$old_handle" \
            || -z "$new_handle" ]] || \
            ! (( detach < release && release < released && released < acquire \
                && acquire < commit && commit < acquired )); then
        echo "POLICY_TRANSITION_ORDER_INVALID from=$from to=$to oldHandle=$old_handle newHandle=$new_handle detach=$detach release=$release released=$released acquire=$acquire commit=$commit acquired=$acquired"
        return 1
    fi
}

if ! transition_order ".codex-bb-$tag-to-moderate-transition-log.txt" \
        performance-hint-4241 moderate-cpu-floor; then
    exit 11
fi
if grep -Eq 'result=(acquire-failed|renewal-failed|release-failed|fallback-acquired)' \
        ".codex-bb-$tag-to-moderate-transition-log.txt"; then
    echo 'MODERATE_TRANSITION_FAILED_OR_FELL_BACK'
    exit 15
fi
measure_phase moderate-b moderate-cpu-floor
if grep -Eq 'vivo mode=(moderate-cpu-floor|performance-hint-4241).*result=(acquire-failed|renewal-failed|release-failed|fallback-acquired)' \
    ".codex-bb-$tag-moderate-b-logcat.txt"; then
    echo 'MODERATE_B_INVALID_DURING_PHASE'
    exit 25
fi
assert_no_policy_failures before-hint-default || exit 49

to_hint_log_line=$(wc -l < ".codex-bb-$tag-logcat.txt")
if ! adb_device shell "run-as app.lsx4.android rm -f '$policy_marker'" \
    >/dev/null 2>&1; then
    echo 'MODERATE_MARKER_REMOVE_FAILED'
    exit 26
fi
if ! adb_device shell \
    "run-as app.lsx4.android sh -c 'test ! -e \"$policy_marker\"'" \
    >/dev/null 2>&1; then
    echo 'MODERATE_MARKER_STILL_PRESENT'
    exit 27
fi
echo "MODERATE_MARKER_REMOVED t=$(( $(date +%s) - start_time ))"
sleep "${BB_POLICY_SWITCH_WAIT:-45}"
tail -n "+$((to_hint_log_line + 1))" ".codex-bb-$tag-logcat.txt" \
    > ".codex-bb-$tag-to-hint-transition-log.txt"
assert_no_policy_failures after-hint-transition-settle || exit 50
if ! transition_order ".codex-bb-$tag-to-hint-transition-log.txt" \
        moderate-cpu-floor performance-hint-4241; then
    exit 28
fi
if grep -Eq 'result=(acquire-failed|renewal-failed|release-failed|fallback-acquired)' \
        ".codex-bb-$tag-to-hint-transition-log.txt"; then
    echo 'HINT_A2_TRANSITION_FAILED_OR_FELL_BACK'
    exit 29
fi
measure_phase hint-a2 performance-hint-4241
if grep -Eq 'vivo mode=(moderate-cpu-floor|performance-hint-4241).*result=(acquire-failed|renewal-failed|release-failed|fallback-acquired)' \
    ".codex-bb-$tag-hint-a2-logcat.txt"; then
    echo 'HINT_A2_INVALID_DURING_PHASE'
    exit 30
fi

scene_metric() {
    local phase="$1"
    local key="$2"
    awk -v key="$key" '{
        for (i = 1; i <= NF; ++i) {
            split($i, value, "=")
            if (value[1] == key) print value[2]
        }
    }' ".codex-bb-$tag-$phase-scene.txt"
}

compare_aba_scene_metric() {
    local key="$1"
    local tolerance="$2"
    local a1 b a2
    a1=$(scene_metric hint-a1 "$key")
    b=$(scene_metric moderate-b "$key")
    a2=$(scene_metric hint-a2 "$key")
    if ! awk -v a="$a1" -v b="$b" -v c="$a2" -v tolerance="$tolerance" '
        BEGIN {
            mean_a = (a + c) / 2
            drift_a = a > c ? a - c : c - a
            drift_b = b > mean_a ? b - mean_a : mean_a - b
            if (mean_a == 0) exit !(b == 0)
            exit !((100 * drift_a / mean_a) <= tolerance &&
                   (100 * drift_b / mean_a) <= tolerance)
        }'; then
        echo "ABA_SCENE_MISMATCH metric=$key hintA1=$a1 moderateB=$b hintA2=$a2 tolerancePct=$tolerance"
        return 1
    fi
}

compare_aba_scene_metric builderUnitsPerFrame \
    "${BB_SCENE_WORK_TOLERANCE_PCT:-15}" || exit 44
compare_aba_scene_metric builderDrawPerFrame \
    "${BB_SCENE_DRAW_TOLERANCE_PCT:-15}" || exit 51
compare_aba_scene_metric builderShaderPerFrame \
    "${BB_SCENE_SHADER_TOLERANCE_PCT:-25}" || exit 52
assert_no_policy_failures aba-complete || exit 53

if [[ -n "$logcat_pid" ]]; then
    kill "$logcat_pid" >/dev/null 2>&1 || true
    wait "$logcat_pid" >/dev/null 2>&1 || true
    logcat_pid=''
fi

if ! ensure_policy_markers_absent || ! ensure_unsafe_markers_absent; then
    exit 31
fi
final_pid=$(adb_device shell 'pidof app.lsx4.android:game' | tr -d '\r')
if [[ "$final_pid" != "$game_pid" ]]; then
    echo "FINAL_GAME_NOT_RUNNING expected=$game_pid actual=$final_pid"
    exit 32
fi
trap - EXIT

echo "DONE pid=$game_pid wall=$(( $(date +%s) - start_time )) finalPolicy=performance-hint-4241 gameRunning=true"
grep -E 'LSX4-PlatformPerf|FATAL EXCEPTION|Fatal signal|SIGBUS|SIGSEGV|am_crash|am_anr|ANR in' \
    ".codex-bb-$tag-logcat.txt" | tail -80 || true
cat ".codex-bb-$tag-hint-a1-pmu.out" ".codex-bb-$tag-hint-a1-pmu.err"
cat ".codex-bb-$tag-moderate-b-pmu.out" ".codex-bb-$tag-moderate-b-pmu.err"
cat ".codex-bb-$tag-hint-a2-pmu.out" ".codex-bb-$tag-hint-a2-pmu.err"
