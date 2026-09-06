# gap 领域包的门禁：LYFLOW_PACKS=gap 跑一遍全链路，再跑两条 A/B（模板路径、模型路径）。
# 数据与基线的位置可以用环境变量覆盖，见下面三个默认值。
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$dataset  = if ($env:LYFLOW_GAP_DATASET) { $env:LYFLOW_GAP_DATASET }
            else { "C:\Users\11601\OneDrive\Documents\DTS\tianmu_0904\dataset.yml" }
$model    = if ($env:LYFLOW_GAP_MODEL) { $env:LYFLOW_GAP_MODEL }
            else { "C:\Users\11601\OneDrive\Documents\DTS\models\v12s0.onnx" }
$baseline = if ($env:LYFLOW_GAP_BASELINE) { $env:LYFLOW_GAP_BASELINE }
            else { "$env:TEMP\lyflow-gap-baseline" }
$baselineModel = if ($env:LYFLOW_GAP_BASELINE_MODEL) { $env:LYFLOW_GAP_BASELINE_MODEL }
                 else { "$env:TEMP\lyflow-gap-baseline-model" }

function Step($name) { Write-Host "`n=== $name ===" -ForegroundColor Cyan }

$env:LYFLOW_PACKS = "gap"
Step "门禁（LYFLOW_PACKS=gap）"
& "$PSScriptRoot\check.ps1"
if ($LASTEXITCODE -ne 0) { throw "带 gap 包的 pnpm check 失败" }

# A/B 脚本按 mtime 挑 lyflow.exe，所以这一份必须是刚刚带包构建出来的
Step "CLI（带 gap 包）"
Push-Location (Join-Path $root "bridge")
try {
  cargo build --bin lyflow --no-default-features
  if ($LASTEXITCODE -ne 0) { throw "CLI 构建失败" }
} finally { Pop-Location }

$ab = Join-Path $root "packs\gap\tools\lyflow_ab.py"
if (-not (Test-Path $dataset)) {
  Write-Host "找不到数据集 $dataset，跳过两条 A/B" -ForegroundColor Yellow
  Write-Host "`ngap 门禁绿（A/B 未跑）" -ForegroundColor Yellow
  exit 0
}

Step "A/B：模板路径"
python $ab --dataset $dataset --baseline $baseline `
    --out "$env:TEMP\lyflow-gap-ab" --json "$env:TEMP\lyflow-gap-ab\ab.json"
if ($LASTEXITCODE -ne 0) { throw "模板路径 A/B 失败" }

Step "A/B：模型路径"
python $ab --dataset $dataset --baseline $baselineModel --model $model `
    --out "$env:TEMP\lyflow-gap-ab-model" --json "$env:TEMP\lyflow-gap-ab-model\ab.json"
if ($LASTEXITCODE -ne 0) { throw "模型路径 A/B 失败" }

Write-Host "`ngap 门禁全绿（两条 A/B 都过）" -ForegroundColor Green
