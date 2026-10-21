# LSX4 repository instructions

## Contributions and AI-assisted changes

Read `CONTRIBUTING.md` before preparing a public change. Use
`docs/GAME_REGRESSION_SAFETY.md` to identify sensitive ownership boundaries and the
appropriate evidence. These guides supplement the mandatory contracts below; they
do not relax their gates. Keep private branches, credentials, personal diagnostics
and game content out of public commits. Model output is unreviewed until verified.

## Playable and performance regression contract

Before changing Android CPU profiles, FEX/JIT geometry, HLE/AIO lifecycle, guest-worker
admission, Mono safepoints, or HLE callback memory, read
`docs/PLAYABLE_PERFORMANCE_REGRESSION_PREVENTION_2026-08-31.md` completely.

An APK is acceptable only when the same final build passes both axes: the affected
golden titles reach real gameplay, and established performance anchors retain their
matched save/scene throughput. Boot/menu is not playable proof. Making another title
playable does not excuse a Bloodborne performance regression; improving Bloodborne does
not excuse an AIO/Mono/Unity/Isaac correctness regression. Named presets must preserve
hard GameDB title constraints, while Custom profiles preserve explicit user overrides.

Shared JIT/HLE changes must also pass PS5 Current-JIT independently of PS4/FEX. The
PS5 Current HLE bridge publishes return state through `LsxMachineImage`; until an
explicit resident-register invalidation handshake exists, the PS5 build must keep
guest GPRs memory-authoritative under `LSX4_PS5_DESKTOP_PATH`. Do not remove that
guard based on PS4/FEX success. The PS5 gate requires a real title with native
coverage (`native>0`, `rejected=0`), growing frame/block counters, no helper marker,
and no fatal signal. Dreaming Sarah (`PPSA02929`) and Void Terrarium++ (`PPSA03061`)
are the current Vivo regression pair.

## Vulkan portability contract

These rules apply to every PS4 and PS5 renderer change.

1. Treat `funnel-arm/src/video_core/renderer_vulkan/vk_capability_profile.h` as the only source of
   Vulkan capability routes and mobile driver quirks.
2. A fast path is available only after checking API promotion or an enabled extension, its feature
   bit, and every required device entry point. API version alone is never sufficient.
3. Every optional core/KHR path must have a complete legacy or unavailable route. Never leave half
   of a promoted feature active.
4. Renderer code may consume `CapabilityProfile` and `HasQuirk()`. Do not add vendor IDs, Arm/Mali,
   Qualcomm/Adreno, or driver-name branches outside profile construction.
5. Do not add raw calls to promoted Vulkan commands. Dynamic rendering and push descriptors may be
   used only in the existing route-aware files. Synchronization2 calls rely on the dispatcher
   fallback installed by `vk_instance.cpp`.
6. Never add title-specific Vulkan workarounds. A genuine driver defect must become a named
   `CapabilityQuirk`, have a safe fallback, and be documented in
   `docs/ANDROID_VULKAN_CAPABILITY_BRIDGE.md`.
7. Run `python tools/check_vulkan_capability_bridge.py` and build both
   `liblsx4_executor_android.so` and `liblsx4_executor_ps5_android.so` after changing Vulkan code.
8. Validate at least one legacy Mali profile and one current Adreno profile. Do not disable an
   Adreno fast path merely to make Mali pass.

The build target `lsx4_vulkan_capability_guard` and CI workflow enforce these rules. Do not weaken
their patterns or allowlists to make an unrelated change pass; route the change through the bridge.

## Vivo APK installation

For the dedicated Vivo `10AG263F1B001HM`, never leave the package-installer confirmation to the
user. Install through `scripts/install-vivo-apk.ps1`; it waits for Vivo's package verification,
checks `com.android.packageinstaller:id/deleted_file_state_cb` when required, and presses the
enabled Install/Continue button itself.

```powershell
& '.\scripts\install-vivo-apk.ps1' `
  -DeviceId '10AG263F1B001HM' `
  -ApkPath '.\android-app\app\build\outputs\apk\debug\app-debug.apk' `
  -TimeoutSeconds 180
```

Do not use a plain blocking `adb install -r` for this device: it can wait indefinitely for the OEM
confirmation dialog. A successful unattended install must end with both `Success` and
`Vivo package-installer confirmation completed automatically.`
