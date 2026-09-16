# 门禁：一条命令验完整条链路 —— C++ 编译/算子自检/doctest -> 三份契约对着 schema 校验
# -> cargo test（DLL 加载、执行事件、二进制输出、中文路径）-> 前端 strict typecheck + build。
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

function Step($name) { Write-Host "`n=== $name ===" -ForegroundColor Cyan }

Step "C++ core"
# 标准算子包（ADR-0014）：默认开，LYFLOW_STD_PACKS=0 跑纯平台构建
if ($env:LYFLOW_STD_PACKS -eq "0") { Write-Host "标准包已关：纯平台构建" -ForegroundColor Yellow }
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

Write-Host "`n全链路绿" -ForegroundColor Green
