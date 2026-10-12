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
    & (Join-Path $PSScriptRoot 'prepare-release-vcpkg.ps1') -ToolDir $env:VCPKG_ROOT -Baseline $config.vcpkgBaseline
    if (-not $env:VCPKG_DEFAULT_BINARY_CACHE) { $env:VCPKG_DEFAULT_BINARY_CACHE = Join-Path $root 'build/release-deps/binary-cache' }
    New-Item -ItemType Directory -Path $env:VCPKG_DEFAULT_BINARY_CACHE -Force | Out-Null
    # 有令牌时再挂上 GitHub Packages 的 NuGet 源：本地缓存没有的包先去那下载，仍没有才编。
    # 写权限只给工作流的依赖 job（LYFLOW_VCPKG_FEED_MODE=readwrite），编译 job 与开发机都是只读。
    $feedMode = if ($env:LYFLOW_VCPKG_FEED_MODE) { $env:LYFLOW_VCPKG_FEED_MODE } else { 'read' }
    & (Join-Path $PSScriptRoot 'use-vcpkg-feed.ps1') -VcpkgRoot $env:VCPKG_ROOT -Mode $feedMode
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
