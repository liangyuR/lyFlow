#Requires -Version 7.0
# glue 领域包的门禁：默认全链路；-Steps 复用 check.ps1 的步骤选择。
# 设了 LYFLOW_GLUE_DATA（演示数据的解压目录，下面有 Glue1/、Glue2/）时，再用刚构建的 CLI 跑真实帧评估
# （packs/glue/tools/glue_eval.py：§4 第 14 / 15 / 18 条，没过就判失败）与人造断胶（glue_synth.py：第 16 条，
# 只打印结果、不判门禁）。报告写到 %TEMP%\lyflow-glue-eval / -synth（真实帧是客户数据，不进仓库，D12）。
param([string[]]$Steps)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

function Step($name) { Write-Host "`n=== $name ===" -ForegroundColor Cyan }

$env:LYFLOW_PACKS = "glue"
$env:LYFLOW_STD_PACKS = "1"
Step "门禁（LYFLOW_PACKS=glue）"
& "$PSScriptRoot\check.ps1" -Steps $Steps
if ($LASTEXITCODE -ne 0) { throw "带 glue 包的 pnpm check 失败" }

$consoleEncoding = [Console]::OutputEncoding
try {
  [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
  $manifestJson = (& (Join-Path $root "build\core\bin\lyflow-dump-manifest.exe")) -join "`n"
  if ($LASTEXITCODE -ne 0) { throw "无法读取算子清单" }
  $manifest = $manifestJson | ConvertFrom-Json
} finally {
  [Console]::OutputEncoding = $consoleEncoding
}
foreach ($op in @("glue.bead_path", "glue.taught_path", "glue.bead_width", "glue.bead_breaks", "glue.edge_distance", "glue.judge", "glue.synth_break", "glue.locate", "glue.station_calipers", "image.board_calib", "image.load_calib")) {
  if ($op -notin $manifest.operators.id) { throw "构建中缺少 $op，不能视为 glue 验收通过" }
}

if ($Steps.Count -gt 0) {
  Write-Host "所选步骤已通过；完整门禁运行 pnpm check:glue" -ForegroundColor Green
  exit 0
}

if (-not $env:LYFLOW_GLUE_DATA) {
  Write-Host "`nglue 门禁绿；没设 LYFLOW_GLUE_DATA，真实帧评估没有跑" -ForegroundColor Yellow
  exit 0
}
if (-not (Test-Path (Join-Path $env:LYFLOW_GLUE_DATA "Glue1"))) {
  throw "LYFLOW_GLUE_DATA=$env:LYFLOW_GLUE_DATA 下面没有 Glue1/"
}

# check.ps1 的「headless CLI」那一步已经按当前 LYFLOW_PACKS 编好了 bridge/target/debug/lyflow.exe
$cli = Join-Path $root "bridge\target\debug\lyflow.exe"
Step "真实帧评估（第 14 / 15 / 18 条）"
python (Join-Path $root "packs\glue\tools\glue_eval.py") --lyflow $cli --out (Join-Path $env:TEMP "lyflow-glue-eval")
if ($LASTEXITCODE -ne 0) { throw "真实帧评估没过（报告在 $env:TEMP\lyflow-glue-eval\report.md）" }

Step "人造断胶（第 16 条，只报告）"
python (Join-Path $root "packs\glue\tools\glue_synth.py") --lyflow $cli --out (Join-Path $env:TEMP "lyflow-glue-synth")
Write-Host "人造断胶退出码 $LASTEXITCODE（报告在 $env:TEMP\lyflow-glue-synth\report.md）"

Write-Host "`nglue 门禁绿" -ForegroundColor Green
exit 0
