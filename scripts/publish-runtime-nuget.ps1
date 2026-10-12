#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Tag,
    [switch]$PackageOnly,
    [string]$OutputDir,
    [string]$CredentialFile = 'C:\Users\11601\.ssh\github_nuget_auth.txt',
    [string]$NuGetPath
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'runtime-nuget-common.ps1')
if ($Tag -notmatch '^v((0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*))$') { throw 'Only formal vX.Y.Z releases can be published.' }
$version = $Matches[1]
$repository = 'liangyuR/lyFlow'
$feed = 'https://nuget.pkg.github.com/liangyuR/index.json'
$previousToken = $env:LYFLOW_NUGET_TOKEN
$token = $null
$headers = @{}
$downloadHeaders = @{}
$feedHeaders = @{}
$work = $null
$result = $null
try {
    $token = $env:LYFLOW_NUGET_TOKEN
    if (-not $token) { $token = $env:NUGET_AUTH_TOKEN }
    if (-not $token -and (Test-Path -LiteralPath $CredentialFile -PathType Leaf)) { $token = (Get-Content -LiteralPath $CredentialFile -Raw).Trim() }
    if (-not $PackageOnly -and -not $token) { throw 'Set LYFLOW_NUGET_TOKEN or NUGET_AUTH_TOKEN, or provide CredentialFile.' }
    $headers = @{ Accept = 'application/vnd.github+json'; 'X-GitHub-Api-Version' = '2022-11-28' }
    if ($env:GH_TOKEN) { $headers.Authorization = "Bearer $env:GH_TOKEN" }
    if (-not $PackageOnly) {
        $feedHeaders.Authorization = 'Basic ' + [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes("liangyuR:$token"))
        $service = Invoke-RestMethod -Uri $feed -Headers $feedHeaders
        $baseResources = @($service.resources | Where-Object { $_.'@type' -eq 'PackageBaseAddress/3.0.0' })
        if ($baseResources.Count -ne 1) { throw 'NuGet feed does not expose a unique package base address.' }
        $baseAddress = [uri]$baseResources[0].'@id'
        if ($baseAddress.Scheme -ne 'https' -or $baseAddress.Host -ine 'nuget.pkg.github.com' -or -not $baseAddress.AbsolutePath.StartsWith('/liangyuR/', [StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected NuGet package endpoint.' }
        $indexUrl = $baseAddress.AbsoluteUri.TrimEnd('/') + '/lyflow.runtime.win-x64/index.json'
        try {
            $existing = Invoke-RestMethod -Uri $indexUrl -Headers $feedHeaders
            if ($version -cin @($existing.versions)) { throw "NuGet package LyFlow.Runtime.win-x64 $version already exists; publication is forbidden." }
        } catch {
            if ($_.Exception.PSObject.Properties.Name -notcontains 'Response' -or $null -eq $_.Exception.Response -or [int]$_.Exception.Response.StatusCode -ne 404) { throw }
        }
    }
    $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$repository/releases/tags/$Tag" -Headers $headers
    if ($release.draft -or $release.prerelease -or -not $release.published_at -or $release.tag_name -cne $Tag) { throw 'The selected Release must already be published and cannot be draft or prerelease.' }
    $commit = Invoke-RestMethod -Uri "https://api.github.com/repos/$repository/commits/$Tag" -Headers $headers
    $revision = [string]$commit.sha
    if ($revision -notmatch '^[0-9a-f]{40}$') { throw 'Could not resolve the published release source revision.' }
    $name = "lyflow-runtime-win-x64-$Tag.zip"
    $assets = @($release.assets | Where-Object { $_.name -ceq $name -and $_.state -ceq 'uploaded' })
    if ($assets.Count -ne 1) { throw "Release must contain exactly one uploaded $name asset." }
    if (-not $OutputDir) { $OutputDir = Join-Path (Split-Path -Parent $PSScriptRoot) ('build/nuget-publish-' + [guid]::NewGuid().ToString('N')) }
    $OutputDir = [IO.Path]::GetFullPath($OutputDir)
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
    $work = Join-Path $OutputDir ('.publish-stage-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $work -Force | Out-Null
    $zip = Join-Path $work $name
    $downloadHeaders = $headers.Clone()
    $downloadHeaders.Accept = 'application/octet-stream'
    Invoke-WebRequest -Uri $assets[0].url -Headers $downloadHeaders -OutFile $zip
    if ((Get-Item -LiteralPath $zip).Length -ne $assets[0].size) { throw 'Release asset download is incomplete.' }
    $runtime = Join-Path $work 'runtime'
    Expand-RuntimeReleaseArchive -ArchivePath $zip -Destination $runtime
    & (Join-Path $PSScriptRoot 'package-runtime-nuget.ps1') -RuntimeDir $runtime -OutputDir $OutputDir -ExpectedVersion $version -ExpectedRevision $revision -NuGetPath $NuGetPath -PublishedAsset
    if (-not $PackageOnly) {
        $nuget = Get-RuntimeNuGetTool -ToolPath $NuGetPath
        $config = Join-Path $work 'nuget.config'
        @'
<?xml version="1.0" encoding="utf-8"?>
<configuration>
  <packageSources><clear /><add key="lyflow-runtime" value="https://nuget.pkg.github.com/liangyuR/index.json" /></packageSources>
  <packageSourceCredentials><lyflow-runtime><add key="Username" value="liangyuR" /><add key="ClearTextPassword" value="%LYFLOW_NUGET_TOKEN%" /></lyflow-runtime></packageSourceCredentials>
</configuration>
'@ | Set-Content -LiteralPath $config -Encoding utf8NoBOM
        $env:LYFLOW_NUGET_TOKEN = $token
        $package = Join-Path $OutputDir "LyFlow.Runtime.win-x64.$version.nupkg"
        $result = & $nuget push $package -Source $feed -ApiKey $token -ConfigFile $config -NonInteractive -Timeout 1800 2>&1
        $exitCode = $LASTEXITCODE
        $safeOutput = ($result | Out-String).Replace($token, '***')
        if ($exitCode -ne 0) { throw "NuGet publication failed; existing versions are never overwritten: $safeOutput" }
        Write-Host $safeOutput
    }
} finally {
    $env:LYFLOW_NUGET_TOKEN = $previousToken
    $headers.Clear()
    $downloadHeaders.Clear()
    $feedHeaders.Clear()
    $token = $null
    $result = $null
    if ($work) {
        $safe = Assert-ReleaseChildPath -Path $work -Parent $OutputDir
        if (Test-Path -LiteralPath $safe) { Remove-Item -LiteralPath $safe -Recurse -Force }
    }
}
