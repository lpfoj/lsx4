# FEX-2608 upstream hooks required by the LSX4 Android backend

> [!IMPORTANT]
> Before any device/FPS/playable measurement, run `scripts/preflight-vivo-performance-test.ps1`;
> stale Android Studio process-trackers and known profiler loads make the result invalid. See the
> mandatory [performance-test preflight](PERFORMANCE_TEST_PREFLIGHT.md).

Date: 2026-08-12  
Pinned FEX revision: `e869aa644a16e4332cdc15c1ea0b4d13d482385d` (`FEX-2608`)  
Status: three upstream contracts are required before the adapter may advertise the corresponding
production capabilities.  This file specifies behavior and acceptance tests; it is not a patch to
the pinned FEX source.

## What is already executable

The API-26 AArch64 build produces a real `libFEXCore.so` and an LSX4 context bridge.  The staged
Android smoke creates a private FEX context, compiles a mapped x86-64 fragment, executes it on an
Android device and verifies its reserved syscall exit and request identity.  The signal adapter also
has fixed-layout JIT metadata, a bounded AArch64 context capture and a deferred normal-context
landing.  None of those facts proves the complete PS4 contract.

Production activation deliberately remains fail-closed.  In particular, the adapter must not claim
`LSX4_FEX_CAP_EXACT_GUEST_CPU_PROFILE_V6`, `LSX4_FEX_CAP_SINGLE_SIGNAL_OWNER`, guest signals or
signal-dependent scheduling until all requirements below are implemented and pass their tests.

The clean 2026-08-12 API-26 build completed all 168 FEXCore build actions, all adapter/bridge and
four Android smoke targets, six of six host tests, source-snapshot verification and the final ELF/
symbol/provenance manifest verification.  On Vivo `10AG263F1B001HM`, the fresh staged JIT returned
the reserved exit `0x2608f3e5`.  The exact-profile probe executed successfully and then returned the
intentional readiness code 77 with:

```text
leaf0=00000016:756e6547:6c65746e:49656e69 vendor=GenuineIntel
leaf1=000a0661:01010800:bcc8330d:278bfbf3
xcr0=0000000000000007 mxcsr=00001f80 mxcsr_mask=0000ffc0
```

The per-run APIC value in leaf 1 EBX is intentionally not treated as a stable profile identity.
The vendor, maximum basic leaf, feature leaves and MXCSR mask prove that stock FEX semantics are not
the frozen PS4 profile.  The production adapter therefore failed preflight before activation with
`api-table-missing-required-contract`; an independent lifecycle mutant that omitted only the new
profile requirement failed with `ps4-preflight-must-require-exact-guest-cpu-profile-v6`.

## 1. One immutable guest CPU profile

### Required behavior

FEX needs a public context API that copies a sized immutable guest profile before `InitCore()` and
before the first thread is created.  A second or late installation must fail.  The context owns the
copy for its complete lifetime; it must never retain borrowed LSX4 callback storage.

The installed profile is the only source for:

- interpreted and JIT-executed `CPUID(function, subleaf)`;
- register-allocation constant folding of CPUID, including the subleaf;
- interpreted and JIT-executed `XGETBV`;
- initial and inherited MXCSR state and the `MXCSR_MASK` emitted by FXSAVE/XSAVE and signal frames;
- validation assumptions and persistent code-cache identity.

Unknown functions/subleaves follow the profile's declared policy.  They must not fall through to a
host-derived or stock FEX table.  CPUID folding is allowed only when both function and subleaf are
compile-time constants, and it calls the same provider used by the runtime call cell.  The profile's
128-bit identity is incorporated in the FEX code-cache key; a cache entry with a different identity
is rejected before executable mappings are published.

For the current LSX4 PS4 profile the identity is
`f0e351bb9bbb26d161e82729ec820a6c`.  Representative required results are:

- CPUID 0: max basic leaf `7`, vendor `AuthenticAMD`;
- CPUID 1: EAX `0x00800f12`, EBX `0x00000800`, ECX `0x7ed8320b`, EDX `0x178bfbff`;
- XCR0: `7`;
- MXCSR reset: `0x1f80`, writable/save mask: `0xffff`.

The likely integration surface is the public Context API and `ContextImpl`, `CPUIDEmu`, the RA pass
created by `PassManager::AddDefaultPasses`, the JIT CPUID/XCR call cells, vector FXSAVE/XSAVE and
signal-frame state.  Merely changing the runtime CPUID callback is insufficient because the RA pass
currently folds values from its own CPUID object.

### Acceptance tests

1. Install two deliberately conflicting fake profiles in separate contexts and prove interpreted,
   JIT runtime and constant-folded CPUID all follow their own context.
2. Cover a constant function with dynamic subleaf and dynamic function with constant subleaf; neither
   may be folded using the wrong leaf.
3. Execute XGETBV and FXSAVE, then check MXCSR and MXCSR_MASK; repeat through guest signal delivery
   and sigreturn.
4. Build a cache with profile A and start with profile B.  No block from A may execute.
5. Reject profile installation after core/thread initialization.
6. Run `lsx4_fex2608_staged_cpu_profile_probe`; only the exact values above may produce PASS.

Only after these tests pass may the adapter claim
`LSX4_FEX_CAP_EXACT_GUEST_CPU_PROFILE_V6`.

## 2. Permanent dispatcher continuation after a signal landing

### Required behavior

The current deferred landing can keep an executable-range generation pinned while the interrupted
guest state is restored.  The pin cannot be released in the POSIX handler or immediately before
`sigreturn`: another thread could retire and unmap the translated block before the CPU fetches the
restored PC.

FEX therefore needs a per-thread, one-shot block-exit continuation API.  The adapter arms a token
after it has reconstructed the guest state.  Generated control-flow exits through a permanent
dispatcher trampoline outside all retireable JIT mappings.  That trampoline atomically claims and
clears the token, calls the registered normal-context continuation exactly once, resets transient
call/return state as required, and then resumes at the ordinary dispatcher loop.  The no-token fast
path is one predictable test and otherwise preserves current code generation.

Every way out of translated code is covered: direct and indirect branch, call, return, loop exit,
cache miss, HLE/syscall, deoptimization, callback return, fault and thread stop.  Callback return has
its own epilogue if sharing the ordinary exit would expose a different state layout.  Cancellation
is explicit and idempotent.  Process teardown cannot leave a token or executable pin live.

### Acceptance tests

1. Inject a signal at every exit class above and assert exactly one continuation callback.
2. In the callback immediately retire and unmap the interrupted block; execution must already be in
   the permanent dispatcher mapping.
3. Race block retirement and cache reuse, including generation/address ABA.
4. Exercise nested signals, a callback during HLE, cancellation and thread stop.
5. Prove the callback never executes in the kernel signal handler and never while PC remains inside
   the pinned range.

Until this hook exists, the adapter may retain a pin until a known safe thread boundary for
diagnostics, but that fallback must not advertise production guest-signal ownership.

## 3. Transactional normal-context handling of guest memory operations

### Required behavior

The POSIX signal handler only classifies and captures.  It must not dereference a guest operand,
take the split-lock mutex, allocate, log, retry an atomic loop or call the current unaligned helper.
The operation runs later in normal context with the owned synchronous-fault signals unmasked.

The frontend supplies a stable VMA snapshot/lease covering the complete widened read/write range.
While the lease is held, `munmap`, `mprotect`, remap and backing replacement for that range are
excluded by the same memory-manager protocol.  Every writer that can overlap a split operation uses
the same serialization domain.

The normal-context request describes operation kind, width, address, input operands, comparison
value, count, guest RIP and restart phase.  Before any write it validates every source and destination
byte under the lease.  A store or RMW that spans pages/native atomic containers either commits the
complete architectural operation or reports the exact guest fault with memory unchanged.  A fault
after a partial write is forbidden; rollback that could overwrite a concurrent writer is not an
acceptable implementation.  Known aligned and safely contained operations remain on their native
fast paths.

The response is explicit: completed result and flags, guest fault descriptor/address and restart RIP,
or a host-fatal invariant violation.  An unexpected secondary hardware fault lands in a fixed
failure continuation; it never re-enters the same helper recursively.

### Acceptance tests

1. STLR/CAS/RMW/CASP-style cases across `RW|PROT_NONE`, `RW|RO` and mapped-page boundaries: exact
   guest fault and no change to the writable first page.
2. Concurrent `mprotect`, unmap/remap and address-generation ABA stress under the real LSX4 memory
   manager.
3. Overlapping locked operations and ordinary writers prove the selected x86 serialization rule.
4. Nested/secondary faults prove bounded landing, no leaked lease/split-lock/pin and no C++ unwinding
   across a signal context.
5. Retirement waits through landing, operation completion and the dispatcher continuation from
   section 2.

## Promotion order

1. Merge and test the immutable CPU profile hook; enable only its capability and rerun the staged
   CPU probe on every supported device class.
2. Merge the permanent dispatcher continuation and prove executable-pin lifetime independently.
3. Merge the transactional memory-operation handler and VMA lease protocol; run the fault/race suite.
4. Only then enable guest signals, guest futex scheduling and single signal ownership together, run
   Current-canary tests, and attempt a minimal PS4 title launch.

Any missing hook, cache/profile mismatch or failed test leaves Current effective and reports a stable
native fallback reason.  There is no in-process fallback after FEX activation.
