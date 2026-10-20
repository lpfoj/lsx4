# Android optional CPU tuning

Performance evidence is valid only after `scripts/preflight-vivo-performance-test.ps1` passes.
See `PERFORMANCE_TEST_PREFLIGHT.md`; any run that skips or fails this gate is invalid.

## Scope and invariants

This page documents the optional CPU controls imported from
`ps4_android_optional_cpu_hacks_references.txt`. The Android screen exposes 37 logical setting
groups, represented by 38 persisted fields because the custom JIT feature mask and custom guest
thread delay each have a dependent value. A preset selector sits above those fields.

The implementation is deliberately process-immutable. Android resolves global, GameDB, and
per-title values, writes one complete `android-cpu-tuning-v1.ini`, and does so before loading the
native runtime. Native code validates, clamps, resolves every Auto value once, hashes the effective
profile, publishes it to funnel-arm, and copies the same 176-byte profile into the FEX adapter ABI.
Changing translator, memory, scheduling, or thread topology therefore takes effect on the next game
process, never halfway through execution.

These invariants are enforced in both Java and native code:

- FEX is only accepted with Fast/Unsafe host-mapped memory. Safe, Software, and Host-mapped-safe
  select the LSX4 IR core, which implements checked access.
- Ignore bad accesses requires both Unsafe and Host-mapped-unsafe and selects LSX4 IR, because FEX
  cannot reconstruct every guest-visible effect of a faulting x86 instruction.
- Fresh interpreter disables JIT-only block linking, large blocks, and persistent translation cache.
- Force real clock resets guest clock to 100% and disables cycle skip.
- A custom ISA disable mask only applies in Custom mode and can remove detected features, never add
  unsupported features.
- Rebuild has the same translation identity as On. It removes only the current title/profile cache
  namespace and then becomes On.
- GameDB values are defaults. Explicit selected-scope values override them; safety contracts are
  applied last.

## Implementation matrix

| Setting | Effective implementation | Correctness/performance contract |
|---|---|---|
| CPU execution mode | Selects FEX JIT, LSX4 IR JIT, cached decoded-block interpreter, or fresh-decode interpreter during backend activation. Interpreter paths disable tiered JIT; fresh mode also bypasses persistent and in-process translation reuse. | Safe-memory combinations are routed to LSX4 IR. This is a backend choice, not a label. |
| Fast memory | Controls whether LSX4 may emit direct guest-memory access sequences. Direct access is also withheld by Strict atomics or Accurate cache-line stores. | Unsafe does not by itself suppress faults; the separate bad-access option and unsafe manager are also required. |
| Memory manager | Software and Host-mapped-safe use checked LSX4 translation. Host-mapped and Host-mapped-unsafe are accepted by FEX and its direct guest address space. | Invalid FEX combinations fail adapter validation even if a stale launcher sends them. |
| JIT block linking | Independently enables FEX multiblock linking and LSX4 resolved-exit chaining. | Independent of block size: short Safe blocks may still link. Interpreter modes disable it. |
| Persistent translation cache | LSX4 stores generated artifacts in a profile/title namespace. FEX exposes an upstream default-OFF WIP native-code cache; Bloodborne GameDB leaves it Off to retain the proven D102 startup path. Experimental W36 RIP-seed prewarming remains a separate explicit default-OFF marker. | FEX never restores unvalidated host code. Rebuild is namespace-scoped. Diagnostics that require exact compilation disable FEX caching. |
| Function replacements / HLE | Off keeps only mandatory system ABI interposition. Safe permits proven HLE replacements. Aggressive additionally enables guarded fast libc and clean-buffer routes. | Resolver and call-site guards remain authoritative; Aggressive does not invent an implementation for an absent symbol. |
| Guest CPU clock | Scales guest-visible monotonic counters and FEX TSC/RDTSCP configuration. | Does not alter Android CPU frequency. Force real clock overrides it to 100%. |
| Cycle skip | Adds a deterministic fraction to guest-visible scheduler time using divisors 16, 8, or 4. | No guest instruction is randomly dropped. Force real clock disables it. |
| Idle-loop skipping | Recognized idle waits park/yield for bounded intervals in kernel timing paths and JIT poll paths. | Safe uses shorter bounds; Aggressive permits longer parks. Only recognized wait shapes are changed. |
| Wait-loop detection | LSX4 tracks the guest page being polled and uses its write event; kernel equeue waits use bounded parking policy. FEX receives the equivalent wait-loop configuration. | A write wakes the waiter. Timeout bounds prevent a missed classifier from becoming an indefinite sleep. |
| Spin-loop detection | Changes x86 PAUSE lowering: no-op, ARM yield hint, or stronger backoff (scheduler yield in LSX4 and two architectural yields in FEX). | It never removes non-PAUSE loop instructions or their memory accesses. |
| Maximum guest workers | Named worker/job threads enter a condition-variable admission gate after creation and initial resume; exit removes them and wakes one waiter. | Main, render, audio, and service threads are not capped. The option does not fake `pthread_create` failure. Auto resolves to 2–6. Title profiles must keep complete persistent barrier cohorts runnable; Bloodborne therefore uses six slots for its five Havok workers. |
| Event timing | Accurate keeps normal deadlines. Fast routes short timers through the short-wait path. Instant only collapses bounded service timers classified safe. | Long and unclassified guest deadlines remain timed. |
| JIT block size | Changes real decode/trace limits: Safe 256, Mega 512, Giga 5000 instructions in FEX, with corresponding LSX4 region limits. Safe preserves the device-proven Bloodborne D102 geometry. | Larger blocks reduce dispatches but increase compile time and invalidation span. |
| Floating-point accuracy | LSX4 controls estimate/fusion lowering. FEX Accurate keeps x87 FP80, Approximate uses FP64/53-bit precision, and Relaxed quantizes each x87 stack write through FP32 while retaining an FP64 physical stack. | Relaxed is observably different from Approximate; it is not a duplicate enum value. |
| Atomics / reservations | LSX4 chooses seq-cst, acquire/release-compatible, or relaxed fences while preserving atomic RMW. FEX keeps software TSO, varies vector/memcpy TSO relaxation, and enables strict split-lock handling in Strict mode. | Atomicity remains intact in every mode. Fast weakens ordering only; Accurate stores can retain stronger paths. |
| Accurate cache-line stores | Adds strong barriers around relevant LSX4 store and atomic paths and prevents relaxed direct-memory/FEX vector-store routes. | Intended for titles that depend on cross-thread cache-line visibility. |
| Accurate DMA / transfer timing | Liverpool flushes deferred transfer work at guest-visible DMA boundaries and records a proof event. | Off may batch transfers further; On preserves ordering at extra CPU/GPU synchronization cost. |
| Memory exceptions | LSX4 delivers mapped faults through the guest exception path; FEX keeps signal/SMC protection appropriate to the selected policy. | Off makes an unexpected non-opted-in fault fatal; it does not fabricate success. |
| Instruction-cache accuracy | LSX4 compares live instruction bytes in addition to page invalidation. FEX selects full, mtrack, or no SMC checks according to the complete memory/fault profile. | Off retains required page invalidation but removes sparse byte auditing. |
| Ignore invalid accesses | LSX4 replays the exact decoded instruction with invalid loads returning zero and invalid stores discarded, with a recursion guard and proof counter. | Available only in the explicit Unsafe + Host-mapped-unsafe pair; forces LSX4 IR. |
| JIT host feature mask | Compatibility and Custom clear detected ARM64 feature bits before either LSX4 or FEX initializes. Custom has an 18-bit dependent disable mask. | Masking can only disable LSE/FlagM/RCPC/SVE/crypto/etc.; it cannot claim unsupported hardware or OS context support. |
| CPU/GPU threading | Single serializes CPU submission with GPU completion. Dual permits producer/consumer overlap. Auto resolves from logical topology. | This controls real Liverpool submit paths, not only thread creation. |
| Thread scheduler | OS is a true no-op. Emulator applies role-based masks. Performance uses the configured fastest set for primary/compute work while retaining separate latency and graphics roles. Auto only uses role placement on a usable heterogeneous topology. | All masks are intersected with Android's current allowed cpuset; failed affinity never broadens permissions. |
| Preferred heavy threads | Selects the highest-scored eligible CPUs by `cpu_capacity`, then max-frequency fallbacks. | Applies to primary and compute/JIT roles. Auto resolves to 1–4; the UI permits up to 8. |
| Subsystem multithreading | Master policy for optional AIO, audio output, and AJM workers. | Mandatory kernel/service threads remain. Off runs optional jobs inline with the same completion state. |
| Separate I/O thread | Real AIO requests are copied into owned descriptors, queued to a `jthread`, and completed through atomic state plus condition-variable wakeups; Off executes the same job inline. | Null/zero timeout preserves the prior unbounded-wait ABI. Cancellation uses compare/exchange and cannot overwrite a completed request. |
| Separate audio/HLE thread | Audio output and AJM dispatch use dedicated workers when enabled and execute the same work inline when disabled. | Locks are acquired before completion notification so inline mode cannot recursively deadlock. |
| Guest thread delay | Auto derives a bounded delay from worker oversubscription; Custom applies the exact microseconds to recognized worker/job starts. | Main/render/audio/system threads are excluded. The dependent value is clamped to 10,000 µs. |
| Synchronize after idle skip | Issues a full memory fence after an idle park before guest execution resumes. | Useful with strict producer/consumer polling; Off avoids the fence. |
| CPU/GPU synchronization | Accurate waits every graphics, compute, and command submit. Adaptive preserves asynchronous overlap while bounding each path with a 64/32 high/low watermark. Relaxed adds no CPU-side backpressure. Retirement notifications wake bounded producers. | The hysteresis prevents Adaptive from becoming a per-submit rendezvous; this is distinct from Single/Dual, which controls topology rather than queue depth. |
| Force real-clock synchronization | Uses unscaled monotonic time, 100% guest clock, and no cycle skip in LSX4, funnel, and FEX. | Overrides conflicting persisted fields during normalization. |
| System module mode | Prefer HLE checks real host implementations before guest exports. Auto checks guest/LLE first and then HLE. Prefer LLE refuses optional HLE substitution when the guest module supplies the symbol. | Mandatory executor ABI interposers remain ahead of this preference. Missing implementations always fall through safely. |
| HLE timer mode | Accurate sleeps to deadline. Fast yield-waits for bounded short sleeps. Aggressive collapses only classified short HLE waits to a yield. | Force real clock prevents timer shortcuts that would alter real-time semantics. |
| Per-game CPU profile | Global ignores title fields. Game requires the title enable switch. Auto discovers an existing enabled/overridden title scope. | Title ID is normalized from CUSA/PPSA launch paths before materialization. |
| CPU presets | Compatibility, Balanced, Performance, and Extreme write complete profiles; Custom is set after any individual edit. | Extreme enables unsafe mapping but still does not silently enable Ignore bad accesses. |
| GameDB policy | Supplies conservative known-title defaults for worker count, scheduling, event/block/atomic policy, and CPU/GPU sync. Bloodborne keeps scheduler mode `OS`: on Vivo, a matched 12-second A/B measured 28.1 FPS with four-core Performance affinity versus 34.0 FPS with Android scheduling, despite lower clocks in the latter run. | Applied before explicit scope values. It cannot defeat final safety normalization. OEM game-performance mode is independent from emulator thread affinity. |

## Proof and observability

The CPU page exposes a default-Off **Override for all games** checkbox. Enabling
it snapshots the current Global selection into `cpu.all_games.*`, then routes
every title to that shared profile. A multi-select exception picker is populated
from the actual library titles and is empty by default; product codes are never
used as its visible labels. An excepted title falls through to its enabled
individual CPU profile and then to Global/GameDB values. Individual profiles are
edited from the burger on that game's library row. Editing or enabling one while
the shared route is active automatically adds that title to the exceptions, so
the edit cannot be silently shadowed. Disabling the checkbox only removes the
routing flag: Global and individual profiles are preserved. The flag, exception
set, preset, and every shared field cross the launcher-to-game process snapshot.

The PS4 HUD/proof ABI contains the full effective CPU profile at indices 114–154 and live action
counters at 155–165. The counters cover idle yields, wait parks, spin parks, synthetic cycles,
ignored accesses, fast/accurate HLE calls, DMA boundaries, delayed guest threads, worker admission
parks, and CPU/GPU synchronization. Consequently `Active` in settings can be checked against both
the materialized selection and a mechanism-specific counter; a selector alone is not treated as
proof of work.

The FEX profile ABI is fixed at 176 bytes with an explicit zero `reserved0` word at offset 164 and
`identity` at offset 168. Making the alignment slot explicit prevents profile identities from
depending on indeterminate C++ padding. Both the LSX4 loader and adapter validate its size,
version, identity, enum ranges, reserved word, and backend/memory compatibility.

## Upstream theory and implementation references

The reference list was used as a taxonomy, not copied as a promise that differently named controls
share identical semantics. The implementation was compared with these primary upstream projects:

- [FEX-Emu](https://github.com/FEX-Emu/FEX) for block linking, block geometry, x87 precision,
  software TSO, SMC checks, code-cache constraints, TSC scaling, and AArch64 host feature gating.
- [PPSSPP](https://github.com/hrydgard/ppsspp) for JIT/interpreter selection, fast-memory safety,
  block linking, and per-game configuration patterns.
- [RPCS3](https://github.com/RPCS3/rpcs3) for decoder/recompiler choices, preferred SPU threads,
  scheduler/loop detection, block size, accurate cache/DMA controls, and per-title configuration.
- [PCSX2](https://github.com/PCSX2/pcsx2) for deterministic cycle-rate/skip concepts and the rule
  that speed controls must declare their compatibility risk.
- [Dolphin](https://github.com/dolphin-emu/dolphin) for CPU core selection, fast memory, idle skip,
  thread separation, and game-specific overrides.
- [Ryujinx](https://github.com/Ryujinx/Ryujinx) for host-mapped versus software memory-manager
  contracts and memory-aliasing trade-offs.
- [Azahar](https://github.com/azahar-emu/azahar) for guest CPU clock configuration and per-game
  settings behavior.
- [Linux kernel memory-barrier documentation](https://docs.kernel.org/core-api/wrappers/memory-barriers.html)
  and [`sched_setaffinity(2)`](https://man7.org/linux/man-pages/man2/sched_setaffinity.2.html) for
  atomic ordering, fences, and the requirement to respect Android/Linux cpuset restrictions.

## Verification gates

`lsx4_cpu_tuning_profile_contract` validates profile layout, presets, unsafe combinations, Auto
resolution, cache identity, parser bounds, and malformed input. `test_android_cpu_tuning_contract.py`
statically verifies the complete UI-to-runtime path, real worker/scheduler/GPU/AIO/HLE mechanisms,
FEX controls, ABI/HUD tail, and isolated build routing. These gates complement the existing native
and Android builds; they do not substitute for later on-device correctness testing.
