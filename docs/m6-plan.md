# M6 实施计划 —— 能被读懂

目标：**靠读 JSON 干活的人和 Agent，不用自己重建「这次 run 到底发生了什么」，也不用手改 JSON 做结构实验。**
一条都不改执行语义：确定性、缓存键、事件流、退出码全部原样。M6 只加可观测性与结构编辑。

前提：M5 已落地（eval / perturb / MCP）。范围来自第二次真实任务的复盘（xyz-gap-inspector 上一段带 11 个
fallback 的多点位调参，见 §来源）。

---

## 来源：第二次真实任务的证据

| 观察 | 核实结果 | 对 M6 的含义 |
|---|---|---|
| run 结束只留一串 `node_state` 和一个 `status` 字串，宿主自己重建成败判定，做错了两次 | `run_finished` 只有 status；输出缺失只有 `missing: true`，分不开「本来就没这一维」与「本该有、崩了」 | **P0：结构化 run 收尾** |
| `gap.result_bundle` 只接了 1 个 fallback 的 choice，图里有 11 个；拿 `roi_source` 当「模板用没用过」连错两次 | 是。`flow.fallback` / `flow.select` 各自产出 `FallbackChoice` Record，没人汇总 | summary 全量收集决策 |
| 结构实验（删节点、短接 fallback）要复制数据库手改 JSON，为此写了两个脚本 | `--set` 只改参数，没有结构编辑命令 | **`lyflow patch`** |
| 参数稀疏存储，「现在生效值是多少」要与 manifest join | 是 | **`lyflow params`** |
| 「`--set` 参数名拼错静默无效」 | **不成立**。CLI 与 `run_start` 走同一个 `buildPlan`，报 `unknown_param`，退出码 1。静默的是业务仓库 `ApplyParameterPatches` 对匹配不到的 `node_id` 直接 `continue` | LyFlow 侧不动；业务仓库修一处 |
| 「模板分支每帧白跑 3 次 ICP」判断错误，翻源码才知 `b` 是 lazy | manifest 端口**已有** `lazy` 字段；`plan` 输出与编辑器没显示 | `plan` 标惰性、编辑器虚线 |
| Record 端口按 `data.inlierCount` 取到 None，翻实现才知形状 | Record 只有 `type` 字串 | 端口 `example` |
| 「同一张图跑 N 组输入」循环写了六遍 | **M5 `eval` 已做**，缺的是每样本吐 P0 对象 | 并进 P0 |
| 空槽 bug：「1280 槽」不变量散在三处散文里，2327 项失败后才 grep 到 | `preconditions` 是给人读的字串 | **端口契约**，第一帧报错 |

---

## 0. 定死的决定

| # | 决定 | 一句话理由 |
|---|---|---|
| H1 | **run summary 是 core 产出的一个对象**，C ABI 加 `lyflow_run_summary(run_id)`；`run_finished` 事件带同一份；`lyflow run --summary`、`eval` 每样本行、MCP `run_graph` 都原样透出 | 「发生了什么」只能由掌握全部节点状态的那一层说，消费者重建必错 |
| H2 | `status` 三态：`ok` 零错误；`degraded` 有节点 error/cancelled 但所有声明输出都是 `value` 或 `inactive`；`failed` 任一声明输出 `failed`，或图没声明输出且有节点 error | 宿主的成败判定变成一行 |
| H3 | 输出三态：`value`（带值或 elementCount）/ `inactive`（上游 `not_demanded` 或 bypass 无源）/ `failed`（带 `from` = 最近的出错节点与其 code） | 「本来就没有」与「本该有、崩了」是两件事 |
| H4 | `decisions` 收集**所有类型为 `FallbackChoice` 的 Record 输出**，不按算子 id 硬编码 | `flow.fallback` / `flow.select` 已经产出这个类型；任何包的决策算子照类型声明就自动进 summary |
| H5 | `lyflow patch` 动作集第一版四个：`--remove-node`、`--add-node`、`--rewire`、`--set`；**幂等**、`--dry-run` 输出 `lyflow diff` 同格式的结构差异、每步之后过 `validate` 形状校验，不过就整体不写 | 两个脚本本质都是这四个动作；幂等 + dry-run 是「改 → 全量重跑 → 证明只有 N 格动了」这种工作法的前提 |
| H6 | `lyflow params <graph>` 给每节点每参数的生效值与来源 `default \| explicit \| bound`（子图提升） | 稀疏存储是对的，但要有一个 join 好的视图 |
| H7 | **端口契约**是 manifest 端口上的可选 `contract`，只有四种：`elementCount: {eq\|min\|max}`、`finite: true`、`shape: [..]`、`recordType`；运行时在输入到达时检查，违反报 `contract_violation`（带期望与实际），进 summary | 与 `visibleWhen` 刻意做弱是同一条原则；表达式语言说明算子该拆 |
| H8 | Record 端口加可选 `example`（一份样例 JSON），不上 JSON Schema | 一份样例就能消掉「每个新端口一次试错」，schema 的维护成本现在不值 |
| H9 | `plan` 输出加 `lazy: true` 与 `demandedBy`；编辑器把惰性边画成虚线，`not_demanded` 的节点半透明（已有） | manifest 早就有 `lazy`，缺的只是显示 |
| H10 | **不做**：执行语义任何改动、historical run 浏览、summary 落库、契约表达式 | 这次所有建议都是补可观测性 |

H1–H4 合写 ADR-0022（run-summary-as-core-output）；H5 一份 ADR-0023（patch-as-idempotent-structural-edit）；H7 一份 ADR-0024（port-contracts-four-kinds）。H6、H8、H9 是小改。

---

## 1. run summary

```jsonc
{
  "runId": "01M2…", "status": "degraded", "durationMs": 31.2,
  "nodes": {
    "n_fit_datum":  { "state": "error", "code": "insufficient_points", "durationMs": 2.1, "cached": false },
    "b_n_align_f1": { "state": "skipped", "reason": "not_demanded" },
    "n_gap":        { "state": "done", "durationMs": 0.4, "cached": true }
  },
  "outputs": {
    "gap":   { "state": "value", "value": 3.52, "node": "n_gap", "port": "gap" },
    "flush": { "state": "failed", "from": "n_fit_datum", "code": "insufficient_points" },
    "bundle":{ "state": "value", "elementCount": 1, "node": "n_bundle", "port": "bundle" }
  },
  "decisions": {
    "n_fb_line": { "chose": "b", "reason": "a 的上游 n_fit_datum 失败" }
  },
  "contractViolations": [
    { "node": "n_tensor", "port": "primary", "expected": { "elementCount": { "eq": 1280 } }, "actual": { "elementCount": 1230 } }
  ]
}
```

- core：`Run` 结束时由执行器构造（它手上有全部节点状态、声明输出、结果仓）；`failed` 输出的 `from` 沿边回溯到最近的 error 节点。
- C ABI v9：`lyflow_run_summary(const char* run_id)` 返回 JSON；`run_finished` 事件加 `summary` 字段（同一对象）。schema `execution-event.schema.json` 加 `summary` 定义。
- CLI：`run --summary` 在 JSON Lines 末尾多一行 `{"kind":"run_summary", …}`；`eval` 的 `eval_row` 加 `summary`（可用 `--no-summary` 关掉以省体积）。
- MCP：`run_graph` 返回值改为以 summary 为主体（`nodes` / `outputs` / `decisions` 直接来自它），`eval` 的失败样本清单带 `summary.outputs`。
- 编辑器：诊断抽屉顶部显示 `status` 三态与 `outputs` 三态；`decisions` 列表。

## 2. `lyflow params`

```
lyflow params <graph> [--node <id>]... [--only explicit|default|bound] [--set ...] [--json]
```

每行 `{ node, op, param, value, source, unit?, min?, max? }`；`--set` 先应用再解析，所以「这组 `--set` 之后生效值是什么」一条命令。
未知节点 / 参数沿用 `unknown_node` / `unknown_param`，退出码 4。MCP 工具 `get_params`。

## 3. 端口契约

manifest 端口：`"contract": { "elementCount": { "eq": 1280 }, "finite": true }`。
四种键之外的任何键 → manifest 自检失败（`lyflow manifest --check`）。
执行器在输入绑定时检查（bypass 与 Error 值透传不检查），违反 → 该节点 `error`，code `contract_violation`，
`message` 带期望与实际，`portName` 指向端口。summary 的 `contractViolations` 汇总。
gap 包：`gap.profile_tensor.primary` 声明 `elementCount.eq = 1280` 与 `finite: true`（空槽 bug 的不变量）；
其余算子读 `preconditions` 里已有的数值不变量，能写成四种之一的都写上。
编辑器：Inspector 在端口 doc 旁显示契约。

## 4. `lyflow patch`

```
lyflow patch <graph> [--remove-node <id|glob>]... [--add-node <json>]... [--rewire <from>=<to>]...
                     [--set <node>.<param>=<json>]... [--dry-run] [-o <out>] [--json]
```

- `--remove-node b_*`：删节点与其所有边；glob 只对 id。
- `--add-node '{"id":"x","op":"…","params":{…}}'`：id 撞了报错。
- `--rewire n_fb_line:out=n_fit_base:line`：所有从左端口出发的边改为从右端口出发（与 perturb 的图手术同一函数）。
- `--set` 同 run。
- 应用顺序固定：remove → add → rewire → set。每步后过一遍结构校验，最后过 `validate`；任一步失败**整体不写**，退出码 1 并打出全部诊断。
- **幂等**：`--remove-node` 对不存在的 id 是 no-op（stderr 提示）；`--rewire` 对已经指向右端口的边是 no-op；`--set` 同值 no-op。再跑一遍 `diff` 为空。
- `--dry-run`：不写文件，stdout 输出 `lyflow diff` 同格式的差异。
- `-o` 省略时原地覆写；图级 `outputs` 指向被删节点时报错而不是静默删输出。
- MCP 工具 `patch_graph`（`dryRun` 默认 true）。

## 5. `plan` 惰性标记与 Record 样例

- `plan` 每节点加 `lazy: true`（它只被惰性端口依赖）与 `demandedBy: [<node>:<port>]`。
- 编辑器：惰性边虚线；边的 tooltip 写「惰性：主路径成功时不跑」。
- manifest 端口加可选 `example`；gap 包所有 Record 输出端口（`quality`、`choice`、`bundle`、`alignment` 等）各给一份样例，从一次真实 run 的输出裁出来。Inspector 端口详情可展开样例。

## 6. 业务仓库（xyz-gap-inspector）

- `ApplyParameterPatches`：`node_id` 匹配不到任何节点 → 返回错误，不再 `continue`。
- `LyFlowMeasurer` 改用 summary：`status` 三态直接映射成败，删掉「两维都量出来了才放过节点报错」那个补丁；`outputs.flush.state == inactive` 映射 `kInactive`。
- harvest 认 `outputsAvailable`（M5 遗留）。

## 7. 实现顺序（每步 `LYFLOW_PACKS=gap pnpm check` 绿）

1. §1 summary（core + ABI v9 + schema + CLI + eval + MCP + 编辑器），ADR-0022
2. §2 params + §3 契约（core 检查 + gap 包声明 + Inspector），ADR-0024
3. §4 patch（复用 perturb 的图手术），ADR-0023
4. §5 plan 惰性 + Record 样例
5. 文档：`agent-tuning.md` 加「先看 summary」一节与 patch / params 用法；`docs/architecture.md` 的运行模式段补 summary
6. §6 业务仓库（单独 PR）

## 8. 验收（M6 完成的定义）

数据沿用 M5 的 51 帧 + 19 张图，外加带 fallback 的模型路径图（`packs/gap/tools/lyflow_graph_from_config.py --model` 生成）。

- [ ] 模型路径图一次 run：summary 的 `decisions` 列出图里**全部** `flow.fallback` / `flow.select` 节点的选择（数量与图里的节点数相等），`outputs.flush` 在没有 flush 要求的点上是 `inactive` 而不是 `failed`
- [ ] 人为让 `n_fit_datum` 失败（`--set` 一个不可能的阈值）：`status = degraded` 当 gap 仍有值且 flush 是 inactive；`status = failed` 当 flush 有要求
- [ ] `gap.profile_tensor` 喂 1230 点：第一帧报 `contract_violation`，消息含 1280 与 1230，summary 的 `contractViolations` 有它；51 帧正常数据 0 条违反
- [ ] `lyflow params` 对点 4：`n_fit_l.distThresh` 显示 `explicit 0.8`，未改过的参数显示 `default` 与 manifest 默认值一致
- [ ] `lyflow patch --remove-node 'b_*' --rewire … --dry-run` 对模型路径图输出的差异与手改 JSON 的 `lyflow diff` 一致；连跑两次第二次 diff 为空；删掉被图级输出引用的节点报错不写
- [ ] `plan` 对模型路径图标出 lazy 节点数 = `b_*` 节点数；编辑器里对应边为虚线
- [ ] 26 个 gap 算子的 Record 输出端口都有 `example`，schema 校验过
- [ ] `pnpm check`、`pnpm e2e`（带 `LYFLOW_PACKS=gap`，输出落盘再 grep）全绿；`e2e:http` 绿
- [ ] MCP：`run_graph` 返回 summary，盲测那套 mcporter 配置下 `get_params` / `patch_graph(dryRun)` 可调

## 9. 风险

- **`failed` 输出的 `from` 回溯**在多输入汇合处可能有多个候选，取拓扑序最早的那个，并在 `nodes` 里保留全部 error，不丢信息。
- **契约检查的开销**：`finite` 是 O(n) 一遍；只在声明了契约的端口上做，默认零开销。
- **`patch` 与编辑器的 UI 布局**：新增节点没有坐标，放在被删节点或 rewire 目标附近；这是 UI 字段，不影响执行。
- **ABI 升 v9**：`client.hpp` 与业务仓库钉的版本号要跟着改，阶段 B 的 `cmake/lyflow.cmake` 检查会 FATAL，这是设计。

## 10. 已确认的决定（2026-09-17）

1. 端口契约只做等值、上下界、finite、shape 四种，不做表达式。
2. `patch` 第一版四个动作：remove-node、add-node、rewire、set。
3. 业务侧「未知参数静默」已查清：LyFlow 侧硬失败成立，静默在业务仓库的 `ApplyParameterPatches` 丢弃不匹配的 `node_id`，列入 §6。
