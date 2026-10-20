# FEX Android runtime artifacts

`build_android_fex_runtime.ps1` is the canonical API-26 arm64 build entry point. It verifies the
pinned FEX base revision, removes only the three known build directories under the workspace,
configures FEXCore and the LSX4 adapter with an explicit NDK, builds all four required Android
smoke executables, runs the adapter host tests, and emits an artifact manifest. The script checks
that every required smoke target is declared before it removes any build directory, builds those
targets explicitly, and records every resulting ELF as an `android_smokes` manifest artifact.

The script snapshots every build-input tree before cleaning and verifies the same fingerprints
after compilation, so a concurrent source edit cannot produce a mixed-source bundle.
All required pinned FEX submodules must already be initialized. Cross-compilation explicitly uses
`TUNE_CPU=none` and `TUNE_ARCH=generic`; it therefore does not inspect the Windows build host's
`/proc/cpuinfo` or accidentally tune Android code for the host. The glibc allocator interposer is
disabled on bionic: its `malloc_usable_size` ABI conflicts with Android, `FEXCORE_ONLY` does not
build the Linux thunk loader, and PS4 native calls use the separately tested LSX4 typed-HLE bridge.
FEX's internal allocator remains enabled.

For the 8 Gen 5 CPU-tuning branch the default source inputs are the sibling worktrees
`fex-emu-cpu-hacks-8gen5-wt` and `fex-adapter-cpu-hacks-8gen5-wt`. This is intentional: the
profile ABI changes FEXCore, the context bridge, and the LSX4 adapter as one unit. Use
`-FexSource` and `-AdapterSource` only to select another compatible pair; `-RepoRoot` selects the
LSX4 tree whose ABI header and funnel policy are consumed. The manifest records dirty-tree
fingerprints, so local uncommitted implementation work is still reproducible without committing
or contacting a remote.

```powershell
& .\tools\fex\build_android_fex_runtime.ps1
```

The default output manifest is:

```text
../.fex-2608-artifacts/android-api26-arm64/manifest.json
```

Gradle consumes that manifest by default. To package a manifest from another isolated build, pass
`-Plsx4FexRuntimeManifest=<absolute-or-root-relative-manifest-path>`; the normal freshness and ABI
verification remains mandatory.

The required Android smoke artifacts prove separate contracts:

- `lsx4_fex2608_runtime_smoke` checks the adapter/runtime ABI and lifecycle integration surface.
- `lsx4_typed_hle_abi_smoke` checks typed HLE ABI layout and marshalling.
- `lsx4_fex2608_staged_jit_smoke` is an executable staged-JIT proof: it maps and executes a small
  x86 program through FEX and exits through the private staged HLT route. It is not production PS4
  activation and does not prove the title loader, production HLE dispatch, signal envelope, or
  end-to-end game startup.
- `lsx4_fex2608_staged_cpu_profile_probe` executes CPUID, XGETBV, and FXSAVE through that staged
  route, compares the observed CPU state with the exact PS4 profile, and fails closed with a
  nonzero `BLOCKED` result when any required field differs. It is a readiness gate, not permission
  to advertise or select the staged backend in production.

The artifact verifier validates these Android executables as fresh API-26 AArch64 ELF inputs; it
does not run them. Until the exact PS4 CPU profile is implemented end to end, a device run of the
profile probe is expected to report `BLOCKED` rather than silently accepting a host-derived
profile. The staged-JIT smoke can pass independently, and that pass still is not production
activation.

APK packaging must verify that manifest immediately before copying native libraries:

```powershell
python .\tools\fex\verify_android_fex_artifacts.py `
  --manifest ..\.fex-2608-artifacts\android-api26-arm64\manifest.json `
  --packaged-dir <path-to-the-arm64-v8a-native-library-staging-directory> `
  --packaged-libcxx <path-to-the-libc++_shared.so-that-will-enter-the-APK>
```

The verifier rejects changed source provenance, stale artifacts, a wrong Android API/ELF ABI,
incorrect SONAMEs or exports, changed hashes/build IDs, missing runtime dependencies, and strong
undefined symbols that do not close over the bundled libraries and the API-26 NDK sysroot. The
packaged C++ runtime is checked as an API-compatible AArch64 ELF provider and the full symbol
closure is repeated against that exact file. This permits a compatible Gradle NDK runtime while
rejecting a substituted `libc++_shared.so` that cannot satisfy the FEX/bridge ABI.

`-VerifyExisting -DiagnosticSkipFreshness` and `--skip-freshness` exist only for diagnosing an old
bundle. They must not be used by APK packaging.
