# LyFlow 文档中心

[项目首页](../README.md) · [快速开始](getting-started.md) · [CLI 指南](cli.md) · [嵌入指南](embedding.md)

使用指南说明当前入口与接口；设计计划解释实现选择；验收记录保留对应修订的证据与未验证项。阅读当前能力时，先从下方指南进入，再按需查看历史记录。

## 按任务查找

| 我要做什么 | 阅读顺序 |
|---|---|
| 第一次运行 LyFlow | [环境与启动](getting-started.md) → [交互清单](interaction-checklist.md) |
| 在终端跑图、扫参数、评估样本 | [CLI 指南](cli.md) → [Agent 调参方法](agent-tuning.md) |
| 让 AI Agent 调试与调参 | [MCP 配置和工具](mcp.md) → [glue 调参流程](mcp-glue-tuning.md) |
| 管理图参数和工况 | [GraphDoc](graph-doc.md) → [参数配方](recipe.md) |
| 接入 C++ / Rust / Tauri / React | [嵌入指南](embedding.md) → [编辑器包](../packages/editor/README.md) → [HTTP 契约](http-transport.md) |
| 开发或引入新算子 | [算子包机制](op-packs.md) → [core 开发指南](../core/README.md) → [算子描述](operator-manifest.md) |
| 构建交付资产 | [Windows x64 发包](deployment/packaging.md) → [测试地图](testing.md) |
| 理解架构与演进 | [架构](architecture.md) → [ADR](adr/) → [Roadmap](roadmap.md) |

## 接口与数据契约

| 文档 | 内容 |
|---|---|
| [架构](architecture.md) | 编辑器、宿主、Rust 桥接、C++ core 与算子包的职责及数据流 |
| [GraphDoc](graph-doc.md) | 图、节点、连线、顶层参数与子图的序列化模型 |
| [OperatorManifest](operator-manifest.md) | 算子注册、端口类型、参数控件与校验约束 |
| [JSON Schema](../schema/README.md) | 图、事件、算子、配方及样本等机器可校验的契约 |
| [嵌入指南](embedding.md) | C ABI、C++ / Rust 客户端、运行输入、输出与宿主装配 |
| [HTTP 传输](http-transport.md) | REST、WebSocket、结果载荷与后端实现约定 |
| [参数配方](recipe.md) | 配方文件、默认值、覆盖顺序与失配处理 |
| [库算子目录](library-dirs.md) | 桌面、CLI 与宿主共用的库目录配置 |

## 算子与业务接入

开发默认启用三个标准包，领域包通过 `LYFLOW_PACKS` 按需选择。Windows 发布组合以 [发布配置](../packaging/release.json) 为准。

| 包 / 场景 | 入口 |
|---|---|
| 点云与通用量测 | [std-pointcloud](../packs/std-pointcloud/README.md) |
| 图像、标定与深度图 | [std-image](../packs/std-image/README.md) |
| ONNX 推理 | [std-ml](../packs/std-ml/README.md) |
| 涂胶检测 | [glue](../packs/glue/README.md) · [业务接入](glue-integration.md) · [MCP 调参](mcp-glue-tuning.md) |
| 间隙 / 段差 | [gap](../packs/gap/README.md) · [接入设计](gap-inspector-integration-design.md) |
| 车门胶条面差 | [dts](../packs/dts/README.md) |
| 外部包与构建开关 | [算子包指南](op-packs.md) |

## 开发、测试与交付

- [快速开始](getting-started.md)：依赖准备、桌面 / CLI / 浏览器、纯平台构建与常见阻塞。
- [测试地图](testing.md)：按目的选择检查入口，区分单测、交互验收、CI 与发布门禁。
- [E2E 脚本说明](../scripts/e2e/README.md)：真实桌面与浏览器的驱动方式、分组运行。
- [Windows 发包](deployment/packaging.md)：固定依赖、运行库 / SDK、版本、草稿 Release 与消费验收。
- [C++ core](../core/README.md)、[Rust 桥接](../bridge/README.md)、[桌面前端](../app/README.md)：各模块的开发约定。
- [React 编辑器](../packages/editor/README.md)：组件 API、Transport、宿主样式与依赖去重。

## 设计与验收档案

下面是功能演进资料。计划中的条目不等于已实现，历史通过数量也不等于当前修订通过；以配套验收中的范围、日期和未验证项为准。总进度见 [Roadmap](roadmap.md)。

| 阶段 | 计划与验收 |
|---|---|
| M2：执行与结果仓 | [计划](m2-plan.md) · [验收](m2-acceptance.md) |
| M3：缓存、并行与热重载 | [计划](m3-plan.md) · [验收](m3-acceptance.md) |
| M4：子图、CLI 与预览 | [计划](m4-plan.md) · [验收](m4-acceptance.md) |
| M5：Agent 调参与 MCP | [计划](m5-plan.md) · [验收](m5-acceptance.md) |
| M6：summary、结构修改与端口契约 | [计划](m6-plan.md) · [验收](m6-acceptance.md) |
| M7：参数与校验约束 | [计划](m7-plan.md) · [验收](m7-acceptance.md) |
| M8：积木、片段与 ROI | [计划](m8-plan.md) · [验收 A](m8a-acceptance.md) · [验收 B](m8b-acceptance.md) · [验收 C](m8c-acceptance.md) |
| 嵌入阶段 A / B | [A 计划](phase-a-plan.md) · [A1 验收](phase-a1-acceptance.md) · [A2 验收](phase-a2-acceptance.md) · [B 计划](phase-b-plan.md) |
| 图像域 | [计划](image-plan.md) · [验收](image-acceptance.md) |
| 大图与抢占 | [计划](large-image-plan.md) · [验收](large-image-acceptance.md) |
| 涂胶检测 | [计划](glue-plan.md) · [MCP 调参计划](mcp-tuning-plan.md) · [MCP 调参验收](mcp-tuning-acceptance.md) |
| 标准算子包 | [计划](std-pack-plan.md) · [验收](std-pack-acceptance.md) |
| 节点运行 | [计划](node-run-plan.md) · [验收](node-run-acceptance.md) |
| 参数面板与配方 | [研究](param-recipe-research.md) · [计划](param-recipe-plan.md) · [P1](param-recipe-p1-acceptance.md) · [P2](param-recipe-p2-acceptance.md) · [P3](param-recipe-p3-acceptance.md) · [P4](param-recipe-p4-acceptance.md) |
| 动效 | [计划](motion-plan.md) · [验收](motion-acceptance.md) |
| 易用性与性能 | [验收](usability-perf-acceptance.md) |
| 业务图迁移 | [KUN10 验收](kun10-graphs-migration-acceptance.md) |

其他专项设计：[结果 Bundle](result-bundle-plan.md)、[通用量测](measure-plan.md)、[Edge Peek](edge-peek-plan.md)、[输出对比](compare-plan.md)、[库算子内联](library-inline-plan.md)、[逐帧锚点](pointfrom-plan.md)、[磁盘缓存研究](disk-cache-research.md)与[计划](disk-cache-plan.md)、[多实例研究](multi-instance-research.md)。

架构决策集中在 [ADR 目录](adr/)，包括 GraphDoc、C ABI、缓存、静态算子包、编辑器分包、MCP、run summary 和图像预览等约定。
