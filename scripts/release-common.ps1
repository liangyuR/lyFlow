#Requires -Version 7.0
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

function Reset-ReleaseDirectory {
    param([string]$Path, [string]$Parent)
    $full = Assert-ReleaseChildPath -Path $Path -Parent $Parent
    if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full -Recurse -Force }
    New-Item -ItemType Directory -Path $full -Force | Out-Null
    return $full
}

function Get-ReleaseVisualStudio {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio C++ tools are required.' }
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    Assert-ReleaseExit 'Locate Visual Studio'
    if (-not $installation) { throw 'Visual Studio x64 C++ tools were not found.' }
    return [string]$installation
}

function Get-ReleaseVersion {
    param([string]$Root)
    $version = (Get-Content -LiteralPath (Join-Path $Root 'package.json') -Raw | ConvertFrom-Json).version
    if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "Unsupported product version: $version" }
    foreach ($relative in @('app/package.json', 'packages/editor/package.json', 'packages/mcp/package.json', 'examples/host-react/package.json', 'bridge/tauri.conf.json')) {
        $actual = (Get-Content -LiteralPath (Join-Path $Root $relative) -Raw | ConvertFrom-Json).version
        if ($actual -ne $version) { throw "Version mismatch in ${relative}: expected $version, got $actual" }
    }
    foreach ($relative in @('bridge/Cargo.toml', 'crates/lyflow-client/Cargo.toml')) {
        $source = Get-Content -LiteralPath (Join-Path $Root $relative) -Raw
        if ($source -notmatch '(?m)^version\s*=\s*"([^"\r\n]+)"' -or $Matches[1] -ne $version) { throw "Version mismatch in $relative" }
    }
    $header = Get-Content -LiteralPath (Join-Path $Root 'core/include/lyflow/version.h') -Raw
    if ($header -notmatch '#define\s+LYFLOW_VERSION\s+"([^"\r\n]+)"' -or $Matches[1] -ne $version) { throw 'Version mismatch in core version header' }
    $cmake = Get-Content -LiteralPath (Join-Path $Root 'core/CMakeLists.txt') -Raw
    if ($cmake -notmatch 'project\(lyflow_core\s+LANGUAGES\s+CXX\s+VERSION\s+([\d.]+)\)' -or $Matches[1] -ne $version) { throw 'Version mismatch in core CMake project' }
    return $version
}

function Write-ReleaseJson {
    param([object]$Value, [string]$Path)
    $Value | ConvertTo-Json -Depth 50 | Set-Content -LiteralPath $Path -Encoding utf8NoBOM
}
