# LyFlow

**把视觉算法组织成可编辑、可执行、可复用的节点图。**

LyFlow 是面向点云与图像处理的视觉流程平台。用节点连接算法、调整参数并查看结果，再将同一张图交给命令行、AI Agent 或业务程序执行。图编辑基于 React，算子与执行引擎运行在 C++，Rust 负责桥接与宿主集成。

[快速开始](docs/getting-started.md) · [文档中心](docs/README.md) · [命令行](docs/cli.md) · [嵌入业务程序](docs/embedding.md) · [MCP / AI 调参](docs/mcp-glue-tuning.md)

## 用 LyFlow 做什么

| 需求               | 能力                                                                                               |
| ------------------ | -------------------------------------------------------------------------------------------------- |
| 处理视觉数据       | 三维点云集合/配准、位姿、标定、NCC 定位与 YOLOv8/11/26 检测；[支持范围](docs/operator-coverage.md) |
| 编排视觉流程       | 节点连线、端口类型检查、参数表单、撤销重做、子图与可复用库算子                                     |
| 调试算法结果       | 点云 3D / 图像预览、连线内容查看、部分运行、运行日志与输出对比                                     |
| 缩短调参循环       | 内容寻址缓存、只重算受影响的下游、并行执行、C++ 热重载                                             |
| 管理不同工况       | 顶层图参数、参数配方、参数来源与失配诊断                                                           |
| 批量评估与 AI 调参 | CLI 样本评估、参数扫描、合成位移检查；MCP 冻结试验、预算搜索和候选比较                             |
| 接入现有产品       | C ABI DLL、C++ / Rust 客户端、可嵌入 React 编辑器、HTTP 传输契约                                   |

## 从编辑到交付

```text
编辑节点图 → 校验与预览 → 样本评估 → 保存图与配方 → 在业务程序中执行
                              ↑
                        CLI / MCP 调参
```

**GraphDoc 是统一的图文档。** 编辑器、磁盘文件和执行入口共享同一份模型，UI 运行时状态不写进图。算子的端口、参数、类型与说明来自 C++ 导出的 `OperatorManifest`，新增算子后由编辑器读取描述生成界面。

核心通过 C ABI 供宿主进程加载，负责校验、调度和结果缓存；算法由构建时编入的算子包提供。具体边界见 [架构](docs/architecture.md) 与 [架构决策记录](docs/adr/)。

## 快速开始

Windows x64 开发环境需要 PowerShell 7、MSVC C++ 工具集与 Windows SDK、Rust、Node 和 pnpm。桌面需要 WebView2 Runtime。首次检出后在仓库根目录执行：

```powershell
$env:LYFLOW_NATIVE_SDK = ./scripts/restore-native-sdk.ps1 -Destination build/native-sdk
pnpm install --frozen-lockfile
pnpm cli:build
& ./bridge/target/release/lyflow.exe run "$env:LYFLOW_NATIVE_SDK/examples/p2-expression.lyflow.json" --summary
pnpm dev
```

示例的命名输出 `mean` 为 13 mm。新开 PowerShell 时，将 `LYFLOW_NATIVE_SDK` 指回已恢复的 SDK 绝对路径。依赖版本与桌面操作见[快速开始](docs/getting-started.md)。

## 选择使用方式

| 方式                          | 从哪里开始                                                                           |
| ----------------------------- | ------------------------------------------------------------------------------------ |
| 桌面编辑与调试                | `pnpm dev` → [快速开始](docs/getting-started.md)                                     |
| 无界面执行与批量评估          | `pnpm cli:build` → [CLI 指南](docs/cli.md)                                           |
| AI Agent 读图、评估与调参     | [MCP 配置](docs/mcp.md) → [glue 调参流程](docs/mcp-glue-tuning.md)                   |
| 在 C++ / Rust 中调用算法      | [嵌入指南](docs/embedding.md) → [Windows 运行库与 SDK](docs/deployment/packaging.md) |
| 在 React / Tauri 中嵌入编辑器 | [编辑器包](packages/editor/README.md) → [HTTP 契约](docs/http-transport.md)          |

CLI 与桌面共用图校验和执行逻辑，CLI 不依赖 Tauri。构建后可直接检查引擎并运行自己的图：

```powershell
pnpm cli:build
& .\bridge\target\release\lyflow.exe info
& .\bridge\target\release\lyflow.exe run .\my-flow.lyflow.json --summary
```

`my-flow.lyflow.json` 是用编辑器保存的图。stdout 输出 JSON Lines，stderr 输出诊断；完整选项见 [CLI 指南](docs/cli.md)。

MCP 同时使用 HTTP 后端与本地 CLI；配置完成后，先调用 `get_environment` 核对两侧环境，再用 `inspect_graph` 查看参数入口。

## 算子包

Windows x64 正式 SDK 包含七个标准包与 Gap、DTS、Glue 三个场景包。工具读取 SDK 的算子清单。每个包的算子和示例见[文档中心](docs/README.md)，原生开发及包组合由[引擎仓](https://github.com/liangyuR/lyflow-engine)维护。

## 开发与验证

`pnpm check` 检查 SDK 契约、Rust、前端、CLI 与 MCP。`pnpm e2e:packaged` 验证桌面构建产物，`pnpm e2e:http` 验证浏览器宿主。`pnpm run package` 组装含 CLI 的完整 SDK 和编辑器 tarball。见[测试地图](docs/testing.md)与[发包指南](docs/deployment/packaging.md)。

## 仓库导航

```text
packs/                 标准包说明
solutions/             场景包说明、图与示例
crates/lyflow-client/   Rust C ABI 客户端
bridge/                Rust 桥接、Tauri 宿主支持与 headless CLI
packages/editor/       可嵌入的 React 节点编辑器
packages/mcp/          Agent 使用的 MCP 服务
app/                   Tauri 桌面前端
examples/              React 宿主与 C++ 消费示例
schema/                图、算子、事件、配方与样本等 JSON Schema
scripts/               开发、验证、发包与端到端验收入口
docs/                  当前指南、接口契约、接入边界与架构决策
```

所有文档按使用场景整理在 [文档中心](docs/README.md)。当前范围与后续边界见 [Roadmap](docs/roadmap.md)。

## 许可

尚未选定开源许可证，默认保留所有权利。发布资产中第三方依赖的许可随包提供，详见 [发包指南](docs/deployment/packaging.md)。
