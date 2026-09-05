# 单独构建 C++ core，跑算子自检 + doctest 测试。
#
# 与 M1 的区别：core 现在是 CMake 构建的 DLL（D1/ADR-0004），bridge/build.rs
# 调的也是这套 CMake，不再用 cc crate 逐个编 .cpp —— PCL 是 vcpkg 动态三元组，
# 几十个 DLL 手工链是死路。所以这个脚本和 cargo 走的是同一条构建路径，
# 「单独调 core」和「跑整个 app」不会再出现只有一边能编过的情况。
#
# 用法：
#   build-core.ps1                     RelWithDebInfo，构建 + 自检 + 测试
#   build-core.ps1 RelWithDebInfo      同上
#   build-core.ps1 RelWithDebInfo -NoTests   只构建
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

# vcpkg 工具链只在**首次** configure 时生效。如果缓存是没有工具链的旧配置
# （比如 M1 留下的），后面怎么传都不会生效，只会得到一句莫名其妙的
# 「找不到 PCLConfig.cmake」。所以发现不一致就直接重配。
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

# 必须在 vcvars 之后调 cmake，所以整条命令交给一个 cmd 进程。
# 注意用 && 而不是 PowerShell here-string 里的 ^ 续行 —— 后者传给 cmd 时
# 换行已经被 PowerShell 吃掉了，^ 反而会把下一行的第一个字符转义掉。
$line = "call `"$vcvars`" >nul 2>&1" +
        " && `"$cmake`" -S `"$source`" -B `"$build`" -G Ninja" +
        " -DCMAKE_MAKE_PROGRAM=`"$ninja`" -DCMAKE_BUILD_TYPE=$config" +
        " -DCMAKE_TOOLCHAIN_FILE=`"$toolchain`"" +
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
