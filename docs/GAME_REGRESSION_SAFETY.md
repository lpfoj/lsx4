# Game regression safety

This is a contribution guide and risk map, not a claim that every path below is currently defective. It supplements, and does not relax, [AGENTS.md](../AGENTS.md), the [playable/performance regression contract](PLAYABLE_PERFORMANCE_REGRESSION_PREVENTION_2026-08-31.md) and [PS5 isolation](ps5_desktop_isolation.md).

## What must stay true

Keep established gameplay, graphics, audio, input and save behavior while improving performance. A faster title screen is not evidence of faster gameplay. A successful launch is not a Playable result. Do not disable required work, synthesize completion or alter game state to pass a gate.

Measure the effective route on the final build. PS4 and PS5 share some source, but have different ABIs, memory policies and runtime libraries. Passing PS4 does not prove PS5, or the reverse.

## Sensitive boundaries

The locations are review entry points. Follow their callees and ownership contracts before making a change. Paths below `funnel-arm/` belong to the pinned submodule; include the reviewed submodule revision in a PR.

| Boundary and source | What can break | Rule and focused evidence |
| --- | --- | --- |
| JIT publication, `src/executor/dynamic_translation/{code_witness,region_registry,retiring_execution_core}.*` | Stale code after guest writes, invalid edges, use-after-retirement, cache reuse across incompatible builds | Preserve page revisions, code identity, fallback entries and retirement. Test mutation, eviction, guard failure and cold/warm cache. Change the artifact ABI/epoch when its contract changes. |
| HLE transitions, `hle_invocation_frame.*`, `machine_state.*`, `src/executor/ps5_desktop/runtime_api.*` | Stale guest registers, incorrect return values, stack arguments, flags, FP/SIMD state or TLS after a callback | Respect the ABI for each route. PS5 Current GPRs stay memory-authoritative until an explicit resident-state invalidation handshake replaces the guard. Cover nested/reentrant callbacks and positive/negative paths. See `tests/test_ps5_current_hle_gpr_coherency_contract.py`. |
| Guest memory, `src/executor/native_runtime_api.cpp`, Funnel `core/memory.*`, `video_core/page_manager.*` | Invalid host pointers admitted as guest memory, stale CPU reads, faults, races, overlapping mappings | Retain exact range leases, full-span validation, page ownership and invalidation. Unknown or partial spans remain rejected. Never make host callback memory globally readable to avoid a fault. |
| GPU ownership and publication, Funnel `buffer_cache`, `texture_cache`, `renderer_vulkan/vk_scheduler.*` | Stale or lost GPU output, incomplete CPU readback, early resource reuse, device loss | GPU-to-GPU consumers should use the correct resident generation. CPU observers must see completed data before completion is signaled. Defer waits only with explicit dependencies, lifetime and retirement. Test overlaps, aliases, partial ranges, eviction and queue boundaries. See `tests/test_vulkan_transfer_transaction_contract.py`. |
| Shaders and resource identity, Funnel `shader_recompiler`, `renderer_vulkan/vk_pipeline_cache.*` | Wrong texture formats, shader variants, resource bindings or geometry during animation | Cache keys must include semantic inputs and relevant capability/ABI state. Preserve borrowed/owned lifetimes. Inspect a moving character and a temporal frame series, not only a static frame. See the shader ownership and payload contract tests. |
| Vulkan capabilities, Funnel `renderer_vulkan/vk_capability_profile.h` | A fast path works on one driver but crashes or renders incorrectly on another | Check API/extension, enabled feature and entry points together. Keep complete legacy routes. Put driver quirks in the capability bridge, not scattered vendor/title checks. Run the bridge guard and validate legacy Mali plus current Adreno for Vulkan changes, as required by AGENTS.md. |
| AIO, file handles and savedata in Funnel HLE/core | Reused fd/slot, early completion, stuck batch tail, cancelled work writing into a later session, save loss | Hold handle leases through retirement; write result fields before terminal state; drain old sessions. Preserve close/cancel/error behavior and savedata commit semantics. Test failures as well as success. Use a disposable test slot only with permission; do not overwrite a personal save for validation. |
| Recursive locks, worker admission and Mono safepoints | Deadlocks, startup barriers that never finish, lost suspend/wake, incorrect reentrant ownership | Removing lock cost is not permission to change recursion semantics. Distinguish lifetime admission from core placement. Do not limit permanent worker cohorts to the number of big cores. Cover Mono and non-Mono paths. |
| Android settings, `CpuTuningSettings`, `GameProfileSettings`, `SettingsProvider` | A global preset silently disables a required title route; PS5 ignores a toggle; stale process snapshots | Check UI to resolver to JNI to effective native state. Named presets preserve hard GameDB constraints; Custom preserves explicit overrides. Test default, override, reset and platform isolation. |
| Session and audio lifecycle, `GameSessionProvider`, `MainActivity`, `GameActivity`, Funnel audio | Old runtime used after an update, duplicate sessions, silent audio after resume, device loss on surface recreation | Verify runtime identity and ownership. Do not kill the settings owner during handoff. Check stop/relaunch, background/resume and audio progress. Never treat a swapped library as proof that the running process loaded it. |
| Experimental markers and GameDB policy | A one-game experiment silently affects every game or survives into a release | Risky experiments are title-scoped and reversible. Record active markers in evidence, remove diagnostic overrides from final acceptance and keep effective configuration visible. Driver workarounds still belong in the capability bridge, not GameDB. |

Existing source-contract tests locate these risks; their presence does not prove runtime coverage. See also the historical incidents in the mandatory regression contract: AIO, Unity cohorts, Mono, Isaac callback stacks, Bloodborne preset routing, PS5 GPR coherency, marker leakage and recursive-lock ownership.

## Proportionate validation

| Change | Required scope |
| --- | --- |
| Documentation, screenshots, contribution templates | Check facts, links, counts and private-data exclusions. No game launch needed. |
| Android-only library/UI behavior | Relevant unit/source checks, app build if code changed and affected UI workflow on device. Confirm no game/profile data loss. |
| A title-specific setting | Resolver tests, that title's real gameplay and an unaffected control for the claimed isolation. Do not generalize a single-title success. |
| Shared CPU/JIT/HLE/memory/scheduler/profile code | Existing mandatory PS4 gameplay gates and the matched Bloodborne performance anchor, plus the PS5 Current pair when the shared route is affected. Keep both axes on one final build. |
| Shared renderer, synchronization or driver route | Focused unit/contract checks, both native builds, capability gates where applicable, affected moving scenes, temporal captures, matched performance anchors and relevant PS4/PS5 controls. Broaden coverage with the actual dependency surface. |

Do not run the entire catalog after every local edit. Batch compatible code review and focused checks, then test the final candidate once against the required matrix. Do not omit an AGENTS.md gate to save time. A release that changes a global default needs broader coverage than an isolated experiment.

## A regression-prevention flow

1. **Baseline:** Record commit, APK and both native-library hashes, package version, effective backend, title ID, device/driver, render resolution, settings, marker inventory, save/scene and matched measurement window. Keep a known-good artifact without replacing user data.
2. **Cause:** Name the violated contract or measured cost. Use existing traces first. Where needed, collect publication/wait/upload/cache/CPU/GPU counters together in one bounded run. Distinguish startup, menu and sustained gameplay. Low GPU utilization plus long waits is not proof that reducing resolution will help.
3. **Design:** Identify the producer, consumer, authoritative copy, synchronization point and lifetime. State what remains correct when a fast path is unavailable. Prefer a structural fix to more marker checks or title names in a generic renderer.
4. **Patch:** Change one causal mechanism in a reviewable commit. Related bottlenecks may be delivered together only when each dependency and fallback is explicit. Avoid combining profile/default changes with unrelated core rewrites.
5. **Checks:** Run focused positive, negative and boundary tests. Include partial overlap, capacity/eviction, cancellation, nested callbacks, reset/relaunch and capability fallback where relevant. Source-pattern tests alone cannot close a semantic bug.
6. **Final artifact:** Build from the reviewed revision and verify the installed hashes and loaded route. Exercise movement, camera/actions and audio with increasing counters. Collect a 20 to 30 second temporal series for rendering changes. Observe save/load when that contract is affected without modifying someone else's save.
7. **Compare:** Use the same scene, input, resolution, patches, driver, thermal range and instrumentation. Report sustained FPS and frame-time distribution, not the best screenshot. Separate external clock/thermal differences and measurement noise from a causal speedup. Any confirmed correctness or anchor-performance regression blocks acceptance.
8. **Review:** Attach the matrix for the same final APK. Missing hardware or game coverage is “not run”, not “passed”. Keep the patch unaccepted or the experiment off until its required gates pass.
9. **Release and recovery:** Preserve the known-good version and settings compatibility. Maintain an evidence manifest by artifact hash. If a regression appears, revert the smallest causal commit or restore the previous route, not unrelated proven fixes. Never erase saves or broaden permissions as a rollback shortcut.

## Suggested PR evidence

```text
Baseline commit / artifact:
Candidate commit / artifact:
Affected contract and platforms:
Requested route / effective route:
Settings, render resolution, driver, markers:
Title / save / scene / actions / test window:
Correctness, audio and temporal result:
Sustained FPS and frame-time comparison:
Focused checks and gameplay matrix:
Not run, reason and remaining risk:
Fallback / rollback:
```

Historical numbers in old reports are not current release targets for every scene or device. Use matched controls. Public compatibility status requires evidence review; a model output, local long-press mark or unverified community report is not sufficient by itself.
