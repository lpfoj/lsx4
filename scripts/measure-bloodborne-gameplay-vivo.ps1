[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[a-zA-Z0-9_-]+$')]
    [string]$Phase,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [string]$AdbTarget =
        'adb-10AG263F1B001HM-2MNzzP._adb-tls-connect._tcp',

    [ValidateRange(5, 60)]
    [int]$MeasureSeconds = 12,

    [ValidateRange(0, 8)]
    [double]$MinimumExpectedCores = 2.0,

    [bool]$CloseAfterMeasure = $true,

    [bool]$CaptureUnexpectedScreen = $true,

    [ValidateSet('Vivo', 'PocoF7Pro')]
    [string]$DeviceProfile = 'Vivo'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path $PSScriptRoot -Parent
$package = 'app.lsx4.android'
$gameProcess = 'app.lsx4.android:game'
$output = New-Item -ItemType Directory -Force -Path $OutputDirectory
$metaRemote = "/sdcard/lsx4-bb-$Phase-meta.txt"
$sfRemote = "/sdcard/lsx4-bb-$Phase-sf.txt"
$metaLocal = Join-Path $output.FullName 'meta.txt'
$sfLocal = Join-Path $output.FullName 'surfaceflinger.txt'
$sfJson = Join-Path $output.FullName 'surfaceflinger.json'
$summaryJson = Join-Path $output.FullName 'summary.json'
$proofStartJson = Join-Path $output.FullName 'proof-start.json'
$proofEndJson = Join-Path $output.FullName 'proof-end.json'
$performancePolicy = if ($DeviceProfile -eq 'PocoF7Pro') { 'policy7' } else { 'policy6' }
$thermalSensor = if ($DeviceProfile -eq 'PocoF7Pro') { 'quiet_therm' } else { 'tz_shell' }

function Invoke-Adb {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    $result = @(& adb -s $AdbTarget @Arguments)
    if ($LASTEXITCODE -ne 0) {
        throw "adb failed ($LASTEXITCODE): $($Arguments -join ' ')"
    }
    return $result
}

function Get-ProofHudSnapshot {
    $token = 'measure-{0}-{1}' -f $PID, `
        ([Guid]::NewGuid().ToString('N').Substring(0, 12))
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

function Get-CounterDelta {
    param(
        [Parameter(Mandatory = $true)]$Before,
        [Parameter(Mandatory = $true)]$After,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $beforeProperty = $Before.PSObject.Properties[$Name]
    $afterProperty = $After.PSObject.Properties[$Name]
    if ($null -eq $beforeProperty -or $null -eq $afterProperty) {
        return $null
    }
    return [double]$afterProperty.Value - [double]$beforeProperty.Value
}

function Get-Median {
    param([double[]]$Values)

    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -eq 0) {
        return 0.0
    }
    if (($sorted.Count % 2) -ne 0) {
        return $sorted[[int][math]::Floor($sorted.Count / 2)]
    }
    return ($sorted[$sorted.Count / 2 - 1] +
            $sorted[$sorted.Count / 2]) / 2
}

function Parse-ProcStat {
    param([string]$Line)

    $close = $Line.LastIndexOf(')')
    if ($close -lt 0) {
        throw "Malformed /proc stat: $Line"
    }
    $fields = @($Line.Substring($close + 2) -split '\s+')
    return [pscustomobject]@{
        Minor = [int64]$fields[7]
        Major = [int64]$fields[9]
        User = [int64]$fields[11]
        System = [int64]$fields[12]
    }
}

try {
    $gamePid = ((Invoke-Adb -Arguments @(
        'shell', 'pidof', $gameProcess
    )) -join '').Trim()
    if ($gamePid -notmatch '^\d+$') {
        throw 'Bloodborne gameplay process is absent'
    }

    $proofStart = Get-ProofHudSnapshot

    $remoteMeasure = @'
game_pid=$(pidof app.lsx4.android:game) || exit 31
meta_path=__META__
sf_path=__SF__
dumpsys SurfaceFlinger --timestats -enable >/dev/null
dumpsys SurfaceFlinger --timestats -clear >/dev/null
dumpsys SurfaceFlinger --timestats -enable >/dev/null
(
  echo "PID|$game_pid"
  echo "CLK|$(getconf CLK_TCK 2>/dev/null || echo 100)"
  echo "UP0|$(cut -d' ' -f1 /proc/uptime)"
  echo -n "STAT0|"; cat /proc/$game_pid/stat
  for z in /sys/class/thermal/thermal_zone*; do
    n=$(cat "$z/type" 2>/dev/null) || continue
    case "$n" in
      tz_shell|tz_shell_tmax|tz_game_shell|battery|quiet_therm)
        echo "TEMP0|$n|$(cat "$z/temp" 2>/dev/null)";;
    esac
  done
  i=0
  while [ $i -lt __SECONDS__ ]; do
    echo "FREQ|$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq)|$(cat /sys/devices/system/cpu/cpufreq/__PERF_POLICY__/scaling_cur_freq)"
    sleep 1
    i=$((i+1))
  done
  echo "UP1|$(cut -d' ' -f1 /proc/uptime)"
  echo -n "STAT1|"; cat /proc/$game_pid/stat
  for z in /sys/class/thermal/thermal_zone*; do
    n=$(cat "$z/type" 2>/dev/null) || continue
    case "$n" in
      tz_shell|tz_shell_tmax|tz_game_shell|battery|quiet_therm)
        echo "TEMP1|$n|$(cat "$z/temp" 2>/dev/null)";;
    esac
  done
) > "$meta_path"
dumpsys SurfaceFlinger --timestats -dump > "$sf_path"
dumpsys SurfaceFlinger --timestats -clear >/dev/null
'@
    $remoteMeasure = $remoteMeasure.Replace('__META__', $metaRemote).
        Replace('__SF__', $sfRemote).
        Replace('__SECONDS__', [string]$MeasureSeconds).
        Replace('__PERF_POLICY__', $performancePolicy)
    # PowerShell materializes here-strings with CRLF on Windows. Android's
    # mksh treats the retained carriage return as part of redirection targets
    # and control-flow tokens (for example /dev/null\r and do\r).
    $remoteMeasure = $remoteMeasure.Replace("`r`n", "`n").Replace("`r", "")
    Invoke-Adb -Arguments @('shell', $remoteMeasure) | Out-Null
    Invoke-Adb -Arguments @('pull', $metaRemote, $metaLocal) | Out-Null
    Invoke-Adb -Arguments @('pull', $sfRemote, $sfLocal) | Out-Null
    $proofEnd = Get-ProofHudSnapshot
    if ($proofStart.pid -ne $proofEnd.pid -or
        [int]$proofStart.pid -ne [int]$gamePid) {
        throw 'Game process changed during the measurement proof window'
    }
    $proofStart | ConvertTo-Json -Depth 10 | Set-Content -Encoding utf8 $proofStartJson
    $proofEnd | ConvertTo-Json -Depth 10 | Set-Content -Encoding utf8 $proofEndJson

    & python (Join-Path $repo `
        'tools\bloodborne-proof\parse_surfaceflinger_timestats.py') `
        $sfLocal --output $sfJson
    if ($LASTEXITCODE -ne 0) {
        throw 'SurfaceFlinger timestats parser rejected the measured window'
    }

    $meta = @(Get-Content $metaLocal)
    $stat0 = Parse-ProcStat (($meta | Where-Object {
        $_ -like 'STAT0|*'
    } | Select-Object -First 1).Substring(6))
    $stat1 = Parse-ProcStat (($meta | Where-Object {
        $_ -like 'STAT1|*'
    } | Select-Object -First 1).Substring(6))
    $culture = [Globalization.CultureInfo]::InvariantCulture
    $up0 = [double]::Parse((($meta | Where-Object {
        $_ -like 'UP0|*'
    }) -replace '^UP0\|', ''), $culture)
    $up1 = [double]::Parse((($meta | Where-Object {
        $_ -like 'UP1|*'
    }) -replace '^UP1\|', ''), $culture)
    $clockTicks = [double](($meta | Where-Object {
        $_ -like 'CLK|*'
    }) -replace '^CLK\|', '')
    $wallSeconds = $up1 - $up0
    $cpuSeconds = (($stat1.User + $stat1.System) -
                   ($stat0.User + $stat0.System)) / $clockTicks
    $frequencies = @($meta | Where-Object {
        $_ -like 'FREQ|*'
    } | ForEach-Object {
        $parts = $_ -split '\|'
        [pscustomobject]@{
            Policy0 = [double]$parts[1]
            Policy6 = [double]$parts[2]
        }
    })
    $temp0Line = $meta | Where-Object {
        $_ -like "TEMP0|$thermalSensor|*"
    } | Select-Object -First 1
    $temp1Line = $meta | Where-Object {
        $_ -like "TEMP1|$thermalSensor|*"
    } | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace([string]$temp0Line) -or
        [string]::IsNullOrWhiteSpace([string]$temp1Line)) {
        throw "Required thermal sensor was not captured: $thermalSensor"
    }
    $temp0 = [double](($temp0Line -split '\|')[2]) / 1000.0
    $temp1 = [double](($temp1Line -split '\|')[2]) / 1000.0
    $sf = Get-Content $sfJson -Raw | ConvertFrom-Json

    $averageUsedCores = $cpuSeconds / $wallSeconds
    $runtimeWindowSeconds = (
        [double]$proofEnd.hud.bootBeforeNs -
        [double]$proofStart.hud.bootAfterNs) / 1e9
    if ($runtimeWindowSeconds -le 0) {
        throw 'Runtime counter window is invalid'
    }
    $drawDelta = Get-CounterDelta -Before $proofStart.hud.counters `
        -After $proofEnd.hud.counters -Name 'drawCalls'
    $submitDelta = Get-CounterDelta -Before $proofStart.hud.counters `
        -After $proofEnd.hud.counters -Name 'submitCalls'
    $presentDelta = Get-CounterDelta -Before $proofStart.hud.counters `
        -After $proofEnd.hud.counters -Name 'presentCalls'
    $generatedDelta = Get-CounterDelta -Before $proofStart.hud.counters `
        -After $proofEnd.hud.counters -Name 'generatedBlocks'
    $jitDelta = Get-CounterDelta -Before $proofStart.hud.counters `
        -After $proofEnd.hud.counters -Name 'fexJitCompilations'
    $jitTimeDelta = Get-CounterDelta -Before $proofStart.hud.counters `
        -After $proofEnd.hud.counters -Name 'fexJitCompileTimeNs'
    $residencyGcPassesDelta = Get-CounterDelta `
        -Before $proofStart.gpuResidency.memory `
        -After $proofEnd.gpuResidency.memory -Name 'residencyGcPasses'
    $residencyGcEvictionsDelta = Get-CounterDelta `
        -Before $proofStart.gpuResidency.memory `
        -After $proofEnd.gpuResidency.memory -Name 'residencyGcEvictions'
    $residencyGcNoVictimSkipsDelta = Get-CounterDelta `
        -Before $proofStart.gpuResidency.memory `
        -After $proofEnd.gpuResidency.memory -Name 'residencyGcNoVictimSkips'
    $textureGcPassesDelta = Get-CounterDelta `
        -Before $proofStart.gpuResidency.memory `
        -After $proofEnd.gpuResidency.memory -Name 'textureGcPasses'
    $textureGcEvictionsDelta = Get-CounterDelta `
        -Before $proofStart.gpuResidency.memory `
        -After $proofEnd.gpuResidency.memory -Name 'textureGcEvictions'
    $textureGcNoVictimSkipsDelta = Get-CounterDelta `
        -Before $proofStart.gpuResidency.memory `
        -After $proofEnd.gpuResidency.memory -Name 'textureGcNoVictimSkips'
    $textureGcForcedPassesDelta = Get-CounterDelta `
        -Before $proofStart.gpuResidency.memory `
        -After $proofEnd.gpuResidency.memory -Name 'textureGcForcedPasses'
    $textureGcAuditPassesDelta = Get-CounterDelta `
        -Before $proofStart.gpuResidency.memory `
        -After $proofEnd.gpuResidency.memory -Name 'textureGcAuditPasses'
    $bufferGcPassesDelta = Get-CounterDelta `
        -Before $proofStart.gpuResidency.memory `
        -After $proofEnd.gpuResidency.memory -Name 'bufferGcPasses'
    $bufferGcEvictionsDelta = Get-CounterDelta `
        -Before $proofStart.gpuResidency.memory `
        -After $proofEnd.gpuResidency.memory -Name 'bufferGcEvictions'
    $bufferGcNoVictimSkipsDelta = Get-CounterDelta `
        -Before $proofStart.gpuResidency.memory `
        -After $proofEnd.gpuResidency.memory -Name 'bufferGcNoVictimSkips'
    $bufferGcForcedPassesDelta = Get-CounterDelta `
        -Before $proofStart.gpuResidency.memory `
        -After $proofEnd.gpuResidency.memory -Name 'bufferGcForcedPasses'
    $bufferGcAuditPassesDelta = Get-CounterDelta `
        -Before $proofStart.gpuResidency.memory `
        -After $proofEnd.gpuResidency.memory -Name 'bufferGcAuditPasses'
    $summary = [ordered]@{
        schema = 'lsx4.bloodborne-fast-phase/v1'
        phase = $Phase
        pid = [int]$gamePid
        wallSeconds = [math]::Round($wallSeconds, 3)
        processCpuSeconds = [math]::Round($cpuSeconds, 3)
        averageUsedCores = [math]::Round($averageUsedCores, 4)
        runtimeCounterWindowSeconds = [math]::Round($runtimeWindowSeconds, 4)
        runtimeDrawCallsPerSecond = [math]::Round(
            $drawDelta / $runtimeWindowSeconds, 3)
        runtimeSubmitCallsPerSecond = [math]::Round(
            $submitDelta / $runtimeWindowSeconds, 3)
        runtimePresentCallsPerSecond = [math]::Round(
            $presentDelta / $runtimeWindowSeconds, 3)
        runtimeGeneratedBlocksDelta = [int64]$generatedDelta
        minorFaults = [int64]($stat1.Minor - $stat0.Minor)
        majorFaults = [int64]($stat1.Major - $stat0.Major)
        policy0MedianGHz = [math]::Round(
            (Get-Median @($frequencies.Policy0)) / 1000000.0, 4)
        performancePolicy = $performancePolicy
        performancePolicyMedianGHz = [math]::Round(
            (Get-Median @($frequencies.Policy6)) / 1000000.0, 4)
        thermalSensor = $thermalSensor
        thermalStartC = [math]::Round($temp0, 2)
        thermalEndC = [math]::Round($temp1, 2)
        sfFps = [math]::Round([double]$sf.layer.aggregateReportedFps, 3)
        sfFrames = [int]$sf.layer.totalFrames
        sfP50UpperMs = [int]$sf.present2present.p50UpperBoundMs
        sfP95UpperMs = [int]$sf.present2present.p95UpperBoundMs
        sfP99UpperMs = [int]$sf.present2present.p99UpperBoundMs
        sfGt40Fraction = [math]::Round(
            [double]$sf.present2present.gt40MsFraction, 6)
        retiredInstructionsPerSecond =
            'unavailable: retail PMU exactMask=0'
    }
    if ($DeviceProfile -eq 'Vivo') {
        $summary.policy6MedianGHz = $summary.performancePolicyMedianGHz
        $summary.tzShellStartC = $summary.thermalStartC
        $summary.tzShellEndC = $summary.thermalEndC
    }
    if ($null -ne $proofEnd.hud.counters.PSObject.Properties['upscalerSchema']) {
        $upscalerFramesDelta = Get-CounterDelta -Before $proofStart.hud.counters `
            -After $proofEnd.hud.counters -Name 'upscalerDispatchFrames'
        $summary.upscalerProof = [ordered]@{
            schema = [int64]$proofEnd.hud.counters.upscalerSchema
            requestedMode = [int64]$proofEnd.hud.counters.upscalerRequestedMode
            effectiveMode = [int64]$proofEnd.hud.counters.upscalerEffectiveMode
            dispatchFramesDelta = [int64]$upscalerFramesDelta
            dispatchFramesPerSecond = [math]::Round(
                $upscalerFramesDelta / $runtimeWindowSeconds, 3)
            fallbackReason = [int64]$proofEnd.hud.counters.upscalerFallbackReason
            nativeTemporalInputs =
                [bool]([int64]$proofEnd.hud.counters.temporalNativeInputs -ne 0)
            sourceWidth = [int64]$proofEnd.hud.counters.fsrSourceWidth
            sourceHeight = [int64]$proofEnd.hud.counters.fsrSourceHeight
            outputWidth = [int64]$proofEnd.hud.counters.fsrOutputWidth
            outputHeight = [int64]$proofEnd.hud.counters.fsrOutputHeight
        }
    }
    if ($null -ne $proofStart.fex -and $null -ne $proofEnd.fex) {
        $summary.fexProof = [ordered]@{
            instrumentationState = [string]$proofStart.fex.instrumentationState
            validMask = [uint64]$proofStart.fex.validMask
            exactMask = [uint64]$proofStart.fex.exactMask
            staticMask = [uint64]$proofStart.fex.staticMask
            dynamicMask = [uint64]$proofStart.fex.dynamicMask
            perturbedMask = [uint64]$proofStart.fex.perturbedMask
            start = $proofStart.fex.values
            end = $proofEnd.fex.values
        }
    }
    if ($null -ne $jitDelta -and $null -ne $jitTimeDelta) {
        $summary.fexJitCompilationsDelta = [int64]$jitDelta
        $summary.fexJitCompileMilliseconds = [math]::Round(
            $jitTimeDelta / 1000000.0, 3)
    }
    if ($null -ne $residencyGcPassesDelta -and
        $null -ne $residencyGcEvictionsDelta -and
        $null -ne $residencyGcNoVictimSkipsDelta) {
        $summary.gpuResidencyProof = [ordered]@{
            passesDelta = [int64]$residencyGcPassesDelta
            evictionsDelta = [int64]$residencyGcEvictionsDelta
            noVictimSkipsDelta = [int64]$residencyGcNoVictimSkipsDelta
            passesPerPresent = if ($presentDelta -gt 0) {
                [math]::Round($residencyGcPassesDelta / $presentDelta, 4)
            } else { 0.0 }
            noVictimSkipsPerPresent = if ($presentDelta -gt 0) {
                [math]::Round($residencyGcNoVictimSkipsDelta / $presentDelta, 4)
            } else { 0.0 }
            texture = [ordered]@{
                passesDelta = [int64]$textureGcPassesDelta
                evictionsDelta = [int64]$textureGcEvictionsDelta
                noVictimSkipsDelta = [int64]$textureGcNoVictimSkipsDelta
                forcedPassesDelta = [int64]$textureGcForcedPassesDelta
                auditPassesDelta = [int64]$textureGcAuditPassesDelta
                attemptsDelta = [int64](
                    $textureGcPassesDelta + $textureGcNoVictimSkipsDelta)
            }
            buffer = [ordered]@{
                passesDelta = [int64]$bufferGcPassesDelta
                evictionsDelta = [int64]$bufferGcEvictionsDelta
                noVictimSkipsDelta = [int64]$bufferGcNoVictimSkipsDelta
                forcedPassesDelta = [int64]$bufferGcForcedPassesDelta
                auditPassesDelta = [int64]$bufferGcAuditPassesDelta
                attemptsDelta = [int64](
                    $bufferGcPassesDelta + $bufferGcNoVictimSkipsDelta)
            }
            startPssBytes = [int64]$proofStart.gpuResidency.memory.androidProcessTotalPssBytes
            endPssBytes = [int64]$proofEnd.gpuResidency.memory.androidProcessTotalPssBytes
            startGraphicsBytes = [int64]$proofStart.gpuResidency.memory.androidProcessGraphicsBytes
            endGraphicsBytes = [int64]$proofEnd.gpuResidency.memory.androidProcessGraphicsBytes
            startSwapBytes = [int64]$proofStart.gpuResidency.memory.androidProcessTotalSwapBytes
            endSwapBytes = [int64]$proofEnd.gpuResidency.memory.androidProcessTotalSwapBytes
        }
    }
    if ($MinimumExpectedCores -gt 0 -and
        $averageUsedCores -lt $MinimumExpectedCores -and
        $CaptureUnexpectedScreen) {
        $unexpectedScreen = Join-Path $output.FullName `
            'unexpected-low-workload.png'
        & adb -s $AdbTarget exec-out screencap -p > $unexpectedScreen
        if ($LASTEXITCODE -eq 0) {
            $summary.unexpectedScreen = $unexpectedScreen
            $summary.workloadWarning =
                "averageUsedCores below $MinimumExpectedCores"
        }
    } elseif ($MinimumExpectedCores -gt 0 -and
        $averageUsedCores -lt $MinimumExpectedCores) {
        $summary.workloadWarning =
            "averageUsedCores below $MinimumExpectedCores; screenshot capture disabled"
    }
    $summary | ConvertTo-Json -Depth 5 |
        Set-Content -Encoding utf8 $summaryJson
    $summary | ConvertTo-Json -Depth 5
} finally {
    Invoke-Adb -Arguments @('shell', 'rm', '-f', $metaRemote, $sfRemote) |
        Out-Null
    if ($CloseAfterMeasure) {
        Invoke-Adb -Arguments @('shell', 'am', 'force-stop', $package) |
            Out-Null
        $cleanupScript = if ($DeviceProfile -eq 'PocoF7Pro') {
            'preflight-poco-f7-pro-performance-test.ps1'
        } else {
            'preflight-vivo-performance-test.ps1'
        }
        & (Join-Path $PSScriptRoot $cleanupScript) -AdbTarget $AdbTarget -CleanupOnly
    }
}
