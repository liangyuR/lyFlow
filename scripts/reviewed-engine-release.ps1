#Requires -Version 7.0
function Get-ReviewedEngineRelease {
    param([string]$Version, [string]$Root = (Split-Path -Parent $PSScriptRoot), [string]$LockFile)
    if (-not $LockFile) {
        if (-not $Version) { $Version = (Get-Content (Join-Path $Root 'package.json') -Raw | ConvertFrom-Json).lyflowEngineVersion }
        if ($Version -notmatch '^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$') { throw 'Set lyflowEngineVersion to a fixed engine version.' }
        $LockFile = Join-Path $Root "packaging/engine-releases/v$Version.json"
    }
    $path = (Resolve-Path -LiteralPath $LockFile -ErrorAction Stop).Path
    $lock = Get-Content -LiteralPath $path -Raw -ErrorAction Stop | ConvertFrom-Json
    if ($lock.schemaVersion -ne 1 -or ($Version -and $lock.engineVersion -cne $Version)) { throw "Unexpected engine lock for v$Version." }
    $Version = $lock.engineVersion
    $assets = [ordered]@{
        "lyflow-native-sdk-win-x64-v$Version.zip" = $lock
        "lyflow-runtime-win-x64-v$Version.zip" = $lock.runtime
        "LyFlow.Runtime.win-x64.$Version.nupkg" = $lock.nuget
    }
    [pscustomobject]@{ Version = $Version; Path = $path; Sdk = $lock; Revision = $lock.sourceRevision; Repository = $lock.sourceRepository; Assets = $assets }
}

function Get-PublishedEngineRelease {
    param($Release)
    if ($Release.Sdk.url -cnotmatch '^https://github\.com/liangyuR/lyFlow/releases/download/([^/]+)/[^/]+$') { throw 'Expected a public LyFlow Release URL in the engine lock.' }
    $tag = $Matches[1]
    if ($tag -cnotin @("v$($Release.Version)", "engine-assets-v$($Release.Version)")) { throw "Engine asset tag $tag does not match the lock." }
    $published = Invoke-RestMethod -Uri "https://api.github.com/repos/liangyuR/lyFlow/releases/tags/$tag" -ErrorAction Stop
    if ($published.tag_name -cne $tag -or $published.draft -or $published.prerelease -or -not $published.published_at) {
        throw "Engine Release $tag must be published and cannot be draft or prerelease."
    }
    foreach ($name in $Release.Assets.Keys) {
        $matchesAsset = @($published.assets | Where-Object { $_.name -ceq $name -and $_.state -ceq 'uploaded' -and $_.browser_download_url -ceq $Release.Assets[$name].url })
        if ($matchesAsset.Count -ne 1) { throw "Engine Release $tag is missing the locked public download for $name." }
    }
    return $published
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
