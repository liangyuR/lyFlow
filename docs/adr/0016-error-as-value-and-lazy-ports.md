# ADR-0016：错误作为值，端口可以是惰性的

日期：2026-09-06　状态：已采纳

## 背景

`xyz-gap-inspector` 现场跑的是模型 ROI 路径：一张 ONNX 分割模型给出四个业务框。
模型偶尔认不出来（脏、遮挡、异形件），那时要**回退**到模板 + ICP 那条老路。
接进 LyFlow 之后这条回退必须由图本身表达
（[gap-inspector-integration-design.md](../gap-inspector-integration-design.md) §2.4）。

用现有语义表达不了，卡在两处：

- **上游一失败，下游全被连坐。** 执行器把失败沿边传播成 `cancelled` +
  `upstream_failed`。可是「模型失败」正是回退节点想知道的**信息**，不是它的死因。
- **两条路都会跑。** 编译期就把全图排进拓扑序，模板加载 + 四次 ICP 是几百毫秒，
  而它们九成九的节拍里根本用不上。产线节拍不允许白花这笔钱。

## 决定

**两条新语义，都是输入端口上的声明，不引入新的节点种类。**

```cpp
struct Port {
  ...
  bool acceptsError = false;   // 上游失败 → 本端口收到一个 Error 值，本节点照跑
  bool lazy = false;           // 本端口的上游闭包不进初始计划，被 demand 时才调度
};
```

### `acceptsError`：`Data::Kind::Error`

新增一种 `Data`，载荷就是那条 `Status`（phase / code / message / paramPath / portName）。
它**只**出现在声明了 `acceptsError` 的输入端口上 —— 算子拿到的要么是正常值、
要么是一条错误，没有第三种可能。端口类型表里对应地多了一个 `Error` 类型（红色），
前端因此不必为它写任何特例。

连坐规则跟着改成：某个上游失败时，**逐条输入绑定**判断。落在 `acceptsError` 端口上的
失败被吸收，其余的照旧连坐成 `cancelled`。取消不在此列 —— 用户按了取消就是全场停。

还有一条不那么显然但很重要的：**被完全吸收的失败不让整轮运行失败**。
一个节点的失败若它在计划里的每一个消费者都声明了 `acceptsError`，
`run_finished` 仍然是 `ok`。否则「模型失败 → 回退 → 量出结果」这条正常路径
会以 `status: error`、CLI 退出码 2 收场，业务侧没法把它和真正的失败分开。

### `lazy`：deferred 节点与 `plan_extended`

编译期：**只被惰性路径依赖**的节点标记 `deferred`。判定是逆拓扑序一遍 ——
一个节点若没有消费者，或有一条落在非惰性端口上、通向「要跑的节点」的边，
它就要跑；否则它是 deferred。

deferred 节点进 `Plan`（所以它们有 cacheKey），但不进初始就绪队列，
也不出现在 `run_started` 的 `plan` / `nodes` / `nodeCount` 里。

运行期：算子在 compute 里发现某个惰性端口缺席，返回 `Status::Demand("b")`。
执行器于是：

1. 从该端口的上游往回收集所有还没跑过的节点，
2. 发一条 `plan_extended` 事件（`nodes[]` 与 `run_started.nodes` **完全同构**），
3. 按拓扑序把它们跑完，
4. **重新调用**同一个算子的 compute。

重复次数上限是该算子惰性端口个数 + 1：一个算子最多把每个惰性端口 demand 一遍。

跑到最后还没被 demand 的 deferred 节点，在 `run_finished` 之前各发一条
`node_state: skipped` + `stats.reason = "not_demanded"`。前端把它们画成半透明 ——
与「命中缓存」（`stats.cached`）和「被静音」（`stats.bypassed`）三者一眼可分。

### 惰性闭包的非惰性祖先

一个坑：deferred 节点的上游未必也是 deferred。`n_load` 同时喂模型路径和模板路径，
它是普通节点。若 demand 发生时它还没轮到，`satisfyDemand` 就会撞上一个没跑过的普通节点。

做法是把这类祖先算进**发起 demand 那个节点**的依赖：每个非惰性节点的依赖集合
= 直接上游穿过 deferred 节点之后落到的那圈非惰性节点。这些都是它在 DAG 上的真祖先，
所以不会造环。有了它，`satisfyDemand` 要跑的就只剩 deferred 节点，
而 deferred 节点从不进就绪队列，于是「谁在跑它」永远只有一个答案。
两个 fallback 共用一条备用闭包时，`demandMu_` 加上逐节点的 `done_` 检查保证闭包只跑一遍。

### `flow.fallback` 与 `flow.select`

core 内置两个算子 —— 它们是**平台语义**，不是某个领域的算法：

```
flow.fallback(a: Any acceptsError, b: Any lazy+acceptsError) -> out: Any, choice: Record
flow.select(cond: Any, a: Any lazy, b: Any lazy)             -> out: Any, choice: Record
```

`a` 成功就透传 `a`；否则 demand `b` 并透传 `b`；两条都失败时报 `a` 的错误并附上 `b` 的。
`choice` 是一个 `Record{type:"FallbackChoice"}`，字段 `choice`（`"a"`/`"b"`）与 `reason`，
`gap.result_bundle` 的 `fallback_reason` 由它填。

`b` 同时声明了 `acceptsError`：计划表里只写了 `lazy`，但「两条都失败时要能同时说出两边的原因」
（设计文档 §2.4）要求算子看得见 `b` 的失败，所以两个标志都要。

### 一个附带的类型系统改动：`Port.anyGroup`

原有规则是「一个节点的全部 `Any` 端口共用一个类型变量」。`flow.select` 破了它：
`cond` 是 Measurement/Record，`a`/`b`/`out` 可以是任意类型。
于是 `Port` 多一个 `int anyGroup = 0`，同节点同组的 `Any` 端口才共用类型变量。
它**不进 manifest** —— 前端不需要知道，而多一个可序列化字段就多一份要同步的契约。

## 缓存

deferred 节点照常算 cacheKey，`flow.fallback` 的键因此包含 `b` 那一路上游的键。
结论是：**fallback 命中缓存时不会去 demand 备用闭包**，备用闭包报 `not_demanded`。
这正是想要的 —— 上一次算过了，这次连问都不必问。

## 后果

- ✅ 回退由图表达，业务侧不再需要一段 if/else 去决定跑哪条路。
- ✅ 主路径成功时备用闭包一个 compute 都不调（用计数算子锁死在 `core/tests/test_flow.cpp`）。
- ✅ 错误变成可以被消费的值，往后「失败也要出一份诊断记录」这类需求有地方落。
- ❌ 调度器复杂了一层：`satisfyDemand` 在发起 demand 的那个 worker 线程上**串行**跑闭包。
  闭包内部因此没有并行。备用闭包是几个节点的量级，暂时不值得为它做真正的挂起/恢复调度。
- ❌ `run_started.nodeCount` 不再等于图里的节点数。前端的进度条以「已知节点」为分母，
  `plan_extended` 到达时分母会变大。
- ❌ 事件流多了一种 kind。任何只认五种 kind 的消费方要跟着加一条分支
  （schema 的 `oneOf` 会挡住漏改）。
