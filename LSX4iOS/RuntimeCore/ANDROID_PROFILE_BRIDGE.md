# Android CPU profile handoff

## Current ordinary launcher mapping (2026-10-03)

For the FEX-enabled 128.9034 candidate, the paired-Mac library relay accepts
`--launch-policy /path/to/receipt.json --cpu-backend auto --cpu-profiles ...`.
The receipt has exactly `schemaVersion: 1`, `profile: "android-mapped-v1"`,
`bundleIdentifier`, `bundleVersion`, and the signed app's `binarySha256`.
The launcher verifies these against the local app and its required build layers
before any phone operation, then also verifies the installed version before
launch/attach. This does not claim a device-side binary hash measurement.

One common policy enables managed OPTIONS, bounded Mono self-check object access,
Precise readback, Android source/output presentation, Android Vulkan submission
policy, quiet rendering diagnostics, and the shared physical-DRS quarantine.
There are no per-title entries in this policy and it cannot be combined with
the diagnostic `--verification` file. The exact policy is archived per launch.
CPU selection still consumes the original exported Android profile, without
rewriting it. The tested preparation capacities are 512 MiB for FEX and 128 MiB
for Current; explicit capacity overrides remain separate from CPU tuning.
Without `--launch-policy`, existing legacy and diagnostic launches are unchanged.

This launcher mapping has host contract coverage; physical library-tap acceptance
must be recorded separately from device_probe's gameplay observations. The relay
must remain stopped while a physical catalog controller owns the phone. Crash
auto-restart remains disabled unless explicitly requested through `--auto-recover`.
Earlier build54/Current-only notes below describe historical checkpoints.

`export_android_cpu_profiles.py` compiles the current
`android-app/.../CpuTuningSettings.java` together with host-only Android API
adapters. It calls the original `materialize` method: GameDB, named presets,
Custom overrides, All games and its exceptions have one source of truth.
It does not rewrite Vivo preferences or resolve Auto using the Mac CPU.

Supply a local JDK, an exported CPU preference XML and the library catalogue:

```sh
python3 LSX4iOS/RuntimeCore/export_android_cpu_profiles.py \
  --jdk /path/to/jdk/Contents/Home \
  --preferences /path/to/cpu-preferences.xml \
  --catalog /path/to/catalog.json \
  --output /path/to/android-cpu-profiles
```

Pass the output directory as `library_device_host.py --cpu-profiles ...`.
The manifest binds each INI to the current resolver source and its checksum.
Regenerate after a resolver update; stale or modified profiles fail before
launch. Tests use `device_probe.py --cpu-profile /path/to/TITLE.ini`.
Both paths copy and read back the file while owning the device lease.
An unconfigured launch publishes an empty/default profile, preventing reuse of
the preceding game's configuration.

Before common runtime initialization, the Apple launch adapter calls
`executor_lsx4_runtime_set_guest_cpu_backend_v2(Current, ..., title)`.
That existing shared API loads the INI, resolves Auto for the actual iPhone,
configures the memory/JIT/HLE policy and then initialization locks the backend.
Importing a profile does **not** provide FEX on Apple. Vivo's requested backend
is recorded separately in the export manifest.

The Android Giga instruction limit exposed a shared Current-JIT inconsistency:
512 instructions were allowed while byte witnesses/hash/compaction scratch
storage accepted only 2048 bytes. All six scratch buffers now derive their
capacity from the common instruction limit times the x86 instruction byte
limit. The native fixture executes a 3011-byte block, modifies its final
immediate beyond byte 2048, and requires the new result. It reproduces the
original rejection before the fix and passes under PS4 and PS5 register policy
after the fix. Bounds and self-modifying-code checks remain enabled.

JIT permission preparation uses bounded 4-MiB slabs and separate RX/RW aliases.
The process pool permits up to 256 MiB; ordinary launches begin with 64 MiB.
Observed exhaustion can trigger a fresh-process retry with more capacity.
Published code is never recycled while guest threads could still reference it.

Physical gameplay and performance acceptance remain separate from profile
parity and CPU fixtures. Full Android golden-title and PS5 physical acceptance
are required before accepting the shared CPU change for an Android release.

Build54 raises the bounded Apple maximum from128 to256MiB. A separate host
code inventory for GoNNER recorded184,287,448 bytes across267,217 distinct
native ranges (entry stubs included). The128MiB physical attempt exhausted;
this is measured demand, not evidence of a recompilation leak. Ordinary
launches still start at64MiB and preserve the measured per-title capacity.
No performance claim comes from the perf-map diagnostic run. Physical builds54
and55 prepared256MiB successfully, but GoNNER ended near49seconds. The55
progress trace reached3,498,413,136bytes footprint with41,579,440bytes of the
process memory allowance remaining at45seconds. No OS termination report was
found, so memory pressure is strong evidence rather than a confirmed kill cause.
Build57 tests deferring the unused512MiB BDA table allocation; capacity256MiB
is not marked as a successful learned gameplay setting yet.
