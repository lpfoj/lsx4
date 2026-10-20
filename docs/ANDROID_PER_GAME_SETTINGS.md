# Android per-game settings

Every library row ends with a burger button that opens one settings root for that title. The
notice at the top defines the ownership rule: values changed there apply only to that game;
unchanged values keep inheriting the current global profile.

The sparse `game.profile.<TITLE_ID>.*` overlay covers settings that can affect a game launch:

- internal and output resolution, display scaling, upscaler algorithm and its parameters;
- graphics backend, Vulkan driver selection, ARM/GPU managed optimizations and anisotropy;
- audio enablement and persistent-cache enablement;
- on-screen controls, opacity, phone-trigger mappings and every normalized overlay element;
- debug HUD visibility and its optional upscaler row.

Patch preferences remain in their existing title-specific keys instead of being nested twice.
CPU settings retain the separately validated `cpu.game.<TITLE_ID>.*` schema and safety contracts.
Language, driver-package import inventory, remote diagnostics and test-mode module management stay
global because they are application or installation state rather than game policy.

The runtime obtains one effective `SharedPreferences` overlay before it resolves output geometry,
loads the Vulkan driver, applies managed optimizations, creates the controller overlay or
materializes cache/render configuration. The library also puts the title identity into the launch
intent so the game process cannot accidentally resolve a different title profile.

The overlay editor embeds the same `MainActivity.DualShockOverlayView` used during gameplay. Its
transparent editing layer only supplies hit testing and the selection outline; geometry, artwork,
visibility, transforms and configured opacity come from the gameplay view itself.
