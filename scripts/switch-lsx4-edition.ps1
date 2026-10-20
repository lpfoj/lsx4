[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Free', 'Gold')]
    [string]$Edition
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$funnelRoot = Join-Path $repoRoot 'funnel-arm'
$targetBranch = if ($Edition -eq 'Free') { 'main' } else { 'gold/main' }

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

function Assert-SourceTreeClean {
    param([Parameter(Mandatory = $true)][string]$Repository)

    & git -C $Repository diff --quiet --ignore-submodules=dirty
    if ($LASTEXITCODE -ne 0) {
        throw "Tracked source changes exist in '$Repository'. Commit or move them before switching editions."
    }
    & git -C $Repository diff --cached --quiet --ignore-submodules=dirty
    if ($LASTEXITCODE -ne 0) {
        throw "Staged source changes exist in '$Repository'. Commit or move them before switching editions."
    }
}

Assert-SourceTreeClean -Repository $repoRoot
Assert-SourceTreeClean -Repository $funnelRoot

Invoke-Git -Repository $repoRoot -Arguments @('show-ref', '--verify', '--quiet', "refs/heads/$targetBranch") | Out-Null
Invoke-Git -Repository $funnelRoot -Arguments @('show-ref', '--verify', '--quiet', "refs/heads/$targetBranch") | Out-Null

# The parent is switched first. Git does not implicitly change the nested Funnel worktree,
# so the second switch selects the matching edition without fetching or contacting a remote.
Invoke-Git -Repository $repoRoot -Arguments @('switch', $targetBranch) | Out-Host
Invoke-Git -Repository $funnelRoot -Arguments @('switch', $targetBranch) | Out-Host

$rootHead = (Invoke-Git -Repository $repoRoot -Arguments @('rev-parse', '--short=12', 'HEAD') |
    Select-Object -Last 1).ToString().Trim()
$funnelHead = (Invoke-Git -Repository $funnelRoot -Arguments @('rev-parse', '--short=12', 'HEAD') |
    Select-Object -Last 1).ToString().Trim()
Write-Host "Edition: $Edition"
Write-Host "LSX4:   $targetBranch @ $rootHead"
Write-Host "Funnel: $targetBranch @ $funnelHead"
