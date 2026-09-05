# 把 C++ 侧的 manifest dump 到 app/public/manifest.dev.json，
# 供浏览器模式（pnpm app:dev）不启动 Tauri 就能迭代界面。状态栏会标成「静态快照」。
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
& "$PSScriptRoot\build-core.ps1" -NoTests

$exe = Join-Path $root "build\core\bin\lyflow-dump-manifest.exe"
$out = Join-Path $root "app\public\manifest.dev.json"
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null

# 用 .NET 写文件：PowerShell 的 > 会写 UTF-16，manifest 里的中文会全废
$json = & $exe
if ($LASTEXITCODE -ne 0) { throw "dump 失败" }
[System.IO.File]::WriteAllText($out, ($json -join "`n") + "`n", (New-Object System.Text.UTF8Encoding($false)))

Write-Host "manifest -> $out" -ForegroundColor Green
