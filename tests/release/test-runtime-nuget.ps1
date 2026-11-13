#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $root 'scripts/runtime-nuget-common.ps1')
$work = Join-Path $root ('build/nuget-contract-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work -Force | Out-Null
$revision = 'a' * 40
$tests = 0
try {
    $runtime = Join-Path $work 'runtime'
    New-Item -ItemType Directory -Path (Join-Path $runtime 'licenses') -Force | Out-Null
    $pe = [byte[]]::new(128)
    $pe[0] = 0x4d; $pe[1] = 0x5a; $pe[0x3c] = 64
    $pe[64] = 0x50; $pe[65] = 0x45; $pe[68] = 0x64; $pe[69] = 0x86
    foreach ($name in @('lyflow_core.dll', 'onnxruntime.dll', 'onnxruntime_providers_shared.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll', 'vcomp140.dll')) { [IO.File]::WriteAllBytes((Join-Path $runtime $name), $pe) }
    Set-Content -LiteralPath (Join-Path $runtime 'licenses/PROJECT-NOTICE.txt') -Value 'Test fixture'
    $configuration = Get-Content -LiteralPath (Join-Path $root 'packaging/release.json') -Raw | ConvertFrom-Json
    $metadata = [ordered]@{
        schemaVersion = 1; architecture = 'x64'; localValidationOnly = $false
        engineVersion = '1.1.0'; abiVersion = '1.1.0'; sourceRevision = $revision
        operators = @($configuration.requiredOperators | ForEach-Object { @{ id = $_; version = '1.0.0' } })
        files = @(@(Get-ChildItem -LiteralPath $runtime -File -Recurse | ForEach-Object { [IO.Path]::GetRelativePath($runtime, $_.FullName).Replace('\', '/') }) + 'lyflow-runtime.json' | ForEach-Object { @{ path = $_ } })
    }
    $metadataFile = Join-Path $runtime 'lyflow-runtime.json'
    $original = $metadata | ConvertTo-Json -Depth 20
    Set-Content -LiteralPath $metadataFile -Value $original
    [void](Assert-RuntimeNuGetMetadata -RuntimeDir $runtime -ExpectedVersion '1.1.0' -ExpectedRevision $revision)
    $tests++
    function Expect-Rejected([scriptblock]$Action, [string]$Name) {
        $rejected = $false
        try { & $Action } catch { $rejected = $true }
        if (-not $rejected) { throw "Expected rejection: $Name" }
        $script:tests++
    }
    foreach ($change in @(
        @{ key = 'abiVersion'; value = '1.1.1' },
        @{ key = 'engineVersion'; value = '1.2.0' },
        @{ key = 'sourceRevision'; value = ('b' * 40) },
        @{ key = 'architecture'; value = 'arm64' },
        @{ key = 'localValidationOnly'; value = $true },
        @{ key = 'contentHash'; value = 'legacy' },
        @{ key = 'operators'; value = @() }
    )) {
        $changed = $original | ConvertFrom-Json -AsHashtable
        $changed[$change.key] = $change.value
        $changed | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $metadataFile
        Expect-Rejected { Assert-RuntimeNuGetMetadata -RuntimeDir $runtime -ExpectedVersion '1.1.0' -ExpectedRevision $revision } $change.key
    }
    Set-Content -LiteralPath $metadataFile -Value $original
    Expect-Rejected { Assert-RuntimeNuGetMetadata -RuntimeDir $runtime -ExpectedVersion '1.1.0-preview.1' -ExpectedRevision $revision } 'prerelease'
    Set-Content -LiteralPath (Join-Path $runtime 'unexpected.txt') -Value 'unexpected'
    Expect-Rejected { Assert-RuntimeNuGetMetadata -RuntimeDir $runtime -ExpectedVersion '1.1.0' -ExpectedRevision $revision } 'extra file'
    Remove-Item -LiteralPath (Join-Path $runtime 'unexpected.txt')
    $pe[68] = 0x4c; $pe[69] = 0x01
    [IO.File]::WriteAllBytes((Join-Path $runtime 'lyflow_core.dll'), $pe)
    Expect-Rejected { Assert-RuntimeNuGetMetadata -RuntimeDir $runtime -ExpectedVersion '1.1.0' -ExpectedRevision $revision } 'x86 DLL'
    $zip = Join-Path $work 'unsafe.zip'
    $archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
    try { [void]$archive.CreateEntry('../escape.dll') } finally { $archive.Dispose() }
    Expect-Rejected { Expand-RuntimeReleaseArchive -ArchivePath $zip -Destination (Join-Path $work 'unpacked') } 'archive traversal'
    foreach ($case in @(
        @{ name = '/root.dll'; attributes = 0; label = 'archive rooted path' },
        @{ name = 'C:/root.dll'; attributes = 0; label = 'archive drive path' },
        @{ name = 'link.dll'; attributes = -1577123840; label = 'archive Unix symlink' },
        @{ name = 'link.dll'; attributes = 0x400; label = 'archive Windows reparse point' }
    )) {
        $caseZip = Join-Path $work ([guid]::NewGuid().ToString('N') + '.zip')
        $archive = [IO.Compression.ZipFile]::Open($caseZip, [IO.Compression.ZipArchiveMode]::Create)
        try { $entry = $archive.CreateEntry($case.name); $entry.ExternalAttributes = $case.attributes } finally { $archive.Dispose() }
        Expect-Rejected { Expand-RuntimeReleaseArchive -ArchivePath $caseZip -Destination (Join-Path $work 'unpacked') } $case.label
    }
    $destination = Join-Path $work 'destination'
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    $junction = Join-Path $work 'junction'
    try {
        New-Item -ItemType Junction -Path $junction -Target $destination | Out-Null
        Expect-Rejected { Expand-RuntimeReleaseArchive -ArchivePath $zip -Destination (Join-Path $junction 'nested') } 'destination reparse path'
    } finally { if (Test-Path -LiteralPath $junction) { Remove-Item -LiteralPath $junction -Force } }
    $unsigned = Join-Path $work 'unsigned-nuget.exe'
    Set-Content -LiteralPath $unsigned -Value 'Untrusted test fixture'
    Expect-Rejected { Get-RuntimeNuGetTool -ToolPath $unsigned } 'unsigned NuGet executable'
    Write-Host "Runtime NuGet contract: $tests scenarios passed."
} finally {
    $safe = Assert-ReleaseChildPath -Path $work -Parent (Join-Path $root 'build')
    if (Test-Path -LiteralPath $safe) { Remove-Item -LiteralPath $safe -Recurse -Force }
}
