# 准备 packs/std-ml 要的 onnxruntime（T9）：优先从本机的 xyz-gap-inspector 复制，
# 否则从 GitHub release 下载同一版本。落到 third_party/onnxruntime/（已 gitignore）。
$ErrorActionPreference = "Stop"

$version = "1.19.2"
$name    = "onnxruntime-win-x64-$version"
$root    = Split-Path -Parent $PSScriptRoot
$dest    = Join-Path $root "third_party\onnxruntime\$name"

if (Test-Path (Join-Path $dest "lib\onnxruntime.lib")) {
  Write-Host "onnxruntime 已就位 -> $dest" -ForegroundColor Green
  exit 0
}

$local = "D:\project\xyz-gap-inspector\3rdparty\onnxruntime\$name"
if (Test-Path (Join-Path $local "lib\onnxruntime.lib")) {
  Write-Host "从 $local 复制…" -ForegroundColor Cyan
  New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
  Copy-Item -Recurse -Force $local $dest
  Write-Host "onnxruntime 就位 -> $dest" -ForegroundColor Green
  exit 0
}

$url = "https://github.com/microsoft/onnxruntime/releases/download/v$version/$name.zip"
$tmp = Join-Path $env:TEMP "$name.zip"
Write-Host "下载 $url …" -ForegroundColor Cyan
Invoke-WebRequest -Uri $url -OutFile $tmp -UseBasicParsing

$stage = Join-Path $env:TEMP "lyflow-ort-stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
Expand-Archive -Path $tmp -DestinationPath $stage -Force
New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
Copy-Item -Recurse -Force (Join-Path $stage $name) $dest
Remove-Item -Recurse -Force $stage
Remove-Item -Force $tmp

if (-not (Test-Path (Join-Path $dest "lib\onnxruntime.lib"))) {
  throw "解压后仍然没有 $dest\lib\onnxruntime.lib"
}
Write-Host "onnxruntime 就位 -> $dest" -ForegroundColor Green
