#Requires -Version 7.0
. (Join-Path $PSScriptRoot 'release-common.ps1')

function Get-RuntimeNuGetTool {
    param([string]$ToolPath)
    $version = '6.14.0'
    if (-not $ToolPath) {
        $tools = Join-Path (Split-Path -Parent $PSScriptRoot) "build/release-tools/nuget/$version"
        New-Item -ItemType Directory -Path $tools -Force | Out-Null
        $ToolPath = Join-Path $tools 'nuget.exe'
        if (-not (Test-Path -LiteralPath $ToolPath -PathType Leaf)) {
            $pending = Join-Path $tools ([guid]::NewGuid().ToString('N') + '.download')
            try {
                Invoke-WebRequest -Uri "https://dist.nuget.org/win-x86-commandline/v$version/nuget.exe" -OutFile $pending
                Move-Item -LiteralPath $pending -Destination $ToolPath
            } finally { if (Test-Path -LiteralPath $pending) { Remove-Item -LiteralPath $pending -Force } }
        }
    }
    $ToolPath = (Resolve-Path -LiteralPath $ToolPath).Path
    $signature = Get-AuthenticodeSignature -LiteralPath $ToolPath
    if ($signature.Status -ne 'Valid' -or $null -eq $signature.SignerCertificate -or $signature.SignerCertificate.Subject -notmatch '(^|,\s*)O=Microsoft Corporation(,|$)') { throw 'NuGet CLI requires a valid Microsoft Authenticode signature.' }
    $output = & $ToolPath help -ForceEnglishOutput 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0 -or $output -notmatch "(?m)^NuGet Version: $([regex]::Escape($version))(?:\.|\s|$)") { throw "Expected official NuGet CLI $version." }
    return $ToolPath
}

function Expand-RuntimeReleaseArchive {
    param([string]$ArchivePath, [string]$Destination)
    $Destination = [IO.Path]::GetFullPath($Destination)
    $cursor = $Destination
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            if ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Runtime archive destination contains a reparse point.' }
        }
        $cursor = Split-Path -Parent $cursor
    }
    if ((Test-Path -LiteralPath $Destination) -and @(Get-ChildItem -LiteralPath $Destination -Force).Count) { throw 'Runtime archive destination must be empty.' }
    $archive = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($entry in $archive.Entries) {
            $name = $entry.FullName.Replace('\', '/')
            $type = ($entry.ExternalAttributes -shr 16) -band 0xf000
            if ($type -eq 0xa000 -or ($entry.ExternalAttributes -band 0x400)) { throw 'Runtime archive cannot contain links or reparse points.' }
            if (-not $name -or $name.StartsWith('/') -or [IO.Path]::IsPathRooted($name) -or $name.Contains(':') -or @($name.TrimEnd('/').Split('/') | Where-Object { $_ -in @('', '.', '..') }).Count -or -not $seen.Add($name.TrimEnd('/'))) { throw 'Unsafe or duplicate runtime archive path.' }
        }
    } finally { $archive.Dispose() }
    [IO.Compression.ZipFile]::ExtractToDirectory($ArchivePath, $Destination)
    # ZIP 的 DOS 时间使用本地时区；未来的 CMake 输入会让 Ninja 持续重新配置。
    $now = [DateTime]::UtcNow
    foreach ($item in Get-ChildItem -LiteralPath $Destination -Force -Recurse) {
        if ($item.LastWriteTimeUtc -gt $now) { $item.LastWriteTimeUtc = $now }
    }
}
