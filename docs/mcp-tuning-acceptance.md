# MCP 调参实现验收（2026-10-08）

基于合并 glue 的 main `df2e477`，实现位于 `codex/mcp-ai-tuning`。范围是 [实现计划](mcp-tuning-plan.md) 的完整 P0/P1/P2；使用说明见 [glue MCP 流程](mcp-glue-tuning.md)。

## 按要求核对

| 要求 | 实现与当前证据 |
|---|---|
| 图参数一致与绑定冲突 | argv/graph 测试；smoke 的 recipe + graphParams.count=123 经 validate/plan/run/get_params/inspect/eval/patch 一致，绑定字段改写有 param_conflict 提示；Rust 验证逐样本覆盖最高优先级 |
| 环境与图检查 | get_environment 对 HTTP/CLI manifest、core 指纹与包；inspect_graph 从 CLI/core 读取生效值、绑定、角色、单位和依赖。smoke 检查 compatible/buildVerified、breakMin 入口与 judge acceptance 角色 |
| 复杂输出与封存 | Record/Bundle 选字段/分页/归约/分布/缺失计数与 artifact；外层分页保留内层点坐标；run.test 验证原文件变化后旧 artifact 保持原值；复杂摘要有界并标记截断 |
| 图像叠画 | Image + 同 run 的 overlay2d，原图 ROI 与 origin/pixelScale；cloud.test 验证映射与像素，真实 MCP smoke 返回 ROI PNG 与标签 |
| 真值评分与分组 | Rust tests 覆盖执行成功但定位/产品失败、null/单位不符、混合 px/mm 行与 eval/quality 两种汇总拒绝、按单位分组恢复可比误差、重复预测的一对一匹配、FN/FP、原图真值投影失败、数组 p95；run.test 验证固定工件分组、顺序无关、多帧不泄漏，以及 tolerance 默认门槛和显式通过率。CLI 另给微平均、误放/误拒率及缺失原因 |
| 冻结评估与比较 | 图、参数、配方、样本/真值、split、缓存策略、输入 SHA-256、manifest/core 指纹与 CLI 可执行文件 SHA-256；smoke 修改输入后续跑被拒，改 acceptance 后比较被拒，并以确定点数验证改善/退化方向 |
| 候选与证据导出 | get_evaluation 的失败 replay 经 run_graph + ROI overlay 回放；validation/holdout 评估与基线比较；导出新图和顶层配方及 provenance，smoke 验证图合法、配方可运行 |
| 受限搜索与生命周期 | one_factor/grid、组合上限、角色/规格/显式参数锁定、数值敏感度；smoke 的 maxRuns=1 到预算耗尽，再追加预算续跑；test.stall 后台任务实际开始后 cancel_job 终止并收尾 |
| 流式与时间限制 | 完整 JSONL 立即落盘，EOF 尾行处理、stderr 上限；cli.test 验证保存后回调、取消/超时保留部分行且结束进程；HTTP 取消确认与执行结束分别报告；cold/warm 策略被冻结 |
| 文档与机器契约 | MCP/CLI 对照、随动/飞拍样本和流程、sample/overlay 资源、测试地图、operator metadata 类型/schema；schema 正例与负例进入 check.ps1，MCP 资源实际可读取 |

## 验证结果

本机 Windows / PowerShell 7，`LYFLOW_PACKS=glue`。项目钉住的 Rust 1.97.1 安装缺 rustc，本次显式使用完整的 stable Rust 1.96.1；构建环境 bootstrap 仅作用于调用进程。原生构建和 HTTP/WS 集成测试使用受限的主机执行权限。

| 检查 | 结果 | 日志（工作区本地） |
|---|---|---|
| MCP typecheck / build / node:test | 35 通过、0 失败、0 跳过；dist 已构建 | `work/mcp-tests-host.log`、`work/mcp-build.log` |
| editor metadata 类型检查 | 通过 | `work/mcp-editor-typecheck.log` |
| Rust 默认 bridge/CLI | lib 155 通过；disk_cache 2 通过；host 11 通过、2 项 dts 条件测试 ignored；两条示例 doctest ignored | `work/mcp-rust-desktop-tests.log` |
| Rust headless CLI | 120 项库测试与 2 项缓存测试通过；最终 --no-default-features --bin lyflow 构建通过，info/帮助已核对 | `work/mcp-rust-host-tests.log`、`work/mcp-rust-build.log` |
| schema / 实际 manifest | 正负例与两份实际 manifest 通过 | `work/mcp-schema-tests.log` |
| C++ core + glue 全量 | 214 项中 212 通过、2 项 ASCII PCD 对照失败；算子清单自检通过 | `work/mcp-core-verify.log`、`work/mcp-manifest-check.log` |
| 未修改 main 的 PCD 基线复现 | 独立源码副本只选择该两项，均失败，同为 24 个断言失败；其余 190 项未运行 | `work/mcp-baseline-build.log`、`work/mcp-baseline-pcd-tests.log` |
| 工作区 diff | 无空白错误；CRLF/LF 提示不算测试失败 | `git diff --check` |

Rust 首轮并发验证遇到原有一秒取消测试的 1.032 秒阈值失败；避开并发 C++ 编译、以 `--test-threads=1` 重跑完整 Rust 默认组合后全部通过，没有放宽测试阈值。

C++ 的两项失败分别是 `packs/std-pointcloud/tests/test_io_pcd.cpp` 的 ASCII PCD 写/读与 PCL 对照。未修改 main 独立复现了相同失败，本次没有改动这些算法；不能把全量 C++ 检查写成全绿。新增 metadata 和 glue 测试在上述 212 项通过范围内。

## 验收边界

完整闭环的工具调用来自 MCP 客户端；测试准备阶段生成合成 PNG 与图，不借助 shell 实现工具步骤。断口数值、量测和失败语义另有底层合成夹具与标注正反例。

本次证明接口、参数语义、评分、搜索控制、记录和回放可用。真实产线模板、stations、标定文件和现场质量阈值没有在本次验收中提供；不把合成图的零误报或故意设置的失败真值当成现场漏检率。未重跑的纯平台/dts/安装包/桌面 UI 组合不计为本次通过。
