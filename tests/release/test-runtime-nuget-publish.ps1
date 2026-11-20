#Requires -Version 7.0
param([string]$ReviewedAssetsDir)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $root 'scripts/reviewed-engine-release.ps1')
$version = if ($ReviewedAssetsDir) {
    (Get-Content -LiteralPath (Join-Path $ReviewedAssetsDir 'lyflow-runtime.json') -Raw | ConvertFrom-Json).engineVersion
} else {
    (Get-ChildItem (Join-Path $root 'packaging/engine-releases') -Filter 'v*.json' |
        Sort-Object { [version]$_.BaseName.Substring(1) } | Select-Object -Last 1).BaseName.Substring(1)
}
$reviewed = Get-ReviewedEngineRelease -Version $version
$tag = "v$version"
$global:LyFlowNuGetPublicationTestState = @{ downloads = 0; scenario = 'duplicate'; reviewed = $reviewed; dir = $ReviewedAssetsDir }
function Invoke-RestMethod {
    param([string]$Uri, [hashtable]$Headers)
    if ($Uri -eq 'https://nuget.pkg.github.com/liangyuR/index.json') {
        $endpoint = if ($global:LyFlowNuGetPublicationTestState.scenario -eq 'foreign-endpoint') { 'https://example.com/credential-sink' } else { 'https://nuget.pkg.github.com/liangyuR/download/' }
        return @{ resources = @(@{ '@type' = 'PackageBaseAddress/3.0.0'; '@id' = $endpoint }) }
    }
    if ($Uri -eq 'https://nuget.pkg.github.com/liangyuR/download/lyflow.runtime.win-x64/index.json') { return @{ versions = @('1.1.0') } }
    if ($Uri -eq 'https://api.github.com/repos/liangyuR/lyFlow/releases/tags/v1.1.0') { return @{ draft = $true; prerelease = $false; published_at = $null; tag_name = 'v1.1.0' } }
    $state = $global:LyFlowNuGetPublicationTestState
    $selectedTag = 'v' + $state.reviewed.Version
    if ($Uri -ceq "https://api.github.com/repos/liangyuR/lyFlow/releases/tags/$selectedTag") {
        return @{ draft = $false; prerelease = $false; published_at = 'published'; tag_name = $selectedTag }
    }
    throw 'Unexpected network request in publication negative test.'
}
function Invoke-WebRequest {
    param([string]$Uri, [string]$OutFile)
    $state = $global:LyFlowNuGetPublicationTestState
    $state.downloads++
    $name = ([uri]$Uri).Segments[-1]
    if ($state.scenario -eq 'wrong-bytes') { [IO.File]::WriteAllText($OutFile, 'changed'); return }
    Copy-Item -LiteralPath (Join-Path $state.dir $name) -Destination $OutFile
}
$previous = $env:LYFLOW_NUGET_TOKEN
$env:LYFLOW_NUGET_TOKEN = 'non-secret-test-token'
try {
    $tests = 0
    foreach ($case in @(
        @{ scenario = 'duplicate'; arguments = @{ Tag = 'v1.1.0' }; expected = '*already exists*' },
        @{ scenario = 'foreign-endpoint'; arguments = @{ Tag = 'v1.1.0' }; expected = '*Unexpected NuGet package endpoint*' },
        @{ scenario = 'draft'; arguments = @{ Tag = 'v1.1.0'; PackageOnly = $true }; expected = '*must already be published*' },
        @{ scenario = 'wrong-bytes'; arguments = @{ Tag = $tag; PackageOnly = $true }; expected = '*reviewed SHA-256*' },
        @{ scenario = 'local-push'; arguments = @{ Tag = $tag; ReviewedAssetsDir = 'local' }; expected = '*require -PackageOnly*' },
        @{ scenario = 'prerelease'; arguments = @{ Tag = 'v1.1.0-preview.1' }; expected = '*Only formal*' }
    )) {
        $global:LyFlowNuGetPublicationTestState.scenario = $case.scenario
        $arguments = $case.arguments
        $message = ''
        try { & (Join-Path $root 'scripts/publish-runtime-nuget.ps1') @arguments } catch { $message = $_.Exception.Message }
        if ($message -notlike $case.expected) { throw "Publication rejection failed: $($case.scenario): $message" }
        if ($env:LYFLOW_NUGET_TOKEN -cne 'non-secret-test-token') { throw 'Publication did not restore the caller token environment.' }
        $tests++
    }
    Write-Host "Runtime NuGet publication: $tests rejection scenarios passed."
    if ($ReviewedAssetsDir) {
        $output = Join-Path $root ('build/nuget-reviewed-test-' + [guid]::NewGuid().ToString('N'))
        $state = $global:LyFlowNuGetPublicationTestState
        $state.scenario = 'original'
        $state.downloads = 0
        & (Join-Path $root 'scripts/publish-runtime-nuget.ps1') -Tag $tag -PackageOnly -OutputDir $output
        $name = "LyFlow.Runtime.win-x64.$version.nupkg"
        if ($state.downloads -ne 1 -or
            (Get-FileHash -LiteralPath (Join-Path $output $name)).Hash -ine $reviewed.Assets[$name].sha256) {
            throw 'Reviewed release did not preserve the original package.'
        }
    }
} finally {
    $env:LYFLOW_NUGET_TOKEN = $previous
    Remove-Variable -Name LyFlowNuGetPublicationTestState -Scope Global
}
