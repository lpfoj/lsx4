# Optional FEX guest backend: contract matrix

> [!IMPORTANT]
> Before any device/FPS/playable measurement, run `scripts/preflight-vivo-performance-test.ps1`;
> stale Android Studio process-trackers and known profiler loads make the result invalid. See the
> mandatory [performance-test preflight](PERFORMANCE_TEST_PREFLIGHT.md).

Date: 2026-08-11
Last proof audit: 2026-08-12
Status: implementation gate, not a compatibility claim
Default/evidence-backed backend: `current-aarch64-jit`

The concrete upstream interface request for the three remaining activation blockers is
[`FEX_UPSTREAM_HOOK_REQUEST_2026-08-12.md`](FEX_UPSTREAM_HOOK_REQUEST_2026-08-12.md).  A real
API-26 AArch64 staged smoke now compiles and executes a mapped x86-64 fragment through the pinned
`libFEXCore.so`; this proves the basic Android JIT route, not the PS4 CPU, signal-lifetime or
transactional-memory contracts.  Production remains fail-closed until the exact CPU-profile probe
and both signal/memory gates in that request pass.

Normative/research inputs for the memory and vCPU sections are the AMD64 APM and family BKDG/CPUID
specification, Intel SDM, Arm A-profile architecture and LSE supplements, Linux HWCAP contract, the
current herdtools7 x86/AArch64 models, RISOTTO/Armed Cats follow-up work and the 2025 dissertation
*Correct Translation between Weak Memory Model Architectures*.  Emulator source and finite device
runs are corroborating evidence only.  When those sources model different abstraction levels, the
document records the discrepancy instead of silently choosing the cheaper behavior.

Formal results are pinned to herdtools7 commit
`4fb4cab7ea425f88b364d17cca92f8435e4056a2`; both `aarch64.cat` and the instruction semantics are
part of the pin.  Updating either file invalidates cached outcomes until the positive cases and all
intentional weak mutants have been rerun.  This matters because the 2022 Arm-model correction
strengthened successful acquire-release AMOs but deliberately did not turn a failed/no-write CAS
into a release operation.

This document is the permanent boundary contract for the optional FEX guest CPU backend.  A
preference is not proof that FEX ran.  Every run must report native `requested`, `effective`,
`supported`, `fallbackReason`, adapter/build identity and cache namespace.  If requested and
effective differ, guest launch is blocked; a silent Current fallback is forbidden.

### Evidence ledger

The labels below are part of the gate.  A claim cannot silently move upward because an emulator uses
the same technique or a phone survived a stress run.

| Status | Meaning | Current examples |
|---|---|---|
| `PRIMARY-PROVEN` | Exact selected-profile behavior is supported by a pinned official ISA/ABI/kernel contract | Android/AAPCS use of `x18`, `x16/x17`, `x30` and v8-v15; AMD same-value writeback on failed CMPXCHG; AMD ordinary 1/2/4/8-byte atomicity envelope; Linux HWCAP discovery rule |
| `CODE-PROVEN` | Exact behavior of the present LSX4/FEX/Box64 revision was inspected, but that behavior is not itself an ISA proof | conditional 15-GPR static-chain ABI, entry-side publication into non-aware bodies, nearest-previous fault marker, raw generated `BLR` inventory |
| `MODEL-UNEXECUTED` | The proposed contract is plausible and has an executable proof plan, but its formal/native gate has not run | x86-TSO-to-AArch64 outcome refinement, failed-CAS contextual ordering, mixed-size container protocols |
| `SOURCE-UNRESOLVED` | Wording, profile or exact primary clause is still incomplete or disputed | promotion-grade LSE2 memory-attribute/shareability envelope; exception-priority collisions not explicit in the AMD manuals |
| `REJECTED` | A finite test, another emulator or a performance result was incorrectly being treated as proof | nearest-previous fault attribution, helper safety inferred from decoded operands, resident-block count used as aggregate-work evidence |

`MODEL-UNEXECUTED` and `SOURCE-UNRESOLVED` paths are disabled.  Missing herdtools7, a native
selected-family AMD fixture or an actual-byte Arm runner is a failed prerequisite, never a skipped
green test.  Every implementation change must cite a row in this document, preserve the evidence
label in its test manifest and be independently removable before promotion.

Pinned primary/theory references for future audits:

- [AMD64 Architecture Programmer's Manual Volume 1](https://docs.amd.com/api/khub/documents/sfvvekC9mDflu6vd3R0NXA/content)
  and [Volume 3](https://docs.amd.com/api/khub/documents/68GKiN0gMEd6bMddsmhPwg/content);
- [Arm Architecture Reference Manual for A-profile](https://developer.arm.com/documentation/ddi0487/latest/),
  [Issue L.a corrections including D23529/D23778](https://documentation-service.arm.com/static/67f9108c725ab804652e87d0)
  and [Linux arm64 HWCAP ABI](https://docs.kernel.org/arch/arm64/elf_hwcaps.html);
- [AAPCS64](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst) and
  [Android ShadowCallStack/x18 contract](https://source.android.com/docs/security/test/shadow-call-stack);
- [herdtools7 pinned revision](https://github.com/herd/herdtools7/commit/4fb4cab7ea425f88b364d17cca92f8435e4056a2),
  [LLVM StackMaps](https://llvm.org/docs/StackMaps.html),
  [LLVM Statepoints](https://llvm.org/docs/Statepoints.html) and
  [QEMU TCG helper contracts](https://www.qemu.org/docs/master/devel/tcg-ops.html).

## Provenance

- Historical LSX4 FEX semantic baseline: `da603db1563909466b0bb6b93f09af2fc0ff7e4c`.
- Current-only behavior entered at `638b1ed3ae822e931e3d9fbaa4b7eda49f6e5804`.
- Archived patched FEX base: `1240a00fa50b48c1b5e0667e2281ba25e08fbd47` with 16 dirty
  integration/JIT/signal files.  It is not a reproducible production input until captured as a
  reviewed patch series with a recorded patch hash.
- Historical Android pair:
  - `libexecutor_fexcore_embed.so`, 2,869,472 bytes,
    SHA-256 `22E2FFFA50EC54B53530A526B80F5C2505089BD05DBBFCE56D93A2760560C602`;
  - `libFEXCore.so`, 28,296,880 bytes,
    SHA-256 `3D703E1EB89CDA4FEE33A631850ECFB21E841E063BC0F37EEEA41E14BBD77DC`.
- Those binaries target Android API 29 and the embed has an incorrectly escaped literal
  `$$ORIGIN` RUNPATH.  They are diagnostic evidence, not release artifacts.
- A clean community build proves upstream FEXCore can compile with NDK 27.2/API 26, but produces
  only `libFEXCore.a`; it does not contain the PS4 adapter, signal or HLE integration.
- Production upstream pin: annotated release `FEX-2608`, commit
  `e869aa644a16e4332cdc15c1ea0b4d13d482385d` (tag object
  `cc88edf084bee3ae218b96218aaa9425565cf15a`). Do not build moving `main`.
- Reproducible toolchain pin: NDK `29.0.14206865` (Clang 21), CMake 3.22.1, Ninja,
  `arm64-v8a`, `ANDROID_PLATFORM=android-26`, C++20 and `TUNE_CPU=none`. Selecting the highest
  installed NDK or compiling the adapter at API 29 is forbidden.

There is no retained proof of FEX reaching Bloodborne gameplay.  Historic evidence is limited to
micro-smoke and partial ItemzFlow/Fluster milestones.  Performance claims require a new same-scene
aggregate benchmark after all gates below pass.

### Archived patch disposition

The archived 16-file dirty delta is evidence, not a production patch stack:

- retain/rewrite only the Android minimal shared-core build, allocator hooks, ART old-handler
  chaining, Orbis signal context/frame handling and single-owner integration;
- redesign the large signal-delegator, thread-pool cap and PS4 guest-frame changes around
  signal-safe fixed-layout sidecars, configured O(1) lifetime management and the v3 ABI;
- drop the global `PreserveAll_SRA` expansion, 512K reverse-map ring, return-predictor disable,
  block-link disable, BranchHint removal and hot-path file-marker probes;
- never carry the full Android LinuxEmulation target.

FEX-2608 already contains `c4c69ca8d` (CAS/CASP non-JIT SIGBUS), `538a9624e`
(zero-register unaligned atomics) and `762932354` (XMM/SRA signal spill). Return prediction and
block linking remain enabled and are correctness-gated rather than silently disabled. Performance
measurements of the archived fork are therefore not evidence for the modern backend.

## Immutable selection/lifecycle rules

1. `Current=0` is the safe serialized default; `FexExperimental=1` is explicit opt-in.
2. Selection is copied to the `:game` process and sanitized there.  It is latched before runtime
   initialization, module mapping, relocation, HLE thunk creation, surface attach or signal setup.
3. Hot switching and `dlclose` are forbidden.  FEX process-global context and signal state remain
   loaded until `:game` exits.  Changing backend restarts `:game`.
4. FEX is published as effective only after manifest/hash check, ABI/capability negotiation,
   callback installation, explicit activation and signal-owner verification all succeed.
5. Failure before activation leaves Current untouched.  Failure after activation terminates the
   `:game` process; it must never fall back to Current in-process.
6. Packaging FEX must not load it on the Current path.  A Current canary must show no FEX constructor,
   init or signal marker even when both libraries are inside the APK.
7. PS5 remains Current until a separate PS5 contract exists; its status is
   `platform-not-implemented` for FEX.

## ABI/behavior matrix

| Boundary | Historical adapter | Required contract before activation |
|---|---|---|
| Configure ABI | string symbols only | sized/versioned function table, build ID, capability versions and exact function pointers |
| Mapped entry | v4, 1,656-byte request, inline 64 segments | v5 pointer/count/stride/generation or bulk range API; v4 only if safely coalesced to at most 64 |
| Range readers | mutable fixed array | atomic raw pointer to immutable sorted POD snapshot; signal handler only acquire-loads and binary-searches; retired snapshots remain alive until quiescence/process exit |
| Range updates | registrar aliases can validate without registering | bulk replace/add/invalidate has observable semantic effect and is tested at 0/1/64/65/1000 ranges |
| HLE identity | exact ASCII `executor:ps4:hle:fexcore:thunk01` | negotiated exact bytes; Current identity and cache ABI are never changed |
| Integer HLE | six GP args plus guest RSP | explicit SysV INTEGER classification and RAX/RDX result |
| SIMD HLE | XMM arguments are passed as zero in archived paths; only low64 XMM0-3 callback shape | full 128-bit XMM0-7 call frame, XMM0/XMM1 result, SSE/SSEUP/MEMORY classes, stack and sret metadata |
| Unknown HLE ABI | silently treated as integer | pre-launch resolved-import scan; unknown descriptor rejects FEX launch |
| TLS | adapter sets FS and GS from `tcb_base` and ignores some image fields | LSX4 is documented TCB owner; accepted/ignored fields are versioned and tested per guest thread |
| Guest callbacks | only valid while active thread TLS exists | explicit reentrant callback capability; outside active FEX thread returns a typed error |
| Signals | FEX installs a delegator; Current may later reclaim SIGBUS | one immutable native owner; Current handler/watchdog/death catcher are gated off for FEX; ownership is verified before and after 60 seconds |
| Host-PC classification | FEX metadata is reachable through mutable `shared_ptr`/vector state | a process-resident POD executable-range registry is acquire-read in the handler; `InlineJITBlockHeader` and its tail are never dereferenced before the whole metadata interval and host PC are proven inside one live published mapping |
| Register state | GP state only, `old_ip` discrepancy | sized state with normalized RIP/old RIP, flags and full vector state where observable |
| Timeout | request field ignored | explicit unsupported capability or implemented watchdog/cancellation; never an infinite join advertised as timeout-aware |
| Teardown | no global shutdown | process-lifetime capability; no unload or in-process fallback |
| Strong memory | FEX TSO config exists, but archived host feature code forced RCPC | feature detection from `getauxval`, never CPU-name inference; conservative fallback paths on old/unknown Android |
| SMC | configurable FEX checks | selected mode is in capability/status/cache identity; executable invalidation is connected to LSX4 mappings |
| Cache | historic FEX paths are ad hoc | separate `fex-v3/...` tree including adapter/FEX patch IDs, host features, API, title/exe, thunk identity, strongmem and SMC modes |

The implemented boundary header is `src/executor/fex_backend_abi_v3.h`: full 128-bit XMM0-7
HLE/callback arguments, RAX/RDX and XMM0/XMM1 results, explicit sret/class metadata, dynamic v5
ranges, configure/query/one-shot activate, signal/context/TLS/thread operations and no deactivate.
Its layout tests compile as strict C11 and C++20 for AArch64 Android API 26.

### Internal profile and range-registry status

The Current PS4 CPU contract is now represented by one immutable
`lsx4-ps4-synthetic-v1` profile.  It serializes all basic leaves through 7 and all extended leaves
through `0x80000008`, including explicit zero leaves, plus the existing XCR0, MXCSR, address-width
and alignment policy.  A deterministic 128-bit identity covers every serialized field; validation
rejects incomplete leaf ranges, policy contradictions and an identity mismatch.  Current CPUID reads
that profile, and `QueryGuestXcr` supplies its canonical XGETBV policy for the pending core/ABI glue.
Passing the complete profile to FEX remains an activation blocker until the append-only guest-CPU ABI
extension is merged; host HWCAP is never profile input.

The internal dynamic executable-range registry now models the v5 lifecycle without changing the
published ABI layout.  Updates validate overflow, flags, unique cookies, ordering and overlap before
publication.  Signal readers acquire-load an immutable sorted POD snapshot and binary-search it;
every published snapshot remains resident.  Generation checks reject stale writers.  An SMC or
retirement update first makes the mapping epoch odd so existing leases become stale and new signal
readers fail closed, calls the translation-invalidation sink exactly once for the normalized spans,
then publishes registrations and restores an even epoch.  Failed retirement never publishes the
candidate snapshot.  Focused tests cover 0/1/64/65/1000 ranges, stale generations, invalid layouts,
concurrent signal readers and SMC/removal while a lease exists.  Wiring this registry to the external
FEX v5 callbacks is deferred to the lifecycle-owner merge.

## Memory-model gate

The x86 TSO contract cannot be replaced by blanket `DMB` or by unconditional acquire/release.  The
adapter and FEX JIT need separately proven paths for ordinary loads/stores, explicit fences,
LOCK/atomic instructions, unaligned atomics, SMC/page protection and signal emulation.  Pinned FEX
is implementation evidence, not the proof: its scalar TSO and compare-exchange sequences must pass
the same formal, native-oracle and fault gates as a new LSX4 lowering.

Feature dispatch is based on one cached `getauxval(AT_HWCAP/AT_HWCAP2)` snapshot:

- LSE atomics only with `HWCAP_ATOMICS`;
- RCpc/`LDAPR` only with `HWCAP_LRCPC`;
- newer RCpc forms only with the matching HWCAP2 bit;
- unknown macros, unavailable auxv or old Android means `false`;
- generated-code cache identity includes every ISA/memory-model feature bit.

The archived unconditional `features.SupportsRCPC = true` is forbidden: it can emit `LDAPR` and
cause `SIGILL` on unsupported ARM CPUs.  Equivalent unconditional AES/SHA/CRC/PMULL assumptions must
also be removed or proven by HWCAP.

## Host-signal and JIT-mapping lifetime gate

`x28 == CurrentFrame` proves the interrupted thread/frame association; it does **not** prove that
`CPUState::InlineJITBlockHeader` is current, mapped or safe to dereference.  FEX itself requires an
address-in-code-buffer check before reading JIT metadata, while its normal checker traverses mutable
`shared_ptr` and vector state and is therefore not async-signal-safe.  The adapter must instead use
a fixed-layout, process-resident registry whose atomics are compile-time lock-free.  Handler readers
perform only bounded acquire loads, checked integer arithmetic and POD reads, save/restore `errno`,
and return not-handled on an unpublished, retiring, overflowed or malformed range.

Publication covers both executable-memory paths:

- ordinary shared JIT buffers allocated RWX by FEX, with only the committed prefix published after
  block bytes, metadata and instruction-cache visibility are complete; dispatcher and temporary
  compilation/staging RWX mappings are different kinds and are never accepted as JIT metadata;
- persistent-cache buffers reserved `PROT_NONE` and made executable page-by-page by the direct
  `mremap(MREMAP_FIXED | MREMAP_DONTUNMAP)` finalization path.

Intercepting only `mmap(PROT_EXEC)` is insufficient: it silently rejects valid signals after a warm
persistent-cache load.  Publishing a whole RWX reservation is also invalid: it would accept unwritten
JIT tail space, the guard page and executable staging buffers.  A cached mapping publishes only page
intervals that have been relocated, remapped and made instruction-cache coherent.  A candidate block
is accepted only when its entire `[header, header + tail.Size)` interval is contained in committed
pages of one live generation.

Retirement is published before unmap/reuse and cannot complete until all pre-retirement signal
readers have left.  The reader lifetime extends through exact guest-RIP reconstruction: ending it at
signal-handler return leaves a deferred `RestoreRIPFromHostPC` unmap/UAF window.  Prefer reconstructing
and capturing the guest RIP inside the handler while the lease is held; otherwise the lease must be
transferred explicitly to deferred processing and released on every exit.  The decoder is bounded by
the proven block end: `OffsetToRIPEntries`, `NumberOfRIPEntries`, each 1/2/9/17-byte `vl64pair`, and all
signed host/guest accumulations use checked arithmetic.  It must reject reserved 32/64-bit tag bits,
truncated entries, non-progressing/backward host offsets and a cursor/count that cannot fit.

Registry overflow, generation exhaustion and partial publish/retire fail closed; no handler may call
STL, allocator, mutex, logging or virtual dispatch.  A bounded retirement timeout quarantines the
mapping instead of unmapping/reusing it or waiting forever.
Mandatory negative tests include forged/unmapped/aligned headers, stale mappings, partial cached
pages, concurrent publish/retire, slot exhaustion, integer overflow and a host helper PC with an
otherwise valid frame register.  Tests also race retirement after capture but before deferred signal
processing, exercise cold and warm persistent-cache pages, and reject executable staging mappings.

### Synchronous-fault exclusion and unaligned atomic emulation

The outer POSIX handler must not dereference the guest atomic operand.  Linux and POSIX explicitly
leave the result undefined when a hardware-generated `SIGBUS`, `SIGSEGV`, `SIGILL` or `SIGFPE`
occurs while that signal is blocked.  FEX's current unaligned helper can touch the operand, enter a
process-wide split-lock mutex, retry an atomic loop and log an error.  Calling it while the complete
owned-fault set is masked therefore permits three independent FPS-0/crash classes: a second fault at
an inaccessible page boundary, an unbounded wait on a lock whose owner is parked, and a
non-async-signal-safe logging/runtime call.  Exact JIT metadata and a read lease prove code lifetime;
they do not prove operand accessibility or helper progress.

The production path must use one of these proven alternatives:

1. a kernel-assisted unaligned-atomic facility whose exact `prctl` is successfully probed at runtime
   and whose semantics match the requested x86 operation; an unknown kernel or `EINVAL` is
   unsupported, never inferred from HWCAP or a kernel-name heuristic;
2. a signal trampoline that copies the complete AArch64 integer, SP, PSTATE and vector context to
   process-resident storage, returns from the kernel handler, performs emulation with synchronous
   faults unmasked, and has a fixed failure landing that converts an operand fault into the original
   guest fault before re-entering FEX;
3. site feedback followed by invalidation and recompilation to an alignment-aware lowering, with the
   first occurrence still handled by one of the two safe mechanisms above.

The future capture/trampoline path has a finite, preallocated per-thread nesting protocol.  The
kernel handler is installed with `SA_SIGINFO|SA_ONSTACK|SA_NODEFER`; none of SIGBUS/SIGSEGV/SIGILL/
SIGFPE may appear in its `sa_mask`, because POSIX makes a hardware instance generated while blocked
undefined.  The handler validates metadata under a read lease, copies the complete integer, SP,
PSTATE, FPSIMD/SVE state and signal identity into the next fixed TLS slot, rewrites only the saved PC
to a normal-context landing, and returns through `sigreturn`.  It performs no operand access,
allocation, lock, log, STL or virtual dispatch.

The landing runs after the kernel has restored the normal signal mask, consumes one owned slot and
may safely perform the checked operation or synthesize the exact guest fault.  A second hardware
fault during that operation uses the next slot and lands in a fixed failure continuation; it never
re-enters the same helper recursively.  Depth exhaustion, malformed context or a fault while the
registry lease cannot be acquired chains to the previous/default action without modifying guest
state.  Alternate-stack size has a measured upper bound for the chosen depth and guard pages;
`SS_AUTODISARM` is not treated as a substitute for explicit nesting ownership.

Returning `handled` without completing the atomic is forbidden.  Returning to the same faulting PC
without invalidating/recompiling is also forbidden because it creates an FPS-0 fault loop.  A
temporary fail-closed gate may reject the optional FEX backend before launch; it may not crash the
process after activation or silently route the operation through Current.

The AArch64 Linux signal ABI stores `x0..x30` in `regs[31]` and stores SP in a separate field.
Consequently an instruction field equal to 31 is never a valid index into the `regs` array.  Every
decoded atomic form must distinguish ZR/SP according to that instruction's architecture, reject an
unsupported SP base, reject a low pair that is odd or whose `low + 1` exceeds 30, and reject every
temporary/source alias not emitted by the exact pinned generator.  The 13-instruction CAS-pair
recognizer is a generator signature, not a generic pattern: saved NZCV, loaded/status temporaries,
address and value pairs must match the producer's fixed non-aliasing allocation.  An unaligned
128-bit fallback that can publish one half before the other is not an implementation of x86
`CMPXCHG16B`; it must fault or use a proven all-or-nothing primitive.

Required adversarial cases include an operand ending at every byte of a readable page followed by
`PROT_NONE`, `Rn=31`, expected/source register 31, odd and terminal CASP pairs, every forbidden
alias, split-lock contention, a park signal during emulation, and a deliberately failing second
memory access.  Each rejection must leave code, host context and all previously published registry
entries unchanged.

### Precise guest-exception classifier

Host `SIGSEGV`/`SIGBUS` is transport evidence, not an x86 exception.  One common instruction-priority
state machine must first determine instruction/feature legality and execution-state exceptions, then
classify the selected segment/address, architecturally attempted access range, mandatory instruction alignment,
ordinary alignment-check state, page permissions and access intent under the selected vCPU profile.
Its immutable descriptor contains the guest vector (including `#UD`, `#NM`, SIMD floating-point
exceptions where enabled, `#GP`, `#SS`, `#AC` or `#PF`), error code/PFEC, CR2 update rule, access
width/type, restart RIP, instruction phase, completed-element/byte count where applicable and
restart class.  The JIT, interpreter, HLE slow paths
and deferred signal landing consume that same descriptor rather than independently guessing from a
host signal.  Priority cells not established by the selected-profile manual or native oracle remain
disabled; address classification alone cannot decide a whole-instruction exception.

The minimum frozen rules are:

- non-canonical data addresses fault `#GP(0)` and SS-based addresses fault `#SS(0)` before an
  ordinary alignment or page check; the native oracle must settle the end-of-operand canonical
  boundary cases that the manuals do not state unambiguously;
- ordinary `#AC` requires `CR0.AM=1`, `RFLAGS.AC=1` and CPL3; those vCPU inputs must exist rather than
  being inferred from a host signal;
- before operand address classification, apply the exact encoding, LOCK-prefix, CPUID, CR0.EM,
  CR4.OSFXSR/OSXSAVE, XCR0 and instruction-specific `CR0.TS` gates.  A failed gate performs no
  operand access.  Simultaneously applicable `#UD` and `#NM` conditions remain profile-specific
  unless the selected vendor instruction definition fixes their priority;
- exception behavior is profile-specific.  The current synthetic LSX4 profile does not advertise
  `MisAlignSse`; the retained PS4 TestKit/Jaguar profile does.  In the latter, `MXCSR.MM` suppresses
  the default `#GP` only for the covered *generic legacy load/execute SSE* forms: with architectural
  alignment checking enabled the result is `#AC`, otherwise the access is permitted.  The vCPU must
  expose an immutable `MXCSR_MASK` with bit 17 set and preserve/restore the `MXCSR.MM` bit itself
  through the selected state-save ABI; `MXCSR_MASK` is capability data, not restorable processor
  state.  AMD explicitly excludes FXSAVE/FXRSTOR,
  `(V)MOVAPD/(V)MOVAPS`, `(V)MOVDQA`, `(V)MOVNTDQ`, `(V)MOVNTPD` and `(V)MOVNTPS` from that escape;
  their individual mandatory alignment contract still applies.  Extended VEX forms do not use MM;
  aligned forms fault on misalignment, while other forms are governed separately by ordinary
  alignment checking.  A shared mnemonic is therefore not a sufficient classifier;
- for the AMD profile, `CPUID.01H:ECX.CMPXCHG16B=0` causes `#UD`; otherwise a non-16-byte-aligned
  `CMPXCHG16B` operand causes `#GP(0)` before an architectural RMW result is published.
  `CMPXCHG8B` and scalar 1/2/4/8-byte forms have no instruction-specific mandatory-alignment `#GP`,
  but ordinary `#AC(0)` still applies;
- an atomic-prestate instruction leaves the saved RIP at the faulting instruction and publishes no
  architectural result from the failed attempt.  Architecturally progressive operations such as REP
  RAM string instructions and instruction-specific split/wide stores are different: the architecturally
  completed prefix and its RCX/RSI/RDI or byte/element progress may be visible, while the failing
  element is not retired.  Repair and resume must finish exactly the remaining work, neither
  duplicating the prefix nor skipping the failing element.  Each lowering declares one restart
  class and exact progress representation; an unknown class is ineligible for the optimized path.
  This does not imply that every failing iteration has no external side effect: `REP INS` may consume
  an I/O read before its destination write faults, so I/O strings require a separate restart class;
- REP address/permission classification is per architecturally attempted iteration.  An initial zero
  count performs no data access, and `REPE/REPNE CMPS/SCAS` may stop before the nominal RCX-sized
  range.  Whole-range preflight is allowed only under a proof covering these early exits.

The current LSX4 path is not evidence for those rules.  Address formation lacks end-range,
SS-vs-data, CPL/CR0.AM/RFLAGS.AC inputs; whole-vector classification merges aligned and unaligned
mnemonics and the read/write helpers use direct copies; the deferred record omits guest vector,
error code, CR2 rule, width, phase or restart class.  Scalar compare-exchange returns on mismatch without an explicit
write-intent/permission contract, and its misaligned mutex fallback is not atomic relative to JIT or
native accesses.  `CMPXCHG16B` has no mandatory-alignment check and is implemented as two 64-bit
operations under a process-local mutex.  Pinned FEX also lowers aligned vector IR with ordinary Arm
loads/stores and therefore remains differential evidence, not a precise-exception oracle.

Ambiguous cases stay unimplemented until an AMD native oracle resolves them: profile-dependent
legacy-SSE alignment versus unmapped/read-only priority; mandatory VEX alignment versus
unmapped/read-only priority; canonical start with non-canonical end; compare-exchange collisions
with alignment, noncanonicality, absence or other protection mechanisms; and split-page
ordinary-store rollback.  AMD
APM Volume 3 revision 3.38 already fixes the architectural part: scalar CMPXCHG and CMPXCHG8B/16B
always perform a memory RMW and mismatch writes back the value read.  The oracle verifies that
contract and resolves only the remaining delivery details; it does not get to replace the specified
write intent with a read.  It crosses aligned/unaligned offsets, RW/RO/unmapped mappings, segment
class, AC/AM combinations, match/mismatch and LOCK state, captures saved RIP/registers/flags/vector
state/CR2/PFEC and resumes after mapping repair.  Intel results are differential only and cannot
silently define an `AuthenticAMD` guest.

### Registry failure domains and publication transaction

A malformed or over-capacity *candidate* is a local admission failure.  It must reject that batch
without changing bucket heads, block count, mapping generation or any prior lookup, and the next
valid batch must still publish successfully.  Process-wide poison is reserved for corruption of an
already published structure (for example an impossible bucket index, broken live generation or
arithmetic inconsistency in committed state).  Treating allocation failure, bucket-depth pressure,
duplicate input or bad new metadata as global corruption turns one stale cache record into a
title-wide FPS-0 failure and is forbidden.

Bulk publication is transactional from the consumer's perspective.  Validate and stage every
block, initialize every immutable node, then publish one generation/root with release ordering.
Signal readers acquire one generation and see either the old complete view or the new complete
view.  Publishing bucket heads one at a time is acceptable only while all executable lookup targets
remain unreachable until the callback returns; it is not a reusable all-or-none registry contract.
Retirement first prevents new leases, then waits for a grace period, then reclaims or quarantines;
an address is never reused under the old generation.

### Persistent native-cache identity and crash consistency

`format version + Git hash` is not a sufficient native-code identity.  A cache key and header must
canonically serialize, in little endian, separate format/JIT-ABI/metadata-schema versions; a full
content digest of dirty FEX, adapter, thunk/helper ABI and build flags; compiler/NDK identity; the
effective HWCAP/HWCAP2 feature mask, SVE vector length and memory model; page/cache-line parameters;
and a cryptographic identity of the exact guest executable bytes.  Missing or mismatched fields
reject before counts, allocation, relocation, registry publication or lookup insertion.

Loading is an all-or-none transaction: check file bounds and exact EOF, all checked count products,
non-overlapping aligned block extents, every header/tail/RIP table, all source/entrypoint maps and all
relocation targets before the first executable side effect.  Relocations belong to exactly one code
region and cannot overlap a header, metadata table, padding or another relocation.  Resolve every
named thunk before staging; relocate private staging; make the complete image coherent; bulk-publish
the observer view once; only then expose lookup-cache entries.

Writing uses a unique `O_EXCL` temporary, checked short/EINTR-safe writes and seeks, checked truncate,
file `fsync`, atomic rename and parent-directory `fsync`.  Failure retains the previous final file
and never mutates the live executable buffer in place.  A loaded block remains quarantined until a
strong digest of its current guest source bytes matches the stored digest immediately before first
direct execution.

The pinned frontend can decode at most `0x10000` instructions, but the RIP table may contain both an
entry-point marker and a side-effect/debug marker for one decoded instruction.  Therefore the current
metadata-v1 decoder uses the structural limit `2 * 0x10000 = 131072`, additionally bounded by the
exact `RIPTableSize` and the complete published block interval.  It does not use the adapter's current
`MaxInst=5000` setting as a lifetime or validation invariant.  The producer rejects a marker count
above that structural limit before narrowing to the 32-bit field.  Host deltas are checked,
four-byte-aligned and nondecreasing (zero is valid); guest deltas are checked signed values and may
move backward in a multiblock trace.  Validation tracks the minimum and maximum decoded guest RIP
and requires their checked span to fit `GuestSize`; it must not incorrectly require every trace
marker to be above the initial entry RIP.

Synchronous and asynchronous signals require different actions.  A hardware-generated
`SIGSEGV`, `SIGBUS`, `SIGILL` or `SIGTRAP` must not be placed in any synchronous handler's
`sa_mask`: POSIX makes a hardware fault while blocked undefined.  Linux also automatically blocks
the delivered signal unless `SA_NODEFER` is set.  Therefore a production synchronous owner needs
`SA_SIGINFO | SA_ONSTACK | SA_NODEFER`, no synchronous signal in `sa_mask`, and a bounded,
preallocated nested-capture protocol.  Merely clearing `sa_mask` is not sufficient.  Merely adding
`SA_NODEFER` to the current single-slot handler is worse: a helper fault would recursively re-enter
and overwrite or spin on the same capture.  Until the finite nested protocol and tests exist, the
optional FEX backend must fail closed before activation rather than claim signal ownership.

The asynchronous `SIGURG` wake has a separate action without `SA_NODEFER`; it may be masked while a
synchronous capture is active because guest signal records live in a persistent queue and one
coalesced pending wake is sufficient to drain it later.  `SIGVTALRM`/`SIGSLEEP` is also a separate
action and retains its own default self-blocking semantics: the current debugger pause protocol
sends the same standard signal for pause and resume and then waits for the second instance.  Adding
that timer signal to another action's mask can coalesce the pair and deadlock.

The nested synchronous protocol uses a finite per-bound-thread stack in process-resident storage.
Depth one may classify/capture.  A second fault never re-enters registry traversal or operand
emulation: if the outer exact context is complete it redirects a copy of that *outer* GPR/SP/PC/
PSTATE and FPSIMD/SVE state to a normal-context failure landing; otherwise it uses an emergency
preallocated stack and terminates the backend.  Depth greater than the proved bound exits via a raw
syscall rather than recurring.  Calling and logging through a previous handler is not a nested
escape because a returning previous handler would resume the same faulting handler instruction.

On successful capture the handler preserves the exact pre-fault kernel mask.  Deferred processing
first publishes reconstructed guest state, releases the capture slot, and restores that exact mask
with raw `rt_sigprocmask` before queued delivery, callbacks or guest re-entry.  Decode failure before
capture leaves `uc_sigmask` byte-identical; restore failure is a controlled backend failure.

The PS4 signal frame is the modified-FEX Orbis inline layout, not stock FreeBSD stable/10's later
external xfpustate ABI.  Its exact producer writes `mc_len=0x480` and
`mc_flags=HASSEGS|HASBASES`; setting `HASFPXSTATE` would falsely advertise pointer-based state that
does not exist.  The public MXCSR mask is the subset the pinned FEX model actually preserves
(`0xFFC0`), rather than FreeBSD's wider hardware mask (`0xFFBF`).

## Alignment-safe software TSO

The existing FEX configuration switches do not provide a mode that both removes `BUS_ADRALN` on
stock Android and preserves the complete x86 memory model.  With software TSO enabled, scalar
loads are lowered to `LDAR*` or, when RCpc is available, `LDAPR*`; scalar stores are lowered to
`STLR*`.  These ordered accesses still require natural alignment.  LRCPC2 can fold a signed
immediate into `LDAPUR*`/`STLUR*`, but it does not turn an unaligned ordered access into an ordinary
unaligned access.  Vector TSO uses an ordinary vector memory operation paired with a barrier, while
atomic REP/memcpy continues to use ordered scalar accesses.  Disabling scalar TSO removes those
faults by using ordinary `LDR`/`STR`, but is an intentional loss of x86 semantics and is not a
correctness fix.

`SetHardwareTSOSupport(true)` is valid only after the OS has actually enabled hardware TSO for every
thread that can execute translated code.  A process-level capability probe is not evidence of that
per-thread state, and the Android integration currently contains no corresponding per-thread
enable operation.  Faking this flag is forbidden.  Explicit guest `LOCK`/RMW operations remain
atomic even with hardware TSO and need their own exact unaligned implementation.

The upstream half-barrier option is a first-fault rewrite policy, not a way to prevent the first
fault.  In the current bridge, backpatching is disallowed, so a hot unaligned ordered site can pay a
complete signal round trip on every execution.  Enabling a two-instruction in-place rewrite is also
forbidden: another core may execute a mixed old/new pair, and an `ISB` on the patching core does not
broadcast an atomic instruction-stream update.

A production replacement needs an alignment-aware lowering rather than signal-time operand
emulation:

- prove alignment in IR and emit only the ordered fast path when possible;
- otherwise branch on the effective address to an aligned ordered fast path and a normal-context
  unaligned slow path;
- validate the ordinary-memory-plus-barrier slow sequence with AArch64/x86-TSO litmus tests before
  accepting it; do not infer equivalence from single-thread execution;
- preserve page-crossing fault order and partial-access semantics explicitly;
- implement `LOCK`/RMW, split-lock and 16-byte compare-exchange separately with all-or-nothing
  publication and no signal-context mutex;
- include the effective TSO/alignment lowering mode and relevant host ISA features in native-cache
  identity.

Until those gates pass, the full FEX signal owner remains unavailable.  A title-specific switch
that disables vector or memcpy TSO may be a diagnostic or explicitly unsafe performance mode, but
must never be reported as full strong-memory support.

### Guest atomicity is vendor-sensitive

Do not promote Intel's wider ordinary-access guarantee to a universal x86 rule.  Intel P6 and later
guarantee cached unaligned 16-, 32- and 64-bit ordinary accesses that remain inside one cache line.
AMD64's published portable rule is narrower: a single load or store is naturally atomic only while
it does not cross an aligned eight-byte boundary.  Bloodborne's PS4/Jaguar guest profile is AMD, so
the AMD rule is the required baseline unless a future virtual CPU profile explicitly selects Intel
semantics.

The pinned FEX `MemoryModelEmulation.md` describes unaligned x86 accesses as atomic “within a
cacheline”.  That is useful implementation intent, but it is not a vendor-neutral architectural
classifier and is too broad for the AMD rule above.  Likewise comments in the current dual-CAS
helper are not proof of all-or-nothing guest semantics.  Official guest and host architecture
contracts take precedence over both comments.

Reference implementations are evidence, not an oracle:

| Reference | Useful evidence | Not transferable without an LSX4 proof |
|---|---|---|
| Pinned FEX-2608 | selective strong-memory lowering, half-barrier design, LSE/LRCPC feature split, first-fault classification | its cache-line wording as a vendor-neutral x86 rule; SIGBUS-first repair; operand access and mutable runtime work in a synchronous handler |
| Box64 reference `86ff049f` | `HWCAP_USCAT` discovery, runtime aligned-container checks and the need to distinguish ordinary/locked accesses | heuristic `SMREAD`/`SMWRITE` barrier levels; a global mutable guest-PC fault hash; bytewise signal-handler emulation and context mutation |
| Current LSX4 | title compatibility evidence and existing fault/state sidecars | comments, a green single-thread fixture or a finite phone stress run as proof of TSO, atomicity or precise exceptions |

Only an official guest ISA contract, an official Arm/Linux host contract and executable
model/native-oracle tests may promote one of those ideas into a production lowering.

### Frozen virtual CPU profile

The effective guest contract is the serialized virtual CPU profile, not the marketing name of the
physical console and not the host CPU.  The current LSX4 CPUID table is a synthetic profile:

- vendor is `AuthenticAMD`;
- leaf 1 EAX is `0x00630f01`, which decodes by AMD's documented formula to display family 15h,
  model 30h, stepping 1 (the Kaveri/Steamroller family range), not family 16h Jaguar;
- leaf `0x80000001` ECX is `0x00000020`: ABM is advertised, while SSE4a and `MisAlignSse` are not;
- leaf 7 EBX is `0x00002128`, which advertises BMI1, AVX2 and BMI2.  This is internally inconsistent
  with a faithful physical Kaveri/Jaguar identity.  A virtual CPU may deliberately expose a
  software-defined superset, but LSX4 has no recorded provenance or semantics for this combination;
- the table originated in the first retained LSX4 snapshot and was later isolated into
  `guest_capability_profile.cpp`; no provenance or compatibility rationale accompanies it.

A retained PS4 TestKit cold-boot log supplies a materially different native reference: vendor
`AuthenticAMD`, ID `0x00710f13` (family 16h, model 11h, stepping 3), leaf-1 EDX `0x178bfbff`, leaf-1
ECX `0x36d8220b`, extended EDX `0x2e500800` and extended ECX `0x154837fb`.  The last value advertises
SSE4a and `MisAlignSse`; AVX2 and BMI2 are absent.  This is evidence for a TestKit Jaguar profile,
not permission to overwrite the already observable LSX4 profile silently.

Pinned FEX-2608 is different from both.  `CPUID_AMD` is a commented debug define, so the normal
build exposes `GenuineIntel` with family 6/model A6-derived identity and a different, partly
host-dependent feature set.  The LSX4 bridge installs host-code-generation features but has no ABI
operation that installs the complete guest CPUID/XSTATE table; setting `SupportsAVX=true` changes
host lowering support and does not make the guest CPUID profile match.  Therefore exact equality of
all guest-visible CPUID/XSTATE leaves between Current and FEX is an activation prerequisite, not a
post-launch compatibility improvement.  Host HWCAP may select generated Arm instructions but must
never leak into the guest CPUID contract.

Changing the Current leaves to the TestKit table is a vCPU migration, not a local bug fix.
It changes instruction discovery and observable exception behavior, so the profile ID and complete
CPUID table belong in persistent native-cache identity and diagnostics.  Existing titles need a
compatibility gate and cache namespace transition; a backend may not silently interpret the current
leaf values with a different Jaguar-only rule.

The current machine images retain `RFLAGS` and `MXCSR` but have no coherent architectural state for
`CR0.AM`, CPL, `MXCSR_MASK`, Jaguar's `MisAlignSse` MSR controls or an exact vCPU profile ID.  Merely
storing the AC bit inside `RFLAGS` does not implement alignment checking.  Before specializing any
alignment path, define one immutable profile containing at least:

- vendor, exact family/model/stepping, complete CPUID/XSTATE leaves and linear-address width;
- SSE/AVX/AVX-512 availability, CR4.OSFXSR/OSXSAVE and XCR0 policy;
- `CR0.AM`, CPL and writable/forced `RFLAGS.AC` behavior;
- effective `MisAlignSse`, `MXCSR.MM`, `MXCSR_MASK`, reset/INIT and save/restore behavior;
- Jaguar MSR exposure/override policy, or an explicit rule that those MSRs are absent and the
  advertised bit is frozen;
- canonical-address, selected-segment, CR2, saved RIP/error-code and exception-priority policy.

Every mutable item used to remove a runtime check either guards/deoptimizes the generated region or
is part of cache identity.  Until that profile exists, the conservative path must not synthesize an
alignment exception from a guessed Jaguar state or elide one from a guessed Current state.

For an AMD guest, the ordinary scalar classes are therefore:

- one byte is always a single-copy-atomic scalar access to ordinary cacheable memory;
- 2/4/8 bytes wholly contained in one aligned eight-byte quantity must not tear;
- an ordinary access crossing that quantity may tear and must still preserve TSO ordering and exact
  page-fault visibility;
- `LOCK`, `XCHG`, RMW and `CMPXCHG16B` are separate contracts and cannot use the ordinary tearing
  path.

On Arm without `FEAT_LSE2`, the unconditional ordinary-scalar single-copy-atomic path is a naturally
aligned single-GPR 1/2/4/8-byte load or store; pair, vector, ordered, atomic and Device accesses keep
their separate rules.  `USCAT` is the Linux userspace capability name for `FEAT_LSE2`, not a second
Arm architectural extension.  With `FEAT_LSE2` (exposed as `HWCAP_USCAT`), an access whose bytes all
fit in one 16-byte quantity aligned to 16 bytes is architecturally guaranteed single-copy atomic
when the effective memory is Normal, Inner Write-Back and Outer Write-Back Cacheable.  For an
unaligned non-atomic Load-Acquire/Store-Release, the guaranteed nonfaulting SCA path additionally
requires Shareable memory.  Outside that complete predicate, Arm may generate an Alignment fault or
perform an access that retains the instruction's acquire/release semantics but is not guaranteed SCA
beyond individual bytes.  `HWCAP_ATOMICS`/LSE does not imply this property.  LSE2 supplies no usable
whole-access SCA or nonfaulting guarantee for Device/MMIO, non-WB or cross-16 accesses; aligned-16
containment also excludes a normal page crossing, and LSE2 does not establish x86 ordering.

The signal-free lowering must follow this matrix:

1. If IR proves natural alignment, `LDAPR*` is the RCpc/AcquirePC candidate where RCpc is present;
   otherwise use `LDAR*`, while `STLR*` supplies release semantics.  The RCpc substitution is enabled
   for x86 only after the surrounding access classes pass the formal TSO/cumulativity suite.
2. If LSE2 is present and the effective address plus mapping sidecar proves the aligned-16,
   Normal Inner/Outer Write-Back **and Shareable** envelope, the corresponding unaligned
   acquire/release access has the guaranteed nonfaulting single-copy-atomic fast path.  The Shareable requirement comes from
   Arm ARM Issue L.a known-issue D23529 and is part of the effective gate, not an optional tuning
   attribute.  Outside that complete envelope Arm permits an alignment fault or a performed access
   retaining acquire/release semantics but guaranteed single-copy atomic only at byte granularity.
3. Without LSE2, an AMD-required unaligned scalar contained in one aligned eight-byte quantity may
   use the smallest naturally aligned containing load plus extraction.  A store/RMW may use a masked
   release/acq-rel CAS loop only after mixed-size overlap tests prove that all guest agents observe a
   compatible protocol; a CAS on a wider container races differently with an ordinary adjacent-byte
   store and cannot be accepted merely because it preserves the untouched bits in one execution.
   Every container path is forbidden for MMIO.
4. For an ordinary AMD access crossing aligned eight bytes, `LDR; DMB ISHLD` for a load and
   `DMB ISH; STR` for a store are candidate order-preserving sequences.  `DMB ISHST` is insufficient
   for the latter because x86 does not permit an earlier Load to pass a later Store.  They may be used
   only inside one stable Normal mapping with no MMIO, watchpoint or page/type boundary, and only
   after an executable mixed-size oracle demonstrates that every host split result is inside the
   Jaguar-permitted outcome set.  The fact that the guest permits tearing does not make an arbitrary
   host split order or fault boundary correct.
5. A page, cacheability or mapping-type boundary goes through a normal-context helper holding exact
   mapping epochs/leases.  A permission check followed by an unchecked access is a TOCTOU bug.

On a specifically supported Android arm64 kernel/configuration, ordinary anonymous and regular
tmpfs/memfd user mappings normally obtain Linux `PAGE_SHARED`/`PAGE_READONLY` protections derived
from `_PAGE_DEFAULT`, whose stage-1 PTE is `MT_NORMAL` and Inner Shareable.  That pinned kernel
implementation fact is useful but is not derivable
from `/proc/self/maps`, `mmap` protection bits or an arbitrary file descriptor alone.  The mapping
sidecar may assert the complete LSE2 memory-attribute predicate only for allocations created and
tracked through LSX4's ordinary anonymous or regular memfd/shmem paths on a supported arm64 kernel.
It must clear the predicate for imported/dma-buf/AHardwareBuffer/Vulkan host mappings, device or
physical-memory files, `pgprot_writecombine`/DMA-coherent mappings, unknown external file mappings
and any untracked alias.  Any active stage-2 translation is part of the effective-attribute proof.
`mprotect` changes access permission, not this provenance, but remap,
replacement, external import and alias creation publish a new mapping epoch before execution.

Misaligned `LOCK`/RMW contained in one aligned container can use a masked acq-rel CAS on that
container only under a global-participation invariant: every CPU, HLE, GPU/DMA and watch agent that
can overlap the container must use the compatible protocol.  Finite stress is corroboration, not a
proof of that invariant.  `CASPAL` is enabled by LSE/`HWCAP_ATOMICS`, not by LSE2/`USCAT`.  A
64-bit-pair `CASPAL` on a 16-byte-aligned container can cover the cross-eight but within-16 case only
when the effective mapping is an architecturally supported Shareable Inner/Outer-WB,
read/write-allocate, non-transient Normal mapping and the global-participation invariant holds.  Two CAS operations
are not an atomic replacement across an
aligned-16, cache-line or page boundary; the existing FEX helper itself documents that its dual-CAS
fallback can tear.  Such a rare operation needs cooperative guest-memory quiescence across CPU,
HLE, DMA and GPU agents, or it must fail closed.  `CMPXCHG16B` retains its guest 16-byte alignment
exception contract with or without a `LOCK` prefix.  Only the locked form receives the global atomic
contract; an unlocked form must not silently inherit it.  Compare failure still has write intent, so
a mismatch on a present read-only user page is a write-protection `#PF` with `PFEC.P=1`,
`PFEC.W/R=1` and the profile's applicable `U/S` value.  The pinned frontend currently emits `_CASPair`
without an explicit guest 16-byte-alignment guard, and a host alignment abort is not a guest #GP.

Ordinary `MOVUPS`/`MOVDQU` and REP copies are not secretly scalar atomics.  A vector ordinary-access
plus directional-barrier fast path is admissible only for proven stable Normal RAM with no page,
mapping-type, MMIO or watchpoint boundary.  A faulting vector load leaves its architectural
destination unchanged.  A wide store can architecturally complete constituent writes before a
later fault; preflight-and-write-nothing is a deterministic strengthening, not automatically an
exact Jaguar/Intel partial-completion model, and must be named and tested as such.

For the PS4/Jaguar gate, unmasked aligned `MOVAPS`/`MOVDQA` require the exact alignment for the
encoded legacy/VEX width: 16 bytes for 128-bit and 32 bytes for 256-bit forms.  Their misalignment is
`#GP(0)` and is never relaxed by `MXCSR.MM`.  EVEX masking, suppression and DQA32/DQA64 semantics are
not extrapolated from that rule; they belong to a separate AVX-512 profile and oracle.

Ordinary alignment checking is different.  It is active only when `CR0.AM=1`, `RFLAGS.AC=1` and
CPL=3.  For AMD legacy generic SSE, `MXCSR.MM` participates only when effective
CPUID.`MisAlignSse=1`; it never affects VEX or explicitly aligned forms.  Intel reserves MXCSR bits
16..31, so loading a set MM bit on an Intel profile is itself invalid rather than an AMD-compatible
mode.  Intel also documents implementation-dependent `#AC` behavior for several explicitly
unaligned moves, so a generic “Intel” label is insufficient: select an exact tested family/model or
freeze a deterministic virtual policy.

Noncanonical stack-segment references produce `#SS(0)` and other noncanonical linear addresses
produce `#GP(0)` without changing CR2.  AMD's ordinary memory pseudocode orders canonical/segment
checks, then ordinary `#AC`, then translation/page faults.  Manuals do not provide a portable
priority for a canonical operand that is both misaligned for an explicitly aligned vector move and
unmapped.  That cell remains fail-closed until a native oracle for the exact guest profile records
the vector, error code, CR2, saved RIP and partial architectural state.  A host AArch64 SIGBUS is
never that oracle.

The minimum vector exception matrix covers load and store forms of legacy-128, VEX-128 and VEX-256
APS/DQA and UPS/DQU, every address residue modulo the required alignment, mapped RW, PROT_NONE,
unmapped, page-crossing and noncanonical SS/non-SS addresses.  It crosses AM/AC/CPL and, for a real
AMD `MisAlignSse` profile, MM.  It runs through T0, T1, T2 and restored native cache.  A faulting load
must leave the destination unchanged; a faulting store and saved-state result must match the exact
selected guest oracle rather than a convenient preflight strengthening.

REP progress need not be written to machine state after every element, but after each retired
element RCX/RSI/RDI must be exactly reconstructible at every guest-visible fault or event.  Pinned
host registers plus fault-site metadata are valid; replaying the bulk operation from its initial
state is not.  For faulted `REPE/REPNE CMPS/SCAS`, RCX and the applicable indices identify the next
iteration, but RFLAGS is restored to its pre-instruction value; interrupt/event and fault restart
images are modeled separately.  With 67h in 64-bit mode, ECX/ESI/EDI use 32-bit wraparound and the
progressed architectural RCX/RSI/RDI have zero upper halves.  Bulk MOPS or wide-copy paths therefore require a stable-mapping proof excluding
synchronous faults, side effects and unsafe overlap, plus bounded event checkpoints.  The pinned
frontend currently updates architectural registers only after `_MemSet`/`_MemCpy` returns and
explicitly says partial-faulting REP is unsupported.  It also rejects the 67h address-size form,
although x86-64 requires ECX/ESI/EDI and 32-bit wrapping semantics; both are activation blockers.

The existing IR `Align` field is not yet a proof and must not directly select one of these paths.
It is present on `LoadMem`/`StoreMem` and `LoadMemTSO`/`StoreMemTSO`, but all four AArch64 lowering
functions currently ignore it.  The frontend normally maps an omitted value to the operation size.
That happens for ordinary scalar ALU memory operands even though an arbitrary x86 effective address
need not be naturally aligned.  Non-REP `MOVS`, `STOS`, `CMPS`, `SCAS` and `LODS` also pass the
element size while RSI/RDI remain unconstrained.  Conversely, explicitly unaligned moves pass one
byte and aligned SIMD forms usually rely on the default.  This mixture shows that the field combines
instruction-required alignment, assumed access alignment and a code-generation hint; it has no
single verified provenance contract.  Its current harmlessness follows only from being ignored.

The highest-value compiler change therefore starts by adding a distinct, monotonic
`KnownAlignment`/low-bit-residue fact whose source is auditable.  Only constants, a proven aligned
allocation/stack invariant, an alignment-preserving arithmetic transfer or an explicit guarded
branch may strengthen it.  An opcode's alignment requirement is a different fact: it requests the
guest #GP/alignment check, but does not prove the runtime address meets it.  Unknown remains one byte.
Every pass must preserve or weaken the proof; no default may silently upgrade unknown to operation
size.  After that audit, the lowering can branch to a cold generic path only when proof or the LSE2
runtime containment guard fails.  `USCAT`, guest vendor atomicity profile, mapping memory
type/shareability and effective lowering mode are ABI and persistent native-cache identity inputs.
The current host
profile/bridge does not carry `USCAT`, so no such path may be enabled yet.

Required alignment-provenance tests dump IR and native code for arbitrary low-bit residues of
ordinary scalar MOV/ALU, aligned and unaligned SIMD, stack operations, every non-REP string form and
REP progress paths.  A negative compiler test deliberately supplies a false legacy `Align` value;
the new classifier must still choose a guard/helper rather than an unconditional ordered access.

Feature discovery and code generation use distinct gates:

- `HWCAP_ATOMICS`/LSE permits CAS/CASP/SWP/LDADD-family RMW encodings but does not relax
  unaligned-access rules;
- `HWCAP_USCAT`/LSE2 changes the aligned-16 Normal-WB atomicity envelope without adding a new
  mnemonic;
- `HWCAP_LRCPC` permits `LDAPR*` RCpc loads; it does not imply LSE2;
- `HWCAP_ILRCPC`/LRCPC2 adds `LDAPUR*`/`STLUR*` immediate forms and is primarily an addressing
  optimization, with the same atomicity constraints and a required MIDR errata/performance
  blacklist;
- LRCPC3 stays disabled until a separately reviewed lowering uses it.

Linux explicitly requires checking HWCAP through `getauxval`; instruction probing is not a reliable
substitute and absence is not guaranteed to produce `SIGILL`.  Add one effective
`LSE2_USCAT` bit through the main host profile, adapter ABI and FEX `HostFeatures`; intersect the
host-advertised value with local HWCAP and user policy.  A per-MIDR override is legal only when every
JIT thread is proven to remain on a homogeneous compatible CPU set; otherwise heterogeneous Android
migration makes that override unsound and only a system-wide HWCAP/kernel workaround may enable the
path.  Hash the effective bit, LSE, LRCPC/LRCPC2/LRCPC3 and emitted TSO mode into native-cache
identity.  Any VL-dependent SVE route additionally establishes and verifies the same Linux per-thread
VL for every JIT thread and parses the signal frame's VL; one process-start VL in cache identity is
insufficient.  Do not hash cache-line size for LSE2: containment is defined by the architectural
aligned-16 quantity and is an IR/runtime fact, not a host-cache attribute.

### Strong-memory oracle and negative controls

Ordering and atomicity are separate proof obligations.  The initial CI oracle uses the upstream
`x86tso.cat` model for ordinary guest outcomes and the current Arm AArch64 model for lowered
outcomes.  Instruction-specific write-intent and exception behavior that `x86tso.cat` abstracts away
uses an extended guest event model plus a native x86 oracle.  Each supported lowering is tested
independently; passing only the fastest hardware path is insufficient.  The suite contains both the
intended lowering and a deliberately weakened ablation that must admit the forbidden result, so a
vacuous harness cannot pass.

The mandatory ordering set is:

- SB: determine whether the `0,0` result allowed by x86 is also admitted by each candidate lowering;
  `STLR; LDAR` is a conservative baseline, not an assumed exact equivalence;
- MP, LB, IRIW and WWC: compare outcome sets for the RCpc scalar path and the directional
  half-barrier path against x86, while the plain `LDR`/`STR` ablation supplies a negative control;
- WRC, RWC, transitive message passing and the DPDK-style ring publication pattern: exercise RCpc
  cumulativity explicitly.  `LDAPR` supplies only RCpc acquire semantics and cannot be promoted from
  basic two-thread MP results; Arm has documented real ring-buffer failures caused by treating a
  partial order as a global one;
- LOCK-SB and LOCK-2+2W: determine whether acquire-release RMW plus the selected barriers provides
  the x86 full-fence/global LOCK envelope across different addresses; do not assume acq-rel alone is
  sufficient.  A missing-barrier/relaxed ablation must expose the forbidden outcome.

### Locked compare-exchange is outcome- and context-sensitive

The current Arm model and the guest instruction contract must both be read at instruction-level
granularity.  In the corrected Arm model, a successful acquire-release AMO has a write event and its
`aob`/`bob` edges can provide the full-fence-like order needed by the successful x86 locked
compare-exchange mapping.  On compare failure, Arm permits `CASAL` either to perform no write or to
atomically write the value read back to memory; release ordering that depends on a generated write
therefore cannot be assumed for every execution.  The corresponding LL/SC fallback executes
`LDAXR; compare; CLREX` and definitely has no release store on mismatch.  A proof for successful CAS
therefore says nothing about the failure path, and a failed `CASAL` is not a standalone full fence.

That fact does not by itself reject the complete FEX scalar mapping.  In the exact contextual
translation `x86 load -> LDAPR(Q)`, `x86 store -> STLR(L)` and `LOCK CMPXCHG -> CASAL`, the surrounding
mapped events can supply the incoming order even when the CAS takes its no-write path: a preceding
RCpc-acquire read is ordered before the later CAS memory event, a preceding release store is ordered
before the CAS acquire read, and the CAS acquire read orders later mapped memory events.  This is a
proof obligation for the whole translation, not a property of `CASAL` in isolation.  It applies only
when every guest access crossing the envelope uses the proved mapping and no plain host `LDR/STR`,
helper, HLE callback, signal repair or mixed-size fallback is silently inserted into that envelope.
Such a boundary needs an independently proved wrapper (a leading full barrier is a conservative
candidate), rather than inheriting the contextual result.

AMD's x86 contract is stricter around a locked instruction: all earlier loads/stores complete before
it and it completes before later loads/stores.  AMD64 APM Volume 3 also specifies that memory
`CMPXCHG` always performs a read-modify-write and, on mismatch, writes the same value that it read;
Intel specifies the same logical old-value write-back.  This is an architectural write intent, not a
claim about a particular external bus transaction.  Consequently a failed guest `LOCK CMPXCHG`
must preserve the full ordering envelope, atomic RMW/write-back contract and write-fault behavior.
Arm CAS uses an atomic read/write access descriptor even on mismatch, so `CASAL` can still request
write permission and fault on a read-only page; however, its permitted no-write execution does not
by itself reproduce the AMD same-value write-back/coherence event.  Ordering, permission checking
and architectural write-back therefore have separate acceptance tests.

Pinned FEX-2608 does not encode the guest LOCK distinction in IR.  Its decoder records whether the
prefix was handled, but memory CMPXCHG emits the same `_CAS` for locked and unlocked forms.  The
AArch64 backend then emits `CASAL` when LSE is available, or `LDAXR`/conditional `STLXR` otherwise.
The surrounding FEX scalar TSO lowering may satisfy the locked ordering relation after the complete
contextual proof above, but the IR still cannot express the different unlocked contract or AMD
failure write-back intent, and helper/unaligned paths cannot inherit the scalar proof.  This behavior
is present in clean `FEX-2608` and is not an artifact of the dirty integration tree.  Current LSX4
instead routes only the locked form through a sequentially-consistent atomic helper; replacing that
helper merely because `CASAL` is available would be a semantic regression unless all four contracts
pass independently.

The IR must carry at least `guestLocked` and the required write-intent/fault class.  Candidate
mismatch implementations include a retrying atomic write-back of the observed value or a
cooperative globally ordered slow path, but neither is accepted by inspection.  The chosen sequence
must prove that it:

- creates the required locked-operation ordering on both success and forced failure;
- cannot turn a guest mismatch into a spurious success while racing another writer;
- requests write permission and reports the selected vCPU's exact read-only mismatch fault even
  when values differ;
- is indivisible for every admitted alignment/size class and includes CPU, HLE, DMA and GPU agents;
- restores the guest accumulator/flags and retires exactly once after a recoverable fault.

Required controls distinguish an isolated instruction from the contextual mapping.  A standalone
`STR; failed CASAL; LDR` no-write execution must remain allowed in the current Arm model; adding the
declared conservative wrapper must forbid it.  Replacing the preceding mapped `STLR` with `STR`, or
the incoming `LDAPR` with `LDR`, must also re-enable the weak outcome.  Required positive cases
include successful SBAL, contextual forced-failure store/CMPXCHG/load, two-address failed-lock
observers, mismatch on a read-only page, contention while a write-back CAS retries, native AMD/Intel
comparison and locked versus unlocked disassembly from the actual decoder output.

The initial formal suite pins the following sensitivity sentinels rather than relying on test names
alone:

1. The model-version sentinel is the successful `SB+CASAL+LDR` case from the 2022 Arm-model fix.  Its
   weak outcome is allowed by the old v06 model and forbidden by v07/current.  Replacing `CASAL` with
   a relaxed CAS or with an LL/SC sequence that lacks the corrected acquire-release AMO edge must
   make the weak outcome allowed again.
2. The isolated failure sentinel uses `STR; CASAL(fail/no-write); LDR` in each thread.  The weak result
   is allowed by the current Arm model.  A declared full-order wrapper must forbid it; removing that
   wrapper must restore it.  This proves that the runner actually enumerates the no-write failure
   path instead of accidentally testing only success/write-back.
3. The guest source sentinel performs symmetric failed `LOCK CMPXCHG` operations surrounded by
   ordinary x86 accesses.  The weak result is forbidden by the pinned x86 model; removing LOCK is
   the negative control.  Architectural same-value write-back and fault behavior are checked by the
   native AMD oracle because the ordering model is not a complete exception/write-intent model.
4. The contextual target sentinel publishes `x` and then `y` with mapped release stores; the other
   thread reads `y` through `LDAPR` and executes a failing `CASAL` on stale `x`.  Observing the
   publication while the CAS still reads the old `x` is forbidden.  Replacing only `LDAPR` with
   `LDR`, or the relevant `STLR` with `STR`, must admit that outcome.
5. The repository `MP+rel+acqpc` case is a separate RCpc-cumulativity sentinel.  Its bad publication
   outcome is forbidden with `LDAPR` and allowed after `LDAPR -> LDR`; this prevents the opposite
   mistake of treating RCpc as if it provided no transitive order at all.

RISOTTO's `CMPXCHG -> CASAL` theorem is useful evidence for the contextual ordering mapping, but its
abstract failed RMW has only a read event.  It therefore does not prove AMD's architectural
same-value write-back, write permission/fault or mixed-size behavior.  The formal ordering result,
the native x86 fault oracle and the byte-level atomicity model are all mandatory and none substitutes
for the others.

The lowering acceptance matrix is therefore:

| Guest operation | Candidate Arm path | Proof/fault gate before use |
|---|---|---|
| Ordinary aligned scalar load/store | `LDAPR`/`STLR`, `LDAR`/`STLR`, or directional barrier sequence | exact ordinary-TSO outcome set, RCpc cumulativity suite and mapping-type sidecar |
| Ordinary unaligned scalar load/store | alignment branch plus ordinary access/barrier or LSE2-qualified path | mixed-size SCA/tearing oracle, aligned-16 Normal-WB Shareable proof and page-crossing first-fault oracle |
| Unlocked memory CMPXCHG | separate non-global compare/write semantics | decoder must retain the absence of LOCK; exact selected-vCPU write intent and page-fault behavior |
| Locked CMPXCHG success | acquire-release RMW candidate | SBAL/LOCK tests, single-copy atomicity, write fault and precise register/flag recovery |
| Locked CMPXCHG failure | proved contextual/global-order path plus AMD same-value write-back; never an isolated CAS miss | failure-order litmus, global locked-order observers, read-only mismatch fault and contention retry |
| CMPXCHG8B/16B | aligned pair RMW or cooperative quiescent slow path | architectural alignment exception before translation fault, all-or-nothing publication and every-agent participation |
| REP/vector memory | explicit per-iteration/per-element progress model | page-boundary partial progress, DF direction, restart RIP/count/index state and vector fault reconstruction |

No row inherits proof from another row.  In particular, LSE availability proves encoding support,
LSE2 proves only a qualified single-copy-atomic envelope, and a successful-CAS ordering proof does
not authorize the failed-CAS row.

Mixed-size and single-copy-atomicity cases use an SCA-aware model such as the current Arm mixed-size
model/RMEM, not C++ atomics as a substitute:

- overlapping W32 and unaligned W16 may finish only in one of the serial x86 results, never a
  byte-torn hybrid;
- aligned W64/W64/R64 admits only the initial value or one complete writer value; split reader and
  split writer ablations each demonstrate the corresponding tear;
- a masked-container CAS racing a guest store to an adjacent byte retries an adjacent-only host CAS
  failure, preserves that byte and never exposes a spurious guest compare failure;
- a naive one-shot widened CAS and a load/merge/store implementation are required negative controls.

A widened CAS is permitted only for a guest LOCK/RMW implementation with that retry protocol.  It
is forbidden as the general implementation of an ordinary sub-width store: it adds a false RMW and
coherence footprint to disjoint adjacent bytes and can strengthen the guest model.  Barriers around
a split access fix ordering only; they never prove single-copy atomicity.

Random Android stress runs follow the executable-model oracle and cover every effective HWCAP path,
page/cacheability boundary and overlap class.  Failure to observe a rare forbidden result on one
device is not correctness evidence.

### Existing-test evidence and anti-vacuity gate

The current tree has no executable x86-TSO litmus suite and no herd/RMEM integration.  Requiring a
strong-memory capability bit in an ABI smoke proves negotiation only; it does not exercise the
lowered memory operations.  The built-in atomic exchange fixture checks final values in one thread,
not ordering, single-copy atomicity or mixed-size interference.  HLE signal tests use an asynchronous
user signal and do not prove nested synchronous `SIGSEGV`/`SIGBUS` behavior.  Source-text contract
tests are useful lint, but cannot prove a concurrent lifetime or fault invariant.

Several existing mechanisms can produce a false green and therefore cannot be promotion gates as
they stand:

- CMake builds the FEX smoke executables but does not register/run them as tests;
- `runtime_selftest_dump` can print a non-null JSON object and return success even when it contains
  `"ok": false`;
- some AArch64/Android fault fixtures retain default-true result fields when their platform-gated
  bodies do not execute;
- a litmus test detached from actual generated bytes can validate a hand-written sequence that the
  backend never emits.

The replacement gate is non-vacuous by construction:

1. Disassemble generated bytes and match them to the declared lowering template.  Every effective
   capability/path cell has a nonzero execution counter; unsupported or unexecuted cells fail rather
   than skip silently.
2. Compare the intended lowering against the official AArch64 model and a guest model that covers
   the exact instruction contract.  Upstream `x86tso.cat` is useful for ordinary ordering but models
   a failed compare-exchange as a read rather than the architectural locked write-intent event; it
   cannot be the sole oracle for that case.  The universal safety condition is refinement:
   `Arm outcomes` must be a subset of `guest outcomes`.  Equality is additionally required only for
   a path advertised as exact/non-strengthening; a strict subset is an explicitly labelled
   conservative path.  Progress, fairness, MMIO and exception behavior are separate obligations and
   cannot be inferred from finite outcome-set inclusion.
3. Pair every intended case with a weakened mutant: remove the directional barrier, replace
   `LDAPR/LDAR` with `LDR`, or relax the RMW.  The harness is accepted only when that mutant admits a
   guest-forbidden outcome.  Otherwise a typo, dead path or undersampled test cannot pass.
4. Use a mixed-size/SCA-aware model for overlap and tearing.  C++ atomics and final-value equality are
   not substitutes for the guest byte-level model.
5. Run hardware stress only after the model gate, across baseline/LSE, RCpc, LSE2-USCAT and fallback
   paths.  Hardware non-observation corroborates a proof; it never creates one.

The first executable set is SB, MP, LB, IRIW, WWC, WRC, RWC, transitive publication, MFENCE-SB,
LOCK-SB, LOCK-2+2W and locked-CMPXCHG success/failure variants, followed by overlapping W32/W16,
complete-value W64 readers/writers and masked-container-CAS adjacent-byte races.  Fault cases cover
read-only CMPXCHG mismatch, every Rn/Rt encoding including x31 semantics, page boundaries, faults
before and after a possible side effect, same-signal recursion under `SA_NODEFER`, SEGV/BUS nesting,
SMC/retirement during a range lease and a stale mapping epoch.  Each operation retires zero or one
time; duplicate completion is always a failure.

## Signal ownership gate

FEX owns the signals it requires only after successful activation.  Current owns them otherwise.
The following Current mechanisms must consult the immutable owner before installing or reclaiming
handlers:

- the Current JIT signal installer;
- the repeating SIGBUS ownership watchdog;
- runtime death catcher/sentinel installation;
- scoped guest-entry handler restoration;
- guest signal queue/context hooks.

Backend ownership cannot be inferred from environment variables or from whether an adapter handle
exists.  A loaded-but-dormant adapter must have zero effect on Current.  A post-activation error
requires process exit because the historical adapter has no safe signal/context teardown.

Capability tables are activation promises, not inventories of non-null function pointers.  An
adapter that cannot complete the full signal-owner contract must omit
`CAP_SINGLE_SIGNAL_OWNER`, its lifetime counterpart and every dependent guest-signal/futex
capability from API, configure and query results.  The host must reject it during preflight and
select Current before publishing any process-global owner.

Signal ownership is the commit record of one activation transaction.  The correct order is:

1. validate the immutable request and all capability/lifetime requirements;
2. stage executable ranges, callbacks and bridge state without making them signal-reachable;
3. create and validate the complete runtime;
4. publish the signal-safe active API with release ordering;
5. atomically publish `FaultSignalOwner::Fex` as the final commit;
6. permit handler installation only when both the committed owner and active signal API agree.

Any failure before step 5 destroys unpublished snapshots, closes uncommitted `dlopen` handles and
leaves Current unchanged.  A successful preflight alone cannot select an immutable owner.  A
handler constructor cannot treat `owner == Fex` as sufficient if the active API is null.  These are
independent defence-in-depth checks because loader, native runtime and renderer have different
construction order.

Implementation status after the lifecycle audit: the bridge rejects the unproved full owner before
process initialization or allocation and advertises only base runtime features.  The outer adapter
now also omits guest signals, signal-dependent guest futex scheduling and single-signal-owner
capabilities consistently from API, configure, query and lifetime results.  The native selector
keeps the public signal owner unselected after preflight because the final module/HLE range snapshot
does not exist at process initialization.  Mapped-entry then reserves an internal, externally
unselected owner slot, stages and validates the process-lifetime adapter, publishes its signal-safe
API, and commits `FaultSignalOwner::Fex` as the final transaction record.  A clean failure before the
adapter becomes active may release the reservation and select Current; an adapter that reports
active with an invalid result is quarantined and requires process restart.  `SignalDispatch` still
requires both the committed owner and active API, and its idempotent late-install hook covers a
dispatcher constructed while activation was staged.  Consequently the optional backend remains
deliberately fail-closed until the complete nested-signal and TSO lowering contracts are implemented;
the current base adapter cannot pass preflight and Current remains the required effective backend.

## HLE ABI gate

Every resolved native HLE target receives an explicit ABI descriptor.  Supported classes include:

- INTEGER arguments and RAX/RDX result;
- SSE/SSEUP arguments in full XMM0-7 and XMM0/XMM1 result;
- mixed INTEGER/SSE register allocation;
- overflow arguments on the guest stack with exact alignment;
- MEMORY/sret result and hidden pointer;
- nested guest callback and signal delivery at an HLE boundary.

The bridge must preserve guest SysV callee-saved GPRs and any vector state required by the descriptor.
Always mirroring a scalar result into XMM0 is allowed only when the descriptor says the return class
is SSE.  An integer result cannot be used as an untyped proxy for every return convention.

Mandatory assembly microcases cover XMM0-7, more than eight SSE arguments, mixed classes, `__m128`,
RAX/RDX, XMM0/XMM1, sret, NaN, signed zero, subnormals, nested callbacks and a signal during the return
window.  Sonic music/audio and a Current Bloodborne canary are title-level binary gates.

## Whole-runtime register-residency ABI

Global pinning is not a block-local cache switch.  It is an ABI shared by generated code, every
direct edge, helpers, signal/fault recovery, HLE, callbacks, the scheduler and code retirement.  At
each host PC there is exactly one authoritative guest-state layout: each GPR/vector half and deferred
flag is in a named host register, the machine image, a constant, or is architecturally unavailable.
An immutable host-PC sidecar records that layout.  A direct edge is legal only when the source
live-out layout satisfies the target live-in layout; otherwise an adapter publishes only dirty live
values, reloads required values and establishes the target layout before transferring control.

Current LSX4 already contains important pieces: on Android it has a conditional static-chain ABI
that can pin guest GPR0..14 in 15 host registers, leaving guest GPR15 memory-authoritative; it also
has cross-block GPR/vector data-flow
masks, layout signatures, selective publish/reload helpers, fixed-XMM support, fault-recovery
sidecars, shadow returns and a chain safepoint budget.  `kStaticResidentGprCount=16` names the guest
layout width, while `kStaticResidentGprCachedCount=15` is the actual pinned count; the sixteenth host
array element is `x30`, which the private chain ABI uses for scheduler phase and must not be enabled
as a guest pin.  Therefore the safe next step is not a second global-register mechanism or a blind
15-to-16 change.  It is to prove the existing ABI observer-complete and remove duplicate
machine-state traffic only after that proof.  Faultable-loop write-through, helper flushes and layout
rejection paths are audit targets, not assumptions that can be deleted for FPS.

There is also a concrete aggregate-work trap in the current eligibility split.  The initial shape scan
computes whether the block body is pin-aware, but the Android path subsequently allows the static ABI
for nearly every block.  A static predecessor entering a non-pin-aware body then executes
`emit_publish_static_gpr_pins()` so the old body can read the machine image: up to 15 stores on that
transition.  This preserves correctness but merely moves the block envelope from exit to entry.
Neither “static ABI blocks” nor “resident coverage” is therefore a valid success metric.  The target
architecture keeps one canonical 15-GPR ABI across every compatible block; an unsupported
instruction gets a local effect-described adapter, not a memory-authoritative target body.  The
primary KPI is executed compatible edges with zero GPR load/store plus aggregate native load/store
and instruction counts per fixed guest-op trace.

Every observer is explicit and has a read/write/clobber contract:

1. synchronous fault and asynchronous signal reconstruction at every emitted instruction boundary;
2. C/C++ helpers, HLE calls, syscalls, PLT/veneer calls and nested guest callbacks;
3. scheduler quantum, pause/stop/event delivery and cancellation;
4. SMC invalidation, deopt, cache eviction and executable-range retirement;
5. debugger, profiler, unwinder and crash diagnostics;
6. GPU/DMA/HLE agents that can read the guest machine image;
7. direct CALL/RET, shadow-return mismatch and indirect-branch fallback;
8. thread/process teardown.

The Android host ABI constrains the map.  `x18` is a platform register and Android reserves it for
ShadowCallStack-compatible code, so Box64's convenient guest-R8-to-`x18` mapping cannot be copied.
The current LSX4 static GPR map correctly avoids `x18`.  `x16`/`x17` may be clobbered by veneers/PLT,
and `x30` is LR across `BL/BLR` and carries LSX4 scheduler phase inside shared-chain bodies; none may
carry persistent guest state across an ordinary call.  Standard AAPCS64 preserves `x19..x29`, but vector
`v8..v15` preservation covers only their low 64 bits, so full XMM/YMM state needs an explicit custom
contract rather than the ordinary C ABI.

Arbitrary C++ helpers may observe every field their descriptor names and may clobber every
caller-saved host register.  Before such a call, publish only the named dirty guest inputs and save
live pinned values exposed to ABI clobber; after return, merge declared outputs and invalidate only
facts the helper can change.  A `preserve_all` custom helper is accepted only after post-link
disassembly proves its exact clobber set and nested fault/callback tests cover it.  Signals instead
reconstruct directly from the exact host `ucontext` plus the immutable sidecar; requiring a hot-loop
write-through machine image defeats residency and is allowed only as an explicit fail-closed
fallback for an unproved site.

Pinned FEX demonstrates the minimum shape but also its boundary: its dispatcher publishes explicit
FillSRA/SpillSRA landing addresses and its signal delegator maps fixed host registers out of the
kernel `ucontext`, reconstructs RIP/flags and spills before returning through the dispatcher.  That
is useful evidence for exact fault-site recovery.  It is not complete proof for LSX4: the same path
has an explicit TODO that it does not recover the upper 128 bits of 256-bit SVE/AVX registers, and
restoring a saved context that originated in JIT is marked unsupported because correct SRA
reconstruction is not implemented.  LSX4 promotion therefore requires full 256-bit state and nested
restore tests even when lower-128-bit FEX tests pass.

Fault-site lookup is exact, not “nearest previous marker”.  Every descriptor owns an immutable exact
host PC or checked `[begin,end)` interval and includes a before/after/partial instruction phase and
restart recipe.  A gap is not handled.  This prevents an unrelated host fault after one guest memory
operation but before the next marker from being attributed to the earlier instruction.  Descriptor
lifetime uses a process-lifetime immutable arena for the first production implementation.  A future
RCU/hazard replacement is a separate promotion: it needs explicit handler enter/exit epochs, a lease
covering deferred landing and chained handlers, removal of all slab-head raw pointers, ABA-resistant
generations and a demonstrated grace period.  Setting `active=false`, unlinking a raw region pointer
and immediately freeing its vector proves none of those properties.  For fixed-width A64 memory
operations the normal interval is the exact committed instruction, commonly `[pc,pc+4)`; padding and
gaps fail closed.

This is a concrete defect boundary, not a theoretical preference.  Current
`ResolveSignalFaultPublicationInRegion()` binary-searches only `host_offset` markers and returns the
nearest preceding publication for every PC anywhere inside the whole block range.  Its site record
has no end offset or phase.  Current depot destruction first marks/unlinks raw region pointers and
then lets the owning vectors/`unique_ptr`s disappear without a signal-reader epoch or grace period.
Therefore neither lookup precision nor reclamation lifetime may be assumed when enabling broader
resident/faultable regions; both must fail closed until the interval and reader-lifetime tests pass.

Retirement cannot reclaim code or its layout sidecar while a signal reader, chain edge or deferred
landing can still refer to it.  Direct/RET chains also retain a finite safepoint budget: periodic
return to the scheduler is semantic event-delivery behavior, not optional instrumentation.  A
shadow-return mismatch publishes the exact source layout before the generic fallback observes it.

Promotion requires generated-code differential testing against an all-memory baseline.  Inject a
fault or signal at every instruction boundary and every possible dirty mask; cross every source and
target layout; force helper clobbers, nested callbacks, edge withdrawal, SMC retirement and shadow
return mismatch; and verify upper YMM/VEX-zero state as well as GPRs/flags.  Negative controls remove
one publish, reload, sidecar field, safepoint or ABI save and must fail.  CI disassembly then proves
that compatible direct edges contain no publish/reload traffic while mismatched edges contain the
minimal adapter.  Only aggregate retired instructions and machine-state load/store counts qualify
the optimization before title FPS is measured; moving work into helpers or signal paths is a failure.

### Compiler-theory anchor: state maps and effect summaries

This sidecar is the emulator analogue of a precise stack/deoptimization map, not debug information.
LLVM stack maps associate an instruction address with each runtime-required live value as a register,
stack/memory location or constant; the runtime, not LLVM, assigns those locations architectural
meaning.  They also record live-out registers and treat a patchpoint conservatively around memory so
a faulting load cannot be hoisted across the state boundary.  LSX4 needs the same exactness at every
fault/deopt/async-observer site, extended with guest width, deferred flags, instruction phase and
code-generation epoch.

Helper effects follow a conservative lattice rather than handwritten booleans.  The top element is a
full observer/full clobber that can read, replace or fault with the complete machine state.  Narrower
descriptors independently declare guest reads, guest writes, host clobbers, memory effects, possible
exception/deopt/callback and restart phase.  An unknown helper, indirect HLE target or callback is
top.  QEMU TCG's published contract is the useful minimum precedent: helpers save all globals by
default; `NO_WRITE_GLOBALS` removes the reload only; `NO_READ_GLOBALS` removes publication and implies
no writes; `NO_SIDE_EFFECTS` additionally permits dead-call elimination and forbids exceptions.
LSX4 may be more precise, but never less conservative without proof.

The LSX4 descriptor is a product lattice with independent fields: guest machine-read and direct-arg
use masks; success/fault/partial write masks for 16 GPRs, XMM/YMM halves, flag subfields, RIP/RSP,
FS/GS, MXCSR and x87; observer class (`None`, subset, full, replace); host GPR/SIMD/NZCV/FPCR and
platform clobbers; memory class/order/fault type; control class (return, throw, no-return, callback,
block, schedule); and restart phase (`Before`, `Prepared`, `InHelper`, `Committed`).  Join is bitwise
union/max.  At a call, publish `dirty intersect (machineReads union observerSet union restartSet)`,
save `liveHost intersect hostClobbers`, and reload/invalidate only outcome writes that remain live.
A context replacement or RIP/RSP replacement discards every residency fact and exits through the
canonical dispatcher.

Stable native target IDs are relocation/cache identities, not effect summaries.  The current JIT has
many raw generated `BLR` sites, while `JitInstructionHelperAbi` contains only two preservation
booleans and the selective path derives effects from decoded operand masks.  That omits
full-observer/control/memory/restart facts and fields such as RIP, FS/GS, MXCSR and x87.  Likewise the
`jit_leaf_direct_safe` HLE allowlist describes direct-call shape, not purity: it includes potentially
blocking synchronization, GPU submit and memory operations.  `PublishedLiveState()` is a raw machine
image view used by signal capture and full-state replacement, so a dirty resident value can be read
stale or later overwrite a replacement unless phase/generation and observer publication are unified.

The data-flow solution is a backward fixed point over the native region CFG.  At each edge compute
architectural `live-in = use union (live-out - def)`, then join it with the observer/effect set and the
precise-exception restart set.  This analysis must be solved over the complete promoted region graph;
unknown successors, truncated exploration and unresolved indirect edges conservatively contribute
live-all rather than manufacturing a dead value at the analysis horizon.  Async signals are live-all
regardless of normal successor liveness, because the guest handler/debugger may inspect or replace the
full context.  A synchronous fault may avoid hot publication only when its exact PC map can reconstruct
every pre-fault or progressive-checkpoint architectural value and phase from `ucontext`; otherwise the
instruction is not eligible.  This distinction prevents using ordinary liveness to delete state that
is dead on the normal path but observable on a signal or deopt path.

Descriptor generation must be mechanical from one registry used by decoder, lowering, helper thunk
and tests.  CI fails an unregistered helper, conflicting declaration, generated callsite without a
descriptor or a descriptor whose deliberately clobbering test does not observe its stated effect.
The current main emitter contains 31 raw `BLR` emission sites, so migration coverage is enumerated
from emitted callsite IDs rather than a hand-maintained estimate; a new raw `BLR` is a build failure.
Post-link disassembly and counters prove that removed traffic disappeared from the whole process;
wall-clock/FPS is a later acceptance measure, never the proof of the transformation.

Host ISA extensions are consequences of that proof, not substitutes for it.  FlagM/FlagM2
(`RMIF`, `SETF8`, `SETF16`, `CFINV`) may shorten a proved x86 flag formula only after the effective
HWCAP route, defined/undefined x86 flag mask, ARM NZCV liveness and actual emitted bytes are in the
translation validator.  RCpc may shorten a proved memory-order template but does not by itself
implement x86 TSO.  SVE/SVE2 may lower a complete guest SIMD operation only with per-thread VL,
signal-frame and ABI coverage plus an identical NEON/baseline fallback; changing vector domains can
cost more than the saved instructions.  `TBL`/`TBX`, NEON REP/string primitives and custom `.S`
helper stubs are admitted by the same whole-trace aggregate-work and precise-state gates.  Counting
an available instruction or matching another emulator's mnemonic is never evidence of a speedup or
semantic equivalence.

Every optimized lowering also gets bounded translation validation.  The validator consumes the
decoded guest operation/profile, initial symbolic architectural state and the *actual emitted Arm
bytes*, then proves refinement of all defined guest outcomes: result registers, defined flags,
memory events, guest exception descriptor, restart RIP and instruction phase.  Loops are checked per
bounded region/induction summary and reported as unproved when the bound is insufficient; timeout is
never success.  This follows the useful discipline of Alive2 and scalable binary-lifter validation:
validate each produced translation rather than trusting the compiler that produced it.  It is paired
with hardware differential fuzzing because formal ISA models can omit implementation-defined or
underspecified behavior.  A reference emulator agreeing with LSX4 is corroboration only; real AMD
and Arm results plus the frozen profile decide disputed cases.

The executable oracle is three independent gates, all of which must agree:

1. **Formal memory gate.** Pin the herdtools7 revision and hashes of `x86tso.cat`, `aarch64.cat`,
   `aarch64hwreqs.cat` and instruction semantics.  A manifest pairs each source and lowering litmus,
   projects target events onto the same defined guest-architectural observables under identical
   mapping, fault and fairness assumptions, and always requires target-to-guest refinement; it additionally
   requires equality only for a lowering advertised as exact/non-strengthening.  A labelled
   conservative path may have a strict subset, but must pass separate progress/fairness and
   exception gates.  Every case includes a weak mutant that must expose a guest-forbidden outcome.
   Timeout, zero executions, skip or a mutant that remains inside the guest set is failure.
2. **AMD precise-fault gate.** Run raw bytes in one forked native-AMD child per case with
   `sigaltstack`/`SA_SIGINFO`; write a fixed binary record containing signal identity/address,
   RIP/RFLAGS/GPR/XSTATE, trap/error/CR2 fields and before/after memory.  Legacy SSE cases cross
   the ring-3 controllable state (`MXCSR.MM`, `MXCSR_MASK[17]` and RFLAGS.AC) and records effective
   CPUID/MisAlignSse, CPL and CR0.AM.  Varying CR0.AM, CPL, delivery policy or a synthetic/TestKit
   profile requires a separate privileged bare-metal, kernel or validated VM fixture; a normal child
   process cannot manufacture those states.  Separate observation from a repair-and-`sigreturn`
   exact-once run.  Record vendor/family/model/microcode/kernel/hypervisor.  A VM/native machine is
   normative only for the exact selected virtual/physical profile and verified exception-delivery
   contract; other AMD and the local Intel host remain differential controls.  Currently there is no
   local normative selected-profile AMD fixture.
3. **Actual-byte AArch64 gate.** At executable-code commit, record guest bytes, decoded
   LOCK/width/fault contract, effective HWCAP path, exact committed Arm bytes, sidecar and hashes.
   Disassemble with pinned NDK `llvm-objdump`, match an allowlisted proved template, execute those same
   bytes on supporting hardware and require a nonzero route counter.  Inject a fault at each marked
   host PC and compare reconstructed state/memory against the interpreter; stale/missing sidecar
   mutants must fail closed.

The local toolchain already has Python, ADB, NDK Clang/`llvm-objdump`, CMake, Ninja and Docker CLI.
It does not currently have herd7/litmus7/diy7, qemu-aarch64 or a native AMD oracle, and local WSL is
not a reliable execution substrate.  Those are explicit missing prerequisites, not silently skipped
tests.  The harness parses schema, `ok`, execution/path counters and artifact hashes rather than a
process exit code; this specifically prevents the existing `runtime_selftest_dump` false green where
a non-null JSON object can return success while reporting `"ok": false`.

Migration starts with zero semantic change: introduce one manifest/wrapper covering every generated
call and mark every entry Top; reject raw unregistered `BLR`; hash the manifest into native-cache ABI.
Then narrow pure/control helpers, parametric arithmetic, and finally precise fault/partial-progress
helpers.  HLE eligibility becomes a predicate over the resolved target, effect summary and DSO build
identity; callback, replacement or blocking paths remain full observers until proven.  A runtime
shadow mode executes the full-boundary and selective plans against poisoned omitted fields, injected
host clobbers, signals at all four phases, nested callbacks and state replacement; any mismatch
disables that descriptor.  Promotion requires coverage of every callable ID and outcome, a hit for
the unknown/Top path, deletion-of-one-summary-bit mutants, and disassembly showing exactly one
boundary plan rather than both old and new work.

## Build and packaging gate

- Rebuild both libraries for ARM64 API 26 or make availability explicitly API-gated without affecting
  Current on API 26-28.
- Fix RUNPATH to `$ORIGIN`; verify ELF class, machine, Android API note, DT_NEEDED, RUNPATH, exported
  ABI table, build IDs and absence of unexpected constructors.
- Store exact FEX base, reviewed patch hash, NDK, libc++, flags and source manifest in the APK/status.
- APK inclusion is opt-in; Current-only packaging remains supported.
- Preserve MIT/BSD/Boost notices and generate an SBOM for the actual linked pair.
- API 26 compatibility uses `posix_memalign`, not absent `valloc` or API-28 `aligned_alloc`, plus
  syscall wrappers with runtime `ENOSYS` fallback for `getrandom`, `memfd_create` and affinity.
  Post-link acceptance audits undefined symbols against API-26 stubs and verifies `DT_NEEDED`.

## Promotion gates

1. Host ABI selftests and malformed capability/table-size tests.
2. Integer, full SIMD/aggregate, TLS and reentrant callback microtests.
3. Strongmem litmus suite: ordinary TSO ordering, fences, LOCK, aligned/unaligned atomics and SMC.
4. Signal owner tests including delayed 60-second reclaim attempt and nested HLE signal.
5. Range tests at 0/1/64/65/1000 plus concurrent signal reader and invalidation.
6. OpenOrbis HLE/thread smoke, then historical Fluster milestones.
7. Current canary with FEX packaged but never loaded.
8. Bloodborne same save/scene/route at 960x544: exact presented frames and aggregate CPU/GPU work;
   immediately force-stop after the 14-second sample.

Until every correctness gate passes, the UI must say `FEX unavailable` with the native reason and
block launch.  Once activated, the HUD must still display both requested and effective backend plus
the FEX build/cache identity so no benchmark can be mislabeled.
