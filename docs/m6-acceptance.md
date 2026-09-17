# M6 验收记录 —— 能被读懂

逐条对着 [m6-plan.md](m6-plan.md) §8 走：**怎么跑 + 实际输出 + 通过/未通过/未验证**。
验收由计划作者独立执行；实现分三个 Opus 子代理两轮完成，每轮落地后先 `LYFLOW_PACKS=gap pnpm check`，
再用真实数据复现，再提交。

分支 `feat/m6-observability`，基于 main `44e3336`：

| commit | 内容 |
|---|---|
| `b044ecb` | M6 计划 |
| `aa701e0` | run summary（ABI v9）与 `lyflow patch`，ADR-0022 / 0023 |
| `762700a` | `lyflow params`、端口契约、`plan` 惰性标记、Record 样例；summary 加 `root`，eval summary 默认关；ADR-0024 |

数据：`tianmu_0904` 的 R1（`lyflow import --kind StandardGap.yml:model` 导出的 56 节点图，12 个 `flow.fallback`、
18 个 `b_*` 节点），点云 `HXMK2A12XTA237802_04-09-2026-09-11-57/device_0/R1`；KUN10 51 帧与点 4 的图沿用 M5。

## 门禁

| 命令 | 结果 |
|---|---|
| `LYFLOW_PACKS=gap pnpm check`（两轮各一次） | 全绿。最终：core doctest 205/205（34083 断言）、三份 schema、`cargo test` 123/123、CLI `--no-default-features`、嵌入 SDK、前端、MCP 45/45 |
| `LYFLOW_PACKS=gap pnpm e2e` | **423/423** 全绿（实现者跑，输出落盘） |
| `LYFLOW_PACKS=gap pnpm e2e:http` | **29/29** 全绿（验收者跑） |
| `pnpm check:gap` | 未跑，本轮没碰 A/B 路径 |

## §8 逐条

### ✅ 1. summary 的 `decisions` 全量、`inactive` 与 `failed` 分开

正常 run（`--summary`，主备两路都喂同一对 PCD）：

```
status ok；outputs gap=value 3.784 / flush=value 2.350 / bundle=value；decisions 12 = 图里 12 个 flow.fallback
```

R5 交叉：11 个 fallback → 11 条 decisions。
「没有 flush 要求的点上 `inactive`」这一条**在这份数据上复现不了**：R1–R6 六张导入图全部声明三维输出。
`inactive` 由 doctest（`test_summary.cpp`）与 cargo 用例覆盖。

### ✅ 2. 三态 status

| 情形 | `--set` | status | outputs | decisions |
|---|---|---|---|---|
| 主路径打断 | `n_fit_base.distThresh=1e-6` | **degraded** | 三维全 value（gap 3.781、flush 2.497） | 12 条，`n_fb_line` 与 `n_fb_quality_base` 选 b |
| 主备都打断 | 再加 `b_n_fit_base.distThresh=1e-6` | **failed** | 三维 failed，`from` = `n_fb_line` / `n_fb_line` / `n_fb_quality_base`，`root` 全为 `n_fit_base` | 10 条（两个报错的 fallback 没产出 choice） |

`run_finished` 在 degraded 情形仍是 `ok`（失败被 `acceptsError` 接住），summary 是 `degraded` —— 这正是要分开的两件事。
`root` 取拓扑序最早的 error（`n_fit_base` 下标 40 < `b_n_fit_base` 45），不做「主路径优先」这类语义判断，保证同图同数据两次运行同一个 `root`。

### ✅ 3. 端口契约第一帧报错

在 `n_load:primary → n_tensor:primary` 之间插一个 `gap.drop_non_finite`（空槽 bug 的现场）：

```
status failed
contractViolations [{"node":"n_tensor","port":"primary","expected":{"elementCount":{"eq":1280}},"actual":{"elementCount":1231}}]
n_tensor: error contract_violation
```

原图同帧：`status ok`，`contractViolations []`。
**偏离**：`gap.profile_tensor` 只声明 `elementCount.eq 1280`，**不声明 `finite`** —— 源码与 doc 都要求它吃带 NaN 空槽的原始剖面，
声明 `finite` 会把唯一正确的输入判成违反。这条把计划 §3 的写法改掉了。
gap 包共 20 个输入端口声明了契约（`profile_tensor` / `roi_from_labels` / `labels_to_cloud` 的 1280、
`labels_from_logits` 的 `shape [2,-1,1280]`、`select_alignment` / `business_rois` / `corner_vertex` / `result_bundle` 的 `recordType`）。
`result_bundle` 六个 Record 入口的 `recordType` 超出「preconditions 里已有的数值不变量」：它是 duck typing 的，接错一份 Record 只会让 bundle 悄悄空几格，而 bundle 是业务侧 `results.csv` 的唯一来源。

### ✅ 4. `lyflow params`

点 4 的图：41 行，`n_fit_l.distThresh` 显示 `explicit 0.8 mm`；9 行 `default` 与 `lyflow manifest` 的默认值逐条对账 0 条不一致
（如 `n_load.layout = "sensor"`、`n_bundle.consistencyMode = "off"`）。`--set n_fit_l.typo=1` 退出码 4。
`explicit` 的判据放在合并迁移之后的 `PlanNode` 上，不读 `RawNode`：v1 的 `count` 在 v2 叫 `keepCount`，读 RawNode 会错报成 `default`。

### ✅ 5. `lyflow patch`

带 12 个 fallback 的图：先 12 处 `--rewire` 把主路径直连，dry-run 差异与真写后 `lyflow diff` **38 行逐字相同**；
再 `--remove-node 'n_fb_*' --remove-node 'b_*'` 删 27 节点 48 边，`validate` 通过；两条各连跑两次，第二次全 no-op、`diff.empty`、不落盘、mtime 不变。
一步到位地删 `b_*` 被拒：`flow.fallback` 的 `b` 是必填输入，12 条 `missing_input`，整体不写。
**计划 §8 原文那条命令走不通，文案已改。** 节点 id 通配大小写敏感（`N_FB_*` 配不上 `n_fb_line`），文件名那份保持不敏感。

### ✅ 6. `plan` 惰性标记

R1 图 56 节点：`lazy` 18 个，`b_*` 18 个，两个集合相等。`demandedBy` 如 `b_n_fit_base → ["n_fb_line:b","n_fb_quality_base:b"]`。
编辑器惰性边虚线与 tooltip 只验到 typecheck / build / e2e，**未在真实界面肉眼确认**。

### ✅ 7. Record 样例

manifest 核对：gap 算子 27 个（计划写 26，实际注册 27），Record 输出端口 16 个，带 `example` 16 个。
其中 11 个从真实 run 裁出，**5 个手工构造**（`corner_vertex.quality`、`groove_joint` 三个 quality、`camera_consistency.quality`）：
手上的图没有这三个算子，字段名与类型照 `compute` 逐字段对，数值是合理量级不是实测。两份样例做了裁剪（`labels` 的 1280 行留 12，`bundle.fits` 留两条）。

### ✅ 8. MCP

用 M5 那套 mcporter 配置（后端 test-server，指向 M6 二进制）：13 个工具在列；`get_params` 对点 4 返回 5 行含 `unit`；
`patch_graph` 默认 dryRun 返回 `patch_result` 与差异；`run_graph` 返回以 summary 为主体（`status ok`、12 条 decisions、三维 value）。
`run_graph` 的 `nodes` 从数组改成对象，是 M5 → M6 的一处返回形状变更，`docs/mcp.md` 已改。
未做完整盲测。

## 与计划不同的决定（已写进 §10）

1. `failed` 输出同时给 `from`（最近）与 `root`（最早）。
2. `eval_row.summary` 默认关：gap 图一维 bundle 6 KB，51 帧 × 8 组参数 2.5 MB 不该是默认。
3. `outputs[].value` 是 `Data::valueJson` 的对象而非裸标量，与 `node_state` / `run_outputs` 同形。
4. `status` 多两条强制 `failed`：整轮取消、图没编译过。
5. `Registry::validate()` 多拦「契约与端口类型不搭」。
6. C ABI 停在 v9 但多了 `lyflow_effective_params` 符号：**这次改动之前编出的 v9 DLL 会 `MissingSymbol` 加载失败**。同一分支内升级，不影响 main 上的 v8 用户。
7. 本仓库的 `lyflow_graph_from_config.py --model` 不产 fallback，带 fallback 的图来自 C++ 导入器。

## 未做

- xyz-gap-inspector 侧（m6-plan §6）：`ApplyParameterPatches` 对匹配不到的 `node_id` 静默 `continue`；`LyFlowMeasurer` 改用 summary 三态；harvest 认 `outputsAvailable`。等用户决定时机，单独 PR。
- MCP 盲测第二轮、`pnpm check:gap`、编辑器三处 UI 的肉眼确认。
- `--add-node` 加不了边（单独用只对源算子成立），等真有人被卡住再加 `--connect`。
