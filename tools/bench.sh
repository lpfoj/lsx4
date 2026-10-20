#!/bin/bash
# Objective Bloodborne benchmark: thermal-gated, SurfaceFlinger timestats.
# usage: bench.sh <label> [window_s] [cool_millideg] [res_mode] [scene_scale]
#
# Every run prints one manifest line. A run is only comparable to another run
# whose manifest matches on SHA, and whose VERDICT is OK. Four things were found
# in one day that silently made runs incomparable - a crash at 218 s, a frequency
# collapse, a shader-compile window, and a binary from another worktree - so each
# of them is now checked rather than assumed.
export MSYS2_ARG_CONV_EXCL='*'
V="${ADB_SERIAL:-adb-10AG263F1B001HM-2MNzzP._adb-tls-connect._tcp}"
LABEL="${1:-run}"
WINDOW="${2:-60}"
COOL="${3:-49000}"
RES="${4:-4}"
SCENE="${5:-}"
G='/data/user/0/app.lsx4.android/files/lsx4-home/runtime-fs/user/app/CUSA03173/eboot.bin'
LOCAL_SO='D:/LProj/PetProj/lsx4-jit-independent-wt/build/android-arm64-release/liblsx4_executor_android.so'
DEV_SO='/data/user/0/app.lsx4.android/files/runtime/liblsx4_executor_android.so'
# Below this the governor has given up on us rather than merely throttled.
COLLAPSE_KHZ=1900000

a() { adb -s "$V" "$@"; }
cleanup() { a shell am force-stop app.lsx4.android >/dev/null 2>&1 || true; }
trap cleanup EXIT

# Refuse to run unless this agent actually holds the device claim. Losing a run to
# a collision costs a charge cycle; the check costs nothing.
if [ -n "$AGENTCHAT_AGENT" ]; then
  if ! python3 D:/LProj/PetProj/agentchat/agentchat.py own \
       --agent "$AGENTCHAT_AGENT" --area "adb:$V" >/dev/null 2>&1; then
    echo "LABEL=$LABEL RESULT=DEVICE_NOT_OWNED_BY_$AGENTCHAT_AGENT"
    exit 3
  fi
fi

# Taking the maximum over every thermal zone reads whichever sensor happens to be
# hottest, and on this device that is pmih010x_lite_tz - a PMIC zone parked at
# ~65 C whether the phone is idle or loaded. A gate waiting on it never opens: the
# skin was 26.7 C at the time. Ask for the skin zone by name instead, since that
# is also what the throttling policy tracks.
# Iterating the zones and taking the first acceptable type follows sysfs order,
# not preference: sys_therm_8 comes first and shadows tz_shell, reporting 36.3 C
# where the skin is 26.7 C. Ask for tz_shell by name, and only fall back if the
# device does not have it.
zonetemp() {
  a shell "for z in /sys/class/thermal/thermal_zone*; do
             [ \"\$(cat \$z/type 2>/dev/null)\" = '$1' ] && cat \$z/temp 2>/dev/null && break
           done" 2>/dev/null | tr -d '\r' | awk '$1>10000 && $1<100000' | head -1
}
cputemp() {
  local t
  t=$(zonetemp tz_shell)
  [ -z "$t" ] && t=$(zonetemp sys_therm_8)
  echo "$t"
}
maxfreq() { a shell "cat /sys/devices/system/cpu/cpu6/cpufreq/scaling_max_freq" 2>/dev/null | tr -d '\r'; }
curfreq() {
  a shell "cat /sys/devices/system/cpu/cpu6/cpufreq/scaling_cur_freq /sys/devices/system/cpu/cpu7/cpufreq/scaling_cur_freq" \
    2>/dev/null | tr -d '\r' | sort -n | head -1
}
# The app's files are private, so the hash has to go through run-as. If that
# fails we say so instead of quietly comparing nothing.
devsha() {
  a shell "run-as app.lsx4.android sha256sum $DEV_SO" 2>/dev/null \
    | tr -d '\r' | awk '{print substr($1,1,12)}'
}
shadercount() {
  a shell "run-as app.lsx4.android sh -c 'ls -1 /data/user/0/app.lsx4.android/cache/shader 2>/dev/null | wc -l'" \
    2>/dev/null | tr -d '\r' | tr -dc '0-9'
}

# Three different builds were in flight at once today: one on the device, one in
# the build directory, and one that had actually been installed. Hashing the build
# directory therefore proves nothing on its own - it is a reference only if it is
# the artefact that was deployed. So the expected hash can be stated explicitly:
#   EXPECT_SHA=28b0bbd7c19e bench.sh label
# With it set, the device is checked against what the operator meant to measure.
# Without it the build directory is a guess, and a difference is reported as
# UNVERIFIED_BUILD rather than as a failed run.
LSHA="${EXPECT_SHA:-}"
SHA_SOURCE=stated
if [ -z "$LSHA" ]; then
  SHA_SOURCE=builddir
  LSHA=$(sha256sum "$LOCAL_SO" 2>/dev/null | awk '{print substr($1,1,12)}')
fi
[ -z "$LSHA" ] && LSHA=nolocal

a shell am force-stop app.lsx4.android >/dev/null 2>&1
# `svc power stayon true` holds the screen on only while the device is charging.
# Unplugged - which is how we run, to keep the clocks off the charging cap - it
# does nothing at all, the panel sleeps, no surface is composited, and the run
# reports no frames while the guest is in fact executing hundreds of thousands of
# blocks. Keep the screen awake explicitly instead.
wake() {
  a shell input keyevent KEYCODE_WAKEUP >/dev/null 2>&1
  a shell input keyevent 82 >/dev/null 2>&1
}
asleep() {
  a shell dumpsys power 2>/dev/null | tr -d '\r' | grep -q "mWakefulness=Asleep"
}
# thermal gate: comparable starting point for every run
# A run that starts warm collapses halfway through and is thrown away, so the
# gate has to be able to outwait a genuinely hot device: 71 C was observed right
# after a previous run, and six minutes is nowhere near enough to shed that.
# Observed cooling: 71 C -> 60 C took twenty minutes of idle. Waiting out a hot
# device is therefore an hour-scale operation, so the cap is settable.
GATE_MAX="${GATE_MAX:-1200}"
GATE_START=$(date +%s)
while :; do
  T=$(cputemp); [ -z "$T" ] && break
  [ "$T" -le "$COOL" ] && break
  [ $(( $(date +%s) - GATE_START )) -ge "$GATE_MAX" ] && break
  sleep 20
done
T0=$(cputemp); F0=$(maxfreq)

wake
a shell "am start -n app.lsx4.android/.MainActivity --ez autoload_existing_runtime true --ez autoinit_runtime true --es scan_game_path $G --es launch_game_path $G --es game_platform ps4 --ez fullscreen_render true --ez embedded_aarch64_jit_backend true --ei render_resolution_mode $RES ${SCENE:+--ei scene_render_scale $SCENE}" >/dev/null 2>&1

# Bloodborne samples the pad only every 80-125 ms at low FPS. A normal keyevent
# pulse lasts about 10 ms and is commonly missed. No input is sent during the
# first 50 seconds: an early press can spawn a second GameActivity.  The current
# save resumes after dismissing the Exit Game warning; further Cross presses hit
# the unskippable logo/load sequence and make the route timing nondeterministic.
sleep 50
asleep && wake
# A 300 ms hold was still occasionally missed while a cold JIT bootstrap kept
# Game:Main below 4 Hz.  One second remains a single edge-triggered pad press,
# but guarantees that at least one guest input sample observes it.
hold_cross() { a shell "input swipe 2903 1231 2903 1231 1000" >/dev/null 2>&1; }
hold_cross

P=$(a shell "pidof app.lsx4.android:game" | tr -d '\r')
if [ -z "$P" ]; then echo "LABEL=$LABEL RESULT=NO_PROCESS"; exit 1; fi

# A menu can report 30 FPS, so SurfaceFlinger alone is not a gameplay gate.
# Require both a Havok worker and GpuComm to spend meaningful CPU time for two
# consecutive 3-second probes. The save then stands at the Bath Messenger; never
# send another Cross here because it opens the shop and invalidates the sample.
find_role_tid() {
  local pattern="$1"
  a shell "for t in /proc/$P/task/*; do n=\$(cat \$t/comm 2>/dev/null); echo \"\$n\" | grep -qi '$pattern' && { basename \$t; break; }; done" 2>/dev/null | tr -d '\r'
}
run_ns() {
  local tid="$1"
  [ -n "$tid" ] && a shell "cat /proc/$P/task/$tid/schedstat" 2>/dev/null | tr -d '\r' | awk '{print $1}'
}
gate_probe() {
  local ht gt h0 g0 h1 g1
  ht=$(find_role_tid HavokWorker)
  gt=$(find_role_tid GpuComm)
  h0=$(run_ns "$ht"); g0=$(run_ns "$gt")
  sleep 3
  h1=$(run_ns "$ht"); g1=$(run_ns "$gt")
  if [ -z "$h0" ] || [ -z "$h1" ]; then h1=0; h0=0; fi
  if [ -z "$g0" ] || [ -z "$g1" ]; then g1=0; g0=0; fi
  echo "$(( (h1-h0) / 30000000 )) $(( (g1-g0) / 30000000 ))"
}
GATE_STREAK=0
for i in $(seq 1 30); do
  asleep && wake
  read -r HAVOK_PCT GPUCOMM_PCT <<< "$(gate_probe)"
  if [ "$HAVOK_PCT" -ge 20 ] && [ "$GPUCOMM_PCT" -ge 8 ]; then
    GATE_STREAK=$(( GATE_STREAK + 1 ))
    [ "$GATE_STREAK" -ge 2 ] && break
  else
    GATE_STREAK=0
  fi
done
if [ "$GATE_STREAK" -lt 2 ]; then
  echo "LABEL=$LABEL RESULT=GAMEPLAY_GATE_FAILED HAVOK=$HAVOK_PCT GPUCOMM=$GPUCOMM_PCT"
  exit 2
fi

# Presence of the layer is not proof that it is producing. Require the frame
# counter to actually move before opening the measurement window, and keep
# nudging the route while it does not.
FRAMES_MOVING=0
for i in $(seq 1 12); do
  a shell "dumpsys SurfaceFlinger --timestats -enable" >/dev/null 2>&1
  a shell "dumpsys SurfaceFlinger --timestats -clear" >/dev/null 2>&1
  sleep 5
  N=$(a shell "dumpsys SurfaceFlinger --timestats -dump --maxlayers 4" 2>/dev/null       | tr -d '
' | awk '/GameActivity\]\(BLAST\)/{f=1} f && /totalFrames/{print $3; exit}')
  # The dump has header lines that also contain "totalFrames", so field 3 is
  # not always a number - compare only when it is one.
  case "$N" in ""|*[!0-9]*) N=0;; esac
  if [ "$N" -gt 0 ]; then FRAMES_MOVING=1; break; fi
  asleep && wake
done

DSHA=$(devsha); [ -z "$DSHA" ] && DSHA=unreadable
SH0=$(shadercount)

a shell "dumpsys SurfaceFlinger --timestats -enable" >/dev/null 2>&1
a shell "dumpsys SurfaceFlinger --timestats -clear" >/dev/null 2>&1

# Sample the clock through the window instead of only at its ends: a collapse in
# the middle is invisible to before/after readings and quietly halves the result.
FMIN=99999999
FSUM=0; FN=0; FLOW=0
SAMPLES=$(( WINDOW / 6 )); [ "$SAMPLES" -lt 1 ] && SAMPLES=1
for i in $(seq 1 "$SAMPLES"); do
  C=$(curfreq)
  if [ -n "$C" ]; then
    [ "$C" -lt "$FMIN" ] && FMIN=$C
    FSUM=$(( FSUM + C )); FN=$(( FN + 1 ))
    [ "$C" -lt "$COLLAPSE_KHZ" ] && FLOW=$(( FLOW + 1 ))
  fi
  sleep 6
done
FAVG=0; [ "$FN" -gt 0 ] && FAVG=$(( FSUM / FN ))
FLOWPCT=0; [ "$FN" -gt 0 ] && FLOWPCT=$(( 100 * FLOW / FN ))

GPUB=$(for i in 1 2 3 4; do a shell "cat /sys/class/kgsl/kgsl-3d0/gpubusy" 2>/dev/null | tr -d '\r' | awk '{if ($2>0) printf "%d ", 100*$1/$2}'; sleep 2; done)
OUT=$(a shell "dumpsys SurfaceFlinger --timestats -dump --maxlayers 4" 2>/dev/null | tr -d '\r')
T1=$(cputemp); F1=$(maxfreq)
SH1=$(shadercount)
PEND=$(a shell "pidof app.lsx4.android:game" | tr -d '\r')

FPS=$(echo "$OUT" | awk '/GameActivity\]\(BLAST\)/{found=1} found && /averageFPS/{print $3; exit}')
FRAMES=$(echo "$OUT" | awk '/GameActivity\]\(BLAST\)/{found=1} found && /totalFrames/{print $3; exit}')
DROP=$(echo "$OUT" | awk '/GameActivity\]\(BLAST\)/{found=1} found && /droppedFrames/{print $3; exit}')

# Each verdict below marks a run that must not be compared with a clean one.
VERDICT=OK
[ -z "$PEND" ] && VERDICT=DIED_DURING_WINDOW
# No layer in SurfaceFlinger means the run never rendered, and then a low clock is
# the governor idling rather than the device throttling. Say which it was, because
# the two look identical in the frequency samples and need opposite fixes.
if [ "$VERDICT" = OK ] && [ -z "$FRAMES" ]; then VERDICT=NO_FRAMES_NAV_FAILED; fi
# A single dip is scheduler noise, not a collapse - one core parking for one
# sample would condemn an otherwise clean run. What matters is how much of the
# window was spent down there, so judge on the share of samples.
if [ "$VERDICT" = OK ] && [ "$FN" -gt 0 ] && [ "$FLOWPCT" -ge 30 ]; then
  VERDICT=FREQ_COLLAPSE
fi
if [ "$VERDICT" = OK ] && [ -n "$SH0" ] && [ -n "$SH1" ] && [ "$SH1" -gt "$SH0" ]; then
  VERDICT=SHADERS_STILL_COMPILING
fi
# Starting temperature decides the clock the whole run gets: 47 C gave 1.96 GHz,
# 57 C gave 1.54 GHz. Two runs that merely both passed the gate are still not
# comparable, so an A/B states the reference start and holds the pair to it.
if [ "$VERDICT" = OK ] && [ -n "$EXPECT_T0" ] && [ -n "$T0" ]; then
  DT=$(( T0 - EXPECT_T0 )); [ "$DT" -lt 0 ] && DT=$(( -DT ))
  [ "$DT" -gt 3000 ] && VERDICT=THERMAL_START_MISMATCH
fi
if [ "$VERDICT" = OK ] && [ "$DSHA" != unreadable ] && [ "$LSHA" != nolocal ] \
   && [ "$DSHA" != "$LSHA" ]; then
  # A stated hash that does not match means the wrong build was measured. A build
  # directory that does not match may just be a newer local compile that was never
  # installed, which is not by itself a reason to discard the run.
  if [ "$SHA_SOURCE" = stated ]; then
    VERDICT=BINARY_MISMATCH
  else
    VERDICT=UNVERIFIED_BUILD
  fi
fi

echo "LABEL=$LABEL FPS=$FPS FRAMES=$FRAMES DROPPED=$DROP GPUBUSY=[$GPUB]" \
     "T0=$T0 T1=$T1 F0=$F0 F1=$F1 FMIN=$FMIN FAVG=$FAVG FLOW=$FLOWPCT% SHADERS=$SH0->$SH1" \
     "SHA_EXPECT=$LSHA($SHA_SOURCE) SHA_DEVICE=$DSHA VERDICT=$VERDICT"
[ "$VERDICT" = OK ] || exit 4
