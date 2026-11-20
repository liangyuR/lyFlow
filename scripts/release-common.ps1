#Requires -Version 7.0
. (Join-Path $PSScriptRoot 'reviewed-engine-release.ps1')

function Assert-ReleaseExit {
    param([string]$Operation)
    if ($LASTEXITCODE -ne 0) { throw "$Operation failed (exit $LASTEXITCODE)" }
}

function Assert-ReleaseChildPath {
    param([string]$Path, [string]$Parent)
    $full = [IO.Path]::GetFullPath($Path)
    $base = [IO.Path]::GetFullPath($Parent).TrimEnd('\', '/')
    if (-not $full.StartsWith($base + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing path outside release workspace: $full (parent: $base)"
    }
    $cursor = $full
    while ($cursor -and $cursor.Length -ge $base.Length) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Release path contains a reparse point: $cursor" }
        }
        $cursor = Split-Path -Parent $cursor
    }
    return $full
}

function Get-ReleaseVersion {
    param([string]$Root)
    $version = (Get-Content -LiteralPath (Join-Path $Root 'package.json') -Raw | ConvertFrom-Json).version
    if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "Unsupported product version: $version" }
    foreach ($relative in @('app/package.json', 'packages/editor/package.json', 'packages/mcp/package.json', 'packages/visualization/package.json', 'examples/host-react/package.json', 'bridge/tauri.conf.json')) {
        $actual = (Get-Content -LiteralPath (Join-Path $Root $relative) -Raw | ConvertFrom-Json).version
        if ($actual -ne $version) { throw "Version mismatch in ${relative}: expected $version, got $actual" }
    }
    foreach ($relative in @('bridge/Cargo.toml', 'crates/lyflow-client/Cargo.toml')) {
        $source = Get-Content -LiteralPath (Join-Path $Root $relative) -Raw
        if ($source -notmatch '(?m)^version\s*=\s*"([^"\r\n]+)"' -or $Matches[1] -ne $version) { throw "Version mismatch in $relative" }
    }
    return $version
}
