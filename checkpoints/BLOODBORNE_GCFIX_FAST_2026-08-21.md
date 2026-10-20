# Local Bloodborne GC-fix checkpoint — 2026-08-21

This checkpoint is intentionally local-only. Nothing from it has been pushed.

## Git state

- Parent branch: `checkpoint/bloodborne-gcfix-fast-20260821`
- Parent base before checkpoint: `a09197bf24a76b91a8415bb33d4e45f7bfa67948`
- Funnel branch: `checkpoint/bloodborne-gcfix-fast-20260821`
- Funnel commit: `fcf5102e5fceaebc9169361cfe8738f8aba00984`

The checkpoint commits all current tracked project changes and all untracked
source, build-system, documentation, test and tooling files. Historical
profiles, screenshots, disassemblies and compiler cache directories are not
committed; the single frozen control screenshot listed below is included.

## Frozen binaries

Directory: `D:\LProj\PetProj\.local-checkpoints\bloodborne-gcfix-fast-20260821`

- `lsx4-bloodborne-gcfix-fast.apk`
  - SHA-256: `5F48CC2044944339C4CDE64B20772729546EF48CA1D39E161E23D611F48F52A2`
- `libvulkan_lsx4_turnip-d6.so`
  - Mesa 26.2.1, Vulkan 1.4.354
  - SHA-256: `B56A4CEC9AF33AF61EC068EFB1D8673B54BB325635CA7A2300DB77DF82D752CD`
- `bloodborne-stable-frame.png`
  - SHA-256: `186D2C2A44AC722FA7A9D01F0DCCFB0C0DF2A4886529DDCBA144393908B994C2`

The APK contains the pinned FEX Android artifacts, the checkpoint executor and
the pinned Turnip D6 library.

## Validated device state

- Device: Vivo V2546A, serial `10AG263F1B001HM`, Adreno 840
- Title: Bloodborne `CUSA03173`
- CPU backend: FEX (`managed_guest_cpu_backend=1`)
- Driver: Turnip enabled (`managed_turnip_vulkan_driver=true`)
- Resolution mode: `4`
- Performance HUD: enabled
- Managed fast guest memory: enabled
- Managed relaxed FP fusion: enabled
- Managed OEM/root performance policy: enabled
- Persistent JIT cache: enabled
- Submit-readback prefetch: enabled; the temporary disable marker is absent

Graphics preference values in this exact device checkpoint:

- depth of field disabled
- motion blur disabled
- anti-aliasing disabled
- SSAO disabled
- chromatic aberration disabled
- dynamic shadows disabled
- anisotropy mode `0`

Active runtime markers:

- `run-jit-persistent-jit-cache`
- `run-live-quiet`
- `run-disable-dof`
- `run-disable-motion-blur`
- `run-disable-anti-aliasing`
- `run-disable-ssao`
- `run-disable-chromatic-aberration`
- `run-disable-dynamic-shadows`
- `run-bisect-disable-jit-tiered-guard-direct-chain`
- `run-bisect-disable-jit-preserve-all-helper-abi`
- `run-bisect-enable-jit-fixed-xmm-dirty-observer`
- `run-diagnostic-present-stage-profile`
- `run-diagnostic-submit-mutex-profile`
- `run-ab-turnip-imgui-persistent-map`

## Correctness result at checkpoint

- Bloodborne reaches gameplay with the HUD identifying
  `GPU Turnip Mesa 26.2.1 VK 1.4.354` and `FEX blocks`.
- The former progressive grey-frame overwrite and subsequent device loss are
  absent in the validated run.
- Buffer GC uses cache-owned residency, retains GPU-dirty and pending-readback
  generations, and cannot instantiate the unsafe asynchronous delete path.
- Texture GC uses its incremental cache-owned counter; whole-device usage is
  only a near-budget emergency signal.
- Targeted coherency/transfer contracts: 20 tests passed.
- Native Android executor and debug APK builds passed.

## Restore

1. Switch the parent repository to
   `checkpoint/bloodborne-gcfix-fast-20260821`.
2. Ensure `funnel-arm` is at commit
   `fcf5102e5fceaebc9169361cfe8738f8aba00984` (or switch its matching local
   checkpoint branch).
3. Reinstall the frozen APK with:

   `scripts\install-vivo-apk.ps1 -ApkPath D:\LProj\PetProj\.local-checkpoints\bloodborne-gcfix-fast-20260821\lsx4-bloodborne-gcfix-fast.apk -DeviceId 10AG263F1B001HM -TimeoutSeconds 300`

4. Restore the preference and marker values listed above before comparing
   performance.
