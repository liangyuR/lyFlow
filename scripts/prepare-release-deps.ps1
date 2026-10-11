#Requires -Version 7.0
param([switch]$SkipDependencySetup)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$config = Get-Content -LiteralPath (Join-Path $root 'packaging/release.json') -Raw | ConvertFrom-Json
$manifestRoot = Join-Path $root 'packaging'
$manifest = Get-Content -LiteralPath (Join-Path $manifestRoot 'vcpkg.json') -Raw | ConvertFrom-Json
if ($manifest.'builtin-baseline' -ne $config.vcpkgBaseline) { throw 'vcpkg manifest 与发布基线不一致' }
if (-not $IsWindows -or -not [Environment]::Is64BitProcess) { throw '发布依赖只支持 Windows x64 进程' }

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program 失败（$LASTEXITCODE）" }
}

if ($SkipDependencySetup) {
    if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = 'C:\vcpkg' }
    if (-not $env:VCPKG_INSTALLED_DIR) { $env:VCPKG_INSTALLED_DIR = Join-Path $env:VCPKG_ROOT 'installed' }
    $env:VCPKG_MANIFEST_MODE = 'OFF'
} else {
    $env:VCPKG_ROOT = Join-Path $root 'build/release-tools/vcpkg'
    $env:VCPKG_INSTALLED_DIR = Join-Path $root 'build/release-deps/vcpkg_installed'
    $env:VCPKG_MANIFEST_MODE = 'ON'
    if (-not (Test-Path -LiteralPath (Join-Path $env:VCPKG_ROOT '.git'))) {
        New-Item -ItemType Directory -Path $env:VCPKG_ROOT -Force | Out-Null
        Invoke-Checked git @('init', $env:VCPKG_ROOT)
        Invoke-Checked git @('-C', $env:VCPKG_ROOT, 'remote', 'add', 'origin', 'https://github.com/microsoft/vcpkg.git')
    }
    $revision = & git -C $env:VCPKG_ROOT rev-parse HEAD 2>$null
    if ($LASTEXITCODE -ne 0 -or $revision -ne $config.vcpkgBaseline) {
        Invoke-Checked git @('-C', $env:VCPKG_ROOT, 'fetch', '--depth=1', 'origin', $config.vcpkgBaseline)
        Invoke-Checked git @('-C', $env:VCPKG_ROOT, 'checkout', '--detach', $config.vcpkgBaseline)
    }
    if (-not (Test-Path -LiteralPath (Join-Path $env:VCPKG_ROOT 'vcpkg.exe'))) {
        Invoke-Checked (Join-Path $env:VCPKG_ROOT 'bootstrap-vcpkg.bat') @('-disableMetrics')
    }
    if (-not $env:VCPKG_DEFAULT_BINARY_CACHE) { $env:VCPKG_DEFAULT_BINARY_CACHE = Join-Path $root 'build/release-deps/binary-cache' }
    New-Item -ItemType Directory -Path $env:VCPKG_DEFAULT_BINARY_CACHE -Force | Out-Null
    Invoke-Checked (Join-Path $env:VCPKG_ROOT 'vcpkg.exe') @('install', "--x-manifest-root=$manifestRoot", "--x-install-root=$env:VCPKG_INSTALLED_DIR", "--triplet=$($config.triplet)", '--clean-after-build')
}
$env:VCPKG_MANIFEST_DIR = $manifestRoot
$env:VCPKG_MANIFEST_INSTALL = 'OFF'
$env:VCPKG_TARGET_TRIPLET = $config.triplet
$revision = & git -C $env:VCPKG_ROOT rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $revision -ne $config.vcpkgBaseline) { throw 'vcpkg checkout 不符合固定发布基线' }
$statusPath = Join-Path $env:VCPKG_INSTALLED_DIR 'vcpkg/status'
$status = Get-Content -LiteralPath $statusPath -Raw
$records = @($status -split '(?:\r?\n){2,}')
foreach ($property in $config.vcpkgVersions.PSObject.Properties) {
    $record = @($records | Where-Object { $_ -match "(?m)^Package: $([regex]::Escape($property.Name))\r?$" -and $_ -notmatch '(?m)^Feature:' -and $_ -match "(?m)^Architecture: $($config.triplet)\r?$" })
    if ($record.Count -ne 1 -or $record[0] -notmatch '(?m)^Status: install ok installed\r?$') { throw "未安装完整依赖 $($property.Name)" }
    $version = [regex]::Match($record[0], '(?m)^Version(?:-String|-Semver|-Date)?: ([^\r\n]+)').Groups[1].Value
    $port = [regex]::Match($record[0], '(?m)^Port-Version: (\d+)').Groups[1].Value
    if ($port -and $port -ne '0') { $version += "#$port" }
    if ($version -ne $property.Value) { throw "$($property.Name) 版本不符：$version，应为 $($property.Value)" }
}
foreach ($feature in @('calib3d', 'fs', 'intrinsics', 'jpeg', 'png', 'thread')) {
    if (-not @($records | Where-Object { $_ -match '(?m)^Package: opencv4\r?$' -and $_ -match "(?m)^Feature: $feature\r?$" -and $_ -match "(?m)^Architecture: $($config.triplet)\r?$" }).Count) { throw "OpenCV 缺少 feature：$feature" }
}
$ortName = "onnxruntime-win-x64-$($config.onnxruntimeVersion)"
if (-not $SkipDependencySetup -or -not $env:LYFLOW_ONNXRUNTIME_ROOT) { $env:LYFLOW_ONNXRUNTIME_ROOT = Join-Path $root "third_party/onnxruntime/$ortName" }
if (-not (Test-Path -LiteralPath (Join-Path $env:LYFLOW_ONNXRUNTIME_ROOT 'lib/onnxruntime.lib'))) {
    if ($SkipDependencySetup) { throw '固定 ONNX Runtime 未准备；请先正常运行依赖准备，或设置 LYFLOW_ONNXRUNTIME_ROOT' }
    $downloadRoot = Join-Path $root 'build/release-tools/downloads'
    New-Item -ItemType Directory -Path $downloadRoot -Force | Out-Null
    $archive = Join-Path $downloadRoot "$ortName.zip"
    Invoke-WebRequest -Uri "https://github.com/microsoft/onnxruntime/releases/download/v$($config.onnxruntimeVersion)/$ortName.zip" -OutFile $archive
    Expand-Archive -LiteralPath $archive -DestinationPath (Split-Path -Parent $env:LYFLOW_ONNXRUNTIME_ROOT) -Force
}
foreach ($file in @('lib/onnxruntime.lib', 'lib/onnxruntime.dll', 'lib/onnxruntime_providers_shared.dll', 'include/onnxruntime_c_api.h', 'VERSION_NUMBER', 'LICENSE', 'ThirdPartyNotices.txt')) {
    if (-not (Test-Path -LiteralPath (Join-Path $env:LYFLOW_ONNXRUNTIME_ROOT $file) -PathType Leaf)) { throw "ONNX Runtime 缺少 $file" }
}
$ortVersion = (Get-Content -LiteralPath (Join-Path $env:LYFLOW_ONNXRUNTIME_ROOT 'VERSION_NUMBER') -Raw).Trim()
if ($ortVersion -ne $config.onnxruntimeVersion) { throw "ONNX Runtime 版本不符：$ortVersion" }
Write-Host "固定依赖已验证：$($config.triplet)，vcpkg $revision，ONNX Runtime $ortVersion"
