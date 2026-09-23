# gap 领域包的门禁：LYFLOW_PACKS=gap 跑一遍全链路（pnpm check）。
# gap 包的行为已有意偏离原算法（M7），A/B 与导入器对拍不再是门槛；设 LYFLOW_GAP_AB=1
# 时作为历史对拍工具顺手跑一遍、只打印结果，不判通过或失败。
# 数据与基线的位置可以用环境变量覆盖，见下面几个默认值。
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

if ($env:LYFLOW_GAP_AB -ne "1") {
  Write-Host "`ngap 门禁绿" -ForegroundColor Green
  exit 0
}
if (-not (Test-Path $dataset)) {
  Write-Host "`ngap 门禁绿；找不到数据集 $dataset，历史对拍没有跑" -ForegroundColor Yellow
  exit 0
}

# 对拍脚本按 mtime 挑 lyflow.exe，所以这一份必须是刚刚带包构建出来的
Step "CLI（带 gap 包）"
Push-Location (Join-Path $root "bridge")
try {
  cargo build --bin lyflow --no-default-features
  if ($LASTEXITCODE -ne 0) { throw "CLI 构建失败" }
} finally { Pop-Location }

# 下面几步的退出码只报告、不判门禁：差异是有意的，要看的是差在哪、差多少。
$ab = Join-Path $root "packs\gap\tools\lyflow_ab.py"
Step "历史对拍：模板路径"
python $ab --dataset $dataset --baseline $baseline `
    --out "$env:TEMP\lyflow-gap-ab" --json "$env:TEMP\lyflow-gap-ab\ab.json"
Write-Host "模板路径对拍退出码 $LASTEXITCODE（报告在 $env:TEMP\lyflow-gap-ab）"

Step "历史对拍：模型路径"
python $ab --dataset $dataset --baseline $baselineModel --model $model `
    --out "$env:TEMP\lyflow-gap-ab-model" --json "$env:TEMP\lyflow-gap-ab-model\ab.json"
Write-Host "模型路径对拍退出码 $LASTEXITCODE（报告在 $env:TEMP\lyflow-gap-ab-model）"

Step "历史对拍：回退图（导入器产出，模型主路径 + 模板备用闭包）"
python (Join-Path $root "packs\gap\tools\ab_fallback.py") --dataset $dataset `
    --baseline $baselineModel --model $model `
    --out "$env:TEMP\lyflow-gap-ab-fallback" --json "$env:TEMP\lyflow-gap-ab-fallback\ab.json"
Write-Host "回退图对拍退出码 $LASTEXITCODE（报告在 $env:TEMP\lyflow-gap-ab-fallback）"

Write-Host "`ngap 门禁绿（历史对拍只供参考）" -ForegroundColor Green
exit 0
