#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$global:LyFlowNuGetPublicationTestState = @{ requests = 0; scenario = 'duplicate' }
function Invoke-RestMethod {
    param([string]$Uri, [hashtable]$Headers)
    $global:LyFlowNuGetPublicationTestState.requests++
    if ($Uri -eq 'https://nuget.pkg.github.com/liangyuR/index.json') {
        $endpoint = if ($global:LyFlowNuGetPublicationTestState.scenario -eq 'foreign-endpoint') { 'https://example.com/credential-sink' } else { 'https://nuget.pkg.github.com/liangyuR/download/' }
        return @{ resources = @(@{ '@type' = 'PackageBaseAddress/3.0.0'; '@id' = $endpoint }) }
    }
    if ($Uri -eq 'https://nuget.pkg.github.com/liangyuR/download/lyflow.runtime.win-x64/index.json') { return @{ versions = @('1.1.0') } }
    if ($Uri -eq 'https://api.github.com/repos/liangyuR/lyFlow/releases/tags/v1.1.0') { return @{ draft = $true; prerelease = $false; published_at = $null; tag_name = 'v1.1.0' } }
    throw 'Unexpected network request in publication negative test.'
}
$previous = $env:LYFLOW_NUGET_TOKEN
$env:LYFLOW_NUGET_TOKEN = 'non-secret-test-token'
try {
    $tests = 0
    foreach ($case in @(
        @{ scenario = 'duplicate'; arguments = @{ Tag = 'v1.1.0' }; expected = '*already exists*'; requests = 2 },
        @{ scenario = 'foreign-endpoint'; arguments = @{ Tag = 'v1.1.0' }; expected = '*Unexpected NuGet package endpoint*'; requests = 1 },
        @{ scenario = 'draft'; arguments = @{ Tag = 'v1.1.0'; PackageOnly = $true }; expected = '*must already be published*'; requests = 1 },
        @{ scenario = 'prerelease'; arguments = @{ Tag = 'v1.1.0-preview.1' }; expected = '*Only formal*'; requests = 0 }
    )) {
        $global:LyFlowNuGetPublicationTestState.requests = 0
        $global:LyFlowNuGetPublicationTestState.scenario = $case.scenario
        $arguments = $case.arguments
        $message = ''
        try { & (Join-Path $root 'scripts/publish-runtime-nuget.ps1') @arguments } catch { $message = $_.Exception.Message }
        if ($message -notlike $case.expected -or $global:LyFlowNuGetPublicationTestState.requests -ne $case.requests) { throw "Publication rejection failed: $($case.scenario): $message" }
        if ($env:LYFLOW_NUGET_TOKEN -cne 'non-secret-test-token') { throw 'Publication did not restore the caller token environment.' }
        $tests++
    }
    Write-Host "Runtime NuGet publication: $tests negative scenarios passed without download or push."
} finally {
    $env:LYFLOW_NUGET_TOKEN = $previous
    Remove-Variable -Name LyFlowNuGetPublicationTestState -Scope Global
}
