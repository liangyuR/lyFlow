# ADR-0021：MCP 服务是 HTTP 契约的消费方，不是第四种传输

日期：2026-09-16（M5）　状态：已采纳并实现（`packages/mcp`）

## 背景

M5 的目标是「一个只拿得到 CLI 或 MCP、拿不到仓库源码的 Agent，能独立完成给一组测点设计图并调稳参数」
（[m5-plan.md](../m5-plan.md)）。CLI 那一半在 [ADR-0020](0020-eval-and-perturb-as-cli.md) 里定了。
剩下的问题是：Agent 要看现场数据、要连阶段 B 的业务服务，**这一段该怎么接**。

仓库里已经有三种 `Transport` 实现（[packages/editor/src/transport/](../../packages/editor/src/transport/)）：
Tauri（桌面 IPC）、HTTP（[docs/http-transport.md](../http-transport.md)）、static（只读快照）。
每一种都是同一组语义的一次重新表达，每加一种，「改一个端点要改几处」就多一处。

## 决定

### G12　`packages/mcp` 对着 `/lyflow/*` HTTP 契约说话，外加本地起 CLI 的 eval / perturb

```
Agent ── stdio ── @lyflow/mcp ──┬── HTTP + WS ── /lyflow/*（test-server 或业务服务）── core
                                └── spawn ────── lyflow.exe eval / perturb / diff ── core
```

### 为什么不是第四种传输

**因为它没有新语义。** MCP 要的每一样东西 —— manifest、validate、plan、run + 事件、输出元信息、
二进制点云 —— HTTP 契约里全都有，且那份契约与 C ABI v7 一一对应。
再写一个 `McpTransport` 意味着同一组语义有第四份表达、第四处要跟着契约变更改。

反过来，做成消费方立刻拿到两件事：

1. **同一个二进制既能接 test-server，也能接阶段 B 的业务服务。** 换后端只改 `LYFLOW_HTTP_BASE`。
   Agent 因此能对着现场数据调参，而不是只能对着合成图。
2. **契约只有一份会坏。** MCP 与编辑器踩同一条路径，编辑器的 e2e 顺带在验 MCP 依赖的那些端点。

代价是老实说清楚的：MCP 拿不到只有桌面才有的东西（文件对话框、库算子保存、热重载帧）。
那些本来也不是 Agent 的工作单元。

### 为什么不依赖 `@lyflow/editor`

`@lyflow/editor` 里已经有一份写好的 `HttpTransport`，直接 import 看着最省事。不这么做的原因：

- **它带 React。** 包的 `main` 是 `src/index.ts`，导出面里有组件、store、hooks；
  peerDependencies 是 react / react-dom / @xyflow/react / three / zustand。
  一个 stdio 服务为了几个 `fetch` 调用去背这棵树，装机体积与故障面都不划算。
- **拆一个 `@lyflow/transport` 子包的收益不够。** MCP 实际用到的只有
  manifest / core-info / validate / plan / run / outputs / clouds 七个端点，
  加上一条 WebSocket。为它做一次包拆分，要动编辑器的构建与导出面，改的地方比省下的多。
- **两边的形状本来就不一样。** 编辑器要的是流式事件推给 store 逐帧渲染；
  MCP 要的是「一次调用一个答案」：连上 WS、发 run、等这个 `runId` 的 `run_finished`、
  收完就关。共用一份实现反而要在里面塞两套生命周期。

所以 `packages/mcp` 直接按 [docs/http-transport.md](../http-transport.md) 实现，
用 Node 的全局 `fetch` 与 `WebSocket`，依赖只有 SDK + zod。
这份契约文档因此从「给业务服务照着实现的规格」变成了**有两个独立实现在盯着的规格** ——
写歪了会有人先撞上。

### 为什么 eval / perturb 走本地 CLI 而不是 HTTP

ADR-0020 已经把它们定成 CLI 子命令，理由在那边。这里只补三条 MCP 侧的：

- **HTTP 契约里没有它们，也不该有。** 一次 eval 是 816 次 run，中间要共享进程内的结果仓。
  把它塞进 REST 要么变成长连接的第二套事件流，要么变成一个带轮询的作业接口 ——
  两样都是新语义，回到了「不发明第四种」这条线的反面。
- **它们本来就要读本地文件。** 样本集 JSON Lines、`--samples-glob`、csv 输出全在跑 Agent 的那台机器上。
  绕一圈 HTTP 反而要先把文件送过去。
- **输出天然是 JSON Lines。** CLI 的 stdout 就是结果流，MCP 只需按行解析、挑出 summary、
  把逐行结果落盘。stderr 掺进来的人话行跳过而不是崩 —— 这件事做对一次，
  就省掉了复盘里那个「每个人都自己写一遍 JSON Lines 解析器」。

代价：`LYFLOW_CLI` 与 `LYFLOW_HTTP_BASE` 可能指向不同版本的 core。
`eval` 的返回里带 `argv`，出入能对得上；真要较真是部署纪律的事，不是接口的事。

### 为什么输出要裁

**Agent 每一次工具调用都在花上下文。** 不裁的话：

| 原样返回 | 大小 | 裁成 |
|---|---|---|
| 50 个算子的完整 manifest | 约 60 KB | 每算子一行、doc 只留第一句，约 1.5 KB |
| 一次 run 的全部事件 | 几十条，含 plan 与 progress | 每节点一行状态 + 图级输出，约 400 字节 |
| 一片 5779 点的点云 | 92 KB 二进制 | 点数 + 包围盒 + 每通道 min/max/mean + 前 8 个点，约 900 字节 |
| 8 组参数 × 51 帧的 `eval_row` | 408 行 | 8 条 `eval_summary` + 失败样本清单，逐行落盘给路径 |

裁的原则是**留判断依据，去原始数据**：

- 列表给 id，详情给全量 —— 包括 `preconditions`。复盘里「什么时候它不成立」比参数列表值钱得多，
  所以 `get_operator` 一个字不删。
- 点云的统计在 Node 侧算，**core 一行不改**（m5-plan §4）。包围盒直接用二进制头里的那一份，
  它是用全量点算的，抽稀不影响。
- 大输出落盘给路径，不进返回值。`eval_row` 是给 `jq` 看的，Agent 需要时自己去读那个文件。
- 诊断与 `preconditions` 一律原样。裁掉它们就等于让 Agent 猜。

### 不做的事（G13）

自动优化器、点云可视化、读写图文件、编辑器内的 AI 面板、core 里的任何 Agent 逻辑。
前两条是范围，后三条是边界：**图文件归 Agent 的文件系统，UI 归编辑器，语义归 core**。

## 后果

- 契约多了一个独立实现盯着，`docs/http-transport.md` 的错会更早暴露。
- `pnpm check` 多一步：`@lyflow/mcp` 的 typecheck / build / test，其中集成冒烟自己起
  test-server 与 MCP 子进程跑一条真链路。
- 工具面**在子代理盲测之前不锁定**（m5-plan §8）。盲测暴露出来的工具再加，没暴露的不加。
- 阶段 B 切到业务服务时，MCP 这边只改一个环境变量。sceneId 注入那条路（ADR-0017）
  等 eval 的样本绑定接进来时再说，接口按 m5-plan G4 不变。
