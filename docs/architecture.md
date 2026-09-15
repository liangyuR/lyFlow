# 架构

## 职责边界

划这条线的原则是：**谁掌握决策所需的信息，谁就做决策。**

| 层 | 语言 | 负责 | 明确不负责 |
|---|---|---|---|
| 编辑器包 `packages/editor` | TS / React | 图编辑、类型校验（UI 级）、参数表单、3D 视图、状态展示、本地撤销重做、快捷键、`--lyflow-*` 主题 | 执行调度、算子语义、结果计算、内存管理、**怎么和后端说话**、文件对话框、页面级样式 |
| 宿主壳 `app/`（Tauri）或 `examples/host-react/`（浏览器） | TS / React | 装配一个 `Transport`、文件对话框、窗口标题与菜单、页面级 CSS、验收窗口桥 | 图的任何语义 |
| 桥接 | Rust | IPC、序列化/反序列化、文件读写、进程与生命周期、事件推流、崩溃隔离 | 理解算子语义、改写图结构 |
| 核心 | C++ | 算子注册表、拓扑调度、中间结果缓存与复用、并行与显存管理、**权威校验** | UI 状态、节点坐标、**任何具体算法** |
| 算子包 | C++ | 算子实现、对第三方库（PCL、领域库）的依赖 | 执行调度、类型表、缓存策略 |

编辑器与「计算在哪儿」之间隔着 `Transport`（[ADR-0018](adr/0018-editor-as-package.md)）：
`TauriTransport` 走 `#[tauri::command]`，`HttpTransport` 走
[docs/http-transport.md](http-transport.md) 的 REST + WebSocket，
`StaticTransport` 只读一份 dump 出来的 manifest。三者的方法一一对应，
再一一对应到 C ABI v7 —— 换传输不换语义。

核心编译成一个只导出 C ABI 的 DLL，桥接层在运行时加载它（[ADR-0004](adr/0004-core-as-dll.md)）。
这条边界同时是崩溃隔离面和 M3 热重载的接缝。

算子包在**构建期**编进同一个 DLL，没有第四个进程也没有插件 ABI
（[ADR-0013](adr/0013-op-packs-static.md)）。包分两类，机制相同：仓库内的
`packs/*` 与 `LYFLOW_OP_PACKS` 指到的外部目录。core 自己只留 `gen.synthetic`
（测试基础设施）与 `util.reroute`（编辑器语义），零第三方依赖
（[ADR-0014](adr/0014-std-as-pack-core-zero-dep.md)）。

仓库内现在有三个包（[ADR-0015](adr/0015-algorithms-live-in-lyflow-packs.md)）：

| 包 | 算子 | 默认 |
|---|---|---|
| `packs/std-pointcloud` | 14 个点云 + 4 个 2D 量测（拟合、ICP、盒裁剪） | 开 |
| `packs/std-ml` | `ml.onnx_run` | 开 |
| `packs/gap` | 21 个 `gap.*`（间隙/段差测量） | 关，`LYFLOW_PACKS=gap` 打开 |

**算法住在包里，通用的那部分住在标准包里。** 标准包导出 `lyflow_std_algo`，
领域包调它而不是再抄一份拟合 —— 「加一个领域」因此不会把同一个 RANSAC 复制第二遍。

两点值得强调：

**执行调度必须在 C++。** 调度需要知道点云有多大、中间结果能不能复用、显存够不够、哪几步能并行。
这些信息只有 C++ 侧有。前端 JS 拿不到，也不该假装拿得到。

**前端的类型校验是"体验"，不是"正确性"。** `isValidConnection` 阻止明显错误的连线是为了手感，
但 C++ 侧在执行前必须独立完整校验一遍。前端可以被绕过（手改文件、旧版本客户端、脚本生成的图），
C++ 不能信任传进来的 GraphDoc。

## 三条数据流

```
                                 ① OperatorManifest[]
   C++ 注册表 ──────► Rust ──────────────────────────► 前端
   (启动 / 热重载)                                    节点面板 + 参数表单 + 类型表

                                 ② GraphDoc
   前端 ─────────────────────────► Rust ──────────────► C++
   (保存 / 运行)                   校验+落盘            权威校验 → 编译 → 执行

                                 ③ ExecutionEvent (流)
   C++ ──────────────────────────► Rust ──────────────► 前端
   (每个节点状态变化)               转发                 节点高亮 / 进度 / 错误
```

### ① OperatorManifest — 算子描述下行

前端启动时拉一次全量算子描述。这决定了节点搜索面板里有什么、每个节点长什么样、
参数面板渲染成什么控件。格式见 [operator-manifest.md](operator-manifest.md)。

关键性质：**前端不硬编码任何算子。** 加一个新算子的完整流程是「在 C++ 里注册 → 重启（或热重载）」，
前端代码零改动。

### ② GraphDoc — 图上行

用户编辑的结果。只包含拓扑、参数值和 UI 布局，不含任何运行时状态。
格式见 [graph-doc.md](graph-doc.md)。

C++ 收到后的处理顺序：
1. **校验** — 算子存在？版本兼容？端口类型匹配？必填参数齐全？参数在合法范围内？有环？
2. **编译** — GraphDoc → 内部执行计划（拓扑序、缓存键、资源预估）
3. **执行** — 按计划跑，边跑边发事件

### ③ ExecutionEvent — 状态上行

一个流式通道，C++ 每次节点状态变化就推一条。前端据此更新节点高亮。

```
NodeState: idle → pending → running → (done | error | cancelled | skipped)
```

- `skipped` 表示命中缓存，没有真正重算。这个状态要单独显示，否则用户会以为没跑。
- `stats.outputsAvailable` 说的是「这个节点的输出此刻取得到」：`done` 与命中缓存、
  被静音、由宿主注入的 `skipped` 都是 `true`，只有 `skipped` + `reason=not_demanded` 是 `false`。
  **消费方要认它，不要认 `state == "done"`** —— 缓存命中时状态是 `skipped` 而输出照样在，
  业务侧只 harvest `done` 的话会在第二次运行时「几何突然消失」。
- `error` 必须带结构化信息：`{ phase, code, message, paramPath?, portName? }`。
  `paramPath` 让前端能直接把红框标到具体那个参数输入框上。
- 一个节点可能同时有**多条**诊断，所以事件里给的是 `errors[]`，`error` 是 `errors[0]`
  的快捷方式。校验一次返回全部而不是第一条：前端一次标出所有红框，
  用户不用「改一个 → 重跑 → 又一个」地挤牙膏，而事后要改成全量得动三层。
- 因上游失败而没跑的节点是 `cancelled` + `errors[0].code = upstream_failed`，
  **不是** `skipped`。`skipped` 严格留给缓存命中 —— 两者混用的话，
  用户永远分不清「没跑」和「不用跑」。

## 运行模式

| 模式 | 触发 | 行为 |
|---|---|---|
| Run | 用户点运行 | 跑整图到所有终端节点 |
| Run to node | 右键节点 | 只跑该节点的上游闭包 |
| Live preview | 参数拖动中 | 降采样 / 限时预览，可被后续输入抢占取消 |

Live preview 是体验的分水岭，但也是最容易做错的一块：需要 C++ 侧支持**可取消**和**降级质量**。
建议 M2 之后再做，不要在早期把接口锁死成不可取消的同步调用。

## 缓存与复用

缓存键由 C++ 计算，形式上是节点的「上游内容哈希」：

```
cacheKey(node) = hash(op.id, op.version, params, [cacheKey(upstream_i) for i in inputs])
```

前端不参与缓存决策，但需要理解一件事：**改一个上游节点的参数，会让它所有下游失效。**
UI 上把受影响的下游节点标成 stale（虚线边框之类）是很值钱的反馈，成本很低。

中间结果本身也留在 C++（[ADR-0006](adr/0006-result-store-binary-ipc.md)）：
结果仓按 cacheKey 内容寻址，`runId → (nodeId, port) → cacheKey` 只是索引。
前端要看点云时按需拉一份**二进制**载荷 —— 一百万点的 JSON 是 30MB 文本加一次
全量解析，那条路走不通。

## 为什么先不做子图

子图/复合算子涉及嵌套序列化、端口提升、跨层引用和跨层缓存键，是整个项目里最贵的一块。
GraphDoc 的 schema 里预留了 `subgraphs` 字段和 `op` 的命名空间，
但 M0–M3 不实现。见 [roadmap.md](roadmap.md)。
