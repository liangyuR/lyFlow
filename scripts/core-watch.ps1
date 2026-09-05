# 盯着 core/ 的源码，改了就增量构建到 build/core，app 那边的 watcher 随即热重载。
# 用法：core-watch.ps1（`pnpm dev` 会自动带上它）
$ErrorActionPreference = "Stop"

$root   = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root "core"
$build  = Join-Path $root "build\core"

# 第一次先走完整的 build-core.ps1：它负责 configure、vcvars 和 vcpkg 工具链。
# 之后的增量构建才能只调 cmake --build。
if (-not (Test-Path (Join-Path $build "CMakeCache.txt"))) {
  Write-Host "首次构建 core（configure + 全量编译）…" -ForegroundColor Cyan
  & "$PSScriptRoot\build-core.ps1" -NoTests
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
$cmake  = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path $cmake)) { $cmake = "cmake" }

# 只构建 DLL 目标。dump-manifest 与测试 exe 在这条循环里没人看，
# 编它们只是让「改一行 cpp 到界面上出现新算子」多等几秒。
$buildCmd = "call `"$vcvars`" >nul 2>&1 && `"$cmake`" --build `"$build`" --target lyflow_core"

Write-Host "core-watch：盯着 $source（Ctrl+C 退出）" -ForegroundColor Green

$fsw = New-Object System.IO.FileSystemWatcher $source, "*.*"
$fsw.IncludeSubdirectories = $true
$fsw.NotifyFilter = [System.IO.NotifyFilters]::LastWrite -bor [System.IO.NotifyFilters]::FileName

# 编辑器保存一次会连发好几个事件，攒一下再动手；构建本身也会改 build/ 但那不在监视范围里。
$interesting = @(".cpp", ".h", ".hpp", ".txt")
$lastChange = $null

try {
  while ($true) {
    $change = $fsw.WaitForChanged([System.IO.WatcherChangeTypes]::All, 500)
    if (-not $change.TimedOut) {
      $ext = [System.IO.Path]::GetExtension($change.Name)
      if ($interesting -contains $ext) { $lastChange = Get-Date }
      continue
    }
    if ($lastChange -and ((Get-Date) - $lastChange).TotalMilliseconds -ge 400) {
      $lastChange = $null
      Write-Host "`n[core-watch] 检测到改动，增量构建…" -ForegroundColor Cyan
      cmd /c $buildCmd
      if ($LASTEXITCODE -eq 0) {
        Write-Host "[core-watch] 构建完成，等 app 热重载" -ForegroundColor Green
      } else {
        # 编译错误留在原地：旧的 DLL 没被覆盖，app 那边什么都不会发生。
        Write-Host "[core-watch] 构建失败，app 继续用上一代" -ForegroundColor Yellow
      }
    }
  }
} finally {
  $fsw.Dispose()
}
