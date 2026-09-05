# M0 门禁：一条命令验完整条链路。
#
#   C++ 编译 + 算子自检
#   -> manifest 对着 schema 校验（契约真的没破）
#   -> Rust 编译 + 测试（FFI 通、结构校验对）
#   -> 前端 strict typecheck + build
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

function Step($name) { Write-Host "`n=== $name ===" -ForegroundColor Cyan }

Step "C++ core"
& "$PSScriptRoot\build-core.ps1"

Step "manifest vs schema"
$exe = Join-Path $root "build\core\lyflow-dump-manifest.exe"
$tmp = Join-Path $env:TEMP "lyflow-manifest-check.json"
[System.IO.File]::WriteAllText($tmp, ((& $exe) -join "`n"), (New-Object System.Text.UTF8Encoding($false)))
python "$PSScriptRoot\validate_schema.py" $tmp (Join-Path $root "schema\operator-manifest.schema.json")
if ($LASTEXITCODE -ne 0) { throw "manifest 不符合 schema" }

Step "Rust bridge"
Push-Location (Join-Path $root "bridge")
try {
  cargo test --quiet
  if ($LASTEXITCODE -ne 0) { throw "cargo test 失败" }
} finally { Pop-Location }

Step "frontend"
Push-Location $root
try {
  pnpm --filter lyflow-app build
  if ($LASTEXITCODE -ne 0) { throw "前端构建失败" }
} finally { Pop-Location }

Write-Host "`nM0 链路全绿" -ForegroundColor Green
