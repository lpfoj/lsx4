# Mandatory device-performance preflight

> [!IMPORTANT]
> Before **every** device, FPS, frame-time, stutter, or playable measurement, run
> [`scripts/preflight-vivo-performance-test.ps1`](../scripts/preflight-vivo-performance-test.ps1)
> from the repository root. Android Studio `adb` process-trackers and known profiler loads must
> be gone before the game starts. A result collected without a successful preflight is
> contaminated and **invalid** for comparison, acceptance, or regression attribution.

## Required invocation

From `D:\LProj\PetProj\lsx4-jit-independent-wt`:

```powershell
& .\scripts\preflight-vivo-performance-test.ps1
```

Run it before the baseline and again before every candidate run. Do not reuse a clean result from
an earlier launch: Android Studio, an IDE integration, or a profiler can recreate background work
between runs.

The test may begin only after the preflight reports success. If it reports a remaining process
tracker, profiler, or other known core-consuming load, stop there and clean the host/device state;
do not record FPS from that run.

## Why this is a hard validity gate

The 2026-08-24 Bloodborne investigation found **1,605 (`1605`)** stale Android Studio process-tracker
processes. On the device, the contaminated state showed approximately **400–600% system CPU**,
only about **1.2 GiB free memory**, and about **7.1 GiB swap** in use. In the same gameplay scene,
cleanup changed the observed result from **11 → 29 FPS**. There was **no emulator code change** and
**no emulator setting change**.

That A/B establishes the rule: background tooling can consume enough CPU and memory bandwidth to
look like a severe emulator regression. The contaminated 11 FPS sample does not measure LSX4, FEX,
JIT, the Vulkan driver, or a game patch. It measures LSX4 plus an uncontrolled host/device load.

## Measurement contract

For every recorded result:

1. Run the preflight immediately before launch and retain its output with the test evidence.
2. Require zero stale Android Studio `adb` process-trackers and zero known active profiler loads.
3. Keep the game scene, backend, Vulkan driver and version, settings, patches, cache state, device,
   temperature, and sampling window explicit.
4. If background load appears during capture, mark the sample invalid and repeat from preflight.
5. Never attribute an FPS delta to code, FEX/JIT, Vulkan, audio, or game patches until both sides of
   the A/B passed this gate.

This preflight is measurement hygiene, not an emulator optimization. It must not be replaced by a
code workaround, CPU affinity hack, or a claim based on an unclean run.
