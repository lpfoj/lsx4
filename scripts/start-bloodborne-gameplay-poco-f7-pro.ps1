[CmdletBinding()]
param(
    [string]$AdbTarget = '65d16af6',

    [ValidateRange(30, 300)]
    [int]$TimeoutSeconds = 90,

    [ValidateRange(500, 5000)]
    [int]$PollMilliseconds = 1500,

    [ValidateRange(5, 30)]
    [int]$PostContinueScreenIntervalSeconds = 10,

    # Full-resolution SurfaceFlinger capture can stall this exact A750 path.
    # Performance routes therefore trust runtime counters after Continue and
    # fail closed on timeout instead of observing the screen again.
    [bool]$EnablePostContinueScreenObservation = $false,

    [ValidateRange(1000, 100000)]
    [double]$GameplayDrawCallsPerSecond = 5000,

    [ValidateRange(0, 7)]
    [int]$RenderResolutionMode = 4,

    [ValidateRange(0, 7)]
    [Alias('FsrOutputResolutionMode')]
    [int]$UpscaledOutputResolutionMode = 2,

    [ValidateRange(0, 9)]
    [int]$UpscalerMode = 1,

    [ValidateRange(0, 100)]
    [int]$SceneRenderScalePercent = 0,

    [ValidatePattern('^[0-9a-fA-F]{64}$')]
    [string]$TurnipSha256 =
        'b0c0ea26586d63d708df19ee203df260881c00e4135c0d58e119d2660cfbe47e',

    [ValidateSet('preference', 'on', 'off')]
    [string]$FsrMode = 'preference',

    [bool]$DebugHud = $false,

    # Keeps driver/runtime/geometry diagnosis moving while HyperOS is holding
    # a post-load CPU clamp.  This still performs the exact diagnostic-process
    # cleanup, but its FPS is never valid as a final performance result.
    [switch]$AllowThrottledFunctionalRun
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path $PSScriptRoot -Parent
$package = 'app.lsx4.android'
$gameProcess = 'app.lsx4.android:game'
$gamePath = '/storage/emulated/0/Emulation/PS4/games/CUSA03173/eboot.bin'
$agentChat = Join-Path (Split-Path $repo -Parent) 'agentchat\agentchat.py'
$room = 'увеличить fps'
$classifier = Join-Path $repo 'tools\bloodborne-proof\classify_screen.py'
$routeScreen = Join-Path ([IO.Path]::GetTempPath()) `
    "lsx4-bloodborne-poco-route-$PID.png"

function Invoke-Adb {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments)

    $output = @(& adb -s $AdbTarget @Arguments)
    if ($LASTEXITCODE -ne 0) {
        throw "adb failed ($LASTEXITCODE): $($Arguments -join ' ')"
    }
    return $output
}

function Assert-PocoF7Pro {
    $model = ((Invoke-Adb shell getprop ro.product.model) -join '').Trim()
    $soc = ((Invoke-Adb shell getprop ro.soc.model) -join '').Trim()
    if ($model -ne '24117RK2CG' -or $soc -ne 'SM8650') {
        throw "POCO F7 Pro route rejected device: model=$model soc=$soc target=$AdbTarget"
    }
}

function Select-VerifiedImportedTurnip {
    $sha = $TurnipSha256.ToLowerInvariant()
    $token = 'poco-{0}-{1}' -f $PID,
        ([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())
    $resultPath = "files/vulkan-drivers/control-result-$token.json"
    Invoke-Adb -Arguments @(
        'shell', 'am', 'broadcast', '--include-stopped-packages',
        '-n', "$package/.VulkanDriverControlReceiver",
        '-a', "$package.SELECT_IMPORTED_VULKAN_DRIVER",
        '--es', 'request_token', $token,
        '--es', 'sha256', $sha
    ) | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        & adb -s $AdbTarget shell run-as $package test -s $resultPath
        if ($LASTEXITCODE -eq 0) {
            break
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($LASTEXITCODE -ne 0) {
        throw "Imported Turnip selection timed out: $sha"
    }
    try {
        $raw = @(& adb -s $AdbTarget exec-out run-as $package cat $resultPath)
        if ($LASTEXITCODE -ne 0) {
            throw "Imported Turnip selection result could not be read: $sha"
        }
        $result = (($raw -join "`n") | ConvertFrom-Json)
        $expectedSelection = "imported:$sha"
        if (-not [bool]$result.success -or
            [string]$result.selectionId -ne $expectedSelection) {
            throw "Imported Turnip selection failed: $([string]$result.error)"
        }
        Write-Output "Verified imported Turnip selected: $sha"
    } finally {
        & adb -s $AdbTarget shell run-as $package rm -f $resultPath | Out-Null
    }
}

function Clear-PocoBenchmarkBackground {
    # These are user-facing/background packages observed resident on the POCO
    # test device. force-stop is process-only: it does not clear package data,
    # saves, imports, caches, permissions, or disable any package. Android may
    # restart a service later; the goal is a reproducible cold performance run.
    $backgroundPackages = @(
        'ru.rolf.rolf.branch',
        'com.google.android.youtube',
        'com.android.chrome',
        'com.spotify.music',
        'com.facebook.katana',
        'com.google.android.apps.messaging',
        'com.android.vending',
        'com.google.android.googlequicksearchbox',
        'com.google.android.gm',
        'com.google.android.apps.photos',
        'com.google.android.calendar'
    )
    foreach ($backgroundPackage in $backgroundPackages) {
        Invoke-Adb shell am force-stop $backgroundPackage | Out-Null
    }
    Start-Sleep -Seconds 5
}

function Get-PocoGamePid {
    return ((@(& adb -s $AdbTarget shell pidof $gameProcess)) -join '').Trim()
}

function Get-PocoLsx4ProcessLines {
    return @(& adb -s $AdbTarget shell ps -A -o PID,ARGS) |
        Where-Object { $_ -match '(^|\s)app\.lsx4\.android(?::game)?(\s|$)' }
}

function Stop-PocoLsx4Processes {
    # HyperOS can leave the separate :game process alive when force-stop is
    # issued without an explicit Android user. A reused process makes a
    # nominally cold benchmark inherit gigabytes of swap and page faults.
    Invoke-Adb shell am force-stop --user 0 $package | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    $emptySamples = 0
    do {
        $survivors = @(Get-PocoLsx4ProcessLines)
        if ($survivors.Count -eq 0) {
            $emptySamples++
        } else {
            $emptySamples = 0
        }
        if ($emptySamples -ge 2) {
            Write-Output 'POCO LSX4 process cleanup verified: no main/game PID remains.'
            return
        }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)

    # run-as has the application UID and can terminate only this package's
    # processes. This is a process-only fallback; it never clears app data.
    foreach ($line in $survivors) {
        if ($line -match '^\s*(\d+)\s+') {
            & adb -s $AdbTarget shell run-as $package kill -9 $Matches[1] | Out-Null
        }
    }
    Start-Sleep -Milliseconds 400
    $remaining = @(Get-PocoLsx4ProcessLines)
    if ($remaining.Count -ne 0) {
        throw "POCO stale LSX4 process survived exact UID cleanup: $($remaining -join '; ')"
    }
    Write-Output 'POCO LSX4 process cleanup required same-UID kill fallback; verified stopped.'
}

function Get-GameProcessStartTicks {
    param([string]$GamePid)

    $stat = ((Invoke-Adb shell cat "/proc/$GamePid/stat") -join '').Trim()
    $close = $stat.LastIndexOf(')')
    if ($close -lt 0) {
        throw "Malformed /proc/$GamePid/stat"
    }
    $fields = @($stat.Substring($close + 2) -split '\s+')
    if ($fields.Count -le 19) {
        throw "Incomplete /proc/$GamePid/stat"
    }
    return [int64]$fields[19]
}

function Assert-FreshPocoGameProcess {
    param(
        [string]$GamePid,
        [int64]$ExpectedStartTicks,
        [DateTime]$FirstSeenAt,
        [double]$MaximumAgeSeconds
    )

    $currentStartTicks = Get-GameProcessStartTicks -GamePid $GamePid
    if ($currentStartTicks -ne $ExpectedStartTicks) {
        throw (
            "POCO benchmark rejected replaced game process: pid=$GamePid " +
            "start=$currentStartTicks expected=$ExpectedStartTicks")
    }
    # Android may specialize a pre-forked USAP whose kernel task starttime is
    # older than the LSX4 lifecycle. The verified empty-process boundary and
    # first post-launch observation define the real benchmark instance age.
    $age = ([DateTime]::UtcNow - $FirstSeenAt).TotalSeconds
    if ($age -lt 0 -or $age -gt $MaximumAgeSeconds) {
        throw (
            "POCO benchmark rejected stale game process: pid=$GamePid " +
            "age=$([math]::Round($age, 2))s maximum=$([math]::Round($MaximumAgeSeconds, 2))s")
    }
    return $age
}

function Arm-PocoGameTurboPerformanceSession {
    # HyperOS applies LSX4's registered Game Turbo/Wild Boost profile only
    # after the application becomes foreground. Warm only MainActivity with
    # every runtime autostart disabled, then hand the automation intent to that
    # same singleTop activity without another force-stop. No guest work or
    # gameplay input occurs during this warm-up.
    Stop-PocoLsx4Processes
    Invoke-Adb -Arguments @(
        'shell', 'am', 'start', '-W', '-n', "$package/.MainActivity",
        '--ez', 'autoload_existing_runtime', 'false',
        '--ez', 'autoinit_runtime', 'false',
        '--ez', 'fullscreen_render', 'false'
    ) | Out-Null

    $policyPath = '/sys/devices/system/cpu/cpufreq/policy7/scaling_max_freq'
    $minimumGameMaxKHz = 2937600
    $deadline = [DateTime]::UtcNow.AddSeconds(8)
    $maxKHz = 0
    do {
        $raw = ((Invoke-Adb shell cat $policyPath) -join '').Trim()
        if ([int]::TryParse($raw, [ref]$maxKHz) -and
            $maxKHz -ge $minimumGameMaxKHz) {
            break
        }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($maxKHz -lt $minimumGameMaxKHz) {
        throw (
            "POCO Game Turbo warm launch did not reach the performance ceiling: " +
            "policy7 scaling_max_freq=$maxKHz KHz, required >=$minimumGameMaxKHz KHz")
    }
    Write-Output "POCO Game Turbo warm launch: policy7 scaling_max_freq=$maxKHz KHz."
}

function Assert-PocoGameTurboActive {
    param([string]$GamePid)

    $policyPath = '/sys/devices/system/cpu/cpufreq/policy7/scaling_max_freq'
    $minimumGameMaxKHz = 2937600
    $deadline = [DateTime]::UtcNow.AddSeconds(8)
    $maximumKHz = 0
    do {
        $livePid = ((Invoke-Adb shell pidof $gameProcess) -join '').Trim()
        if ($livePid -ne $GamePid) {
            throw "Bloodborne process changed before Game Turbo verification: $GamePid -> $livePid"
        }
        $raw = ((Invoke-Adb shell cat $policyPath) -join '').Trim()
        if ([int]::TryParse($raw, [ref]$maximumKHz) -and
            $maximumKHz -ge $minimumGameMaxKHz) {
            Write-Output (
                "POCO Game Turbo active: policy7 scaling_max_freq=$maximumKHz KHz.")
            return
        }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    throw (
        "POCO Game Turbo did not activate for Bloodborne: policy7 " +
        "scaling_max_freq=$maximumKHz KHz, required >=$minimumGameMaxKHz KHz")
}

function Assert-PocoUnlocked {
    param([string]$Phase)

    # A secure HyperOS keyguard can leave GameActivity in Resumed state while
    # NotificationShade owns window focus.  The runtime must remain paused in
    # that state, so launching through it would deadlock the first guest-thread
    # admission behind the legitimate WINDOW_FOCUS_LOST lease.  Wake and ask
    # Android to dismiss only a non-secure/trusted keyguard, then fail before
    # launch if interactive authentication is still required.  Never alter or
    # bypass the user's lock credential.
    Invoke-Adb shell input keyevent 224 | Out-Null
    Invoke-Adb shell wm dismiss-keyguard | Out-Null
    Start-Sleep -Milliseconds 300
    $policy = (Invoke-Adb shell dumpsys window policy) -join "`n"
    $keyguard = [regex]::Match(
        $policy,
        '(?s)KeyguardServiceDelegate\s+showing=(true|false).*?secure=(true|false)')
    if (-not $keyguard.Success) {
        throw "POCO keyguard state unavailable during $Phase"
    }
    if ($keyguard.Groups[1].Value -eq 'true') {
        $secure = $keyguard.Groups[2].Value
        throw "POCO is locked during $Phase (secure=$secure); unlock it before the route starts"
    }
}

function Press-Cross {
    # POCO F7 Pro reports 1440x3200 in portrait; Bloodborne input is 3200x1440.
    Invoke-Adb shell input swipe 2950 1220 2950 1220 80 | Out-Null
}

function Get-ScreenState {
    & adb -s $AdbTarget exec-out screencap -p > $routeScreen
    if ($LASTEXITCODE -ne 0 -or
        -not (Test-Path -LiteralPath $routeScreen -PathType Leaf)) {
        throw 'Temporary route screencap failed'
    }
    $raw = @(& python $classifier $routeScreen --json)
    if ($LASTEXITCODE -ne 0) {
        throw 'Bloodborne screen classifier failed'
    }
    return (($raw -join "`n") | ConvertFrom-Json)
}

function Get-ProofHudSnapshot {
    $token = 'route-{0}-{1}' -f $PID, `
        ([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())
    $path = "files/lsx4-home/runtime/proof-metrics/snapshot-$token.json"
    Invoke-Adb -Arguments @(
        'shell', 'am', 'broadcast',
        '-n', "$package/.ProofMetricsReceiver",
        '-a', "$package.CAPTURE_PROOF_METRICS",
        '--es', 'proof_request_token', $token
    ) | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        & adb -s $AdbTarget shell run-as $package test -s $path
        if ($LASTEXITCODE -eq 0) {
            break
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($LASTEXITCODE -ne 0) {
        throw "Runtime proof snapshot timed out: $token"
    }
    try {
        $raw = @(& adb -s $AdbTarget exec-out run-as $package cat $path)
        if ($LASTEXITCODE -ne 0) {
            throw "Runtime proof snapshot could not be read: $token"
        }
        return (($raw -join "`n") | ConvertFrom-Json)
    } finally {
        & adb -s $AdbTarget shell run-as $package rm -f $path | Out-Null
    }
}

function Get-RuntimeDrawRate {
    $before = Get-ProofHudSnapshot
    Start-Sleep -Milliseconds 1000
    $after = Get-ProofHudSnapshot
    if ($before.pid -ne $after.pid) {
        throw 'Game process changed during runtime draw-rate gate'
    }
    $elapsedNs = [double]$after.hud.bootBeforeNs -
        [double]$before.hud.bootAfterNs
    if ($elapsedNs -le 0) {
        throw 'Runtime draw-rate gate has an invalid time boundary'
    }
    $drawDelta = [double]$after.hud.counters.drawCalls -
        [double]$before.hud.counters.drawCalls
    return $drawDelta / ($elapsedNs / 1e9)
}

function Get-GameAverageUsedCores {
    param(
        [Parameter(Mandatory = $true)][string]$GamePid,
        [int]$SampleSeconds = 2
    )

    $readTicks = {
        $line = ((Invoke-Adb shell cat "/proc/$GamePid/stat") -join '').Trim()
        $close = $line.LastIndexOf(')')
        if ($close -lt 0) {
            throw "Malformed /proc/$GamePid/stat"
        }
        $fields = @($line.Substring($close + 2) -split '\s+')
        return [int64]$fields[11] + [int64]$fields[12]
    }
    $clockTicks = [double](((Invoke-Adb shell getconf CLK_TCK) -join '').Trim())
    $before = & $readTicks
    Start-Sleep -Seconds $SampleSeconds
    $after = & $readTicks
    return (($after - $before) / $clockTicks) / $SampleSeconds
}

function Save-UnexpectedScreen {
    param([string]$Reason)

    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $path = Join-Path $repo ".codex-bb-unexpected-$stamp.png"
    if (Test-Path -LiteralPath $routeScreen -PathType Leaf) {
        Copy-Item -LiteralPath $routeScreen -Destination $path -Force
    } else {
        & adb -s $AdbTarget exec-out screencap -p > $path
    }
    Write-Warning "Bloodborne route failed ($Reason); screenshot: $path"
}

Assert-PocoF7Pro
Assert-PocoUnlocked -Phase 'preflight'
Clear-PocoBenchmarkBackground
# POCO owns an independent device-identity and policy7 performance gate.  It
# intentionally does not call or modify either Vivo script.
if ($AllowThrottledFunctionalRun) {
    & (Join-Path $PSScriptRoot 'preflight-poco-f7-pro-performance-test.ps1') `
        -AdbTarget $AdbTarget -CleanupOnly
    Write-Warning (
        'POCO functional run: CPU/thermal measurement gates were not applied; ' +
        'FPS from this launch is not benchmark evidence.')
} else {
    & (Join-Path $PSScriptRoot 'preflight-poco-f7-pro-performance-test.ps1') `
        -AdbTarget $AdbTarget -CpuSampleSeconds 2
}
if ($LASTEXITCODE -ne 0) {
    throw 'Mandatory POCO F7 Pro performance preflight failed'
}

Stop-PocoLsx4Processes
Select-VerifiedImportedTurnip
Stop-PocoLsx4Processes
Invoke-Adb logcat -c | Out-Null
Arm-PocoGameTurboPerformanceSession
Assert-PocoUnlocked -Phase 'launch'
$routeStopwatch = [Diagnostics.Stopwatch]::StartNew()

if (Test-Path -LiteralPath $agentChat -PathType Leaf) {
    & python $agentChat event --agent codex --room $room `
        --action bloodborne-fast-route-start --target device:sm8650-65d16af6 `
        --summary 'POCO F7 Pro adaptive Bloodborne route; transient classifier frames; persistent screenshots only on unexpected state' |
        Out-Null
}

$launchArguments = @(
    'shell', 'am', 'start', '-n', "$package/.MainActivity",
    '--ez', 'autoload_existing_runtime', 'true',
    '--ez', 'autoinit_runtime', 'true',
    '--es', 'scan_game_path', $gamePath,
    '--es', 'launch_game_path', $gamePath,
    '--es', 'game_platform', 'ps4',
    '--ez', 'fullscreen_render', 'true',
    '--ez', 'embedded_aarch64_jit_backend', 'true',
    '--ei', 'render_resolution_mode', [string]$RenderResolutionMode,
    '--ei', 'upscaled_output_resolution_mode', [string]$UpscaledOutputResolutionMode,
    '--ei', 'upscaler_mode', [string]$UpscalerMode,
    '--ez', 'profile_graphics_override', 'true',
    '--ez', 'profile_disable_dynamic_shadows', 'true',
    '--ez', 'profile_disable_ssao', 'true',
    '--ez', 'profile_disable_motion_blur', 'true',
    '--ez', 'profile_disable_depth_of_field', 'true',
    '--ez', 'profile_disable_anti_aliasing', 'true',
    '--ez', 'profile_disable_chromatic_aberration', 'true'
)
if ($SceneRenderScalePercent -gt 0) {
    $launchArguments += @(
        '--ei', 'scene_render_scale', [string]$SceneRenderScalePercent)
}
if ($FsrMode -ne 'preference') {
    $launchArguments += @(
        '--ez', 'profile_fsr_enabled',
        $(if ($FsrMode -eq 'on') { 'true' } else { 'false' }))
}
if ($DebugHud) {
    $launchArguments += @('--ez', 'input_debug_hud', 'true')
}
Invoke-Adb -Arguments $launchArguments | Out-Null

# Navigate by screen structure, never by cumulative sleeps.  Temporary frames
# are overwritten and deleted; only Save-UnexpectedScreen persists one.
$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
$offlineSelected = $false
$continueSent = $false
$lastState = ''
$lastPressAt = [DateTime]::MinValue
$lastPostContinueScreenAt = [DateTime]::MinValue
$drawRate = 0.0
$observedGamePid = ''
$observedGameStartTicks = [int64]0
$observedGameFirstSeenAt = [DateTime]::MinValue
try {
    while ([DateTime]::UtcNow -lt $deadline) {
        $gamePid = ((@(& adb -s $AdbTarget shell pidof $gameProcess)) -join '').Trim()
        if ($gamePid -notmatch '^\d+$') {
            Start-Sleep -Milliseconds $PollMilliseconds
            continue
        }
        if ([string]::IsNullOrWhiteSpace($observedGamePid)) {
            $observedGamePid = $gamePid
            $observedGameStartTicks = Get-GameProcessStartTicks -GamePid $gamePid
            $observedGameFirstSeenAt = [DateTime]::UtcNow
            Write-Output (
                "POCO post-launch game witness: pid=$gamePid " +
                "startTicks=$observedGameStartTicks")
        } elseif ($gamePid -ne $observedGamePid) {
            throw "POCO game PID changed during cold route: $observedGamePid -> $gamePid"
        }

        if ($continueSent) {
            # No input is ever emitted after Continue.  Exact cumulative runtime
            # counters reject title/menu false positives without a screenshot.
            $drawRate = Get-RuntimeDrawRate
            if ($drawRate -ge $GameplayDrawCallsPerSecond) {
                $usedCores = Get-GameAverageUsedCores -GamePid $gamePid
                if ($usedCores -ge 2.0) {
                    Assert-PocoGameTurboActive -GamePid $gamePid
                    $processAge = Assert-FreshPocoGameProcess -GamePid $gamePid `
                        -ExpectedStartTicks $observedGameStartTicks `
                        -FirstSeenAt $observedGameFirstSeenAt `
                        -MaximumAgeSeconds ($routeStopwatch.Elapsed.TotalSeconds + 15)
                    Remove-Item -LiteralPath $routeScreen -Force `
                        -ErrorAction SilentlyContinue
                    Write-Output ((
                        'Bloodborne gameplay ready: pid={0}; draw={1:N0}/s; ' +
                        'workload={2:N2} cores; route={3:N2}s; age={4:N2}s; ' +
                        'no Cross sent after Continue.') -f
                        $gamePid, $drawRate, $usedCores,
                        $routeStopwatch.Elapsed.TotalSeconds, $processAge)
                    return
                }
            }
            # Some title frames reject the first tap while the menu animation
            # is settling.  Re-tap only after positively reclassifying the
            # still-visible Continue menu; never infer it from low draw alone.
            if (-not $EnablePostContinueScreenObservation) {
                Start-Sleep -Milliseconds $PollMilliseconds
                continue
            }
            if (([DateTime]::UtcNow - $lastPostContinueScreenAt).TotalSeconds -lt
                $PostContinueScreenIntervalSeconds) {
                Start-Sleep -Milliseconds $PollMilliseconds
                continue
            }
            $lastPostContinueScreenAt = [DateTime]::UtcNow
            $screen = Get-ScreenState
            $isLandscape = [int]$screen.width -gt [int]$screen.height
            if ($isLandscape -and
                [string]$screen.state -eq 'continue-menu' -and
                ([DateTime]::UtcNow - $lastPressAt).TotalSeconds -ge 3) {
                Press-Cross
                $lastPressAt = [DateTime]::UtcNow
            }
            Start-Sleep -Milliseconds $PollMilliseconds
            continue
        }

        $screen = Get-ScreenState
        $state = [string]$screen.state
        $isLandscape = [int]$screen.width -gt [int]$screen.height
        if ($state -ne $lastState) {
            Write-Output "Bloodborne route state: $state"
            $lastState = $state
        }

        # HyperOS exposes a portrait 1440x3200 bootstrap surface before the
        # guest swaps to Bloodborne's 3200x1440 surface.  The brightness-only
        # classifier can call that transient spinner startup-clickable; never
        # emit game input until the guest surface is actually landscape.
        if (-not $isLandscape) {
            Start-Sleep -Milliseconds $PollMilliseconds
            continue
        }

        # The brightness-only classifier intentionally has no gameplay
        # template.  A bright scene can therefore report startup-clickable,
        # while the retained Iosefka clinic scene is dark enough to report
        # dark-transition.  Before every possible startup Cross, use the same
        # exact draw/workload gates as the normal Continue path for both
        # ambiguous states.  This prevents a gameplay Cross from opening a
        # menu or activating a world prompt; no input is emitted by recovery.
        if ($state -eq 'startup-clickable' -or $state -eq 'dark-transition') {
            $drawRate = Get-RuntimeDrawRate
            if ($drawRate -ge $GameplayDrawCallsPerSecond) {
                $usedCores = Get-GameAverageUsedCores -GamePid $gamePid
                if ($usedCores -ge 2.0) {
                    Assert-PocoGameTurboActive -GamePid $gamePid
                    $processAge = Assert-FreshPocoGameProcess -GamePid $gamePid `
                        -ExpectedStartTicks $observedGameStartTicks `
                        -FirstSeenAt $observedGameFirstSeenAt `
                        -MaximumAgeSeconds ($routeStopwatch.Elapsed.TotalSeconds + 15)
                    Remove-Item -LiteralPath $routeScreen -Force `
                        -ErrorAction SilentlyContinue
                    Write-Output ((
                        'Bloodborne gameplay ready: pid={0}; draw={1:N0}/s; ' +
                        'workload={2:N2} cores; route={3:N2}s; age={4:N2}s; ' +
                        'recovery=ambiguous-screen-draw-gate; no gameplay Cross sent.') -f
                        $gamePid, $drawRate, $usedCores,
                        $routeStopwatch.Elapsed.TotalSeconds, $processAge)
                    return
                }
            }
        }
        $canPress = ([DateTime]::UtcNow - $lastPressAt).TotalMilliseconds -ge `
            $PollMilliseconds

        if ($state -eq 'continue-menu' -and $canPress) {
            Press-Cross
            $continueSent = $true
            $lastPressAt = [DateTime]::UtcNow
            $lastPostContinueScreenAt = [DateTime]::UtcNow
            Start-Sleep -Seconds 3
            continue
        }
        if ($state -eq 'online-menu' -and $canPress) {
            Press-Cross
            $offlineSelected = $true
            $lastPressAt = [DateTime]::UtcNow
            Start-Sleep -Milliseconds $PollMilliseconds
            continue
        }
        if (-not $offlineSelected -and $state -eq 'startup-clickable' -and
            $canPress) {
            Press-Cross
            $lastPressAt = [DateTime]::UtcNow
        }
        Start-Sleep -Milliseconds $PollMilliseconds
    }

    $reason = 'adaptive route timed out before exact gameplay draw gate'
    Save-UnexpectedScreen -Reason $reason
    Stop-PocoLsx4Processes
    throw $reason
} catch {
    if ($_.Exception.Message -notlike '*adaptive route timed out*') {
        Save-UnexpectedScreen -Reason $_.Exception.Message
        Stop-PocoLsx4Processes
    }
    throw
} finally {
    Remove-Item -LiteralPath $routeScreen -Force -ErrorAction SilentlyContinue
}
