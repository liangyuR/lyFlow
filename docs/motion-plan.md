# 动效计划 —— 节点与连线的动画、hover 反馈

目标：**画布上的每一次变化都「看得见它是怎么变的」**：节点进出场、连线接上、状态切换、运行时数据沿边流动；
鼠标指到哪里，哪里和它的关联就亮起来。动效只做反馈，不改任何编辑与执行语义，不拖慢大图。

2026-09-24 与用户确认：
- 动画库用 **motion**（framer-motion 的现名，`motion/react`）；
- 动画范围：节点进出场、状态切换反馈、连线/选中反馈、运行时边上数据流动；
- hover 范围：节点 hover、边 hover、端口 hover、节点 hover 高亮关联边。

---

## 1. 现状

- 只有两处动效：`node--running` 的 `node-pulse` 呼吸光、进度条 `width` 过渡（`styles.editor.css`）。
- 边：默认边 + `LazyEdge`（`GraphCanvas.tsx`），样式来自 `lib/mapping.ts` 的 `style`。
- 节点状态从 `useNodeExecution(id)` 现查；`store/execution.ts` 已有 `onNodeTransition`。
- 超过 80 个节点开 `onlyRenderVisibleElements` —— 节点会因为**平移进视口**而重新挂载。

## 2. 定死的决定

### 动效基础设施

| # | 决定 | 理由 |
|---|---|---|
| A1 | `@lyflow/editor` 的 `dependencies` 加 `motion`（^12）。只从 `motion/react` 导入 | 用户指定；和 dagre、immer 一样是编辑器自带依赖，宿主不用管 |
| A2 | 新增 **`lib/motion.ts`** 作为编辑器的动效库：集中时长（`fast 120ms` / `base 200ms` / `slow 320ms`）、缓动、motion 的 variants/transition 预设，以及「当前是否启用动效」的判定。新增 **`styles.motion.css`**（由 `styles.css` 引入）放全部动效 CSS：CSS 变量 `--lyflow-motion-fast/base/slow`、keyframes、hover 规则。别处**不再散写**时长与 keyframes，现有 `node-pulse` 与进度条过渡迁进来 | 一处调手感；换库或关动效只改这里 |
| A3 | **分工**：一次性、有进出场的动效（节点进场、删除残影、连线生长、状态切换闪光/抖动、布局过渡）用 motion；**持续循环与 hover** 用 CSS（keyframes / transition） | 几百条边同时 hover 判定、流动循环放在 JS 里每帧跑不划算；CSS 由合成线程处理 |
| A4 | 开关：`LyFlowEditorProps` 新增 `animations?: boolean`（默认 `true`）。关掉或系统 `prefers-reduced-motion: reduce` 时：motion 全部跳到终态（`MotionConfig reducedMotion`），CSS 由 `.lyflow-motion-off` 根类与 `@media (prefers-reduced-motion: reduce)` 同时禁掉 keyframes 与 transition。**信息不能丢**：流动的边关了动画后仍以静态高亮表示「正在流动」 | 嵌入宿主要能关；无障碍 |

### 不能踩的坑：React Flow 的端口量测

React Flow 用 `getBoundingClientRect` 量端口相对节点包装层的位置（节点挂载、尺寸变化、`updateNodeInternals` 时）。
**任何作用在「含端口的元素」上的位移/缩放，只要赶上一次量测，连线端点就会永久错位。**

| # | 决定 |
|---|---|
| A5 | 含端口的元素（`.node`、`.node__body`、`.node-port`）**不做位移、缩放动画**，hover 也不行。节点进场只用 `opacity` + 光晕（`box-shadow`/`outline`）；hover「抬起」只用阴影表达，不 `translateY` |
| A6 | 允许 transform 的只有：不含端口的 `.node__head`（错误抖动）、端口圆点 `.react-flow__handle` 本身（以圆心为原点缩放，量测取的是中心，缩放不改变中心；注意与现有 `translateY(-50%)` 组合），以及删除残影（它不是 React Flow 节点） |

### 节点进出场

| # | 决定 | 理由 |
|---|---|---|
| N1 | **谁算「新出现」由 doc 的差分决定，不由挂载决定**：画布订阅 graph store，同一作用域（同一张 doc、同一面包屑路径）内的**编辑动作**前后比较节点/边 id 集合。`loadDoc`、`newDoc`、进出子图等「换一整张图」的变化不参与差分。新增 id 记入一个短命集合（播完即删），节点组件据此决定播不播进场 | 虚拟化下节点平移进视口会重新挂载，按挂载判定会满屏乱闪；打开文件也不该闪 |
| N2 | 进场：`opacity 0→1` + 一圈 accent 光晕淡出，`base` 时长。粘贴、片段插入、自动连线新建、撤销恢复出来的节点都走这一条 | 同上 A5 |
| N3 | 删除：doc 立即改（撤销栈、e2e、store 一律即时），画布在 store 变化的**同步回调里**（DOM 还在）克隆被删节点的 DOM，放进一个 `ViewportPortal` 残影层，`opacity→0` + `scale→0.96`，`fast` 时长后移除。**克隆体去掉全部 `id`、`data-*` 属性**，`aria-hidden`、`pointer-events: none`。一次删除超过 30 个节点不出残影 | 不能为了动画推迟真实删除；残影不能被 e2e 或无障碍树当成真节点 |
| N4 | `applyLayout`（自动布局）的位置过渡：doc 一次提交（一个撤销步），画布用 motion 的 `animate` 在 `slow` 时长内把**临时位置覆盖**从旧位置插值到新位置，喂给 React Flow（所以连线跟着走）；结束后撤掉覆盖。节点数超过 80（虚拟化阈值）或动效关闭时直接跳 | CSS transition 只动节点不动边，会看到线和节点分家 |

### 状态切换反馈

| # | 决定 |
|---|---|
| S1 | 只对**本次挂载期间发生的迁移**播放：首次渲染时已经是 done/error 的节点不闪（节点组件用 ref 记上一个 state，或用 `onNodeTransition`）|
| S2 | `→ done`：整个节点一次绿色光环闪（`box-shadow`，`slow`）。`→ error`：红色光环闪 + `.node__head` 水平抖动（±4px 衰减，约 300ms）。`→ running` 保持现有呼吸光（迁到 `styles.motion.css`）|
| S3 | stale、bypass、not-demanded 的透明度变化加 `base` 时长的 `opacity` 过渡 |

### 连线

| # | 决定 | 理由 |
|---|---|---|
| E1 | 新增一个自定义边组件替换 `default` 边类型（`LazyEdge` 并入它，按 `data.lazy` 决定虚线与 tooltip）。所有 hover/流动/生长都在这个组件里做 | 默认边拿不到状态与 hover |
| E2 | **连线生长**：N1 差分出来的新边，`motion.path` 的 `pathLength 0→1`，`base` 时长；惰性边生长完再恢复虚线 | — |
| E3 | **数据流动**：边的**目标节点** `running` 时，边上叠一条同色半透明描边，`stroke-dasharray` + `stroke-dashoffset` 的 CSS 循环向目标方向流动；目标节点离开 running 立即停。边加 `is-flowing` 类与 `data-flowing="1"`。每条边只订阅它目标节点的 state（selector 只取 state 字符串） | 数据被下游消费的那段时间才是「在流」；只订阅一个字段，不让每条事件重渲所有边 |
| E4 | 选中边：描边宽度与颜色走 `fast` 过渡 | — |

### Hover

| # | 决定 |
|---|---|
| H1 | **节点 hover**：阴影加深 + 边框提亮（CSS transition，不位移，见 A5）|
| H2 | **节点 hover 高亮关联边**：`ui` store 加 `hoverNodeId`（经 React Flow 的 `onNodeMouseEnter/Leave`）。与它相连的边 `is-related`（加粗、提亮），其余边 `is-dimmed`（降低不透明度）。拖连线（`pendingFrom`）、拖节点、框选期间不做淡化 |
| H3 | **边 hover**：边加粗 + 同色光晕；`ui` store 加 `hoverEdge`（两端的 node/port），两端端口加 `node-port--edge-end`、两端节点加 `is-edge-end`，都用 CSS 高亮 |
| H4 | **端口 hover**：圆点以圆心放大到 1.35 倍 + 类型色光晕（`box-shadow` 用端口自己的颜色，经 CSS 变量传入），标签文字提亮。与拖线期间的 `compatible/incompatible` 样式共存，后者优先 |
| H5 | hover 状态是纯 UI 状态，不进 GraphDoc、不进撤销栈 |

### 不做

节点拖动的惯性/弹簧；画布平移缩放的动画（React Flow 自带的 `fitView` 过渡除外）；边的路由变化动画；主题切换动画。

## 3. 验收

子代理写进 `docs/motion-acceptance.md`，逐条标 通过 / 未通过 / 未验证，附实际输出。新增 e2e 分组 `scripts/e2e/motion.mjs`，接进 `run.mjs`，并登记到 `scripts/e2e/README.md`。

1. `pnpm check` 全过；`packages/editor/package.json` 的 `dependencies` 有 `motion`；`git grep -n "@keyframes" packages/editor/src` 只命中 `styles.motion.css`。
2. **进场**：`addNode` 后 50ms 内新节点的计算 `opacity < 1`，400ms 后为 1。`loadDoc` 一张 10 节点的图，任何节点在任何时刻都没有进场标记；一张 > 80 节点的图平移到另一半，新进入视口的节点没有进场标记。
3. **删除**：删除节点后的下一帧 `[data-testid="node-X"]` 已不存在；此时存在一个残影元素，且其子树里没有任何 `data-testid` 与 `id` 属性；500ms 后残影消失。撤销一次恢复节点（并播进场）。
4. **端点对齐**：分别在进场结束、`→ error` 抖动结束、hover 节点期间、自动布局过渡的**中途**和结束，对若干条边断言：边路径的起止点与对应端口圆点中心的距离 ≤ 1px（画布坐标）。
5. **连线生长**：`connect` 后新边带生长标记，结束后路径完整（`pathLength` 终值 1 / 无残留 dasharray；惰性边的 dasharray 恢复为 `6 4`）。
6. **流动**：跑一张目标节点会运行足够久的图（找现有可控耗时的算子或测试算子），目标节点 running 时其入边 `data-flowing="1"`，其余边没有；运行结束后全图没有 `data-flowing="1"`。
7. **状态反馈**：一个节点从 running 到 done 恰有一次 done 闪光标记；到 error 时 `.node__head` 在抖动期间有非零 `translateX`、`.node` 本身始终没有 transform；重新挂载（例如进出子图）已是 done 的节点不闪。
8. **hover**：真鼠标移到节点 A 上 → A 的所有边 `is-related`、其余 `is-dimmed`；移开全部清除。移到边上 → 两端端口有 `node-port--edge-end`、两端节点有 `is-edge-end`。移到端口 → 圆点计算后的缩放 > 1、`box-shadow` 非 none。拖连线途中经过节点，不出现 `is-dimmed`。
9. **关动效**：CDP `Emulation.setEmulatedMedia` 设 `prefers-reduced-motion: reduce` 后，第 2、3、5 条的标记都不出现（或时长为 0），第 6 条的边仍有 `data-flowing="1"` 但计算后的 `animation-name` 为 `none`。`animations={false}` 在 `examples/host-react` 或 e2e 能触达的入口至少验证一次。
10. **不回归**：带 `LYFLOW_PACKS` 跑全部 e2e 分组全绿（尤其 M4 的大图性能阈值不变、M3 的连线手感）；hover/流动引入的重渲染不让大图性能组超阈值。

## 4. 实施顺序建议

A1–A4 → 自定义边 E1 → hover H1–H5 → 流动 E3 → 状态反馈 S1–S3 → 差分 N1 与进场/生长 N2、E2 → 删除残影 N3 → 布局过渡 N4 → 关动效 A4 的收尾 → 文档（`docs/embedding.md` 写 `animations` prop，`packages/editor/README.md` 写动效约定与 A5 的坑）。
