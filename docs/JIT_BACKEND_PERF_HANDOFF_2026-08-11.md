# LSX4 Bloodborne CPU/JIT and optional Backend-A handoff

> [!IMPORTANT]
> Before any device/FPS/playable measurement, run `scripts/preflight-vivo-performance-test.ps1`;
> stale Android Studio process-trackers and known profiler loads make the result invalid. See the
> mandatory [performance-test preflight](PERFORMANCE_TEST_PREFLIGHT.md).

Captured: 2026-08-11 19:42 MSK  
Primary benchmark: Bloodborne `CUSA03173`, Vivo `10AG263F1B001HM` only  

The authoritative optional-FEX boundary and promotion gates are in
[`FEX_BACKEND_CONTRACT_MATRIX_2026-08-11.md`](FEX_BACKEND_CONTRACT_MATRIX_2026-08-11.md).
The self-contained visual frame-budget report is
[`BLOODBORNE_FRAME_REPORT_2026-08-11.html`](BLOODBORNE_FRAME_REPORT_2026-08-11.html).
Goal: stable 30 FPS at actual internal `960x544`, correct graphics/audio/input, no PS4/PS5 regressions

This is the authoritative continuation point for the current performance work. It records the exact
measured state, conclusions that are and are not proven, correctness invariants, and the gate for an
optional guest-only Backend-A/FEX path. Historical detail remains in
[`CODEX_BRAINSTORM_HANDOFF_2026-08-01.md`](CODEX_BRAINSTORM_HANDOFF_2026-08-01.md) and
[`android_guest_memory_frame_flicker_postmortem.md`](android_guest_memory_frame_flicker_postmortem.md).
The machine-readable device/marker snapshot is
[`JIT_DIAGNOSTIC_MARKER_MANIFEST.json`](JIT_DIAGNOSTIC_MARKER_MANIFEST.json).

## 1. Current result, without optimistic interpretation

- Current JIT reaches correct Bloodborne gameplay on the Vivo. The game process is stopped after the
  last measurement.
- The last source change restored T0 loop profiling cadence from 64 to 1024 iterations while tiering
  is enabled. Against strict tiered-ON cadence64 it removed 17.2% Game:Main instructions/frame,
  11.3% cycles/frame, and 3.9% Game:Main CPU ms/frame.
- Presented throughput was 9.71 versus 9.57 FPS, inside run noise and in the wrong direction. The
  removed profiling instructions were cheap. Instruction count alone is not the target.
- Strict T1 admission produced zero active T1 bodies. Operational prefs were returned to
  `managed_tiered_jit=false` and `managed_jit_trace_compilation=false`.
- T2 trace compilation is a proven loss in this scene: about +11.9% instructions, +8.33% cycles and
  +7.84% task-clock in fixed-wall A/B, with a very large guard/deopt tax.
- Current frame model is approximately 72.9 ms Game:Main producer/preparation plus 30.2 ms GPU and
  0.7 ms submit, effectively serial. Perfect overlap alone would cap near 13.7 FPS; 30 FPS requires
  structural producer reduction and useful overlap.
- Generated JIT code remains dominant: 81.72% of Game:Main cycles in the valid sidecar profile. No
  single generated region dominates; the cost is distributed.
- Optional Backend-A is not yet proven current-runtime compatible and has never been shown running
  Bloodborne. It stays experimental; Current AArch64 JIT stays default.

## 2. Exact provenance

| Item | Exact value |
|---|---|
| Main branch/HEAD | `main`, `a09197bf24a76b91a8415bb33d4e45f7bfa67948` |
| HEAD subject | `perf: optimize tiered JIT and mobile runtime` |
| Funnel HEAD | `58fbbe860190652ff21908b676f95b0e758fe9ce` |
| Worktree | heavily dirty; user, Claude and Codex work coexist; do not reset or bulk-clean |
| Native output | `build/android-arm64-release/liblsx4_executor_android.so` |
| Native/installed SHA-256 | `ce7fc76f083e5e0bfe7afeca45a5521b21134f823634a6d89a8faba73b975fdf` |
| APK | `android-app/app/build/outputs/apk/debug/app-debug.apk` |
| Installed app | `app.lsx4.android`, version `0.1.0`, versionCode 1, minSdk 26, targetSdk 35 |
| Device | Vivo serial `10AG263F1B001HM`; never select by ordinal/model |
| Bloodborne | `/data/user/0/app.lsx4.android/files/lsx4-home/runtime-fs/user/app/CUSA03173/eboot.bin` |

Installed native SHA was read from the package native directory and matches the release output.
Older `.so` files exist in several build/symfs directories; never infer currency from name/time.

Final cadence delta in `retiring_execution_core.cpp`:

```cpp
constexpr std::uint32_t kTieredLoopSafepointPeriod = 1024;
constexpr std::uint32_t kTier0PromotionLoopSamplePeriod =
    kTieredLoopSafepointPeriod;
```

This replaces the former T0 promotion-loop period 64 while retaining the scheduler/HLE bound. A
period of 16384 previously produced 0 FPS by starving an observable boundary; never restore it.

## 3. Operational Vivo state

Relevant `shared_prefs/lsx4_settings.xml` values after the last run:

| Preference | Value |
|---|---:|
| `managed_tiered_jit` | `false` |
| `managed_jit_trace_compilation` | `false` |
| `managed_fast_guest_memory` | `true` |
| `persistent_jit_cache` | `true` |
| `managed_relaxed_fp_fusion` | `true` |
| `managed_audio_simd` | `true` |
| `managed_android_performance_hint` | `true` |
| `managed_vulkan_driver_calls` | `true` |
| `managed_arm_gpu_fast_path` | `true` |
| `managed_adaptive_mobile_gpu` | `true` |
| `managed_async_pipeline` | `true` |
| `managed_readback_batching` | `true` |
| `managed_coarse_fragment_2x2` | `false` |
| `managed_disable_vk_robustness` | `false` |
| `managed_anisotropy_mode` | `2` |
| `res_mode` | `4`; benchmark must still verify actual `960x544` |

Active diagnostic markers:

| Marker | Class | Reason/status |
|---|---|---|
| `run-bisect-disable-jit-tiered-guard-direct-chain` | correctness workaround | aggressive guard chaining corrupted state/resources; keep disabled |
| `run-bisect-disable-jit-preserve-all-helper-abi` | correctness workaround | broad helper ABI lacks complete clobber/fault/unwind proof; keep disabled |
| `run-bisect-enable-jit-fixed-xmm-dirty-observer` | validated path | current fixed-XMM dirty publication/observation contract |

`run-bisect-enable-jit-tier1-equal-footprint.codex-disabled-20260811` is inactive. It previously
allowed equal-size T1 bodies and changed the meaning of measurements. Suffix-renamed `*.ab-enabled`,
`*.diag-off`, backup and layout-probe files are not active switches.

## 4. Benchmark contract

Every performance claim requires:

1. Exact serial `10AG263F1B001HM`; never touch another attached device.
2. Installed/loaded `.so` SHA, prefs snapshot and exact active-marker set.
3. Logged actual GNM/render extent `960x544`; UI selection alone is not proof.
4. Same Hunter's Dream workshop/fountain save, scene, camera and route.
5. Same 14-second analog movement interval.
6. Exact SurfaceFlinger unique presented frames divided by exact wall time.
7. Same-window Game:Main PMU instructions/cycles/task-clock divided by exact frames.
8. For a foreign/container backend, aggregate UID/cgroup CPU for all processes.
9. Immediate force-stop after the sample.

Known fast route:

```text
launch exact CUSA03173 eboot
wait 35 s, Cross at (2920,1220)
wait 13 s, Cross
wait 8 s, Cross
wait about 40 s to the established gate
analog: input swipe 950 1245 950 1060 14000
```

Do not spam Cross: a previous route entered Iosefka's Clinic and invalidated comparison. Inspect a
screenshot only at an unexpected route gate. Temporal artifacts require a 12–30 second high-FPS
recording; a still cannot prove absence.

Rounded HUD FPS, fixed-wall totals without exact frames, different scenes/resolutions, or unverified
installed binaries are not evidence. Temperature/frequency annotate a run but do not replace work
metrics.

## 5. Exact cadence/T1 measurements

| State | Frames / seconds | FPS | Instructions | Cycles | Task-clock | Instr/frame | Cycles/frame | Game:Main ms/frame |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Clean tiered OFF | 138 / 14.001782 | 9.856 | 46,796,537,517 | 22,349,195,724 | 10,065.5971 ms | 339.105 M | 161.951 M | 72.939 |
| Strict tiered ON, cadence64, no equal marker | 136 / 14.001854 | 9.713 | 54,272,922,738 | 24,469,340,758 | 10,139.1495 ms | 399.066 M | 179.922 M | 74.553 |
| Strict tiered ON, cadence1024, no equal marker | 134 / 14.001208 | 9.570 | 44,300,101,066 | 21,382,700,304 | 9,603.270291 ms | 330.598 M | 159.572 M | 71.666 |

Cadence64: T1 active 0, queued 23, rejected 9, `noStaticCostWin=8`,
`profiledEdges=1,620,316`.

Cadence1024: T1 active 0, queued 8, rejected/no-win 8, `profiledEdges=845,684`.
First frame/gameplay stayed correct.

Conclusions:

- Keep cadence1024: it is a valid constant-bookkeeping reduction.
- Its 17.2% instruction/frame reduction yielded only 3.9% CPU-time/frame; target time, not counts.
- Strict T1 currently publishes nothing, so tiered master pays tax without a product.
- Cadence1024 ON versus clean OFF does not prove a throughput win.
- These cross-runs do not isolate residual throughput as GPU serialization; use same-window timeline.

Artifacts: `.codex-cadence1024-*`, `.codex-bb-cadence1024-*`, `.codex-t1-strict-*`,
`.codex-bb-t1-strict-*`, `.codex-t1off-exact-*`, `.codex-bb-t1off-exact-*`.

## 6. T2 trace result

Fixed-wall A/B lacks a reliable frame denominator, so it proves CPU cost direction only:

| State | Instructions | Cycles | task-clock |
|---|---:|---:|---:|
| trace true | 48.222 B | 23.513 B | 9739 ms |
| trace false | 43.093 B | 21.705 B | 9031 ms |
| trace cost | +11.9% | +8.33% | +7.84% |

Trace true: T1 1274, T2 3428, member blocks 16717, guard deopts 76,700,416, observer
callbacks 299,611; exact deopt/callback ratio 256. Trace false: T1 3424, T2 0.

Mechanism:

- Guards are admitted at 80% dominance after only eight samples.
- Rollback needs cold path over 25%, so a stable 80/20 guard can miss forever.
- Each miss pays LSE `ldadd`+test; 255/256 misses flush/return; every 256th enters C++ observer.
- T2 displaced T1 (`2052` `tier2Active` rejects); worker schedules eight T2 jobs before one T1.
- 94,124 queues, 87,974 rejects, 55,036 no-wider builds, 28.47 s planning, 9.13 s emission;
  about 44.3% of compiled traces retired.
- Resident guard chaining caused full texture corruption, black rectangles and bright flashes.

Trace remains off until expected-cost admission includes miss probability, deopt/observer cost,
target lifetime, displacement cost and measured dynamic benefit.

## 7. Causal profile and frame model

Valid profile: `.codex-bb-sidecar-poll.data`, 63,228 samples, three lost.

- Game:Main anonymous generated code: 81.72% of its cycles.
- Two host hot pages: about 6.74% and 6.72% of Game:Main. Exact guest RIP requires a same-PID map.
- Whole-profile shares: Game:Main generated 17.10%, FMOD 7.80%, Thread17 6.0%, Havok 5.37%,
  GpuComm host 4.97%, GpuComm libc 2.87%, driver 1.79%.
- One FMOD generated region is below 2%; point assembly cannot provide a title-wide 2x.
- Historical Vulkan driver CPU is about 1.7–1.9%; not the main CPU wall, though GPU time matters.

Approximate current frame:

```text
Game:Main producer/preparation     72.9 ms
GPU                                30.2 ms
submit                              0.7 ms
observed                           about 100 ms
```

## 8. Completed completion-label sidecar correction

The old page-protection watcher was replaced by exact CPU-side sidecar completion labels in funnel
`liverpool.cpp/.h`, `page_manager.cpp` and tests.

- Old `.codex-bb-precise-logcat.txt`: 544286 labels, 1342 waits, 27.93 s CPU wait over 246 s;
  waits were almost entirely GpuComm, not Game:Main.
- Sidecar `.codex-bb-sidecar-logcat.txt`: 279843 labels, zero waits.
- Removing waits recovered about 0.1135 core and task-clock rose by about the same 0.111 core.

The earlier guest busy-poll hypothesis is withdrawn. Do not add speculative JIT parking based on
these labels.

## 9. Closed or low-ROI directions

| Direction | Result |
|---|---|
| Dirty-mask micro-optimization | about 0.037% |
| Pass-break removal | below about 0.03 ms/frame |
| Poll backoff | about three blocks, no meaningful parks |
| Fixed GPR mapping | already substantially implemented; static coverage understated it |
| One FMOD asm helper | hottest region below 2% |
| Vulkan driver-call micro-edits | driver CPU about 1.7–1.9% |
| Lower than 960x544 | does not solve producer CPU wall |
| Coarse fragment 2x2 | damages image; optional/default off |
| T2 traces | measured negative |
| Strict T1 | zero bodies; master tax |
| Equal-footprint T1 | admitted non-winning bodies; marker inactive |
| Unbounded RET chaining | scheduler starvation / 0 FPS |
| 16384 loop safepoint | loading handoff stopped draw/submit |
| Broad write-back/residency | 0 FPS, frozen frames or corrupt resources |
| Partial GPR write-back | swapped loads for stores+moves and grew code |
| Broad barriers/serialization | hides symptoms by shifting work |
| Alpha/cull/wind/TAA/LOD probes | did not fix temporal/resource corruption |

“N optimized blocks of 332043” is static coverage, not dynamic coverage and cannot be extrapolated
linearly to FPS.

## 10. Correctness invariants

1. Current AArch64 JIT stays default until the alternative backend passes the smoke matrix.
2. Resident state may cross an edge only under an explicit versioned ABI proving every live and
   deferred GPR/XMM/YMM/RFLAGS value; otherwise publish canonical machine state.
3. Guard miss is a deopt/observer boundary and must reconstruct baseline state before observation.
4. Every faultable native site needs exact guest PC/live/deferred recovery metadata.
5. RET/HLE/event/deopt/loop observer boundaries may be amortized, never removed; return is bounded.
6. Narrow GPR writes are read-modify-write for liveness. Legacy SSE is partial at whole-YMM scope;
   VEX/EVEX retains full-definition/upper-zero semantics.
7. Live results cannot alias shared flag-lowering scratch. The old ADC/SBB alias blocked all games.
8. Persistent IR/native ABI changes whenever semantic metadata/register layout changes.
9. Vulkan handle is not allocation lifetime identity. Dedup needs allocation generation+epoch.
10. Flush intersecting pending HLE copies before backing selection/rename; barriers do not repair
    stale backing identity.
11. Shared JIT changes must not regress PS4 or PS5; PS5 page/ABI isolation remains distinct.
12. Accept only reductions in total work/critical path, not work moved to another thread/device.

Full incidents and rejected visual hypotheses are in the frame-flicker postmortem.

## 11. Best remaining Current-JIT targets

1. Same-PID generated-code attribution: capture perf map and PMU together; map hot pages to guest
   RIP/op classes and dynamic state traffic.
2. Region-wide flag producer/consumer fusion only where liveness proves no observer/helper/fault;
   use host flags/FlagM with fallback.
3. Versioned helper ABI with exact per-helper clobber/fault metadata; no broad unproven preserve-all.
4. Dynamic-edge costed residency, accounting for saves, reloads, faults, helpers and observers.
5. Bounded continuous chaining with periodic scheduling/HLE and exact publication.
6. Measured producer/GPU overlap, moving only ownership-independent preparation.

None currently has a justified 2x estimate. Do not promise 30 FPS by summing unrelated percentages.

## 12. Optional guest-only Backend-A/FEX

Desired architecture:

```text
native ARM64 Android UI/runtime and native ARM64 PS4 HLE/Vulkan
                              |
                    versioned backend adapter
                              |
               x86-64 PS4 guest translated by Backend-A/FEX
```

No Debian and no translated emulator are intended. A Bachata container is only an external control.

### Existing path and blockers

- `g_runtime_guest_backend` exists in `native_runtime_api.cpp`.
- Historical private mapping is 1=Current AArch64 JIT, 0=Backend-A/reference box64-fex.
- `executor_lsx4_runtime_launch_game()` currently stores 1 unconditionally, so Backend-A is
  unreachable.
- The `:game` process requires the new preference in `MainActivity.CROSS_PROCESS_SETTING_KEYS`.
- Archived source is under `legacy-backend-a-archive/work/fexcore-android-embed`; old build-script
  relative paths are broken after relocation.
- Recovered proof covers OpenOrbis and FlusterCluck/Unity Mono, not Bloodborne.
- Archived artifacts target API 29 while the app minSdk is 26.

First confirmed contract mismatch: archived embed expects exactly 32 ASCII bytes
`executor:ps4:hle:fexcore:thunk01`; Current emits SHA-derived
`5f194677fad26c435a86129993875668e036d08d135c471412baa403fe1b31aa`.
Loader symbols can pass then fail at the first thunk. Never change Current identity for the archive;
use a versioned Backend-A identity adapter or negotiated identity set.

### Required complete contract matrix

Before execution compare:

- symbols, visibility, library names, signatures, calling convention and unwind assumptions;
- thunk identity/version and capability handshake;
- request/state sizes, alignments, field offsets, enum values and ownership;
- init, mapping, relocation, TLS, HLE, launch, stop and teardown order;
- x86 memory ordering/atomics and page reservation/protection ownership;
- signal/fault resume, TCB/TLS/DTV and thread attach/detach;
- futex/thread scheduling and scheduler observation;
- HLE trampoline range, callback return convention/lifetime/thread safety;
- API level, ELF ABI, packaging, cache identity and licenses.

Close mismatches only in a versioned boundary adapter. Do not bend Current to a stale contract.

### Selector contract

- Typed enum with safe zero/default `Current=0`, `FexExperimental=1`; never expose old inverted raw int.
- UI labels `Current AArch64 JIT` and `Experimental Backend-A` until FEX is proven.
- Selection explicit, persisted, included in cross-process snapshot and logged before initialize.
- No live switching; apply after native library load and before runtime initialize.
- Preserve requested experimental value on unsupported builds but report effective Current+reason.
- Optional JNI exports let old/sandbox runtimes safely fall back to Current.
- Current cache stays unchanged. Experimental backend uses a separate per-title/backend/ABI namespace;
  never reuse Current decoded IR or native records.
- `g_jit_launch_entry_requested`, signal handlers and persistent cache routing depend on effective
  backend; merely removing the launch hard-reset is insufficient.
- Failed experimental launch reports the failed gate and returns safely; no silent fallback during a
  benchmark. Selector must not alter PS5 routing.

### Staged gates

1. Complete offline contract matrix.
2. Build/package/API compatibility.
3. Loader+capability+identity handshake.
4. First HLE thunk with return-state verification.
5. Historically proven light PS4 smoke.
6. Bloodborne first frame.
7. Same gameplay/audio/input/temporal correctness.
8. Same-scene exact SF and aggregate UID/cgroup CPU benchmark.

## 13. Bachata/FEX external gate

The video is qualitative. Before gain estimates:

1. Obtain exact Bachata package/build.
2. Inspect every ELF `e_machine`/ABI.
3. Determine native ARM64 emulator+guest-only FEX versus Debian/glibc+x86-64 emulator under FEX.
4. Inspect rootfs/launcher/build scripts.
5. Respect licensing: Bachata-S4 appears GPL-3; this project is GPL-2-or-later. Do not copy code
   without an explicit licensing decision.
6. If runnable, test same Vivo/save/scene/resolution/input; exact frames and all-process CPU.

If it is not materially faster, retain the safe selector but avoid a week-long Bloodborne port. If
it is materially faster, guest-only FEX gains a justified high-priority hypothesis. It is not a
mathematical floor/ceiling because renderer, upstream, config and device work may differ.

## 14. Evidence, build and communication

High-value artifacts: `.codex-cadence1024-*`, `.codex-t1-strict-*`,
`.codex-t1off-exact-*`, `.codex-bb-trace-{true,false}-*`, `.codex-bb-{precise,sidecar}-*`,
`.codex-bb-sidecar-poll.data`, `.codex-bb-provenance-*`, and `docs/bloodborne_smoke_route.md`.

There are hundreds of stale artifacts. Do not bulk-delete and do not choose binaries by timestamp.
C: was full; set task-specific TEMP/TMP and Gradle user home on D: for builds.

Known native build:

```powershell
$taskTmp='D:\LProj\PetProj\.codex-build-tmp'
$env:TEMP=$taskTmp; $env:TMP=$taskTmp
$cm='D:\LProj\PetProj\executor-pcsx2-lab\work\vcpkg\downloads\tools\cmake-4.3.2-windows\cmake-4.3.2-windows-x86_64\bin\cmake.exe'
& $cm --build build\android-arm64-release --target lsx4_executor_android -j 12
```

Known APK build/install:

```powershell
$env:GRADLE_USER_HOME='D:\LProj\PetProj\.codex-gradle-home'
$env:GRADLE_OPTS="-Djava.io.tmpdir=$taskTmp"
& 'C:\Users\Litesav\.gradle\wrapper\dists\gradle-8.10.2-bin\a04bxjujx95o3nb99gddekhwo\gradle-8.10.2\bin\gradle.bat' :app:assembleDebug --no-daemon --console=plain
.\scripts\install-vivo-apk.ps1 -ApkPath .\android-app\app\build\outputs\apk\debug\app-debug.apk -DeviceId 10AG263F1B001HM -TimeoutSeconds 180
```

Agent Chat room `увеличить fps` is the coordination source; the listener relays new user/Claude
messages to primary Codex. Work happens in this repository; status/evidence goes back to the room.

Stop an experiment immediately on 0 FPS, audio-only frozen frame, bad fonts/route, black rectangles,
flashes, stale-frame alternation, texture corruption, input/audio loss, PS4/PS5 regression, or total
critical-path increase hidden by a local metric. Completion requires controlled stable 30 FPS with
all correctness gates, not a light scene, rounded HUD value, or predicted number.
