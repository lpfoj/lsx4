# iOS 128.9757 release source

The `release/ios-128.9757` branch ports the supplied 128.9757 handoff onto the
public `iOS` source snapshot (`9c33cac98ba924f800701f5a15e4e75103310627`).
That snapshot predates 9564, so the port also includes the missing 9564 Swift
files, resource catalogues and buffer/scheduler declarations used by the handoff.

The five changed link inputs relative to the published 9564 application are:

| Input | Repository source | 9757 change |
| --- | --- | --- |
| Swift overlay | `LSX4iOS/App/*.swift` | Per-title managed defaults, required game patches, update prompt, Thanks layout and unknown-FPS display. |
| Rasterizer | `funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.cpp` | Disable the single-quad triangle-fan shortcut on Apple; use the existing conversion path. |
| VideoOut | `funnel-arm/src/core/libraries/videoout/video_out.cpp` | Apple follows the existing Android label-fixup branch before EOP submission. |
| Buffer cache | `funnel-arm/src/video_core/buffer_cache/buffer_cache.cpp` and `upload_snapshot_retirement.h` | Pass destination GPU-write status to committed-upload retention; written destinations are excluded. |
| Scheduler | `funnel-arm/src/video_core/renderer_vulkan/vk_scheduler.cpp` | Optional boundary-depth/wait diagnostics; boundary depth defaults to zero. |

All bundled JSON resources are synchronized with the supplied application.
`Info.plist` is stored as XML for review; its decoded values match the bundle.
Inherited resource changes include the safe Vulkan policy, game-patch catalogue,
managed-option catalogue, PS5 profiles, translations and compatibility reports.

## Building the candidate on the handoff Mac

Run from a clean checkout with the retained native inputs and local signing
identity available:

```sh
python3 LSX4iOS/Release/build_1289757.py \
  --handoff /path/to/handoff-128.9757 \
  --output /path/to/new-build-directory
```

The script compiles the 41 repository Swift sources and scheduler, verifies the
three narrow native-object patches, and links the retained 9564 runtime inputs.
It checks source/resource correspondence, input hashes and the deep signature,
and writes commands and `build-proof.json` beside the application. It refuses to
overwrite an existing output directory. It does not install, launch, publish or
modify the update service.

The inherited native runtime has **not** been rebuilt entirely from this public
source snapshot. This remains a source-consolidation limitation, as it was in the
earlier iOS branch. The release process must not describe the relink as a clean
full-runtime rebuild or a new phone performance result.

`release-notes.en.md` contains the proposed public release text. Publication and
the `updates.json` switch are separate steps after review.
