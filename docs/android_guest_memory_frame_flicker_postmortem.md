# Android Guest-Memory Frame Flicker Postmortem

> [!IMPORTANT]
> Before any device/FPS/playable measurement, run `scripts/preflight-vivo-performance-test.ps1`;
> stale Android Studio process-trackers and known profiler loads make the result invalid. See the
> mandatory [performance-test preflight](PERFORMANCE_TEST_PREFLIGHT.md).

## Scope and result

This document records the investigation of recurring previous-frame and partially constructed-frame flashes in Android builds. Nidhogg 2 (`CUSA07640`) was the primary reproducer. The same class of defect could affect other games that update GPU-visible resources through frequent CPU writes.

The confirmed fix is a strict page-watcher contract: logical watcher permissions and the host operating-system page protection must be updated synchronously, and an ordinary write fault may invalidate only the faulting 4 KiB page. Explicit bulk writes retain a separate whole-range path.

The final build was verified on the Vivo device at 60 FPS with no detected ABA reversals or severe texture-strip frames during a 30-second capture. The fix adds no polling loop, timeout, or global CPU-side submit serialization.

Recorded against:

- repository HEAD: `b082c63c35f9` plus the current uncommitted working tree;
- `src/video_core/page_manager.cpp` SHA-256: `2E0E9005D565CA0E45DEF9310F6C8730165FC03DB564FCC6A441B369B8A33B94`;
- tested Android library SHA-256: `45BF090B2544590BBC88CE34F72F32CE0759103949F265B07B643A99FA0DF4F6`;
- verification date: 2026-07-24.

Line numbers below refer to that snapshot. Function names are the durable references if later edits move the lines.

## Observable symptoms

The defect had two visually distinct layers:

1. A severe texture-atlas or horizontal-strip corruption appeared periodically, commonly once every six captured frames.
2. After the severe strip was removed, the game could still alternate between a complete frame and a partially constructed frame. Missing UI, fragments of text, diagonal geometry, or a previous frame appeared for a single refresh and then reverted.

Important diagnostic characteristics:

- The flashes occurred approximately two or three times per second during ordinary observation, but the severe form could have a precise six-frame cadence in a 60 FPS recording.
- A static screenshot was not a reliable test because it usually captured a valid frame.
- The defect was content-dependent. A scene or character selection could temporarily look correct while another scene still reproduced it.
- Stable reported FPS and submit/present rates did not rule it out. Nidhogg 2 continued to report approximately `60/60/60` while individual presented frames were wrong.
- Limbo was not a useful negative or positive reproducer for this specific contract. Nidhogg 2 and Hyper Light Drifter exercised the relevant update pattern more aggressively.

## Reliable detection method

Use a continuous 12-30 second, 60 FPS screen recording rather than isolated screenshots. Inspect both visual contact sheets and frame-to-frame metrics.

Two useful detectors are:

- **Severe-frame detector:** identifies the large texture-atlas or strip discontinuity seen in the first failure mode.
- **ABA detector:** identifies a short reversal in which frame `N` returns toward frame `N-2` after a different or incomplete frame `N-1`. This catches alternating complete/incomplete frames that do not look like the severe atlas strip.

The recordings used during this investigation were temporary diagnostic artifacts and were not added to the repository. Their identifying names and results are retained here so later work can reproduce the same progression.

| Experiment | Capture | Result |
| --- | --- | --- |
| Initial clean baseline | `lsx4_nidhogg_clean.mp4`, 714 frames / 12 s | 120 severe frames and 118 ABA events; severe corruption was nearly every sixth frame |
| CPU-side submit serialization | `lsx4_nidhogg_serial.mp4`, 588 frames | 98 severe frames; still exactly periodic, so CPU queue serialization was not the root fix |
| GPU timeline chaining | `lsx4_nidhogg_gpu_chain.mp4` | Removed the severe detector result in one red scene, but did not eliminate the content-dependent defect |
| Different character/scene | `lsx4_nidhogg_char.mp4`, 719 frames | 120 severe frames, again every six frames |
| Exact ordinary fault page | `nidhogg_exact_page.mp4`, 722 frames | Severe atlas-strip frames fell to zero, but 58 ABA/partial-frame reversals remained |
| Exact ordinary fault page, alternate content | `nidhogg_exact_alt.mp4`, 721 frames | 41 ABA reversals remained |
| Strict synchronous watcher protection | `nidhogg_strict_watchers.mp4`, 717 frames / 12 s | Severe frames: 0; ABA events: 0; frame delta mean 1.520, p95 2.017, max 2.134 |
| Strict watcher long run | `nidhogg_strict_watchers_30.mp4`, 1801 frames / 30 s | Severe frames: 0; ABA events: 0; frame delta mean 1.516, p95 2.002, max 2.662 |

## Unsuccessful or incomplete approaches

### Static screenshots

Screenshots frequently showed a correct frame even while the defect flashed several times per second. They are suitable for checking persistent texture damage, but not for accepting a temporal-ordering fix.

### Global CPU-side GNM submit serialization

The diagnostic mode waits for the submitted master-timeline value after each submit:

- [`src/video_core/renderer_vulkan/vk_scheduler.cpp`](../src/video_core/renderer_vulkan/vk_scheduler.cpp), `Scheduler::SubmitExecution`, lines 1277-1286;
- [`src/executor/native_runtime_api.cpp`](../src/executor/native_runtime_api.cpp), sentinel `run-android-serialize-gnm-submits`, lines 22360-22367.

This did not remove the six-frame corruption cadence. It also places a host wait in a throughput-critical path, so it must not become the production solution.

### Vulkan master-timeline chaining

Android submissions were chained to the preceding master-timeline value:

- [`src/video_core/renderer_vulkan/vk_scheduler.cpp`](../src/video_core/renderer_vulkan/vk_scheduler.cpp), `Scheduler::SubmitExecution`, lines 1192-1211.

This made queue ordering explicit and improved some scenes, but later content still exposed partial-frame flashes. It was therefore useful ordering hardening, not the complete root-cause fix.

### Presenter and render-target source selection

The investigation traced the live color target and the VideoOut source:

- [`src/video_core/renderer_vulkan/vk_presenter.cpp`](../src/video_core/renderer_vulkan/vk_presenter.cpp), `Presenter::PrepareFrame`, lines 438-468;
- [`src/video_core/texture_cache/texture_cache.cpp`](../src/video_core/texture_cache/texture_cache.cpp), `ExecutorTrackLiveDrawnColorTarget`, lines 889-907;
- [`src/video_core/texture_cache/texture_cache.cpp`](../src/video_core/texture_cache/texture_cache.cpp), `ExecutorTakeLiveFullResolutionTarget`, lines 909 onward;
- [`src/video_core/renderer_vulkan/vk_presenter.cpp`](../src/video_core/renderer_vulkan/vk_presenter.cpp), final present waits and submit, lines 1084-1089.

The trace established that Nidhogg 2 legitimately alternated between two full-resolution 1920x1080 surfaces: one VideoOut/sRGB surface and one UNORM render-target alias. The presenter was not randomly selecting an unrelated target. Source-selection changes could alter how the symptom looked, but did not remove its memory-coherency cause.

The bounded trace switch is retained at [`src/executor/native_runtime_api.cpp`](../src/executor/native_runtime_api.cpp), lines 22736-22740. The `run-trace-videoout-source` sentinel must be absent during normal performance testing.

### Treating every `AcquireMem` as a full barrier

PM4 tracing showed a high `AcquireMem` rate, approximately 17,849 occurrences in 18 seconds. Converting every occurrence into a full host/GPU barrier was rejected because:

- the frequency would make the cost substantial;
- it did not explain the exact page-update corruption signature;
- it would mask the broken watcher contract with global synchronization.

The `run-live-pm4-trace` sentinel was diagnostic only and must be absent during normal runs.

### Expanding one write fault to neighboring pages

An earlier optimization speculatively expanded a single write fault to a wider neighboring range. This reduced fault traffic but invalidated pages that had not actually faulted. It caused the severe texture-atlas strip and its precise six-frame cadence.

Removing that expansion and invalidating only the aligned faulting page eliminated the severe strip, but did not by itself fix the remaining ABA flashes. It was necessary, but not sufficient.

## Root cause

The problem was guest-memory coherency, not raw presenter ordering.

There were two coupled violations:

1. **Speculative range invalidation:** an ordinary page fault could be expanded beyond the page that actually faulted. Neighboring GPU-visible data was invalidated at the wrong point in the producer sequence.
2. **Logical/physical protection divergence:** watcher counters could say that a page required write protection while the host page remained writable because protection application was deferred or skipped through a permissive bypass state. CPU writes could then occur without a fault, so the texture cache did not receive the event that made the new data visible at the correct time.

This explains why queue serialization and presenter changes were incomplete. They could order already-recorded events, but they could not manufacture a missing memory-write notification.

## Final fix

### 1. Apply watcher protection synchronously

[`src/video_core/page_manager.cpp`](../src/video_core/page_manager.cpp), `ApplyWatcherProtection`, lines 445-449, now always calls `Protect(address, size, perms)`.

Do not reintroduce a condition that skips this call because an `ActualWriteProtected` bit, deferred state, or historical bypass appears to match. The watcher transition and operating-system protection transition form one contract.

The two watcher update paths that invoke it are:

- `UpdatePageWatchers`, lines 663-724, with the protection application at 679-686;
- `UpdatePageWatchersForRegion`, lines 727 onward, with the protection application at 747-754.

### 2. Do not persist an ordinary write-fault bypass

[`src/video_core/page_manager.cpp`](../src/video_core/page_manager.cpp), `CommitWriteFault`, lines 465-480, ignores the former persistent-fault request and only restores writable host permissions if the current cached page permissions actually contain `Write`.

The critical behavior is:

- if the page is already physically writable, the fault is committed;
- if the current logical permission does not allow writes, the function refuses to relax protection;
- otherwise it applies the current page permissions to exactly that page.

Do not allow a normal fault to leave the page permanently writable while a watcher remains active.

### 3. Invalidate exactly one page for an ordinary fault

[`src/video_core/page_manager.cpp`](../src/video_core/page_manager.cpp), `GuestFaultSignalHandler`, lines 587-624, initializes the invalidation range to the aligned 4 KiB faulting page at lines 595-596 and submits that range at lines 608-610.

Only an explicitly registered bulk write may replace that range with its known full extent, at lines 598-605. There must be no heuristic neighboring-page burst based solely on a recent fault address or cadence.

### 4. Preserve the explicit bulk-write fast path

Large known copies must not pay one signal fault per page. Their full range is invalidated before the write:

- [`src/video_core/page_manager.cpp`](../src/video_core/page_manager.cpp), `PrepareBulkGuestWrite`, lines 519-540;
- `SetBulkGuestWriteActivity`, lines 558-584;
- public entry points, lines 876-891.

The libc guest bridges explicitly bracket their destination range:

- [`src/core/aerolib/stubs.cpp`](../src/core/aerolib/stubs.cpp), `ExecutorLibcMemcpy`, lines 3937-3944;
- `ExecutorLibcMemmove`, lines 3954-3961;
- `ExecutorLibcMemset`, lines 4014-4019.

This separation is essential:

- unknown ordinary writes use the exact faulting page;
- known bulk writes use the exact caller-provided range;
- neither path guesses neighboring pages.

## Required invariants

Future changes to page tracking must preserve all of the following:

1. A watcher-count transition that changes logical permissions synchronously applies the equivalent host page protection before returning.
2. A write fault relaxes protection only when the current logical permissions permit writing.
3. An ordinary fault invalidates exactly the aligned faulting page.
4. A range invalidation larger than one page requires an explicit, active bulk-write range from the producer.
5. A bypass used for an active bulk write is removed when the bulk operation ends or a new watcher becomes active.
6. Presenter or Vulkan queue barriers are not substitutes for a missing CPU-write notification.
7. The production path remains event-driven: no polling, fixed delay, per-submit host wait, or game-specific address/signature rule.

## Regression procedure

For a page-watcher or Android presentation change:

1. Build and install the current `.so`, then verify its host and device SHA-256 values match.
2. Remove diagnostic sentinels, especially `run-android-serialize-gnm-submits`, `run-live-pm4-trace`, and `run-trace-videoout-source`.
3. Run Nidhogg 2 through both menu and character/gameplay scenes for at least 30 seconds.
4. Record at 60 FPS and inspect the sequence, not only a screenshot.
5. Require zero severe-strip frames and zero ABA reversals for acceptance.
6. Confirm FPS, submit rate, and present rate did not regress; the confirmed run remained at approximately `60/60/60`.
7. Recheck Hyper Light Drifter because it exercised a related resource-update pattern, and retain a game such as Limbo as a broader non-regression check rather than as the primary reproducer.

If a future change improves one scene but fails another, treat it as incomplete. The successful fix must satisfy the memory-protection invariants independently of engine, game, surface address, or frame cadence.

## Bloodborne mobile-renderer regression (2026-07-29)

### Symptom

After enabling the new managed renderer optimizations, Bloodborne could alternate between complete
frames and frames with missing character parts, missing terrain, white attachment regions, or
temporarily recolored foliage. Static screenshots were insufficient; the reliable reproducer was a
20-second 60 FPS screen recording of the Hunter's Dream gameplay scene.

### Confirmed cause and fix

The ordinary draw path had inherited the late-render-scope ordering needed by physical DRS and
deferred transfer batching. It bound the graphics pipeline and vertex/index state before opening
the dynamic-rendering scope even when neither optimization had pending work. That ordering is legal
in the abstract Vulkan model, but was not stable on the tested Qualcomm mobile path and produced
intermittent loss of draw state/resources.

[`vk_rasterizer.cpp`](../funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.cpp) now uses two
explicit paths:

- ordinary draws enter dynamic rendering before binding graphics/vertex/index state, matching the
  previously stable renderer contract;
- the late scope remains enabled only when a physical-DRS target is active or the scheduler has
  pending buffer transfers that can still end the scope.

This is not a blanket rollback: transfer coalescing and physical DRS retain their required late
scope, while unrelated draws no longer pay its compatibility risk. A 20-second gameplay recording
after the change contained zero missing-character, missing-terrain, white-region, or foliage-color
frames.

The diagnostic `run-bisect-disable-pm4-descriptor-cache` sentinel was also removed before the
confirmation run; it must not be left on during performance measurements.

### Hypotheses tested but not sufficient

- Reverting typed buffer alias bindings removed one independent regression but did not eliminate
  all missing-surface frames.
- Restoring strict guest page watcher protection was required for coherency but did not fix this
  renderer-order regression by itself.
- Making new scheduler optimization state ABI-neutral removed the stale-layout failure, but current
  scheduler behavior still exposed the draw-order issue.
- Correcting the fault-bitset Vulkan barrier to describe `atomicOr`/`atomicExchange` as shader
  read-modify-write access was valid synchronization hardening, but the dynamic artifact remained.
- Replacing the atomic fault producer with the older non-atomic load/OR/store path did not change
  the artifact; the atomic producer was restored.
- Disabling render-scope reuse, transfer coalescing, state deduplication, PM4 descriptor caching,
  tile scratch reuse, graphics-pipeline generation shortcuts, async pipelines, and page-manager
  experiments individually did not remove the defect.
- Pipeline-cache telemetry showed frequent legitimate graphics-state changes; it was not evidence
  that shader invalidation itself caused the missing meshes.

### Additional invariant

On the Android renderer, do not apply late dynamic-rendering scope creation globally. It is allowed
only while a known operation can still flush transfers or replace the render target. The ordinary
path must preserve the stable ordering:

`BindResources / barriers -> BeginRendering -> bind pipeline -> bind vertex/index -> draw`.

### Related full-profile T2 stall

Enabling every managed optimization after the renderer fix exposed a separate loading-screen
stall: GNM progress stopped after `Continue`, while the tiered worker kept publishing T2 traces.
An A/B run with T0/T1, Vulkan, readback, DRS and async pipelines unchanged isolated the failure to
trace compilation.

The trace planner could be queued from a forward edge, follow several hot successors, and later
discover a backedge to a member already in the trace. It stopped extending the plan but still
published it as `loopOsr=0`. The resulting native self-chain had no periodic composite-witness
safepoint and could remain inside stale polling/control flow indefinitely.

[`retiring_execution_core.cpp`](../src/executor/dynamic_translation/retiring_execution_core.cpp)
now handles the cycle without disabling T2:

- an exact closure to the trace seed is promoted to the existing safe loop-OSR path;
- a cycle into another member is rejected and re-profiled from its real loop head;
- minimum-size and loop-closure validation use the final plan classification, including dynamically
  discovered loops.

With `TRACE` enabled again alongside the complete managed GPU/readback/async profile, Bloodborne
passed the same loading point and ran gameplay continuously. A 20-second recording contained no
stall, missing geometry, white attachment regions, or foliage-color frames.

Additional JIT invariant: no trace that can native-chain back into its own member set may be
published as a straight-line trace. Every such cycle requires a validated loop head and periodic
OSR/deopt safepoint.

## Nidhogg 2 Adreno `DEVICE_LOST` from descriptor snapshots (2026-07-31)

### Symptom and fault signature

Nidhogg 2 could reach live rendering and accept input, then lose the Vulkan device when a
post-process-heavy gameplay transition was submitted. The terminal error was always reported by
`queue_submit` as `VK_ERROR_DEVICE_LOST`; it was not a guest crash or a frozen FPS counter.

The useful identity was the 56-byte device-fault fingerprint
`0x721c15d6dc5462f3`. It recurred at unrelated scheduler ticks:

- `cusa07640-direct-valid-r1-log.txt`, lines 1511-1515: tick 1579;
- `vivo-main-robust-current.txt`, lines 1859-1863 and 4392 onward: ticks 1347 and
  6092, with two later occurrences at 13968 and 19102;
- `vivo-main-alloff-keyroute.txt`: ticks 1347 and 6092 even with the managed
  Vulkan optimizations disabled;
- `vivo-barrierfix-logcat.txt`, lines 1708-1710: tick 1815;
- `vivo-shaderdump-logcat.txt`, lines 1717-1720: tick 1669.

The driver supplied no useful fault address or vendor record (`addresses=0/0`,
`vendors=0/0`). The draw journal was therefore evidence about the workload, not proof that its
last recorded draw was the instruction that faulted. For example,
`cusa07640-vk-journal-r4.txt` repeatedly records the same 640x360 post-process family
(`key=0xb525d5f028a56f6f`, two buffers and four images) immediately before the delayed submit
failure.

### Root cause and fix

The Android read-only rename fast path copied small CPU-readable guest ranges into the shared,
host-visible `StreamBuffer` ring. `Rasterizer::BindResources` then used that ring slice directly
as the backing of read-only Uniform or Storage descriptors. Under sustained descriptor churn and
ring wrap, an Adreno submission could consume a descriptor range after the shared slice had been
recycled for a newer draw. Flushes, barriers, `Commit()`, and `RetainCurrentAllocation()` make
writes visible and track ordinary ring use, but they did not establish the required lifetime for
these descriptor-backed snapshots.

The fix is type-specific rather than a global rollback. In
[`buffer_cache.cpp`](../funnel-arm/src/video_core/buffer_cache/buffer_cache.cpp),
`ExecutorReadOnlyRenameUsageEnabled` unconditionally rejects `Uniform` and `Storage`. Those
requests fall back through `ObtainReadOnlyBuffer` to the stable tracked buffer-cache backing.
Vertex and index snapshots retain the fast path because the reproducer isolated the unsafe
lifetime to descriptor-bound UBO/SSBO ranges. The binding site is
[`vk_rasterizer.cpp`](../funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.cpp),
`Rasterizer::BindResources`.

The A/B progression is retained in the workspace:

- `current-vivo-cusa07640-renameoff.png` shows active gameplay at 00:55 with the whole rename
  path disabled;
- `current-vivo-cusa07640-hardgate-death.png` shows the selective UBO/SSBO gate still running
  gameplay at 01:47;
- `current-vivo-cusa07640-final-nobisection2.png` shows the production selective gate, without
  pipeline bisection, active at 01:10 with draw, submit, and present counters advancing.

The failing instrumented runs lost the device around 15-20 seconds or at the corresponding
gameplay transition. Surviving the same match/death path for more than a minute while retaining
vertex/index rename distinguishes the descriptor-lifetime fix from a blanket performance
disable.

### Hypotheses and attempts that were not fixes

- Robust descriptor access did not change the fingerprint; `vivo-main-robust-current.txt`
  contains four occurrences.
- Disabling the managed Vulkan optimization group did not remove it; the all-off run retained
  the same fingerprint and failing ticks.
- Extra buffer/image barriers and descriptor-range adjustments did not make a recycled ring
  slice immutable. The barrier-fix and range-fix runs still produced a failing journal.
- Shader dumping and pipeline/journal instrumentation localized the recurring post-process
  workload but did not prevent the loss. Pipeline skipping is not a valid production fix because
  device loss is reported asynchronously and skipping a consumer only hides the lifetime defect.
- Treating the final journal draw, one shader hash, or one title address as the cause was rejected.
  The solution contains no title, pipeline, shader, or guest-address hardcode.

### Required invariants

1. Every Uniform or Storage descriptor backing range remains byte-stable and allocated until the
   completion timeline of every submission that can consume it.
2. Host coherence, a flush, or a Vulkan memory barrier is a visibility guarantee, not an
   allocation-lifetime guarantee.
3. The common host-visible `StreamBuffer` ring must not back read-only Uniform/Storage descriptors
   under the current retirement model. A diagnostic marker must not bypass this hard gate.
4. Reintroducing descriptor snapshots requires a dedicated arena whose slices are retired by the
   actual consuming submit timeline, including wrapped ranges and deferred command buffers.
5. Uniform/Storage fallback must preserve descriptor alignment, offset/range, synchronization, and
   stable tracked backing. Vertex/index eligibility remains independent.
6. A `DEVICE_LOST` returned by submit may describe earlier GPU work. Correlate the binary
   fingerprint, journal window, and per-usage A/B gates; never hardcode the last recorded draw.
7. Compatibility fixes remain engine- and title-independent.

### Regression tests

1. Add a routing contract test: read-only `Uniform` and `Storage` requests must never return the
   shared stream buffer; eligible `Vertex` and `Index` requests may still do so.
2. Add an in-flight wrap stress test with a deliberately small ring and multiple uncompleted
   submissions. Shader-observed UBO/SSBO checksums must remain unchanged, and no descriptor may
   reference a recycled slice.
3. In telemetry, require `renamedUsage.ubo == 0` and `renamedUsage.ssbo == 0`; nonzero
   vertex/index rename counts are expected and guard against an accidental global rollback.
4. Run Nidhogg 2 through menu, match, death, restart, and a second match for at least five minutes.
   Acceptance requires no `EXECUTOR_VK_DEVICE_FAULT`, no
   `EXECUTOR_VK_TERMINAL_ORIGIN ... ErrorDeviceLost`, and specifically no
   `0x721c15d6dc5462f3`.
5. Recheck Bloodborne or another high-descriptor-churn title, then a PS4 control title that relies
   heavily on streamed vertex/index data. This verifies both descriptor safety and retention of
   the unaffected fast path.

## Dead Cells nondeterministic missing texture layers from deferred uploads (2026-07-31)

### Symptom

Dead Cells (`CUSA10484`) intermittently entered the same live dialogue scene with characters,
particles, UI, and the spotlight present, but the room masonry, foreground, weapon, and other
texture layers absent. The surviving image was a flat blue background. Draw, submit, and present
continued at 56-60 per second, so this was neither a guest freeze nor a lost Vulkan device.

The failure was startup-order dependent. An unchanged binary could render the full room once and
lose it on the next cold launch, which made single screenshots and title-specific shader theories
misleading.

### Isolation and root cause

The five-way Vulkan bisection was repeatable:

- disabling state, pipeline-bind, vertex-bind, transfer, and render-scope fast paths produced two
  consecutive correct cold launches;
- enabling all bind/state deduplication while leaving transfer coalescing and render-scope reuse
  disabled remained correct;
- leaving only transfer coalescing disabled remained correct;
- enabling transfer coalescing while disabling only render-scope reuse reproduced the missing
  room immediately;
- retaining delayed coalescing but replacing range-local barriers with the old conservative
  global barriers still reproduced the defect.

The last result separates visibility-barrier shape from command timing. Transfer telemetry on the
failing path reported only uploads (`origin=h:0,u:45140,g:0`) and a maximum of two pending groups.
The rare second pending upload was sufficient: an `Upload` source belongs to a staging/ring
allocation whose current lifetime contract ends once its copy is recorded. Carrying that upload
across a later request allowed the source range to be recycled or republished before the deferred
`copyBuffer` consumed it. Which texture layers received stale bytes therefore depended on request
timing.

[`vk_scheduler.cpp`](../funnel-arm/src/video_core/renderer_vulkan/vk_scheduler.cpp) now preserves
coalescing of adjacent/multiple regions inside one upload request, then immediately materializes
that request. Cross-request pending batching remains available for HLE and GNM DMA transfers,
whose sources have separate lifetime/ordering contracts. The fix contains no title, address,
texture, shader, or pipeline hardcode.

### Rejected hypotheses

- Recording detile work in the primary command buffer instead of its secondary buffer produced
  one correct launch followed by a broken launch.
- Disabling detile scratch-pool reuse and allocating fresh scratch buffers remained broken.
- Disabling all read-only rename paths remained broken; UBO/SSBO rename was already hard-gated.
- Disabling asynchronous pipelines remained broken.
- Disabling adaptive mobile GPU/residency/DRS remained broken; coarse 2x2 shading was already off.
- Restoring direct page-manager `InvalidateMemory`/`ReadMemory` routing remained broken, excluding
  guest-invalidation batching as the cause.
- State, pipeline-bind, and vertex-bind deduplication remained enabled in a correct run.
- Render-scope reuse disabled by itself did not help.
- Conservative global memory barriers around the delayed batch did not help; the defect was the
  staging lifetime crossed by deferral, not a narrower access mask.

The detile scratch pool, guest-thread invalidation routing, async pipeline, adaptive mobile GPU,
bind/state deduplication, render-scope reuse, and local transfer barriers were restored after their
negative A/B results. No diagnostic bisection marker remains enabled.

### Required invariant and regression checks

1. A staging-backed upload must be recorded before the producer can recycle, overwrite, retire,
   or republish its source allocation. A memory barrier cannot extend allocation lifetime.
2. Multi-region merging within one request is safe; cross-request upload deferral requires an
   explicit submit-timeline pin for every source slice and must not be reintroduced without one.
3. HLE/GNM DMA pending batching must remain independent from upload eligibility.
4. `current-vivo-deadcells-upload-lifetime-r1.png` and
   `current-vivo-deadcells-upload-lifetime-r2.png` are two consecutive correct cold launches with
   every managed optimization enabled and no bisection marker. The later
   `current-vivo-deadcells-upload-lifetime-gameplay.png` remains correct after additional dialogue
   and resource activity at 58 FPS.
5. Regressions must be checked with at least two cold launches and a resource-transition segment;
   one correct frame is insufficient for this timing-dependent bug.

## Downwell recurrence on Android 9 (2026-08-08)

Downwell on a Redmi Note 6 Pro again alternated complete and unreadable frames. The first-frame
trace showed the same legitimate double-buffer pattern as Nidhogg 2: one 1920x1080 sRGB VideoOut
surface and one compatible UNORM render-target alias. That alternation was diagnostic evidence,
not the cause, and the presenter source-selection heuristic was left unchanged.

The regression was the page-manager `Protect` no-op shortcut. It treated
`ActualReadProtected`/`ActualWriteProtected` as proof of the current host PTE and skipped the OS
protection call. This repeated the logical/physical watcher divergence described above: a later
CPU write could remain writable and therefore produce no invalidation fault. The shortcut was
removed; every watcher transition again applies the host protection synchronously and only then
updates the cached telemetry bits.

Do not restore a cached-bit `mprotect` elision. Optimizing watcher transitions requires a contract
that is authoritative for signal-side and bulk-write transitions too; the current cached bits are
not that contract. Also do not pin one VideoOut buffer or normalize away the sRGB/UNORM alias pair:
double-buffered compatible aliases are valid and were already disproved as the root cause.

## Cross-title Android 0-FPS regression from unbounded submit deferral (2026-08-09)

### Symptom and root cause

Downwell and Sonic could stop before useful rendering on both an Adreno 509 Android 9 device and
a Snapdragon 8 Elite Gen 5 device. This was not a JIT deadlock. Android submit coalescing deferred
`FlushPlain` until a batch of 16, but no pending-submit worker was started and the stored deadline
was never consumed. An underfilled queue therefore had no bounded route to Vulkan submission.

Android submit coalescing is now quarantined. It must not be re-enabled until every deferred batch
has an independently scheduled, deadline-bounded flush and every HLE wait observes that pending
work. A throughput optimization may delay a submit; it may never make progress depend on a later
submit arriving.

### Related launch and protection regressions

- Normal launches now select the `.so` from the installed APK's `nativeLibraryDir`. The writable
  sandbox runtime is selected only by the explicit runtime picker. This prevents an old copied
  binary from silently overriding a newly installed build.
- Repeated ordinary page-protection requests use the cached state again to avoid process-wide
  `mprotect`/TLB churn. Watcher-count transitions (`0 -> 1` and `1 -> 0`) force the host transition,
  preserving the physical PTE invariant that fixed the Android 9 frame alternation.
- Tier-1 compilation has a negative-yield circuit breaker. On the Redmi trace, 64 blocks compiled,
  zero activated, and all 64 were rejected, consuming a CPU core without improving generated code.
  After eight consecutive `no-static-cost-win` rejections with no active T1 block, new admission is
  suspended and queued work is dropped. A successful promotion resets the streak.

### Regression checks

The packaged PS4 and PS5 libraries must hash-identically to the two build outputs before install.
After install, launch both titles by exact title path, confirm nonzero draw/submit/present, and stop
the process immediately after the measurement. The r237 control run on Snapdragon 8 Elite Gen 5
showed Downwell at 60 FPS with 60 submit/present per second and Sonic at 60 FPS with 60
submit/present per second. The Vulkan capability bridge guard also passed.

## Cross-process settings and transient CPU topology regression (2026-08-09)

### Symptom and root cause

The launcher preferences showed fast guest memory, tiered JIT and trace JIT enabled, while the
separate `:game` process logged all three as disabled. Android `SharedPreferences` is not a
cross-process coherence mechanism: the game process could retain a stale in-memory view even
after the launcher committed newer values. This silently selected alignment checks and stronger
guest-memory ordering in generated code and made a low-end ARMv8 device substantially slower.

The second independent issue was transient CPU hotplug. Selecting one preferred graphics CPU
could fail with `EINVAL` while that core was temporarily offline, leaving the graphics thread on
an arbitrary little core for the rest of the session.

### Required contract

- Every game launch carries one typed, whitelisted settings snapshot in the launch Intent. The
  `:game` process applies and synchronously commits it before any runtime option is read.
- Launcher and game logs must report the same snapshot generation and fingerprint. The runtime
  summary must agree with the values in that snapshot.
- Exact latency/graphics affinity first falls back to the available performance-cluster mask when
  a selected CPU rejects the request, then retries exact isolation after the workload has started.
  Never leave a latency-sensitive thread on its inherited CPU after one transient `EINVAL`.
- Do not replace the snapshot with multi-process preference polling. New managed settings must be
  added to the explicit snapshot whitelist, with primitive types only.

### Regression check

The r238 control run on TECNO CK8n (Mali, Android 14) logged an identical 16-key snapshot and
fingerprint in both processes, applied `fastGuestMemory=true tiered=true trace=true`, and accepted
the exact latency and graphics affinity masks. Sonic then sustained approximately 59-60 FPS after
warm-up, with no fatal signal or Vulkan device loss. The process was stopped immediately after the
measurement.

## Bloodborne ABI65 guard-chain and HLE transfer regressions (2026-08-10)

### Confirmed breakages

- The ABI65 aggressive T2 cold-guard direct-chain experiment produced complete texture/resource
  corruption. A mid-trace guard exit carried fixed GPR/XMM state directly into a target whose
  resident-register and machine-state authority contract was not equivalent. The old dispatcher
  exit had published canonical state; bypassing it made the target observe stale or incompatible
  values. The affected artifact was `app-debug-abi65-guard-chain.apk`. Restoring the ABI64 control
  restored complete geometry, textures, character and lighting.
- A narrower resident guard-chain variant avoided the immediate full-frame corruption but still
  produced black rectangles during movement and rare bright flashes. It is not accepted as safe
  and remains disabled pending an edge-by-edge proof of GPR, XMM/YMM, RFLAGS, deferred-fault and
  machine-state equivalence. Its current opt-in controls are
  `run-bisect-enable-jit-tiered-guard-resident-chain` and
  `EXECUTOR_ENABLE_JIT_TIERED_GUARD_RESIDENT_CHAIN`. The earlier aggressive experiment used
  `run-bisect-enable-jit-tiered-guard-direct-chain` and
  `EXECUTOR_ENABLE_JIT_TIERED_GUARD_DIRECT_CHAIN`; those historical controls must not be restored
  as default-on aliases.
- Default-on HLE transfer deferral skipped the range flush before `ObtainReadOnlyBuffer` could
  select or rename the source backing. Pending copies could therefore leave a stale Vulkan buffer
  identity for a later source or destination alias, producing nondeterministic resource contents.
  HLE deferral is now default-off and available only through
  `run-bisect-enable-vk-hle-transfer-deferral`. Destination acquisition always calls
  `FlushPendingBufferTransfersForRange`; source acquisition may skip it only inside the explicitly
  enabled experiment after an equivalent lifetime/identity proof.
- The Clang `preserve_all` helper ABI currently applies only to the narrow multiply-accumulator
  helper experiment. Its measured coverage is tiny and no title-wide gain or complete boundary
  proof has been established. It remains opt-in through
  `run-bisect-enable-jit-preserve-all-helper-abi` or
  `EXECUTOR_ENABLE_JIT_PRESERVE_ALL_HELPER_ABI`; it must not be treated as a validated general JIT
  calling convention.

The repeated JIT failures belong to one contract class. Unconditional RET chaining and a loop
safepoint interval widened to 16384 iterations both starved the scheduler/HLE observer and produced
FPS 0. Write-back across faultable regions, implicit-boundary caching and aggressive GPR/XMM
chaining likewise moved the point at which architectural state became authoritative; depending on
the path, the result was FPS 0, a frozen frame or corrupted GPU descriptors/resources. A partial
write-back attempt was also rejected on objective code-generation evidence: it replaced loads with
nearly the same number of stores plus moves and increased native code size, so it shifted work
rather than reducing it.

### Measurement and routing errors that invalidated comparisons

Repeated Cross input during startup selected a different route/save and entered Iosefka's Clinic
instead of the established Hunter's Dream fountain scene. That run was useful only as a graphics
correctness control. It was not a performance A/B against the former fountain result. Startup
automation must not spam Cross: wait for the exact expected screen, issue at most the required
single input, and reject the sample if the scene, save, camera path or gameplay interval differs.

Raw FPS alone is not causal evidence across different scenes, resolutions, temperatures or
governor states. Performance acceptance requires the same executable and cache generation, same
internal extent, save/scene, deterministic analog movement and measurement duration. Record at
least generated ARM instructions per guest operation, generated-code bytes, native-chain/internal
edge rate, dispatcher/`RunGuest` share, guard deopts/rollbacks, machine-state load/store traffic,
GPU timestamp/busy time, submits, driver calls and blocking/wakeup counts. Stop the game immediately
after each sample. Thermal and frequency data annotate a run; they do not substitute for those
internal work metrics.

### Rejected visual hypotheses

The following switches can change the appearance of a bad frame but did not remove this class of
temporal/resource corruption and must not be presented as fixes:

- forcing alpha threshold to zero or disabling shader `discard`;
- forcing `CULL_MODE_NONE` or changing winding;
- freezing foliage/wind game time in the vertex shader;
- disabling TAA or camera jitter;
- changing anisotropy, LOD, blur or dynamic resolution;
- globally serializing submit/present or adding broad barriers;
- changing presenter source selection, render scale or one title's shader/pipeline state.

These remain useful isolation probes only. A correct fix must retain correct consecutive frames
while the original content, camera motion and resource update path are active.

### Additional prevention invariants

1. A direct JIT edge may retain fixed GPR/XMM/YMM/RFLAGS state only when source and target share an
   explicit, versioned resident-state ABI and the edge proves every live register, dirty mask,
   deferred-zero/deferred-flag recipe, fault metadata and target revision. Otherwise it must use the
   canonical publication/dispatcher boundary.
2. A mid-trace guard is an observable deopt boundary. Its miss path must reproduce the exact
   architectural state of the baseline exit before HLE, signals, scheduling, invalidation or the
   target block can observe it. A passing straight-line self-test is not sufficient evidence.
3. Guard direct/resident chaining stays opt-in until moving gameplay recordings show zero black
   rectangles, bright flashes, missing resources and ABA reversals, and an ABI64 A/B shows matching
   guest state at every sampled exit.
4. Before selecting, renaming or returning a Vulkan backing for an HLE copy, flush pending transfers
   intersecting the relevant source and destination ranges. A barrier provides visibility; it does
   not make a stale buffer identity current or extend a recycled allocation's lifetime.
5. Transfer deferral requires explicit ownership and submit-timeline retirement of every captured
   source/destination backing. Without that proof, record the copy before a later `Obtain*` call may
   replace either identity.
6. Experimental helper ABIs require per-helper clobber metadata, unwind/signal compatibility and
   generated-code tests for every live scalar/vector register. Unsupported hosts and unproven
   helpers must use the standard ABI.
7. A visual performance run is valid only after route, scene, actual render extent and cache/build
   identity are logged. Do not infer an optimization gain or regression from FPS gathered on a
   different route.
8. RET, HLE, event, deopt and loop observer boundaries remain sampled and bounded. Native chaining
   may amortize them but may not remove them or stretch the maximum return interval without a
   scheduler-progress proof and a watchdog-visible fallback.
9. Every faultable native site carries exact guest-PC, live-register and deferred-state recovery
   metadata. A fault path must not rely on memory-authoritative state if the optimized path retained
   newer values only in host registers.
10. Accept only reductions in total dynamic instructions, state traffic, waits or driver work.
    Replacing loads with stores/moves, moving work to another thread, or improving one component
    while total task-clock grows is not a performance optimization.

## Qualcomm host-pass and rendering-epoch regression (2026-08-11)

### Symptom and evidence

Bloodborne could render correctly for a while, then show black rectangles or brief bright flashes
during camera movement. One run later crashed on `GpuComm` inside the Adreno driver from
`vkCmdDrawIndexed`, with a null-derived fault address of `0xf0`. The same workload reopened a
dynamic-rendering scope very frequently, and most reopens reused the same guest state. A single
still screenshot was therefore not sufficient evidence: the usual outcome of stale state was a
wrong draw, while the driver crash was the rarer outcome.

Code history identified a concrete regression. Commit `313f0419` removed
`Scheduler::RenderingEpoch()` from the graphics-pipeline bind cache. On devices requiring
`RequirePostBindDynamicState`, reopening a rendering scope in the same command buffer could then
skip both the guest pipeline bind and the required Qualcomm post-bind dynamic-state replay.
Two host paths independently violated the same cache-authority contract:

- `PostProcessingPass` and `FsrPass` issued direct host pipeline, descriptor and push-constant
  commands without publishing the corresponding scheduler epochs; post-processing also changed
  viewport/scissor without invalidating the tracked dynamic state;
- the late physical-DRS draw path could call `BeginRendering` after guest pipeline, vertex, index
  and dynamic-state binds instead of before them.

The narrow repair restores the rendering-epoch key only for
`RequirePostBindDynamicState`, publishes the exact state masks modified by post-processing and FSR,
invalidates dynamic state after the post-processing viewport/scissor commands, and orders late DRS
as `BeginRendering` followed by all guest binds. It deliberately does not invalidate all bindings
on every `EndRendering`/`BeginRendering`, so ordinary Vulkan state deduplication remains active.

SRT snapshot eviction was examined and rejected as the cause: snapshot payload, ranges and lookup
results are copied into their consumers, graphics pipelines are owned separately, no snapshot
pointer survives until the draw, and the crashing run reported no capacity eviction. Globally
disabling bind deduplication, serializing scopes or adding blanket barriers remains an isolation
probe, not an accepted fix.

### Acceptance status and prevention

The first repaired build (`698256bca4316cc9e2a15ed1903f019f891a6310`) reached correct gameplay,
survived beyond the earlier crash point and showed no rectangle/flash in its control frame. This is
only **likely fixed, pending repeated deterministic moving routes**: absence of a rare symptom in
one run is not proof. Acceptance also requires a positive debug invariant showing zero stale-cache
hits after host passes and scope reopens.

Any renderer helper that issues direct Vulkan state commands must publish every modified
pipeline/descriptor/push/dynamic/vertex/index epoch before returning. New host passes should use a
single mask-specific tracked scope (or an equivalent checked wrapper), so omission is a structural
error rather than a convention. Debug/self-test builds must reject guest draws whose cached state
predates a host mutation and must reject guest graphics binds recorded before the required rendering
scope is open. These checks stay off the release hot path.

## BufferCache watcher orphan after buffer retirement (2026-08-11)

Bloodborne stopped during startup with FPS 0 on a guest write to `0x243400048`. The page was a valid
guest RW mapping, but one generic write watcher remained: owner provenance was BufferCache,
`readWatchers=0`, and neither BufferCache nor TextureCache invalidation changed the count. This was
not a texture watcher, bad guest permission, stale-PTE shortcut, JIT-poll fault, or the earlier
same-page fault-generation race.

The exact lifecycle was: a successful buffer upload changed the MemoryTracker CPU state to clean and
installed its write watcher; garbage collection could then retire a buffer with no GPU-dirty download
range; `DeleteBuffer -> Unregister -> ChangeRegister<false>` removed only `page_table` and
`buffer_ranges`. MemoryTracker deliberately outlived that registry entry. A later guest fault reached
the old `BufferCache::InvalidateMemory` early return for `!IsRegionRegistered`, so the tracker never
returned the page to CPU-modified state and its watcher could never be released.

The repair calls `memory_tracker->MarkRegionAsCpuModified(device_addr, size)` before that early
return. This is owner- and state-aware: `IteratePages<false>` creates no tracker, and
`RegionManager::UpdateProtection` removes only the watcher implied by its own CPU/writeable bit
transition. Do not replace it with a raw watcher-count clear, blanket unprotect, or unconditional
cleanup in `Unregister`: overlap replacement deletes the old buffer before registering its successor
and must preserve GPU ownership across that handoff.

Required invariant: a guest CPU invalidation must reconcile MemoryTracker ownership even when no
Vulkan buffer currently intersects the range. Registry presence is an optimization lookup, not proof
that no cache watcher exists. Regression checks must keep the reconciliation before the unregistered
return and prove that the non-creating tracker path is used; a traced fault must finish with the
BufferCache watcher count decremented rather than being accepted by a permissive fault-handler bypass.

## FEX host-signal misclassification and stale JIT metadata (2026-08-12)

An early FEX signal-owner implementation classified a host PC by first trusting
`CurrentFrame->State.InlineJITBlockHeader`, applying only alignment and a 128 MiB distance bound, and
then reading the header/tail.  That is not a lifetime proof.  `x28 == CurrentFrame` identifies the
thread frame, but the stored header may be stale or forged; the first metadata load can itself fault
before any size check runs.  FEX's ordinary membership helper cannot be called from the POSIX handler
because it walks mutable `shared_ptr` and vector state.

The prevention contract is a fixed, process-resident executable-mapping registry with lock-free POD
publication.  A signal reader must prove the host PC and the entire immutable block interval belong
to committed pages of the same live generation before dereferencing metadata.  Proving only the
header and fixed tail is insufficient: FEX's later RIP reconstruction reads unchecked
`OffsetToRIPEntries`, `NumberOfRIPEntries` and 1/2/9/17-byte variable records.  The safe path validates
and decodes that table with an end pointer and checked signed accumulation while the registry read
lease is still held, then captures the exact guest RIP.  Releasing the lease in the handler and later
calling `RestoreRIPFromHostPC` leaves a real retire/unmap/UAF window.

Unmap/reuse publishes retirement first and waits for earlier readers outside the handler.  That wait
is bounded; failure to quiesce quarantines the mapping for process lifetime instead of deadlocking or
reusing an address with stale metadata.  The handler remains bounded and uses no STL, allocation,
mutexes, logging or virtual dispatch.

Do not implement this as a `mmap(PROT_EXEC)` hook alone.  FEX persistent code cache reserves a
`PROT_NONE` destination and later installs executable pages with direct fixed `mremap`; a hook-only
registry would work on cold JIT code and fail nondeterministically after a warm cache load.  Both
ordinary JIT buffers and finalized cached-code pages must share the same lifecycle protocol.
Likewise, registering every executable allocation is unsafe.  Dispatcher code is classified by its
own immutable bounds, temporary compilation and cache-finalization staging must never publish, a
shared JIT mapping publishes only its completed prefix after cache visibility, and a persistent
mapping publishes only finalized pages rather than its `PROT_NONE` reservation.

Related signal ABI traps rejected during the same review:

- copying stock FreeBSD `HASFPXSTATE` into the Orbis frame even though the modified FEX producer uses
  an inline 832-byte FP area and publishes only `HASSEGS|HASBASES`;
- advertising FreeBSD's `0xFFBF` MXCSR mask when pinned FEX preserves only `0xFFC0`;
- using non-lock-free C++ atomics in an async handler, which may enter an out-of-line runtime lock;
- retrying token or slot CAS indefinitely.  HLE leases now use unique saturating tokens, bounded
  contention and bounded pool traversal, exact token-CAS cleanup, depth 64, and fail closed on
  exhaustion rather than producing an FPS-0 spin.

A second independent race existed after an otherwise successful capture.  A single capture slot is
insufficient when one hardware fault interrupts another, but mutually masking SIGSEGV/SIGBUS/
SIGILL/SIGTRAP is not a valid repair: POSIX leaves a hardware-generated synchronous signal while
blocked undefined, and Linux automatically blocks the delivered signal unless `SA_NODEFER` is set.
The corrected design separates asynchronous wake signals and uses a bounded, preallocated nested
synchronous-capture stack.  A nested fault never re-enters mutable lookup or guest-memory emulation;
it redirects the complete outer AArch64 context to a normal-context failure landing or terminates
through a fixed emergency path.  FEX activation remains fail-closed until that protocol is present.
Deferred processing restores the exact pre-signal kernel mask before guest re-entry.  Regression
coverage includes alternating cross-kind faults, same-kind nesting, a signal during `Ready`, exact
restoration of unrelated bits, and a deliberate second guest fault after delivery resumes.

## Late rasterizer attach loses pre-existing GPU mappings (2026-08-13)

The first FEX-rendered Bloodborne frames were valid, but later draws became flat colours or retained
old contents.  The JIT backend rendered the same command stream correctly.  A draw-for-draw A/B
comparison excluded PM4 decoding, pipeline and shader selection, draw state, geometry, descriptors
and guest execution: at the first divergent draw, the guest bytes at `0x313100900` were identical,
while the JIT GPU backing matched them and the FEX GPU backing was zero.  The next two consumed
ranges, `0x313100b14` and `0x313100eb8`, showed the same stale-backing pattern.

Page-manager state identified the boundary.  The JIT page was `mapped=1 protected=1`; the FEX page
was `mapped=0 protected=0` despite a logical write watcher.  FEX had mapped the DMEM VMA before
Vulkan created the rasterizer, whereas JIT created the rasterizer first.  `MemoryManager::SetRasterizer`
previously attached the pointer and replayed only the executor libc mspace.  Existing concrete VMAs
therefore never passed through `MapRasterizerMemory -> Rasterizer::MapMemory -> PageManager::OnGpuMap`.
Without that GPU-map publication, the page was never write-protected and later guest writes could not
invalidate or upload the stale GPU copy.

The repair serializes rasterizer lifecycle changes with `unmap_mutex`, snapshots all existing mapped
VMAs under the memory mutex, and replays them after releasing that mutex.  PRT ranges are replayed
before concrete mappings, file VMAs are excluded, and the executor heap uses only its precise active
intersection.  `SetPrtArea` and executor-mspace growth use the same serialization.  Rasterizer map
and unmap interval mutations release their own range lock before calling PageManager, avoiding the
opposite lock order; destruction detaches the rasterizer only after scheduled GPU work is finished.

Required invariant: attaching a rasterizer after guest mappings already exist must produce the same
PageManager mapping/protection state as mapping those ranges after rasterizer creation.  A repeated
attach of the same pointer is a no-op, and detach must leave no callable stale pointer.  Do not repair
this with blanket uploads, barriers, per-title copies, or permissive fault handling: those hide the
missing lifecycle notification and either retain stale pages or add permanent hot-path work.

Acceptance used the same title and PM4 anchors.  After replay, all three ranges above reported exact
GPU-byte matches and `mapped=1 protected=1`; no later mismatch or fatal signal occurred.  A clean FEX
APK then rendered the warning, title menus, loading screen and Hunter's Dream gameplay correctly.
The same APK, explicitly switched to the JIT backend before process start, rendered the matching
warning screen without a regression.  Temporary watcher-owner and page-state diagnostics were
removed after the proof so invalidation retains no per-call environment lookup or debug counting.

## Early ASC consumption before lazy rasterizer creation (2026-08-13)

Bloodborne under FEX rendered valid menus but its indoor gameplay lighting was almost black.  The
same save and graphics path under the built-in JIT was lit correctly.  FEX could issue
`sceGnmDingDong` before the first graphics submit, while the Android presenter was still created
lazily.  `ProcessCompute` parsed the ASC ring and advanced its cursor with a null rasterizer, but
silently skipped DMA and dispatch operations.  This permanently discarded the title's one-shot
lighting initialization.

The old FEX run lacked compute programs `0x83450e2f` and `0xe2c27f0c`.  After creating the presenter
before consuming a non-empty DingDong range, the first 32 compute dispatches matched the JIT
sequence.  Storage outputs matched for 31 of 32 dispatches; the only expected difference was the
random 16x16x16 R8 noise volume.  The previously zero 1x1 RGBA8 value at `0x2d1b3a300` became the
exact JIT value `0xffffffff` with content hash `0x6efb487bec55167d`.  The repair also validates the
virtual queue id and resets the per-queue HLE cursor on map and unmap.

A guest GPU submission may become consumed only after an executable rasterizer exists, or after an
immutable snapshot has been durably retained for replay.  Before that point no ring read pointer,
HLE cursor, initialization flag, completion event, or guest-buffer lifetime may advance.  A
readiness failure must leave the submission byte-for-byte retryable; parsing PM4 state while
skipping raster operations is not a valid partial consume.

The same invariant must remain structural rather than limited to DingDong.  Graphics workload
submission, `SubmitDone`, and the public `SubmitGfx`/`SubmitAsc` boundaries must not gain a path that
consumes work after presenter creation failed.  If early work can arrive without a later doorbell,
retain it in a durable pending queue and drain it when the rasterizer attaches.
