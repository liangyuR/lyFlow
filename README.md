# LyFlow

**把视觉算法组织成可编辑、可执行、可复用的节点图。**

LyFlow 是面向点云与图像处理的视觉流程平台。用节点连接算法、调整参数并查看结果，再将同一张图交给命令行、AI Agent 或业务程序执行。图编辑基于 React，算子与执行引擎运行在 C++，Rust 负责桥接与宿主集成。

[快速开始](docs/getting-started.md) · [文档中心](docs/README.md) · [命令行](docs/cli.md) · [嵌入业务程序](docs/embedding.md) · [MCP / AI 调参](docs/mcp-glue-tuning.md)

## 用 LyFlow 做什么

| 需求 | 能力 |
|---|---|
| 编排视觉流程 | 节点连线、端口类型检查、参数表单、撤销重做、子图与可复用库算子 |
| 调试算法结果 | 点云 3D / 图像预览、连线内容查看、部分运行、运行日志与输出对比 |
| 缩短调参循环 | 内容寻址缓存、只重算受影响的下游、并行执行、C++ 热重载 |
| 管理不同工况 | 顶层图参数、参数配方、参数来源与失配诊断 |
| 批量评估与 AI 调参 | CLI 样本评估、参数扫描、合成位移检查；MCP 冻结试验、预算搜索和候选比较 |
| 接入现有产品 | C ABI DLL、C++ / Rust 客户端、可嵌入 React 编辑器、HTTP 传输契约 |

## 从编辑到交付

```text
编辑节点图 → 校验与预览 → 样本评估 → 保存图与配方 → 在业务程序中执行
                              ↑
                        CLI / MCP 调参
```

**GraphDoc 是统一的图文档。** 编辑器、磁盘文件和执行入口共享同一份模型，UI 运行时状态不写进图。算子的端口、参数、类型与说明来自 C++ 导出的 `OperatorManifest`，新增算子后由编辑器读取描述生成界面。

核心通过 C ABI 供宿主进程加载，负责校验、调度和结果缓存；算法由构建时编入的算子包提供。具体边界见 [架构](docs/architecture.md) 与 [架构决策记录](docs/adr/)。

## 快速开始

当前开发与发包脚本面向 **Windows x64 + PowerShell 7**。先安装 Visual Studio C++ 工具集与 Windows SDK、CMake/Ninja、Git、Rust、Node、pnpm，并完成 vcpkg bootstrap；步骤与版本声明见 [环境准备](docs/getting-started.md)。桌面运行还需要 WebView2 Runtime。

在仓库根目录执行，以下假设 vcpkg 位于 `C:\vcpkg`：

```powershell
$env:VCPKG_ROOT = 'C:\vcpkg'
& "$env:VCPKG_ROOT\vcpkg.exe" install pcl:x64-windows opencv4:x64-windows
pwsh -NoProfile -File scripts/fetch-onnxruntime.ps1
pnpm install --frozen-lockfile
python -m pip install -r scripts/requirements.txt
pnpm dev
```

默认构建包含点云、图像与 ONNX 推理标准包。`pnpm dev` 同时启动桌面应用和 C++ 监视构建；修改 `core/` 或 `packs/` 后增量编译并热重载。首次构建需要准备原生依赖，详细步骤、纯平台模式与浏览器模式见 [快速开始](docs/getting-started.md)。

打开桌面后，在算子面板添加 `gen.synthetic`，运行并查看生成的点云，即可开始一条无需外部数据的流程。

## 选择使用方式

| 方式 | 从哪里开始 |
|---|---|
| 桌面编辑与调试 | `pnpm dev` → [快速开始](docs/getting-started.md) |
| 无界面执行与批量评估 | `pnpm cli:build` → [CLI 指南](docs/cli.md) |
| AI Agent 读图、评估与调参 | [MCP 配置](docs/mcp.md) → [glue 调参流程](docs/mcp-glue-tuning.md) |
| 在 C++ / Rust 中调用算法 | [嵌入指南](docs/embedding.md) → [Windows 运行库与 SDK](docs/deployment/packaging.md) |
| 在 React / Tauri 中嵌入编辑器 | [编辑器包](packages/editor/README.md) → [HTTP 契约](docs/http-transport.md) |

CLI 与桌面共用图校验和执行逻辑，CLI 不依赖 Tauri。构建后可直接检查引擎并运行自己的图：

```powershell
pnpm cli:build
& .\bridge\target\release\lyflow.exe info
& .\bridge\target\release\lyflow.exe run .\my-flow.lyflow.json --summary
```

`my-flow.lyflow.json` 是用编辑器保存的图。stdout 输出 JSON Lines，stderr 输出诊断；完整选项见 [CLI 指南](docs/cli.md)。

MCP 同时使用 HTTP 后端与本地 CLI；配置完成后，先调用 `get_environment` 核对两侧环境，再用 `inspect_graph` 查看参数入口。

## 算子包

| 包 | 处理领域 | 开发默认构建 |
|---|---|---|
| [std-pointcloud](packs/std-pointcloud/README.md) | 点云读写、滤波、分割、拟合、配准与量测 | 启用 |
| [std-image](packs/std-image/README.md) | 图像处理、标定、深度图与点云转换 | 启用 |
| [std-ml](packs/std-ml/README.md) | ONNX Runtime 推理 | 启用 |
| [glue](packs/glue/README.md) | 胶路、胶宽、断胶、边距与判定 | 按需启用 |
| [gap](packs/gap/README.md) | 间隙与段差测量 | 按需启用 |
| [dts](packs/dts/README.md) | 车门胶条面差 | 按需启用 |

开发涂胶流程时，在构建或启动前指定：

```powershell
$env:LYFLOW_PACKS = 'glue'
pnpm core:build
pnpm dev
```

多个领域包用分号分隔，例如 `'gap;dts;glue'`。切换包配置后先停止开发进程、运行 `pnpm core:build` 重新配置，再启动 `pnpm dev`，使监视构建使用新的包组合。Windows 发布产物固定包含三个标准包、`glue` 与 `gap`，与开发默认组合不同。构建开关、外部包和注册方式见 [算子包指南](docs/op-packs.md)；新增算子的实现约定见 [core 开发指南](core/README.md)。

## 开发与验证

| 命令 | 用途 |
|---|---|
| `pnpm core:build` | 构建 C++ core，运行 manifest 自检与 core 测试 |
| `pnpm typecheck` | 检查编辑器和桌面前端类型 |
| `pnpm check` | 运行 core、契约、Rust、CLI、SDK、前端与 MCP 门禁 |
| `pnpm check:glue` / `pnpm check:gap` | 对应领域包的专项门禁 |
| `pnpm e2e` | 启动真实桌面应用进行交互验收 |
| `pnpm e2e:http` / `pnpm e2e:packaged` | 浏览器宿主 / 桌面构建产物验收 |
| `pnpm run package` | 构建 Windows x64 运行库、SDK/CLI、编辑器 tarball 与正式运行库 NuGet 包 |

测试范围与 CI 覆盖见 [测试地图](docs/testing.md)。发布条件、版本契约与下游验收边界见 [发包指南](docs/deployment/packaging.md)。历史验收结果记录在对应文档中，不代表任意新修订已经验证通过。

## 仓库导航

```text
core/                  C++ 执行引擎、C ABI 与 C++ 客户端
packs/                 标准与领域算子包
crates/lyflow-client/   Rust C ABI 客户端
bridge/                Rust 桥接、Tauri 宿主支持与 headless CLI
packages/editor/       可嵌入的 React 节点编辑器
packages/mcp/          Agent 使用的 MCP 服务
app/                   Tauri 桌面前端
examples/              React 宿主与 C++ 消费示例
schema/                图、算子、事件、配方与样本等 JSON Schema
scripts/               开发、验证、发包与端到端验收入口
docs/                  使用指南、接口契约、设计与验收记录
```

所有文档按使用场景整理在 [文档中心](docs/README.md)。功能演进与未完成项见 [Roadmap](docs/roadmap.md)。

## 许可

尚未选定开源许可证，默认保留所有权利。发布资产中第三方依赖的许可随包提供，详见 [发包指南](docs/deployment/packaging.md)。
