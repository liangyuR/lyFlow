# 快速开始

[项目首页](../README.md) · [文档首页](README.md) · [CLI 指南](cli.md)

以下命令均在仓库根目录的 **PowerShell 7** 中执行，开发脚本面向 Windows x64。先完成环境准备，再选择桌面、CLI 或浏览器入口。

## 1. 准备工具链

| 工具 | 要求与版本来源 |
|---|---|
| Visual Studio | 2022，安装 C++ 桌面开发工具集与 Windows SDK |
| CMake / Ninja | 可使用 Visual Studio 随附版本，构建脚本通过 vswhere 定位 |
| PowerShell | 7，命令为 `pwsh`；Windows PowerShell 5.1 不支持 |
| Rust | rustup 按 [rust-toolchain.toml](../rust-toolchain.toml) 安装固定工具链 |
| Node.js | 使用 [.nvmrc](../.nvmrc) 指定的版本 |
| pnpm | 使用 [package.json](../package.json) 的 `packageManager` 指定版本 |
| Python | Python 3，CI 使用 3.12；契约校验依赖见 [requirements.txt](../scripts/requirements.txt) |
| Git | 用于依赖管理与源码修订识别 |
| vcpkg | 完成 bootstrap，目录中存在 `vcpkg.exe` 与 `scripts/buildsystems/vcpkg.cmake` |
| WebView2 Runtime | 运行 Tauri 桌面窗口需要 |

若尚未安装 vcpkg，先将它安装到一个尚不存在的目录；下面以 `C:\vcpkg` 为例，已有安装可跳过这两条命令：

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
& 'C:\vcpkg\bootstrap-vcpkg.bat' -disableMetrics
```

使用 vcpkg 准备标准算子包的 PCL（最低 1.15.1）与 OpenCV，依赖划分见 [算子包指南](op-packs.md)。开发默认查找 `C:\vcpkg`，其他位置通过 `VCPKG_ROOT` 指定：

```powershell
$env:VCPKG_ROOT = 'C:\vcpkg'
& "$env:VCPKG_ROOT\vcpkg.exe" install pcl:x64-windows opencv4:x64-windows
pwsh -NoProfile -File scripts/fetch-onnxruntime.ps1
pnpm install --frozen-lockfile
python -m pip install -r scripts/requirements.txt
```

`std-ml` 默认启用，需要 CPU 版 ONNX Runtime 1.19.2。准备脚本会复用已准备的依赖，必要时下载到 `third_party/onnxruntime/onnxruntime-win-x64-1.19.2`；也可将 `LYFLOW_ONNXRUNTIME_ROOT` 指向包含 `include/` 与 `lib/` 的现有目录。缺失时 CMake 会报错。

以上是日常开发环境。正式发包会按 [packaging/release.json](../packaging/release.json) 准备固定版本依赖，步骤见 [Windows 发包](deployment/packaging.md)。

## 2. 启动桌面编辑器

```powershell
pnpm dev
```

这会启动 core 监视构建、Tauri 和 Vite。首次构建需要编译 C++、Rust 与前端；后续修改 `core/` 或 `packs/` 会增量构建并热重载 DLL。热重载保留画布与撤销栈，取消正在执行的 run 并清空旧 DLL 的结果缓存，见 [ADR-0009](adr/0009-hot-reload-by-copy.md)。

第一次使用可以走一条无需外部数据的流程：

1. 在算子面板搜索 `gen.synthetic`，添加到画布。
2. 运行图，在预览中查看生成的点云。
3. 加入 `filter.voxel_grid`，连接两个节点的 `cloud` 端口。
4. 修改 `leafSize` 后重跑，比较点数与预览结果。
5. 保存为 `.lyflow.json`，供下一次编辑或 CLI 执行。

快捷键、节点调试、子图与参数配方见 [交互清单](interaction-checklist.md)、[库目录](library-dirs.md) 和 [配方指南](recipe.md)。

## 3. 选择算子组合

默认启用 `std-pointcloud`、`std-image`、`std-ml`。领域包需要在启动或构建前设置：

```powershell
$env:LYFLOW_PACKS = 'glue'
pnpm core:build
pnpm dev
```

多个领域包用分号分隔，例如 `'gap;dts;glue'`。切换配置前停止正在运行的开发进程；`pnpm core:build` 会重新配置 `build/core`，再启动监视构建，避免后续热重载使用旧包组合。标准包与领域包的说明见 [文档首页](README.md)。

只开发平台、不需要 PCL / OpenCV / ONNX Runtime 时，可在新的 PowerShell 会话中选择纯平台构建：

```powershell
$env:LYFLOW_STD_PACKS = '0'
Remove-Item -LiteralPath Env:LYFLOW_PACKS -ErrorAction SilentlyContinue
Remove-Item -LiteralPath Env:LYFLOW_OP_PACKS -ErrorAction SilentlyContinue
pnpm core:build
pnpm dev
```

此模式仍需要 vcpkg 工具链，但不需要安装标准包的原生依赖；只包含 `gen.synthetic`、`util.reroute`、`flow.fallback` 和 `flow.select`，不能运行前面需要 `filter.voxel_grid` 的步骤。恢复标准包时，停止开发进程，设置 `$env:LYFLOW_STD_PACKS = '1'`，运行 `pnpm core:build` 后再启动。构建开关的完整语义见 [算子包指南](op-packs.md)。

## 4. 不开桌面，直接执行图

准备完依赖后构建 CLI：

```powershell
pnpm cli:build
& .\bridge\target\release\lyflow.exe --help
& .\bridge\target\release\lyflow.exe info
& .\bridge\target\release\lyflow.exe manifest --check
```

把下面的最小图保存为 `build/quickstart.lyflow.json`（先创建 `build/` 目录），它只使用 core 内置算子：

```json
{
  "schemaVersion": 1,
  "id": "01J8XQZ4K7N3M2R5V8W1YB6TCD",
  "name": "快速开始",
  "nodes": [
    {
      "id": "source",
      "op": "gen.synthetic",
      "params": { "pointCount": 20000, "seed": 7 },
      "ui": { "position": { "x": 0, "y": 0 } }
    }
  ],
  "edges": []
}
```

```powershell
& .\bridge\target\release\lyflow.exe validate .\build\quickstart.lyflow.json
& .\bridge\target\release\lyflow.exe run .\build\quickstart.lyflow.json --summary
```

stdout 是 JSON Lines，stderr 是诊断信息；CLI 构建使用 `--no-default-features`，无需 Tauri 窗口。参数覆盖、配方、批量评估、缓存和退出码见 [CLI 指南](cli.md)。需要一次完整的合成数据演示，可在标准包构建后运行 `pwsh -NoProfile -File scripts/headless-demo.ps1`，演示会在临时目录生成图、库算子和结果文件。

## 5. 浏览器与嵌入开发

只查看和调整界面，可先导出 manifest，再启动浏览器前端：

```powershell
pnpm core:dump
pnpm app:dev
```

此入口使用静态快照，状态栏会显示提示；执行与文件操作仍需要实际后端。仅停止 core 监视构建可用 `pnpm dev --no-core-watch`；单独运行监视构建用 `pnpm core:watch`。

要通过浏览器连接真实 CLI，可在两个终端分别启动：

```powershell
node packages/editor/test-server/server.mjs --port 8787 --root . --cli bridge/target/release/lyflow.exe
```

```powershell
pnpm host:dev
```

`test-server` 是开发用 HTTP 桩服务器；业务服务需要实现 [HTTP 契约](http-transport.md)。React 编辑器、Tauri 宿主以及 C++ / Rust 接入见 [嵌入指南](embedding.md)。Agent 接入先配置 [MCP](mcp.md)，再按 [调参流程](mcp-glue-tuning.md) 检查 HTTP 与 CLI 的引擎身份。

## 验证与常见阻塞

| 现象 | 检查位置 |
|---|---|
| 找不到 PCL / OpenCV | vcpkg 的 `x64-windows` 安装结果与 `VCPKG_ROOT` |
| 找不到 ONNX Runtime | 运行准备脚本，或设置 `LYFLOW_ONNXRUNTIME_ROOT` |
| 找不到领域算子 | 构建前设置 `LYFLOW_PACKS`，再用 `lyflow manifest` 检查实际 DLL |
| 浏览器显示静态快照 | 当前使用 `app:dev`；需要执行时改用 HTTP 宿主或桌面入口 |
| Node / Rust / pnpm 行为不同 | 对照仓库的工具链版本文件 |

完成环境准备后，可用 `pnpm check` 运行全链路门禁。分层检查与交互验收入口见 [测试地图](testing.md)，发布构建门禁与下游验收边界见 [发包指南](deployment/packaging.md)。
