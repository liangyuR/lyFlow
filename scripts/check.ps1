# 门禁：一条命令验完整条链路 —— C++ 编译/算子自检/doctest -> 三份契约对着 schema 校验
# -> cargo test（DLL 加载、执行事件、二进制输出、中文路径）-> 前端 strict typecheck + build。
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

function Step($name) { Write-Host "`n=== $name ===" -ForegroundColor Cyan }

Step "C++ core"
# 标准算子包（ADR-0014）：默认开，LYFLOW_STD_PACKS=0 跑纯平台构建
$pureStd = $env:LYFLOW_STD_PACKS -eq "0"
if ($pureStd) { Write-Host "标准包已关：纯平台构建" -ForegroundColor Yellow }
& "$PSScriptRoot\build-core.ps1"

Step "manifest vs schema"
$bin = Join-Path $root "build\core\bin"
$exe = Join-Path $bin "lyflow-dump-manifest.exe"
$tmp = Join-Path $env:TEMP "lyflow-manifest-check.json"
[System.IO.File]::WriteAllText($tmp, ((& $exe) -join "`n"), (New-Object System.Text.UTF8Encoding($false)))
python "$PSScriptRoot\validate_schema.py" $tmp (Join-Path $root "schema\operator-manifest.schema.json")
if ($LASTEXITCODE -ne 0) { throw "manifest 不符合 schema" }
# 带上测试算子再导一份（test.param_showcase：14 种参数类型，含 transform / curve 的默认值，param-recipe P2.10）
$env:LYFLOW_TEST_OPS = "1"
try {
  [System.IO.File]::WriteAllText($tmp, ((& $exe) -join "`n"), (New-Object System.Text.UTF8Encoding($false)))
} finally { Remove-Item Env:\LYFLOW_TEST_OPS }
python "$PSScriptRoot\validate_schema.py" $tmp (Join-Path $root "schema\operator-manifest.schema.json")
if ($LASTEXITCODE -ne 0) { throw "带测试算子的 manifest 不符合 schema" }

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
# 图参数的完整规格（param-recipe P1.1）：规格字段复用 manifest 的 paramSpec（跨文件 $ref），
# 老格式 { type?, default, binds, doc? } 同一份样例里也有一条；负例是拼错的字段名，必须被拒。
python "$PSScriptRoot\validate_schema.py" `
    (Join-Path $root "schema\examples\graph-params.example.lyflow.json") `
    (Join-Path $root "schema\graph-doc.schema.json")
if ($LASTEXITCODE -ne 0) { throw "图参数样例不符合 schema" }
python "$PSScriptRoot\validate_schema.py" `
    (Join-Path $root "schema\examples\graph-params.invalid.lyflow.json") `
    (Join-Path $root "schema\graph-doc.schema.json") --expect-fail
if ($LASTEXITCODE -ne 0) { throw "图参数负例居然通过了 schema" }
# 参数面板的全类型示例图（param-recipe P2.10）：transform / curve 的值、子图、完整规格的图参数都在里面
python "$PSScriptRoot\validate_schema.py" `
    (Join-Path $root "examples\param-showcase.lyflow.json") `
    (Join-Path $root "schema\graph-doc.schema.json")
if ($LASTEXITCODE -ne 0) { throw "examples/param-showcase.lyflow.json 不符合 schema" }

Step "配方文件 vs schema"
# 配方的共享夹具（param-recipe P3，schema/fixtures/recipes/）：每个配方文件按 expected.json 的 schemaValid
# 该过的过、该拒的拒（缺 graph 的那份 schema 不认，编辑器照样读、报失配 ④）；index.json 与夹具图也各查一遍。
# 失配报告本身两边各断言一遍：Rust（bridge/src/recipe.rs，下面 cargo test 那一步）与 @lyflow/editor 的单测（frontend 那一步），
# 对着同一份 expected.json（摘要、条目、建议与文案逐字）。
$fixtures = Join-Path $root "schema\fixtures\recipes"
$expected = Get-Content (Join-Path $fixtures "expected.json") -Raw -Encoding UTF8 | ConvertFrom-Json
foreach ($prop in $expected.recipes.PSObject.Properties) {
  $file = Join-Path $fixtures "graph.recipes\$($prop.Name)"
  if ($prop.Value.schemaValid) {
    python "$PSScriptRoot\validate_schema.py" $file (Join-Path $root "schema\recipe.schema.json")
  } else {
    python "$PSScriptRoot\validate_schema.py" $file (Join-Path $root "schema\recipe.schema.json") --expect-fail
  }
  if ($LASTEXITCODE -ne 0) { throw "配方夹具 $($prop.Name) 与 schema 的预期不符" }
}
python "$PSScriptRoot\validate_schema.py" (Join-Path $fixtures "graph.recipes\index.json") (Join-Path $root "schema\recipe-index.schema.json")
if ($LASTEXITCODE -ne 0) { throw "配方夹具 index.json 不符合 schema" }
python "$PSScriptRoot\validate_schema.py" (Join-Path $fixtures "graph.lyflow.json") (Join-Path $root "schema\graph-doc.schema.json")
if ($LASTEXITCODE -ne 0) { throw "配方夹具的图不符合 graph-doc schema" }

Step "片段文件 vs schema"
# 包随附的片段（m8-plan L14）。算子与端口对不对由 core 的启动自检查（manifest --check），
# 这里只查文件形状 —— 没编进本次构建的包，它的片段也照样要合格。
foreach ($snippet in Get-ChildItem (Join-Path $root "packs\*\snippets\*.lyflow-snippet.json")) {
  python "$PSScriptRoot\validate_schema.py" $snippet.FullName (Join-Path $root "schema\snippet.schema.json")
  if ($LASTEXITCODE -ne 0) { throw "片段 $($snippet.Name) 不符合 schema" }
}

Step "Rust bridge"
Push-Location (Join-Path $root "bridge")
try {
  cargo test --quiet
  if ($LASTEXITCODE -ne 0) { throw "cargo test 失败" }
} finally { Pop-Location }

Step "Rust 客户端：脱开 bridge 单独构建"
# 这个 crate 的卖点是宿主不必参与 core 的构建（docs/embedding.md「Rust 客户端」）。
# 单独编一遍是那句话的最低证据：它要是又依赖回 bridge 或 core 的构建产物，这一步就断了。
Push-Location (Join-Path $root "crates\lyflow-client")
try {
  cargo build --quiet
  if ($LASTEXITCODE -ne 0) { throw "lyflow-client 单独构建失败" }
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

Step "嵌入 SDK：安装布局 + 独立消费方工程"
# A1-11：cmake --install 的产物必须能被一个只 find_package(lyflow) 的外部工程用起来。
# 装到临时前缀而不是 build/install，免得本地开发时那一份被 CI 的跑法覆盖。
$prefix = Join-Path $env:TEMP "lyflow-install-check"
if (Test-Path $prefix) { Remove-Item -Recurse -Force $prefix }
# CLI 用上一步刚构建好的那份 debug 产物：让门禁顺手 cargo build --release 会产出
# 一个更新的 lyflow.exe，而 A/B 脚本按 mtime 挑，会挑到不带算子包的那一个。
& "$PSScriptRoot\install-lyflow.ps1" -Prefix $prefix -SkipBuild `
    -CliPath (Join-Path $root "bridge\target\debug\lyflow.exe")
if ($LASTEXITCODE -ne 0) { throw "安装布局产出失败" }

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
$cmake  = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja  = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if (-not (Test-Path $cmake)) { $cmake = "cmake" }
if (-not (Test-Path $ninja)) { $ninja = "ninja" }

$consumerSrc   = Join-Path $root "examples\consumer"
$consumerBuild = Join-Path $env:TEMP "lyflow-consumer-build"
if (Test-Path $consumerBuild) { Remove-Item -Recurse -Force $consumerBuild }
# 消费方工程刻意不带 vcpkg 工具链：它只该依赖安装目录，不该依赖 LyFlow 的构建环境。
$line = "call `"$vcvars`" >nul 2>&1" +
        " && `"$cmake`" -S `"$consumerSrc`" -B `"$consumerBuild`" -G Ninja" +
        " -DCMAKE_MAKE_PROGRAM=`"$ninja`" -DCMAKE_BUILD_TYPE=RelWithDebInfo" +
        " -DCMAKE_PREFIX_PATH=`"$($prefix -replace '\\', '/')`"" +
        " && `"$cmake`" --build `"$consumerBuild`""
cmd /c $line
if ($LASTEXITCODE -ne 0) { throw "examples/consumer 编译失败" }

& (Join-Path $consumerBuild "embed_minimal.exe")
if ($LASTEXITCODE -ne 0) { throw "embed_minimal 跑合成图失败" }

Step "frontend"
Push-Location $root
try {
  # 包先单独过一遍 strict typecheck：app 那一遍是顺着 import 走的，
  # 漏掉的文件（比如只有宿主才用的入口）在这里才会暴露。
  pnpm --filter "@lyflow/editor" typecheck
  if ($LASTEXITCODE -ne 0) { throw "@lyflow/editor 类型检查失败" }
  pnpm --filter "@lyflow/editor" test
  if ($LASTEXITCODE -ne 0) { throw "@lyflow/editor 单测失败" }
  pnpm --filter lyflow-app build
  if ($LASTEXITCODE -ne 0) { throw "前端构建失败" }
  pnpm --filter lyflow-host-react build
  if ($LASTEXITCODE -ne 0) { throw "examples/host-react 构建失败" }
} finally { Pop-Location }

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

# 纯平台构建里要标准包算子的用例不算通过，只是没跑：cargo 记 ignored、node --test 记 skip。
if ($pureStd) {
  Write-Host ("`n纯平台构建：要标准包算子的 Rust 测试记为 ignored、MCP 集成冒烟记为 skip（数目见上面两处汇总行）。" +
              "这一趟只证明 core 零依赖，不替代默认的 pnpm check") -ForegroundColor Yellow
}
Write-Host "`n全链路绿" -ForegroundColor Green
