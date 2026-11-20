#Requires -Version 7.0
param([string]$ReviewedAssetsDir)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $root 'scripts/release-common.ps1')
$reviewed = Get-ReviewedEngineRelease -Root $root
$version = $reviewed.Version
$tag = "v$version"
$global:LyFlowNuGetPublicationTestState = @{ downloads = 0; scenario = 'duplicate'; reviewed = $reviewed; dir = $ReviewedAssetsDir; patchTag = 'v' + ([version]::new(([version]$version).Major, ([version]$version).Minor, ([version]$version).Build + 1)) }
function Invoke-RestMethod {
    param([string]$Uri, [hashtable]$Headers)
    if ($Uri -eq 'https://nuget.pkg.github.com/liangyuR/index.json') {
        $endpoint = if ($global:LyFlowNuGetPublicationTestState.scenario -eq 'foreign-endpoint') { 'https://example.com/credential-sink' } else { 'https://nuget.pkg.github.com/liangyuR/download/' }
        return @{ resources = @(@{ '@type' = 'PackageBaseAddress/3.0.0'; '@id' = $endpoint }) }
    }
    if ($Uri -eq 'https://nuget.pkg.github.com/liangyuR/download/lyflow.runtime.win-x64/index.json') { return @{ versions = @($global:LyFlowNuGetPublicationTestState.reviewed.Version) } }
    $state = $global:LyFlowNuGetPublicationTestState
    $selectedTag = 'v' + $state.reviewed.Version
    $toolTag = if ($state.scenario -eq 'reuse') { $state.patchTag } else { $selectedTag }
    if ($Uri -ceq "https://api.github.com/repos/liangyuR/lyFlow/contents/package.json?ref=$toolTag") {
        return [pscustomobject]@{ version = $toolTag.Substring(1); lyflowEngineVersion = $state.reviewed.Version }
    }
    if ($Uri -ceq "https://api.github.com/repos/liangyuR/lyFlow/releases/tags/$toolTag" -and $Headers.ContainsKey('X-GitHub-Api-Version')) {
        return @{ draft = $state.scenario -eq 'draft'; prerelease = $false; published_at = 'published'; tag_name = $toolTag }
    }
    $engineTag = ([uri]$state.reviewed.Sdk.url).Segments[-2].TrimEnd('/')
    if ($Uri -ceq "https://api.github.com/repos/liangyuR/lyFlow/releases/tags/$engineTag") {
        $assets = foreach ($name in $state.reviewed.Assets.Keys) {
            @{ name = $name; state = 'uploaded'; browser_download_url = $state.reviewed.Assets[$name].url }
        }
        if ($state.scenario -eq 'engine-url') { $assets[0].browser_download_url = 'https://example.com/wrong' }
        if ($state.scenario -eq 'engine-api') { throw 'Engine release metadata unavailable' }
        return @{ draft = $false; prerelease = $state.scenario -eq 'engine-prerelease'; published_at = 'published'; tag_name = $engineTag; assets = $assets }
    }
    throw 'Unexpected network request in publication negative test.'
}
function Invoke-WebRequest {
    param([string]$Uri, [string]$OutFile, [hashtable]$Headers)
    $state = $global:LyFlowNuGetPublicationTestState
    $state.downloads++
    $name = ([uri]$Uri).Segments[-1]
    if ($state.scenario -eq 'wrong-bytes') { [IO.File]::WriteAllText($OutFile, 'changed'); return }
    Copy-Item -LiteralPath (Join-Path $state.dir $name) -Destination $OutFile
}
$previous = $env:LYFLOW_NUGET_TOKEN
$env:LYFLOW_NUGET_TOKEN = 'non-secret-test-token'
try {
    $scratch = [IO.Directory]::CreateTempSubdirectory('lyflow-lock-test-').FullName
    try {
        $pin = [version]$version
        $other = [version]::new($pin.Major, $pin.Minor, $pin.Build + 1).ToString()
        [void][IO.Directory]::CreateDirectory((Join-Path $scratch 'packaging/engine-releases'))
        @{ lyflowEngineVersion = $version } | ConvertTo-Json | Set-Content (Join-Path $scratch 'package.json')
        @{ schemaVersion = 1; engineVersion = $other } | ConvertTo-Json | Set-Content (Join-Path $scratch "packaging/engine-releases/v$version.json")
        $message = ''
        try { $null = Get-ReviewedEngineRelease -Root $scratch } catch { $message = $_.Exception.Message }
        if ($message -notlike '*Unexpected engine lock*') { throw 'A lock from a different engine version was accepted.' }
    } finally {
        $safe = Assert-ReleaseChildPath -Path $scratch -Parent ([IO.Path]::GetTempPath())
        Remove-Item -LiteralPath $safe -Recurse -Force
    }
    $tests = 1
    foreach ($case in @(
        @{ scenario = 'duplicate'; arguments = @{ Tag = $tag }; expected = '*already exists*' },
        @{ scenario = 'foreign-endpoint'; arguments = @{ Tag = $tag }; expected = '*Unexpected NuGet package endpoint*' },
        @{ scenario = 'draft'; arguments = @{ Tag = $tag; PackageOnly = $true }; expected = '*must already be published*' },
        @{ scenario = 'engine-prerelease'; arguments = @{ Tag = $tag; PackageOnly = $true }; expected = '*cannot be draft or prerelease*' },
        @{ scenario = 'engine-url'; arguments = @{ Tag = $tag; PackageOnly = $true }; expected = '*missing the locked public download*' },
        @{ scenario = 'engine-api'; arguments = @{ Tag = $tag; PackageOnly = $true }; expected = '*metadata unavailable*' },
        @{ scenario = 'wrong-bytes'; arguments = @{ Tag = $tag; PackageOnly = $true }; expected = '*reviewed SHA-256*' },
        @{ scenario = 'local-push'; arguments = @{ Tag = $tag; ReviewedAssetsDir = 'local' }; expected = '*require -PackageOnly*' },
        @{ scenario = 'prerelease'; arguments = @{ Tag = ("$tag-preview.1") }; expected = '*Only formal*' }
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
        $state.scenario = 'reuse'
        $state.downloads = 0
        & (Join-Path $root 'scripts/publish-runtime-nuget.ps1') -Tag $state.patchTag -OutputDir $output
        if ($state.downloads -ne 1) { throw 'Tool-only publication must verify and reuse the existing feed package.' }
    }
} finally {
    $env:LYFLOW_NUGET_TOKEN = $previous
    Remove-Variable -Name LyFlowNuGetPublicationTestState -Scope Global
}
