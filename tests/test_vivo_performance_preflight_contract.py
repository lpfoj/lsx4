import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PREFLIGHT = (ROOT / "scripts" / "preflight-vivo-performance-test.ps1").read_text(
    encoding="utf-8"
)
INSTALLER = (ROOT / "scripts" / "install-vivo-apk.ps1").read_text(
    encoding="utf-8"
)


def _read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def test_preflight_is_hard_bound_to_dedicated_vivo_and_sm8650_transports():
    assert "$DeviceId = '10AG263F1B001HM'" in PREFLIGHT
    assert "$dedicatedVivoDeviceId = '10AG263F1B001HM'" in PREFLIGHT
    assert "$dedicatedSm8650DeviceId = '65d16af6'" in PREFLIGHT
    assert "$isSm8650Transport = $AdbTarget -ceq $dedicatedSm8650DeviceId" in PREFLIGHT
    assert "$PSBoundParameters.ContainsKey('DeviceId')" in PREFLIGHT
    assert "$DeviceId -cne $expectedDeviceId" in PREFLIGHT
    assert "$reportedSerial -cne $expectedDeviceId" in PREFLIGHT
    assert "@('shell', 'getprop', 'ro.serialno')" in PREFLIGHT
    assert '"$deviceLabel identity mismatch:' in PREFLIGHT
    assert "_adb-tls-connect\\._tcp" in PREFLIGHT

    direct_adb = [line.strip() for line in PREFLIGHT.splitlines() if "& $adb " in line]
    assert direct_adb == ["$output = @(& $adb -s $AdbTarget @Arguments 2>$null)"]


def test_one_agentchat_lease_covers_the_whole_preflight_not_each_adb_call():
    assert PREFLIGHT.count("& $python $agentchat event") == 1
    assert PREFLIGHT.count("& $python $agentchat own") == 2
    assert "$agentTarget = 'device:vivo'" in PREFLIGHT
    assert '$agentTarget = "device:sm8650-$dedicatedSm8650DeviceId"' in PREFLIGHT
    assert '$agentAdbTarget = "adb:$expectedDeviceId"' in PREFLIGHT
    lease = PREFLIGHT.split("function Enter-DevicePreflightLease", 1)[1].split(
        "function Invoke-Adb", 1
    )[0]
    assert "--area $agentTarget" in lease
    assert "--area $agentAdbTarget" in lease
    assert PREFLIGHT.count("Enter-DevicePreflightLease") == 2

    adb_wrapper = PREFLIGHT.split("function Invoke-Adb", 1)[1].split(
        "function Get-ProcessInventory", 1
    )[0]
    assert "Enter-DevicePreflightLease" not in adb_wrapper


def test_cleanup_uses_one_bulk_inventory_and_a_small_exact_allowlist():
    allowlist = PREFLIGHT.split("$diagnosticProcessNames = @(", 1)[1].split(")", 1)[0]
    assert re.findall(r"'([^']+)'", allowlist) == [
        "process-tracker",
        "simpleperf",
        "perfetto",
        "heapprofd",
        "screenrecord",
    ]
    assert "@('shell', 'ps', '-An', '-o', 'PID,UID,NAME,ARGS')" in PREFLIGHT
    assert "/proc/$" not in PREFLIGHT
    assert "'pidof'" not in PREFLIGHT
    assert "$maximumSignalBatchSize = 100" in PREFLIGHT
    assert "$Process.Name -cnotin $diagnosticProcessNames" in PREFLIGHT
    assert "$Process.Uid -eq 2000" in PREFLIGHT
    assert "$Process.Uid -eq $Lsx4Uid" in PREFLIGHT

    lowered = PREFLIGHT.lower()
    for forbidden in (
        "am force-stop",
        "killall",
        "pkill",
        "stop-app",
        "com.android.systemui",
    ):
        assert forbidden not in lowered


def test_every_signal_batch_revalidates_pid_uid_name_args_and_protects_lsx4():
    signal_helper = PREFLIGHT.split("function Invoke-VerifiedSignalBatches", 1)[1].split(
        "function Get-CpuSnapshot", 1
    )[0]
    assert "Get-ProcessInventory" in signal_helper
    assert "Test-SameProcessIdentity" in signal_helper
    assert "Test-IsEligibleDiagnosticProcess" in signal_helper
    assert "Refusing to signal changed or untrusted pid" in signal_helper
    assert "Group-Object Uid" in signal_helper
    assert "@('shell', 'kill', $signalArgument) + $processIds" in signal_helper
    assert "@('shell', 'run-as', $lsx4Package, 'kill', $signalArgument)" in signal_helper
    assert "[AllowEmptyCollection()]" in signal_helper
    assert "$Process.Name -ceq $lsx4Package" in PREFLIGHT
    assert '$Process.Name.StartsWith("${lsx4Package}:"' in PREFLIGHT
    assert PREFLIGHT.index("-Signal TERM") < PREFLIGHT.index("-Signal KILL")


def test_cleanup_only_is_safe_for_a_live_game_and_full_gate_is_fail_closed():
    for token in (
        "[switch]$CleanupOnly",
        "$MinimumCpuIdlePercent = 60.0",
        "$MinimumAvailableMemoryMb = 1024",
        "$MaximumThermalStatus = 2",
        "$MaximumSurfaceTemperatureC = 48.0",
        "$MinimumBatteryLevelPercent = 20",
        "$MinimumPolicy0MaxKHz = 2361600",
        "$MinimumPolicy6MaxKHz = 2438400",
        "Get-BatteryState",
        "Get-CpuPolicyState",
        "batteryLevel=",
        "externalPower=",
        "cpuPolicies=[policy0=",
        "$secondaryCpuPolicy = 6",
        "$secondaryCpuPolicy = 7",
        "Get-CpuPolicyState -Policy $secondaryCpuPolicy",
        "'/proc/stat'",
        "'/proc/meminfo'",
        "'dumpsys', 'thermalservice'",
        "'dumpsys', 'hardware_properties'",
        "hardwarePropertiesCpu",
        "/sys/class/thermal/thermal_zone*",
        "aggregate CPU idle",
        "thermal telemetry is unavailable",
        "Vivo performance preflight FAILED. Do not start an FPS measurement",
        "diagnostic processes=0; no LSX4 process was stopped",
    ):
        assert token in PREFLIGHT
    assert "'dumpsys', 'cpuinfo'" in PREFLIGHT
    assert PREFLIGHT.index("if ($CleanupOnly)") < PREFLIGHT.index(
        "$availableMemoryMb = Get-AvailableMemoryMb"
    )
    assert "diagnostic process active at final gate" in PREFLIGHT


def test_battery_and_cpu_policy_regexes_use_powershell_dotnet_escaping():


    assert '$escapedLabel = [regex]::Escape($label)' in PREFLIGHT
    assert '"(?im)^\\s*$escapedLabel powered:\\s*(true|false)\\s*$"' in PREFLIGHT
    assert '"(?im)^\\\\s*' not in PREFLIGHT

    policy_reader = PREFLIGHT.split("function Get-CpuPolicyState", 1)[1].split(
        "function Get-ThermalStatus", 1
    )[0]
    assert "@('shell', 'cat', \"$path/$name\")" in policy_reader
    assert "$output.Count -ne 1" in policy_reader
    assert "-notmatch '^\\d+$'" in policy_reader
    assert "'sh', '-c'" not in policy_reader


def test_fixed_cpu_trip_thresholds_are_not_treated_as_live_temperatures():
    processor_reader = PREFLIGHT.split("function Get-ProcessorTemperatures", 1)[
        1
    ].split("Enter-DevicePreflightLease", 1)[0]
    assert "$sensorName = $matches[1]" in processor_reader
    assert "(?:hw[-_])?trip" in processor_reader
    assert processor_reader.index("(?:hw[-_])?trip") < processor_reader.index(
        "$raw = [double]$matches[2]"
    )


def test_installer_and_live_capture_use_cleanup_only():
    installer_invocation = (
        "& $performancePreflight -DeviceId $DeviceId "
        "-AdbTarget $adbTarget -CleanupOnly"
    )
    assert installer_invocation in INSTALLER
    assert "Mandatory Vivo performance preflight not found" in INSTALLER
    assert INSTALLER.index("$reportedSerial =") < INSTALLER.index(installer_invocation)
    assert INSTALLER.index(installer_invocation) < INSTALLER.index(
        "try {\n    Assert-AgentChatDeviceLease -Action 'adb-install-apk'"
    )

    capture = _read("tools/capture_android_temporal_series.ps1")
    assert "preflight-vivo-performance-test.ps1" in capture
    assert "-AdbTarget $DeviceId -CleanupOnly" in capture
    assert capture.index("& $performancePreflight") < capture.index(
        "$seriesDir = [System.IO.Path]::GetFullPath"
    )
    assert "$startProcessDeviceId" in capture
    assert "if ($DeviceId -match '\\s')" in capture
    assert "'-s', $startProcessDeviceId, 'exec-out', 'screencap'" in capture


def test_bloodborne_pmu_runs_full_before_launch_and_cleanup_before_measurement():
    driver = _read("tools/codex_bloodborne_pmu.sh")
    main = driver.split("verify_canonical_fex_host || exit 51", 1)[1]
    assert main.index("stop_game || exit 65") < main.index(
        "run_performance_preflight full || exit 72"
    )
    assert main.index("run_performance_preflight full || exit 72") < main.index(
        "adb_device shell input keyevent KEYCODE_WAKEUP"
    )
    assert driver.count("run_performance_preflight cleanup-only || exit 72") == 1
    pre_measure = driver.index("run_performance_preflight cleanup-only || exit 72")
    assert pre_measure < driver.index(
        "dumpsys SurfaceFlinger --timestats -clear", pre_measure
    )


def test_other_launch_harnesses_fail_closed_before_starting_the_game():
    smoke_scripts = (
        "tools/codex_fex_title_smoke.sh",
        "tools/codex_ps5_fex_title_smoke.sh",
        "tools/codex_ps1_current_jit_smoke.sh",
    )
    for relative in smoke_scripts:
        script = _read(relative)
        assert "preflight-vivo-performance-test.ps1" in script
        assert "performance-preflight-pwsh-missing" in script
        launch_flow = script.split('adb_device shell am force-stop "$package"', 1)[1]
        assert launch_flow.index("run_performance_preflight") < launch_flow.index(
            "adb_device shell \"am start"
        )

    hle = _read("tools/codex_bloodborne_hle_clean_ab.sh")
    assert "preflight-vivo-performance-test.ps1" in hle
    assert "prepare_save_seed\nrun_performance_preflight" in hle
    assert "force_stop_and_verify_no_app_pid discovery-only\n    run_performance_preflight" in hle
