#Requires -Version 7.0
param(
  [string[]]$Steps
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$allSteps = @("manifest", "schema", "bridge", "client", "cli", "frontend", "mcp")
$picked = @($Steps | ForEach-Object { $_ -split "," } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
foreach ($s in $picked) {
  if ($allSteps -notcontains $s) { throw "不认识的步骤: $s（可选：$($allSteps -join '、')）" }
}
function Want($name) { return $picked.Count -eq 0 -or $picked -contains $name }

function Step($name) { Write-Host "`n=== $name ===" -ForegroundColor Cyan }

if (Want "manifest") {
  python "$PSScriptRoot/validate_schema.py" "$env:LYFLOW_NATIVE_SDK/operator-manifest.json" "$root/schema/operator-manifest.schema.json"
  if ($LASTEXITCODE) { throw "SDK manifest 不符合公共 schema" }
}

if (Want "schema") {
  Step "execution-event vs schema"
  # 事件流没有「真实产物文件」可校验，所以校验的是手写样例（理由见 schema/README.md）。
  python "$PSScriptRoot/validate_schema.py" `
      (Join-Path $root "schema/examples/execution-event.example.json") `
      (Join-Path $root "schema/execution-event.schema.json") --each
  if ($LASTEXITCODE -ne 0) { throw "执行事件样例不符合 schema" }

  Step "graph-doc vs schema"
  python "$PSScriptRoot/validate_schema.py" `
      (Join-Path $root "schema/examples/graph.example.lyflow.json") `
      (Join-Path $root "schema/graph-doc.schema.json")
  if ($LASTEXITCODE -ne 0) { throw "图样例不符合 schema" }
  # 图参数的完整规格（param-recipe P1.1）：规格字段复用 manifest 的 paramSpec（跨文件 $ref），
  # 老格式 { type?, default, binds, doc? } 同一份样例里也有一条；负例是拼错的字段名，必须被拒。
  python "$PSScriptRoot/validate_schema.py" `
      (Join-Path $root "schema/examples/graph-params.example.lyflow.json") `
      (Join-Path $root "schema/graph-doc.schema.json")
  if ($LASTEXITCODE -ne 0) { throw "图参数样例不符合 schema" }
  python "$PSScriptRoot/validate_schema.py" `
      (Join-Path $root "schema/examples/graph-params.invalid.lyflow.json") `
      (Join-Path $root "schema/graph-doc.schema.json") --expect-fail
  if ($LASTEXITCODE -ne 0) { throw "图参数负例居然通过了 schema" }
  # 参数面板的全类型示例图（param-recipe P2.10）：transform / curve 的值、子图、完整规格的图参数都在里面
  python "$PSScriptRoot/validate_schema.py" `
      (Join-Path $root "examples/param-showcase.lyflow.json") `
      (Join-Path $root "schema/graph-doc.schema.json")
  if ($LASTEXITCODE -ne 0) { throw "examples/param-showcase.lyflow.json 不符合 schema" }

  Step "配方文件 vs schema"
  # 配方的共享夹具（param-recipe P3，schema/fixtures/recipes/）：每个配方文件按 expected.json 的 schemaValid
  # 该过的过、该拒的拒（缺 graph 的那份 schema 不认，编辑器照样读、报失配 ④）；index.json 与夹具图也各查一遍。
  # 失配报告本身两边各断言一遍：Rust（bridge/src/recipe.rs，下面 cargo test 那一步）与 @lyflow/editor 的单测（frontend 那一步），
  # 对着同一份 expected.json（摘要、条目、建议与文案逐字）。
  $fixtures = Join-Path $root "schema/fixtures/recipes"
  $expected = Get-Content (Join-Path $fixtures "expected.json") -Raw | ConvertFrom-Json
  foreach ($prop in $expected.recipes.PSObject.Properties) {
    $file = Join-Path $fixtures "graph.recipes/$($prop.Name)"
    if ($prop.Value.schemaValid) {
      python "$PSScriptRoot/validate_schema.py" $file (Join-Path $root "schema/recipe.schema.json")
    } else {
      python "$PSScriptRoot/validate_schema.py" $file (Join-Path $root "schema/recipe.schema.json") --expect-fail
    }
    if ($LASTEXITCODE -ne 0) { throw "配方夹具 $($prop.Name) 与 schema 的预期不符" }
  }
  python "$PSScriptRoot/validate_schema.py" (Join-Path $fixtures "graph.recipes/index.json") (Join-Path $root "schema/recipe-index.schema.json")
  if ($LASTEXITCODE -ne 0) { throw "配方夹具 index.json 不符合 schema" }
  python "$PSScriptRoot/validate_schema.py" (Join-Path $fixtures "graph.lyflow.json") (Join-Path $root "schema/graph-doc.schema.json")
  if ($LASTEXITCODE -ne 0) { throw "配方夹具的图不符合 graph-doc schema" }

  Step "片段文件 vs schema"
  # 包随附的片段（m8-plan L14）。算子与端口对不对由 core 的启动自检查（manifest --check），
  # 这里只查文件形状 —— 没编进本次构建的包，它的片段也照样要合格。
  foreach ($snippet in Get-ChildItem -Path (Join-Path $root "solutions/*/snippets/*.lyflow-snippet.json")) {
    python "$PSScriptRoot/validate_schema.py" $snippet.FullName (Join-Path $root "schema/snippet.schema.json")
    if ($LASTEXITCODE -ne 0) { throw "片段 $($snippet.Name) 不符合 schema" }
  }

  Step "glue 图与像素叠画 vs schema"
  foreach ($graph in Get-ChildItem (Join-Path $root "solutions/glue/graphs/*.lyflow.json")) {
    python "$PSScriptRoot/validate_schema.py" $graph.FullName (Join-Path $root "schema/graph-doc.schema.json")
    if ($LASTEXITCODE -ne 0) { throw "glue 图 $($graph.Name) 不符合 schema" }
  }
  python "$PSScriptRoot/validate_schema.py" (Join-Path $root "schema/examples/overlay2d.example.json") (Join-Path $root "schema/overlay2d.schema.json")
  if ($LASTEXITCODE -ne 0) { throw "像素叠画样例不符合 schema" }
  python "$PSScriptRoot/validate_schema.py" (Join-Path $root "schema/examples/overlay2d.invalid.json") (Join-Path $root "schema/overlay2d.schema.json") --expect-fail
  if ($LASTEXITCODE -ne 0) { throw "像素叠画负例没有被拒绝" }
  python "$PSScriptRoot/validate_schema.py" (Join-Path $root "schema/examples/eval-samples.example.json") (Join-Path $root "schema/eval-sample.schema.json") --each
  if ($LASTEXITCODE -ne 0) { throw "标注样本不符合 schema" }
  python "$PSScriptRoot/validate_schema.py" (Join-Path $root "schema/examples/eval-sample.invalid.json") (Join-Path $root "schema/eval-sample.schema.json") --expect-fail
  if ($LASTEXITCODE -ne 0) { throw "标注样本负例没有被拒绝" }
}

if (Want "bridge") {
  Step "Rust bridge"
  Push-Location (Join-Path $root "bridge")
  try {
    cargo test --quiet
    if ($LASTEXITCODE -ne 0) { throw "cargo test 失败" }
  } finally { Pop-Location }
}

if (Want "client") {
  Step "Rust 客户端：脱开 bridge 单独构建"
  # 这个 crate 的卖点是宿主不必参与 core 的构建（docs/embedding.md「Rust 客户端」）。
  # 单独编一遍是那句话的最低证据：它要是又依赖回 bridge 或 core 的构建产物，这一步就断了。
  Push-Location (Join-Path $root "crates/lyflow-client")
  try {
    cargo build --quiet
    if ($LASTEXITCODE -ne 0) { throw "lyflow-client 单独构建失败" }
  } finally { Pop-Location }
}

if ((Want "cli") -or ((Want "mcp") -and !$env:LYFLOW_CLI)) {
  $metadata = cargo metadata --format-version 1 --no-deps --manifest-path (Join-Path $root 'bridge/Cargo.toml') | ConvertFrom-Json
  if ($LASTEXITCODE) { throw "无法定位 Cargo 构建目录" }
  $env:LYFLOW_CLI = Join-Path $metadata.target_directory "debug/lyflow.exe"
}

if (Want "cli") {
  Step "headless CLI（不带 Tauri）"
  # --no-default-features 是 F6 的证据：关掉 desktop 之后 tauri 一行都不该被编到。
  Push-Location (Join-Path $root "bridge")
  try {
    cargo build --quiet --bin lyflow --no-default-features
    if ($LASTEXITCODE -ne 0) { throw "CLI 构建失败" }
    & $env:LYFLOW_CLI manifest --check | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "lyflow manifest --check 失败" }
  } finally { Pop-Location }
}

if (Want "frontend") {
  Step "frontend"
  Push-Location $root
  try {
    # 包先单独过一遍 strict typecheck：app 那一遍是顺着 import 走的，
    # 漏掉的文件（比如只有宿主才用的入口）在这里才会暴露。
    pnpm --filter "@lyflow/editor" typecheck
    if ($LASTEXITCODE -ne 0) { throw "@lyflow/editor 类型检查失败" }
    pnpm --filter "@lyflow/visualization" test
    if ($LASTEXITCODE -ne 0) { throw "@lyflow/visualization tests failed" }
    pnpm --filter "@lyflow/editor" test
    if ($LASTEXITCODE -ne 0) { throw "@lyflow/editor 单测失败" }
    pnpm --filter lyflow-app build
    if ($LASTEXITCODE -ne 0) { throw "前端构建失败" }
    pnpm --filter lyflow-host-react build
    if ($LASTEXITCODE -ne 0) { throw "examples/host-react 构建失败" }
  } finally { Pop-Location }
}

if (Want "mcp") {
  Step "MCP 服务"
  # 集成冒烟自己起 test-server，用的是前面刚构建好的 debug lyflow.exe；
  # 那个 exe 不在时冒烟 skip 并打出原因，其余用例照跑。
  Push-Location $root
  try {
    pnpm --filter "@lyflow/mcp" typecheck
    if ($LASTEXITCODE -ne 0) { throw "@lyflow/mcp 类型检查失败" }
    pnpm --filter "@lyflow/mcp" build
    if ($LASTEXITCODE -ne 0) { throw "@lyflow/mcp 构建失败" }
    pnpm --filter "@lyflow/mcp" test
    if ($LASTEXITCODE -ne 0) { throw "@lyflow/mcp 测试失败" }
  } finally { Pop-Location }
}

if ($picked.Count -eq 0) { Write-Host "`n全链路绿" -ForegroundColor Green }
else { Write-Host "`n点名的几步都绿：$($picked -join '、')" -ForegroundColor Green }
