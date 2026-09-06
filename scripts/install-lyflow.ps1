# 一键产出嵌入宿主用的安装目录（A1-11）。布局与用法见 docs/embedding.md。
# 用法：install-lyflow.ps1 [-Prefix <dir>] [-SkipBuild]
# 默认 prefix 是 LYFLOW_INSTALL_PREFIX，再缺省是 <repo>/build/install。
param(
  [string]$Prefix,
  [switch]$SkipBuild,
  # 已经有一份 lyflow.exe 时直接拷它，不再 cargo build --release。
  # pnpm check 用它把「刚构建好的那一份」放进布局 —— A/B 脚本按 mtime 挑
  # lyflow.exe，让门禁顺手产出一个更新的 release 版会让它挑错人。
  [string]$CliPath
)
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build\core"

if (-not $Prefix) {
  $Prefix = if ($env:LYFLOW_INSTALL_PREFIX) { $env:LYFLOW_INSTALL_PREFIX }
            else { Join-Path $root "build\install" }
}

function Step($name) { Write-Host "`n=== $name ===" -ForegroundColor Cyan }

if (-not $SkipBuild) {
  Step "构建 core"
  & "$PSScriptRoot\build-core.ps1" -NoTests
  if ($LASTEXITCODE -ne 0) { throw "core 构建失败" }
}
if (-not (Test-Path (Join-Path $build "CMakeCache.txt"))) {
  throw "没有 $build 的构建缓存，先跑一次不带 -SkipBuild 的本脚本"
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
$cmake = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path $cmake)) { $cmake = "cmake" }

Step "cmake --install -> $Prefix"
& $cmake --install $build --prefix $Prefix
if ($LASTEXITCODE -ne 0) { throw "cmake --install 失败" }

# CLI 归 cargo 管，cmake 不知道它，所以在这里拷。--no-default-features 是
# headless 构建：安装目录里不该带 Tauri。
Step "lyflow CLI"
if ($CliPath) {
  $cli = $CliPath
} else {
  Push-Location (Join-Path $root "bridge")
  try {
    cargo build --release --bin lyflow --no-default-features
    if ($LASTEXITCODE -ne 0) { throw "CLI 构建失败" }
  } finally { Pop-Location }
  $cli = Join-Path $root "bridge\target\release\lyflow.exe"
}
if (-not (Test-Path $cli)) { throw "找不到 $cli" }
Copy-Item $cli (Join-Path $Prefix "bin") -Force

# 自检：布局齐不齐。少一样的表现是宿主那边一个远得离谱的错误。
Step "自检"
$want = @("bin\lyflow_core.dll", "bin\lyflow.exe", "include\lyflow\client.hpp",
          "include\lyflow\c_api.h", "lyflow-config.cmake", "examples\embed_minimal.cpp",
          "library")
foreach ($rel in $want) {
  $p = Join-Path $Prefix $rel
  if (-not (Test-Path $p)) { throw "安装目录里缺 $rel" }
}
$dlls = @(Get-ChildItem (Join-Path $Prefix "bin") -Filter *.dll).Count
Write-Host "bin 里有 $dlls 个 DLL" -ForegroundColor Green
Write-Host "安装完成 -> $Prefix" -ForegroundColor Green
