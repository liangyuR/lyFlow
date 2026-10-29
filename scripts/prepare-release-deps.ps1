#Requires -Version 7.0
param(
    [switch]$SkipDependencySetup,
    [switch]$AllowBuild,
    [string]$InstallRoot,
    [string]$BinaryCache,
    [string]$DiagnosticsName = 'dependencies'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$config = Get-Content -LiteralPath (Join-Path $root 'packaging/release.json') -Raw | ConvertFrom-Json
$manifestRoot = Join-Path $root 'packaging'
$manifest = Get-Content -LiteralPath (Join-Path $manifestRoot 'vcpkg.json') -Raw | ConvertFrom-Json
if ($manifest.'builtin-baseline' -ne $config.vcpkgBaseline) { throw 'vcpkg manifest 与发布基线不一致' }
if (-not $IsWindows -or -not [Environment]::Is64BitProcess) { throw '发布依赖只支持 Windows x64 进程' }
if ($AllowBuild -and $env:GITHUB_REF_TYPE -eq 'tag') { throw 'tag 发布禁止编译依赖；请先在分支上显式预热缓存' }
if ($DiagnosticsName -notmatch '^[a-zA-Z0-9_-]+$') { throw '无效的依赖诊断名称' }

if ($SkipDependencySetup) {
    if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = 'C:\vcpkg' }
    if (-not $env:VCPKG_INSTALLED_DIR) { $env:VCPKG_INSTALLED_DIR = Join-Path $env:VCPKG_ROOT 'installed' }
    $env:VCPKG_MANIFEST_MODE = 'OFF'
} else {
    $env:VCPKG_ROOT = Join-Path $root 'build/release-tools/vcpkg'
    $env:VCPKG_INSTALLED_DIR = if ($InstallRoot) { [IO.Path]::GetFullPath($InstallRoot) } else { Join-Path $root 'build/release-deps/vcpkg_installed' }
    $env:VCPKG_MANIFEST_MODE = 'ON'
    if ($env:VCPKG_FORCE_SYSTEM_BINARIES) { throw '固定发布依赖不允许 VCPKG_FORCE_SYSTEM_BINARIES' }
    $env:VCPKG_FORCE_DOWNLOADED_BINARIES = '1'
    & (Join-Path $PSScriptRoot 'prepare-release-vcpkg.ps1') -ToolDir $env:VCPKG_ROOT -Baseline $config.vcpkgBaseline
    $toolchain = $config.dependencyToolchain
    if ($toolchain.msvcToolset -notmatch '^\d+\.\d+\.\d+$' -or $toolchain.windowsSdk -notmatch '^\d+\.\d+\.\d+\.\d+$') { throw '发布配置缺少固定 MSVC / Windows SDK 版本' }
    $env:VCPKG_OVERLAY_TRIPLETS = Join-Path $root 'build/release-deps/triplets'
    New-Item -ItemType Directory -Path $env:VCPKG_OVERLAY_TRIPLETS -Force | Out-Null
    $tripletLines = @(
        'set(VCPKG_TARGET_ARCHITECTURE x64)',
        'set(VCPKG_CRT_LINKAGE dynamic)',
        'set(VCPKG_LIBRARY_LINKAGE dynamic)',
        'set(VCPKG_PROVIDED_FORTRAN ON)',
        'set(VCPKG_PLATFORM_TOOLSET v143)',
        "set(VCPKG_PLATFORM_TOOLSET_VERSION $($toolchain.msvcToolset))",
        "set(VCPKG_CMAKE_SYSTEM_VERSION $($toolchain.windowsSdk))"
    )
    [IO.File]::WriteAllText((Join-Path $env:VCPKG_OVERLAY_TRIPLETS "$($config.triplet).cmake"), ($tripletLines -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
    if ($BinaryCache) { $env:VCPKG_DEFAULT_BINARY_CACHE = [IO.Path]::GetFullPath($BinaryCache) }
    if (-not $env:VCPKG_DEFAULT_BINARY_CACHE) { $env:VCPKG_DEFAULT_BINARY_CACHE = Join-Path $root 'build/release-deps/binary-cache' }
    New-Item -ItemType Directory -Path $env:VCPKG_DEFAULT_BINARY_CACHE -Force | Out-Null
    $feedMode = if ($env:LYFLOW_VCPKG_FEED_MODE) { $env:LYFLOW_VCPKG_FEED_MODE } else { 'read' }
    & (Join-Path $PSScriptRoot 'use-vcpkg-feed.ps1') -VcpkgRoot $env:VCPKG_ROOT -Mode $feedMode
    $vcpkg = Join-Path $env:VCPKG_ROOT 'vcpkg.exe'
    $diagnostics = Join-Path $root "build/release-dependency-diagnostics/$DiagnosticsName"
    New-Item -ItemType Directory -Path $diagnostics -Force | Out-Null
    $tools = [ordered]@{}
    foreach ($name in @('cmake', 'ninja', 'powershell-core')) {
        $tool = (& $vcpkg fetch $name | Select-Object -Last 1).Trim()
        if ($LASTEXITCODE -ne 0) { throw "无法获取固定工具 $name" }
        $tools[$name] = (& $tool --version | Out-String).Trim()
    }
    [ordered]@{
        baseline = $config.vcpkgBaseline
        toolchain = $toolchain
        tools = $tools
        runnerImage = $env:ImageVersion
        allowBuild = [bool]$AllowBuild
    } | ConvertTo-Json -Depth 5 | Tee-Object -FilePath (Join-Path $diagnostics 'environment.json') | Write-Host
    $arguments = @('install', "--x-manifest-root=$manifestRoot", "--x-install-root=$env:VCPKG_INSTALLED_DIR", "--triplet=$($config.triplet)", "--host-triplet=$($config.triplet)", '--clean-after-build')
    if (-not $AllowBuild) { $arguments += '--only-binarycaching' }
    try {
        & $vcpkg @arguments 2>&1 | ForEach-Object {
            $line = "$_"
            if ($env:LYFLOW_VCPKG_FEED_TOKEN) { $line = $line.Replace($env:LYFLOW_VCPKG_FEED_TOKEN, '***') }
            $line
        } | Tee-Object -FilePath (Join-Path $diagnostics 'install.log') | Write-Host
        if ($LASTEXITCODE -ne 0) { throw '固定依赖恢复失败；默认禁止源码编译。检查依赖诊断，更新构建配置后仅在预热任务中使用 -AllowBuild。' }
    } finally {
        foreach ($source in @(
            @{ name = 'planned'; path = (Join-Path $env:VCPKG_ROOT 'buildtrees') },
            @{ name = 'installed'; path = (Join-Path $env:VCPKG_INSTALLED_DIR "$($config.triplet)/share") }
        )) {
            if (-not (Test-Path -LiteralPath $source.path)) { continue }
            foreach ($file in Get-ChildItem -LiteralPath $source.path -Filter '*abi_info.txt' -Recurse -File) {
                $target = Join-Path (Join-Path $diagnostics $source.name) ([IO.Path]::GetRelativePath($source.path, $file.FullName))
                New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
                Copy-Item -LiteralPath $file.FullName -Destination $target -Force
            }
        }
    }
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
