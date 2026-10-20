# Android Audio Compatibility Postmortem

> [!IMPORTANT]
> Before any device/FPS/playable measurement, run `scripts/preflight-vivo-performance-test.ps1`;
> stale Android Studio process-trackers and known profiler loads make the result invalid. See the
> mandatory [performance-test preflight](PERFORMANCE_TEST_PREFLIGHT.md).

## Scope and final status

This document records the Android audio failures investigated across Nidhogg, Shovel
Knight, Owlboy, Dead Cells, Inside, and Bloodborne. Similar user-visible symptoms came
from four different fault classes and must not be treated as one bug:

1. incomplete x86-64 System V variadic-argument reconstruction corrupted
   floating-point values written to game configuration files;
2. a game could retain a previously corrupted zero-volume options file after the
   runtime had been corrected;
3. the NGS2 HLE renderer had incomplete voice-format handling and consumed the
   render buffer capacity instead of the configured NGS2 grain.
4. the Android library prune removed the stateful AJM/ATRAC9 producer used by
   Inside's Wwise middleware; unresolved imports then returned false success without
   mutating PCM/sideband buffers, while a by-value `AjmInstanceFlags` aggregate also
   required an explicit x86-64 SysV-to-AArch64 ABI classification.

The final observed status on Vivo was:

| Title | Final status | Owning fault class |
| --- | --- | --- |
| Nidhogg 1 | Audio working | HLE variadic ABI/configuration |
| Nidhogg 2 | Audio already working; retained as a control | None in this investigation |
| Shovel Knight | Audio works on first start, before entering or saving audio settings | HLE variadic ABI/default configuration |
| Owlboy | Audio and music defaults remain nonzero after removal of the stale options file | Persisted game state created while formatting was broken |
| Dead Cells (`CUSA10484`) | Effects, music, ambience, and voices sound correct | NGS2 HLE format, resampling, and render-grain contract |
| Inside (`CUSA02754`) | Wwise footsteps and effects are correct; periodic noise-echo is absent | AJM/ATRAC9 HLE registration plus x86 SysV-to-AArch64 HLE ABI |
| Bloodborne (`CUSA03173`) | Audio remained available | FMOD/AudioOut control path |
| Android virtual controls | No host key-click sound | Android client UI policy |

Original 2026-07-24 snapshot, retained for the earlier cases:

- LSX4 repository HEAD `fb006616e66101cb9ccc660ab4370670fcb177f1` plus the current uncommitted worktree;
- Funnel ARM repository HEAD `ec8bae673db8df270488ccc1fb9037eefde92a39` plus the current uncommitted worktree;
- verified native library SHA-256
  `ED18A55DAADED4F5DFBCD454A2920D4ABF281667FB854C71D673CDB02DB8EE3A`;
- verification device: Vivo `V2546A`, Adreno;
- verification date: 2026-07-24.

Inside closure snapshot, 2026-08-24:

- backend: FEX on Vivo `V2546A`, Snapdragon 8 Gen 5 / Adreno 840;
- effective Vulkan driver: bundled Turnip, Mesa `26.2.1` (`git-da14d65e44`);
- ELF HLE inventory gate: `815/815`, with zero memory-typed arguments;
- candidate APK: `50,884,414` bytes,
  SHA-256 `2A201C98B4DFCE8AF5751F1BFD8524369305A204E0614D06144AC3DC36E99F2F`;
- packaged `liblsx4_executor_android.so`: `62,038,920` bytes,
  SHA-256 `05E4E088D2BA958FCA35FADC53EAC03EB6771B58C1FBF84AA69D0CD6C3F4193B`;
- the source trees were intentionally dirty; the exact artifacts and hashes above,
  not a misleading repository HEAD, identify the device-tested candidate.

Line numbers below refer to this snapshot. Function and symbol names are the durable
references if later changes move the lines.

## Runtime stacks

The affected titles do not all use the same audio middleware.

```mermaid
flowchart TD
    GAME[Guest game]

    GAME --> CONFIG[Configuration serialization]
    CONFIG --> PRINTF[Guest sprintf / snprintf / fprintf]
    PRINTF --> HLE[LSX4 HLE invocation frame]
    HLE --> VALIST[Funnel guest SysV va_list reconstruction]
    VALIST --> SAVE[Guest save or options file]

    GAME --> AUDIOOUT[libSceAudioOut]
    GAME --> NGS2[libSceNgs2 HLE]
    GAME --> FMOD[Game-bundled FMOD]
    GAME --> WWISE[AkSoundEngine.prx / Wwise]

    NGS2 --> VOICE[NGS2 voice control and decode]
    VOICE --> GRAIN[NGS2 grain renderer and mixer]
    GRAIN --> AUDIOOUT
    WWISE --> AJM[libSceAjm batches / AjmWorker]
    AJM --> AT9[ATRAC9 decode]
    AT9 --> AUDIOOUT
    FMOD --> AUDIOOUT
    AUDIOOUT --> BACKEND[Funnel Android audio backend]
    BACKEND --> AAUDIO[AAudio shared low-latency stream]
    AAUDIO --> DEVICE[Android media output]
```

The configuration stack explains Nidhogg 1 and Shovel Knight, and explains how
Owlboy acquired a persistent zero-volume file. The NGS2 stack explains Dead Cells.
Inside exercises the Wwise/AJM/ATRAC9 branch: a success-returning resolver stub hid
the absence of the producer's required buffer effects. Bloodborne exercises the FMOD
branch and was therefore a useful negative control.

## Shared case: floating-point values became zero in game options

### Symptoms

- One or more games opened with music and sound-effect volume at zero.
- Entering the game's settings and selecting **Set to default** could make audio
  start immediately.
- Shovel Knight initially required that operation on every fresh start.
- Owlboy visibly reset its own music and sound sliders to `0%`.
- Host audio was enabled and the Android output stream could be opened, so this
  was not an AAudio device-selection failure.

### Fault stack

```text
guest configuration code
  -> guest sprintf/snprintf/fprintf with %f/%g
  -> translated HLE call
  -> only a partial set of XMM arguments reached the runtime context
  -> synthetic guest va_list lacked the complete 176-byte SysV register-save area
  -> floating conversion read zero or the wrong slot
  -> game saved a syntactically valid zero-volume value
  -> later launches loaded silence as an intentional game setting
```

The critical distinction is that AudioOut did not reset the game sliders. The
game persisted the zero values because its formatted configuration data was wrong.
This also explains why manually resetting settings appeared to "activate" sound.

### Working general solution

The fix reconstructs the complete x86-64 System V variadic state instead of adding
title-specific volume overrides:

- [`src/executor/dynamic_translation/hle_invocation_frame.h`](../src/executor/dynamic_translation/hle_invocation_frame.h),
  `HleInvocationFrame`, line 13: retain all eight floating argument lanes;
- [`src/executor/dynamic_translation/retiring_execution_core.cpp`](../src/executor/dynamic_translation/retiring_execution_core.cpp),
  `TryExecuteJitDirectHle`, lines 27697-27718: capture XMM0-XMM7 from guest state;
- [`src/executor/native_runtime_api.cpp`](../src/executor/native_runtime_api.cpp),
  `executor_live_get_current_hle_floating_arguments`, lines 5185-5197: expose the
  current thread's floating arguments to Funnel;
- [`funnel-arm/src/core/aerolib/stubs.cpp`](../funnel-arm/src/core/aerolib/stubs.cpp),
  `ExecutorDirectGuestVaList`, lines 6663-6698: construct the 176-byte register-save
  area with six general-purpose slots and eight 16-byte floating-point slots;
- the same Funnel file, `ExecutorLibcVasprintf`, lines 6472-6506: advance
  `gp_offset`, `fp_offset`, and `overflow_arg_area` according to the guest ABI;
- the same Funnel file, `ExecutorLibcFprintf`, `ExecutorLibcSnprintf`, and
  `ExecutorLibcSprintf`, lines 6728-6742 and 7028-7039: use the shared guest
  `va_list` builder rather than incomplete local arrays.

This is a cross-layer ABI fix. Reducing the captured lanes back to four, packing
floating values at 8-byte rather than 16-byte intervals, or pointing
`overflow_arg_area` at the host stack will recreate the defect.

### Regression checks

1. Start a title with no existing options file.
2. Confirm that default music and effects are audible before opening settings.
3. Inspect the game's sliders; defaults must be nonzero.
4. Restart without saving settings and confirm the same result.
5. Save a non-default floating volume, restart, and verify exact persistence.
6. Exercise a format string containing both integer and floating conversions so
   GP and FP offset advancement are tested together.

## Nidhogg 1

### Observed symptom

Nidhogg 1 was silent. Nidhogg 2 was explicitly tested separately and already
worked, so the two titles must not be conflated.

### Stack

```text
Nidhogg 1 startup/options initialization
  -> formatted floating-point configuration values
  -> LSX4 direct HLE boundary
  -> Funnel libc formatted-output HLE
  -> persisted audio levels
  -> game audio mixer
  -> libSceAudioOut
  -> AAudio
```

### Working solution

The complete eight-lane HLE floating-argument path and correctly laid-out guest
`va_list` fixed the title. No title ID check, forced gain, or Nidhogg-specific
fallback was added.

### Rejected diagnoses

- Nidhogg 2's previous-frame flicker fix is unrelated.
- A missing NGS2 firmware module is unrelated to this title's confirmed path.
- Forcing AudioOut gain would only hide the bad persisted/configured value and
  would violate guest mute semantics.

## Shovel Knight

### Observed symptom

On initial launch the game was silent. Opening audio settings and selecting
**Set to default** made sound work. An intermediate state worked only after the
settings had been saved; the required behavior was audio before any settings visit
or save.

### Stack

```text
first-run default option construction
  -> guest formatted libc call with floating values
  -> translated HLE argument capture
  -> guest SysV va_list reconstruction
  -> initial in-memory audio configuration
  -> game mixer
  -> libSceAudioOut
  -> AAudio
```

### Working solution

The shared variadic ABI fix restored the first-run default values. The accepted
result does not depend on an existing options file or the settings reset action.
The AudioOut port itself also starts at PS4 unity gain in
[`funnel-arm/src/core/libraries/audio/audioout.cpp`](../funnel-arm/src/core/libraries/audio/audioout.cpp),
`OpenPort`, line 396, and applies that value to the backend at line 436. This is
normal platform behavior, not a Shovel Knight override.

### Regression checks

- Test with the title's options removed.
- Do not enter settings before checking title-screen and gameplay audio.
- Restart once without saving, then once after changing and saving a non-default
  volume.

## Owlboy

### Observed symptom

Owlboy repeatedly presented both music and sound volume as `0%`. Correcting the
runtime alone did not alter the already persisted values.

### Root cause

This was a two-stage failure:

1. the earlier incomplete floating-point variadic path allowed zero values to be
   serialized;
2. Owlboy then correctly reloaded those values from its existing options state.

There was no evidence that Owlboy continued to force valid nonzero values back to
zero after the runtime fix and a clean options regeneration.

### Working solution

Keep the shared HLE ABI correction, then remove only Owlboy's persisted options
file from its own save data once. On the next launch the game regenerates nonzero
defaults. The exact device-side filename can vary with title build and save layout;
identify it from the title's mounted save directory instead of deleting the whole
application data or other games' saves.

No emulator source was rolled back and no Owlboy-specific code path was added.

### Regression checks

- Back up the title save before manipulating options.
- Remove only the options artifact, launch, and confirm nonzero defaults.
- Restart twice to distinguish correct persistence from a one-run fallback.
- Do not treat deletion as the product fix; it is cleanup for state produced by
  the old runtime.

## Dead Cells

### Symptoms and progression

Dead Cells had two successive failures:

1. the game was initially completely silent;
2. after NGS2 voice support was added, effects became correct but music, ambience,
   and voices were rough, growling, and temporally inaccurate.

Runtime measurements for the second failure showed:

```text
[LSX4_NGS2_RENDER] bytes=32768 frames=4096 channels=2 waveform=0x18
                   systemRate=48000 grain=256
```

The active streaming PCM voice used stereo S16 data at 44.1 kHz. A representative
block was 16,384 bytes / 4,096 frames. Larger observed voices included 359,200-byte
/ 89,800-frame and 179,600-byte / 44,900-frame blocks.

### Fault stack

```text
Dead Cells NGS2 commands
  -> sceNgs2VoiceControl
  -> canonical sampler/submixer parameter decoding
  -> PCM waveform decoding and streaming callbacks
  -> 44.1 kHz to 48 kHz voice resampling
  -> sceNgs2SystemRender
  -> configured 256-frame NGS2 grain
  -> game forwards the rendered grain to libSceAudioOut
  -> 256-frame AAudio output at 48 kHz
```

Before the final fix, `sceNgs2SystemRender` treated the full 4,096-frame buffer
capacity as the requested render length. The game consumed or forwarded only the
configured 256-frame grain. The HLE runtime therefore decoded and advanced up to
sixteen grains per call while fifteen grains of temporal progress were discarded.
Streaming callbacks and block changes occurred every 10-20 ms instead of roughly
100-120 ms, producing the rough, accelerated, discontinuous music and voices.

### Working solution

The accepted result is the combination of these general NGS2 contracts:

1. **Canonical parameter identifiers.**
   [`funnel-arm/src/core/libraries/ngs2/ngs2_runtime.cpp`](../funnel-arm/src/core/libraries/ngs2/ngs2_runtime.cpp),
   lines 55-82, recognizes canonical sampler IDs `0x40010000` through
   `0x40010005`, submixer ID `0x20010000`, and normalizes legacy aliases.
2. **Canonical PCM waveform formats.**
   The same file, lines 45-52 and 85-111, handles S8 `0x11`, S16 `0x12`,
   S24 `0x13`, S32 `0x14`, F32 `0x18`, plus the legacy PCM aliases.
   `DecodePcmFrame`, lines 888-940, converts each supported format without
   changing guest block stride semantics.
3. **Continuous resampling state.**
   `VoiceState`, lines 254-259, retains the current frame, next frame, and
   fractional phase. `MixVoice`, lines 1187-1257, linearly interpolates across
   frame and streaming-callback boundaries instead of dropping fractional source
   progress.
4. **Render-grain bound.**
   `Ngs2Runtime::Render`, lines 648-688, renders
   `min(buffer_capacity, system.grain_samples)` frames. It still clears the full
   output capacity, so unused space cannot leak stale samples.
5. **Real HLE entry-point delegation.**
   [`funnel-arm/src/core/libraries/ngs2/ngs2.cpp`](../funnel-arm/src/core/libraries/ngs2/ngs2.cpp),
   `sceNgs2SystemRender`, lines 552-555, and `sceNgs2VoiceControl`, lines 592-594,
   delegate to the runtime instead of returning success from empty stubs.

The render-grain bound was the decisive quality fix. Linear resampling improved
sample-rate conversion but did not by itself eliminate the distortion.

### Diagnostic dead ends and incomplete attempts

- Installing `libSceNgs2.sprx` was investigated. No firmware NGS2 module was
  present in the tested Vivo `sys_modules` directory; the final accepted run used
  the built-in HLE runtime.
- The unimplemented filter parameter `0x40001300`, type 9, was observed and
  considered, but it was not required for the accepted fix and was not represented
  as implemented.
- Historical removed AudioFlinger tracks showed thousands of underruns. The
  currently active track after the fix reported zero underruns, so old removed-track
  counters were not evidence for the final defect.
- Increasing AAudio capacity, changing gain, or adding silence does not correct a
  renderer that advances sixteen times too far.

### Regression checks

1. Confirm effects, background ambience, music, and voices separately.
2. Log `bufferSize`, capacity-derived frames, configured grain, sample rate,
   channel count, and waveform type.
3. Require decoded source progress per render call to match the configured grain,
   not the destination allocation capacity.
4. Exercise both 44.1 kHz and 48 kHz voices and a block transition.
5. Inspect only the active AudioFlinger/AAudio track when evaluating underruns.

## Inside (Wwise / AJM / ATRAC9)

### Observed symptom and pre-convert signature

Inside's opening audio could sound plausible, but once gameplay began, footsteps
and other Wwise events pushed a deformed periodic noise with an echo-like tail.
Changing AudioOut pacing or downmix could change the presentation but did not remove
the source waveform defect.

A content-gated diagnostic captured 12 seconds of the executor's eight-channel,
48 kHz Float32 stream before format conversion, downmix, AAudio, and the device:

- only channels 0 and 1 carried content; channels 2-7 were zero;
- there were no NaNs and no clipped samples;
- the broken capture contained 39 isolated six-frame impulses, 37 exact 6,144-frame
  pairs, a longest repeating chain of 20, and 30 occurrences of the legacy
  fingerprint;
- a second periodic family contained 42 bursts of 109-113 frames separated by
  5,605/5,606 frames, with a chain length of 42.

At 48 kHz, 6,144 frames are 128 ms, or six common 1,024-sample ATRAC9
superframes. The periodicity, Wwise event correlation, and later disappearance
when the real decoder returned make this a causal decoder-service signature, not
an AAudio underrun correlation.

### Fault stack

```text
Inside gameplay event
  -> AkSoundEngine.prx / Wwise
  -> ten libSceAjm imports
  -> Android-prune build had no AJM registration
  -> unresolved generic thunk: xor rax, rax; ret
  -> guest observes ORBIS_OK but encoded input is not consumed
  -> PCM and AJM sideband/output accounting remain stale or unfilled
  -> Wwise mixes periodic stale data
  -> libSceAudioOut faithfully transports noise-echo
```

The active import set was:

- `sceAjmBatchJobControlBufferRa`;
- `sceAjmBatchJobRunSplitBufferRa`;
- `sceAjmBatchStartBuffer` and `sceAjmBatchWait`;
- `sceAjmInitialize` and `sceAjmFinalize`;
- `sceAjmInstanceCreate` and `sceAjmInstanceDestroy`;
- `sceAjmModuleRegister` and `sceAjmModuleUnregister`.

Android-prune commit `ec8bae67` removed `src/core/libraries/ajm/*` and
`src/common/slot_array.h`. The resolver's generic success-zero fallback made the
missing stateful service look healthy to the game. This is why output-thread,
gain, and scheduler experiments could not repair the waveform.

### Working solution

The accepted fix restores a general platform contract rather than adding an
Inside path:

1. Restore the byte-compatible AJM implementation and `slot_array.h` from
   `ec8bae67^`. The ATRAC9 decoder, AJM batch parser, context/instance ownership,
   worker, and sparse output accounting must move together; copying an unrelated
   later decoder file alone is not a compatible update.
2. Include AJM in the Android-essential HLE set by calling
   `Libraries::Ajm::RegisterLib(sym)` in
   [`funnel-arm/src/core/libraries/libs.cpp`](../funnel-arm/src/core/libraries/libs.cpp).
   Registration is part of the shipped build contract, not an optional firmware
   probe.
3. Classify the 64-bit union `AjmInstanceFlags` as one x86-64 SysV INTEGER
   eightbyte in
   [`funnel-arm/src/core/aerolib/hle_abi_overrides.h`](../funnel-arm/src/core/aerolib/hle_abi_overrides.h).
   Without this override, the Android x86-to-AArch64 HLE bridge rejects or
   misroutes `sceAjmInstanceCreate` even though the object is trivially copyable
   and layout-compatible with `u64`.
4. Extend the compile-time ABI inventory smoke to prove the aggregate override,
   all ten integer arguments of `sceAjmBatchJobRunSplitBufferRa`, and its 32 bytes
   of x86-64 stack arguments. Raise the expected inventory to the actual
   `815/815` exports only as the coupled registration change lands.

5. Keep per-decoder ATRAC9 state, but initialize LibAtrac9's immutable MDCT,
   Huffman, and gradient tables only once per process. The implementation uses
   <code>InitOnceExecuteOnce</code> on Windows, <code>pthread_once</code> on
   Android/POSIX, and an acquire/release atomic fallback elsewhere. This removes
   repeated global-table setup from later decoder initialization without sharing
   mutable decoder handles.

No forced gain, title-ID branch, fake sample insertion, fixed delay, global
scheduler wait, or per-buffer polling was added. AJM decode remains on its worker.
The upstream-compatible `sceAjmFinalize` and `sceAjmModuleUnregister` no-ops are
acceptable for the exercised path because no later PCM/output effect depends on
them; decode-critical functions are real.

### Verification

Runtime proof on Vivo `V2546A`, FEX, effective Turnip Mesa 26.2.1:

| Evidence | Broken candidate | AJM-fixed candidate |
| --- | --- | --- |
| AJM producer | Ten imports resolve to success-zero thunks | Real `shadPS4:AjmWorker`, contexts, batches, instances, and ATRAC9 decode |
| Raw capture | `.codex-a840-inside-fex-preconvert-content-12s.wav` | `.codex-a840-inside-ajmfix-fex-preconvert-content-12s.wav` |
| File bytes | 18,432,044 | 18,432,044 |
| SHA-256 | `E1AE6DEF3F429C3C0FAF2E5E8914E7B9C75278DFFEA03240A67377DC3D431E47` | `DF189BAF617A31E287AF41482263058605A780782C80EC19F4CE4EB2EC2A0418` |
| Six-frame / 6,144-frame signature | 39 impulses, 37 exact pairs, longest chain 20 | 0 / 0 / 0 |
| 109-113-frame burst chain | 42 bursts, chain 42 | 0 |
| Legacy fingerprint | 30 occurrences | 0 |
| NaN / clipping | 0 / 0 | 0 / 0 |
| Gameplay acceptance | Deformed noise-echo | User confirmed correct sound |

The accepted gameplay screenshot
`.codex-vivo-inside-ajmfix-gameplay-final.png` is 1,839,664 bytes with SHA-256
`4F3C37726C64D87E50B8B766547F2542A79FEAEA56D67A480C941441E2D64E3C`.
The image proves the tested gameplay phase; the raw capture and user listening
gate prove the audio result.

### Post-fix performance attribution

A gameplay CPU profile taken after the accepted audio fix assigned approximately
<code>1.1%</code> to <code>shadPS4:AjmWorker</code> and <code>0.38%</code> to
<code>AudioBatch</code>, while the game's FMOD mixer and stream threads accounted
for approximately <code>18.7%</code> and <code>4.3%</code> respectively. These are
profile shares from that run, not additive frame-time claims and not a clean
before/after measurement.

The boundary is nevertheless useful: the roughly 10 FPS gameplay result from the
same investigation cannot be attributed to restored AJM alone. The stateful AJM
producer is required for correct sound and its measured worker share is too small
to explain the whole deficit; subsequent FPS work must profile the guest/FMOD,
renderer, and translation paths independently. The one-time LibAtrac9 static-table
initialization removes redundant setup, but no isolated FPS delta is claimed for
that change.

### Non-regression boundary

- A mandatory stateful decoder/service NID must mutate its documented buffers and
  progress state or fail closed; a generic successful zero-return is not an
  implementation.
- Keep AJM registration in the Android-essential library set and keep
  `AjmInstanceFlags` classified as one INTEGER eightbyte.
- Preserve AJM worker execution. Do not replace it with global waits, audio-thread
  polling, or game-specific timing.
- Retest Inside footsteps/effects, Dead Cells' independent NGS2 path, and
  Bloodborne's independent FMOD/AudioOut path after shared media or ABI changes.

## Bloodborne

### Observed result

Bloodborne audio was present during this investigation. It remained a control for
the ordinary game-middleware path while the NGS2 and configuration cases were
being changed.

### Stack

```text
Bloodborne guest audio / FMOD mixer
  -> game-bundled middleware threads
  -> libSceAudioOut
  -> Funnel Android audio backend
  -> AAudio
```

The observed FMOD mixer thread can be CPU-intensive, but CPU cost is not evidence
of silence or the Dead Cells NGS2 grain defect. In the current post-AJM gameplay
profile the FMOD mixer and stream threads accounted for approximately
<code>18.7%</code> and <code>4.3%</code>, versus <code>1.1%</code> for
<code>shadPS4:AjmWorker</code> and <code>0.38%</code> for
<code>AudioBatch</code>. Therefore the observed low FPS is not attributable to
AJM alone.

The bundled official Bloodborne 01.09 catalog also exposes an optional
<code>FMOD Crash Fix</code>. It is default-off and was not required to establish
the accepted Inside AJM result or Bloodborne's working FMOD/AudioOut control.
Merely recognizing that patch is not evidence that it was applied or that it
improves audio or FPS.

### Non-regression rule

Do not route all titles through NGS2, force global nonzero volume, or bypass guest
mute calls to repair another title. Bloodborne must continue to exercise its
existing FMOD/AudioOut route unchanged.

## Android output hardening

### AAudio backend

[`funnel-arm/src/core/libraries/audio/sdl_audio_out.cpp`](../funnel-arm/src/core/libraries/audio/sdl_audio_out.cpp),
`AndroidAAudioPortBackend`, lines 578-809, owns the Android output route:

- shared low-latency AAudio stream;
- requested guest sample rate;
- mono or stereo host output;
- floating-point host samples;
- bounded blocking write;
- SDL fallback if AAudio cannot be opened.

`SetVolume`, lines 651-659, derives host gain from the active guest channel gains
and the application volume slider. It must preserve an intentional guest zero.

### Eight-channel downmix

`ConvertToOutput`, lines 718-784, downmixes an eight-channel guest stream to stereo
instead of copying only the first two channels. Center and surround content are
mixed at approximately -3 dB. This prevents dialogue, music, or effects assigned
outside FL/FR from disappearing on a stereo Android device.

Inside's diagnostic A/B was also an eight-channel stream, but only channels 0 and
1 were active and the periodic defect was already present in the pre-convert raw
capture. Therefore the accepted AJM fix did not change downmix coefficients or
channel routing.

### Silent-buffer keepalive

`Output`, lines 599-627, inserts an inaudible alternating `1e-8` sample only when
the entire converted buffer is exactly zero. This keeps vendor audio paths from
treating an active stream as permanently idle without making muted guest audio
audible. It is output-path hardening, not a substitute for valid game volume,
format decoding, or NGS2 timing.

### Content-gated pre-convert capture

The default-off diagnostic property `debug.lsx4.audio_raw_capture=1` arms one
capture to `diag-audio-raw-preconvert-f32.wav`. Capture begins on the first
nonzero Float32 PCM rather than at port creation, retains the source channel count
and sample rate, and stops after 12 seconds. Its insertion point is before format
conversion, stereo downmix, AAudio, and the physical device.

`[EXECUTOR_AUDIO_RAW_CAPTURE]` reports armed, started, completed, skipped, and
failed states. This diagnostic must remain property-gated: it is an oracle for
separating decoder/mixer defects from output transport, not production I/O on
every audio buffer.

### First-nonzero S16 stereo block probe

For a port-specific, log-only trace, set the existing property to the exact
selector instead of the broad `1` mode:

```sh
adb shell setprop debug.lsx4.audio_probe s16-2ch-48k-1024
adb logcat -v threadtime LSX4AudioProbe:W '*:S'
```

The selector matches only a signed-16-bit, two-channel, 48 kHz, 1024-frame
direct guest port. Silent buffers do not consume its budget. The first nonzero
block produces one bounded chain and then permanently disarms for that port:

1. `[EXECUTOR_AUDIO_SLOT_SUBMIT]` hashes the guest source immediately before
   and after the scheduler-slot copy and reports the copied hash.
2. `[EXECUTOR_AUDIO_SLOT_ACQUIRE]` verifies that copied hash when the scheduler
   acquires the slot, before optional LSX4 mixing and backend output.
3. `[EXECUTOR_AUDIO_RAW_HASH]` and `[EXECUTOR_AUDIO_RAW_PCM*]` describe the
   direct backend input before `ConvertToOutput` applies format conversion and
   channel gain.
4. `[EXECUTOR_AUDIO_POST_HASH]` and `[EXECUTOR_AUDIO_POST_STEREO*]` describe the
   float stereo buffer after conversion.
5. `[EXECUTOR_AUDIO_AAUDIO_FLOW]` records the completed frame count, write
   chunks, and AAudio written/read counters after the bounded write loop.

The raw, post-conversion, and write markers use the same `id`; for the targeted
mode it is always `1`. Compare the slot `copied`/`actual` hash with the raw hash
to prove that the source block survived the scheduler handoff unchanged. The
shape fields (`frames=1024`, `sampleRate=48000`, `channels=2`, `sample=s16`)
keep the target unambiguous even when other ports are open.

To re-arm after a capture, force the disabled transition first:

```sh
adb shell setprop debug.lsx4.audio_probe 0
adb shell setprop debug.lsx4.audio_probe s16-2ch-48k-1024
```

No app-private marker or environment variable is required. In particular,
`run-diagnostic-audio-optimization-stats` enables unrelated conversion timing
counters and should stay absent for this trace. The broad legacy
`debug.lsx4.audio_probe=1` mode remains available unchanged. The separate
`debug.lsx4.audio_raw_capture=1` WAV capture is not a substitute for this
selector because its single global owner can be claimed by an earlier Float32
main port.

## Android overlay key-click sound

### Symptom

Touching virtual controller buttons produced an Android UI click unrelated to
guest audio.

### Working solution

Sound effects are disabled on the overlay containers and interactive controller
views in
[`android-app/app/src/main/java/app/lsx4/android/MainActivity.java`](../android-app/app/src/main/java/app/lsx4/android/MainActivity.java),
lines 2334, 2341, 3042, and 4972. Haptic feedback remains independent and may
continue to provide touch confirmation.

## Firmware-module and test-mode diagnostics

The Android settings UI can import a user-provided decrypted
`libSceNgs2.sprx` into the application's `lsx4-home/sys_modules` directory. Debug
test mode can manage a bundled copy only when the build was explicitly configured
with one. These paths are diagnostic or compatibility alternatives and do not
change the final Dead Cells finding: the verified fix ran through the built-in
NGS2 HLE implementation.

Inside's accepted AJM/ATRAC9 route is likewise built-in HLE, not a user-imported
firmware module. A real mandatory library cannot be replaced by a generic resolver
success stub or explained away by installing an unrelated PRX.

Never bundle or distribute a copyrighted firmware module as part of the normal
runtime. A missing module must not be used to explain a failure until logs show
that the title actually selected the module path rather than the registered HLE
library.

## Diagnostic markers

Use the following markers to identify the owning layer:

| Marker | Interpretation |
| --- | --- |
| `[EXECUTOR_LIBC_FORMATTED]` | Guest format string and formatted result; use for zero-volume persistence |
| `[EXECUTOR_LIBC_FPRINTF]` | Direct guest `fprintf` HLE path |
| `[LSX4_AUDIO_STATE]` | AudioOut port/open/host-output state |
| `[LSX4_AUDIO_VOLUME]` | Guest `sceAudioOutSetVolume` requests |
| `[EXECUTOR_AUDIO_AAUDIO_OPEN]` | AAudio open parameters or SDL fallback |
| `[EXECUTOR_AUDIO_AAUDIO_OUTPUT]` | Host writes and peak level |
| `[EXECUTOR_AUDIO_RAW_CAPTURE]` | Content-gated Float32 capture before conversion/downmix/backend |
| `Lib_Ajm` lifecycle/batch logs | Real AJM contexts, instances, modules, jobs, and waits rather than resolver stubs |
| `shadPS4:AjmWorker` | Stateful AJM batch/decoder producer is alive off the audio callback thread |
| `[LSX4_NGS2_ROUTE]` | Voice matrix routing |
| `[LSX4_NGS2_VOLUME]` | NGS2 voice-port gain |
| `[LSX4_NGS2_MIX]` | Effective route and voice volume |
| `[LSX4_NGS2_AUDIO]` | Nonzero mixed NGS2 output |
| `[LSX4_NGS2_RENDER]` | Output capacity, format, system rate, and grain |

Interpret them in order. A nonzero NGS2 peak with a zero AudioOut gain is a
different failure from a silent NGS2 decoder, and a valid AAudio write cannot
repair configuration data that intentionally requests zero volume.

## Triage decision tree

1. **Does the Android audio probe produce sound?**
   If no, diagnose the host AudioOut/AAudio route first.
2. **Does `[EXECUTOR_AUDIO_AAUDIO_OPEN]` report a live stream?**
   If no, diagnose stream creation or SDL fallback.
3. **Does the game call `[LSX4_AUDIO_VOLUME]` with zero?**
   If yes, inspect guest configuration and formatted output before touching gain.
4. **Do the game's own sliders show zero after a clean first run?**
   Inspect the HLE variadic path. If the runtime is already fixed, isolate and
   regenerate only that title's options artifact.
5. **Is noise already present in the pre-convert Float32 raw capture?**
   If yes, diagnose guest configuration, middleware, decoder, or mixer before
   changing downmix/AAudio. If raw PCM is correct, continue down the host output
   path.
6. **Does the middleware import `libSceAjm`?**
   Require real AJM registration, a live `shadPS4:AjmWorker`, batch progress, and
   mutations of output/sideband buffers. A successful zero-return stub is a
   failure for decode-critical calls.
7. **Is the title using NGS2?**
   Compare canonical parameter IDs, waveform formats, route matrix, source rate,
   and configured grain.
8. **Are effects correct but music/voices rough?**
   Compare decoder progress to the NGS2 grain. Do not infer an AAudio underrun from
   old removed tracks.
9. **Is an eight-channel guest stream audible only partially?**
   Verify the stereo downmix path rather than raising global gain.

## Non-regression rules

- Do not add title-name, title-ID, or byte-signature audio fixes.
- Do not force nonzero volume after a valid guest mute request.
- Do not delete all application data to repair one title's options.
- Do not reduce guest HLE floating arguments below XMM0-XMM7.
- Do not use host `va_list` layout for a guest x86-64 System V call.
- Do not render NGS2 buffer capacity when the configured grain is smaller.
- Do not reset resampler phase at ordinary streaming block boundaries.
- Do not map mandatory stateful decoder/service NIDs to success-zero thunks. They
  must produce their documented guest-memory/progress effects or fail closed.
- Keep AJM in the Android-essential registration set, preserve
  `AjmInstanceFlags` as one SysV INTEGER eightbyte, and keep decode on the AJM
  worker.
- Do not infer current underruns from removed historical AudioFlinger tracks.
- Keep audio pacing event-driven; do not add polling loops or fixed game-specific
  delays.
- Do not repair AJM with title patches, global synchronization, fake samples, or
  per-buffer expensive validation.
- After changing any shared audio layer, retest Nidhogg 1, Shovel Knight first-run,
  Owlboy persistence, Inside Wwise/AJM footsteps and effects, Dead Cells effects
  and music through NGS2, and Bloodborne as the FMOD control.
