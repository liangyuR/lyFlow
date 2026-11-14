# LyFlow 文档中心

[项目首页](../README.md) · [快速开始](getting-started.md) · [当前范围与后续边界](roadmap.md)

这里维护当前使用方法、接口契约和接入边界。已完成阶段的实施计划、临时调研和逐轮验收记录不再保留在文档树中，需要追溯时查 Git 历史；验证当前修订请使用[测试地图](testing.md)。架构决策保留在 [ADR](adr/) 中，决策当时的版本和数字不代表当前构建状态。

## 使用与调试

| 我要做什么 | 入口 |
|---|---|
| 准备环境、启动桌面或浏览器宿主 | [快速开始](getting-started.md) |
| 编辑多工作流、连线、运行选区、预览和量测 | [交互指南](interaction-checklist.md) |
| 批量运行、扫描参数与评估样本 | [CLI 指南](cli.md) · [Agent 调参](agent-tuning.md) |
| 配置 AI Agent 工具 | [MCP 服务](mcp.md) · [glue 调参](mcp-glue-tuning.md) · [结果可视化](mcp-visualization.md) |
| 管理工况、参数覆盖与算子库 | [参数配方](recipe.md) · [库目录](library-dirs.md) |
| 编写自定义 Python 节点 | [Python SDK 与运行环境](python-nodes.md) |

## 契约与嵌入

| 文档 | 内容 |
|---|---|
| [架构](architecture.md) | 编辑器、宿主、Rust 桥接、C++ 执行器和算子包的职责 |
| [GraphDoc](graph-doc.md) | 图、连线、顶层参数、命名输出和子图 |
| [OperatorManifest](operator-manifest.md) | 算子描述、端口、参数控件与校验 |
| [JSON Schema](../schema/README.md) | 可由机器校验的图、事件、配方和样本格式 |
| [嵌入指南](embedding.md) | C ABI、C++ / Rust 客户端、输入注入与 React / Tauri 宿主 |
| [React 编辑器](../packages/editor/README.md) | 组件属性、Transport、依赖与宿主样式 |
| [HTTP 契约](http-transport.md) | REST、WebSocket、结果载荷与快照 |

## 算子与业务接入

实际算子由构建选项、库目录和 Python 节点配置共同决定，以当前引擎的 `lyflow manifest` 为准。开发和发布组合不同，发布组合见 [release.json](../packaging/release.json)。

| 领域 | 入口 |
|---|---|
| 点云、拟合、配准与通用量测 | [std-pointcloud](../packs/std-pointcloud/README.md) |
| 图像、标定与深度转换 | [std-image](../packs/std-image/README.md) |
| ONNX 推理 | [std-ml](../packs/std-ml/README.md) |
| 涂胶检测 | [glue 包](../packs/glue/README.md) · [接入指南](glue-integration.md) · [测量算子契约](glue-measure-ops.md) |
| 间隙与段差 | [gap 包](../packs/gap/README.md) · [宿主接入](gap-inspector-integration-design.md) |
| 车门胶条面差 | [dts 包](../packs/dts/README.md) |
| 开发新算子或外部包 | [算子包机制](op-packs.md) · [core 开发指南](../core/README.md) |
| 算子资源与性能约束 | [缓存、结果读取与推理资源](operator-optimization.md) |

## 外部库能力参考

[xyz-vision-lib 算子清单](<xyz-vision-lib 算子清单.md>)是用户提供的外部库快照，标注修订为 `72d7885c`。原文的算子数量、分类和描述属于该外部库，未在本仓库逐项核验；不表示 LyFlow 已实现相同能力，也不保证流程、端口或参数兼容。它用于后续能力对照，LyFlow 当前契约仍以上面的 manifest、包指南和源码为准。

## 开发与交付

- [测试地图](testing.md)：本地检查、CI 覆盖及仍需独立验收的范围。
- [E2E 驱动](../scripts/e2e/README.md)：真实桌面与浏览器验收的运行方式。
- [Windows 发包](deployment/packaging.md)：固定依赖、运行库 / SDK / 编辑器资产、版本和 ABI 门禁。
- [版本变更](CHANGELOG.md)：发布行为与兼容要求。
- 模块说明：[core](../core/README.md)、[bridge](../bridge/README.md)、[app](../app/README.md)、[MCP](mcp.md)。

## 追溯旧资料

源码和测试注释中的 M 编号、R 编号等是历史实现标识，不是待执行计划。需要原始说明时可运行 `git log --all -- docs/<旧文件名>`，再用 `git show <修订>:docs/<旧文件名>` 查看；清理前的完整文档树位于提交 `7f414a1b84d7806bca5cfb45ba9a0efb0dfe0032`。ADR 中需要支撑决策的历史来源链接已固定到该修订。
