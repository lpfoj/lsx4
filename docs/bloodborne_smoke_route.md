# Bloodborne CUSA03173 — smoke route to valid gameplay (Vivo V2546A)

> [!IMPORTANT]
> Before any device/FPS/playable measurement, run
> `pwsh -NoProfile -File scripts/preflight-vivo-performance-test.ps1`;
> stale Android Studio process-trackers and known profiler loads make the result invalid. See the
> mandatory [performance-test preflight](PERFORMANCE_TEST_PREFLIGHT.md).

Established 2026-08-09 by stepping the route with screenshots. Device
`10AG263F1B001HM`, package `app.lsx4.android`, game process
`app.lsx4.android:game`.

## The one thing that breaks every naive script

`input keyevent 96` and `input tap` **do not register**. At the 8–12 FPS this
title runs, the guest samples the pad roughly once per 80–125 ms, and both of
those emit a press/release pulse of about 10 ms, which falls between polls. The
occasional success looks like flaky navigation and sends people hunting for the
wrong bug.

Every button press must be a **hold**:

```powershell
adb -s $dev shell "input swipe $x $y $x $y 300"
```

## On-screen pad coordinates (3168x1440, landscape)

```text
X / cross    2903 1231
circle       3045 1085
square       2756 1085
triangle     2902  939
```

Exact coordinate-free title launch (do not tap a library tile):

```powershell
$path = '/data/user/0/app.lsx4.android/files/lsx4-home/runtime-fs/user/app/CUSA03173/eboot.bin'
adb -s $dev shell am start -n app.lsx4.android/.MainActivity `
  --ez autoload_existing_runtime true --ez autoinit_runtime true `
  --es scan_game_path $path --es launch_game_path $path `
  --es game_platform ps4 --ez fullscreen_render true `
  --ez embedded_aarch64_jit_backend true
```

## Route

| # | action | wait | resulting screen | draw/s |
|---|--------|------|------------------|--------|
| 1 | `am force-stop app.lsx4.android` | 2 s | — | — |
| 2 | exported `MainActivity` exact launch with full `scan_game_path` + `launch_game_path`, `autoload_existing_runtime`, `autoinit_runtime`, `game_platform=ps4`, `fullscreen_render` and `embedded_aarch64_jit_backend` | 18–22 s, no input | dialog `Exit Game may not have been selected` / `OK` | ~780 |
| 3 | screenshot-confirm dialog, hold X 300 ms | 20–25 s, no input on logos | `Play Offline` selected | ~840 |
| 4 | screenshot-confirm selection, hold X 300 ms | 4 s | `Continue` selected | ~1400 |
| 5 | screenshot-confirm selection, hold X 300 ms | 12 s | Hunter's Dream gameplay | rising |
| 6 | **send no more X** | — | character stands at the **Bath Messenger** | ~19–20 K |

The current installed route needs exactly three Cross holds: warning,
`Play Offline`, and `Continue`.  This supersedes the older one-Cross route.
Every press is permitted only after a screenshot confirms the named pre-game
screen.  Once the world is visible, do not send Cross.

Step 2 is not padding. Sending input before the warning exists makes
`MainActivity` spawn a second `GameActivity`; the task ends with `sz=0` and the
launcher comes back to the foreground while the `:game` process stays alive with
~17 threads. That failure looks exactly like a hang.

## The Bath Messenger trap

The save used here places the character directly in front of the Bath Messenger.
Any further X hold opens the **shop menu** (`Buy` / `Consumables`, ~7900 draw/s),
not a scene transition. A script that keeps pressing X to "make progress" sits in
that menu forever while its counters look plausible. Leave with two Circle holds:

```powershell
adb -s $dev shell "input swipe 3045 1085 3045 1085 300"   # x2
```

## Automatic gameplay gate

A title-menu or dialog run reports 30 FPS and looks *better* than gameplay, so
any A/B has to reject it mechanically. Two independent signals, both read from
`/proc/<tid>/schedstat` over 3 s and expressed in percent of one CPU
(`scratchpad/gate.sh`):

```text
                havok   gpucomm
menu / dialog       4         3
shop menu          38-44     38-40
world (valid)      46        38
```

Gate: require `havok >= 20` and `gpucomm >= 8` on **two consecutive** probes, so
a transient spike during loading cannot open it. The `havok >= 8` single-probe
gate used earlier passed on the Exit-Game dialog and produced two invalid A/B
runs.

## Valid measurement state

```text
Hunter's Dream, standing at the Bath Messenger
draw ~19,000-20,100/s, submit ~388-426/s, present ~33-35/s
blocks ~333,000 (+0-123/s while the save settles; require a later +0/s window
for a fully warm JIT claim)
havok 46, gpucomm 38
```

Do not compare against the 60-FPS title menu or any run that lacks the world
image and the Havok/GpuComm gameplay gate.  Draw rate alone is not sufficient:
the current renderer/patch combination changed its scale relative to the old
~6100/s capture.
