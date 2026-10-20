# Snapdragon 8 Gen 5 stable build checkpoint (2026-08-28)

Performance evidence is valid only after `scripts/preflight-vivo-performance-test.ps1` passes.
See `PERFORMANCE_TEST_PREFLIGHT.md`; any run that skips or fails this gate is invalid.

This checkpoint records the exact source and packaged-runtime identity of the
LSX4 build installed on the Vivo Snapdragon 8 Gen 5 / Adreno 840 device
`10AG263F1B001HM` on 2026-08-28.

## Source identity

- Root base before the Android/HUD checkpoint:
  `f0878971316873eb9e2ff1f4abde4fce00f66ed6`
- Funnel gitlink: `dd87c696818cf29cbf7b70bae5087b64dcdf7ac9`
- FEX Core: `3ab2f24c7a5371dcef2f950b3342a3c37b0d1627`
- FEX adapter: `4470f1791699378f43312232efc94e2eb40a4f7b`
- Android build marker: `lsx4-2026-08-28-a840-pre8gen3-hud`

The root checkpoint also contains the HUD layout requested in the 2026-08-28
session and shared discovery of games under `/storage/emulated/0/Emulation/PS4/games`.

## Packaged artifacts

- APK SHA-256:
  `e2b0c2cb5139bab554bf28734a677c9bd9d29c0fbf2eabe986b9d6cb9d9d9914`
- APK size: `37208536` bytes
- `libFEXCore.so` SHA-256:
  `dda493fcb9973e0bdf05d8ed0189783cfbc9bd7556f302fa95774507fdb0f616`
- `liblsx4_fex_backend_v3.so` SHA-256:
  `20366d84be91e175697ee1622f6a237dfa2c2f789715b86fcb0ed809f0e0343e`
- `liblsx4_fex2608_context_bridge.so` SHA-256:
  `83e9faef78947735d2036a15562897171e71dbffa8f097488f7cf01f24c523cc`

The APK hash was checked both locally and against the installed base APK on the
device. Installation completed through the guarded Vivo installer.

## Runtime-state caveat

Files below the application's writable data directory are not part of this
checkpoint. In particular, an old `run-use-vivo-moderate-cpu-floor` marker can
override the source default `performance-hint-4241`; it does not describe this
branch's compiled default.
