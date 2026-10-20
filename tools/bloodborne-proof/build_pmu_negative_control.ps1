[CmdletBinding()]
param(
    [string]$NdkRoot = "$env:LOCALAPPDATA\Android\Sdk\ndk\29.0.14206865",
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$scriptDirectory = Split-Path $PSCommandPath -Parent
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $scriptDirectory 'out'
}
$clang = Join-Path $NdkRoot 'toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android26-clang.cmd'
if (-not (Test-Path -LiteralPath $clang -PathType Leaf)) {
    throw "Android NDK clang not found: $clang"
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$source = Join-Path $scriptDirectory 'pmu_negative_control.c'
$output = Join-Path $OutputDirectory 'lsx4-pmu-negative-control'
& $clang -O2 -fno-unroll-loops -fno-vectorize -fno-slp-vectorize `
    -Wall -Wextra -Werror $source -o $output
if ($LASTEXITCODE -ne 0) {
    throw "NDK clang failed with exit code $LASTEXITCODE"
}
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $output).Hash.ToLowerInvariant()
Write-Output "binary=$output"
Write-Output "sha256=$hash"
