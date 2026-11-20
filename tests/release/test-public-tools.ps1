#Requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory)][string]$AssetsDir, [string]$Workspace)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $root 'scripts/runtime-nuget-common.ps1')
$version = Get-ReleaseVersion -Root $root
$assets = (Resolve-Path -LiteralPath $AssetsDir).Path
if (-not $Workspace) { $Workspace = Join-Path ([IO.Path]::GetTempPath()) ('lyflow-tools-test-' + [guid]::NewGuid().ToString('N')) }
$work = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Workspace)
if (Test-Path $work) { throw "Choose a new test workspace: $work" }
[void][IO.Directory]::CreateDirectory($work)
Write-Host "Public tool consumption tests: $work"
$sdk = Join-Path $work 'sdk'
Expand-RuntimeReleaseArchive -ArchivePath (Join-Path $assets "lyflow-sdk-win-x64-v$version.zip") -Destination $sdk
$cli = Join-Path $sdk 'bin/lyflow.exe'
$examples = Join-Path $sdk 'examples'
$info = & $cli info | ConvertFrom-Json
Assert-ReleaseExit 'Inspect installed CLI'
$lock = (Get-ReviewedEngineRelease -Root $root).Sdk
if ($info.corePath -ine (Join-Path $sdk 'bin/lyflow_core.dll') -or $info.version -cne $lock.engineVersion -or
    -not $info.buildIdentity.EndsWith("-source-$($lock.sourceRevision)")) { throw 'Installed SDK loaded a different engine.' }

$modelDir = $env:LYFLOW_P4_ASSETS
if (-not $modelDir) {
    $modelDir = Join-Path $work 'p4-assets'
    & python -B (Join-Path $sdk 'scripts/fetch-p4-model-assets.py') --output-dir $modelDir
    Assert-ReleaseExit 'Restore P4 test models'
}
& python -B (Join-Path $PSScriptRoot 'p4_contract.py') --cli $cli --examples $examples --assets $modelDir --output-dir (Join-Path $work 'p4')
Assert-ReleaseExit 'Installed P4 CLI contract'
& python -B (Join-Path $PSScriptRoot 'p5_contract.py') --cli $cli --examples $examples --output-dir (Join-Path $work 'p5')
Assert-ReleaseExit 'Installed P5 CLI contract'
$consumerBuild = Join-Path $work 'cpp-host'
& cmake -S (Join-Path $PSScriptRoot 'p5_sdk_consumer') -B $consumerBuild -G Ninja -DCMAKE_BUILD_TYPE=Release "-Dlyflow_DIR=$sdk" "-DLYFLOW_CONSUMER_VERSION=$($lock.engineVersion)"
Assert-ReleaseExit 'Configure external C++17 host for the exact SDK version'
& cmake --build $consumerBuild
Assert-ReleaseExit 'Build external C++17 host'
& python -B (Join-Path $PSScriptRoot 'p5_sdk_contract.py') --sdk $sdk --consumer (Join-Path $consumerBuild 'p5_sdk_consumer.exe') --output-dir (Join-Path $work 'p5-cpp')
Assert-ReleaseExit 'Installed C++17 graphs and DLL dependency closure'
& python -B (Join-Path $PSScriptRoot 'scene_contract.py') --cli $cli --examples $examples --output-dir (Join-Path $work 'scenes')
Assert-ReleaseExit 'Installed scene contracts'
& python -B (Join-Path $root 'bridge/tests/p6_delivery_contract.py') --cli $cli --core (Join-Path $sdk 'bin/lyflow_core.dll') --examples $examples --output-dir (Join-Path $work 'p6')
Assert-ReleaseExit 'Installed frame delivery contracts'
& python -B (Join-Path $PSScriptRoot 'profile_workflow_contract.py') --sdk $sdk --cli $cli --output-dir (Join-Path $work 'profile')
Assert-ReleaseExit 'External Profile workflow consumer'

$hostDir = Join-Path $work 'host'
[void][IO.Directory]::CreateDirectory($hostDir)
$sample = Join-Path $root 'examples/host-react'
foreach ($relative in @('src', 'index.html', 'tsconfig.json', 'vite.config.ts', 'package.json')) {
    Copy-Item -LiteralPath (Join-Path $sample $relative) -Destination $hostDir -Recurse
}
$manifestPath = Join-Path $hostDir 'package.json'
$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json -AsHashtable
$manifest.dependencies['@lyflow/editor'] = 'file:' + (Join-Path $assets "lyflow-editor-v$version.tgz").Replace('\', '/')
$manifest | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $manifestPath -Encoding utf8NoBOM
$names = @('LYFLOW_PACKAGED_HOST','LYFLOW_CLI','LYFLOW_HTTP_PORT','LYFLOW_HTTP_TOKEN','VITE_LYFLOW_API','VITE_LYFLOW_TOKEN','LYFLOW_E2E_HEADLESS')
$previous = @{}
foreach ($name in $names) { $previous[$name] = [Environment]::GetEnvironmentVariable($name) }
Push-Location -LiteralPath $hostDir
try {
    $env:LYFLOW_PACKAGED_HOST = $hostDir
    $env:LYFLOW_CLI = $cli
    $env:LYFLOW_E2E_HEADLESS = '1'
    if (-not $env:LYFLOW_HTTP_PORT) { $env:LYFLOW_HTTP_PORT = '8788' }
    $env:VITE_LYFLOW_API = "http://127.0.0.1:$env:LYFLOW_HTTP_PORT"
    $env:LYFLOW_HTTP_TOKEN = [guid]::NewGuid().ToString('N')
    $env:VITE_LYFLOW_TOKEN = $env:LYFLOW_HTTP_TOKEN
    & npm install --ignore-scripts --no-audit --no-fund
    Assert-ReleaseExit 'Install editor tarball into the external React host'
    & npm run build
    Assert-ReleaseExit 'Compile external React host'
    & node (Join-Path $root 'scripts/e2e/http.mjs')
    Assert-ReleaseExit 'Open and run installed graph from the external React host'
} finally {
    Pop-Location
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
}
