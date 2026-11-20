#Requires -Version 7.0
$ErrorActionPreference = 'Stop'

if ('--no-core-watch' -notin $args -or -not $env:LYFLOW_PREBUILT_CORE_DIR) {
    $buildDir = Join-Path (Split-Path -Parent $PSScriptRoot) 'build/core'
    & (Join-Path $PSScriptRoot 'build-core.ps1') -NoTests -BuildDir $buildDir
    $env:LYFLOW_PREBUILT_CORE_DIR = Join-Path $buildDir 'bin'
}

& node (Join-Path $PSScriptRoot 'dev.mjs') @args
exit $LASTEXITCODE
