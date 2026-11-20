# 测试地图

[文档首页](README.md) · [环境准备](getting-started.md) · [Windows 发包](deployment/packaging.md)

本页说明当前验证入口和覆盖边界，不记录某次修订的通过结论。测试数量、跳过项、耗时及产物身份，以对应提交的实际输出为准。

## 公共工具门禁

先按[快速开始](getting-started.md)恢复 SDK，安装 pnpm 和 Python 依赖。

| 入口 | 验证内容 |
| --- | --- |
| `pnpm check` | 校验随包 manifest 和公共图、事件、配方等 schema 夹具。构建独立客户端、CLI、桌面和 React 示例，运行 Rust、编辑器、可视化与 MCP 测试。 |
| `./scripts/check.ps1 -Steps schema,frontend` | 单独选择 manifest、schema、bridge、client、cli、frontend、mcp 步骤。MCP 集成测试需要先构建 debug CLI。 |
| `cargo test --locked --manifest-path bridge/Cargo.toml` | SDK DLL 的加载、ABI 封送、命令行执行、缓存、宿主注入与 Python 节点。跨进程缓存按首次运行实际落盘的节点核对命中。 |
| `pnpm --filter @lyflow/editor test`、`pnpm --filter @lyflow/visualization test` | 图操作、撤销、标签、运行状态、图像和点云显示。React 18/19 的类型检查见编辑器的 `typecheck`。 |
| `pnpm --filter @lyflow/mcp test` | MCP 工具、资源、HTTP/CLI 的真实运行结果与调参流程。指定本地 CLI 时，集成冒烟的依赖缺失会直接报错。 |
| `pnpm e2e:packaged --only noderun:suiteSmart` | 从构建产物暂存独立目录，启动 Tauri/WebView2，执行点云图并验证单节点运行和缓存。桌面及 CLI 须已构建，分组选择见[交互验收](../scripts/e2e/README.md)。 |
| `pnpm e2e:packaged --only params_p4` | 在真实桌面保存配方，让同一安装目录的 CLI 运行已保存的图，核对三个输出、参数覆盖优先级和不同宽度的窗口布局。 |
| `pnpm e2e:http` | 浏览器中的外部 React 宿主、HTTP 传输、结果取数。 |
| `pnpm run package` | 从锁定的原生 SDK 构建并组装公共工具。`./tests/release/test-public-tools.ps1 -AssetsDir <目录>` 从成包运行 P4/P5、外部 C++17 EXACT 与依赖拒绝用例、Profile/P6、场景图和外部 React 宿主。见[发包说明](deployment/packaging.md)。 |

公共 PR 和 main 的 CI 检查格式、公共契约、前端，以及在 Windows x64 匿名恢复 SDK 后运行 Rust、CLI、MCP 与真实桌面。Rust 构建缓存只用于公共客户端和工具。SDK 采用版本锁指定的原始 ZIP。

## 原生引擎与下游验收

[引擎仓](https://github.com/liangyuR/lyflow-engine)维护 core、kernels、标准与场景包、Python worker、原生依赖与运行库制包。在该仓运行 CMake / CTest、Windows C ABI 和 C++17 EXACT 宿主、Linux 视觉门禁、Python 单元测试及运行库 NuGet 契约。算法数值和退役配置的诊断由对应包的测试覆盖。公共仓的 `schema/fixtures`、宿主和发布测试覆盖公共客户端行为。

公共工具交付后，从正式地址恢复的 SDK、桌面和下游的升级回退按[拆仓计划](deployment/runtime-sdk-repo-split-plan.md)验收。模型、设备、现场检测准确率和生产节拍由对应业务宿主独立验证。

## 结果应如何解释

- 同时记录源码修订、包组合、依赖/数据前提、执行入口和失败或跳过项。旧提交的通过记录不能代替当前代码验证。
- 缺包、缺模型、缺数据要明确报失败或说明跳过，不能让分组提前返回后仍显示全部通过。
- e2e 的启动、等待及必要前提用 `mustOk`；行为断言要说明真正验证的结果，避免只断言元素存在。
- 现场精度、标定单位、客户数据效果及产品节拍，需要对应宿主和数据的独立验证。基准、模拟结果或历史算法对拍不能自动转成现场结论。

