#Requires -Version 7.0
[CmdletBinding()]
param(
    [string]$LockFile,
    [Parameter(Mandatory)][string]$Destination,
    [string]$Archive
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'runtime-nuget-common.ps1')

if (!$LockFile) {
    $root = Split-Path -Parent $PSScriptRoot
    $version = (Get-Content (Join-Path $root 'package.json') -Raw | ConvertFrom-Json).lyflowEngineVersion
    $LockFile = Join-Path $root "packaging/engine-releases/v$version.json"
}
$lock = Get-Content -LiteralPath $LockFile -Raw | ConvertFrom-Json
if ($lock.schemaVersion -ne 1) { throw 'Unsupported native SDK lock version.' }
$destinationPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Destination)
if (Test-Path -LiteralPath $destinationPath) { throw "SDK destination already exists: $destinationPath" }
$parent = Split-Path -Parent $destinationPath
$temporary = Assert-ReleaseChildPath -Parent $parent -Path (Join-Path $parent ('.lyflow-sdk-' + [guid]::NewGuid().ToString('N')))
[IO.Directory]::CreateDirectory($temporary) | Out-Null
try {
    if ($Archive) {
        $zip = (Resolve-Path -LiteralPath $Archive).Path
    } else {
        $zip = Join-Path $temporary 'sdk.zip'
        Invoke-WebRequest -Uri $lock.url -OutFile $zip
    }
    $sdk = Join-Path $temporary 'sdk'
    $stream = [IO.File]::OpenRead($zip)
    try {
        $digest = (Get-FileHash -InputStream $stream -Algorithm SHA256).Hash
        if ($digest -ne $lock.sha256) { throw 'Native SDK archive SHA-256 differs from the lock.' }
        Expand-RuntimeReleaseArchive -ArchivePath $zip -Destination $sdk
    } finally { $stream.Dispose() }
    $metadata = Get-Content -LiteralPath (Join-Path $sdk 'bin/lyflow-runtime.json') -Raw | ConvertFrom-Json
    if ($metadata.localValidationOnly -or
        $metadata.engineVersion -cne $lock.engineVersion -or
        $metadata.sourceRevision -cne $lock.sourceRevision) {
        throw 'Native SDK runtime identity differs from the lock or is for local validation only.'
    }
    [IO.Directory]::Move($sdk, $destinationPath)
    $destinationPath
} finally {
    Remove-Item -LiteralPath $temporary -Recurse -Force
}
