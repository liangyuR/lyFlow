#Requires -Version 7.0
[CmdletBinding()]
param(
    [switch]$SkipDependencySetup,
    [switch]$SkipVisionBuild,
    [switch]$AllowDirty,
    [string]$OutputDir,
    [string]$ExpectedTag
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'release-common.ps1')
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
$environmentNames = @('LYFLOW_STD_PACKS', 'LYFLOW_PACKS', 'LYFLOW_MODULES', 'LYFLOW_OP_PACKS', 'LYFLOW_PREBUILT_CORE_DIR', 'LYFLOW_TEST_ONNX_MODEL', 'LYFLOW_P4_ASSETS', 'CARGO_TARGET_DIR', 'VCPKG_ROOT', 'VCPKG_INSTALLED_DIR', 'LYFLOW_ONNXRUNTIME_ROOT', 'OpenCV_DIR', 'LYFLOW_OPENCV_ROOT', 'LYFLOW_OPENCV_BIN', 'VCPKG_MANIFEST_MODE', 'VCPKG_MANIFEST_DIR', 'VCPKG_MANIFEST_INSTALL', 'VCPKG_TARGET_TRIPLET', 'VCPKG_DEFAULT_BINARY_CACHE', 'VCPKG_BINARY_SOURCES', 'VCPKG_NUGET_REPOSITORY', 'VCPKG_FORCE_DOWNLOADED_BINARIES', 'VCPKG_OVERLAY_TRIPLETS')
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
    # 发布组合以 packaging/release.json 为准：std-* 随 LYFLOW_STD_PACKS 编入，其余（默认关的仓库内包）按名点开。
    $releasePacks = @($configuration.packs | Sort-Object)
    $releaseModules = @($configuration.modules | Sort-Object)
    $env:LYFLOW_MODULES = $releaseModules -join ';'
    $env:LYFLOW_PACKS = (@($releasePacks | Where-Object { $_ -notlike 'std-*' }) -join ';')
    $env:LYFLOW_OP_PACKS = ''
    $buildParent = Join-Path $root 'build'
    if (-not $OutputDir) { $OutputDir = Join-Path $buildParent 'release' }
    $output = [IO.Path]::GetFullPath($OutputDir)
    foreach ($reserved in @('release-stage', 'release-core', 'release-rust', 'release-fixtures', 'release-tools', 'release-deps', 'release-diagnostics', 'release-dependency-diagnostics', 'matching-deps', 'barcode-deps', 'p4-assets', 'vision-deps', 'vision-downloads', 'vision-opencv', 'opencv5-install')) {
        $reservedPath = [IO.Path]::GetFullPath((Join-Path $buildParent $reserved))
        if ($output.Equals($reservedPath, [StringComparison]::OrdinalIgnoreCase) -or $output.StartsWith($reservedPath + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Output directory overlaps internal release workspace: $output"
        }
    }
    New-Item -ItemType Directory -Path $output -Force | Out-Null
    $runtimeZip = Join-Path $output "lyflow-runtime-win-x64-v$version.zip"
    $sdkZip = Join-Path $output "lyflow-sdk-win-x64-v$version.zip"
    $editorTgz = Join-Path $output "lyflow-editor-v$version.tgz"
    foreach ($archive in @($runtimeZip, $sdkZip, $editorTgz, (Join-Path $output 'lyflow-runtime.json'), (Join-Path $output "LyFlow.Runtime.win-x64.$version.nupkg"))) {
        if (Test-Path -LiteralPath $archive) { throw "Release asset already exists; use a fresh output directory: $archive" }
    }
    $stage = Reset-ReleaseDirectory -Path (Join-Path $buildParent 'release-stage') -Parent $buildParent
    $coreBuild = Reset-ReleaseDirectory -Path (Join-Path $buildParent 'release-core') -Parent $buildParent
    $diagnostics = Reset-ReleaseDirectory -Path (Join-Path $buildParent 'release-diagnostics') -Parent $buildParent
    $runtime = Join-Path $stage 'runtime'
    $sdk = Join-Path $stage 'sdk'
    New-Item -ItemType Directory -Path $runtime, $sdk -Force | Out-Null
    $env:LYFLOW_P4_ASSETS = Join-Path $buildParent 'p4-assets'
    & (Join-Path $PSScriptRoot 'prepare-release-deps.ps1') -SkipDependencySetup:$SkipDependencySetup -SkipVisionBuild:$SkipVisionBuild -P4ModelTests -DiagnosticsName package-dependencies
    $fixtures = Join-Path $buildParent 'release-fixtures'
    & python (Join-Path $root 'tests/release/make_fixtures.py') --output-dir $fixtures
    Assert-ReleaseExit 'Generate release fixtures'
    $env:LYFLOW_TEST_ONNX_MODEL = Join-Path $fixtures 'channels-6-to-8.onnx'
    if (-not (Test-Path -LiteralPath $env:LYFLOW_TEST_ONNX_MODEL)) { throw 'Release ONNX fixture is missing.' }
    & (Join-Path $PSScriptRoot 'build-core.ps1') -BuildDir $coreBuild -ReleaseBuild -SkipVisionBuild:$SkipVisionBuild -P4ModelTests
    $env:LYFLOW_PREBUILT_CORE_DIR = Join-Path $coreBuild 'bin'
    $env:CARGO_TARGET_DIR = Join-Path $buildParent 'release-rust'
    & cargo build --locked --release --manifest-path (Join-Path $root 'bridge/Cargo.toml') --bin lyflow --no-default-features 2>&1 |
        Tee-Object -FilePath (Join-Path $diagnostics 'cargo-build.log')
    $buildExit = $LASTEXITCODE
    if ($buildExit -ne 0) { Write-Host "Build release CLI failed (exit $buildExit)"; exit $buildExit }
    & cargo test --locked --release --manifest-path (Join-Path $root 'bridge/Cargo.toml') --no-default-features --features host --lib --bins 2>&1 |
        Tee-Object -FilePath (Join-Path $diagnostics 'cargo-test.log')
    $testExit = $LASTEXITCODE
    if ($testExit -ne 0) { Write-Host "Release Rust unit tests failed (exit $testExit)"; exit $testExit }
    $vs = Get-ReleaseVisualStudio
    $cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
    if (-not (Test-Path -LiteralPath $cmake)) { $cmake = 'cmake' }
    & $cmake --install $coreBuild --config RelWithDebInfo --prefix $sdk
    Assert-ReleaseExit 'Install SDK layout'
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $sdk 'bin') -Filter '*.dll' -File) {
        Copy-Item -LiteralPath $file.FullName -Destination $runtime
    }
    Copy-Item -LiteralPath (Join-Path $sdk 'bin/python') -Destination (Join-Path $runtime 'python') -Recurse
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
    foreach ($required in @('lyflow_core.dll', 'onnxruntime.dll', 'onnxruntime_providers_shared.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll', 'vcomp140.dll', 'tbb12.dll')) {
        if (-not (Test-Path -LiteralPath (Join-Path $runtime $required))) { throw "Runtime missing $required" }
    }
    foreach ($module in @('core', 'features', 'flann', 'geometry', 'imgcodecs', 'imgproc', 'objdetect')) {
        $file = "opencv_${module}510.dll"
        if (-not (Test-Path -LiteralPath (Join-Path $runtime $file) -PathType Leaf)) { throw "Runtime missing $file" }
    }
    if (@(Get-ChildItem -LiteralPath $runtime -Filter '*zxing*.dll' -File).Count) { throw 'ZXing must be statically linked, without an additional runtime DLL.' }
    $licenses = Join-Path $runtime 'licenses'
    New-Item -ItemType Directory -Path $licenses -Force | Out-Null
    $coreLicenses = Join-Path $licenses 'core'
    New-Item -ItemType Directory -Path $coreLicenses -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $root 'core/third_party/README.md') -Destination (Join-Path $coreLicenses 'SOURCES.md')
    foreach ($relative in @('nlohmann/LICENSE.MIT', 'doctest/LICENSE.txt', 'xxhash/LICENSE')) {
        $source = Join-Path $root "core/third_party/$relative"
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Core dependency license missing: $relative" }
        $destination = Join-Path $coreLicenses $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination
    }
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
    if (-not $env:LYFLOW_OPENCV_ROOT -or -not $env:OpenCV_DIR -or -not $env:LYFLOW_OPENCV_BIN) {
        throw 'Dependency preparation did not return the verified OpenCV installation.'
    }
    $opencvLicenses = Join-Path $licenses 'opencv'
    $installedNotices = Join-Path $env:LYFLOW_OPENCV_ROOT 'etc/licenses'
    foreach ($name in @('zlib-LICENSE', 'libpng-LICENSE', 'libpng-README', 'libjpeg-turbo-LICENSE.md',
        'libjpeg-turbo-README.md', 'libjpeg-turbo-README.ijg', 'SoftFloat-COPYING.txt',
        'mscr-chi_table_LICENSE.txt', 'annoylib-LICENSE', 'dlpack-LICENSE', 'flatbuffers-LICENSE.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $installedNotices $name) -PathType Leaf)) {
            throw "OpenCV bundled component license missing: $name"
        }
    }
    New-Item -ItemType Directory -Path $opencvLicenses -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $env:LYFLOW_OPENCV_ROOT 'LICENSE') -Destination $opencvLicenses
    Copy-Item -LiteralPath $installedNotices -Destination (Join-Path $opencvLicenses 'third-party') -Recurse
    $visionConfig = Get-Content -LiteralPath (Join-Path $root 'packaging/vision-deps.json') -Raw | ConvertFrom-Json
    foreach ($asset in $visionConfig.assets) {
        $noticeRoot = if ($asset.name -eq 'opencv') { $opencvLicenses } else { Join-Path $licenses "vision/$($asset.name)" }
        New-Item -ItemType Directory -Path $noticeRoot -Force | Out-Null
        foreach ($name in $asset.licenseFiles) {
            $source = Join-Path $root "build/vision-deps/$($asset.name)/$name"
            if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Vision dependency license missing: $source" }
            $destination = Join-Path $noticeRoot $name
            New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
            Copy-Item -LiteralPath $source -Destination $destination -Force
        }
        "$($asset.repository)@$($asset.revision)`n$($asset.license)" | Set-Content -LiteralPath (Join-Path $noticeRoot 'SOURCE.txt') -Encoding utf8NoBOM
    }
    'Only the MIT-licensed STag dictionary tables are consumed. LyFlow implements contour extraction and decoding independently; upstream ED/LSD implementations are not compiled, linked, or distributed.' |
        Set-Content -LiteralPath (Join-Path $licenses 'vision/stag/USAGE.txt') -Encoding utf8NoBOM
    $barcodeConfig = Get-Content -LiteralPath (Join-Path $root 'packaging/barcode-deps.json') -Raw | ConvertFrom-Json
    foreach ($asset in $barcodeConfig.assets) {
        $sourceRoot = Join-Path $root "build/barcode-deps/$($asset.name)"
        $noticeRoot = Join-Path $licenses "barcode/$($asset.name)"
        foreach ($name in $asset.licenseFiles) {
            $source = Join-Path $sourceRoot $name
            if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Barcode dependency license missing: $source" }
            $destination = Join-Path $noticeRoot $name
            New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
            Copy-Item -LiteralPath $source -Destination $destination
        }
        "$($asset.repository)@$($asset.revision)`n$($asset.license)`nStatic reader-only build; no Zint writer. LyFlow patch revision $($barcodeConfig.patchRevision) adds QR Model2 verified-empty-payload propagation only." |
            Set-Content -LiteralPath (Join-Path $noticeRoot 'SOURCE.txt') -Encoding utf8NoBOM
        Copy-Item -LiteralPath (Join-Path $root "packaging/patches/$($asset.patch)") -Destination (Join-Path $noticeRoot $asset.patch)
        Copy-Item -LiteralPath (Join-Path $root 'packaging/licenses/barcode-third-party.txt') -Destination (Join-Path $noticeRoot 'THIRD-PARTY-NOTICES.txt')
    }
    $matchingConfig = Get-Content -LiteralPath (Join-Path $root 'packaging/matching-deps.json') -Raw | ConvertFrom-Json
    foreach ($asset in $matchingConfig.assets) {
        $sourceRoot = Join-Path $root "build/matching-deps/$($asset.name)"
        $noticeRoot = Join-Path $licenses "matching/$($asset.name)"
        foreach ($name in $asset.licenseFiles) {
            $source = Join-Path $sourceRoot $name
            if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Matching dependency license missing: $source" }
            $destination = Join-Path $noticeRoot $name
            New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
            Copy-Item -LiteralPath $source -Destination $destination
        }
        "$($asset.repository)@$($asset.revision)`n$($asset.license)" | Set-Content -LiteralPath (Join-Path $noticeRoot 'SOURCE.txt') -Encoding utf8NoBOM
    }
    Copy-Item -LiteralPath (Join-Path $root 'packaging/licenses/matching-small-gicp-NOTICES.txt') -Destination (Join-Path $licenses 'matching/small-gicp/THIRD-PARTY-NOTICES.txt')
    Copy-Item -LiteralPath (Join-Path $root 'packaging/licenses/matching-kiss-NOTICES.txt') -Destination (Join-Path $licenses 'matching/kiss-matcher/THIRD-PARTY-NOTICES.txt')
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
            if ($paragraph -match '(?m)^Version(?:-String|-Semver|-Date)?: (.+)$' -and $paragraph -notmatch '(?m)^Feature:') {
                $dependencyVersion = $Matches[1].Trim()
                if ($paragraph -match '(?m)^Port-Version: (\d+)') { $dependencyVersion += "#$($Matches[1])" }
                if ($paragraph -match "(?m)^Architecture: $([regex]::Escape($configuration.triplet))\s*$") {
                    $dependencies.Add([ordered]@{ name = $name; version = $dependencyVersion })
                }
            }
        }
    }
    if (-not $dependencies.Count) { throw 'Installed dependency version inventory is empty.' }
    foreach ($bundled in @(
        @{ name = 'core-json'; file = 'nlohmann/json.hpp'; prefix = 'NLOHMANN_JSON'; patch = 'PATCH'; scope = 'runtime' },
        @{ name = 'core-doctest'; file = 'doctest/doctest.h'; prefix = 'DOCTEST'; patch = 'PATCH'; scope = 'test-only' },
        @{ name = 'core-xxhash'; file = 'xxhash/xxhash.h'; prefix = 'XXH'; patch = 'RELEASE'; scope = 'runtime' }
    )) {
        $header = Get-Content -LiteralPath (Join-Path $root "core/third_party/$($bundled.file)") -Raw
        $parts = foreach ($component in @('MAJOR', 'MINOR', $bundled.patch)) {
            $macro = "$($bundled.prefix)_VERSION_$component"
            $value = [regex]::Match($header, "(?m)^#define\s+$macro\s+(\d+)\b")
            if (-not $value.Success) { throw "Bundled header version missing: $macro" }
            $value.Groups[1].Value
        }
        $dependencies.Add([ordered]@{ name = $bundled.name; version = ($parts -join '.'); scope = $bundled.scope })
    }
    $dependencies.Add([ordered]@{ name = 'onnxruntime'; version = $configuration.onnxruntimeVersion })
    foreach ($asset in $visionConfig.assets) {
        $assetVersion = if ($asset.PSObject.Properties.Name -contains 'tag') { $asset.tag } else { $asset.revision }
        $dependencies.Add([ordered]@{ name = "vision-$($asset.name)"; version = $assetVersion; sourceRevision = $asset.revision })
    }
    foreach ($asset in $matchingConfig.assets) {
        $dependencies.Add([ordered]@{ name = "matching-$($asset.name)"; version = $asset.revision })
    }
    foreach ($asset in $barcodeConfig.assets) {
        $dependencies.Add([ordered]@{ name = "barcode-$($asset.name)"; version = $asset.tag; sourceRevision = $asset.revision; patchRevision = $barcodeConfig.patchRevision; linkage = 'static' })
    }
    $dependencies.Add([ordered]@{ name = 'visual-cpp-crt'; version = (Get-Item -LiteralPath (Join-Path $runtime 'vcruntime140.dll')).VersionInfo.FileVersion })
    $inspectionPath = Join-Path $stage 'inspection.json'
    & (Join-Path $PSScriptRoot 'inspect-runtime.ps1') -RuntimeDir $runtime -ReportPath $inspectionPath
    $inspection = Get-Content -LiteralPath $inspectionPath -Raw | ConvertFrom-Json
    if ($inspection.engineVersion -ne $version) { throw 'Staged DLL version differs from source product version.' }
    if ($inspection.abiVersion -isnot [string] -or $inspection.abiVersion -cne $version -or $inspection.abiVersion -cne $inspection.engineVersion) { throw 'Staged DLL ABI must be a string exactly equal to the product and engine version.' }
    if ($inspection.sourceRevision -isnot [string] -or ($inspection.sourceRevision -cne $revision -and -not ($AllowDirty -and $inspection.sourceRevision -ceq ''))) { throw 'Staged DLL source revision differs from HEAD. Only -AllowDirty diagnostics may use an empty compiled source revision.' }
    $packs = @($inspection.manifest.operators | Where-Object { $_.PSObject.Properties.Name -contains 'pack' -and $_.pack } | ForEach-Object pack | Sort-Object -Unique | ForEach-Object {
        $parts = $_ -split '@', 2
        if ($parts.Count -ne 2) { throw "Versioned operator pack required: $_" }
        [ordered]@{ name = $parts[0]; version = $parts[1] }
    })
    if (@(Compare-Object -ReferenceObject $releasePacks -DifferenceObject @($packs | ForEach-Object { $_.name })).Count) { throw 'Staged DLL operator packs differ from the release contract.' }
    $operators = @($inspection.manifest.operators | Sort-Object id | ForEach-Object {
        [ordered]@{ id = $_.id; version = $_.version; pack = $(if ($_.PSObject.Properties.Name -contains 'pack') { $_.pack } else { 'core' }) }
    })
    $metadataPath = Join-Path $runtime 'lyflow-runtime.json'
    $fileNames = @(@(Get-ChildItem -LiteralPath $runtime -File -Recurse | ForEach-Object { [IO.Path]::GetRelativePath($runtime, $_.FullName).Replace('\', '/') }) + 'lyflow-runtime.json' | Sort-Object)
    $metadata = [ordered]@{
        schemaVersion = 1; engineVersion = $inspection.engineVersion; abiVersion = $inspection.abiVersion
        sourceRevision = $revision; architecture = 'x64'; buildConfiguration = 'RelWithDebInfo'
        packs = $packs; modules = $releaseModules; operators = $operators; dependencies = @($dependencies | Sort-Object { $_.name } -Unique)
        files = @($fileNames | ForEach-Object { [ordered]@{ path = $_ } })
        localValidationOnly = [bool]($AllowDirty -or $SkipDependencySetup)
        licenseStatus = 'Project license not declared in source repository'
    }
    Write-ReleaseJson -Value $metadata -Path $metadataPath
    foreach ($file in Get-ChildItem -LiteralPath $runtime -Force) { Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $sdk 'bin') -Recurse -Force }
    Copy-Item -LiteralPath (Join-Path $env:CARGO_TARGET_DIR 'release/lyflow.exe') -Destination (Join-Path $sdk 'bin/lyflow.exe')
    & python (Join-Path $root 'tests/release/p4_contract.py') --cli (Join-Path $sdk 'bin/lyflow.exe') --examples (Join-Path $sdk 'examples') --assets $env:LYFLOW_P4_ASSETS --output-dir (Join-Path $diagnostics 'p4-cli')
    Assert-ReleaseExit 'Installed P4 CLI saved default/override/default contracts'
    & python (Join-Path $root 'tests/release/p5_contract.py') --cli (Join-Path $sdk 'bin/lyflow.exe') --examples (Join-Path $sdk 'examples') --output-dir (Join-Path $diagnostics 'p5-cli')
    Assert-ReleaseExit 'Installed P5 CLI native lineage and saved parameter contracts'
    & python (Join-Path $root 'bridge/tests/p6_delivery_contract.py') --cli (Join-Path $sdk 'bin/lyflow.exe') --core (Join-Path $sdk 'bin/lyflow_core.dll') --examples (Join-Path $sdk 'examples') --output-dir (Join-Path $diagnostics 'p6-cli')
    Assert-ReleaseExit 'Installed P6 CLI native frame recording replay logging and publication contracts'
    foreach ($relative in $configuration.requiredSdkFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $sdk $relative) -PathType Leaf)) { throw "SDK missing $relative" }
    }
    foreach ($relative in @('bin/lyflow_core.dll', 'bin/lyflow.exe', 'bin/python/lyflow_python/worker.py', 'bin/python/examples/offset.py', 'include/lyflow/client.hpp', 'include/lyflow/c_api.h', 'lib/lyflow_core.lib', 'lyflow-config.cmake', 'lyflow-config-version.cmake', 'examples/embed_minimal.cpp', 'examples/embed_concurrent.cpp', 'examples/embed_inject.cpp', 'scripts/fetch-p4-model-assets.py', 'scripts/generate-p4-ocr-fixture.py', 'packaging/p4-model-assets.json', 'examples/p4-ocr-scene.png', 'examples/p4-ocr-scene-truth.json', 'docs/p4-assets.md', 'scripts/generate-p5-fixtures.py', 'examples/p5-fixtures.json', 'examples/p5-depth-inspection.lyflow.json', 'examples/p5-depth-filters.lyflow.json', 'examples/p5-orthographic-roi.lyflow.json', 'examples/p5-box-crop.lyflow.json', 'examples/p5-pose-rigid.lyflow.json', 'examples/p5-pose-project-plane.lyflow.json', 'examples/p5-circles-3d.lyflow.json', 'examples/p5-mask-remove.lyflow.json', 'docs/p5-examples.md', 'library')) {
        if (-not (Test-Path -LiteralPath (Join-Path $sdk $relative))) { throw "SDK missing $relative" }
    }
    if (@(Get-ChildItem -LiteralPath $stage -File -Recurse | Where-Object Extension -In @('.pdb', '.onnx')).Count) { throw 'Debug symbols or test models leaked into staging.' }
    if (-not $AllowDirty -and -not $SkipDependencySetup) {
        & (Join-Path $PSScriptRoot 'package-runtime-nuget.ps1') -RuntimeDir $runtime -OutputDir $output -ExpectedVersion $version -ExpectedRevision $revision
    }
    $pendingRuntime = Join-Path $stage (Split-Path -Leaf $runtimeZip)
    $pendingSdk = Join-Path $stage (Split-Path -Leaf $sdkZip)
    [IO.Compression.ZipFile]::CreateFromDirectory($runtime, $pendingRuntime)
    [IO.Compression.ZipFile]::CreateFromDirectory($sdk, $pendingSdk)
    [IO.File]::Copy($pendingRuntime, $runtimeZip, $false)
    [IO.File]::Copy($pendingSdk, $sdkZip, $false)
    # 编辑器以 TypeScript 源码包随版本发布（宿主用自己的打包器编译），版本必须与产品版本一致。
    $editorDir = Join-Path $root 'packages/editor'
    $editorVersion = (Get-Content -LiteralPath (Join-Path $editorDir 'package.json') -Raw | ConvertFrom-Json).version
    if ($editorVersion -cne $version) { throw "@lyflow/editor version $editorVersion differs from product version $version." }
    $editorStage = Join-Path $stage 'editor'
    New-Item -ItemType Directory -Path $editorStage -Force | Out-Null
    & node (Join-Path $editorDir 'scripts/pack.mjs') --pack-destination $editorStage
    Assert-ReleaseExit 'Pack @lyflow/editor'
    $editorPacked = @(Get-ChildItem -LiteralPath $editorStage -Filter '*.tgz' -File)
    if ($editorPacked.Count -ne 1) { throw 'npm pack did not produce exactly one editor tarball.' }
    [IO.File]::Copy($metadataPath, (Join-Path $output 'lyflow-runtime.json'), $false)
    [IO.File]::Copy($editorPacked[0].FullName, $editorTgz, $false)
    Write-Host "Built runtime, SDK and editor archives: $output"
} finally {
    foreach ($name in $environmentNames) { [Environment]::SetEnvironmentVariable($name, $previousEnvironment[$name], 'Process') }
    Pop-Location
}
