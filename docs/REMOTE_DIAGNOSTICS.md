# LSX4 remote diagnostics

> [!IMPORTANT]
> Before any device/FPS/playable measurement, run `scripts/preflight-vivo-performance-test.ps1`;
> stale Android Studio process-trackers and known profiler loads make the result invalid. See the
> mandatory [performance-test preflight](PERFORMANCE_TEST_PREFLIGHT.md).

Debug APKs built with `LSX4_DIAGNOSTICS_UPLOAD_TOKEN` contain an opt-out diagnostics client.
The launcher and `:game` processes keep separate, bounded logcat rings and upload a report after
startup, every 20 seconds, after a previous crashed session, or when the user selects **Send
diagnostics now** in Settings → Other.

Reports include LSX4/LSX4Native logcat, Java crash traces, JIT status when available, selected
runtime text logs, process status/maps, CPU and memory information, the title ID, platform,
device/Android build, and emulator settings. They exclude game content, saves, screenshots,
accounts, Android serial numbers, and unrelated personal files.

The upload-only endpoint is `https://llayer.tech/lsx4-diag/upload`. Nginx enforces TLS, an 8 MiB
body limit and rate limiting. The receiver validates the bearer token and ZIP structure, runs as
the unprivileged `lsx4-diag` user, and stores mode-0600 reports below
`/srv/lsx4-diagnostics/<install-id>/`. The APK has no read/list/delete API and contains no SSH
credential.

To fetch and expand the newest report from a machine with the `unixvps` SSH alias:

```powershell
tools/remote-diagnostics/fetch-latest.ps1
```

The upload token is intentionally outside Git at
`C:\Users\Litesav\.config\lsx4\diagnostics-upload.token`. Build the private diagnostic APK with:

```powershell
$env:LSX4_DIAGNOSTICS_UPLOAD_TOKEN = Get-Content -Raw `
    C:\Users\Litesav\.config\lsx4\diagnostics-upload.token
android-app\gradlew.bat -p android-app :app:assembleDebug
Remove-Item Env:LSX4_DIAGNOSTICS_UPLOAD_TOKEN
```

Without that environment variable or the equivalent Gradle property, the diagnostics client is
compiled without an upload credential and remains unavailable.

## Readback contract trace is one-shot

`EXECUTOR_READBACK_CONTRACT_TRACE` changes guest-memory protection/fault telemetry and is not a
production metric. The retired `run-readback-contract-trace` marker is cleanup-only: every PS4
runtime initialization clears inherited trace state and removes that legacy marker before the JIT
or FEX backend is selected.

To arm a single diagnostic process, create `run-readback-contract-trace-once` in `lsx4-home`. The
runtime must remove the marker successfully before it enables the trace; a failed removal leaves
the trace disabled. A later game process therefore starts clean unless the one-shot marker is
created again. Never use readback trace output as FPS evidence.

This rule was added after the 2026-08-24 TECNO CK8n/Mali reports for Sonic Mania (`CUSA07010`) and
Minit (`CUSA09747`) showed `source=sentinel` in otherwise live sessions. Those reports later emitted
periodic game heartbeats and contained no fatal signal, device loss, or ANR, so their
`previous-session` envelope is not crash evidence. Device acceptance of the cleanup remains
separate: it requires a new JIT and FEX report from the original TECNO device.
