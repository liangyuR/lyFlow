#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$RuntimeDir,
    [Parameter(Mandatory)][string]$OutputDir,
    [Parameter(Mandatory)][string]$ExpectedVersion,
    [Parameter(Mandatory)][string]$ExpectedRevision,
    [string]$NuGetPath,
    [switch]$PublishedAsset
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'runtime-nuget-common.ps1')
$RuntimeDir = (Resolve-Path -LiteralPath $RuntimeDir).Path
$metadata = Assert-RuntimeNuGetMetadata -RuntimeDir $RuntimeDir -ExpectedVersion $ExpectedVersion -ExpectedRevision $ExpectedRevision
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if ($OutputDir.Equals($RuntimeDir, [StringComparison]::OrdinalIgnoreCase) -or $OutputDir.StartsWith($RuntimeDir.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Output directory cannot overlap the input runtime.' }
$package = Join-Path $OutputDir "LyFlow.Runtime.win-x64.$ExpectedVersion.nupkg"
if (Test-Path -LiteralPath $package) { throw "Package already exists; use a fresh output directory: $package" }
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$stage = Join-Path $OutputDir ('.nuget-stage-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage -Force | Out-Null
try {
    $native = Join-Path $stage 'runtimes/win-x64/native'
    New-Item -ItemType Directory -Path $native -Force | Out-Null
    foreach ($file in Get-ChildItem -LiteralPath $RuntimeDir -Force) { Copy-Item -LiteralPath $file.FullName -Destination $native -Recurse }
    $report = Join-Path $OutputDir "LyFlow.Runtime.win-x64.$ExpectedVersion.inspection.json"
    if ($PublishedAsset) {
        & (Join-Path $PSScriptRoot 'inspect-published-runtime.ps1') -RuntimeDir $native -ReportPath $report -ExpectedVersion $ExpectedVersion -ExpectedRevision $ExpectedRevision
    } else {
        & (Join-Path $PSScriptRoot 'inspect-runtime.ps1') -RuntimeDir $native -ReportPath $report
    }
    $inspection = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
    if ($inspection.engineVersion -cne $ExpectedVersion -or $inspection.abiVersion -cne $ExpectedVersion -or $inspection.sourceRevision -cne $ExpectedRevision) { throw 'Actual DLL identity differs from the release metadata.' }
    foreach ($operator in $metadata.operators) {
        $actual = @($inspection.manifest.operators | Where-Object { $_.id -ceq $operator.id -and $_.version -ceq $operator.version })
        if ($actual.Count -ne 1) { throw "Actual DLL operator differs from metadata: $($operator.id)" }
    }
    if (@($inspection.manifest.operators).Count -ne @($metadata.operators).Count) { throw 'Actual DLL operator inventory differs from metadata.' }
    $nuspec = Join-Path $stage 'LyFlow.Runtime.win-x64.nuspec'
    @"
<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://schemas.microsoft.com/packaging/2013/05/nuspec.xsd">
  <metadata>
    <id>LyFlow.Runtime.win-x64</id>
    <version>$ExpectedVersion</version>
    <authors>liangyuR</authors>
    <requireLicenseAcceptance>false</requireLicenseAcceptance>
    <description>Formal LyFlow Windows x64 native runtime with dependency DLLs, version inventory and third-party notices. Project license is not declared; included notices do not grant additional rights.</description>
    <projectUrl>https://github.com/liangyuR/lyFlow</projectUrl>
    <repository type="git" url="https://github.com/liangyuR/lyFlow" commit="$ExpectedRevision" />
    <tags>native lyflow win-x64</tags>
  </metadata>
  <files><file src="runtimes\**\*" target="runtimes" /></files>
</package>
"@ | Set-Content -LiteralPath $nuspec -Encoding utf8NoBOM
    $nuget = Get-RuntimeNuGetTool -ToolPath $NuGetPath
    $packageOutput = Join-Path $stage 'packed'
    New-Item -ItemType Directory -Path $packageOutput -Force | Out-Null
    & $nuget pack $nuspec -BasePath $stage -OutputDirectory $packageOutput -NonInteractive -NoDefaultExcludes
    Assert-ReleaseExit 'Pack native runtime NuGet'
    $pending = Join-Path $packageOutput (Split-Path -Leaf $package)
    if (-not (Test-Path -LiteralPath $pending -PathType Leaf)) { throw 'NuGet did not create the expected package.' }
    $archive = [IO.Compression.ZipFile]::OpenRead($pending)
    try {
        $prefix = 'runtimes/win-x64/native/'
        $files = @($archive.Entries | Where-Object { $_.FullName.StartsWith($prefix, [StringComparison]::Ordinal) -and -not $_.FullName.EndsWith('/') })
        $expected = @($metadata.files | ForEach-Object { $_.path })
        $packed = @($files | ForEach-Object { $_.FullName.Substring($prefix.Length) })
        if (@(Compare-Object -ReferenceObject $expected -DifferenceObject $packed).Count -or $packed.Count -ne $expected.Count) { throw 'Packed NuGet file inventory differs from the validated runtime.' }
        foreach ($file in $files) {
            if ($file.Length -ne (Get-Item -LiteralPath (Join-Path $RuntimeDir $file.FullName.Substring($prefix.Length))).Length) { throw "Incomplete NuGet file: $($file.FullName)" }
        }
    } finally { $archive.Dispose() }
    [IO.File]::Copy($pending, $package, $false)
    Write-Host "Validated NuGet runtime: $package"
} finally {
    $safe = Assert-ReleaseChildPath -Path $stage -Parent $OutputDir
    if (Test-Path -LiteralPath $safe) { Remove-Item -LiteralPath $safe -Recurse -Force }
}
