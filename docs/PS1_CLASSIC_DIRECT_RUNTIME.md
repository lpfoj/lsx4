# PS1 Classic direct runtime

> [!IMPORTANT]
> Before any device/FPS/playable measurement, run `scripts/preflight-vivo-performance-test.ps1`;
> stale Android Studio process-trackers and known profiler loads make the result invalid. See the
> mandatory [performance-test preflight](PERFORMANCE_TEST_PREFLIGHT.md).

## Why this path exists

Sony PS1 Classics packaged for PS4 are not native PS4 games. Their `eboot.bin`
is an x86-64 PS4 wrapper around a PS1 emulator and the package contains PS1 disc
images, a `PS1HD` configuration tree, and SCPH550x BIOS files. Running the whole
wrapper through LSX4 creates two emulation layers:

`ARM64 -> LSX4 x86-64 PS4 runtime -> Sony PS1 emulator -> PS1 game`

The direct runtime replaces that with:

`ARM64 -> native PS1 AArch64 recompiler -> PS1 game`

The PS4 and PS5 runtimes remain the fallback for packages that are not detected
as PS1 Classics.

## Package recognition

The direct route requires all of the following below the selected `eboot.bin`:

- `conf/PS1HD/package-ps4.conf`
- `assets/PS1HD/bios`
- `data`

Disc selection is package-driven and title-independent. An explicit image from
the package configuration wins; otherwise the runtime scans supported PS1 image
extensions and scores region/language variants deterministically. MediEvil
currently selects the NTSC-U English image, avoiding PAL timing and LibCrypt
when an official NTSC image is present.

## Runtime contract

- CPU: SwanStation AArch64 recompiler.
- GPU: threaded software renderer presented directly to `ANativeWindow`.
- Audio: native AAudio callback and lock-free sample ring. SDL JNI audio must not
  be used from a dynamically loaded runtime library.
- Input: DualShock protocol. All PS4 face/shoulder buttons remain mapped;
  Options maps to Start, the touchpad exposes Select/Start, and both analog
  sticks are forwarded. In digital mode analog sticks act as a D-pad so older
  games remain usable.
- Saves: one Libretro memory-card file per CUSA under
  `lsx4-home/ps1-classics/<title-id>/card1.srm`.
- Timing: the frontend paces at the disc region's reported frame rate and logs
  `PS1_DIRECT_METRIC` with frontend FPS and core execution time.

## Compatibility model

The generic route is safe for standard decrypted PS1 images. A complete PS1HD
bridge should translate package metadata into a structured manifest containing:

1. ordered discs and active region/language;
2. BIOS selection and CD-ROM timing;
3. controller type and analog policy;
4. memory-card identity;
5. common Lua game-fix categories such as LibCrypt keys, replacement textures,
   widescreen/crop policy, and disc swaps.

Each translated feature must be capability-gated. Unknown Lua callbacks or an
unsupported protected image must select the existing PS4-wrapper fallback rather
than silently launching with missing fixes. No CUSA-specific branch belongs in
the runtime.

## Verified result

On Vivo `10AG263F1B001HM`, MediEvil `CUSA42759` reaches gameplay, accepts face
buttons and the virtual analog stick, and sustains 59.8 FPS. Core work varies
between roughly 4 and 10.6 ms per 16.7 ms frame. The previous nested-emulation
route produced about 9-11 FPS and could deadlock at 0 FPS when Options skipped
the wrapper intro.

## References

- https://ps1hd.pages.dev/
- https://github.com/Zcor3x/Ps1HDemu-for-Playstation-4
- https://github.com/libretro/swanstation
