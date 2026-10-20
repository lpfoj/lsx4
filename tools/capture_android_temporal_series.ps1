[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$DeviceId,

    [string]$Package = 'app.lsx4.android',

    [ValidateSet('Vivo', 'PocoF7Pro')]
    [string]$DeviceProfile = 'Vivo',

    [ValidateRange(3, 240)]
    [int]$Frames = 24,

    [ValidateRange(100, 10000)]
    [int]$IntervalMilliseconds = 750,

    [string]$CompanionRunAsPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputDir
)

$ErrorActionPreference = 'Stop'
$adb = (Get-Command adb -ErrorAction Stop).Source
$startProcessDeviceId = if ($DeviceId -match '\s') {
    '"' + $DeviceId.Replace('"', '\"') + '"'
} else {
    $DeviceId
}
$repoRoot = Split-Path $PSScriptRoot -Parent
$performancePreflight = if ($DeviceProfile -eq 'PocoF7Pro') {
    Join-Path $repoRoot 'scripts\preflight-poco-f7-pro-performance-test.ps1'
} else {
    Join-Path $repoRoot 'scripts\preflight-vivo-performance-test.ps1'
}
if (-not (Test-Path -LiteralPath $performancePreflight -PathType Leaf)) {
    throw "Mandatory $DeviceProfile performance preflight not found: $performancePreflight"
}
# The capture target is intentionally already running. Remove/reject profilers
# immediately before the series without treating LSX4's own load as pollution.
if ($DeviceProfile -eq 'PocoF7Pro') {
    & $performancePreflight -AdbTarget $DeviceId -CleanupOnly
} else {
    & $performancePreflight -DeviceId '10AG263F1B001HM' `
        -AdbTarget $DeviceId -CleanupOnly
}

$seriesDir = [System.IO.Path]::GetFullPath($OutputDir)

if (Test-Path -LiteralPath $seriesDir) {
    $occupied = Get-ChildItem -LiteralPath $seriesDir -Force -ErrorAction Stop |
        Where-Object { $_.Name -eq 'capture-manifest.json' -or $_.Name -like 'frame-*.png' }
    if ($occupied) {
        throw "Output directory already contains a capture: $seriesDir"
    }
} else {
    New-Item -ItemType Directory -Path $seriesDir -Force | Out-Null
}

function Invoke-AdbText {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments)
    $output = & $adb -s $DeviceId @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "adb failed ($LASTEXITCODE): $($Arguments -join ' ')`n$output"
    }
    return ($output -join "`n").Trim()
}

function Get-GameState {
    $pidText = Invoke-AdbText shell pidof "${Package}:game"
    $activityText = Invoke-AdbText shell dumpsys activity activities
    $foreground = ($activityText -split "`n" |
        Where-Object { $_ -match 'topResumedActivity' } |
        Select-Object -First 1).Trim()
    [pscustomobject]@{
        pid = $pidText
        foreground = $foreground
        valid = (-not [string]::IsNullOrWhiteSpace($pidText)) -and
            $foreground.Contains("${Package}/.GameActivity")
    }
}

$initial = Get-GameState
if (-not $initial.valid) {
    throw "GameActivity is not foreground or the game process is missing: $($initial.foreground)"
}
$expectedPid = $initial.pid
$records = [System.Collections.Generic.List[object]]::new()
$captureValid = $true
$failure = $null

try {
    for ($index = 0; $index -lt $Frames; $index++) {
        $before = Get-GameState
        if (-not $before.valid -or $before.pid -ne $expectedPid) {
            throw "Game state changed before frame $index (expected PID $expectedPid, got $($before.pid)): $($before.foreground)"
        }

        $name = 'frame-{0:D4}.png' -f $index
        $path = Join-Path $seriesDir $name
        $stderrPath = Join-Path $seriesDir ('.frame-{0:D4}.stderr' -f $index)
        $process = Start-Process -FilePath $adb -ArgumentList @(
            '-s', $startProcessDeviceId, 'exec-out', 'screencap', '-p'
        ) -WindowStyle Hidden -Wait -PassThru `
          -RedirectStandardOutput $path -RedirectStandardError $stderrPath
        if ($process.ExitCode -ne 0) {
            $stderr = Get-Content -LiteralPath $stderrPath -Raw -ErrorAction SilentlyContinue
            throw "screencap failed for frame $index ($($process.ExitCode)): $stderr"
        }
        Remove-Item -LiteralPath $stderrPath -Force -ErrorAction SilentlyContinue

        $companionName = $null
        if (-not [string]::IsNullOrWhiteSpace($CompanionRunAsPath)) {
            $companionName = 'frame-{0:D4}.companion.bin' -f $index
            $companionPath = Join-Path $seriesDir $companionName
            $companionStderrPath = Join-Path $seriesDir ('.frame-{0:D4}.companion.stderr' -f $index)
            $companionProcess = Start-Process -FilePath $adb -ArgumentList @(
                '-s', $startProcessDeviceId, 'exec-out', 'run-as', $Package, 'cat', $CompanionRunAsPath
            ) -WindowStyle Hidden -Wait -PassThru `
              -RedirectStandardOutput $companionPath -RedirectStandardError $companionStderrPath
            if ($companionProcess.ExitCode -ne 0 -or (Get-Item -LiteralPath $companionPath).Length -eq 0) {
                $stderr = Get-Content -LiteralPath $companionStderrPath -Raw -ErrorAction SilentlyContinue
                throw "companion capture failed for frame $index ($($companionProcess.ExitCode)): $stderr"
            }
            Remove-Item -LiteralPath $companionStderrPath -Force -ErrorAction SilentlyContinue
        }

        $stream = [System.IO.File]::OpenRead($path)
        try {
            $signature = New-Object byte[] 8
            $read = $stream.Read($signature, 0, $signature.Length)
        } finally {
            $stream.Dispose()
        }
        $pngSignature = [byte[]](137, 80, 78, 71, 13, 10, 26, 10)
        if ($read -ne 8 -or [System.Linq.Enumerable]::SequenceEqual($signature, $pngSignature) -ne $true) {
            throw "Frame $index is not a valid PNG stream: $path"
        }

        $after = Get-GameState
        if (-not $after.valid -or $after.pid -ne $expectedPid) {
            throw "Game state changed after frame $index (expected PID $expectedPid, got $($after.pid)): $($after.foreground)"
        }
        $records.Add([pscustomobject]@{
            frame = $index
            file = $name
            capturedUtc = [DateTime]::UtcNow.ToString('o')
            pidBefore = $before.pid
            pidAfter = $after.pid
            foregroundBefore = $before.foreground
            foregroundAfter = $after.foreground
            companion = $companionName
        })
        if ($index + 1 -lt $Frames) {
            Start-Sleep -Milliseconds $IntervalMilliseconds
        }
    }
} catch {
    $captureValid = $false
    $failure = $_.Exception.Message
} finally {
    $manifest = [ordered]@{
        schema = 'lsx4.android-temporal-series.v1'
        valid = $captureValid
        failure = $failure
        deviceId = $DeviceId
        deviceProfile = $DeviceProfile
        package = $Package
        expectedPid = $expectedPid
        requestedFrames = $Frames
        capturedFrames = $records.Count
        intervalMilliseconds = $IntervalMilliseconds
        seriesFps = 1000.0 / $IntervalMilliseconds
        companionRunAsPath = $CompanionRunAsPath
        frames = $records
    }
    $manifest | ConvertTo-Json -Depth 6 |
        Set-Content -LiteralPath (Join-Path $seriesDir 'capture-manifest.json') -Encoding utf8
}

if (-not $captureValid) {
    throw $failure
}
Write-Output "ANDROID_TEMPORAL_SERIES_OK frames=$($records.Count) pid=$expectedPid dir=$seriesDir"
