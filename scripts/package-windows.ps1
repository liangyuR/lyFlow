#Requires -Version 7.0
[CmdletBinding()]
param(
    [string]$Archive,
    [string]$OutputDir,
    [string]$ExpectedTag,
    [string]$SourceRoot = (Split-Path -Parent $PSScriptRoot)
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'release-common.ps1')
$root = (Resolve-Path -LiteralPath $SourceRoot).Path
$version = Get-ReleaseVersion -Root $root
if ($ExpectedTag) {
    $head = git -C $root rev-parse HEAD
    Assert-ReleaseExit 'Read public tool revision'
    $tagRevision = git -C $root rev-parse --verify "refs/tags/$ExpectedTag^{commit}"
    Assert-ReleaseExit 'Resolve public release tag'
    $dirty = git -C $root status --porcelain
    Assert-ReleaseExit 'Check public release sources'
    if ($ExpectedTag -cne "v$version" -or $head -cne $tagRevision -or $dirty) { throw "Tool sources must be a clean checkout of v$version." }
}
$lockFile = Join-Path $root "packaging/engine-releases/v$version.json"
if (-not $OutputDir) { $OutputDir = Join-Path $root "build/public-tools-v$version" }
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDir)
if (Test-Path -LiteralPath $output) { throw "Choose a new output directory: $output" }
$parent = Split-Path -Parent $output
$stage = Assert-ReleaseChildPath -Parent $parent -Path (Join-Path $parent ('.lyflow-tools-' + [guid]::NewGuid().ToString('N')))
$assets = Join-Path $stage 'assets'
[void][IO.Directory]::CreateDirectory($assets)
$oldSdk = $env:LYFLOW_NATIVE_SDK
$oldCore = $env:LYFLOW_PREBUILT_CORE_DIR
$oldTarget = $env:CARGO_TARGET_DIR
Push-Location -LiteralPath $root
try {
    $sdk = & (Join-Path $PSScriptRoot 'restore-native-sdk.ps1') -LockFile $lockFile -Archive $Archive -Destination (Join-Path $stage 'sdk')
    $env:LYFLOW_NATIVE_SDK = $sdk
    $env:LYFLOW_PREBUILT_CORE_DIR = $null
    if (-not $env:CARGO_TARGET_DIR) { $env:CARGO_TARGET_DIR = Join-Path $root 'build/release-rust' }
    $env:CARGO_TARGET_DIR = [IO.Path]::GetFullPath($env:CARGO_TARGET_DIR)
    & cargo build --release --locked --manifest-path bridge/Cargo.toml --bin lyflow --no-default-features
    Assert-ReleaseExit 'Build Release CLI'
    $cli = Join-Path $sdk 'bin/lyflow.exe'
    [IO.File]::Copy((Join-Path $env:CARGO_TARGET_DIR 'release/lyflow.exe'), $cli, $false)
    $runtimeFile = Join-Path $sdk 'bin/lyflow-runtime.json'
    $runtime = Get-Content -LiteralPath $runtimeFile -Raw | ConvertFrom-Json
    $info = & $cli info | ConvertFrom-Json
    Assert-ReleaseExit 'Inspect packaged CLI'
    if ($info.version -cne $version -or $info.corePath -ine (Join-Path $sdk 'bin/lyflow_core.dll') -or
        -not $info.buildIdentity.EndsWith("-source-$($runtime.sourceRevision)")) { throw 'CLI did not load the locked engine from the SDK.' }
    & $cli manifest --check
    Assert-ReleaseExit 'Check packaged CLI manifest'
    $lines = @(& $cli run (Join-Path $sdk 'examples/p2-expression.lyflow.json') --summary)
    Assert-ReleaseExit 'Run packaged example'
    $result = $lines[-1] | ConvertFrom-Json
    if ($result.status -cne 'ok' -or $result.outputs.mean.state -cne 'value') { throw 'CLI did not return the example output.' }

    & node (Join-Path $root 'packages/editor/scripts/pack.mjs') --pack-destination $assets
    Assert-ReleaseExit 'Pack editor from public sources'
    [IO.File]::Move((Join-Path $assets "lyflow-editor-$version.tgz"), (Join-Path $assets "lyflow-editor-v$version.tgz"))
    [IO.Compression.ZipFile]::CreateFromDirectory($sdk, (Join-Path $assets "lyflow-sdk-win-x64-v$version.zip"))
    [IO.File]::Copy($runtimeFile, (Join-Path $assets 'lyflow-runtime.json'))
    [IO.Directory]::Move($assets, $output)
    Write-Host "Packaged public tools in $output"
} finally {
    Pop-Location
    $env:LYFLOW_NATIVE_SDK = $oldSdk
    $env:LYFLOW_PREBUILT_CORE_DIR = $oldCore
    $env:CARGO_TARGET_DIR = $oldTarget
    Remove-Item -LiteralPath $stage -Recurse -Force
}
