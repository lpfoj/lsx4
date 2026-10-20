# Android Vulkan upscalers

Performance evidence is valid only after `scripts/preflight-vivo-performance-test.ps1` passes.
See `PERFORMANCE_TEST_PREFLIGHT.md`; any run that skips or fails this gate is invalid.

LSX4 performs resolution reconstruction between the final guest color target
and host UI/post-processing. `Internal render resolution` is the input extent.
`Upscaled output resolution` is the shared destination contract used by every
active backend; it does not imply that the algorithms use the same filter or
the same controls. An algorithm is bypassed when both source axes are not
smaller than the output.

## Modes

The persisted `Config::UpscalerMode` values are a stable Android/native ABI:

| Value | Backend | Implementation |
|---:|---|---|
| 0 | Off | no host reconstruction |
| 1 | Auto | SGSR1 on capability-profiled Adreno; FSR1 otherwise |
| 2 | Bilinear | one-pass sampled-image compute |
| 3 | Sharp Bilinear | nearest prescale region plus bilinear interpolation |
| 4 | Bicubic | 9-sample Catmull–Rom, Mitchell–Netravali or B-spline reconstruction |
| 5 | FSR1 | AMD EASU and optional RCAS |
| 6 | SGSR1 | Qualcomm one-pass 12-tap mobile shader |
| 7 | NIS | NVIDIA scaler with the official scale/USM coefficient textures |
| 8 | SGSR2 | Qualcomm two-pass temporal convert/upscale |
| 9 | Arm ASR | official Arm generic-library Vulkan backend |

NIS 1.0.3 accepts a source/output ratio from 0.5 through 1.0 per axis. Requests
outside that domain, and SGSR1 HDR input, route to FSR1 and publish a non-zero
fallback reason instead of claiming that the requested backend ran.

## Backend-specific parameters

Only controls consumed by the selected backend are shown:

| Backend | Parameters |
|---|---|
| Bilinear | none |
| Sharp Bilinear | sharp-prescale blend strength |
| Bicubic | reconstruction kernel |
| FSR1 | RCAS enable and RCAS sharpness |
| SGSR1 | edge sharpness mapped to the official 1.0–2.0 range |
| NIS | sharpening strength |
| SGSR2 | temporal stability/minimum-current-frame contribution |
| Arm ASR | sharpening enable and sharpness |

`Auto` owns no extra tuning values: it selects a concrete backend from the
central Vulkan capability profile, then that backend consumes its persisted
defaults. `Off` hides both the destination selector and parameter controls.
The legacy `fsr_output_res_mode` preference and launch extra are read only for
migration; new writes use `upscaled_output_res_mode` and
`upscaled_output_resolution_mode`.

## Temporal input contract

`Sgsr2Pass::TemporalInputs` accepts guest-native depth, velocity,
clip-to-previous-clip, jitter, reset and camera continuity. When the renderer
does not yet expose matching guest attachments, an emulator-owned bounded 5x5
color search reconstructs current-to-previous motion, writes a neutral depth
surface and maintains low-resolution color history. This is intentionally
reported as `reconstructed motion/depth`, never as native input.

SGSR2 consumes its encoded-velocity form. Arm ASR consumes the normalized raw
motion form through the official `ffxmFsr2ContextDispatch` Vulkan API. Both
paths retain reset/history handling and use per-frame presentation outputs.

## Proof telemetry

Runtime HUD ABI indices 108 through 113 append schema version, requested mode,
effective mode, cumulative dispatched frames, fallback reason and the
native-temporal-input flag. The on-device HUD displays the effective backend,
source/output extent, dispatch rate and any fallback. Existing FSR1 EASU/RCAS
counters at indices 56 and 99 through 107 remain unchanged.

The Android/native settings bridge is version 4. Alongside the common source,
destination and backend selection it carries the backend-specific values above;
native configuration clamps each value before a presenter or host pass sees it.

All GPU-family selection remains in `vk_capability_profile.h`; presenter and
host passes consume the resulting profile without inspecting vendor IDs or
driver strings.
