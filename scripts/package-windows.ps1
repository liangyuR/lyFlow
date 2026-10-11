#Requires -Version 7.0
[CmdletBinding()]
param(
    [switch]$SkipDependencySetup,
    [switch]$AllowDirty,
    [string]$OutputDir,
    [string]$ExpectedTag
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'release-common.ps1')
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
$environmentNames = @('LYFLOW_STD_PACKS', 'LYFLOW_PACKS', 'LYFLOW_OP_PACKS', 'LYFLOW_PREBUILT_CORE_DIR', 'LYFLOW_TEST_ONNX_MODEL', 'CARGO_TARGET_DIR', 'VCPKG_ROOT', 'VCPKG_INSTALLED_DIR', 'LYFLOW_ONNXRUNTIME_ROOT', 'VCPKG_MANIFEST_MODE', 'VCPKG_MANIFEST_DIR', 'VCPKG_MANIFEST_INSTALL', 'VCPKG_TARGET_TRIPLET', 'VCPKG_DEFAULT_BINARY_CACHE')
$previousEnvironment = @{}
foreach ($name in $environmentNames) { $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    if (-not $IsWindows -or -not [Environment]::Is64BitProcess) { throw 'Packaging requires Windows x64 PowerShell.' }
    $version = Get-ReleaseVersion -Root $root
    $revision = (& git rev-parse HEAD | Out-String).Trim()
    Assert-ReleaseExit 'Read source revision'
    if ($revision -notmatch '^[0-9a-f]{40}$') { throw 'A known Git source revision is required.' }
    $dirty = @(& git status --porcelain --untracked-files=all)
    Assert-ReleaseExit 'Read source status'
    if ($dirty.Count -gt 0 -and -not $AllowDirty) { throw 'Release packaging requires a clean source tree. -AllowDirty is only for local validation.' }
    if ($ExpectedTag) {
        if ($AllowDirty -or $SkipDependencySetup) { throw 'Tag releases cannot use local validation switches.' }
        if ($ExpectedTag -cne "v$version") { throw "Expected tag $ExpectedTag does not match v$version" }
        $tagRevision = (& git rev-parse "$ExpectedTag^{commit}" | Out-String).Trim()
        Assert-ReleaseExit 'Resolve release tag'
        if ($tagRevision -ne $revision) { throw 'Release tag does not point to HEAD.' }
    }
    if ($env:LYFLOW_OP_PACKS) { throw 'External operator packs are forbidden in release packaging.' }
    $configuration = Get-Content -LiteralPath (Join-Path $root 'packaging/release.json') -Raw | ConvertFrom-Json
    $env:LYFLOW_STD_PACKS = '1'
    $env:LYFLOW_PACKS = 'glue'
    $env:LYFLOW_OP_PACKS = ''
    $buildParent = Join-Path $root 'build'
    if (-not $OutputDir) { $OutputDir = Join-Path $buildParent 'release' }
    $output = [IO.Path]::GetFullPath($OutputDir)
    foreach ($reserved in @('release-stage', 'release-core', 'release-rust', 'release-fixtures', 'release-tools', 'release-deps')) {
        $reservedPath = [IO.Path]::GetFullPath((Join-Path $buildParent $reserved))
        if ($output.Equals($reservedPath, [StringComparison]::OrdinalIgnoreCase) -or $output.StartsWith($reservedPath + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Output directory overlaps internal release workspace: $output"
        }
    }
    New-Item -ItemType Directory -Path $output -Force | Out-Null
    $runtimeZip = Join-Path $output "lyflow-runtime-win-x64-v$version.zip"
    $sdkZip = Join-Path $output "lyflow-sdk-win-x64-v$version.zip"
    foreach ($archive in @($runtimeZip, $sdkZip, (Join-Path $output 'lyflow-runtime.json'), (Join-Path $output 'verification-report.json'))) {
        if (Test-Path -LiteralPath $archive) { throw "Release asset already exists; use a fresh output directory: $archive" }
    }
    $stage = Reset-ReleaseDirectory -Path (Join-Path $buildParent 'release-stage') -Parent $buildParent
    $coreBuild = Reset-ReleaseDirectory -Path (Join-Path $buildParent 'release-core') -Parent $buildParent
    $runtime = Join-Path $stage 'runtime'
    $sdk = Join-Path $stage 'sdk'
    New-Item -ItemType Directory -Path $runtime, $sdk -Force | Out-Null
    & (Join-Path $PSScriptRoot 'prepare-release-deps.ps1') -SkipDependencySetup:$SkipDependencySetup
    $fixtures = Join-Path $buildParent 'release-fixtures'
    & python (Join-Path $root 'tests/release/make_fixtures.py') --output-dir $fixtures
    Assert-ReleaseExit 'Generate release fixtures'
    $env:LYFLOW_TEST_ONNX_MODEL = Join-Path $fixtures 'channels-6-to-8.onnx'
    if (-not (Test-Path -LiteralPath $env:LYFLOW_TEST_ONNX_MODEL)) { throw 'Release ONNX fixture is missing.' }
    & (Join-Path $PSScriptRoot 'build-core.ps1') -BuildDir $coreBuild
    $env:LYFLOW_PREBUILT_CORE_DIR = Join-Path $coreBuild 'bin'
    $env:CARGO_TARGET_DIR = Join-Path $buildParent 'release-rust'
    & cargo build --locked --release --manifest-path (Join-Path $root 'bridge/Cargo.toml') --bin lyflow --no-default-features
    Assert-ReleaseExit 'Build release CLI'
    $vs = Get-ReleaseVisualStudio
    $cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
    if (-not (Test-Path -LiteralPath $cmake)) { $cmake = 'cmake' }
    & $cmake --install $coreBuild --config RelWithDebInfo --prefix $sdk
    Assert-ReleaseExit 'Install SDK layout'
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $sdk 'bin') -Filter '*.dll' -File) {
        Copy-Item -LiteralPath $file.FullName -Destination $runtime
    }
    $redistRoot = Join-Path $vs 'VC/Redist/MSVC'
    $redistVersions = @(Get-ChildItem -LiteralPath $redistRoot -Directory | Where-Object Name -Match '^\d+\.\d+\.\d+$' | Sort-Object { [version]$_.Name } -Descending)
    if (-not $redistVersions.Count) { throw 'Visual C++ redistributable files were not found.' }
    $crtDirectories = @(Get-ChildItem -LiteralPath (Join-Path $redistVersions[0].FullName 'x64') -Directory | Where-Object Name -Match '^Microsoft\.VC\d+\.CRT$')
    if ($crtDirectories.Count -ne 1) { throw 'Expected one x64 Visual C++ CRT directory.' }
    foreach ($file in Get-ChildItem -LiteralPath $crtDirectories[0].FullName -Filter '*.dll' -File) {
        Copy-Item -LiteralPath $file.FullName -Destination $runtime -Force
    }
    $openmpDirectories = @(Get-ChildItem -LiteralPath (Join-Path $redistVersions[0].FullName 'x64') -Directory | Where-Object Name -Match '^Microsoft\.VC\d+\.OpenMP$')
    if ($openmpDirectories.Count -ne 1) { throw 'Expected one x64 Visual C++ OpenMP runtime directory.' }
    Copy-Item -LiteralPath (Join-Path $openmpDirectories[0].FullName 'vcomp140.dll') -Destination $runtime -Force
    foreach ($required in @('lyflow_core.dll', 'onnxruntime.dll', 'onnxruntime_providers_shared.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll', 'vcomp140.dll')) {
        if (-not (Test-Path -LiteralPath (Join-Path $runtime $required))) { throw "Runtime missing $required" }
    }
    $licenses = Join-Path $runtime 'licenses'
    New-Item -ItemType Directory -Path $licenses -Force | Out-Null
    $installed = $env:VCPKG_INSTALLED_DIR
    if (-not $installed) { throw 'Dependency preparation did not return VCPKG_INSTALLED_DIR.' }
    $share = Join-Path $installed "$($configuration.triplet)/share"
    $copyrights = @(Get-ChildItem -LiteralPath $share -Filter copyright -File -Recurse)
    if (-not $copyrights.Count) { throw 'No vcpkg dependency copyright files found.' }
    foreach ($file in $copyrights) {
        $relative = [IO.Path]::GetRelativePath($share, $file.FullName)
        $destination = Join-Path $licenses "vcpkg/$relative"
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $destination
    }
    $ortLicenses = Join-Path $licenses 'onnxruntime'
    New-Item -ItemType Directory -Path $ortLicenses -Force | Out-Null
    foreach ($name in @('LICENSE', 'ThirdPartyNotices.txt')) {
        $source = Join-Path $env:LYFLOW_ONNXRUNTIME_ROOT $name
        if (-not (Test-Path -LiteralPath $source)) { throw "ONNX Runtime license missing: $source" }
        Copy-Item -LiteralPath $source -Destination $ortLicenses
    }
    $crtLicenseDir = Join-Path $licenses 'visual-cpp'
    New-Item -ItemType Directory -Path $crtLicenseDir -Force | Out-Null
    $vsLicenses = Join-Path $vs 'Licenses'
    if (Test-Path -LiteralPath $vsLicenses) {
        $notices = @(Get-ChildItem -LiteralPath $vsLicenses -File -Recurse | Where-Object { $_.Name -match '(ThirdPartyNotices|Redist|Visual.*Studio|VC.*License)' -and $_.Directory.Name -in @('1033', '2052', 'Licenses') })
        foreach ($file in $notices) { Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $crtLicenseDir "$($file.Directory.Name)-$($file.Name)") }
    }
    "App-local CRT source: VC/Redist/MSVC/$($redistVersions[0].Name)/x64/$($crtDirectories[0].Name)`nVisual Studio: $vs`nRedistribution is subject to the applicable Microsoft Visual Studio license terms. Included notices are copied from this installation; this file does not grant additional rights." | Set-Content -LiteralPath (Join-Path $crtLicenseDir 'SOURCE.txt') -Encoding utf8NoBOM
    'The source repository does not declare a project license. This archive does not grant additional rights. Third-party notices are included in this directory.' | Set-Content -LiteralPath (Join-Path $licenses 'PROJECT-NOTICE.txt') -Encoding utf8NoBOM
    $dependencies = [Collections.Generic.List[object]]::new()
    $status = Get-Content -LiteralPath (Join-Path $installed 'vcpkg/status') -Raw
    foreach ($paragraph in ($status -split '\r?\n\r?\n')) {
        if ($paragraph -match '(?m)^Package: (.+)$') {
            $name = $Matches[1].Trim()
            if ($paragraph -match '(?m)^Version: (.+)$') {
                $dependencyVersion = $Matches[1].Trim()
                if ($paragraph -match '(?m)^Port-Version: (\d+)') { $dependencyVersion += "#$($Matches[1])" }
                if ($paragraph -match "(?m)^Architecture: $([regex]::Escape($configuration.triplet))\s*$") {
                    $dependencies.Add([ordered]@{ name = $name; version = $dependencyVersion })
                }
            }
        }
    }
    if (-not $dependencies.Count) { throw 'Installed dependency version inventory is empty.' }
    $dependencies.Add([ordered]@{ name = 'onnxruntime'; version = $configuration.onnxruntimeVersion })
    $dependencies.Add([ordered]@{ name = 'visual-cpp-crt'; version = (Get-Item -LiteralPath (Join-Path $runtime 'vcruntime140.dll')).VersionInfo.FileVersion })
    $inspectionPath = Join-Path $stage 'inspection.json'
    & (Join-Path $PSScriptRoot 'test-release.ps1') -InspectOnly -RuntimeDir $runtime -ReportPath $inspectionPath
    $inspection = Get-Content -LiteralPath $inspectionPath -Raw | ConvertFrom-Json
    if ($inspection.engineVersion -ne $version) { throw 'Staged DLL version differs from source product version.' }
    $abiHeader = Get-Content -LiteralPath (Join-Path $root 'core/include/lyflow/c_api.h') -Raw
    if ($abiHeader -notmatch '#define\s+LYFLOW_ABI_VERSION\s+(\d+)' -or [int]$Matches[1] -ne $inspection.abiVersion) { throw 'Staged DLL ABI differs from source declaration.' }
    $packs = @($inspection.manifest.operators | Where-Object { $_.PSObject.Properties.Name -contains 'pack' -and $_.pack } | ForEach-Object pack | Sort-Object -Unique | ForEach-Object {
        $parts = $_ -split '@', 2
        if ($parts.Count -ne 2) { throw "Versioned operator pack required: $_" }
        [ordered]@{ name = $parts[0]; version = $parts[1] }
    })
    $expectedPacks = @('glue', 'std-image', 'std-ml', 'std-pointcloud')
    if (@(Compare-Object -ReferenceObject $expectedPacks -DifferenceObject @($packs | ForEach-Object { $_.name })).Count) { throw 'Staged DLL operator packs differ from the release contract.' }
    $operators = @($inspection.manifest.operators | Sort-Object id | ForEach-Object {
        [ordered]@{ id = $_.id; version = $_.version; pack = $(if ($_.PSObject.Properties.Name -contains 'pack') { $_.pack } else { 'core' }) }
    })
    $metadataPath = Join-Path $runtime 'lyflow-runtime.json'
    $fileNames = @(@(Get-ChildItem -LiteralPath $runtime -File -Recurse | ForEach-Object { [IO.Path]::GetRelativePath($runtime, $_.FullName).Replace('\', '/') }) + 'lyflow-runtime.json' | Sort-Object)
    $metadata = [ordered]@{
        schemaVersion = 1; engineVersion = $inspection.engineVersion; abiVersion = $inspection.abiVersion
        sourceRevision = $revision; architecture = 'x64'; buildConfiguration = 'RelWithDebInfo'
        packs = $packs; operators = $operators; dependencies = @($dependencies | Sort-Object name -Unique)
        files = @($fileNames | ForEach-Object { [ordered]@{ path = $_ } })
        localValidationOnly = [bool]($AllowDirty -or $SkipDependencySetup)
        licenseStatus = 'Project license not declared in source repository'
    }
    Write-ReleaseJson -Value $metadata -Path $metadataPath
    foreach ($file in Get-ChildItem -LiteralPath $runtime -Force) { Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $sdk 'bin') -Recurse -Force }
    Copy-Item -LiteralPath (Join-Path $env:CARGO_TARGET_DIR 'release/lyflow.exe') -Destination (Join-Path $sdk 'bin/lyflow.exe')
    foreach ($relative in @('bin/lyflow_core.dll', 'bin/lyflow.exe', 'include/lyflow/client.hpp', 'include/lyflow/c_api.h', 'lib/lyflow_core.lib', 'lyflow-config.cmake', 'lyflow-config-version.cmake', 'examples/embed_minimal.cpp', 'examples/embed_concurrent.cpp', 'examples/embed_inject.cpp', 'library')) {
        if (-not (Test-Path -LiteralPath (Join-Path $sdk $relative))) { throw "SDK missing $relative" }
    }
    if (@(Get-ChildItem -LiteralPath $stage -File -Recurse | Where-Object Extension -In @('.pdb', '.onnx')).Count) { throw 'Debug symbols or test models leaked into staging.' }
    $pendingRuntime = Join-Path $stage (Split-Path -Leaf $runtimeZip)
    $pendingSdk = Join-Path $stage (Split-Path -Leaf $sdkZip)
    [IO.Compression.ZipFile]::CreateFromDirectory($runtime, $pendingRuntime)
    [IO.Compression.ZipFile]::CreateFromDirectory($sdk, $pendingSdk)
    $report = Join-Path $stage 'verification-report.json'
    & (Join-Path $PSScriptRoot 'test-release.ps1') -ArchiveDir $stage -ReportPath $report
    $verification = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
    if ($verification.status -ne 'passed') { throw 'Archive verification did not report passed.' }
    [IO.File]::Copy($pendingRuntime, $runtimeZip, $false)
    [IO.File]::Copy($pendingSdk, $sdkZip, $false)
    [IO.File]::Copy($metadataPath, (Join-Path $output 'lyflow-runtime.json'), $false)
    [IO.File]::Copy($report, (Join-Path $output 'verification-report.json'), $false)
    Write-Host "Verified runtime and SDK archives: $output"
} finally {
    foreach ($name in $environmentNames) { [Environment]::SetEnvironmentVariable($name, $previousEnvironment[$name], 'Process') }
    Pop-Location
}
