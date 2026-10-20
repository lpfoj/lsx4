[CmdletBinding()]
param(
    [string]$AdbTarget = '65d16af6',

    [string]$ExpectedDriverSha256 =
        'b0c0ea26586d63d708df19ee203df260881c00e4135c0d58e119d2660cfbe47e',

    [string]$ExpectedApkSha256 = '',

    [string]$ExpectedRuntimeSha256 = '',

    [string]$OutputDirectory = '',

    [ValidateRange(30, 180)]
    [int]$RouteTimeoutSeconds = 90,

    [ValidateNotNullOrEmpty()]
    [int[]]$SampleSeconds = @(10, 25, 40, 55, 65, 75, 85, 95)
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$package = 'app.lsx4.android'
$gameProcess = $package + ':game'
$repo = Split-Path $PSScriptRoot -Parent
$marker = 'files/lsx4-home/run-diagnostic-sm8650-a750-residency'
$adb = (Get-Command adb -ErrorAction Stop).Source
$pwsh = (Get-Command pwsh -ErrorAction Stop).Source
$newline = [Environment]::NewLine

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repo ".codex-bb-proof/a750-d9-residency-$stamp"
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

$logcatProcess = $null
$routeProcess = $null
$initialPid = ''
$stopwatch = [Diagnostics.Stopwatch]::new()

function Invoke-AdbText {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    return ((@(& $adb -s $AdbTarget @Arguments) -join $newline).Trim())
}

function Get-GamePid {
    return Invoke-AdbText -Arguments @('shell', 'pidof', $gameProcess)
}

function Test-AppFile {
    param([Parameter(Mandatory = $true)][string]$Path)

    & $adb -s $AdbTarget shell run-as $package test -f $Path 2>$null
    return $LASTEXITCODE -eq 0
}

function Write-AdbLines {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    $lines = @(& $adb -s $AdbTarget @Arguments)
    [IO.File]::WriteAllLines($Destination, [string[]]$lines)
}

function Capture-Sample {
    param(
        [Parameter(Mandatory = $true)][int]$TargetSecond,
        [Parameter(Mandatory = $true)][string]$GamePid
    )

    $actualSecond = [math]::Round($stopwatch.Elapsed.TotalSeconds, 1)
    $label = 't{0:D3}' -f $TargetSecond
    $token = "d9res-$stamp-$label"
    $remoteProof = "files/lsx4-home/runtime/proof-metrics/snapshot-$token.json"
    $broadcast = Invoke-AdbText -Arguments @(
        'shell', 'am', 'broadcast',
        '-n', "$package/.ProofMetricsReceiver",
        '-a', "$package.CAPTURE_PROOF_METRICS",
        '--es', 'proof_request_token', $token
    )

    $proofRaw = ''
    $proofDeadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        Start-Sleep -Milliseconds 150
        if (Test-AppFile -Path $remoteProof) {
            $proofRaw = ((@(
                & $adb -s $AdbTarget exec-out run-as $package cat $remoteProof 2>$null
            ) -join $newline).Trim())
        }
    } while (
        -not $proofRaw.StartsWith('{', [StringComparison]::Ordinal) -and
        [DateTime]::UtcNow -lt $proofDeadline
    )

    if (-not $proofRaw.StartsWith('{', [StringComparison]::Ordinal)) {
        [IO.File]::WriteAllText(
            (Join-Path $OutputDirectory "$label-proof-missing.txt"),
            $broadcast + $newline)
    } else {
        [IO.File]::WriteAllText(
            (Join-Path $OutputDirectory "$label-proof.json"),
            $proofRaw + $newline)
        try {
            $proof = $proofRaw | ConvertFrom-Json
            if ([string]$proof.pid -cne $GamePid) {
                throw "Proof PID $($proof.pid) differs from game PID $GamePid"
            }
        } finally {
            & $adb -s $AdbTarget shell run-as $package rm -f $remoteProof | Out-Null
        }
    }

    Write-AdbLines -Arguments @('shell', 'dumpsys', 'meminfo', $GamePid) -Destination (Join-Path $OutputDirectory "$label-meminfo.txt")
    Write-AdbLines -Arguments @('shell', 'cat', "/proc/$GamePid/status") -Destination (Join-Path $OutputDirectory "$label-proc-status.txt")
    Write-AdbLines -Arguments @('exec-out', 'run-as', $package, 'cat', "/proc/$GamePid/smaps_rollup") -Destination (Join-Path $OutputDirectory "$label-smaps-rollup.txt")

    $remoteScreenshot = "/sdcard/lsx4-$token.png"
    & $adb -s $AdbTarget shell screencap -p $remoteScreenshot | Out-Null
    & $adb -s $AdbTarget pull $remoteScreenshot (Join-Path $OutputDirectory "$label-screen.png") | Out-Null
    & $adb -s $AdbTarget shell rm -f $remoteScreenshot | Out-Null

    $meta = "targetSecond=$TargetSecond" + $newline +
        "actualSecond=$actualSecond" + $newline +
        "pid=$GamePid" + $newline
    [IO.File]::WriteAllText(
        (Join-Path $OutputDirectory "$label-meta.txt"),
        $meta)
    Write-Output (
        "SAMPLE label=$label target=$TargetSecond actual=$actualSecond " +
        "pid=$GamePid proof=$(-not [string]::IsNullOrWhiteSpace($proofRaw))")
}

try {
    $driverRaw = Invoke-AdbText -Arguments @(
        'exec-out', 'run-as', $package, 'cat', 'files/vulkan-driver-status.json'
    )
    $driver = $driverRaw | ConvertFrom-Json
    if (
        $driver.sha256 -cne $ExpectedDriverSha256 -or
        $driver.effective -cne 'turnip' -or
        [string]$driver.deviceName -notlike '*750*' -or
        [string]$driver.driverInfo -notlike '*da14d65e44*'
    ) {
        throw "Exact D9 driver gate failed: $driverRaw"
    }
    [IO.File]::WriteAllText(
        (Join-Path $OutputDirectory 'driver-status-before.json'),
        $driverRaw + $newline)

    if (
        -not [string]::IsNullOrWhiteSpace((Get-GamePid)) -or
        -not [string]::IsNullOrWhiteSpace(
            (Invoke-AdbText -Arguments @('shell', 'pidof', $package)))
    ) {
        throw 'Cold-start gate failed: LSX4 PID already exists'
    }
    if (Test-AppFile -Path $marker) {
        throw 'Diagnostic marker unexpectedly existed before the run'
    }

    Write-AdbLines -Arguments @('shell', 'dumpsys', 'activity', 'exit-info', $package) -Destination (Join-Path $OutputDirectory 'exit-info-before.txt')

    & $adb -s $AdbTarget shell run-as $package touch $marker | Out-Null
    if (-not (Test-AppFile -Path $marker)) {
        throw 'Diagnostic marker was not created'
    }

    & $adb -s $AdbTarget logcat -c
    $logcatProcess = Start-Process -FilePath $adb -ArgumentList @(
        '-s', $AdbTarget, 'logcat', '-v', 'threadtime'
    ) -RedirectStandardOutput (Join-Path $OutputDirectory 'logcat-threadtime.txt') -RedirectStandardError (Join-Path $OutputDirectory 'logcat-stderr.txt') -WindowStyle Hidden -PassThru

    $routeProcess = Start-Process -FilePath $pwsh -ArgumentList @(
        '-NoProfile',
        '-File', (Join-Path $repo 'scripts/start-bloodborne-gameplay-vivo.ps1'),
        '-AdbTarget', $AdbTarget,
        '-TimeoutSeconds', [string]$RouteTimeoutSeconds
    ) -RedirectStandardOutput (Join-Path $OutputDirectory 'route-stdout.txt') -RedirectStandardError (Join-Path $OutputDirectory 'route-stderr.txt') -WindowStyle Hidden -PassThru

    $stopwatch.Start()
    $pidDeadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        Start-Sleep -Milliseconds 250
        $initialPid = Get-GamePid
    } while (
        $initialPid -notmatch '^\d+$' -and
        [DateTime]::UtcNow -lt $pidDeadline
    )
    if ($initialPid -notmatch '^\d+$') {
        throw 'Game PID did not appear within 20 seconds'
    }
    # Sample times are relative to the actual isolated game-process spawn, not
    # to route preflight or MainActivity startup.
    $stopwatch.Restart()

    $identity = "pid=$initialPid" + $newline +
        "startUtc=$([DateTime]::UtcNow.ToString('o'))" + $newline +
        "apkSha256=$ExpectedApkSha256" + $newline +
        "runtimeSha256=$ExpectedRuntimeSha256" + $newline +
        "driverSha256=$ExpectedDriverSha256" + $newline
    [IO.File]::WriteAllText(
        (Join-Path $OutputDirectory 'run-identity.txt'),
        $identity)
    Write-Output "RUN_STARTED pid=$initialPid out=$OutputDirectory"

    foreach ($target in $SampleSeconds) {
        while ($stopwatch.Elapsed.TotalSeconds -lt $target) {
            Start-Sleep -Milliseconds 250
            $currentPid = Get-GamePid
            if ([string]::IsNullOrWhiteSpace($currentPid)) {
                break
            }
            if ($currentPid -cne $initialPid) {
                throw "PID changed from $initialPid to $currentPid"
            }
        }

        $currentPid = Get-GamePid
        if ([string]::IsNullOrWhiteSpace($currentPid)) {
            Write-Output (
                "GAME_EXITED beforeTarget=$target " +
                "elapsed=$([math]::Round($stopwatch.Elapsed.TotalSeconds, 1))")
            break
        }
        if ($currentPid -cne $initialPid) {
            throw "PID changed from $initialPid to $currentPid"
        }
        Capture-Sample -TargetSecond $target -GamePid $initialPid
    }

    Write-AdbLines -Arguments @('shell', 'dumpsys', 'activity', 'exit-info', $package) -Destination (Join-Path $OutputDirectory 'exit-info-pre-force-stop.txt')

    $routeProcess.Refresh()
    $routeExit = if ($routeProcess.HasExited) {
        [string]$routeProcess.ExitCode
    } else {
        'running'
    }
    $routeState = "hasExited=$($routeProcess.HasExited)" + $newline +
        "exitCode=$routeExit" + $newline +
        "elapsedSeconds=$([math]::Round($stopwatch.Elapsed.TotalSeconds, 1))" +
        $newline
    [IO.File]::WriteAllText(
        (Join-Path $OutputDirectory 'route-state.txt'),
        $routeState)
} catch {
    [IO.File]::WriteAllText(
        (Join-Path $OutputDirectory 'collector-error.txt'),
        $_.Exception.ToString() + $newline)
    Write-Output "COLLECTOR_ERROR $($_.Exception.Message)"
} finally {
    try {
        & $adb -s $AdbTarget shell am force-stop $package | Out-Null
    } catch {
    }
    Start-Sleep -Seconds 1
    try {
        & $adb -s $AdbTarget shell run-as $package rm -f $marker | Out-Null
    } catch {
    }
    Start-Sleep -Milliseconds 500

    if ($routeProcess) {
        try {
            $routeProcess.Refresh()
            if (-not $routeProcess.HasExited) {
                Stop-Process -Id $routeProcess.Id -Force
            }
        } catch {
        }
    }
    if ($logcatProcess) {
        try {
            $logcatProcess.Refresh()
            if (-not $logcatProcess.HasExited) {
                Stop-Process -Id $logcatProcess.Id -Force
            }
        } catch {
        }
    }

    Write-AdbLines -Arguments @('shell', 'dumpsys', 'activity', 'exit-info', $package) -Destination (Join-Path $OutputDirectory 'exit-info-after.txt')

    $markerPresent = Test-AppFile -Path $marker
    $gamePidAfter = Get-GamePid
    $appPidAfter = Invoke-AdbText -Arguments @('shell', 'pidof', $package)
    $cleanup = "markerPresent=$markerPresent" + $newline +
        "gamePid=$gamePidAfter" + $newline +
        "appPid=$appPidAfter" + $newline
    [IO.File]::WriteAllText(
        (Join-Path $OutputDirectory 'cleanup-state.txt'),
        $cleanup)
    Write-Output (
        "CLEANUP markerPresent=$markerPresent " +
        "gamePid=$gamePidAfter appPid=$appPidAfter")
    Write-Output "ARTIFACT_DIR=$OutputDirectory"
}
