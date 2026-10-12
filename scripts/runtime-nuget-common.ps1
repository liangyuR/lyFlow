#Requires -Version 7.0
. (Join-Path $PSScriptRoot 'release-common.ps1')

function Get-RuntimeNuGetTool {
    param([string]$ToolPath)
    $version = '6.14.0'
    if (-not $ToolPath) {
        $tools = Join-Path (Split-Path -Parent $PSScriptRoot) "build/release-tools/nuget/$version"
        New-Item -ItemType Directory -Path $tools -Force | Out-Null
        $ToolPath = Join-Path $tools 'nuget.exe'
        if (-not (Test-Path -LiteralPath $ToolPath -PathType Leaf)) {
            $pending = Join-Path $tools ([guid]::NewGuid().ToString('N') + '.download')
            try {
                Invoke-WebRequest -Uri "https://dist.nuget.org/win-x86-commandline/v$version/nuget.exe" -OutFile $pending
                Move-Item -LiteralPath $pending -Destination $ToolPath
            } finally { if (Test-Path -LiteralPath $pending) { Remove-Item -LiteralPath $pending -Force } }
        }
    }
    $ToolPath = (Resolve-Path -LiteralPath $ToolPath).Path
    $signature = Get-AuthenticodeSignature -LiteralPath $ToolPath
    if ($signature.Status -ne 'Valid' -or $null -eq $signature.SignerCertificate -or $signature.SignerCertificate.Subject -notmatch '(^|,\s*)O=Microsoft Corporation(,|$)') { throw 'NuGet CLI requires a valid Microsoft Authenticode signature.' }
    $output = & $ToolPath help -ForceEnglishOutput 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0 -or $output -notmatch "(?m)^NuGet Version: $([regex]::Escape($version))(?:\.|\s|$)") { throw "Expected official NuGet CLI $version." }
    return $ToolPath
}

function Assert-RuntimeNuGetMetadata {
    param([string]$RuntimeDir, [string]$ExpectedVersion, [string]$ExpectedRevision)
    if ($ExpectedVersion -notmatch '^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$') { throw 'A stable three-part product version is required.' }
    if ($ExpectedRevision -notmatch '^[0-9a-f]{40}$') { throw 'An exact source revision is required.' }
    $metadata = Get-Content -LiteralPath (Join-Path $RuntimeDir 'lyflow-runtime.json') -Raw | ConvertFrom-Json
    function Assert-NoContentDigests([object]$Value) {
        if ($null -eq $Value -or $Value -is [string] -or $Value.GetType().IsValueType) { return }
        if ($Value -is [System.Collections.IEnumerable]) { foreach ($child in $Value) { Assert-NoContentDigests $child }; return }
        foreach ($property in $Value.PSObject.Properties) {
            if ($property.Name -match '(?i)(hash|digest|fingerprint|checksum|^sha\d*$)') { throw 'Legacy content digest fields are forbidden in runtime metadata.' }
            Assert-NoContentDigests $property.Value
        }
    }
    Assert-NoContentDigests $metadata
    if ($metadata.schemaVersion -ne 1 -or $metadata.architecture -cne 'x64' -or $metadata.localValidationOnly -isnot [bool] -or $metadata.localValidationOnly) { throw 'Only formal x64 runtime metadata is accepted.' }
    if ($metadata.engineVersion -cne $ExpectedVersion -or $metadata.abiVersion -cne $ExpectedVersion -or $metadata.sourceRevision -cne $ExpectedRevision) { throw 'Runtime version, ABI or source revision does not match the requested release.' }
    $listed = @($metadata.files | ForEach-Object { $_.path })
    $actual = @(Get-ChildItem -LiteralPath $RuntimeDir -File -Recurse | ForEach-Object { [IO.Path]::GetRelativePath($RuntimeDir, $_.FullName).Replace('\', '/') })
    if ($listed.Count -ne @($listed | Sort-Object -Unique).Count -or @(Compare-Object -ReferenceObject $listed -DifferenceObject $actual).Count) { throw 'Runtime file inventory is incomplete or contains unexpected files.' }
    foreach ($item in Get-ChildItem -LiteralPath $RuntimeDir -Recurse -Force) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Runtime cannot contain reparse points.' }
        if (-not $item.PSIsContainer) {
            $relative = [IO.Path]::GetRelativePath($RuntimeDir, $item.FullName).Replace('\', '/')
            if ($relative -ne 'lyflow-runtime.json' -and $relative -notlike 'licenses/*' -and ($relative.Contains('/') -or $item.Extension -ine '.dll')) { throw "Unexpected runtime content: $relative" }
            if ($item.Extension -ieq '.dll') {
                $reader = [IO.BinaryReader]::new([IO.File]::OpenRead($item.FullName))
                try {
                    if ($reader.ReadUInt16() -ne 0x5a4d) { throw "Not a PE DLL: $relative" }
                    $reader.BaseStream.Position = 0x3c
                    $offset = $reader.ReadInt32()
                    if ($offset -lt 64 -or $offset -gt $reader.BaseStream.Length - 6) { throw "Invalid PE DLL: $relative" }
                    $reader.BaseStream.Position = $offset
                    if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) { throw "DLL architecture must be x64: $relative" }
                } finally { $reader.Dispose() }
            }
        }
    }
    foreach ($name in @('lyflow_core.dll', 'onnxruntime.dll', 'onnxruntime_providers_shared.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll', 'vcomp140.dll', 'licenses/PROJECT-NOTICE.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $RuntimeDir $name) -PathType Leaf)) { throw "Runtime missing $name" }
    }
    $configuration = Get-Content -LiteralPath (Join-Path (Split-Path -Parent $PSScriptRoot) 'packaging/release.json') -Raw | ConvertFrom-Json
    foreach ($id in $configuration.requiredOperators) {
        if ($id -cnotin @($metadata.operators | ForEach-Object { $_.id })) { throw "Runtime metadata missing required operator: $id" }
    }
    return $metadata
}

function Expand-RuntimeReleaseArchive {
    param([string]$ArchivePath, [string]$Destination)
    $Destination = [IO.Path]::GetFullPath($Destination)
    $cursor = $Destination
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            if ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Runtime archive destination contains a reparse point.' }
        }
        $cursor = Split-Path -Parent $cursor
    }
    if ((Test-Path -LiteralPath $Destination) -and @(Get-ChildItem -LiteralPath $Destination -Force).Count) { throw 'Runtime archive destination must be empty.' }
    $archive = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($entry in $archive.Entries) {
            $name = $entry.FullName.Replace('\', '/')
            $type = ($entry.ExternalAttributes -shr 16) -band 0xf000
            if ($type -eq 0xa000 -or ($entry.ExternalAttributes -band 0x400)) { throw 'Runtime archive cannot contain links or reparse points.' }
            if (-not $name -or $name.StartsWith('/') -or [IO.Path]::IsPathRooted($name) -or $name.Contains(':') -or @($name.TrimEnd('/').Split('/') | Where-Object { $_ -in @('', '.', '..') }).Count -or -not $seen.Add($name.TrimEnd('/'))) { throw 'Unsafe or duplicate runtime archive path.' }
        }
    } finally { $archive.Dispose() }
    [IO.Compression.ZipFile]::ExtractToDirectory($ArchivePath, $Destination)
}
