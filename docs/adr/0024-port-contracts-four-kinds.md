# ADR-0024：端口契约只有四种键

日期：2026-09-17（M6）　状态：已采纳并实现

## 背景

第二次真实任务里的「空槽 bug」：`gap.profile_tensor` 要的是**原始 1280 槽**剖面（保留 NaN 空槽，
不能被 `dropNonFinite` 提前删掉），这条不变量当时散在三处散文里 —— 算子的 `doc`、
`preconditions`、以及业务侧代码的注释。三处都是给人读的字串，没人在第一帧查它。
结果是下游拟合算子在 2327 项失败之后，靠 grep 才找到「原来这里要求 1280 槽」这句话。

`OperatorDesc::preconditions`（M5，见 `docs/op-packs.md`）解决的是「这个算子什么时候整个不该用」，
是自由文本，Agent 可以按它过滤算子，但它不会在运行时拦住任何东西。而空槽 bug 需要的是反过来
的东西：一个**能在数据到达的那一刻就检查**的不变量。

## 决定

manifest 的端口上加一个可选的 `contract`，只有四种键：

```jsonc
"contract": {
  "elementCount": { "eq": 1280 },   // 或 { "min": n } / { "max": n } / min+max；eq 与 min/max 互斥
  "finite": true,                    // 只能写 true
  "shape": [2, -1, 1280],            // 张量形状，-1 = 这一维随便
  "recordType": "GapLabels"          // Record 的 type 字串
}
```

- `elementCount` 的口径：点云 = 点数，Indices = 下标个数，Tensor = 元素总数，其余类型固定为 1
  （`Data::elementCount()`）。
- `finite` 只对 PointCloud（坐标）/ Tensor（全部元素）/ Measurement（value）有意义，声明在别的
  类型上是 manifest 自检失败；同理 `shape` 只对 Tensor、`recordType` 只对 Record 有意义。
- **为什么只有四种、为什么不做表达式**：与 `Param::visibleWhen` 同一条原则 —— 需要更复杂判断
  的时候，通常说明这个算子该拆了。表达式语言要配一个求值器、一套错误信息、一份文档，而它换来
  的表达力在这四条之外几乎没有真实用例。四种之外的任何键都让 `Registry::validate()` 拒掉
  （`validatePortContract`，`lyflow manifest --check` 与 startup 自检走的是同一个函数），
  schema 里也是 `additionalProperties: false`。
- **在输入绑定时检查**，不是算子自己查：值一到端口上就查（`Executor::collectInputs`，
  在类型匹配之后、`compute` 之前），第一帧就报，而不是流进算子之后变成一个「拟合失败」。
  违反 → 该节点 `error`，`code` 是 `contract_violation`，`message` 带期望与实际，`portName`
  指向那个端口；同时进 run summary 的 `contractViolations`（`{ node, port, expected, actual }`，
  ADR-0022）—— `Status` 只留得下一句话，而消费者要的是「期望 vs 实际」这一对结构化的值。
- **两种情况不查**：静音（bypass）节点的透传（它只是搬运，不声称自己算得对）、
  Error 值（那条端口上流的是失败本身，不是数据；ADR-0016 里只有 `acceptsError` 端口才会
  收到 Error 值）。
- **默认零开销**：没声明 `contract` 的端口一次遍历都不做（`checkPortContract` 第一行就是
  `contract 为空对象则 return true`）。`finite` 是 O(n) 一遍，只在声明了它的端口上跑。
- **输出端口也可以声明契约**，但执行器只在输入绑定时检查；写在输出上是给读图的人看
  「这一维保证是什么」，不产生额外的运行时检查。
- `contractViolations` 按拓扑序（再按端口名）排，同一张图同一份数据两次运行给出逐字节一样
  的 JSON。没有违反时是空数组，而不是这一项不出现。

## 影响

- **schema**：`operator-manifest.schema.json` 的 `$defs.portContract`（`additionalProperties: false`，
  四个键各自的形状校验，`elementCount.eq` 与 `min`/`max` 互斥）；`execution-event.schema.json` 的
  `summary.contractViolations`。
- **core**：`lyflow/contract.h` + `contract.cpp`（`validatePortContract` 供 `Registry::validate()`
  调用，`checkPortContract` 供执行器调用）；`lyflow/manifest.h` 的 `Port::contract` /
  `Port::example` 字段与 `withContract` / `withExample` 两个构造辅助函数（`Port` 是聚合初始化，
  尾部字段要么全填要么不填，这两个函数省得每个算子都重写前面几个 `false`/`0`）。
- **执行器**：`Executor::collectInputs` 新增契约检查这一步，产出的 `ContractViolation` 由
  事件汇集器（`contractViolation()` / `contractViolations()`）收集，按拓扑序重排后写进
  run summary 的 `contractViolations`。
- **编辑器**：`OperatorDetail.tsx` 在端口 doc 旁以 `describeContract()` 渲染契约的人话摘要
  （`port__contract`），完整 JSON 挂在 `title` 提示上。
- **C ABI 不变**（仍是 v9，端口契约全部走 manifest，没有新增/删改任何 C ABI 入口）。
- **gap 包**：20 个输入端口声明了契约。写的时候有一条与 m6-plan §3 不一样 ——
  计划里 `gap.profile_tensor` 要同时声明 `elementCount.eq = 1280` 与 `finite: true`，
  但那个算子要的恰恰是**保留了 NaN 空槽**的原始剖面（`load` 时把 `dropNonFinite` 关掉），
  声明 `finite` 会把它唯一正确的输入判成违反。**契约照源码写，不照计划写** ——
  一条与实现对不上的契约比没有契约更糟，它会在第一帧拦住本来对的那份数据。

## 明确不做

- **表达式**。参见「为什么只有四种」。
- **契约推断**。不从算子实现反推契约 —— 契约是算子作者的声明，不是工具替他猜的。
- **把 `preconditions` 机械地翻译成契约**。写不成这四种之一的数值不变量继续留在
  `preconditions` 里，那是给人读的，两者互补，不互替。
