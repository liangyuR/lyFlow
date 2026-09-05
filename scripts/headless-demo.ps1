# M4 §6 的第二条：一条真实任务用「库算子 + CLI」跑完，全程没有 GUI。
# 用法：headless-demo.ps1 [-Exe <lyflow.exe 的路径>]  —— 默认用 bridge/target/release/lyflow.exe
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$exe = Join-Path $root "bridge\target\release\lyflow.exe"
for ($i = 0; $i -lt $args.Count; $i++) {
  if ($args[$i] -eq "-Exe") { $exe = $args[$i + 1] }
}
if (-not (Test-Path $exe)) { throw "找不到 $exe —— 先跑 pnpm cli:build" }

$work = Join-Path ([System.IO.Path]::GetTempPath()) "lyflow 无界面 演示"
$lib = Join-Path $work "library"
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $lib | Out-Null

# 1) 库算子：一份存成文件的子图（体素降采样 + 统计离群点剔除），leafSize 提升成对外参数
$op = @'
{
  "id": "denoise",
  "version": "1.0.0",
  "name": "去噪",
  "category": "Cleanup",
  "keywords": ["denoise", "去噪"],
  "doc": "体素降采样 + 统计离群点剔除。",
  "nodes": [
    { "id": "voxel", "op": "filter.voxel_grid", "params": {} },
    { "id": "sor", "op": "filter.statistical_outlier", "params": { "meanK": 20 } }
  ],
  "edges": [
    { "id": "e1", "from": { "node": "voxel", "port": "cloud" },
                  "to": { "node": "sor", "port": "cloud" } }
  ],
  "inputs": [
    { "name": "cloud", "type": "PointCloud", "label": "Cloud",
      "to": [{ "node": "voxel", "port": "cloud" }] }
  ],
  "outputs": [
    { "name": "cloud", "type": "PointCloud", "label": "Cloud",
      "from": { "node": "sor", "port": "cloud" } }
  ],
  "params": [
    { "name": "leafSize", "type": "vec3f", "label": "Leaf size",
      "default": [0.01, 0.01, 0.01], "min": 0.0001, "unit": "m",
      "binds": [{ "node": "voxel", "param": "leafSize" }] }
  ]
}
'@
[System.IO.File]::WriteAllText((Join-Path $lib "denoise.lyflow-op.json"), $op,
                               (New-Object System.Text.UTF8Encoding($false)))

# 2) 一张用它的图。gen.synthetic 当数据源，省掉往仓库里塞 pcd
$graph = @'
{
  "schemaVersion": 1,
  "id": "01HEADLESSDEMO0000000000",
  "name": "headless demo",
  "nodes": [
    { "id": "n_gen", "op": "gen.synthetic",
      "params": { "pointCount": 200000, "seed": 7, "outlierRatio": 0.02 } },
    { "id": "n_clean", "op": "lib.denoise",
      "params": { "leafSize": [0.01, 0.01, 0.01] } }
  ],
  "edges": [
    { "id": "e1", "from": { "node": "n_gen", "port": "cloud" },
                  "to": { "node": "n_clean", "port": "cloud" } }
  ]
}
'@
$graphPath = Join-Path $work "demo.lyflow.json"
[System.IO.File]::WriteAllText($graphPath, $graph, (New-Object System.Text.UTF8Encoding($false)))

$env:LYFLOW_LIBRARY_DIRS = $lib
function Step($name) { Write-Host "`n=== $name ===" -ForegroundColor Cyan }

Step "manifest --check：库算子已注册"
& $exe manifest | ConvertFrom-Json | ForEach-Object {
  $hit = $_.operators | Where-Object { $_.id -eq "lib.denoise" }
  if (-not $hit) { throw "manifest 里没有 lib.denoise" }
  Write-Host ("  lib.denoise -> " + $hit.category + "，" + $hit.params.Count + " 个参数")
}

Step "validate：图是合法的"
& $exe validate $graphPath
if ($LASTEXITCODE -ne 0) { throw "validate 失败 (exit $LASTEXITCODE)" }

Step "plan：库算子展开成了路径式节点"
$plan = & $exe plan $graphPath | ConvertFrom-Json
$plan | ForEach-Object { Write-Host ("  " + $_.nodeId + "  " + $_.cacheKey) }
if (-not ($plan.nodeId -contains "n_clean/voxel")) { throw "计划里没有 n_clean/voxel" }

Step "run：JSON Lines 事件流"
$events = & $exe run $graphPath --no-cache | ForEach-Object { $_ | ConvertFrom-Json }
if ($LASTEXITCODE -ne 0) { throw "run 失败 (exit $LASTEXITCODE)" }
$final = $events | Where-Object { $_.kind -eq "node_state" -and $_.state -eq "done" }
$final | ForEach-Object { Write-Host ("  " + $_.nodeId + "  " + $_.stats.elementCount + " 点") }
$out = $final | Where-Object { $_.nodeId -eq "n_clean/sor" }
if (-not $out) { throw "n_clean/sor 没跑完" }

Step "dump：把结果写成 PCD"
$pcd = Join-Path $work "cleaned.pcd"
& $exe dump $graphPath "n_clean/sor:cloud" $pcd | Out-Null
if ($LASTEXITCODE -ne 0) { throw "dump 失败 (exit $LASTEXITCODE)" }
Write-Host ("  " + $pcd + "  " + (Get-Item $pcd).Length + " 字节")

Step "sweep：扫 5 组 leafSize，源头只加载一次"
$csv = Join-Path $work "sweep.csv"
$rows = & $exe sweep $graphPath --param "n_clean.leafSize=0.005:0.03:5" `
    --metric "n_clean/sor:cloud.elementCount" --csv $csv | ForEach-Object { $_ | ConvertFrom-Json }
$reused = ($rows | Where-Object { $_.skipped -contains "n_gen" }).Count
Write-Host ("  " + $rows.Count + " 组，其中 " + $reused + " 组复用了源头的结果")
if ($reused -lt 4) { throw "源头应当只算一次，实际复用了 $reused 次" }
Get-Content $csv | ForEach-Object { Write-Host "  $_" }

Step "diff：只挪了坐标的两份图没有语义差异"
$moved = Join-Path $work "moved.lyflow.json"
$doc = Get-Content $graphPath -Raw | ConvertFrom-Json
$doc.nodes | ForEach-Object { $_ | Add-Member -NotePropertyName ui -NotePropertyValue @{ position = @{ x = 999; y = 42 } } -Force }
[System.IO.File]::WriteAllText($moved, ($doc | ConvertTo-Json -Depth 20),
                               (New-Object System.Text.UTF8Encoding($false)))
$diff = & $exe diff $graphPath $moved --json | ConvertFrom-Json
if (-not $diff.empty) { throw "diff 不该有内容: $($diff | ConvertTo-Json -Compress)" }
Write-Host "  两份图在语义上完全一样"

Step "退出码"
& $exe run $graphPath --set "n_clean.leafSize=[0,0,0]" | Out-Null
Write-Host ("  坏参数 -> exit " + $LASTEXITCODE + "（应当是 1）")
if ($LASTEXITCODE -ne 1) { throw "校验失败应当退出 1" }

Write-Host "`n无界面演示全程跑通：$work" -ForegroundColor Green
