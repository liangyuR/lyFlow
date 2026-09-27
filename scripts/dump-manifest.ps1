#Requires -Version 7.0
# 把 C++ 侧的 manifest dump 到 app/public/manifest.dev.json，
# 供浏览器模式（pnpm app:dev）不启动 Tauri 就能迭代界面。状态栏会标成「静态快照」。
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
& "$PSScriptRoot\build-core.ps1" -NoTests

$exe = Join-Path $root "build\core\bin\lyflow-dump-manifest.exe"
$out = Join-Path $root "app\public\manifest.dev.json"
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null

# exe 输出的是 UTF-8；pwsh 按控制台代码页解码，ACP=936 的机器上中文会变乱码
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$json = & $exe
if ($LASTEXITCODE -ne 0) { throw "dump 失败" }
Set-Content -Path $out -Value (($json -join "`n") + "`n") -NoNewline -Encoding utf8NoBOM

Write-Host "manifest -> $out" -ForegroundColor Green
