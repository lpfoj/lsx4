# POCO F7 Pro Bloodborne route

The canonical gate policy is defined by `scripts/preflight-vivo-performance-test.ps1` and
`PERFORMANCE_TEST_PREFLIGHT.md`; POCO runs use the matching
`scripts/preflight-poco-f7-pro-performance-test.ps1`. Any skipped or failed gate is invalid.

The unattended Snapdragon 8 Gen 3 route is isolated in
`scripts/start-bloodborne-gameplay-poco-f7-pro.ps1`.  Do not use or modify the
Vivo route for POCO runs.

The POCO script rejects every device except model `24117RK2CG` with SoC
`SM8650`.  Android reports its physical display as portrait `1440x3200`, while
Bloodborne receives landscape input.  The verified Cross point is therefore
`2950,1220`; the Vivo point remains owned by the unchanged Vivo script.
The route sends no game input while the captured surface is still portrait, so
HyperOS's temporary bootstrap spinner cannot be mistaken for a Bloodborne
prompt.

Run the current 8gen3 quality/performance candidate with:

```powershell
pwsh -NoProfile -File scripts/start-bloodborne-gameplay-poco-f7-pro.ps1 `
  -AdbTarget 65d16af6 `
  -TimeoutSeconds 240 `
  -RenderResolutionMode 5 `
  -UpscaledOutputResolutionMode 2 `
  -UpscalerMode 6 `
  -FsrMode on
```

Resolution mode `5` is `640x360`, output mode `2` is `1280x720`, and upscaler
mode `6` is SGSR1.  Launch extras are per-run and do not rewrite the user's
stored `res_mode` preference.

`-AllowThrottledFunctionalRun` exists only for driver, crash, memory and
geometry diagnosis while HyperOS is holding a post-load CPU-frequency clamp.
It still runs cleanup-only preflight and emits an explicit warning.  FPS from
that launch is not benchmark evidence and cannot satisfy the 30–35 FPS gate;
the final proof must use the default full preflight path above.

The route first validates the exact POCO identity and runs its own
`scripts/preflight-poco-f7-pro-performance-test.ps1`.  That gate accepts only
ADB serial `65d16af6`, model `24117RK2CG`, SoC `SM8650`, and checks POCO's
policy0/policy7 CPU limits plus the same thermal, memory, idle-CPU and stale
diagnostic-process invariants.  Neither POCO script calls or modifies
`start-bloodborne-gameplay-vivo.ps1` or
`preflight-vivo-performance-test.ps1`.  The route dismisses startup prompts,
selects Play Offline and Continue through screenshot classification, and exits
only after a fresh game PID passes the gameplay draw-rate and CPU-workload
gates.  It does not clear app data, games, saves, shader caches, imported
Turnip drivers, or FEX state.

The warm launch is required because HyperOS applies LSX4's registered Game
Turbo/Wild Boost CPU policy only after LSX4 becomes foreground. On the test
device, the idle policy7 cap is `1593600` KHz and changed to `3244800` KHz
during this UI-only launch while thermal status stayed normal. The route first
completes strict idle preflight and verified Turnip selection, force-stops the
last control process, then starts `MainActivity` with runtime autoload/autoinit
disabled. It starts no guest during this step and requires policy7
`scaling_max_freq` to reach at least `2937600` KHz. The automation launch is
then delivered to that existing `singleTop` activity without another
force-stop, so the recognized foreground session remains continuous through
the handoff to `GameActivity`. The launcher then performs its established
finish plus delayed process retirement; keeping that extra process alive is
neither required for Game Turbo nor accepted. After the fresh game process reaches the
gameplay draw/workload gate, the route independently requires the same ceiling
again. A functional launch can no longer be mistaken for a valid performance
launch merely because a disconnected warm-up was boosted.

Immediately before launch, the route selects the already imported and verified
Turnip package by its complete SHA-256 (default
`b0c0ea26586d63d708df19ee203df260881c00e4135c0d58e119d2660cfbe47e`).
This prevents the three-failed-launch safety recovery from silently turning a
Turnip measurement into a Qualcomm/system-driver measurement.  Selection uses
the same `VulkanDriverManager.selectDriver` validation path as the settings UI,
through `VulkanDriverControlReceiver`.  That receiver requires Android's
signature-level `DUMP` permission, so it is callable by the attached ADB shell
but not by ordinary applications.  The route waits for a token-bound result,
verifies the exact persisted selection, and aborts instead of falling back.

Before the preflight, the POCO-only route force-stops a documented allowlist of
resident user applications (browser, media, social, Play Store/Search and the
previously used external launcher).  It deliberately leaves HyperOS Home,
POCO content panels and Bachata running so cleanup cannot reveal, stop or
disturb the Bachata task underneath Home.  This is a
process-only cleanup for a repeatable memory baseline: it never calls
`pm clear`, never disables or uninstalls packages, and Android can start those
applications normally afterward.  The unchanged Vivo script does not contain
or call this cleanup.

The POCO route does **not** put the display to sleep.  HyperOS has a secure
keyguard on this device, and sleeping during cleanup can leave `GameActivity`
reported as resumed while `NotificationShade` still owns focus.  LSX4 then
correctly keeps guest threads paused for `WINDOW_FOCUS_LOST`.  The route checks
that the keyguard is gone both before cleanup and immediately before launch;
it may dismiss an already trusted/non-secure keyguard, but it never changes or
bypasses a lock credential.  If interactive authentication is still required,
it aborts before LSX4 starts.  Global screen/lock settings remain unchanged.

## D19 no-log driver parity result (2026-08-29)

The D19 diagnostic driver SHA
`90a23dc8aa854c5c9bb1fe43531f81a27de7ee69cd5e4330f49ae7d6a6a2c176`
keeps D9's BO-table walk and formatting but removes its dynamic BO census
logcat writes.  Earlier D9/D19 comparisons were invalid: because D19 had a new
package SHA, `CapabilityProfile` classified it as `Unknown`, which also removed
D9's 32 MiB VMA blocks, sustained residency policy and several typed cache
routes.  The resulting LMK therefore did not isolate logging overhead.

D19 now has its own exact SHA identity and joins one centralized
`exact_mesa_turnip_a750_d9_lineage` policy with D9.  Renderer/cache consumers
use that resolved profile bit rather than comparing driver enums themselves.
A840, proprietary Qualcomm, Mali and unknown packages remain outside the
lineage.

Two strict-preflight Bloodborne runs then held correct geometry through the
gameplay capture series:

- a temporary 1024-submit timeline checkpoint peaked near 4.37 GiB PSS and
  settled near 4.22 GiB;
- the otherwise identical run without the checkpoint peaked near 4.64 GiB PSS
  and settled near 4.24 GiB.

The no-checkpoint control stayed alive and its memory footprint fell, so the
checkpoint hypothesis was rejected and all of that wait/marker code was
removed.  The retained change is only the exact D19 identity and D9-lineage
profile parity.  Evidence is under
`artifacts/dualgoal/poco-d19-lineage-checkpoint-series-20260829-2156` and
`artifacts/dualgoal/poco-d19-lineage-no-checkpoint-series-20260829-2200`.
No FPS conclusion is attached to these correctness/memory runs.  The device
was returned to the verified D9 selection afterward.

The completed same-APK performance check later rejected D19. At matched end
temperature and a slightly higher candidate policy7 median, D19 reached
`10.559` presents/s, `12,355.300` draws/s and `10.177` SF FPS versus D9's
`10.820`, `12,653.191` and `10.478`. D19 also recorded only seven forced
TextureCache/BufferCache residency passes per cache versus 70 for D9. Its lower
throughput therefore independently agrees with the protected-GC experiment:
the retained housekeeping is performance-positive in this workload. D9 was
restored; D19 is not a production-performance candidate. Evidence:
`artifacts/dualgoal/poco-d19-matrix-candidate-w1-20260830` and
`artifacts/dualgoal/poco-d20-matrix-d9-control-w1-20260830`.

## D20 no-BO-census candidate (2026-08-29)

D20 keeps the D9/D19 KGSL correctness changes but restores Mesa's ordinary
non-`TU_DEBUG(BOS)` allocation path: `device->bo_sizes` is not created, BO
allocation/free does not update a census table, and the periodic debug call
returns immediately.  Its exact stripped-library SHA is
`df53cdaefc249f142b3c35c8f4df696466d3191d9ca2deb60845210c67b0c68f`.

The app recognizes that SHA only on exact Mesa Turnip + Adreno 750 and assigns
the same typed D9-lineage memory/cache policy used by the valid D19 control.
Static contracts explicitly reject the package identity on Adreno 840, so the
Vivo route is unchanged.  Both Android native runtimes, the Vulkan capability
guard and the APK build pass.  The import archive is
`.codex-mesa-turnip-2621-diagnostic/output/lsx4-turnip-d20-no-bo-census.zip`.
Device correctness, memory and performance acceptance remain pending; D9 must
stay selected after any interrupted or inconclusive candidate run.

The first D20 device attempt on 2026-08-30 was inconclusive and is not an FPS
result.  The strict preflight passed at `quiet_therm=45.5 C`, the exact D20 SHA
was selected, and the continuous handoff reached the gameplay draw gate.  At
that point HyperOS exposed only a `2246400` KHz policy7 ceiling, below the
route's required `2937600` KHz live-game gate, so the route rejected the run
without measuring it.  LSX4 was force-stopped and the exact D9 SHA
`b0c0ea26586d63d708df19ee203df260881c00e4135c0d58e119d2660cfbe47e`
was restored and verified in `active.json`.  Evidence screenshot:
`.codex-bb-unexpected-20260830-024420.png`.

A same-device repeat also passed idle preflight and reached the Bloodborne
route after a `3052800` KHz warm ceiling, but the live ceiling was only
`2380800` KHz.  It was rejected before measurement as well; evidence is
`.codex-bb-unexpected-20260830-024829.png`.  The immediately requested D9
control was correctly stopped by preflight before launch because
`quiet_therm=48.8 C` and the idle policy0/policy7 ceilings had already fallen
to `1574400/1132800` KHz.  Consequently these runs neither prove a D20 driver
regression nor provide an FPS comparison.  D9 was again restored and LSX4 was
left stopped for cooldown.

The completed same-APK retry later rejected D20. D9 reached `10.820`
presents/s, `12,653.191` draws/s and `10.478` SF FPS. D20 reached only `10.226`,
`11,966.634` and `9.786` respectively despite a substantially higher policy7
median (`1.9392` versus `1.3632` GHz) and a nearly identical end temperature.
D20 also took `100.34` seconds to reach gameplay versus `71.57` for D9. The
candidate therefore lost roughly 5.5% presents and 5.4% SF under favourable
clocks; no reverse run was needed. Exact D9 was restored and verified in
`active.json` immediately afterward. Evidence:
`artifacts/dualgoal/poco-d20-matrix-d9-control-w1-20260830` and
`artifacts/dualgoal/poco-d20-matrix-d20-candidate-w1-20260830`.

## Continuous Game Turbo handoff (2026-08-30)

The earlier route warmed `MainActivity`, force-stopped the package, performed
preflight and driver selection, then cold-launched the game. That sequence
proved only a disconnected warm-up. The corrected POCO-only order is strict
idle preflight, exact Turnip selection, final force-stop, UI-only warm launch,
and automation delivered to the same `singleTop` activity with no intervening
stop. The warm and live-game gates both require policy7
`scaling_max_freq >= 2937600` KHz.

Keeping the paused launcher process alive after the handoff was tested and
rejected. That run reached policy7 `3052800` KHz but entered memory pressure at
about 4.47 GiB process PSS / 2.39 GiB graphics memory, then Turnip reported
`TU_KGSL_CONTEXT_RESET` and `VK_ERROR_DEVICE_LOST`; the loading screen remained
at 0 FPS. With the same APK and D9 driver but established launcher retirement,
the next run retained a `3244800` KHz live-game ceiling and fully entered
gameplay. The route measured 12.960 SF FPS / 13.150 presents/s at 960x544 to
1280x720 SGSR1 while `quiet_therm` rose from 51.83 to 52.63 C. This is a valid
hot baseline, not the 30 FPS target and not evidence for retaining the launcher.

Evidence:

- `.codex-bb-unexpected-20260830-023515.png`
- `artifacts/dualgoal/poco-continuous-handoff-d9-baseline-20260830-0250`

## Exact-D9 protected no-victim GC candidate (2026-08-30)

The same-APK B control proved a renderer-owned CPU tax rather than guessing at
another compiler flag.  At 224 presents the TextureCache and BufferCache each
performed 224 full residency scans, including 17 pressure-forced passes per
cache, while both reported zero evictions.  The valid 10-second control was
12.918 SF FPS / 13.019 presents/s / 15232.9 draws/s at 960x544 to 1280x720
SGSR1.  Evidence:
`artifacts/dualgoal/poco-protected-gc-b2-measure-20260830-055341`.

The A candidate is default-off and requires the opt-in marker
`run-sm8650-a750-d9-protected-gc-fast-noop-ab`.  `Instance` resolves that
marker once into a typed capability which additionally requires the exact
D9/D19/D20 A750 lineage; A840, proprietary drivers, unknown packages and
marker-free launches remain unchanged.  Texture and buffer collectors share
the same hard-protect predicates used immediately before their Free/Delete
paths.  They may skip only non-critical, non-emergency, non-pressure scans.
Every pressure epoch still forces the legacy path, BufferCache still flushes
pending transfers first, TextureCache still runs sampler GC, and every 128th
eligible call is a full audit scan which asserts zero evictions.  Per-cache
pass/skip/forced/audit counters are exported in the proof snapshot.

The candidate is rejected and must not be promoted.  Its first A window did
remove 209 of 213 TextureCache scans, but started about 1.9 C hotter than the B
control, so that comparison was inconclusive.  The repeated A2/B2 pair was
thermally matched more conservatively: A2 started at `quiet_therm=50.87 C`,
whereas the marker-free B2 control started slightly hotter at `51.34 C`.
Despite that disadvantage, B2 reached 12.657 SF FPS / 12.831 presents/s /
15049.2 draws/s, versus A2's 11.260 / 11.605 / 13624.1.  The control therefore
won by 12.4% SF, 10.6% presents and 10.5% draws.  Both runs reported zero
evictions and preserved the exact D9 driver, settings hash and live
`3244800` KHz performance ceiling.  A2 safely skipped 190 of 193 TextureCache
walks, proving that the mechanism worked but that the skipped housekeeping was
performance-positive in this workload.  BufferCache correctly skipped none:
the effective Bloodborne configuration has `directMemoryAccess=false`, so its
BDA identity-protection predicate is false and weakening that safety predicate
is forbidden.  The opt-in marker was removed and the app was force-stopped.
Evidence:
`artifacts/dualgoal/poco-protected-gc-a2-measure-20260830-055949` and
`artifacts/dualgoal/poco-protected-gc-b2matched-measure-20260830-060323`.

## Exact-D9 narrow image flags check (2026-08-30)

The existing marker `run-sm8650-a750-turnip-narrow-image-flags-ab` was checked
on the same APK and exact D9 driver. Runtime proved the intended one-factor
route with `requested=1 exactTurnipA750=1 driverBuild=4 effective=1`.

The useful OFF/ON pair was neutral: OFF reached 13.162 SF FPS, 13.191
presents/s and 15,474.955 draws/s; ON reached 12.993, 13.197 and 15,504.753.
That is +0.045% presents and +0.19% draws with -1.28% SF, below a material
promotion threshold. A later OFF2 window entered a partial low-workload stall
at 3.842 presents/s / 4,525 draws/s and is excluded. The pre-existing ON marker
was restored only to preserve the known stable device state, not as a new
performance claim. Evidence:

- `artifacts/dualgoal/poco-narrow-image-flags-off-measure-20260830-061004`
- `artifacts/dualgoal/poco-narrow-image-flags-on-a2-measure-20260830-061316`
- excluded: `artifacts/dualgoal/poco-narrow-image-flags-off2-measure-20260830-061608`

## Rejected: A750 FEX HLE shared-reader lock extension (2026-08-30)

The A840 shared-reader HLE lock was temporarily exposed behind an exact
A750+CUSA opt-in marker, while the A840 default and S1 pending mirror remained
unchanged. Startup logs proved `eligible=1 shared=1 a840Default=0
a750Requested=1` and `mirror=0`.

The marker-free control reached 13.284 SF FPS, 13.390 presents/s and
15,724.528 draws/s. The first candidate was already slower at 12.047, 12.266
and 14,399.097 under lower clocks. A cooler retry entered a partial stall at
11.673 SF FPS / 7.872 presents/s / 9,239.732 draws/s. There is no evidence that
the A750 workload benefits from this lock policy. The marker was removed, the
A750 source extension was reverted, preferences retained SHA-256
`f8d07993cacb02824b49c49ba13229ab9707ecb14762a7197763e1b41b1e092f`,
and the app was force-stopped.

## Rejected on A750: ProgramHot hit-LRU defer (D92)

Fresh simpleperf localization placed `PipelineCache::GetProgram` and its
ProgramHot work in the active GpuComm hot cluster. The already implemented
D92 marker `run-enable-program-hot-hit-lru-defer` therefore received a
same-binary A750 check: it delays victim selection until an exact lookup has
actually missed, without changing hit identity, generation, byte validation,
MRU, or the miss/replacement path. Runtime logged
`selected=1 hitVictimUnused=1 missLruExact=1`.

The first marker-free B and candidate A had different DVFS states. A showed a
raw +10.0% SF and +5.8% draws, but policy clocks were higher, while draw/core
improved only about 0.8%; that pair is not causal evidence. The hotter reverse
control B2 then outperformed A: 12.974 versus 12.806 SF FPS, 13.205 versus
12.835 presents/s, 15,460.508 versus 15,040.276 draws/s, and about +1.3%
draw/core for marker-free B2. Policy0/policy7 clocks were mixed rather than
matched, so no precise negative percentage is claimed, but the A/B2 direction
removes the only apparent material gain. D92 is rejected for A750, its marker
is absent, and the app is force-stopped.

Evidence:

- `artifacts/dualgoal/poco-d92-control-b-20260830-0647`
- `artifacts/dualgoal/poco-d92-candidate-a-20260830-0650`
- `artifacts/dualgoal/poco-d92-control-b2-20260830-0656`

## CPU-optimization configuration decisions (2026-08-29/30)

The current custom Bloodborne profile is intentional rather than an untouched
preset. Its effective runtime proof is FEX + Fast/Host mapped, block linking
On, D102 Safe/256 geometry, six guest workers, Android scheduler, dual
CPU/GPU, subsystem/I/O/audio workers On, adaptive CPU/GPU sync, Safe loop
policies and Safe function replacements. Atomics remain Fast. The proof also
shows that these settings are active: wait parks advance while idle/spin
counters remain zero in the sampled scene; 52,529 accurate HLE calls were
observed in the latest B2 window.

Two higher-risk configurations were already tested and must not be silently
restored:

- Aggressive function/idle/wait/spin initially rendered 12/12 correct frames,
  then entered a zero draw/submit/present-delta window after about two minutes.
  `waitParks=657` while idle/spin counters remained zero, identifying
  Aggressive wait-loop policy as the primary discriminator.
- Aggressive function replacements alone stayed visually correct to 02:37 but
  reduced presents from 11.372/s to 9.148/s (-19.6%), reduced draws by 19.4%,
  and increased CPU/present by 30.6% despite slightly higher candidate clocks.

Both were rejected. The device was restored to function/idle/wait/spin Safe,
atomics Fast and exact D9. Current preferences still hash to
`f8d07993cacb02824b49c49ba13229ab9707ecb14762a7197763e1b41b1e092f`.
Correct-frame series are retained at
`artifacts/dualgoal/poco-d9-cpu-guarded-series-20260829-224002`; the failed
Aggressive-only capture directory contains zero-byte frames and is not visual
evidence.

## Rejected on A750: ProgramHot overflow way (D93)

D93 adds one exact collision way to each grouped ProgramHot set and otherwise
retains cache identity, generation, byte, SRT, MRU and replacement contracts.
The exact D9 route already enables grouped TLS, so the existing
`run-enable-program-hot-overflow-way` marker exercised the intended path
without a rebuild.

The candidate lost despite a favourable device state. Marker-free B2 reached
12.974 SF FPS, 13.205 presents/s and 15,460.508 draws/s at policy0/policy7
medians of `1.5744/2.1888 GHz`, starting at 50.01 C. D93 reached only 12.518,
12.702 and 14,794.053 at higher `1.8048/2.4384 GHz` clocks and a cooler 48.29 C
start. The candidate therefore regressed raw SF by 3.5%, presents by 3.8%, and
draws by 4.3% even with more available frequency. No reverse run is needed;
the marker was removed and the app was force-stopped. Evidence:
`artifacts/dualgoal/poco-d93-candidate-a-20260830-0704` with the preceding
marker-free control
`artifacts/dualgoal/poco-d92-control-b2-20260830-0656`.

## Rejected on A750: SRT victim owner-count gate (D90)

D90 was reached, not merely selected. Runtime emitted
`firstSkip=1 ownerVictims=0 globalVictims=0 exactOwnerCount=1`, proving that the
per-Program zero-owner count bypassed the process-wide victim hash while every
identity/content/lifetime proof remained unchanged.

The A750 result was negative. Against marker-free B2, D90 changed SF
12.974 -> 12.466, presents 13.205 -> 12.628, and draws 15,460.508 ->
14,789.722. Candidate draw/core was about 3.6% lower. Its policy0 clock was
lower but policy7 was higher; the mixed DVFS state cannot establish an exact
negative percentage, yet it cannot support retention and the normalized
direction is also adverse. The marker
`run-enable-srt-victim-owner-count-gate` was removed and the app was
force-stopped. Evidence:
`artifacts/dualgoal/poco-d90-candidate-a-20260830-0709`.

## Accepted on A750: combined proven Snapdragon stack (2026-08-30)

The current APK promotes the previously measured high-yield Snapdragon paths
as one fail-closed Bloodborne production policy: D51/D53/D98 CPU bulk-write and
leaf-HLE work, D57/D59 shader/program-hot work, D58/D83/D89 descriptor and
modified-range work, and the retained D74/D77/D85 SRT/GPU-thread paths. The
same APK exposes `run-preserve-bloodborne-snapdragon-strong-bundle` as the
whole-stack control. The legacy DRS preserve marker remained present in every
window, so the rejected empty-physical DRS cleanup was not mixed into this
result.

After clearing cached background processes, the matched control produced
10.958 SF FPS, 11.084 presents/s and 12,967.314 draws/s. Two candidate windows
produced 11.925/11.248 SF FPS, 12.157/11.565 presents/s and
14,223.347/13,529.368 draws/s. Their mean versus control is +5.74% SF and
+7.01% presents/draws, while mean draw/core improved by 9.02%. The second
candidate was hotter and ran at a materially lower sampled policy7 clock, yet
still improved draw/core by about 7.7% on its own.

An eight-frame temporal series passed with advancing counters, stable geometry
and no duplicate/two-resolution frame. SGSR1 remained active at
960x544 -> 1280x720. POCO gameplay windows currently incur roughly one thousand
major faults because this route is swap-heavy; candidate and control were
therefore admitted only with comparable RAM/swap envelopes rather than mixed
with the normally clean Vivo criterion. The strong-bundle preserve marker is
absent in the accepted state, the DRS preserve marker is present, and LSX4 is
closed after capture.

Evidence:

- `artifacts/dualgoal/poco-sm8650-strong-bundle-aba-verdict-20260830`

### Superseded by long-liveness split (2026-08-30)

The short A/B/A acceptance above is withdrawn. A no-screencap production run
with the whole bundle stopped advancing draw, submit, and present counters in
its third 60-second window. The same APK with the whole bundle preserved stayed
live for all three windows. Splitting the controls then isolated the CPU layer:

- render defaults on with the CPU bundle preserved stayed live for three
  60-second windows (`10.810 -> 9.243 -> 9.157` presents/s);
- D53 alone stayed live but was neutral (`9.150` presents/s in the third window);
- D51 alone initially reached `14,203` draws/s, then caused a Turnip context
  reset, `VK_ERROR_DEVICE_LOST`, and SIGSEGV after 297 seconds of process uptime.
  The native backtrace entered `memcpy` through the typed HLE dispatch path.

D51 is therefore rejected on A750 despite its short-window gain. D98 depends on
D51 and is not independently eligible. The automatic D51/D53/D98 CPU bundle is
now A840-only; A750 keeps the independently validated render stack and the
accepted zero deferred-promotion RMW optimization. The POCO device also retains
`run-preserve-bloodborne-snapdragon-cpu-bundle` while APKs predating this source
scope may still be installed.

Evidence:

- `artifacts/dualgoal/poco-sm8650-splitbase-w1-20260830`
- `artifacts/dualgoal/poco-sm8650-splitbase-w2-20260830`
- `artifacts/dualgoal/poco-sm8650-splitbase-w3-20260830`
- `artifacts/dualgoal/poco-sm8650-d53-only-w1-20260830`
- `artifacts/dualgoal/poco-sm8650-d53-only-w2-20260830`
- `artifacts/dualgoal/poco-sm8650-d53-only-w3-20260830`
- `artifacts/dualgoal/poco-sm8650-d51-only-w1-20260830`
