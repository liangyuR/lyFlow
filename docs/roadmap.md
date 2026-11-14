# 当前范围与后续边界

[文档中心](README.md) · [架构](architecture.md) · [测试地图](testing.md)

本页只保留当前能力和明确的后续边界，不再复述已完成的 M0–M8 阶段与逐轮测试数字。实现和兼容约束以当前指南、源码及所用构建的 manifest 为准；历史计划与验收可通过 Git 历史追溯。

## 当前能力

| 范围 | 当前入口 |
|---|---|
| 多工作流标签、参数面板、选区运行、结果预览与量测 | [交互指南](interaction-checklist.md) |
| C++ 调度、缓存、子图、惰性分支、端口契约与结果仓 | [架构](architecture.md) · [ADR](adr/) |
| CLI 批处理、样本评估、灵敏度分析与 MCP 工具 | [CLI](cli.md) · [Agent 调参](agent-tuning.md) · [MCP](mcp.md) |
| 参数配方、静态算子包、库算子和 Python 节点 | [配方](recipe.md) · [算子包](op-packs.md) · [库目录](library-dirs.md) · [Python](python-nodes.md) |
| C++ / Rust / Tauri / React 嵌入与交付 | [嵌入](embedding.md) · [发包](deployment/packaging.md) |

## 仍需独立推进或验收的范围

- **同一页面的多个编辑器实例**：当前 store 和 Transport 仍为模块级单例，一页只支持一个编辑器。编辑器内部的多工作流标签已经实现，不等于实例隔离。支持方式见[嵌入指南](embedding.md)。
- **业务宿主迁移**：xyz-gap-inspector、GlueSight 等仓库的数据库、采集、生产 UI 和部署由各自仓库负责。LyFlow 平台能力已实现不代表这些系统已完成迁移；量测准确率、误检漏检和生产节拍需要业务数据独立验收，不能以旧算法逐字段一致代替正确性验证。
- **现场数据与产品判据**：真实多相机选取、现场标定、完整 NG 集和生产质量指标不能用合成图或单次离线回放替代。已有算子的前提和未验证边界见 [glue 接入](glue-integration.md)与 [gap 接入](gap-inspector-integration-design.md)。
- **跨会话缓存**：现有磁盘结果缓存仅由 CLI 显式启用；编辑器跨会话复用、自动配额和跨机器共享不是现有承诺，参见 [CLI 缓存](cli.md#跨进程缓存)。工作区自动备份与计算结果缓存是不同能力。
- **外部算子补齐**：[198 项能力对照](operator-coverage.md)已更新 P0 实现：通用值与有界值列表、二维几何、图像区域统计与直线卡尺，见[源码分析与示例](p0-operator-analysis.md)。[后续计划](operator-completion-plan.md)继续推进三维处理、标定和模型任务，机器人与设备专项独立验收。原生工件/云集合和部分高级模式仍缺失，不构成外部流程兼容承诺。

新增需求应先明确数据、接口和验收条件，再形成可执行任务；不继续在本页累计已完成的实施步骤。
