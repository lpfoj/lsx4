# Selective launcher and compatibility-report transfer

Local transfer into `8gen3fin` from Gold `dc8f85721e48f11405a0d811d866409a9ca1fe79` on 13 October 2026.
Parent: `550f25093d0baf3843edce79200910c9dc8570a2`. This is a selective feature commit, not a Gold merge.

## Included

- All seven launcher layouts: list, horizontal, vertical, library horizontal,
  library vertical, tiles horizontal and tiles vertical; their existing themes,
  selection sounds, optional menu music and saved preferences.
- The device-information screen and its independent system OpenGL/Vulkan probe.
  The probe does not load or configure a guest runtime.
- The existing playable catalogue (104 entries), with the first-100 milestone
  pinned to `ebc1c1deb6c3151fc1d39e79e8b310257cd05878`. It describes the existing
  public results, not a new gameplay or FPS verification of this branch.
- Voluntary compatibility reports for a rated game: localized preview and
  consent, explicit Send, copyable receipt, and manual retry of the same snapshot.
  The existing endpoint is `https://lsx45.app/api/v1/compatibility-reports`.
- English, Russian, Spanish, French, German and simplified Chinese feature text.
- A random UUID per submission, no new persistent installation/device ID, and
  server statistics that count different submissions from identical hardware
  separately. Retrying one unchanged UUID does not double-count it.
- The already implemented private VPS service and its tests/documentation;
  report history has no automatic expiry and submissions have no rate throttle.
  No credentials or private keys are included.

## Preserved

Guest CPU/JIT/HLE/renderer code, runtime libraries, graphics profiles, gameplay
presets, import behavior, diagnostics and existing app identity/version remain
from this branch. Launcher integration in MainActivity changes only menu window
orientation and menu sound activation during launcher resume/pause. The free
version retains its local-only document-picker settings and verifies the same
approved native-core hash; it adds only the independent menu probe beside it.

The protected Gold checkout is not switched, stashed or reset. Existing pending
changes in the target checkout are excluded from this selective commit. Source
changes are committed locally only; no remote publication is part of this transfer.

See [compatibility reports](COMPATIBILITY_REPORTS.md) for payload, consent,
service operation and the separate release-policy review boundary.
