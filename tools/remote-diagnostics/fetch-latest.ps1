param(
    [string]$OutputDirectory = "artifacts/lsx4-remote-diagnostics"
)

$remote = ssh -o BatchMode=yes unixvps `
    'find /srv/lsx4-diagnostics -mindepth 2 -maxdepth 2 -type f -name "*.zip" -printf "%T@ %p\n" | sort -nr | head -1 | cut -d" " -f2-'
if (-not $remote) {
    throw "No LSX4 diagnostics reports are available"
}

$destination = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force $destination | Out-Null
$archive = Join-Path $destination ([System.IO.Path]::GetFileName($remote.Trim()))
scp "unixvps:$($remote.Trim())" $archive
$expanded = [System.IO.Path]::Combine(
    $destination, [System.IO.Path]::GetFileNameWithoutExtension($archive))
Expand-Archive -LiteralPath $archive -DestinationPath $expanded -Force
Write-Output $expanded
