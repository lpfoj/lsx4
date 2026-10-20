# Vivo A840 Bloodborne CPU optimization log (2026-08-29)

Performance evidence is valid only after `scripts/preflight-vivo-performance-test.ps1` passes.
See `PERFORMANCE_TEST_PREFLIGHT.md`; any run that skips or fails this gate is invalid.

Scope: Vivo `V2546A`, SM8850/Adreno 840, Bloodborne `960x544`, upscaler Off.
POCO/A750 results are deliberately excluded.

## Fixed route

- Device `10AG263F1B001HM`; root `8gen3fin` at `da1daa2e35a1`; renderer at `b6408aea1fe8`.
- Current installed APK SHA-256: `0763530e1e7808903897dd182412104da6c02d11e1643ccb978ac213b17a65b5`.
- FEX requested/effective; adapter v3, mapped-entry ABI v5, software TSO, SVE VL16.
- Bundled Turnip SHA-256 `b56a4cec9af33af61ec068efb1d8673b54bb325635ca7a2300db77df82d752cd`.
- Mesa 26.2.1 `git-da14d65e44`, Vulkan 1.4.354. Every run passed clean preflight.

## Final APK runtime provenance

The installed APK and its device `base.apk` both hash to `8ea87dc2...cd70`.
Its canonical PS4 executor is `3358ef1a82e7f617c26123b04411ec442ccb05ea7aeecf7aeaa897762ebaeb8`.
With the package stopped, that exact file was copied through the documented
`run-as` deployment path to both `files/liblsx4_executor_android.so` and
`files/runtime/liblsx4_executor_android.so`; both device hashes were verified.
No preference, driver, game, save, or cache directory was changed.

On the subsequent launch, `/proc/7202/maps` proved that Bloodborne loaded
`/data/data/app.lsx4.android/files/runtime/liblsx4_executor_android.so`.
The eight-frame series remained geometrically stable at `960x544`, upscaler Off:
`artifacts/dualgoal/vivo-a840-final-canonical-series-20260829-230319`.

The first canonical 12-second window is retained only as a DVFS-confounded
observation: SF `36.347`, presents `35.780/s`, draws `16,206/s`, submits
`429.496/s`, and `3.6295` used cores. Its policy0/policy6 medians were only
`1.9968/2.112 GHz`, versus the matched baseline's `2.3616/2.4384 GHz`, after a
long launch and screenshot series. Temperature was `40.12 C`. It therefore
cannot attribute a regression to the cleanup. Evidence:
`artifacts/dualgoal/vivo-a840-final-canonical-proof-20260829-230401`.

The retained-marker audit found no accidentally disabled prior A840 gain:
D31 multiblock, D102 MaxInst 256, D51 fast bulk write, D53 leaf-boundary
elision, D98 fused bulk lease, the Vivo moderate floor, and `run-live-quiet`
were all present. The mutually exclusive MaxInst 512 marker was absent.

## Reset baseline

Reset means Global Balanced plus Bloodborne GameDB, not a complete game preset.

```text
FEX; Fast/HostMapped memory; linking On; persistent cache Off
replacements Safe; clock 100%; cycle skip Off; idle/wait/spin Safe
workers 6; event Fast; JIT Safe/256; FP Accurate; atomics Compatible
cache-line and DMA accuracy Off; memory exceptions On; icache audit Off
Dual CPU/GPU; scheduler OS; heavy threads 4; subsystem/IO/audio threads On
guest delay 50 us; sync-after-idle On; CPU/GPU sync Adaptive
PreferHLE; HLE timers Fast
```

Baseline was 39.831 SF FPS / 38.976 presents/s. A matched repeat was 40.058 SF
FPS / 39.334 presents/s at 2.3616/2.4384 GHz. Frame-series geometry was correct.

Evidence:

- `artifacts/dualgoal/vivo-a840-baseline-20260829-222739`
- `artifacts/dualgoal/vivo-a840-baseline-proof-20260829-222805`
- `artifacts/dualgoal/vivo-a840-baseline-repeat-proof-20260829-223521`

## A/B 1: aggressive function replacements

Only `function_replacements: Safe -> Aggressive` changed. Matched baseline/candidate:
SF 40.058/36.813, presents/s 39.334/36.258, draws/s 17,673/16,478, cores
3.591/3.589, and identical 2.3616/2.4384 GHz medians. The candidate regressed by
about 8%; `hleFastCalls` had no new witness. Correctness passed, but the treatment
is **rejected**. Exact baseline preferences were restored and hash-verified as
`ebe955d64cccf95ea864802e1dce9c48c7a58cdb26e7997f34060f6794c9d83c`.

Evidence: `artifacts/dualgoal/vivo-a840-function-aggressive-20260829-223248` and
`artifacts/dualgoal/vivo-a840-function-aggressive-proof-20260829-223324`.

## A/B 2: aggressive HLE timers

Only `hle_fast_timers: Fast -> Aggressive` changed. Matched baseline/candidate:
SF 40.058/39.977, presents/s 39.334/39.077, draws/s 17,673/17,646, and cores
3.591/3.603. Startup was 48.63/48.51 seconds. The candidate had accumulated
286,727 fast calls before proof, but the steady delta was zero; accurate calls
advanced 7,514. Mean throughput was neutral, so the riskier mode is **not retained**.
Exact baseline preferences were restored and hash-verified.

Evidence: `artifacts/dualgoal/vivo-a840-hle-timer-aggressive-20260829-223934` and
`artifacts/dualgoal/vivo-a840-hle-timer-aggressive-proof-20260829-224011`.

## Admission audit before the next APK

Across all four 12-second proofs, steady deltas for `idleYields`, `waitParks`,
`spinParks`, `dmaDelays`, `guestThreadsDelayed`, `workerStartsThrottled`, and
`cpuGpuSyncs` were zero. Fast-HLE delta was also zero; accurate HLE calls advanced
about 7,500 per window. Thus idle/wait/spin, post-idle fence, Instant event timing,
more worker slots, Relaxed CPU/GPU sync, and another timer treatment have no current
steady witness. Fewer than six workers can deadlock Bloodborne startup.

Protected boundaries: D102 exact-Multiblock 256 geometry; scheduler OS (historical
34.0 versus 28.1 FPS for Performance affinity); persistent cache Off; memory
exceptions On; icache audit Off; atomics Compatible. Fast atomics would jointly
weaken rejected D32 MemcpySetTSO and D41 VectorTSO axes, each below a 1.2%
normalized code effect. These are not to be changed in the next matrix.

### Accepted next source candidate: identity clock scaling

Current `clock=100`, `cycle_skip=0`, `force_real_clock_sync=0` still executes the
general `ScaleGuestDuration` quotient/remainder path for every guest time read.
The retained N3a census measured 43,102,835 `posix_gettimeofday` calls, 46.70% of
92,274,688 HLE boundaries. The exact cleanup is an early identity return when
clock is 100% and the cycle-skip divisor is zero. It preserves `force_real=0`, so
short-timer semantics do not change. Parent is adding this to the next APK; the
intermediate APK and the force-real settings probe must not be run.

`force_real_clock_sync: 0 -> 1` is only a no-build mechanism probe: it removes the
same scaling arithmetic but also disables short sleep and HR-timer shortcuts. It
must never be promoted from FPS alone. After the new APK baseline, the next truly
untested setting is FP Accurate -> Approximate, which changes FEX x87 FP80 to FP64
and needs long physics/audio/geometry validation. Relaxed is excluded: it keeps the
same FP64 stack and adds FP64 -> FP32 -> FP64 on every x87 write.

## Accepted source candidate: identity time path plus lock-free frozen HLE profile

APK `e5a66c993cf2286379562daeddac6e364c81eae8ebcb816c7055dc74e4c0a441`
contains PS4 executor
`6c16b4d00a7ade3218412bab1ebb2d086da10f0a65b8ae8c8b5f3bbdc17d75b4`.
Both sandbox runtime locations were refreshed with that exact file without
clearing application data. The running process map proved the runtime was loaded
from `files/runtime/liblsx4_executor_android.so`. It also loaded the packaged FEX
core, adapter, and bridge with hashes `198d1938...c78d`, `e837931f...9a89`, and
`d3d52028...fa14`, respectively.

This candidate combines three semantics-preserving hot-path cleanups applicable
to A840: the exact 100% clock identity return, reading the immutable prelaunch
CPU profile through `CpuTuning::Frozen()` instead of copying it under
`ProfileMutex` at every HLE boundary, and skipping FPCR refresh for exact typed
integer-only time calls. The A750-only direct-leaf fence cleanup is present in the
same APK but is not eligible on this A840 route.

Eight screenshots captured from runtime time 01:02 through 01:27 remained
geometrically stable, with no delayed corruption or stale-resolution frame:
`artifacts/dualgoal/vivo-e5a66c99-series-20260829-232709`.

At the matched policy0/policy6 medians of `2.3616/2.4384 GHz`, the 12-second
window measured SF `42.246 FPS`, presents `41.224/s`, draws `18,704.827/s`,
submits `494.759/s`, and `3.6194` used cores at `39.10 -> 39.35 C`. Relative to
the matched reset repeat, this is +5.46% SF FPS and +4.80% presents/s. The bundle
is therefore **retained**; the result does not claim that any one of its three
cleanups independently owns the full gain.

Evidence: `artifacts/dualgoal/vivo-frozen-hle-20260829-232825`.

## A/B 3: FP Approximate

Only `fp_accuracy: Accurate -> Approximate` changed. The first launch is retained
for correctness only: all eight screenshots remained geometrically stable, but
the device was already DVFS-capped at `1.9968/2.112 GHz`, so its FPS cannot be
used for attribution. Evidence:
`artifacts/dualgoal/vivo-fp-approx-series-20260829-233138`.

The cold retry matched the accepted candidate's policy0/policy6 medians exactly
at `2.3616/2.4384 GHz`. It measured SF `40.407 FPS`, presents `39.440/s`, draws
`17,940.733/s`, submits `473.834/s`, and `3.5728` used cores at
`39.34 -> 39.97 C`. Against the retained source-candidate baseline of SF
`42.246` and `41.224` presents/s, this is -4.35% SF FPS and -4.33% presents/s.
The FP Approximate treatment is therefore **rejected** for Bloodborne on A840.

The application was stopped immediately after the proof and the exact original
preferences backup was restored. The active `lsx4_settings.xml` was verified as
`ebe955d64cccf95ea864802e1dce9c48c7a58cdb26e7997f34060f6794c9d83c`.

Evidence: `artifacts/dualgoal/vivo-fp-approx-matched-20260829-233720`.

## A/B 4: exact typed-time direct callback (N3a.5)

N3a.5 does not remove FEX's architectural spill/fill. It adds an append-only
host callback that is eligible only for the separately negotiated exact typed
identities `sceKernelGettimeofday`/`ejekcaNQNq0` and
`posix_gettimeofday`/`n88vx3C5nW8`. The adapter keeps both deferred-write epoch
publications, guest-exit/controlled-failure handling, the context-epoch gate,
and the unchanged generic fallback. On a successful exact call it avoids only
the generic 256-byte call frame, 96-byte result frame, descriptor resolution,
and their copies. Unknown, FP, stack, exit, and context-replacing targets fail
closed to the old path. Full N3b spill/fill removal remains rejected without a
separate fault/liveness proof.

The deployed PS4 executor is
`000bb6fd82ed130a912ea7f9e1f4a41cb810a2b8cf37e3ed78647a78880a2548`;
both Vivo sandbox runtime locations were hash-verified. The canonical FEX
adapter/bridge/core hashes are `552d81cc...5004`, `6081df13...f8aa0d8`, and
`198d1938...c78d`. The same-binary control used only the process-immutable
`run-disable-fex-typed-time-direct` marker. The final marker state is absent, so
the accepted path is enabled. The final APK was installed in place without
clearing data; its device `base.apk` and local file both hash to
`8a519fe3d58f5d2dc990426ca97c594867860b41ca2d5e7e7bbb312f54ee4e7f`.
The exact original preferences remain
`ebe955d64cccf95ea864802e1dce9c48c7a58cdb26e7997f34060f6794c9d83c`.

Two early controls are excluded because the route crossed OriginOS's DVFS
threshold and measured at only `1.9968/2.112 GHz`. After deep cooling, the valid
same-binary order was candidate A1, control B, candidate A2. All three windows
had policy0/policy6 medians `2.3616/2.4384 GHz`, instrumentation Off, upscaler
Off, and nearly identical measurement temperatures:

```text
              SF FPS   present/s   draw/s       cores    shell C     route s
candidate A1  41.018   40.068      18,074.696   3.5760   39.42-39.87 49.97
control B     40.408   39.601      17,828.258   3.5892   39.33-39.71 50.49
candidate A2  41.630   40.554      18,344.153   3.6246   39.30-39.75 46.55
```

A1 is the strict route-time match to B: `+1.51%` SF and `+1.18%` presents.
A2 confirms the direction but had a 3.94-second shorter route, so its larger
`+3.02%`/`+2.41%` deltas are not used as a standalone causal claim. The two
candidate mean is SF `41.324` and presents `40.311/s`, `+2.27%`/`+1.79%` over
B. This is accepted as a small semantics-preserving gain, not a claim that
N3a.5 alone reaches 60 FPS.

Correctness is covered by the eight-frame candidate series from runtime 01:05
through 01:40; geometry remained stable with no delayed corruption or stale
resolution frame. ABI/layout, typed-identity, direct-leaf isolation, resume,
lease, epoch, and Vivo preflight contracts pass. The ABI layout test was also
corrected to assert the actual append-only `Lsx4FexHostCallbacksV3` size of 80
bytes.

Evidence:

- `artifacts/dualgoal/vivo-typed-time-series-20260829-235142`
- `artifacts/dualgoal/vivo-typed-time-candidate-matched-20260829-2410`
- `artifacts/dualgoal/vivo-typed-time-control-full-repeat-20260829-2416`
- `artifacts/dualgoal/vivo-typed-time-candidate-a2-20260829-2422`

## A/B 5: terminal controlled-thunk dynamic save elimination (N3b-safe)

The full N3b proposal remains rejected: static architectural spill/fill cannot
be removed because the time callback writes guest memory and may fault, while
callback, deferred-epoch, context-replacement, and `ResumeContext` semantics
still require the published `CpuStateFrame`. The accepted subset removes only
`PushDynamicRegs`/`PopDynamicRegs` when the IR carries both fail-closed proofs:
the thunk uses the controlled ABI and is terminal with no dynamic SSA value
live across it. Static GPR/XMM/NZCV/PF/AF spill/fill remains unchanged. Legacy,
unknown, or future non-terminal thunk forms preserve all dynamic registers.

On AArch64 this avoids saving and restoring 7 dynamic GPRs and 14 vector
registers: a 288-byte aligned stack write plus a 288-byte stack read per
eligible controlled boundary. The same APK exposes an append-only control bit;
the marker `run-preserve-fex-terminal-thunk-dynamic-regs` restores the old
dynamic save path and disables persistent/lazy code-cache reuse so candidate
and control translations cannot mix.

Correctness was checked before performance attribution with eight frames over
approximately 35 seconds. Runtime advanced from 01:34 to 02:25 with stable
geometry and no freeze, stale frame, delayed corruption, or two-resolution
flicker. Compiler-object disassembly and source contracts verify that the two
dynamic save calls are conditional, while static spill/fill and
`ResumeContext` restoration remain unconditional.

The valid same-binary order was candidate A1, control B, candidate A2. Every
12-second window matched policy0/policy6 medians at `2.3616/2.4384 GHz`, used
instrumentation Off and upscaler Off, and stayed in the same thermal window:

```text
              SF FPS   present/s   draw/s       cores    shell C     route s
candidate A1  41.064   40.112      18,174.857   3.5962   38.95-39.57 48.62
control B     40.536   39.584      17,912.763   3.5283   39.29-39.85 50.83
candidate A2  40.864   39.907      18,031.029   3.5880   39.35-39.76 47.10
```

Both candidates are above the control on SF, present, and draw throughput. The
candidate mean is SF `40.964`, presents `40.010/s`, and draws `18,102.943/s`:
`+1.06%`, `+1.08%`, and `+1.06%` respectively versus B. This is retained as a
small repeatable self-tax reduction, not a claim that the remaining full N3b
is safe or that this stage alone reaches 60 FPS.

The accepted final marker state is absent. The app is force-stopped after the
run. The installed and local APK hash is
`4bb49fcda866c7210fb758810267ddea7e98788babb97e8e70759ee0beaff81d`;
both sandbox executor copies hash to
`d1abf714a06b99480ee5cb36ae60ee4199f30e92ed2f4a940f01d89e15bf845c`.
The packaged FEX core/adapter/bridge hashes are respectively
`c7b50940a41be00decfce40fdc22c74cfa571fecf3bff45e595d9a64eb863319`,
`0fc95a8b31fec7e6d16d74189e2a8747adee08cfb23c04ec10d983e37d56a12b`,
and `7f972c08fe78936996f597160988bd2a4ecf7847a8cba26f9920df009397bba0`.
Preferences remain unchanged at
`ebe955d64cccf95ea864802e1dce9c48c7a58cdb26e7997f34060f6794c9d83c`.

Evidence:

- `artifacts/dualgoal/vivo-terminal-thunk-candidate-series-retry-20260829-2437`
- `artifacts/dualgoal/terminal-thunk-a1-20260829-2500`
- `artifacts/dualgoal/terminal-thunk-b-20260829-2504`
- `artifacts/dualgoal/terminal-thunk-a2-20260829-2508`

## Rejected: direct in-JIT typed-time writer

The follow-up candidate recognized distinct `sceKernelGettimeofday` and POSIX
`gettimeofday` thunk identities, calibrated `CNTVCT_EL0` against realtime at
runtime creation, and wrote an exact `timeval` directly from generated AArch64.
It was gated to A840 + CUSA03173 + 100% guest clock + cycle skip off. The same-APK
control kept code caching disabled too, so calibration-specific cache policy did
not bias the comparison.

Correctness passed: four captures 2.5 seconds apart retained stable geometry and
one process, while the HUD reported 39--41 FPS. Activation logs proved
`eligible=1`, `enabled=1`, and `calibrated=1`.

The matched A1/B result rejected the candidate:

- A1 inline: SF 40.492 FPS, present 39.537/s, draw 18,073.166/s;
- B full typed thunk: SF 41.473 FPS, present 40.452/s, draw 18,367.702/s;
- both: policy0 2.3616 GHz, policy6 2.4384 GHz.

A2 was not admitted for attribution because DVFS fell to 1.9968/2.112 GHz; its
37.272 SF FPS is recorded only as an invalid thermal/frequency sample. The JIT
inline implementation and transport flags were removed, the accepted typed-time
common-frame and terminal dynamic-spill optimizations were retained, and the
stable APK was rebuilt, attested, and reinstalled. Final APK SHA-256 is
`33474750dce9af89eab2b9e174e91aa7b740d681305cbd4fa9ca8e8dca4b3041`.
Preferences remain byte-identical at
`ebe955d64cccf95ea864802e1dce9c48c7a58cdb26e7997f34060f6794c9d83c`;
the app is stopped and the rejected control marker is absent.

Evidence:

- `artifacts/dualgoal/vivo-typed-time-inline-series-20260829-1951`
- `artifacts/dualgoal/vivo-typed-time-inline-a1-20260829-1952`
- `artifacts/dualgoal/vivo-typed-time-inline-b-20260829-1953`
- `artifacts/dualgoal/vivo-typed-time-inline-a2-20260829-1955`

## Accepted: shared HLE-dispatch reader state lock

The FEX HLE enter/leave path previously serialized every reader operation on
one process-wide exclusive mutex, including the common case with no pending
guest signal. The A840 + CUSA03173 route now uses a shared reader lock for
thread-state publication and polling. Signal insertion, removal, delivery, and
draining remain exclusive, so state mutation and suspend/resume ordering are
unchanged. A750 and all ineligible routes retain the legacy exclusive path.

The same APK exposes the marker
`run-preserve-fex-hle-dispatch-exclusive-lock` as the control. A stable
four-frame temporal series passed before attribution: gameplay time advanced,
geometry remained correct, and no delayed corruption, stale frame, freeze, or
two-resolution flicker appeared.

The valid same-binary order was candidate A1, control B, candidate A2. Every
10-second window matched policy0/policy6 medians at `2.3616/2.4384 GHz`, used
instrumentation Off and upscaler Off, and stayed in a narrow shell-temperature
range:

```text
              SF FPS   present/s   draw/s       cores    shell C
candidate A1  41.024   40.011      18,054.093   3.6191   39.46-39.62
control B     40.463   39.598      17,891.107   3.5864   39.66-39.94
candidate A2  41.138   40.101      18,067.814   3.5636   38.11-38.56
```

The candidate mean is SF `41.081`, presents `40.056/s`, and draws
`18,060.954/s`: `+1.53%`, `+1.16%`, and `+0.95%` respectively versus the
control. The result is retained as a small repeatable removal of self-created
HLE synchronization overhead; it is not a claim that the Vivo target has
reached 60 FPS.

The accepted marker state is absent and the app is force-stopped. The installed
and local APK SHA-256 is
`1656c69d87054ef51fee3cdfe29c0625c2da67ddaac651282f9419db3abc490a`.
Preferences remain byte-identical at
`ebe955d64cccf95ea864802e1dce9c48c7a58cdb26e7997f34060f6794c9d83c`.

Evidence:

- `artifacts/dualgoal/vivo-hle-shared-lock-series-20260829-2010`
- `artifacts/dualgoal/vivo-hle-shared-lock-a1-20260829-2011`
- `artifacts/dualgoal/vivo-hle-shared-lock-b-20260829-2013`
- `artifacts/dualgoal/vivo-hle-shared-lock-a2-20260830-0220`

## Provisionally retained: pending HLE-dispatch membership mirror (S1)

S1 keeps an exact membership mirror for the pending HLE-dispatch set so the
common reader path does not perform `unordered_set::contains` while holding the
shared lock. Signal insertion/removal/drain still update the authoritative set
and mirror under the exclusive writer lock. The existing A840 + CUSA03173
predicate remains unchanged, and A750 does not enter this route.

Charging prevented a valid full-clock repeat. The closest matched B/A2 pair did
hold identical throttled policy clocks at `1.7856/1.9776 GHz` and preserved
geometry and liveness:

```text
         SF FPS   present/s   draw/s       cores
control  33.849   33.171      15,003.026   3.6453
S1       35.530   34.896      15,903.786   3.7696
delta    +4.97%   +5.20%      +6.00%       +2.51% draw/core
```

The strict HLE-workload attribution gate differed by about 4%, so this is not
labelled a fully attributed benchmark. It is nevertheless retained
provisionally because the user explicitly values the repeatable 5--6% raw
direction. A later full-clock same-workload repeat is for narrowing the causal
magnitude, not a prerequisite for keeping S1. The control marker
`run-preserve-fex-hle-pending-set-lookup` is absent, preferences remain
byte-identical at
`ebe955d64cccf95ea864802e1dce9c48c7a58cdb26e7997f34060f6794c9d83c`,
and the app was force-stopped.

Evidence:

- `artifacts/dualgoal/vivo-hle-pending-mirror-b-series-20260830-0605`
- `artifacts/dualgoal/vivo-hle-pending-mirror-b-20260830-0606`
- `artifacts/dualgoal/vivo-hle-pending-mirror-b2-20260830-0613`
- `artifacts/dualgoal/vivo-hle-pending-mirror-a2-series-20260830-0621`
- `artifacts/dualgoal/vivo-hle-pending-mirror-a2-20260830-0622`

## Rejected: exact-demand readback window

The candidate kept GPU-to-CPU demand reads of at most 4 KiB at the exact
requested range instead of expanding them to the established bounded readback
window. It was independently marker-gated by
`run-enable-exact-demand-readback-window`; S1 stayed enabled in both phases.
Startup witnesses proved the intended one-factor route, and both four-frame
series preserved geometry, liveness, and advancing gameplay time.

The same-cap comparison was decisively negative despite the candidate being
slightly cooler. Both phases held policy0/policy6 at `1.7856/1.9776 GHz`, had
zero JIT compilations and zero major faults, and matched HLE work within 0.42%:

```text
           SF FPS   present/s   draw/s       cores    draw/core
control    35.859   35.076      15,922.853   3.7728   4,220.434
candidate  34.219   33.518      15,077.185   3.6601   4,119.337
delta      -4.57%   -4.44%      -5.31%                -2.40%
```

The exact-window marker was removed. S1 remains enabled by the absence of its
control marker, the app is force-stopped, `stayon=0`, and preferences remain
byte-identical at
`ebe955d64cccf95ea864802e1dce9c48c7a58cdb26e7997f34060f6794c9d83c`.

Evidence:

- `artifacts/dualgoal/vivo-exact-demand-control-a-series-20260830-0641`
- `artifacts/dualgoal/vivo-exact-demand-control-a-20260830-0642`
- `artifacts/dualgoal/vivo-exact-demand-candidate-b-series-20260830-0648`
- `artifacts/dualgoal/vivo-exact-demand-candidate-b-20260830-0649`

## Accepted: exact SM8850 direct-leaf HLE dispatch

The adapter's existing direct-leaf HLE callback was available only behind the
exact SM8650 host gate even though its dispatch path is independently
fail-closed on immutable target metadata, executable state, integer-only ABI,
no stack ABI, no thread exit, no context-replacing fiber, and a non-null guest
thread. A separate exact `ro.soc.model == SM8850` opt-in route now exposes the
same callback on the Vivo A840 target. Other SoCs and the SM8650 marker remain
unchanged. The Vivo marker is
`run-enable-sm8850-fex-direct-leaf-hle`.

The installed APK logged
`[EXECUTOR_FEX_DIRECT_LEAF_GATE] enabled=1 sm8650=0 sm8850=1 metadataFailClosed=1`.
An eight-frame temporal series passed with a stable PID, advancing game time,
correct geometry, and no stale frame, freeze, or two-resolution flicker.

The initial candidate ran at lower policy medians and was not used as the main
attribution pair. The matched full-clock cold candidate and reverse control
both held policy0/policy6 medians at `2.3616/2.4384 GHz`:

```text
                 SF FPS   present/s   draw/s       cores    shell C
candidate        45.247   43.947      19,657.510   3.5775   38.60-39.32
reverse control  40.488   39.606      17,836.421   3.5931   39.38-39.83
delta            +11.75%  +10.96%     +10.21%      -0.43%
```

Both phases had zero major faults, instrumentation Off, upscaler Off, and the
same `960x544 -> 960x544` render/output path. The candidate is accepted and its
marker is present. S1 remains enabled by absence of its preserve marker, the
exact-demand readback candidate remains disabled, the app is force-stopped,
`stayon=0`, and preferences remain byte-identical at
`ebe955d64cccf95ea864802e1dce9c48c7a58cdb26e7997f34060f6794c9d83c`.
The installed and local APK SHA-256 is
`6cc6fc3670d3b023d7c43e62e44826e12ee767367acb1172b5f5cba952bc080b`.

Evidence:

- `artifacts/dualgoal/vivo-sm8850-direct-leaf-control-20260830-0730`
- `artifacts/dualgoal/vivo-sm8850-direct-leaf-candidate-series-20260830-0733`
- `artifacts/dualgoal/vivo-sm8850-direct-leaf-candidate-20260830-0733`
- `artifacts/dualgoal/vivo-sm8850-direct-leaf-candidate-cold-20260830-0740`
- `artifacts/dualgoal/vivo-sm8850-direct-leaf-reverse-control-20260830-0745`

## Accepted: zero deferred-promotion telemetry RMW

The FEX adapter no longer executes the shared `fetch_add(0)` telemetry RMW
when a boundary has no deferred promotions. Non-zero promotion publication and
its counter remain exact. The same-binary control marker is
`run-preserve-fex-zero-deferred-promotion-rmw`; absence selects the candidate.

At identical policy0/policy6 medians of `2.3616/2.4384 GHz`, candidate B versus
the mean of the forward and reverse controls was positive but small:

```text
                 SF FPS   present/s   draw/s
control mean     45.206   43.983      19,854.311
candidate        45.690   44.282      19,977.743
delta            +1.07%   +0.68%      +0.62%
```

This does not independently reach the 5--6% Vivo significance target, but it
has no measured regression. It is retained because the identical implementation
subsequently crossed the repeatable threshold on SM8650, while the operation it
removes is semantically inert. The preserve marker is absent.

Evidence:

- `artifacts/dualgoal/vivo-zero-rmw-control-a-20260830-0943`
- `artifacts/dualgoal/vivo-zero-rmw-candidate-b-20260830-0948`
- `artifacts/dualgoal/vivo-zero-rmw-reverse-control-a2-20260830-0952`

## Rejected on SM8850: HLE bulk-write TLS grouping

The candidate grouped the bulk-write begin/end/consumed fields and FEX lease
phase in one emulated-TLS object, then resolved it once in each hot
`memcpy`/`memmove`/`memset` HLE boundary. Object disassembly proves one
`__emutls_get_address` on the candidate path versus four in legacy Begin and
three in legacy End. The marker
`run-preserve-hle-bulk-write-tls-lookups` restores the legacy path.

The first candidate window was invalid for attribution because Vivo applied a
lower DVFS cap and it incurred 82 major faults. The immediate warm candidate
repeat and reverse control were clean enough and held identical
policy0/policy6 medians of `2.3616/2.4384 GHz`:

```text
                 SF FPS   present/s   draw/s       cores   major faults
reverse control  45.825   44.566      20,109.430   3.5904  0
candidate warm   45.767   44.490      20,069.820   3.5802  7
delta            -0.13%   -0.17%      -0.20%      -0.28%
```

Disassembly then exposed a second revision issue: the phase helper allowed
Clang to speculate both the candidate and legacy emulated-TLS address
resolvers. Revision 2 uses an explicit non-inlined resolver, a real `cbz`, and
a compiler barrier. Its object code proves candidate and legacy phase paths
each execute only their own single TLS resolver. A new same-binary pair again
held identical `2.3616/2.4384 GHz` clocks and zero major faults:

```text
                 SF FPS   present/s   draw/s       cores
candidate r2     45.882   44.618      20,122.796   3.5391
control r2       45.864   44.704      20,002.699   3.5629
delta            +0.04%   -0.19%      +0.60%      -0.67%
```

Both revisions are neutral rather than an actionable 5--6% win, so SM8850 keeps the
legacy path. The preserve marker is present, direct-leaf and S1 remain enabled,
zero-RMW remains enabled, the app is force-stopped, `stayon=0`, and preferences
remain byte-identical at
`ebe955d64cccf95ea864802e1dce9c48c7a58cdb26e7997f34060f6794c9d83c`.
The installed APK SHA-256 is
`0763530e1e7808903897dd182412104da6c02d11e1643ccb978ac213b17a65b5`.

Evidence:

- `artifacts/dualgoal/vivo-hle-tls-control-a1-20260830`
- `artifacts/dualgoal/vivo-hle-tls-candidate-b1-20260830`
- `artifacts/dualgoal/vivo-hle-tls-reverse-control-a2-20260830`
- `artifacts/dualgoal/vivo-hle-tls-candidate-b2-20260830`
- `artifacts/dualgoal/vivo-hle-tls-r2-candidate-b1-20260830`
- `artifacts/dualgoal/vivo-hle-tls-r2-control-a1-20260830`
- `artifacts/dualgoal/hle-tls-page-manager-disasm-nonspeculative-fixed.txt`

## 2026-08-30 current-APK CPU profile recovery and unsafe-memory A/B/A

APK `1436577abb2467a83aca989f2caef04cfea451fbd17516a70a75c9a9024654e3`
adds independent default-Off upscaler HUD visibility and a reversible shared
CPU profile for the new "Activate current CPU settings for all games" control.
The installation retained application data. Before testing, the altered CPU
preferences were backed up as `lsx4_settings.pre-20260830-cpu-recovery.xml`
and the proven baseline was restored byte-for-byte at
`ebe955d64cccf95ea864802e1dce9c48c7a58cdb26e7997f34060f6794c9d83c`.

The first two current-APK baseline windows held identical policy0/policy6
medians of `2.3616/2.4384 GHz`:

```text
                   SF FPS   present/s   draw/s       cores
baseline A1        42.951   41.827      24,666.304   3.7076
baseline A2        42.660   41.694      24,558.198   3.7042
baseline mean      42.806   41.761      24,612.251   3.7059
```

The one remaining high-risk settings candidate with a plausible hot-path
mechanism changed only `Fast/Host mapped` to `Unsafe/Host mapped unsafe` while
keeping memory exceptions On and invalid-access replay Off. It reached correct
gameplay, but at the same frequency medians measured SF `42.512`, presents
`41.256/s`, draws `24,334.672/s`, and `3.7492` cores. Against the forward
baseline mean this is `-0.69%`, `-1.21%`, `-1.13%`, and `+1.17%` CPU use. Its
over-40-ms frame fraction also rose to `2.09%` from `1.55%/1.04%`.

The immediate reverse baseline, at the same clocks but a warmer `39.50 ->
39.98 C`, measured SF `42.459`, presents `41.317/s`, draws `24,350.039/s`, and
`3.7258` cores. It therefore confirms no candidate throughput advantage while
retaining lower CPU use and lower tail risk. Unsafe memory is rejected and the
exact baseline preference hash is restored. Previously rejected Aggressive HLE
(`-8%`), Aggressive timers (neutral), FP Approximate (`-4.3%`), Mega blocks
(startup failure), four-worker admission (startup deadlock), and Performance
affinity (`28.1` versus `34.0` FPS) were not repeated.

Final effective Bloodborne CPU profile remains FEX, Fast/Host-mapped memory,
block linking On, cache Off, Safe replacements, 100% clock/no cycle skip,
Safe 256-block geometry, Accurate FP, Compatible atomics, cache/DMA accuracy
Off, memory exceptions On, OS scheduler, six guest workers, dual CPU/GPU,
Adaptive synchronization, Prefer HLE, and Fast HLE timers. Bloodborne remains
running after the reverse-control window.

Evidence:

- `artifacts/dualgoal/vivo-cpu-baseline-restored-20260830`
- `artifacts/dualgoal/vivo-cpu-baseline-repeat-20260830`
- `artifacts/dualgoal/vivo-cpu-unsafe-current-20260830`
- `artifacts/dualgoal/vivo-cpu-baseline-reverse-control-20260830`

## 2026-08-31 five-title compatibility control and AIO candidate

The user confirmed that the historical control APK reaches playable state in
Inside, Cyber Shadow, Just Shapes & Beats, Rain World, and The Binding of
Isaac: Rebirth. It is retained at
`artifacts/vivo-five-games-20260831/apk-backups/vivo-working-control-root9f-oldfex-20260831.apk`
(30,241,502 bytes, SHA-256
`c52d749894a9a6e0cbc6e44a2b2fd096c67655277fe06afbb2f8ac9541df9caf`).
This control remains the rollback artifact; the current settings and title
data must be backed up before replacing it on Vivo.

The common regression boundary introduced a dedicated AIO worker with several
cross-title lifecycle faults: terminal submit IDs were never recycled without
an explicit delete, a terminal ID could become visible before its guest result
record, a failed batch left later results in `PROCESSING`, cancellation could
reuse a slot before its worker retired, and queued operations retained only a
numeric fd that could be closed and reused for a different file.

The candidate fixes only that kernel AIO/handle lifetime layer. It preserves
the CPU settings schema and routing, all-games overrides and exceptions,
per-title profiles, FEX geometry, renderer paths, and Snapdragon capability
gates. `Separate I/O thread` still selects real worker versus inline dispatch;
both paths now share the same request lifecycle. The handle table holds an AIO
lease until the accepted operation finishes, so close cannot detach or recycle
the fd prematurely. Worker drain precedes a new AIO session, result fields are
published before terminal ID state, inactive terminal IDs are reclaimed in one
cyclic pass, and every batch tail becomes terminal on failure.

Static gates are green: the ARM64 Android objects and final executor link, 8/8
AIO lifecycle contracts, and 31/31 combined AIO/CPU/all-games contracts. The
signed debug APK is staged outside the workspace as
`C:\Users\Litesav\AppData\Local\Temp\lsx4-vivo-five-games-aiofix-20260831.apk`
(64,808,356 bytes, SHA-256
`34cad78d45f041ed67d0bf3d5c4fb8610ad033db2e4af6a81d096da6880e74ee`).
Its packaged executor is 65,369,384 bytes with SHA-256
`c0851f9495e6599c69d04c7569dc2fdea1f7244ad9e94fef7cc2e894e680e153`.

This is a pending candidate, not an accepted result. It must reach actual
gameplay in all five titles on Vivo with temporal screenshots before it may
replace the control or be used for performance work. After that compatibility
gate, Bloodborne must recover the established CPU profile and approximately
42.8 SurfaceFlinger FPS / 41.8 presents per second before any new optimization
axis is evaluated.

## 2026-08-31 five-title compatibility gate passed

The candidate now reaches actual gameplay, rather than only boot or menu, in
all five Vivo control titles:

- Inside: `artifacts/vivo-five-games-20260831/inside-historical-fex-after-150s.png`
- Cyber Shadow: `artifacts/vivo-five-games-20260831/cyber-shadow-historical-fex-series-01`
- Just Shapes & Beats: `artifacts/vivo-five-games-20260831/just-shapes-gameplay-proof-*`
- Rain World: `artifacts/vivo-five-games-20260831/rain-world-gameplay-proof-02`
- The Binding of Isaac: Rebirth:
  `artifacts/vivo-five-games-20260831/isaac-gameplay-proof-final`

The remaining Isaac barriers were not title-specific bypasses. Nested FEX
callbacks use the lower half of a leased HLE frame-pool slot as real SysV
stack storage. The pool is a host-owned mutable mapping rather than a PS4 VMA.
It is now registered as exact mutable sideband only: it is not published as a
general guest mapping and is not added to GC roots. Strict non-reentrant atomic
helpers can lease only a fully contained span. Typed HLE arguments beyond the
sixth GPR and AeroLib libc string readers can read only a fully contained
registered span. This fixed both the seventh argument to
`sceVideoOutSetBufferAttribute` and the stack-resident path strings used while
loading Isaac's `rebirth.prx`.

The earlier AIO lifetime corrections, 32-worker title overrides, and Mono
condition wake corrections remain present. The final focused root suite passes
14/14 contracts and the rebuilt FEX adapter passes all 15 host tests. The APK
installed through the mandatory Vivo installer has SHA-256
`172291767bf8bf02301d0f93682248b07804422d6913bce56da9c28b1cee2c81`.

Bloodborne was then relaunched without changing its established per-title CPU
profile. It reached the title screen and continued into the saved playable
scene with correct geometry and no rejected guest signal, mapped-entry failure,
or invalid libc pointer. The temporal proof is
`artifacts/vivo-five-games-20260831/bloodborne-gameplay-proof-after-isaac-fix`.
The scene visibly presents at 30 FPS with the 60 FPS++ patch enabled; this is a
separate performance limit, not a launch or compatibility regression. No POCO
work is resumed after this gate per the user's instruction.

## 2026-08-31 Bloodborne regression after the five-title gate: root cause and fix

The five compatibility fixes were not themselves the first 15-FPS regression
boundary. A retained-data APK A/B used the same Vivo A840, settings XML, Bloodborne
save and gameplay scene:

```text
Aug-30 artifact  SHA 750A91...   45 FPS
pre-five-fix APK SHA DA0B8...    28--31 FPS
settings XML     SHA 8F8F9235... identical
policy0/policy6                  2.3616/2.4384 GHz
```

The faster old artifact was not colder. The scene, resolution, 60 FPS++ patch,
application data and device were retained, excluding thermal, stale processes,
save/camera and final AIO/Mono/Isaac changes as the primary explanation.

The causal effective-log delta was exact:

```text
45 FPS:
[EXECUTOR_FEX_EXACT_MULTIBLOCK] requested=1 configured=1
[EXECUTOR_FEX_MAXINST_GEOMETRY] requested256=1 exactMultiblock=1 configured256=1

28--31 FPS:
[EXECUTOR_FEX_EXACT_MULTIBLOCK] requested=0 configured=0
[EXECUTOR_FEX_MAXINST_GEOMETRY] requested256=1 exactMultiblock=0 configured256=0
```

The new universal profile was enabled with named Balanced preset and a materialized
`cpu.all_games.block_linking=0`. That scope superseded the individual game scope.
GameDB restored Bloodborne Safe geometry but did not restore its paired exact
block-linking constraint, so requested 256 geometry could not become effective.

The resolver now applies hard GameDB constraints after a named preset has
materialized all of its fields. Custom remains deliberately different: GameDB is
its base and the user's explicit Custom fields overlay it afterward. Bloodborne
GameDB now pins `block_linking=1` beside Safe 256 geometry. This preserves the
ordinary all-games Balanced workflow without making Custom controls fictitious.

The focused Android CPU and all-games suites pass 23/23. The rebuilt and installed
APK has SHA-256
`DA153629CEBF88B2D6E48D5DAC88723E041A3AD1DDB6248416FA0ED5A511155C`.
On the same save/scene its log reports exact multiblock effective and
`effective256=1 maxInst=256`; gameplay recovered to 41 FPS with correct geometry.
After the temporary preserve marker was removed, a clean final relaunch reached
46 FPS in the same scene. Device-side `sha256sum` of the installed `base.apk`
matched `DA153629CEBF88B2D6E48D5DAC88723E041A3AD1DDB6248416FA0ED5A511155C`.

A final same-binary discriminator disabled only the current Snapdragon strong
bundle through `run-preserve-bloodborne-snapdragon-strong-bundle`. FPS fell from
41 to 37, so the bundle contributes about +4 FPS and is not the remaining source
of loss relative to the historical 45-FPS sample. The preserve marker was removed;
the accepted default remains enabled.

This incident is now a mandatory two-axis regression gate. The complete causal
record and prevention rules are in
`docs/PLAYABLE_PERFORMANCE_REGRESSION_PREVENTION_2026-08-31.md`. Any future common
CPU/JIT/FEX/HLE/worker/memory change must pass real gameplay in the affected golden
titles and the matched Bloodborne/Vivo performance anchor on the same final APK.

## 2026-08-31 PS5 Current-JIT regression: stale resident GPR after HLE

Dreaming Sarah (`PPSA02929`) and Void Terrarium++ (`PPSA03061`) had also regressed.
Dreaming Sarah crashed about 1.6 seconds after `GameActivity` with `SIGSEGV`, fault
address `0x38`, while the historical Aug-7 PS5 runtime
`EDF961487EB9A335E7F04559EA84B673075664E8B21566176AB49D0BC30D5CCB`
remained upright and live. The Aug-10 `r270` runtime
`B6E7AD66D8560C3338D62F29224A983DCBB0FE4A20DD40F1445291CD000CE76D`
already reproduced the crash, giving an exact regression interval.

Clean persistent-cache and identical managed CPU options did not change the result.
Forcing helper execution on the current runtime rendered the Dreaming Sarah menu,
so renderer, loader, HLE coverage and CPU settings were excluded. JIT perf-map tied
the crash to guest block `0x9202e15d5`; its stale guest pointer `0x20` was consumed
by a store at `+0x18`, producing host fault address `0x38`.

The PS5 Current HLE bridge publishes return registers through `LsxMachineImage`, but
the shared lowering core could retain pre-HLE x23--x30 in its resident GPR cache.
The repaired PS5 build keeps GPR state memory-authoritative under
`LSX4_PS5_DESKTOP_PATH` until an explicit invalidation handshake exists. This guard
does not compile into Android PS4/FEX, so Bloodborne's resident-register fast path is
unchanged. APK packaging freshness now tracks the shared retiring JIT source for PS5.

Final PS5 runtime SHA-256 is
`11481F724FD32923D7BE01F832D4D97A96DE3CEAEFBE467F853904AE2F0A1072`.
The officially installed APK/base.apk SHA-256 is
`1CD0D4AFBB3D49F3EA7D3A75E640E53966109D44C2DDC74DE63BB495610ADF6E`.
Without helper/perf-map markers, final verification showed:

```text
Dreaming Sarah:    decoded=16384 native=16381 bridges=2 rejected=0, present 60/s
Void Terrarium++:  decoded=16384 native=16379 bridges=4 rejected=0, present 60/s
```

Both reached live upright title menus with growing counters and no fatal signal.
Future common JIT/HLE changes require separate PS4/FEX and PS5/Current gates; success
on Bloodborne cannot stand in for PS5 post-HLE coherency.

## 2026-08-31 Inside temporal flicker: cross-title marker leakage

Inside (`CUSA02754`) exposed a renderer regression that individual screenshots could
not prove or disprove. The first 30-second candidate series contained 9 event groups
and 20 corrupted frames out of 248, including full-white and raw/noisy frames. A clean
same-binary preserve A/B produced 0 events in 251 frames, which excluded resolution,
DRS and temperature as the causal axis.

The device still contained 17 global `run-enable-*` files from Bloodborne profiling.
They silently enabled risky shader payload, SRT/TLS and PageManager routes for Inside.
Those routes now use `Common::TitleScopedOptimizationMarkerEnabled` and read only
`files/lsx4-home/title-markers/<TITLE_ID>/<marker>`. The 17 Vivo markers were migrated
to `title-markers/CUSA03173`; they were not deleted, so the accepted Bloodborne bundle
is retained without leaking into other games.

Final APK and installed `base.apk` SHA-256:
`31D6DAE78BDDE02ADECF2AE69A0C09510ECBC3FE061D299D777BAB0868856FAE`.
The packaged PS4 runtime is
`3F5126590117280FB862F1447416C2EBE64A7AEEF46B778F6C5F4E74FFDF0228`;
the accepted PS5 runtime remains
`11481F724FD32923D7BE01F832D4D97A96DE3CEAEFBE467F853904AE2F0A1072`.
The final no-preserve Inside series contained 255 frames, 0 events, max component
0.0096%.

The existing managed ARM GPU fast path was then enabled only through
`game.profile.CUSA02754.managed_arm_gpu_fast_path=true`, while the global value stayed
false. The matched opening scene increased from the historical 6 FPS to 8 FPS. A
second 24-second temporal series passed with 0 events and max component 0.0207%, so
the per-game performance gain did not reintroduce white/raw/stale frames.

Full causal record and mandatory marker-scope/temporal rules are in
`docs/PLAYABLE_PERFORMANCE_REGRESSION_PREVENTION_2026-08-31.md`.

## 2026-08-31 Inside 30+ closure: narrow mapped-range concurrency, common-lock rollback

The accepted closure deliberately does not add a `CUSA02754` code path. A generic
`Common::RecursiveSharedFirstMutex` is used only by the Vulkan rasterizer's
`mapped_ranges_mutex`: recursive read ownership is local to that mutex, read-only
queries use shared ownership, and mapped-range mutation remains exclusive.

An earlier attempt to replace the process-wide recursive-lock TLS unordered map with
an inline registry was rejected. It caused an immediate Isaac `SIGSEGV` through
`internal_strcpy_s` / `ExecutorFexDispatchHle`; HLE TLS and three HLE lock-preserve A/Bs
did not change the failure. Reverting only `recursive_lock.cpp` restored `rebirth.prx`
and savedata startup on the same executor. The common registry therefore stays on its
legacy implementation. Do not revive the inline registry without a full cross-title
mutex/HLE gameplay matrix.

Installed final APK SHA-256:
`C9717C50A9CDAD7280AEFDEAB79BF859B47F820C4552202B0B10B60E44C75F66`.
Inside opening gameplay proof:

```text
window:             62.20 s
runtime present:    34.167/s
SurfaceFlinger:     34.590 FPS
SF p50/p95/p99:     33 / 33 / 50 ms
SF >40 ms:          1.1147%
process CPU:        2.1248 cores
tz_shell:           37.42 -> 37.90 C
temporal:           30 unique / 30, adjacent duplicates 0, corruption events 0
max white/near:     0.02499% / 0.03200%
```

Artifacts:

- `artifacts/vivo/inside-c971-final-live60/summary.json`;
- `artifacts/vivo/inside-c971-final-temporal30`;
- `artifacts/vivo/inside-c971-final-temporal30-analysis`.

This closes the 30 FPS target without resolution reduction, upscaling, a new title
marker, or title hardcode. No further speculative optimization was layered on after
the gate passed.

The same APK then passed the reverse Bloodborne anchor. The adaptive route reached
Hunter's Dream gameplay in `49.06 s` with `23849 draw/s` and `4.00` process cores;
the retained gameplay frame reported `38 FPS` and `23177 draw/s`, with valid geometry
and no fatal signal. The stored post-Isaac-fix golden in the same area reported
`34 FPS` and about `16.2k draw/s`, so the accepted mapped-range lock did not trade
Inside throughput for Bloodborne performance.
