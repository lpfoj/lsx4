from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PREFLIGHT_PATH = ROOT / "scripts" / "preflight-poco-f7-pro-performance-test.ps1"
ROUTE_PATH = ROOT / "scripts" / "start-bloodborne-gameplay-poco-f7-pro.ps1"
DOC_PATH = ROOT / "docs" / "POCO_F7_PRO_BLOODBORNE_ROUTE_2026-08-28.md"

PREFLIGHT = PREFLIGHT_PATH.read_text(encoding="utf-8")
ROUTE = ROUTE_PATH.read_text(encoding="utf-8")
DOC = DOC_PATH.read_text(encoding="utf-8")
DRIVER_MANAGER = (
    ROOT
    / "android-app/app/src/main/java/app/lsx4/android/VulkanDriverManager.java"
).read_text(encoding="utf-8")


def test_poco_owns_separate_route_preflight_and_documentation():
    assert "preflight-poco-f7-pro-performance-test.ps1" in ROUTE
    assert "preflight-vivo-performance-test.ps1" not in ROUTE
    assert "start-bloodborne-gameplay-vivo.ps1" not in ROUTE
    assert "preflight-vivo-performance-test.ps1" not in PREFLIGHT
    assert "start-bloodborne-gameplay-vivo.ps1" not in PREFLIGHT
    assert ROUTE_PATH.name in DOC
    assert PREFLIGHT_PATH.name in DOC
    assert "Neither POCO script calls or modifies" in DOC


def test_poco_preflight_is_fail_closed_to_exact_sm8650_device():
    for token in (
        "$DeviceId = '65d16af6'",
        "$dedicatedPocoDeviceId = '65d16af6'",
        "$AdbTarget -cne $dedicatedPocoDeviceId",
        "ro.product.model",
        "$reportedModel -cne '24117RK2CG'",
        "ro.soc.model",
        "$reportedSoc -cne 'SM8650'",
        "$secondaryCpuPolicy = 7",
        "$MinimumPolicy7MaxKHz = 1593600",
        'device:sm8650-$dedicatedPocoDeviceId',
    ):
        assert token in PREFLIGHT
    for vivo_identity in ("10AG263F1B001HM", "device:vivo", "wirelessAdbSerialPattern"):
        assert vivo_identity not in PREFLIGHT


def test_poco_preflight_preserves_process_and_measurement_safety_gates():
    for token in (
        "Invoke-VerifiedSignalBatches",
        "Test-SameProcessIdentity",
        "Test-IsEligibleDiagnosticProcess",
        "Refusing to signal changed or untrusted pid",
        "[switch]$CleanupOnly",
        "$MinimumCpuIdlePercent = 60.0",
        "$MinimumAvailableMemoryMb = 1024",
        "$MaximumThermalStatus = 2",
        "$MaximumSurfaceTemperatureC = 48.0",
        "Get-BatteryState",
        "Get-CpuPolicyState -Policy $secondaryCpuPolicy",
        "diagnostic processes=0; no LSX4 process was stopped",
    ):
        assert token in PREFLIGHT
    lowered = PREFLIGHT.lower()
    for forbidden in ("am force-stop", "killall", "pkill", "pm clear"):
        assert forbidden not in lowered


def test_poco_route_never_sleeps_into_secure_keyguard_and_fails_before_launch():
    assert "input keyevent 223" not in ROUTE
    assert "function Assert-PocoUnlocked" in ROUTE
    assert ROUTE.count("Assert-PocoUnlocked -Phase") == 2
    assert "KeyguardServiceDelegate" in ROUTE
    assert "wm dismiss-keyguard" in ROUTE
    assert "never changes or" in DOC and "bypasses a lock credential" in DOC
    assert ROUTE.rindex("Assert-PocoUnlocked -Phase 'preflight'") < ROUTE.rindex(
        "Clear-PocoBenchmarkBackground"
    )
    assert ROUTE.index("Assert-PocoUnlocked -Phase 'launch'") < ROUTE.index(
        "Invoke-Adb -Arguments $launchArguments"
    )


def test_poco_route_preserves_bachata_and_selects_exact_turnip():
    assert "com.bachatas4.android" not in ROUTE
    assert "pm clear" not in ROUTE.lower()
    assert "b0c0ea26586d63d708df19ee203df260881c00e4135c0d58e119d2660cfbe47e" in ROUTE
    assert "SELECT_IMPORTED_VULKAN_DRIVER" in ROUTE
    assert "Verified imported Turnip selected" in ROUTE
    assert "leaves HyperOS Home" in DOC
    assert "Bachata running" in DOC


def test_explicit_same_sha_selection_rearms_without_disabling_recovery():
    select_driver = DRIVER_MANAGER.split("static void selectDriver", 1)[1].split(
        "private static boolean persistSelection", 1
    )[0]
    begin_launch = DRIVER_MANAGER.split("static LaunchDecision beginGameLaunch", 1)[1].split(
        "static void confirmGameLaunch", 1
    )[0]
    activation = DRIVER_MANAGER.split(
        "private static void activateSelectedImported", 1
    )[1].split("private static DriverPackage readActiveImported", 1)[0]

    assert "activateSelectedImported(context, driver, true)" in select_driver
    assert "activateSelectedImported(context, requested, false)" in begin_launch
    assert "!explicitReselection && current != null" in activation
    assert 'active.put("consecutiveFailures", 0)' in activation
    assert 'active.put("launchPending", false)' in activation
    assert 'persisted.optBoolean("launchPending", true)' in activation
    assert 'persisted.optInt("consecutiveFailures", -1) != 0' in activation
    assert "failures >= MAX_CONSECUTIVE_FAILURES" in begin_launch


def test_throttled_functional_mode_cannot_be_confused_with_final_fps_proof():
    assert "[switch]$AllowThrottledFunctionalRun" in ROUTE
    functional = ROUTE.split("if ($AllowThrottledFunctionalRun)", 1)[1].split(
        "if ($LASTEXITCODE -ne 0)", 1
    )[0]
    assert "-CleanupOnly" in functional
    assert "FPS from this launch is not benchmark evidence" in functional
    assert "-CpuSampleSeconds 2" in functional
    assert "cannot satisfy the 30–35 FPS gate" in DOC


def test_poco_arms_game_turbo_after_preflight_without_breaking_foreground_handoff():
    warm = ROUTE.split("function Arm-PocoGameTurboPerformanceSession", 1)[1].split(
        "function Assert-PocoUnlocked", 1
    )[0]
    assert "$package/.MainActivity" in warm
    assert "'autoload_existing_runtime', 'false'" in warm
    assert "'autoinit_runtime', 'false'" in warm
    assert "'fullscreen_render', 'false'" in warm
    assert "policy7/scaling_max_freq" in warm
    assert "Stop-PocoLsx4Processes" in warm
    assert "$minimumGameMaxKHz = 2937600" in warm
    after_start = warm.split("Invoke-Adb -Arguments", 1)[1]
    assert "am force-stop" not in after_start
    assert "launch_game_path" not in warm
    assert ROUTE.count("Arm-PocoGameTurboPerformanceSession") == 2
    preflight = ROUTE.index("if ($AllowThrottledFunctionalRun)")
    select_driver = ROUTE.rindex("Select-VerifiedImportedTurnip")
    arm = ROUTE.rindex("Arm-PocoGameTurboPerformanceSession")
    launch = ROUTE.index("Invoke-Adb -Arguments $launchArguments")
    assert preflight < select_driver < arm < launch
    assert "am force-stop $package" not in ROUTE[arm:launch]
    assert "1593600" in DOC and "3244800" in DOC
    assert "starts no guest" in DOC


def test_poco_rejects_reused_swap_thrashed_game_processes():
    cleanup = ROUTE.split("function Stop-PocoLsx4Processes", 1)[1].split(
        "function Get-GameProcessStartTicks", 1
    )[0]
    assert "am force-stop --user 0 $package" in cleanup
    assert "run-as $package kill -9" in cleanup
    assert "no main/game PID remains" in cleanup
    freshness = ROUTE.split("function Assert-FreshPocoGameProcess", 1)[1].split(
        "function Arm-PocoGameTurboPerformanceSession", 1
    )[0]
    assert "Get-GameProcessStartTicks" in freshness
    assert "$FirstSeenAt" in freshness
    assert "benchmark rejected stale game process" in freshness
    assert "POCO post-launch game witness" in ROUTE
    assert ROUTE.count("Assert-FreshPocoGameProcess -GamePid $gamePid") == 2


def test_poco_rate_limits_full_screen_observation_after_continue():
    assert "[int]$PostContinueScreenIntervalSeconds = 10" in ROUTE
    assert "[bool]$EnablePostContinueScreenObservation = $false" in ROUTE
    assert "$lastPostContinueScreenAt = [DateTime]::MinValue" in ROUTE
    after_continue = ROUTE.split("if ($continueSent)", 1)[1].split(
        "$screen = Get-ScreenState", 1
    )[0]
    assert "Get-RuntimeDrawRate" in after_continue
    assert "if (-not $EnablePostContinueScreenObservation)" in after_continue
    assert "$PostContinueScreenIntervalSeconds" in after_continue
    assert "Start-Sleep -Milliseconds $PollMilliseconds" in after_continue


def test_poco_verifies_game_turbo_again_on_the_live_game_process():
    gate = ROUTE.split("function Assert-PocoGameTurboActive", 1)[1].split(
        "function Assert-PocoUnlocked", 1
    )[0]
    assert "$minimumGameMaxKHz = 2937600" in gate
    assert "pidof $gameProcess" in gate
    assert "scaling_max_freq" in gate
    assert ROUTE.count("Assert-PocoGameTurboActive -GamePid $gamePid") == 2
    assert "idle ceiling" in DOC
    assert "at least" in DOC and "2937600" in DOC


def test_poco_dark_gameplay_recovery_is_draw_gated_and_input_free():
    start = ROUTE.index(
        "if ($state -eq 'startup-clickable' -or $state -eq 'dark-transition')"
    )
    end = ROUTE.index("$canPress =", start)
    recovery = ROUTE[start:end]
    assert "Get-RuntimeDrawRate" in recovery
    assert "Get-GameAverageUsedCores" in recovery
    assert "recovery=ambiguous-screen-draw-gate" in recovery
    assert "Press-Cross" not in recovery
