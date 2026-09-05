# 门禁：一条命令验完整条链路 —— C++ 编译/算子自检/doctest -> 三份契约对着 schema 校验
# -> cargo test（DLL 加载、执行事件、二进制输出、中文路径）-> 前端 strict typecheck + build。
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

function Step($name) { Write-Host "`n=== $name ===" -ForegroundColor Cyan }

Step "C++ core"
& "$PSScriptRoot\build-core.ps1"

Step "manifest vs schema"
$bin = Join-Path $root "build\core\bin"
$exe = Join-Path $bin "lyflow-dump-manifest.exe"
$tmp = Join-Path $env:TEMP "lyflow-manifest-check.json"
[System.IO.File]::WriteAllText($tmp, ((& $exe) -join "`n"), (New-Object System.Text.UTF8Encoding($false)))
python "$PSScriptRoot\validate_schema.py" $tmp (Join-Path $root "schema\operator-manifest.schema.json")
if ($LASTEXITCODE -ne 0) { throw "manifest 不符合 schema" }

Step "execution-event vs schema"
# 事件流没有「真实产物文件」可校验，所以校验的是手写样例（理由见 schema/README.md）。
python "$PSScriptRoot\validate_schema.py" `
    (Join-Path $root "schema\examples\execution-event.example.json") `
    (Join-Path $root "schema\execution-event.schema.json") --each
if ($LASTEXITCODE -ne 0) { throw "执行事件样例不符合 schema" }

Step "graph-doc vs schema"
python "$PSScriptRoot\validate_schema.py" `
    (Join-Path $root "schema\examples\graph.example.lyflow.json") `
    (Join-Path $root "schema\graph-doc.schema.json")
if ($LASTEXITCODE -ne 0) { throw "图样例不符合 schema" }

Step "Rust bridge"
Push-Location (Join-Path $root "bridge")
try {
  cargo test --quiet
  if ($LASTEXITCODE -ne 0) { throw "cargo test 失败" }
} finally { Pop-Location }

Step "headless CLI（不带 Tauri）"
# --no-default-features 是 F6 的证据：关掉 desktop 之后 tauri 一行都不该被编到。
Push-Location (Join-Path $root "bridge")
try {
  cargo build --quiet --bin lyflow --no-default-features
  if ($LASTEXITCODE -ne 0) { throw "CLI 构建失败" }
  & (Join-Path $root "bridge/target/debug/lyflow.exe") manifest --check | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "lyflow manifest --check 失败" }
} finally { Pop-Location }

Step "frontend"
Push-Location $root
try {
  pnpm --filter lyflow-app build
  if ($LASTEXITCODE -ne 0) { throw "前端构建失败" }
} finally { Pop-Location }

Write-Host "`n全链路绿" -ForegroundColor Green
