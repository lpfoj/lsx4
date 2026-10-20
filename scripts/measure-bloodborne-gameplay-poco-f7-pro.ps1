[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[a-zA-Z0-9_-]+$')]
    [string]$Phase,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [string]$AdbTarget = '65d16af6',

    [ValidateRange(5, 60)]
    [int]$MeasureSeconds = 12,

    [ValidateRange(0, 8)]
    [double]$MinimumExpectedCores = 2.0,

    [bool]$CloseAfterMeasure = $true,

    # A full-resolution screencap allocates and reads back the display image.
    # Keep it outside POCO performance windows unless explicitly requested.
    [bool]$CaptureUnexpectedScreen = $false
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Keep the proof window implementation shared, but select the POCO-specific
# prime CPU policy (policy7), thermal sensor (quiet_therm), and cleanup
# preflight explicitly.  The Vivo entry point retains its original defaults.
& (Join-Path $PSScriptRoot 'measure-bloodborne-gameplay-vivo.ps1') `
    -Phase $Phase `
    -OutputDirectory $OutputDirectory `
    -AdbTarget $AdbTarget `
    -MeasureSeconds $MeasureSeconds `
    -MinimumExpectedCores $MinimumExpectedCores `
    -CloseAfterMeasure $CloseAfterMeasure `
    -CaptureUnexpectedScreen $CaptureUnexpectedScreen `
    -DeviceProfile PocoF7Pro
