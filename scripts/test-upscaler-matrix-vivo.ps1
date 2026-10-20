[CmdletBinding()]
param(
    [string]$AdbTarget =
        'adb-10AG263F1B001HM-2MNzzP._adb-tls-connect._tcp',

    [string]$OutputDirectory = '',

    [ValidateRange(5, 30)]
    [int]$MeasureSeconds = 8
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path (Split-Path $PSScriptRoot -Parent) `
        '.codex-upscaler-matrix-20260828'
}

$startScript = Join-Path $PSScriptRoot 'start-bloodborne-gameplay-vivo.ps1'
$measureScript = Join-Path $PSScriptRoot 'measure-bloodborne-gameplay-vivo.ps1'
$output = New-Item -ItemType Directory -Force -Path $OutputDirectory
$modes = @(
    [pscustomobject]@{ Name = 'off'; Mode = 0; FsrMode = 'off' },
    [pscustomobject]@{ Name = 'bilinear'; Mode = 2; FsrMode = 'on' },
    [pscustomobject]@{ Name = 'sharp_bilinear'; Mode = 3; FsrMode = 'on' },
    [pscustomobject]@{ Name = 'bicubic'; Mode = 4; FsrMode = 'on' },
    [pscustomobject]@{ Name = 'fsr1'; Mode = 5; FsrMode = 'on' },
    [pscustomobject]@{ Name = 'sgsr1'; Mode = 6; FsrMode = 'on' },
    [pscustomobject]@{ Name = 'nis'; Mode = 7; FsrMode = 'on' },
    [pscustomobject]@{ Name = 'sgsr2'; Mode = 8; FsrMode = 'on' },
    [pscustomobject]@{ Name = 'arm_asr'; Mode = 9; FsrMode = 'on' }
)

$matrix = @()
foreach ($mode in $modes) {
    $modeDirectory = Join-Path $output.FullName $mode.Name
    New-Item -ItemType Directory -Force -Path $modeDirectory | Out-Null
    $routeLog = Join-Path $modeDirectory 'route.txt'
    Write-Output "UPSCALER_MATRIX_BEGIN name=$($mode.Name) mode=$($mode.Mode)"
    $route = @(& $startScript -AdbTarget $AdbTarget -TimeoutSeconds 120 `
        -RenderResolutionMode 7 -UpscaledOutputResolutionMode 4 `
        -UpscalerMode $mode.Mode -FsrMode $mode.FsrMode -DebugHud $false)
    if ($LASTEXITCODE -ne 0) {
        throw "Bloodborne route failed for $($mode.Name)"
    }
    $route | Set-Content -Encoding utf8 $routeLog
    Start-Sleep -Seconds 3
    & $measureScript -Phase ("upscaler-" + $mode.Name) `
        -OutputDirectory $modeDirectory -AdbTarget $AdbTarget `
        -MeasureSeconds $MeasureSeconds -CloseAfterMeasure $true | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Bloodborne measurement failed for $($mode.Name)"
    }
    $summary = Get-Content (Join-Path $modeDirectory 'summary.json') -Raw |
        ConvertFrom-Json
    $matrix += [pscustomobject]@{
        name = $mode.Name
        requestedMode = $summary.upscalerProof.requestedMode
        effectiveMode = $summary.upscalerProof.effectiveMode
        fallbackReason = $summary.upscalerProof.fallbackReason
        dispatchFramesPerSecond = $summary.upscalerProof.dispatchFramesPerSecond
        source = '{0}x{1}' -f $summary.upscalerProof.sourceWidth,
            $summary.upscalerProof.sourceHeight
        output = '{0}x{1}' -f $summary.upscalerProof.outputWidth,
            $summary.upscalerProof.outputHeight
        nativeTemporalInputs = $summary.upscalerProof.nativeTemporalInputs
        runtimePresentCallsPerSecond = $summary.runtimePresentCallsPerSecond
        sfFps = $summary.sfFps
        averageUsedCores = $summary.averageUsedCores
        tzShellEndC = $summary.tzShellEndC
    }
    $matrix | ConvertTo-Json -Depth 5 |
        Set-Content -Encoding utf8 (Join-Path $output.FullName 'matrix.json')
    Write-Output ("UPSCALER_MATRIX_END name={0} effective={1} dispatch={2}/s fps={3}" -f
        $mode.Name, $summary.upscalerProof.effectiveMode,
        $summary.upscalerProof.dispatchFramesPerSecond,
        $summary.runtimePresentCallsPerSecond)
}

$matrix | Format-Table -AutoSize
