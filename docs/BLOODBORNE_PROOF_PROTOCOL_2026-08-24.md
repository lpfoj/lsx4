# Bloodborne proof protocol — 2026-08-24

> **Mandatory preflight:** before every device performance phase run
> `scripts/preflight-vivo-performance-test.ps1` as defined by
> `PERFORMANCE_TEST_PREFLIGHT.md`; evidence captured without this gate is
> invalid.

This protocol is the acceptance boundary for the performance work that starts
from local branch `proof/bloodborne-baseline-20260824`.  No optimization is
promoted from a plausible screenshot, HUD number, archival log, or unmatched
scene.

## Truth rules

1. A numeric zero is a measurement only when the corresponding validity bit is
   set.  Missing counters are `unavailable`; they are never replaced with a
   similarly named counter or an inferred denominator.
2. Production frame-time windows contain no proof/JIT/overlap instrumentation.
   Instrumented captures are diagnostic windows and cannot establish FPS.
3. Every window is bound to one boot, PID, `/proc/<pid>/stat` starttime, UID,
   foreground activity, exact BLAST layer generation, APK/signing identity,
   loaded executor/adapter/bridge/FEXCore/ICD paths and hashes, settings,
   patches, markers, save inventory, cache inventory, route revision, driver,
   thermal state, and device identity.  Any drift rejects the window.
4. Device reads/actions are preceded by the AgentChat event and ownership
   checks.  Every performance phase runs the mandatory
   `scripts/preflight-vivo-performance-test.ps1`; a live process uses
   `-CleanupOnly` before the window.
   A comparable phase additionally requires thermal status <= 2, battery >=
   20%, the declared policy0/policy6 maximum-frequency floors, and exact
   equality of external-power state plus CPU policy min/max limits across all
   A/B/A phase starts.  A run that changes these bands is rejected rather than
   normalized after the fact.
5. Pipeline cache is inventoried and preserved.  It is not silently deleted:
   that previously produced a black gameplay frame and changed the workload.
6. The old app-UID `simpleperf` captures are invalid for the current gate.
   They launched `simpleperf` itself through `run-as` and emitted
   `Running child command in app uid isn't allowed`.  Some artifacts also
   contain counter rows, but the forbidden internal workload diagnostic makes
   that invocation non-clean and ambiguous.  A 2026-08-25 retest proved that
   the shell candidate using `--app` together with `-p` for the exact running
   game PID emits the same diagnostic on this retail Vivo firmware.  Shell
   `-p` without `--app` is denied by `perf_event_open`.  Consequently attached
   hardware PMU is currently **unavailable** on this device.  Exit code zero
   and counter-looking CSV rows do not override stderr.  The old artifacts may
   explain why a conclusion was formed, but cannot pass a current gate.

The in-app HUD has a fail-closed hardware-PMU slot for `Gi/s`, `IPC`, and
`branch-miss%`.  On the retail Vivo build, `perf_event_open` from the app UID
returns `EACCES` (`PMU n/a E13`).  The HUD must not synthesize these values or
start a shell sidecar: a sidecar is a separate profiler process and invalidates
the clean production window.  On a rooted/system/permitted build the native
sampler reads inherited cumulative hardware counters at approximately 1 Hz and
does no work in frame, JIT, or GPU hot paths.  Literally zero-cost measurement
does not exist; the permitted path is only negligibly small.

## Frozen baseline and route

- device: Vivo `10AG263F1B001HM`, Snapdragon 8 Gen 5;
- package/process: `app.lsx4.android` / `app.lsx4.android:game`;
- title: Bloodborne `CUSA03173`;
- CPU backend: actual loaded FEX 2608, no fallback;
- Vulkan: actual loaded Turnip Mesa 26.2.1, including mapped ICD SHA;
- patch: `60 FPS++`; the six established graphics-disable settings remain
  identical in all compared phases;
- warm correctness route: coordinate-free exact `CUSA03173/eboot.bin` launch,
  screenshot-confirmed previous-session warning -> Play Offline -> Continue,
  exactly three pre-game Cross presses, then Hunter's Dream at the Bath
  Messenger; after gameplay appears Cross is forbidden because it opens the
  menu and hides the map workload;
- gameplay gate: Havok >= 20% and GpuComm >= 8% on two consecutive 3-second
  `/proc/<tid>/schedstat` probes;
- each compared phase restores the same immutable save snapshot and verifies
  unchanged save/cache/settings inventories.

The heavy streaming route must be recorded separately as machine-readable
input (timed actions plus position/image anchors) before it is used for an A/B.
A menu, dialog, shop, different camera, different draw workload, or manually
moved run is rejected rather than normalized after the fact.

## Metric availability boundary

The Android PMU negative-control binary and validator live in
`tools/bloodborne-proof/`.  On the target device, positive controls validated
host cycles, host instructions, L1I misses, branch misses, and page faults.
The raw DMB event failed its positive control (95 counts for 1,000,000 explicit
DMB instructions), so executed DMB is `unavailable` on this kernel/PMU path.

The separate default-OFF FEX proof ABI reports masks for requested, valid,
unavailable, exact, sampled, static, dynamic, perturbed, and
attempted-not-retired metrics.  Its current truthful boundary is:

| Metric | Current source | Production use |
|---|---|---|
| host cycles/instructions | unavailable on current retail Vivo attach policy | no; raw CSV is diagnostic provenance only |
| host L1I/branch misses | unavailable on current retail Vivo attach policy | no; negative-control capability alone does not authorize process attach |
| page faults | `/proc` process counters where identity-bound; `simpleperf` attach unavailable | yes only from the declared `/proc` source |
| invalidation requests/completions/wait | marker-gated runtime atomics | diagnostic only |
| generated-code owned faults | PageManager owned-fault counter; exact only with `EXECUTOR_LIVE_QUIET=0`, otherwise explicitly unavailable | diagnostic only |
| compiled guest sites, IR blocks, spill/fill/flag/fence sites, published native bytes | FEX adapter static proof mode | diagnostic only |
| executed IR-block entries | FEX adapter dynamic proof mode | diagnostic and perturbed |
| executed guest instructions | unavailable | no proxy allowed |
| dispatcher entries | unavailable | no proxy allowed |
| generic helper executions | unavailable | explicit Syscall/Thunk sites are only a subset |
| executed DMB | unavailable | explicit fence-to-DMB sites are only a static subset |
| fastmem misses | unavailable until an authoritative source passes review | no inferred zero |

Consequently, the full guest-normalized panel is not yet proven.  Until a
rooted/system/permitted build passes a clean attach negative control, Ginst/s,
IPC and branch-miss% are also unavailable for production acceptance on this
Vivo.  CPU-time deltas, frame distribution, frequency, temperature and the
explicitly available diagnostic subsets may be reported with their exact
sources.  `cycles/guest`, L1I misses/guest, branch misses/guest,
dispatcher/guest, helpers/guest, and DMB/guest remain visibly unavailable.
This is a proof result, not a failed zero.

## Window classes

### Production baseline

- all proof-metric and overlap markers absent;
- Multiblock, CaptureAck, deferred write epoch, bounded invalidation drain, and
  clean-memset HLE remain OFF;
- fresh PID after the full preflight;
- SurfaceFlinger and identity-bound `/proc` windows start and stop inside the
  same device `CLOCK_BOOTTIME` interval; attached PMU is required only on a
  device where its clean capability gate passes;
- no launcher/profiler process is admitted during the window;
- receiver snapshots run inside the already-live `:game` process and are
  rejected if PID/starttime changes.

### Static FEX diagnostic

`diag-fex-proof-metrics-static` is the only diagnostic axis.  Persistent FEX
code cache is disabled to prevent mixed instrumented/uninstrumented blocks.
The result describes compiled shape, not steady FPS.

### Dynamic FEX diagnostic

`diag-fex-proof-metrics-dynamic` is the only diagnostic axis.  Its injected
atomic IR-block counter perturbs code and may add spills.  The capture can
describe IR-block execution density only; it cannot be compared as production
frame time.

### Invalidation and overlap diagnostics

The invalidation source profile and causal overlap collector are default OFF.
Overlap ABI v1 proves only timestamp intersection.  ABI v2 is acceptable only
after explicit request -> drain-batch -> post-arm frame epoch -> owned present
membership, ordering checks, zero censoring/saturation/uncommitted records, and
a separate instrumentation-OFF control.  Even then it proves membership, not
that every millisecond of frame delay was caused by invalidation.

## Required campaigns before optimization

1. Clean production baseline with the synchronized available panel.
2. Separate invalidation diagnostic to retest the archival
   approximately-1011-invalidations/frame hypothesis.  The 2026-08-25 attempt
   with `run-diagnostic-guest-invalidation-profile` failed the observer-effect
   gate: it reproducibly caused `VK_ERROR_DEVICE_LOST` at the XDEV/video
   transition and a later AVVideo SIGSEGV, while the same release build passed
   with the marker absent.  That attempt is rejected, not a production
   regression.  A redesigned bounded/passive observer plus an
   instrumentation-OFF control is required before the campaign can be retried.
3. Causal overlap capture plus a matched instrumentation-OFF frame-time window.
4. Fresh-process MemcpySetTSO `A/B/A`, with the marker as the only changed input.
5. Fresh-process VectorTSO `A/B/A`, run separately after MemcpySetTSO is fully
   restored; never a combined relaxed-memory phase.
6. Clean-memset HLE causal `A/B/A`, followed by a distinct production handoff.
   A positive diagnostic result alone does not promote the default-OFF
   candidate.  Async shader retranslation is no longer a safety gate because
   its submitted payload is an owned snapshot; it still must remain constant
   across A/B/A as an experimental control.  Causal coverage is a separate
   matched diagnostic capture with
   `run-diagnostic-hle-clean-buffer-stats`; its numerator is committed writes,
   not merely prepared/`avoidedSync` tickets, and boundary-active tickets must
   be rejected or explicitly corrected.  Until the runner enforces both
   contracts, `HleCleanMemset` is not an executable promotion axis.
7. Multiblock remains quarantined.  Verify OFF in all baselines and do not run
   an ON performance phase until the historical stall/crash contracts have an
   independent redesign and safety proof.
8. Synthetic eight-slice present-fence timeout test, then a fresh normal PID;
   foreground/background and surface recreation cycles; finally a continuous
   run longer than 40:38 with PID/starttime, progress, surface, RSS, thermal,
   and crash/fault heartbeats.
9. Position-locked heavy streaming route with at least 300 frame intervals,
   average >= 25 FPS, p99 <= 40 ms, zero intervals above 100 ms, and explicit
   p50/p95/p99/1%-low and >40 ms counts.  Average FPS alone cannot pass.

## Optimization admission

Only after the campaigns above produce accepted or explicitly unavailable
results may an optimization branch start.  Each branch changes one mechanism,
has an immediate fallback/deopt path, preserves image/audio/liveness, and uses
the same heavy route.  The initial order is determined by measured cost, not by
the archival ranking: remove proven duplicate work; reduce proven publication
or wait cost; reduce helper/flag/spill cost only if its exact diagnostic subset
is material; add exact reuse; add bounded overlap; relax memory semantics last.

### Mandatory optimization result card

Before changing code, create a local-only Git branch/tag anchor in every tree
that will be modified.  For baseline A, candidate B, and return A2, one result
card records the same gameplay milestone/route and all of the following:

- Moscow timestamp, root/funnel/FEX commit or tag, APK and loaded native-library
  SHA-256, device, SoC/GPU, actual driver/version and actual FEX/no-fallback;
- preflight result, PID/starttime, scene/image anchor, elapsed window and cache,
  patch, settings, power and thermal identity;
- host instructions/s, cycles/s, IPC, branch instructions/s, branch-miss%, and
  any valid FEX guest/static counters; unavailable values stay unavailable;
- effective CPU GHz per policy/cluster, battery/SoC/GPU/skin temperature before
  and after, FPS and p50/p95/p99/1%-low frame-time, >40/>100 ms counts;
- visual, audio, freeze, device-loss and progress verdict, plus `accepted`,
  `retained default-OFF`, or `rejected/reverted` decision;
- interaction with already accepted Ω coordinates and any triggered anti-FPS or
  anti-playable rule.

The game is force-stopped immediately after every measurement and the absence
of `app.lsx4.android` processes is verified.  `X` is used only for the known
three-to-four pre-game transitions; once gameplay load is visible it is not
pressed because the resulting menu hides the map workload.

## Current status

- local baseline commits are recorded in
  `docs/LOCAL_BASELINE_SNAPSHOT_2026-08-24.md` and have not been pushed;
- PMU positive/negative control: complete; DMB unavailable;
- provenance and SurfaceFlinger parsers: implemented and negative-tested;
- FEX proof ABI: host/Android smoke complete, full guest denominator unavailable;
- runtime counters and proof receiver: implemented and host/build tested;
- causal overlap review and campaign runner: in progress;
- retail-Vivo production PMU attach is unavailable; old counter-looking rows
  are provenance only.  Current acceptance uses SurfaceFlinger, identity-bound
  `/proc` CPU/fault deltas, policy frequencies and thermal sensors; exact
  heavy-route proof is still incomplete;
- accepted production optimizations are OPT-H1/G2/C3/C4/G5; OPT-H6/F7/F8 were
  rejected.  OPT-H9 remains implemented default-OFF but its clean exact-route
  2026-08-25 08:13–08:30 MSK A/B/A rejected promotion: A1/B/A2 SurfaceFlinger
  was `34.695/29.562/32.858 FPS`, process CPU
  `4.619/4.716/4.478 core`, minor faults `898/2223/1954`, major faults zero,
  with all windows at `1.9968/2.1120 GHz`.  B is 12.5% below the A/A2 FPS mean
  and 3.7% above its CPU mean.  Visual/audio/liveness passed, the marker was
  removed, and all PIDs were stopped.  Host instructions/s and IPC are
  unavailable; the older `+3.10%` row is not admissible evidence.  Local
  rejection anchor: `bb-opt13-h9-retest-rejected-20260825T0830-msk`;
- OPT-S14 (`D9 · M0 · T2/T5 · O0/O5/O6 · P2/P3`) is accepted as a
  device-runtime policy for the measured Vivo A840, not as a universal source
  default.  A same-PID fixed-camera `moderate floor -> Vivo 4241 -> moderate
  floor` A/B/A at 2026-08-25 08:40–08:46 MSK reported SurfaceFlinger
  `34.747/31.622/32.707 FPS`, process CPU `4.285/4.457/4.364 core`, minor
  faults `1004/779/671`, major faults zero, and p95 `33/50/50 ms`.  The 4241
  phase was 6.24% below the A/A2 FPS mean while using 3.1% more process CPU.
  Exact controller logs proved release/acquire in PID `53855`; image, audio,
  liveness and dropped-frame gates passed.  The marker was restored and the
  process was stopped after A2.  Hardware instructions/s and IPC are
  unavailable (`PMU n/a E13`).  A2 did not restore A1's high frequency band,
  so the result must not be generalized or stacked with max clocks.  Local
  anchor: `bb-opt14-s14-vivo-floor-accepted-20260825T0846-msk`;
- OPT-G15 (`D7/D8 · M0/M7 · T5 · O0/O1/O2/O5 · P0/P2`) tested a bounded
  250-us queue-empty gap only on the owner GpuComm thread.  Fresh-process
  screenshot-locked A/B/A at 2026-08-25 09:05–09:15 MSK reported SF
  `35.485/32.524/32.747 FPS`, p95 `33/50/33 ms`, p99 `50/66/66 ms`, and
  >40-ms fractions `3.232/9.032/2.985%`.  Process CPU was
  `4.503/4.315/4.359 core`; faults `1139/0, 1133/0, 2579/0`.  B and A2 were
  matched at `1.9968/2.1120 GHz` and near-matched shell temperature
  (`40.153->40.226C` versus `40.311->40.571C`).  B was 0.68% below A2 and
  tripled the >40-ms tail, so G15 is rejected.  Visual/liveness passed,
  every phase was stopped, both G15 markers are absent, and S14 remains ON.
  Hardware instructions/s and IPC are unavailable (`PMU n/a E13`).  The HUD
  `submit/s` counter is guest HLE submit-call rate, not physical
  `vkQueueSubmit`; it cannot support a coalescing claim.  Candidate anchor:
  `bb-opt15-g15-candidate-20260825T0900-msk`, root `39227c5978f2`, funnel
  `5b1425e3aa38`;
- OPT-M16 is the clean SF retest of M10 exact protected invalidation runs.
  Fresh A/B/A at 2026-08-25 09:18–09:29 MSK produced
  `36.115/35.597/34.211 FPS`, p95 `33/33/33 ms`, p99 `50/50/50 ms`, and
  >40-ms `1.953/2.953/1.232%`; CPU was `4.348/4.330/4.309 core`, faults
  `2001/0, 2308/0, 2498/0`.  A and B had identical
  `2.3616/2.4384 GHz`; B was 1.43% below A and had a worse tail.  A2 policy0
  fell to 2.112 GHz, so B's +1.23% versus the A/A2 mean is confounded and
  below the predefined +5% gate.  PMU remains unavailable (`E13`).  Image,
  audio and liveness passed, every PID was stopped, the marker is absent,
  G15 remains OFF, and S14 remains ON.  Result: rejected/default-OFF.  Base
  anchor `bb-opt16-m10-retest-base-20260825T0917-msk`, root
  `fbe76f161e75`, funnel `17ace072d46d`;
- OBS-T17 at 2026-08-25 09:33–09:38 MSK used only two
  `/proc/<pid>/task/*/{stat,schedstat}` snapshots around a 10.028-second
  gameplay window.  It is a passive localization result, not an FPS A/B:
  `Game:Main` consumed `1.0092 core` and 147,048 slices, while
  `shadPS4:GpuComm` consumed `0.9683 core`, waited only 9.77 ms runnable and
  executed 78,271 slices.  Six FEX workers consumed `2.2757 core`; five Havok
  workers `0.5854 core`; observed-thread total was `5.759 core`.  The route HUD
  showed about 20,251 draw/s at about 36 FPS (about 562 draw/frame), implying
  about 33,752 draw/s or +66.7% GpuComm throughput for 60 FPS at the same
  workload.  Since SF distribution, clocks and thermal endpoints were not
  captured, no FPS verdict is claimed.  PMU fields remain unavailable (`E13`).
  Evidence is in `.codex-bb-proof/thread-ledger-20260825/`; local anchor is
  `bb-obs17-thread-ledger-a840-20260825T0938-msk`;
- OPT-G18 (`D8 · M0/M5 · T5 · O0/O1/O2/O5 · P2`) at
  2026-08-25 09:44–10:03 MSK tested generation-guarded reuse of the previous
  graphics structural signature.  It removed repeated XXH3 structural-key
  hashes only when primitive override, `GraphicsOtherGeneration`, and
  `GraphicsShaderProgramGeneration` were unchanged; SRT/user-data refresh,
  source validation, map lookup, draw/order/lifetime, and all generation
  invalidation remained intact.  The existing exact-A750 behavior was
  unchanged, while A840 required the fresh-process marker
  `run-enable-generation-guarded-graphics-signature-reuse`.  Fresh A1/B/A2
  produced SF `35.934/33.057/32.426 FPS`, frames `572/531/518`, p95
  `33/33/50 ms`, p99 `50/50/66 ms`, >40-ms
  `2.098/2.825/10.618%`, and zero dropped frames.  B/A2 were clock-matched at
  `1.9968/2.1120 GHz` with shell `40.120->40.301C` and
  `40.562->40.784C`; process CPU was `4.567/4.493 core`, faults
  `1840+6/2155+0`.  B was only 1.95% above A2 and about 0.3% better in
  CPU-seconds/frame, below the +5% gate.  A1 used the higher
  `2.3616/2.4384 GHz` band and is not a causal comparator.  Its first CPU
  collector accidentally read PowerShell's host `$PID`; those zero values are
  rejected.  A corrected adjacent window measured `4.304 core`, 533+0 faults
  and loaded executor SHA `418A...`, but is not called synchronous with A1 SF.
  Instructions/s, cycles/s, IPC and branch misses remain unavailable (`E13`).
  Image/audio/progress/liveness passed, every PID was stopped, S14 stayed ON,
  and the G18 marker is OFF.  Result: rejected/recoverable default-OFF.  Local
  candidate anchor `bb-opt18-g18-candidate-20260825T0947-msk`, root
  `7de0ef95e4b0`, funnel `506928705405`; evidence under
  `.codex-bb-proof/opt18-g18/`.  Anti-practices: accept a hash-elision from a
  mismatched cold phase or one p95, broaden reuse without generation guards,
  or silently repair invalid telemetry;
- OPT-G19 (`D9 · M0/M5 · T5 · O0/O1/O2/O5 · P2/P3`) at
  2026-08-25 10:08–10:30 MSK decoupled exact push-constant elision from the
  Android descriptor-dedup flag that must remain false for the Qualcomm stale
  push-descriptor defect.  The fresh-process marker
  `run-enable-exact-vk-push-constant-dedup` used exact initialized
  `Shader::PushData` bytes plus pipeline identity, command buffer, recording
  tick and scheduler epoch; descriptor/resource/lifetime/order behavior was
  unchanged.  Production A1/B/A2 yielded `36.965/33.993/34.072 FPS`,
  `646/601/606` frames, p95 `33/42/50 ms`, p99 `33/66/86 ms`, >40-ms
  `0.929/7.654/8.251%`, zero drops and average used cores
  `4.2773/4.2160/4.2224`.  B/A2 were matched at `2.3616/2.4384 GHz` and near
  thermally (`38.24->39.04C` versus `38.68->39.71C`); B was `-0.232%` FPS and
  `+0.387%` CPU-seconds/frame.  Instructions/s, cycles/s, IPC and branch misses
  remain unavailable (`E13`).  User menu input made startup frames non-causal;
  a gameplay-HUD state machine stopped all Cross before admitted windows, and
  the first leaked-input B was rejected.  Final state is G19 OFF, G18 OFF, S14
  ON, no PID.  Result: rejected/recoverable default-OFF.  Local anchors are
  `bb-opt19-g19-base-20260825T1008-msk` and
  `bb-opt19-g19-candidate-20260825T1013-msk`; root `b798cc7e2`, funnel
  `691d60d4`; APK `9E9F71D9...`, PS4 executor `7C011A7F...`, PS5 executor
  `F311B549...`, unchanged FEXCore `71D6903D...`; evidence is under
  `.codex-bb-proof/opt19-g19/`.  Anti-practices: couple safe command state to
  unsafe descriptor elision, accept fixed-time startup input, or substitute HUD
  draw rate for unavailable hardware instructions/s;
- OPT-B20 (`B3 · D10 · M0/M5 · T5 · O0/O1/O2/O5 · P2/P3`) at
  2026-08-25 10:34–10:49 MSK first used the marker-only draw-stage profiler as
  localization, not FPS proof.  Stable windows 36–42 reported median
  `16.542 ms CPU/frame`: buffers `4.087`, pipeline `2.299`, textures `1.407`,
  prepare `0.635`, samplers `0.417`, other `4.121`.  The last complete line
  reported buffer obtain `2.210 ms CPU/frame`, but a later 1.98-second
  vertex/index off-CPU wait invalidated the diagnostic run for performance
  comparison.  The candidate then reused only an already resolved exact
  non-written/non-formatted `VkBuffer/offset` within one `BindResources` call;
  writable/formatted bindings cleared the cache, replay/capture disabled it,
  and descriptor submission, barriers, lifetime, ordering and epochs stayed
  unchanged.  A1/B/A2 yielded `35.686/33.853/33.155 FPS`, `624/597/590`
  frames, p95 `33/33/50 ms`, p99 `50/86/86 ms`, >40-ms
  `2.724/4.355/6.441%`, zero drops and CPU-seconds/frame
  `0.116715/0.118710/0.122475`.  B/A2 was only `+2.105% FPS` and
  `-3.074% CPU/frame`, while B regressed against clock-matched colder A1; the
  +5% gate failed.  PMU instructions/s/IPC/branch misses remain unavailable
  (`E13`).  Visual/liveness passed, audio was not separately captured, every
  admitted PID was stopped, and the B20 marker is absent.  Result:
  rejected/recoverable default-OFF.  Anchors
  `bb-opt20-b20-base-20260825T1039-msk` and
  `bb-opt20-b20-candidate-20260825T1041-msk`; root `eec969466`, funnel
  `c112feb3`; APK `5C3C1183...`, PS4 executor `FC6EC14F...`, PS5 executor
  `F311B549...`, unchanged FEXCore `71D6903D...`; evidence under
  `.codex-bb-proof/opt20-b20/`.  Anti-practices: global mutable descriptor
  caching, crossing writable bindings, accepting a favorable sub-5% DVFS
  comparison, or treating a diagnostic stall as production behavior;
- OPT-M21 (`B3 · M11 · D10 · T5 · O0/O1/O2/O5 · P2/P3`) at
  2026-08-25 10:52–10:58 MSK tested default-OFF stable canonical streaming.
  Only ordinary read-only descriptor ranges <=64 KiB without texture aliases
  were eligible; three CPU-streaming uploads were required before a page could
  retain canonical CPU state, and alias/GPU conflict/non-candidate access
  demoted it.  Snapshot/write tracking and mandatory uploads remained intact.
  A1/B/A2 yielded `34.173/35.365/32.950 FPS`, `602/626/587` frames,
  p95 `33/33/50 ms`, p99 `86/50/50 ms`, >40-ms
  `4.651/3.355/8.348%`, zero drops, and CPU-seconds/frame
  `0.117724/0.116789/0.126116`.  Clock-matched A1/B ran at
  `2.3616/2.4384 GHz`; B was only `+3.488% FPS` and `-0.794% CPU/frame`,
  below the +5% gate.  A2 dropped to `1.9968/2.1120 GHz` and contained
  stretched geometry triangles, so favorable B/A2 deltas are inadmissible.
  No `[EXECUTOR_STABLE_CANONICAL_STREAM]` promotion event appeared in B;
  activation of the intended mechanism was therefore not proven.  PMU
  instructions/s/IPC/branch misses remain unavailable (`E13`), audio was not
  separately captured, and manual startup input is non-causal.  Result:
  inconclusive/rejected, marker absent, production unchanged.  Anchors
  `bb-opt21-m21-base-20260825T1052-msk` and
  `bb-opt21-m21-candidate-20260825T1055-msk`; root `63f1709a5`, funnel
  `c112feb3`; evidence under `.codex-bb-proof/opt21-m21/`.  Anti-practices:
  accept a throttled/visually invalid A2, infer promotions from marker presence,
  or keep a sub-gate candidate based on a single favorable FPS sample;
- OBS-D22 (`D7/D8 · M0 · T5 · O0/O5 · P0/P2`) at 2026-08-25
  11:03–11:11 MSK split the marker-only draw profiler into prelude, resource
  setup/flush, descriptor submit, dynamic state, pipeline/vertex bind,
  post-draw and reset.  One full window measured `19.831 ms CPU/frame`:
  buffers `3.886`, pipeline lookup `2.338`, textures `1.377`, vertex/index
  `1.308`, begin rendering `1.188`, draw emit `0.927`, resource setup
  `0.703`, descriptor submit `0.685`, pipeline/vertex bind `0.787` and
  dynamic state `0.278`.  Residual `other` remained `4.527 ms` after twice
  as many timestamp boundaries, proving it is materially profiler-clock
  overhead rather than a production function.  A separate slow pipeline
  probe found zero lookups above 1 ms.  Diagnostic FPS is inadmissible.
  Branch `local/bb-a840-opt22-draw-other-localization-20260825`, anchor
  `bb-opt22-diagnostic-candidate-20260825T1110-msk`, root `4eab8cda7`,
  funnel `cc5d3905`; evidence under `.codex-bb-proof/opt22-draw-other/`;
- OPT-M23 (`D7/D8 · M5/M11 · T5 · O0/O1/O2/O5 · P2/P3`) at
  2026-08-25 11:12–11:23 MSK tested a default-OFF SRT byte-validation
  elision only under exact snapshot identity, uninterrupted non-overlapping
  CPU invalidation history and GPU-clean ranges.  All uncertainty retained
  the original `memcmp`.  A1/B/A2 yielded `34.777/31.798/30.790 FPS`,
  `612/568/554` frames, no drops, p95 `33/50/42 ms`, p99 `50/66/58 ms`,
  >40-ms `2.288/8.275/13.718%`, CPU-seconds/frame
  `0.118824/0.128891/0.135054`, clocks
  `2.3616/2.4384 · 1.9968/2.1120 · 1.7856/1.9776 GHz`, and shell
  `38.71→39.57 / 40.24→40.80 / 40.92→41.35°C`.  Proof confirmed an
  actual `firstHit=1 byteValidationSkipped=1`, but B alone contained a large
  black stretched geometry triangle.  Raw B/A2 was only `+3.27% FPS` and
  `-4.56% CPU/frame` with higher B clocks.  Correctness and +5% gates fail;
  PMU remains unavailable (`E13`), audio was not separately captured.
  Result: rejected/default-OFF, marker/proof absent, production unchanged.
  Anchors `bb-opt23-srt-epoch-base-20260825T1112-msk` and
  `bb-opt23-srt-epoch-candidate-20260825T1116-msk`; root `fb4e540c0`, funnel
  `497fa1c7`; evidence under `.codex-bb-proof/opt23-srt-epoch/`.
  Anti-practices: treat tracking as content proof, accept a corrupt B frame,
  or add a title-specific renderer workaround to hide the defect;
- OPT-H24 (`D7/D9 · M4/M7 · T5 · O0/O2/O5 · P2/P3`) at 2026-08-25
  11:30–11:51 MSK tested a marker-only typed batch for the four known Android
  PM4 deferred completion classes.  It replaces one heap-allocated
  `UniqueFunction` per EOS/EOP/compute RELEASE_MEM/IRQ-only packet with a
  72-byte record while preserving retirement tick, late clock/perf-counter
  sampling, CPU fence write, snapshot invalidation, completion map, EOP trace,
  IRQ and priority-worker ordering.  Unknown producers retain the generic
  fallback and marker-OFF retains the old route.  A1/B/A2 yielded
  `32.269/30.490/29.686 FPS`, `574/552/538` frames, no drops, identical
  p50/p95/p99 `33/50/66 ms`, >40-ms `6.620/8.877/12.454%`, CPU-seconds/frame
  `0.126777/0.136938/0.141431`, clocks
  `1.9968/2.1120 · 1.7856/1.9776 · 1.7856/1.9776 GHz`, and shell
  `39.92→40.77 / 41.12→41.50 / 41.27→41.65°C`.  The B proof log confirmed
  actual execution.  Clock-matched B/A2 was only `+2.708% FPS` and
  `-3.177% CPU/frame`; A2 also had six major faults, so the comparison is
  already biased in B's favour.  All screenshots were correct; near-camera
  bush occlusion was explicitly excluded from geometry-corruption evidence.
  PMU remained unavailable (`E13`), audio was not separately captured, and
  Android hidepid prevented the loaded `/proc/maps` SHA without affecting
  liveness.  Result: rejected below the predefined +5% gate, default OFF,
  marker absent, S14 present, PID absent.  Anchors
  `bb-opt24-typed-completion-base-20260825T1132-msk` and
  `bb-opt24-typed-completion-candidate-20260825T1140-msk`; root `552747318`,
  funnel `7f99efe5`; evidence under `.codex-bb-proof/opt24-h24/`.
  Anti-practices: accept a tail-only improvement without the throughput gate,
  count startup clicks as workload, treat a bush as SRT corruption, or keep a
  broad allocator change when the measured residual gain is sub-gate;
- OPT-M25 (`D3/D7 · M3/M6 · T5 · O0/O2/O5 · P0/P2`) at 2026-08-25
  12:09–12:24 MSK built a marker-only exact single-reference page→epoch-token
  index for `BeginFexDeferredBufferCapture`.  Exact address/token validation is
  mandatory; collisions, stale slots, ownership transitions and ranges of at
  least 4096 pages retain the original bounded scan.  Focused contracts passed
  `24/24`, the Vulkan bridge passed, PS4/PS5 targets built, canonical FEX stayed
  at `56be0e219c8b51ef6a8dfd40a8b09fa3909cf046`, and the adapter host suite
  passed `15/15`.  Device A1/B produced `32.163/29.810 FPS`, `576/543` frames,
  no drops, p50/p95/p99 `33/50/66 · 33/50/86 ms`, >40-ms
  `7.292/17.311%`, CPU-seconds/frame `0.127812/0.139208`, clocks
  `1.9968/2.1120 · 1.7856/1.9776 GHz`, and shell
  `40.25→40.89 / 41.30→41.63°C`.  These numbers are deliberately not treated
  as an optimization A/B: the scenes and clocks differed, and the B startup
  proof said `requested=0 configured=0 captureAck=0 deferredBulk=0`, with no
  epoch-index activation line.  Therefore the current production route never
  executed the candidate.  A2 was skipped to avoid needless heating after the
  activation gate had already failed.  Result: rejected as unreachable on the
  current route, default OFF, marker absent, S14 present, PID absent.  Anchors
  `bb-opt25-exact-epoch-index-base-20260825T1209-msk` and
  `bb-opt25-exact-epoch-index-candidate-20260825T1214-msk`; root `2933d6172`,
  funnel `9b8fb2b1`; evidence under `.codex-bb-proof/opt25-m25/`.
  Anti-practices: attribute an archived r130 hotspot to today's production
  route without an activation proof, compare unmatched scenes as causal, or
  spend a third hot run after the tested axis is proven inactive;
- OPT-M26 (`D7/D8 · M4/M5 · T5 · O0/O2/O5 · P2/P3`) at 2026-08-25
  12:30–12:41 MSK sends an exact already-validated cache-hit flattened userdata
  payload directly into the stable selected shader permutation with
  `vector::assign`.  This reuses capacity and removes the intermediate
  `Program::info` allocation/move plus destruction of the selected vector's
  previous storage; SRT validation, module identity, bindings, descriptors,
  barriers, lifetimes and Vulkan commands remain exact.  Focused contracts
  passed `18/18`, the Vulkan bridge and PS4/PS5 builds passed, canonical FEX
  remained `56be0e219c8b51ef6a8dfd40a8b09fa3909cf046`, and its Android/host suite
  passed `15/15`.  A1 OFF / B ON was `34.853/31.546 FPS`, `617/567` frames,
  no drops, p95 `42/50 ms`, p99 `86/66 ms`, >40-ms `5.673/8.289%`, and
  CPU-seconds/frame `0.115575/0.132257`.  This is not a causal performance
  estimate: B policy clocks were `15.45/13.39%` lower and its shell began
  `2.36C` hotter.  The B proof did confirm
  `firstHit=1 directSelectedCopy=1`; the user confirmed the questioned polygon
  was normal camera geometry, and no freeze/device-loss/drops/major faults were
  present.  A2 was omitted after the user directed that the correct candidate
  remain active.  Result: retained with marker ON and S14 ON, PID absent;
  effect size remains pending a later matched moving-scene proof.  Anchors
  `bb-opt26-cached-permut-flattened-base-20260825T1230-msk` and
  `bb-opt26-cached-permut-flattened-candidate-20260825T1233-msk`; root
  `55ef9c7cc`, funnel `06594d19`; evidence under
  `.codex-bb-proof/opt26-m26/`.  Anti-practices: turn unmatched DVFS/scene
  phases into a speedup/regression claim, mistake normal camera geometry for
  descriptor corruption, or relax exact payload validation to avoid a copy;
- the 20:57 production-state audit supersedes the earlier 09:38 snapshot.
  The locally installed FREE runtime is rooted at `ba11a5074` with Funnel
  `02e283660`, uses FEX, bundled Turnip Mesa 26.2.1 and `60FPS++`, and retains
  the accepted production/conditional markers `D31/D48/M26/S14/D51/D53/D57`
  and `D58/D59/D61/D62/D63/D64`.  In particular, multiblock is now enabled by the
  accepted D31 gate; the former statement that all multiblock markers were
  absent is stale and must not be used for later comparisons.  The canonical
  detailed ledger, individual effects and rollback anchors are
  `BLOODBORNE_FRAME_REPORT_2026-08-11.html#optimization-status-20260825-2057`,
  `#production-anchor-index` and `#proof-card-20260825-d64`.  Live diagnostic,
  capture and proof markers remain absent.  This state still does not
  authorize skipped draws, cross-thread Vulkan, global waits or title-specific
  renderer paths;
- the 23:23 Android lifecycle/audio checkpoint adds independent pause reasons
  (background, stop, focus, side menu and terminating), a fail-closed PS4
  SIGSLEEP capability proof, PS1 frame/audio gating, and honest cooperative PS5
  entry/vblank/HLE plus sparse-JIT coverage.  Transient pause preserves decoder
  and port state; title-session teardown now stops AudioOut scheduler/recovery,
  closes AAudio safely, clears ports and joins every AJM context so a second
  title cannot inherit stale/muted audio.  Host proof is 52/52 focused contracts,
  Java compile, both Android native targets and the Vulkan guard.  Vivo/FEX APK
  `284692DB...774DF` proved HOME pause with `sleepSignalHandler=true`,
  `guestPaused=true`, `audioPaused=true` and an exact successful resume.  That
  run also exposed Android 13 duplicate Back ownership; API 33+ now routes it
  only through `OnBackInvokedCallback`.  Corrected APK `3E63BC47...113D9`
  passed host proof but is not yet device-approved: secure keyguard blocked
  reinstall with `INSTALL_FAILED_ABORTED`, so held-open side-menu and sequential
  two-title audio smoke remain mandatory.  Canonical detail is
  `BLOODBORNE_FRAME_REPORT_2026-08-11.html#lifecycle-audio-checkpoint-20260825-2323`;
- OPT-M10 (`D3/D7/D9 · M0/M7 · T5 · O0/O1/O2/O5 · P1/P2`) is implemented on
  local branch `local/bb-a840-opt10-protected-runs-20260825`, fresh-process
  marker gated and default OFF.  At 2026-08-25 06:22–06:43 MSK its static
  Hunter's Dream A/B/A was A `32 FPS, 10.861 Ginst/s, IPC 1.697, BM 2.218%,
  1.487 GHz, shell 40.29→40.32C`; B `29 FPS, 10.262 Ginst/s, IPC 1.746,
  BM 2.080%, 1.331 GHz, shell 41.03→41.04C`; thermal-matched A2 `28 FPS,
  10.176 Ginst/s, IPC 1.742, BM 2.148%, 1.359 GHz, shell 41.20→41.26C`.
  The historical raw rows said B versus A2 was `+0.85%` wall instruction
  throughput, `+0.20%` IPC, and `-3.16%` relative branch-miss rate, with no
  visual/audio/liveness fault. They are now explicitly inadmissible because the
  captures used app-UID `run-as simpleperf` and emitted the forbidden workload
  diagnostic; no performance gain is claimed from them.
  A separate perturbed coverage run at 06:49–06:52 MSK observed `480256`
  candidate calls, `1019351` requested pages, `667429` exact protected pages,
  and `480525` protected runs by gameplay.  The `351922` clean pages are a
  34.52% upper bound, not exact savings versus the baseline potential span,
  which already trims clean edges.  The diagnostic's FPS/PMU are inadmissible
  because source atomics/logging were enabled.  This is retained default-OFF,
  not promoted: position-locked moving frame-time A/B/A remains required;
- OPT-F10 / Ω-08, post-publication removal of FEX `InlineJITBlockHeader` entry
  instructions from the adapter observer, was rejected before implementation
  at 2026-08-25 06:18 MSK.  The observer contract publishes/observes executable
  ranges but does not own a safe mutable-code transition; binary patching would
  introduce execution/invalidation/I-cache races.  No FEX source or device
  state changed;
- OPT-H11, integer-only FEX HLE XMM round-trip elision, was implemented behind
  a fresh-process default-OFF marker and rejected at 2026-08-25 07:20 MSK.
  The attempted A/B/A reached the same Hunter's Dream milestone with correct
  image/liveness, but its start frequency bands drifted and all three PMU files
  used the forbidden app-UID `run-as simpleperf` path. Their counter rows are
  provenance only and cannot support a gain or regression. Independent code
  review found the stronger rejection: an integer-only callee descriptor proves
  argument/result classes, not that FEX's other live guest XMM state survives a
  native thunk without the existing reconstruct/set materialization contract.
  The marker was removed, every process was force-stopped, and the implementation
  is preserved only at local rejection anchor
  `bb-opt11-h11-rejected-20260825T0721-msk` before being reverted;
- OPT-P12, replacing one PageManager capture-drain `sched_yield` with AArch64
  userspace pause, was rejected and reverted at 2026-08-25 08:02 MSK.  Matched
  B/A2 was `29.856/30.411 FPS`, the same p95/p99, and
  `5.812/5.883 CPU-core`; the archived r130 hotspot was not current because
  capture/deferred markers are absent in production.  Local restored anchor:
  `bb-opt12-p12-rejected-20260825T0802-msk`;
- the 2026-08-25 invalidation-profile attempt is classified AF-33 observer
  effect.  Current-marker audit found capture, deferred epoch, proof metrics
  and invalidation profiling all OFF before the attempt.  The marker is absent
  again and cannot be combined with FPS acceptance;
- known-good local boundary at 2026-08-25 06:04 MSK uses tag
  `bb-known-good-a840-20260825T060408-msk`, funnel commit `823c4437`, and FEX
  adapter commit `cb73660`; nothing is pushed to a remote.
