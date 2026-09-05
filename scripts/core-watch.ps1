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

$interesting = @(".cpp", ".h", ".hpp", ".txt")

# 「源码指纹」：关心的文件的最后写入时间之和 + 个数。改名式写入（sed -i、编辑器的
# 原子保存）事件监视很容易漏掉，而指纹对「怎么写进去的」完全不敏感。
function Get-SourceFingerprint {
  $sum = 0.0
  $count = 0
  Get-ChildItem -Path $source -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object {
    if ($interesting -contains $_.Extension.ToLowerInvariant()) {
      $sum += $_.LastWriteTimeUtc.ToFileTimeUtc() / 1e7
      $count += 1
    }
  }
  return "$count/$sum"
}

Write-Host "core-watch：盯着 $source（Ctrl+C 退出）" -ForegroundColor Green

# 事件监视只当「快一点」的信号源，真正的判据永远是指纹。
$fsw = New-Object System.IO.FileSystemWatcher $source, "*.*"
$fsw.IncludeSubdirectories = $true
$fsw.NotifyFilter = [System.IO.NotifyFilters]::LastWrite -bor
                    [System.IO.NotifyFilters]::FileName -bor
                    [System.IO.NotifyFilters]::CreationTime

$fingerprint = Get-SourceFingerprint
$lastChange = $null
# 轮询兜底：两秒一次。FileSystemWatcher.WaitForChanged 只在被调用的那一刻才注册，
# 两次调用之间发生的改名事件是直接丢掉的。
$pollEverySeconds = 2
$lastPoll = Get-Date

try {
  while ($true) {
    $change = $fsw.WaitForChanged([System.IO.WatcherChangeTypes]::All, 500)
    $now = Get-Date

    if (-not $change.TimedOut) {
      # Renamed 时 Name 是新名字；临时文件改名成 .cpp 走的正是这一条
      $ext = [System.IO.Path]::GetExtension($change.Name)
      if ($interesting -contains $ext) { $lastChange = $now }
    }

    if (($now - $lastPoll).TotalSeconds -ge $pollEverySeconds) {
      $lastPoll = $now
      $current = Get-SourceFingerprint
      if ($current -ne $fingerprint) {
        $fingerprint = $current
        if (-not $lastChange) { $lastChange = $now }
      }
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
      # 构建期间源码可能又被改过，重新采一次指纹才不会立刻再触发一轮
      $fingerprint = Get-SourceFingerprint
      $lastPoll = Get-Date
    }
  }
} finally {
  $fsw.Dispose()
}
