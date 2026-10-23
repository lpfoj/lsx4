# LSX4 for iOS

This branch contains the iOS JIT source snapshot based on version 128.9299.
It includes the library interface, game-folder and PKG import, controller support,
on-device JIT helper, FEX and Current JIT bridges, and the shared graphics runtime.

The iOS deployment target is 17.0. Building requires macOS, Xcode, CMake, Ninja,
a Rust toolchain with `aarch64-apple-ios`, and the upstream native dependencies.
Developer signing credentials and device pairing records are supplied locally.

The native source integration is being consolidated into a reproducible application
build. The CMake runtime targets are included; a clean full application rebuild has
not yet been validated for this snapshot. Use the published IPA for installation.

Download and installation instructions:
https://github.com/lpfoj/lsx4/releases/tag/ios-jit-128.9299

Playable/Unplayable reports are optional. Automatic diagnostic uploads are disabled.
Third-party licenses and copyright notices are retained in their source directories.
