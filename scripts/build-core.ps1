# 单独构建 C++ core 并跑自检。
#
# Rust 侧不走这个脚本 —— bridge/build.rs 用 cc crate 直接编译 core 的源文件，
# 不需要 CMake。这里是给单独调试 core 用的（改 manifest 时比启动整个 app 快）。
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
$config = if ($args.Count -gt 0) { $args[0] } else { "Debug" }

# 必须在 vcvars 之后调 cmake，所以整条命令交给一个 cmd 进程。
# 注意用 && 而不是 PowerShell here-string 里的 ^ 续行 —— 后者传给 cmd 时
# 换行已经被 PowerShell 吃掉了，^ 反而会把下一行的第一个字符转义掉。
$line = "call `"$vcvars`" >nul 2>&1" +
        " && `"$cmake`" -S `"$source`" -B `"$build`" -G Ninja" +
        " -DCMAKE_MAKE_PROGRAM=`"$ninja`" -DCMAKE_BUILD_TYPE=$config" +
        " && `"$cmake`" --build `"$build`""

cmd /c $line
if ($LASTEXITCODE -ne 0) { throw "core 构建失败 (exit $LASTEXITCODE)" }

$exe = Join-Path $build "lyflow-dump-manifest.exe"
& $exe --check
if ($LASTEXITCODE -ne 0) { throw "算子描述自检失败" }

Write-Host "core ok -> $exe" -ForegroundColor Green
