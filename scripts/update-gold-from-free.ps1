[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$funnelRoot = Join-Path $repoRoot 'funnel-arm'

function Invoke-Git {
    param(
        [Parameter(Mandatory = $true)][string]$Repository,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    $output = & git -C $Repository @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "git -C '$Repository' $($Arguments -join ' ') failed:`n$($output -join "`n")"
    }
    return $output
}

function Assert-Branch {
    param(
        [Parameter(Mandatory = $true)][string]$Repository,
        [Parameter(Mandatory = $true)][string]$Expected
    )

    $actual = (Invoke-Git -Repository $Repository -Arguments @('branch', '--show-current') |
        Select-Object -Last 1).ToString().Trim()
    if ($actual -ne $Expected) {
        throw "Expected '$Expected' in '$Repository', found '$actual'. Run scripts/switch-lsx4-edition.ps1 -Edition Gold first."
    }
}

function Assert-SourceTreeClean {
    param([Parameter(Mandatory = $true)][string]$Repository)

    & git -C $Repository diff --quiet --ignore-submodules=dirty
    if ($LASTEXITCODE -ne 0) {
        throw "Tracked source changes exist in '$Repository'. Commit them on Gold before updating."
    }
    & git -C $Repository diff --cached --quiet --ignore-submodules=dirty
    if ($LASTEXITCODE -ne 0) {
        throw "Staged source changes exist in '$Repository'. Commit them on Gold before updating."
    }
}

Assert-Branch -Repository $repoRoot -Expected 'gold/main'
Assert-Branch -Repository $funnelRoot -Expected 'gold/main'
Assert-SourceTreeClean -Repository $repoRoot
Assert-SourceTreeClean -Repository $funnelRoot

$freeRootBefore = (Invoke-Git -Repository $repoRoot -Arguments @('rev-parse', 'main') |
    Select-Object -Last 1).ToString().Trim()
$freeFunnelBefore = (Invoke-Git -Repository $funnelRoot -Arguments @('rev-parse', 'main') |
    Select-Object -Last 1).ToString().Trim()

# Funnel is merged first. Recording its resulting Gold commit in the parent before the LSX4
# merge lets Git recognize that the Gold gitlink descends from the FREE gitlink.
Invoke-Git -Repository $funnelRoot -Arguments @('merge', '--no-edit', 'main') | Out-Host
$goldFunnel = (Invoke-Git -Repository $funnelRoot -Arguments @('rev-parse', 'HEAD') |
    Select-Object -Last 1).ToString().Trim()
$recordedFunnelLine = (Invoke-Git -Repository $repoRoot -Arguments @('ls-tree', 'HEAD', 'funnel-arm') |
    Select-Object -Last 1).ToString().Trim()
$recordedFunnel = ($recordedFunnelLine -split '\s+')[2]
if ($recordedFunnel -ne $goldFunnel) {
    Invoke-Git -Repository $repoRoot -Arguments @('add', '--', 'funnel-arm') | Out-Null
    Invoke-Git -Repository $repoRoot -Arguments @('commit', '-m', 'gold: merge latest FREE Funnel state') | Out-Host
}

Invoke-Git -Repository $repoRoot -Arguments @('merge', '--no-edit', 'main') | Out-Host

& git -C $repoRoot merge-base --is-ancestor main gold/main
if ($LASTEXITCODE -ne 0) {
    throw 'Invariant failed: LSX4 gold/main does not contain local FREE main.'
}
& git -C $funnelRoot merge-base --is-ancestor main gold/main
if ($LASTEXITCODE -ne 0) {
    throw 'Invariant failed: Funnel gold/main does not contain local FREE main.'
}

$goldRootAfter = (Invoke-Git -Repository $repoRoot -Arguments @('rev-parse', 'gold/main') |
    Select-Object -Last 1).ToString().Trim()
$goldFunnelAfter = (Invoke-Git -Repository $funnelRoot -Arguments @('rev-parse', 'gold/main') |
    Select-Object -Last 1).ToString().Trim()
Write-Host 'Local-only FREE -> GOLD update complete. No fetch or push was performed.'
Write-Host "FREE LSX4:   $freeRootBefore"
Write-Host "GOLD LSX4:   $goldRootAfter"
Write-Host "FREE Funnel: $freeFunnelBefore"
Write-Host "GOLD Funnel: $goldFunnelAfter"
