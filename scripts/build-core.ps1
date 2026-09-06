# 单独构建 C++ core，跑算子自检 + doctest 测试。与 bridge/build.rs 同一条 CMake 路径。
# 用法：build-core.ps1 [RelWithDebInfo] [-NoTests]
$ErrorActionPreference = "Stop"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "找不到 vswhere，Visual Studio 可能没装" }

$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsPath) { throw "找不到带 C++ 工具集的 Visual Studio 安装" }

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
$cmake  = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja  = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if (-not (Test-Path $cmake)) { $cmake = "cmake" }   # 回退到 PATH 上的
if (-not (Test-Path $ninja)) { $ninja = "ninja" }

$root   = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root "core"
$build  = Join-Path $root "build\core"

# D8：固定 RelWithDebInfo，不跟 cargo profile 联动。
# Rust 永远用 /MD，vcpkg 的 debug 库是 /MDd，混用会在完全无关的地方崩。
$config = "RelWithDebInfo"
$runTests = $true
foreach ($a in $args) {
  if ($a -eq "-NoTests") { $runTests = $false }
  elseif ($a -like "-*") { throw "未知参数: $a" }
  else { $config = $a }
}

# vcpkg 工具链只在首次 configure 时生效，缓存里没有它的话之后怎么传都不管用
# （症状是「找不到 PCLConfig.cmake」）。所以发现不一致就直接重配。
$vcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { "C:/vcpkg" }
$toolchain = (Join-Path $vcpkgRoot "scripts/buildsystems/vcpkg.cmake") -replace '\\', '/'
if (-not (Test-Path $toolchain)) {
  throw "找不到 vcpkg 工具链: $toolchain（设 VCPKG_ROOT 或把 vcpkg 装到 C:\vcpkg）"
}
$cache = Join-Path $build "CMakeCache.txt"
if (Test-Path $cache) {
  $cached = @(Get-Content $cache | Where-Object { $_ -like 'CMAKE_TOOLCHAIN_FILE:*vcpkg.cmake*' })
  if ($cached.Count -eq 0) {
    Write-Host "旧的 CMake 缓存没有 vcpkg 工具链，重新配置" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $build
  }
}

# 算子包（ADR-0013）：环境变量 LYFLOW_OP_PACKS 是分号分隔的目录列表，缺省空。
# 永远显式传 -D，缓存里那份才不会和当前环境脱节。
$packs = if ($env:LYFLOW_OP_PACKS) { $env:LYFLOW_OP_PACKS } else { "" }
if ($packs) { Write-Host "算子包: $packs" -ForegroundColor Cyan }

# 必须在 vcvars 之后调 cmake，所以整条命令交给一个 cmd 进程。
# 用 && 而不是 ^ 续行：换行已被 PowerShell 吃掉，^ 会转义掉下一行的首字符。
$line = "call `"$vcvars`" >nul 2>&1" +
        " && `"$cmake`" -S `"$source`" -B `"$build`" -G Ninja" +
        " -DCMAKE_MAKE_PROGRAM=`"$ninja`" -DCMAKE_BUILD_TYPE=$config" +
        " -DCMAKE_TOOLCHAIN_FILE=`"$toolchain`"" +
        " -DLYFLOW_OP_PACKS=`"$packs`"" +
        " && `"$cmake`" --build `"$build`""

cmd /c $line
if ($LASTEXITCODE -ne 0) { throw "core 构建失败 (exit $LASTEXITCODE)" }

# 产物统一落在 bin/：DLL、两个 exe，以及 vcpkg applocal 拷来的 PCL/boost 依赖。
# bridge/build.rs 整目录拷贝它。
$bin = Join-Path $build "bin"
$dump = Join-Path $bin "lyflow-dump-manifest.exe"
& $dump --check
if ($LASTEXITCODE -ne 0) { throw "算子描述自检失败" }

if ($runTests) {
  $tests = Join-Path $bin "lyflow-core-tests.exe"
  & $tests
  if ($LASTEXITCODE -ne 0) { throw "core 测试失败" }
}

Write-Host "core ok -> $bin" -ForegroundColor Green
