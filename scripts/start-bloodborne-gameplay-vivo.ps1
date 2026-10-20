[CmdletBinding()]
param(
    [string]$AdbTarget = 'adb-10AG263F1B001HM-2MNzzP._adb-tls-connect._tcp',

    [ValidateRange(30, 180)]
    [int]$TimeoutSeconds = 90,

    [ValidateRange(500, 5000)]
    [int]$PollMilliseconds = 1500,

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

    [ValidateSet('preference', 'on', 'off')]
    [string]$FsrMode = 'preference',

    [bool]$DebugHud = $false
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
    "lsx4-bloodborne-route-$PID.png"

function Invoke-Adb {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments)

    $output = @(& adb -s $AdbTarget @Arguments)
    if ($LASTEXITCODE -ne 0) {
        throw "adb failed ($LASTEXITCODE): $($Arguments -join ' ')"
    }
    return $output
}

function Press-Cross {
    # Vivo input coordinates are 3168x1440 even though captured frames may be scaled.
    Invoke-Adb shell input swipe 2850 1150 2850 1150 80 | Out-Null
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

& (Join-Path $PSScriptRoot 'preflight-vivo-performance-test.ps1') `
    -AdbTarget $AdbTarget -CpuSampleSeconds 2
if ($LASTEXITCODE -ne 0) {
    throw 'Mandatory Vivo performance preflight failed'
}

Invoke-Adb shell am force-stop $package | Out-Null
Invoke-Adb logcat -c | Out-Null
$routeStopwatch = [Diagnostics.Stopwatch]::StartNew()

if (Test-Path -LiteralPath $agentChat -PathType Leaf) {
    & python $agentChat event --agent codex --room $room `
        --action bloodborne-fast-route-start --target device:vivo `
        --summary 'adaptive Bloodborne route; transient classifier frames; persistent screenshots only on unexpected state' |
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
$drawRate = 0.0
try {
    while ([DateTime]::UtcNow -lt $deadline) {
        $gamePid = ((@(& adb -s $AdbTarget shell pidof $gameProcess)) -join '').Trim()
        if ($gamePid -notmatch '^\d+$') {
            Start-Sleep -Milliseconds $PollMilliseconds
            continue
        }

        if ($continueSent) {
            # No input is ever emitted after Continue.  Exact cumulative runtime
            # counters reject title/menu false positives without a screenshot.
            $drawRate = Get-RuntimeDrawRate
            if ($drawRate -ge $GameplayDrawCallsPerSecond) {
                $usedCores = Get-GameAverageUsedCores -GamePid $gamePid
                if ($usedCores -ge 2.0) {
                    Remove-Item -LiteralPath $routeScreen -Force `
                        -ErrorAction SilentlyContinue
                    Write-Output ((
                        'Bloodborne gameplay ready: pid={0}; draw={1:N0}/s; ' +
                        'workload={2:N2} cores; route={3:N2}s; ' +
                        'no Cross sent after Continue.') -f
                        $gamePid, $drawRate, $usedCores,
                        $routeStopwatch.Elapsed.TotalSeconds)
                    return
                }
            }
            # Some title frames reject the first tap while the menu animation
            # is settling.  Re-tap only after positively reclassifying the
            # still-visible Continue menu; never infer it from low draw alone.
            $screen = Get-ScreenState
            if ([string]$screen.state -eq 'continue-menu' -and
                ([DateTime]::UtcNow - $lastPressAt).TotalSeconds -ge 3) {
                Press-Cross
                $lastPressAt = [DateTime]::UtcNow
            }
            Start-Sleep -Milliseconds $PollMilliseconds
            continue
        }

        $screen = Get-ScreenState
        $state = [string]$screen.state
        if ($state -ne $lastState) {
            Write-Output "Bloodborne route state: $state"
            $lastState = $state
        }

        # The brightness-only classifier intentionally has no gameplay
        # template.  On a bright scene it can therefore report
        # startup-clickable even when autoload resumed directly into gameplay,
        # without showing Offline/Continue.  Before every startup Cross, use
        # the same exact draw/workload gates as the normal Continue path.  This
        # prevents a gameplay Cross from opening a menu or activating a world
        # prompt; no input is emitted by this recovery.
        if ($state -eq 'startup-clickable') {
            $drawRate = Get-RuntimeDrawRate
            if ($drawRate -ge $GameplayDrawCallsPerSecond) {
                $usedCores = Get-GameAverageUsedCores -GamePid $gamePid
                if ($usedCores -ge 2.0) {
                    Remove-Item -LiteralPath $routeScreen -Force `
                        -ErrorAction SilentlyContinue
                    Write-Output ((
                        'Bloodborne gameplay ready: pid={0}; draw={1:N0}/s; ' +
                        'workload={2:N2} cores; route={3:N2}s; ' +
                        'recovery=direct-or-post-offline-draw-gate; no gameplay Cross sent.') -f
                        $gamePid, $drawRate, $usedCores,
                        $routeStopwatch.Elapsed.TotalSeconds)
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
    Invoke-Adb shell am force-stop $package | Out-Null
    throw $reason
} catch {
    if ($_.Exception.Message -notlike '*adaptive route timed out*') {
        Save-UnexpectedScreen -Reason $_.Exception.Message
        Invoke-Adb shell am force-stop $package | Out-Null
    }
    throw
} finally {
    Remove-Item -LiteralPath $routeScreen -Force -ErrorAction SilentlyContinue
}
