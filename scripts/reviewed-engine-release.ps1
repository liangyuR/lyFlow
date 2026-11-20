#Requires -Version 7.0
function Get-ReviewedEngineRelease {
    param([string]$Version)
    $path = Join-Path $PSScriptRoot "../packaging/engine-releases/v$Version.json"
    $lock = Get-Content -LiteralPath $path -Raw -ErrorAction Stop | ConvertFrom-Json
    if ($lock.schemaVersion -ne 1 -or $lock.engineVersion -cne $Version) { throw "Unexpected engine lock for v$Version." }
    $assets = [ordered]@{
        "lyflow-native-sdk-win-x64-v$Version.zip" = $lock
        "lyflow-runtime-win-x64-v$Version.zip" = $lock.runtime
        "LyFlow.Runtime.win-x64.$Version.nupkg" = $lock.nuget
    }
    [pscustomobject]@{ Version = $Version; Revision = $lock.sourceRevision; Repository = $lock.sourceRepository; Assets = $assets }
}

function Assert-ReviewedEngineAssets {
    param($Release, [string]$Directory, [string[]]$Names = $Release.Assets.Keys)
    foreach ($name in $Names) {
        $file = Join-Path $Directory $name
        if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ine $Release.Assets[$name].sha256) {
            throw "Downloaded $name differs from the reviewed SHA-256."
        }
    }
}
