# ADR-0022：run summary 是 core 产出的一个对象

日期：2026-09-17（M6）　状态：已采纳并实现（C ABI v9）

## 背景

M5 之后的第二次真实任务（xyz-gap-inspector 上一段带 11 个 fallback 的多点位调参，
见 [m6-plan.md](../m6-plan.md) §来源）暴露出同一类问题的三个面：

- 一次 run 结束后留下的只有一串 `node_state` 和一个 `run_finished.status`。
  宿主要自己重建「这次到底成没成」，**做错了两次**：一次把缓存命中的 `skipped`
  当成没跑，一次把被 `acceptsError` 端口接住的失败当成整轮失败。
- 图级输出缺值时 `lyflow_run_outputs` 只给一个 `missing: true`。
  「这个点位本来就不量 flush」与「flush 本该量出来、上游崩了」是两件完全不同的事，
  而它们在 JSON 里长得一模一样。业务侧据此打过错误的 NG。
- 图里有 11 个 `flow.fallback` / `flow.select`，`gap.result_bundle` 只接了 1 个的
  `choice`。想知道「这一帧到底走了哪几条备用路径」只能逐个节点去翻输出。

共同点是：**这些判断需要同时握着全部节点状态、声明输出、结果仓和图的拓扑**，
而这四样只有执行器同时握着。任何在它下游的层（Rust、MCP、编辑器、业务仓库）
重建这份结论，都是在用不完整的信息复现一遍执行器的逻辑 —— 迟早会错，而且每一层错得不一样。

## 决定

### H1　run summary 是 core 产出的一个对象

执行器在 `Run` 结束时构造它，登记进结果仓（与图级输出同生共死），然后才发 `run_finished`。
它有两个出口，内容逐字节相同：

| 出口 | 谁用 |
|---|---|
| `run_finished` 事件的 `summary` 字段 | 前端、MCP、任何在读事件流的人 |
| `lyflow_run_summary(const char* run_id)`（C ABI v9） | 嵌入宿主、Rust 桥接 |

`lyflow_run_summary` 在 run 结束之前返回 `NULL`（而不是 `"{}"`）：
「还没有」与「跑完了、什么都没有」是两件事。`lyflow_run_free` 之后同样返回 `NULL`。

形状：

```jsonc
{
  "runId": "01M2…", "status": "degraded", "durationMs": 31.2,
  "nodes": {
    "n_fit_datum":  { "state": "error", "code": "insufficient_points",
                      "durationMs": 2.1, "outputsAvailable": false },
    "b_n_align_f1": { "state": "skipped", "reason": "not_demanded",
                      "cached": false, "outputsAvailable": false },
    "n_gap":        { "state": "done", "durationMs": 0.4, "cached": true,
                      "outputsAvailable": true }
  },
  "outputs": {
    "gap":    { "state": "value", "node": "n_gap", "port": "gap",
                "type": "Measurement", "elementCount": 1,
                "value": { "kind": "Measurement", "value": 3.52, "ok": true, "unit": "mm" } },
    "flush":  { "state": "failed", "node": "n_flush", "port": "flush",
                "from": "n_fit_datum", "code": "insufficient_points" },
    "bundle": { "state": "inactive", "node": "b_n_bundle", "port": "bundle",
                "reason": "not_demanded" }
  },
  "decisions": {
    "n_fb_line": { "choice": "b", "reason": "io: a 的上游 n_fit_datum 失败",
                   "port": "choice", "type": "FallbackChoice" }
  },
  "contractViolations": []
}
```

`contractViolations` 已经会真的填东西（端口契约见 [ADR-0024](0024-port-contracts-four-kinds.md)）：
执行器在输入绑定时检查 manifest 端口上的 `contract`，违反时该节点 `error`、`code` 是
`contract_violation`，同时在这里留一条 `{ node, port, expected, actual }`。没有违反时是空数组，
而不是这一项不出现 —— 消费方不必分「老 core 没这个字段」与「这一轮没有违反」。

### H2　`status` 三态

按顺序判，第一条命中就定：

1. **`failed`** —— 任一**声明输出**是 `failed`；或者图没声明 `outputs` 且有节点 `error`；
   或者整轮被取消，或者图根本没编译过。
2. **`degraded`** —— 有节点 `error` 或 `cancelled`，但每个声明输出都是 `value` 或 `inactive`。
3. **`ok`** —— 零个节点 `error` / `cancelled`。

「取消」与「没编译过」两条是对 m6-plan H2 的补充：这两种情况下什么都不能信，
报 `degraded` 会让宿主以为「坏了一点点但结果能用」。

**`status` 与 `run_finished.status` 不是一回事，两个都要留着。**
`run_finished.status` 回答「执行器这一轮有没有翻车」，它按 ADR-0016 的裁决走 ——
一个失败被下游的 `acceptsError` 端口全数接住就不算整体失败。带 fallback 的图因此
经常是 `run_finished: ok` + `summary: degraded`：主路径确实炸了（值得报警、值得看日志），
但结果确实量出来了（不该判 NG）。把这两件事压成一个字串，正是业务侧做错的那两次。

### H3　输出三态

对每个图级声明输出 `<名字> → <节点>.<端口>`：

| 状态 | 判据 | 带什么 |
|---|---|---|
| `value` | 结果仓里有这个端口的结果 | `type`、`elementCount`、非点云的 `value` |
| `inactive` | 产出节点 `skipped` + `reason=not_demanded`（惰性分支没被 demand）；或产出节点被静音而那个输出找不到类型兼容的源可以透传；或该节点这一轮压根没进计划 | `reason`：`not_demanded` / `bypassed_no_source` / `not_run` |
| `failed` | 以上都不是 | `from`、`code` |

判定顺序就是表的顺序：先查结果仓，再查产出节点的收尾状态，都不是就是 `failed`。
点云与 Indices 不给 `value`（走二进制通道，D4），只给 `elementCount`。

`value` 是一个对象（`{"kind": "Measurement", "value": 3.52, …}`）而不是裸标量 ——
它就是 `Data::valueJson()` 那一份，与 `node_state.stats.outputs[].value` 和
`lyflow_run_outputs` 的 `value` 完全一致。m6-plan §1 的示意 JSON 里写成 `"value": 3.52`
是简写；真给裸标量的话，同一个字段在三个地方是两种形状。

### `from` 的回溯规则

`failed` 的 `from` 说的是**根因节点**，不是「哪个端口空着」——
后者看名字就知道了，前者才是要查的东西。

从产出节点开始，沿输入边**逆着**做 BFS：

1. 按跳距一层层往上走，产出节点自己是第 0 跳。
2. 每一层先收齐这一层里 `state == "error"` 的候选；有候选就停，取**拓扑序最早**的那个
   （执行计划的节点数组本身是拓扑序，也就是下标最小的那个）。
3. 整棵上游都没有 `error`（比如整轮被取消，全图都是 `cancelled`）时，
   用同一套规则再找一遍 `cancelled`。
4. 还是找不到就退回产出节点自己，`code` 用它自己的错误码，没有就是 `no_result`。

选「最近」而不是「最早的那个错误」，是因为一次运行里可能有几处独立的失败，
而这一维输出只被其中一处影响。选到多个时取拓扑序最早的那个，是为了确定性 ——
同一张图同一份数据两次运行必须给同一个 `from`。

**`nodes` 里保留全部 `error`，一个都不丢。** 回溯只是给「这一维为什么没有」一个入口，
不是在挑一个「主要错误」；想看全部失败去读 `nodes`。

### `root`：`from` 之外再给一个根因（m6-plan §10 第 4 条）

`failed` 输出**同时**给 `from` 与 `root`：

- `from` 还是上面那条规则算出来的最近出错节点，不变。
- `root` 从 `from` 继续沿**失败链**往上游走，取拓扑序最早的那个 `error` 节点
  （`traceRootError`，`core/src/exec/executor.cpp`）。「沿失败链」的意思是只穿过
  `state` 是 `error` 或 `cancelled` 的节点 —— 一个跑成功了的上游不可能是这条失败链
  的一环，从它继续往上会追到另一处无关的失败上去，一遇到 `done`/`skipped` 就停止
  沿那条边继续走。整条链上只有一个节点（`from` 自己）时 `root == from`。
  整棵上游都没有 `error` 时（比如整轮被取消）按同一套规则退而找 `cancelled`；
  两者都没有就退回 `from` 自己，`rootCode` 用 `from` 的 `code`。
- `rootCode` 是 `root` 那个节点的错误码。

**为什么两个都给**：实测里 `from` 常常是 fallback 自己（1 跳）—— 它的 `a` 和 `b` 两路
都失败了，所以它自己也 `error`；而真正要查的根因在 2 跳外的那个拟合节点上。只给 `from`
的话消费者每次都要自己再往上翻一遍（等于自己重实现这条回溯规则）；只给 `root` 的话
「哪个环节把这一维掐断的、离这个输出最近的那一层是谁」又丢了。两个都给，消费者按需取，
`nodes` 里仍然是全部 `error` 一个不丢。

### H4　`decisions` 按 Record 的类型收，不按算子 id

凡是某个节点的某个输出端口产出了一个 `Record` 且 `record.type == "FallbackChoice"`，
就进 `decisions`。`flow.fallback` 与 `flow.select` 的 `choice` 端口现在就是这个类型；
任何算子包新写的决策算子只要照这个类型声明，自动就进 summary，core 一行都不用改。

键是节点 id（同一个节点真有第二个这种输出时退成 `<节点>:<端口>`），
值是那份 Record 的字段原样，外加 `port` 与 `type`。

硬编码算子 id 的那一版会在「gap 包新加一个三选一的决策算子」时静默漏掉它 ——
而漏掉的表现是 `decisions` 少一条，没人会注意到。

## 影响

- **C ABI v8 → v9**。新增一个入口，没有删改。`client.hpp` 的 `kClientAbiVersion`、
  `bridge/src/core_ffi.rs` 的 `ABI_VERSION`、`core/CMakeLists.txt` 的
  `LYFLOW_INSTALL_ABI_VERSION` 三处跟着改。阶段 B 的 `cmake/lyflow.cmake` 版本检查会 FATAL，
  这是设计：宿主必须知道自己在跟哪一版说话。
- **schema**：`execution-event.schema.json` 加 `$defs.summary` 并挂到 `run_finished`。
- **CLI**：`lyflow run --summary` 在 JSON Lines 末尾多一行 `{"kind":"run_summary", …}`；
  `lyflow eval` 每行 `eval_row` **默认不带**这一份，`--summary` 打开（`--no-summary` 还认，
  但已经是 no-op）。一维 bundle 就 6 KB，51 帧 × 8 组参数 2.5 MB 不该是默认值
  （m6-plan §10 第 5 条）。`sweep` / `perturb` 不带 —— 它们每行只报一个标量，summary
  在那里是纯体积。`run --summary` 不受这条影响，一次 run 只多一行，体积不是问题。
- **MCP**：`run_graph` 的返回值以 summary 为主体，`status` / `nodes` / `outputs` /
  `decisions` 直接来自它。`nodes` 因此从数组变成 `id → 状态` 的对象（M5 那一版是数组）。
  `runStatus` 保留 `run_finished` 那个三值，两者不互相顶替。
- **编辑器**：诊断抽屉顶部显示 `status` 与每个图级输出的三态，有决策时列出来。
- **业务仓库**（m6-plan §6，单独 PR）：`LyFlowMeasurer` 改读 summary，
  删掉「两维都量出来了才放过节点报错」那个补丁。

## 明确不做

- **summary 不落库。** 它跟着 run 的结果仓索引一起活、一起死（`lyflow_run_free` 之后就没了）。
  历史 run 的浏览是另一件事，现在没有需求。
- **不改任何执行语义。** 确定性、缓存键、事件流、退出码全部原样。summary 是纯观察，
  它读的每一样东西执行器本来就有。
- **不在 summary 里重复 `errors[]`。** 节点的全部诊断留在 `node_state` 事件里，
  summary 每个节点只给一个 `code`。想看完整诊断读事件流 —— 两处都存一份必然会漂。
