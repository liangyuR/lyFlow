# 两个节点输出并排对比（Compare）实施计划

目标：**主预览（右侧「预览」）一分为二，A / B 两栏各显示一个节点的输出，相机同步；栏下自动列出两侧的差异**
（点数、包围盒、以及输出值里同名的数值字段）。交互清单 P2 #35。

2026-09-28 与用户确认的两条：
- 形态：**主预览分成 A / B 两栏**，相机同步。不建在 Edge Peek 浮窗之上。
- 除了并排看，还要**自动算差异**：点数、包围盒、值里同名的数值字段（Measurement 的 value、Record 的 data 里的数……）。

两个必须同时成立的用例：
1. **同一个节点、两次运行**：改参数前把结果冻住，改完重跑，看前后差了多少。
2. **两个不同节点、同一次运行**：比如两条支路各自的输出。

---

## 0. 定死的决定

| # | 决定 | 一句话理由 |
|---|---|---|
| C1 | **A = 跟随选中 / 钉住的那个节点**（与今天的 `activeId = pinnedId ?? selectedId` 完全一样，`Viewer3D.tsx:398`）；**B = 基准**，一个显式的槽 | 用例 1 是「A 跟着新运行走、B 冻在旧结果」，用例 2 是「B 定住一个节点、点别的节点看 A」。一个跟随、一个固定，两个用例都不用多学东西 |
| C2 | 进入对比时 **B 默认 = 当前节点在当前结果上的冻结快照**；当前节点还没结果就 B = 同一节点（跟随最新）并提示 | 「改参数前先冻住」是最高频的一步，进入对比就顺手做完 |
| C3 | **只有 B 能冻结**。冻结 = 冻住前端已经拿到的整份来源（stats、类型、解析后的叶子、已解码的云），与 Edge Peek §4.5 同一条硬约束：桥接层只留最近一个已完成 run 的句柄（`bridge/src/execution.rs:142-146`），新 run 一开始旧 run 的索引就被 `ResultStore::freeRun` 抹掉（`core/src/exec/result_store.h:131-132`） | A 跟随选中，「冻结 A」没有意义；B 的快照只能是前端已有的数据，冻结后不能再向后端要这个 run 的东西 |
| C4 | **一个 `WebGLRenderer`、一块画布、两个 scissor 视口**，两侧共用同一台相机与同一个 `OrbitControls`；A / B 的点云各放在一个 `THREE.Layers` 层上，每帧渲染两次 | 相机同步不用「镜像」，本来就是同一台相机；WebGL 上下文数**不变**；导出 PNG 天然是两栏一张图 |
| C5 | 着色模式 / 色带 / 点大小 / 相机模式 **两侧共用一套**；「自动」范围取两侧数据范围的**并集** | 并排对比的前提是同一个值涂同一种颜色，各栏各调等于白比 |
| C6 | 差异计算是**纯函数**（`lib/compareDiff.ts`），只吃 `OutputStat[]` 与 `CloudPayload`，规则按**端口类型与字段路径**走，**不认算子名**（ADR-0003） | 单测直接测它（docs/testing.md「能在低一层测的不放 e2e」）；新算子零改动 |
| C7 | 点云的点数与包围盒取**载荷头里的 `totalPoints` 与全量 `bounds`**（`types/execution.ts:349-361`），不用抽稀后的 `pointCount` | 两侧的 maxPoints 可能不同（B 冻在 20 万点、A 在 200 万点），抽稀后的数字比出来的是假差异 |
| C8 | 对比模式下 **ROI 拖框、底图（roiBackdrop）关掉**；参数面板里「进入拖框」会先退出对比 | 拖框依赖单场景的 DOM 摆位（`RoiLayer` 靠 `frameListeners`），两个视口下没有意义；两件事互斥比同时做省一整套坐标映射 |
| C9 | 差异表放在两栏**下方**，可折叠，默认展开；顶栏一个「N 项不同」徽标 | 看图与看数在同一块地方，不再开窗 |
| C10 | 退出对比：工具栏按钮 / 同一快捷键 / B 标签上的 ×。**`Esc` 不退出** | Esc 已经身兼关 Peek 窗、退子图、取消运行三职（`hooks/useShortcuts.ts:62-73`），再加一职只会误触 |

---

## 1. 交互

### 1.1 进入

三个入口，同一个动作 `enterCompare(localId, path)`：

| 入口 | 位置 | 行为 |
|---|---|---|
| 工具栏「对比」按钮 | `Viewer3D` 第二条工具栏，「钉住」旁边，`data-testid="viewer-compare"` | 以当前 `activeId` 进入；没有选中节点时置灰 |
| 节点右键「设为对比基准（B）」 | `GraphCanvas.tsx` 的 `node-context-menu`（`:1001`），排在「清除此节点及下游的缓存」之后 | 没在对比就进入并 B = 该节点（**跟随最新，不冻结**）；已在对比就换 B。A 照旧跟随选中 |
| 快捷键 `Ctrl+Shift+D` | `lib/keymap.ts` 新增 `{ id: "compare", keys: ["Ctrl+Shift+D"], label: "对比 / 退出对比", scope: "global", group: "视图" }` | 与工具栏按钮同一动作（开 ↔ 关）；`?` 面板自动列出 |

进入时（C2）：B 槽 = `{ path, nodeId: activeId }`；若 A 此刻**已显示出结果**（`display.status === null && !loading`，与 Edge Peek 的锁定条件 `EdgePeek.tsx:76` 同一判据），立刻冻结 B；否则 B 跟随最新，toast「B 还没有结果，运行一次后可以冻结」。
参数面板开着且预览收起（`ui.paramPanel.viewerOpen === false`，`store/ui.ts:90`）时先 `setPanelViewerOpen(true)`，否则进入了也看不见。

### 1.2 两栏

```
┌ 预览 ─────────────────────── [3D▾][2M▾][●size][⤢] ┐
│ 着色[强度▾][viridis▾][lo][hi][自动]   📌  [退出对比] [PNG] │
├───────────────────────┬──────────────────────────┤
│ A · 当前  gen_1       │ B · 基准  gen_1  ❄ run …a3f2 ×  │
│      (3D 视口)        │        (3D 视口)              │
│ 12.3 万 / 200 万 点    │ 12.3 万 / 200 万 点            │
├───────────────────────┴──────────────────────────┤
│ ▾ 差异  3 项不同 · 7 项相同                 [只看不同] │
│ cloud.points        2 000 000   1 998 120   +1 880 (+0.09%) │
│ cloud.bounds.max    (1.20, 0.80, 0.31) (1.20, 0.80, 0.30) (0, 0, +0.01) │
│ gap.value           0.512 mm    0.498 mm    +0.014 mm │
└──────────────────────────────────────────────────┘
```

- 每栏一个标签条：`A · 当前` / `B · 基准`，节点标题，A 侧沿用「已钉住」「预览 N 万点」「底图：X」三个既有徽标；B 侧多一个冻结开关 ❄（`data-testid="compare-freeze"`）与 ×（退出对比）。
- 每栏各自的状态文字（「未运行」「正在计算…」「该节点无点云输出，上游也没有可当底图的点云」……）只盖自己那半，文案沿用 `Viewer3D.tsx:559-604` 那一组。
- 一块画布，`Scene` 加 `views: 1 | 2` 与 `split: "lr" | "tb"`。默认左右；容器宽度 < 480 px 时自动改上下叠（右侧栏可能很窄，`styles.css:99` 的 `.app__viewer` 只有 44% 高），`data-split` 暴露给验收脚本。
- 对比开着时视图容器长高：`.app__viewer:has(> .viewer[data-compare="1"])` 的 `flex-basis` 44% → 60%（`LyFlowEditor.tsx:715`）。

### 1.3 相机与取景（C4）

- `OrbitControls` 绑在整块画布上，在任一栏拖动，两栏一起转、一起缩放 —— 同一台相机。
- 每栏的视口尺寸相同，所以 `camera.aspect`/`ortho` 的边按**单栏**尺寸算（`resize` 里取 `w/2 × h` 或 `w × h/2`）。
- 自动取景与「⤢」按 **A ∪ B** 的联合包围盒（复用 `unionBounds`，`Viewer3D.tsx:239`），触发条件与今天一样：换了云或换了几何才取景，调参数不拉视角。
- 2D 剖面（`ui.viewerMode`，`store/ui.ts:97`）照常切；两栏同一台正交相机。

### 1.4 着色一致（C5）

- 着色模式、色带、点大小、maxPoints、相机模式：一套控件，两栏同值。
- 「自动」范围 = `dataRangeOf(A) ∪ dataRangeOf(B)`（`Viewer3D.tsx:216`，高度用的是全量 `bounds`，两侧 maxPoints 不同也一致）；手动范围两栏同用。
- `effectiveShading` 的降级（选了强度但这片云没有强度）按**两侧都有**才算有：A 有强度 B 没有 → 两栏都退到高度，下拉框显示实际那一种（沿用 `Viewer3D.tsx:488-495` 的诚实原则）。
- B 冻结后，maxPoints 下拉只作用于 A；B 标签 tooltip 写明「快照固定在 N 点」。这是 Edge Peek「需要回后端的控件置灰」规则（§4.5）在这里的唯一体现 —— 其它控件都是纯前端的。

### 1.5 冻结与解冻（C3）

- ❄ 点亮：把 B 此刻的整份来源存进快照（见 §3.2），标签显示「❄ run ·a3f2」（后 6 位，与 `EdgePeek.tsx:59` 的 `shortRun` 同）。B 没有已显示的结果时按钮置灰，tooltip「先运行一次」。
- ❄ 熄灭：丢快照，B 回到跟随最新（同一节点）。
- **冻结的 run 被后端回收**是常态而不是异常：快照全在前端，画面与差异表照常；不需要任何「取不到了」的分支。唯一被禁的操作是给 B 换 maxPoints（§1.4），tooltip 用 `PEEK_FROZEN` 那句（`store/peek.ts:47`）。
- 右键换 B（§1.1）= 换节点 + 解冻。

### 1.6 内容：点云场景还是值的表格

沿用 `viewerContentFor`（`lib/viewRule.ts:72`）逐侧判定，再合并：
**一侧可画就两栏都是点云场景**（只有值的那一侧显示「该节点无点云输出……」，它的值照样进差异表）；**两侧都只有值**才换成两张并排的 `ValuePane`（`Viewer3D.tsx:334`）加差异表。
手动选的「值」（`ui.viewerContentPick`）以 A 的 `fullId` 为键，行为不变。

### 1.7 退出与自动退出

- 工具栏「退出对比」/ `Ctrl+Shift+D` / B 标签的 ×。
- B 的节点被删（撤销也算）→ 自动退出并 toast，与钉住节点被删自动松开同一条（`Viewer3D.tsx:514-516`）。
- 切换子图层级**不退出**：B 槽带 `path`，`resolveOutput(doc, B.path, B.nodeId, port)`（`lib/subgraph.ts:413`）跨层照样解得开，执行表也是按路径 id 存的（`store/execution.ts:498` 的 `aggregatedNodes(B.path, …)`）。A 跟随当前层的选中。
- 参数面板 ROI 行「拖框」→ 先退出对比再切 2D（C8）。
- 新建 / 打开图：退出。

---

## 2. 差异计算（`lib/compareDiff.ts`）

### 2.1 输入与输出

```ts
export interface CompareSide {
  outputs: readonly OutputStat[] | undefined;   // node_state 事件里那份 stats.outputs
  cloud: CloudPayload | null;                   // 这一侧场景里正在画的那片云（自己的，不含借来的底图）
  cloudPort: string | null;                     // 那片云是哪个端口（含 `<port>.<field>`）
  preview: boolean;                             // 来自预览运行（ADR-0011，源头抽稀）
}

export interface DiffRow {
  key: string;            // `<port>.<字段路径>`，例如 cloud.points / gap.value / info.data.score
  type: string;           // 端口类型（来自 stats）
  kind: "number" | "vector" | "text" | "count" | "only";
  a: string; b: string;   // 显示文本（num() 的 6 位有效数字，Inspector.tsx:31）
  delta: string | null;   // A − B；text 与 only 为 null
  changed: boolean;
  side?: "A" | "B";       // kind=only 时：只有哪一侧有
}

export interface DiffResult {
  rows: DiffRow[];
  changed: number; same: number; only: number;
  /** 两侧来自不同模式的运行（一侧预览、一侧正式）：数字不可比，界面整表变淡并挂徽标 */
  modeMismatch: boolean;
}

export function diffSides(a: CompareSide, b: CompareSide, tol?: Tolerance): DiffResult;
```

### 2.2 端口配对

1. **同名端口**配对（同一节点两次运行、或同一算子的两个实例，全部走这条）。
2. 剩下的按**（类型，出现顺序）**配对：A 的第一个 `PointCloud` 对 B 的第一个 `PointCloud`。
3. 还剩的出 `kind: "only"` 行，只计数不算「不同」。
4. Bundle 端口：`stats.outputs` 里已经带了 `<port>.<field>` 的字段项（`core` 展开，`lib/basecloud.ts:22` 就是这么找云的），**只比字段项**，整端口那条跳过，免得同一个数出两遍。

### 2.3 规则表：每种类型比什么

| 类型 | 行 | 取自 | 显示 / Δ |
|---|---|---|---|
| `PointCloud`（含 Bundle 的点云字段） | `points` | `cloud.totalPoints`；这一侧没取到云时退回 `stat.elementCount` | 整数 + 百分比：`+1 880 (+0.09%)` |
| | `bounds.min` `bounds.max` `size` | `cloud.bounds`（全量）；没云时 `—`，不算不同 | 三元组，Δ 逐分量：`(0, 0, +0.01)` |
| `Box2D` | `min` `max` `size` `center` | `value.min/max` | 二元组，Δ 逐分量 |
| `Line2D` | `point` `dir`（无线段）或 `start` `end` `length`（有线段） | `value` | 二元组 / 标量 |
| `Circle2D` | `center` `radius` | `value` | 二元组 / 标量 |
| `Point2D` | `p` | `value` | 二元组 |
| `Measurement` | `value` `nominal` `upper` `lower` | `value`；`unit` 不同时 Δ 写「单位不同」并算不同 | 标量带单位：`+0.014 mm` |
| | `ok` `verdict` `message` | `value` | text |
| `Record` | `data.<路径>` 逐键递归（深度 ≤ 2，与 `ValueView.tsx` 的 `MAX_DEPTH` 同），数字 → number，数字数组 → vector，其余 → text | `value.data` | |
| `Plane` | `normal` `d` | `value` | 三元组 / 标量 |
| `Transform` | `m`（16 个） | `value` | 逐分量，Δ 只显示最大绝对差 `max|Δ|` |
| `Tensor` | `shape`（text）`count` `min` `max` `mean` | `value`（事件里只有统计量，ADR-0015） | 标量 |
| `Indices` | `count` | `stat.elementCount` | 整数 + 百分比 |
| `Error` | `message` | `value` | text |
| 其它 / 未知 | `elementCount` | `stat.elementCount` | 整数 |

说明：
- 通用规则其实只有一条 ——「**同路径的数值字段逐个比，数组逐分量比，字符串按相等比**」，上表只是把 `OutputValue`（`types/execution.ts:103-139`）的各字段按这条规则摊开，外加点云那三行走载荷头（C7）。新类型只要 `valueJson` 里是数字就自动进表。
- `null`（Measurement 没测出）与数字比：算不同，Δ 为 `—`。
- **容差**：`|a − b| ≤ max(absTol, relTol · max(|a|, |b|))`，默认 `absTol = 1e-9`、`relTol = 1e-6`。显示用 `num()` 的 6 位有效数字，所以容差比显示精度细一档，不会出现「两边显示一样却标不同」；反过来「显示不同却标相同」只在第 7 位以后，可接受。
- `modeMismatch`：`a.preview !== b.preview`。预览 run 是源头抽稀（ADR-0011），点数、包围盒、乃至量测值都变了，这时整表变淡并挂「预览中，数值仅供参考」；`autoRun` 补完正式 run 后自然恢复。

### 2.4 显示

`components/CompareDiff.tsx`：一张表，列 `字段 | A | B | Δ`；不同的行加 `is-changed`（accent 左边框），`only` 行灰字。
顶部「N 项不同 · M 项相同（· K 项仅一侧有）」+「只看不同」开关（组件内 state，不持久）。
`data-testid="compare-diff"`，行 `data-key` / `data-changed`，表 `data-changed-count` / `data-mode-mismatch`。

---

## 3. 状态与数据流

### 3.1 store（`store/compare.ts`，独立于 `ui`）

```ts
export interface CompareSlot {
  path: SubPath;
  nodeId: string;                       // 局部 id；取数时 fullId(path, nodeId)
}

/** B 冻结时存下来的东西。整份一起冻（与 Edge Peek §4.5 同一理由：只冻状态文字的话，
 *  重跑一开始 stats 就没了，冻结的 Measurement 立刻变空）。 */
export interface CompareSnapshot {
  runId: string;
  preview: boolean;
  label: string;
  content: ViewerContent;
  outputs: readonly OutputStat[] | undefined;
  cloud: CloudPayload | null;           // 直接持有解码后的载荷，不靠 cloudCache
  cloudPort: string | null;
  base: BaseCloud | null;
  maxPoints: number;
}

interface CompareState {
  on: boolean;
  b: CompareSlot | null;
  snapshot: CompareSnapshot | null;     // 非 null = 已冻结

  enter(slot: CompareSlot): void;
  exit(): void;
  setB(slot: CompareSlot): void;        // 换节点 + 解冻
  freeze(snap: CompareSnapshot): void;
  unfreeze(): void;
  prune(doc: GraphDoc): void;           // B 的节点没了就 exit()
}
```

单独开 store 而不塞进 `ui`，理由与 `peek` 相同：快照里挂着几十兆的 `Float32Array`，不该让每个订阅 `ui` 的组件跟着比对引用。

**快照不走 `cloudCache` 的 pin**（`lib/cloudCache.ts:19-30`）：Edge Peek 锁定后仍按 key 回缓存里查（`CloudView.tsx:301` 用 `lockedRun` 拼 key），所以要 pin；这里快照直接持有载荷，`dropOtherRuns` 清掉缓存条目也不影响它。少一个跨模块的引用计数。

### 3.2 取数：一份代码，两个来源

`Viewer3D.tsx:548-646` 的「取点云」effect 抽成 `hooks/useViewerSource.ts`：

```ts
export function useViewerSource(sel: {
  slot: CompareSlot | null;        // null = 没有节点
  maxPoints: number;
  content: ViewerContent;
  frozen: CompareSnapshot | null;  // 给了就原样返回，不订阅、不取数
}): { display: Display; loading: boolean; outputs: readonly OutputStat[] | undefined; op: OperatorDesc | undefined }
```

- 内部照旧：只订阅那一个节点的 `state` 与 `stats.outputs`（`Viewer3D.tsx:403-411` 的注释说明了为什么不能订阅整张 Map）；`firstCloudPort` → `resolveOutput` → 没云就 `findBaseCloud`（`lib/basecloud.ts:105`）；`cloudCache` 命中 / `transport.getOutputCloud` / `decodeCloud`；预览时 `min(maxPoints, previewMaxPoints)`。
- **行为不变**：普通模式的 `Viewer3D` 就是调一次 `useViewerSource({ slot: A })`。
- 对比模式：`CompareStage` 调两次（A、B）。`frozen` 非空时 hook 直接返回快照里的 `display`，effect 的依赖全部恒定，「冻结后不再发请求」是自然结果（Edge Peek §4.5 的那条经验）。
- `dropOtherRuns(runId)`（`Viewer3D.tsx:607`）仍在 hook 里调；A、B 同一个 runId 时不互相清。B 跟随最新时与 A 同 run；B 冻结时不经过缓存。

### 3.3 场景：`lib/cloudScene.ts`

从 `Viewer3D.tsx:48-303` 抽出 `Scene` / `createScene` / `fitToBounds` / `unionBounds` / `overlayBoundsOf` / `writeColors` / `writeNormalColors` / `dataRangeOf`（原样搬家，注释带走），加三样：

- `views: 1 | 2`、`split: "lr" | "tb"`；`tick` 在 `views === 2` 时开 `setScissorTest(true)`，对两个矩形各 `setViewport`/`setScissor`，`camera.layers.set(1)` 渲染左栏、`set(2)` 渲染右栏；grid / axes / 底图 `layers.enable(1); enable(2)`。
- `points` 变成 `points: [THREE.Points | null, THREE.Points | null]`，`overlay` 同样两组；`views === 1` 时只用第 0 个，**普通模式一行都不改行为**。
- `resize(rect)`：按单栏矩形算 `aspect`。

`peek/CloudView.tsx` 自己那份场景代码（`:30-45`）**本轮不动**（见 §7）。

### 3.4 组件

```
components/CompareStage.tsx   两栏 + 标签条 + 各栏状态 + 差异表；持有 Scene（views=2）
components/CompareDiff.tsx    §2.4 的表
```

`Viewer3D` 的 `viewer__stage`（`:1170`）里：`compare.on ? <CompareStage …/> : <现在那些>`。两条工具栏留在 `Viewer3D`，控件的 state（shading / ramp / range / pointSize / maxPoints）也留在那里，经 props 下发 —— 对比模式退出后这些设置还在，与今天一致。
普通模式的场景在进入对比时 `dispose()`（`Viewer3D.tsx:150-172`，含 `forceContextLoss`），退出时重建；`RoiLayer` / backdrop 只在普通模式挂。

### 3.5 运行被回收、部分运行、库算子

| 情形 | A（跟随） | B 跟随最新 | B 冻结 |
|---|---|---|---|
| 新 run 开始，旧 run 索引被 `freeRun` | 跟着新 run（今天的行为） | 同 A | 不受影响：快照全在前端 |
| 部分运行（`targets` / `isolate`），B 的节点不在计划里也没被挂上 → `dropUnreachable` 退回 idle（`store/execution.ts:396`） | — | 显示「该节点尚未产出结果」，差异表 B 列 `—` | 不受影响 |
| B 是库算子（`resolveOutput` 返回 null） | — | 「这个算子的内部结果查不到」；差异表无 B | 冻结按钮置灰（没有可冻的） |
| 预览 run（`exec.preview`） | 显示预览徽标 | 同 A | `modeMismatch` → 整表变淡 |

### 3.6 数据属性（验收脚本用）

`.viewer` 根上：`data-compare="0|1"`、`data-compare-a`（A 的 fullId）、`data-compare-b`、`data-compare-frozen="0|1"`、`data-compare-run-b`、`data-split`；两栏各自 `data-testid="compare-pane-a|b"` 带 `data-view`（loading / cloud / value / empty，规则同 `Viewer3D.tsx:934`）与 `data-cloud-bounds`。

---

## 4. WebGL 上下文预算

**不变。** Edge Peek 定的是「主 Viewer3D 占 1，浮窗带 WebGL 的最多 4（`PEEK_MAX_WEBGL`，`store/peek.ts:50`），总 5」。C4 的单 renderer 双视口让对比模式仍然只占 1，`PEEK_MAX_WEBGL` 不用动，`edge-peek-plan.md §4.6` 那段不用改。

被否掉的做法：两个 `WebGLRenderer`（各一个上下文）+ 逐帧把 A 的相机矩阵抄给 B。它要把主视图记成 2、`PEEK_MAX_WEBGL` 降到 3，相机同步要处理 damping 的相位差，导出 PNG 还得自己拼两张。没有一样比得过「本来就是同一台相机」。

代价：`tick` 每帧两次 `render`，点云 material 共享、几何各一份，开销约 2×；`ResizeObserver` 的回调按单栏算 aspect。可接受。

---

## 5. 实施顺序

| 片 | 内容 | 动的文件 | 测试 |
|---|---|---|---|
| S1 | 差异计算纯函数 | 新 `packages/editor/src/lib/compareDiff.ts` | 新 `packages/editor/test/compare-diff.test.mjs`（表驱动：§2.3 每种类型一行；容差边界；`totalPoints ≠ pointCount` 时取 total；没云退回 elementCount；同名 / 同类型顺序 / only 三种配对；Bundle 只比字段项；`null` 对数字；单位不同；`modeMismatch`） |
| S2 | compare store + 快捷键表 | 新 `store/compare.ts`；`lib/keymap.ts` 加一行；`hooks/useShortcuts.ts` 加一个 case | 新 `test/compare-store.test.mjs`（enter 有 / 无结果的两种默认；freeze 要求有快照；setB 解冻；prune 节点没了就 exit；exit 清空） |
| S3 | 场景抽取 + 双视口 | 新 `lib/cloudScene.ts`（从 `Viewer3D.tsx:48-303` 搬）；`Viewer3D.tsx` 改成 import | 无新测试。`pnpm typecheck`；现有 e2e `m3 / m4 / gap / m8b / m8c / peek` 全绿（它们断言 `data-cloud-bounds`、`data-overlay-bounds`、取景与 RGB 着色，正是搬家最容易碰坏的） |
| S4 | 取数抽取 | 新 `hooks/useViewerSource.ts`（从 `Viewer3D.tsx:548-646` 搬）；`Viewer3D.tsx` 改成调 hook | 同 S3：行为不变，靠现有 e2e 兜底 |
| S5 | 对比舞台 | 新 `components/CompareStage.tsx`、`components/CompareDiff.tsx`；`Viewer3D.tsx` 工具栏「对比」「退出对比」、冻结开关、范围并集、fit 并集、`roiEditing` 加 `!compare.on`；`styles.viewer.css`；`styles.css` 的 `:has` 规则；`view-rule.test.mjs` 加一行（`compareContentFor`：一侧可画就是 cloud） | 新 e2e `scripts/e2e/compare.mjs`（§6 的 1–6），登记进 `run.mjs` 与 `scripts/e2e/README.md` |
| S6 | 入口与收尾 | `GraphCanvas.tsx` 右键项；`ParamPanel` ROI 行退出对比；节点删除 / 新建 / 打开图退出；PNG 导出；`docs/interaction-checklist.md` #35 登记；`docs/testing.md` 地图与数字；`ShortcutPanel` 自动带出 | e2e `compare.mjs` 的 7–9 |

S1 / S2 与 S3 / S4 之间没有依赖，可并行；S5 依赖前四片。

---

## 6. 验收

`scripts/e2e/compare.mjs`（带 `LYFLOW_PACKS=gap;dts` 跑，复用 `peek.mjs` 的 `CHAIN_NODES` 那张链）：

1. 选中 `gen` 跑一遍，点「对比」：`data-compare="1"`、`data-compare-b` = gen 的 fullId、`data-compare-frozen="1"`、`data-compare-run-b` = 当前 runId；两栏 `data-view="cloud"` 且 `data-cloud-bounds` 相同；差异表 `data-changed-count="0"`。
2. 改 `gen.pointCount` 5000 → 6000 重跑：A 栏 `data-cloud-bounds` 变、B 栏不变；`cloud.points` 行 `data-changed="1"`，Δ 文本为 `+1 000 (+20%)`；`data-compare-run-b` 仍是旧 runId（此时旧 run 已被后端回收 —— 这一条就是 C3 的硬约束在界面上的证据）。
3. 在 A 栏真鼠标拖动旋转：两栏共用相机 —— 断言 `window.__lyflow` 暴露的相机位置变了且只有一台（`snapshot().viewer.cameras === 1`）；导出 PNG 的宽度等于整块画布。
4. 切 2D：`data-camera="2d"`，两栏都还是 `data-view="cloud"`。
5. 右键 `fit` →「设为对比基准（B）」：`data-compare-b` 换成 fit、`data-compare-frozen="0"`；`fit.line` 只有 2D 几何，B 栏 `data-base` = 它上游那片云（底图规则不变）；差异表按（类型，顺序）配对出 `cloud.points` 行且 `only` 计数 ≥ 1（`line` 只有 B 有）。
6. 点别的节点：`data-compare-a` 跟着换，B 不动；钉住后再点别的：A 不换。
7. 删掉 B 的节点：`data-compare="0"`，toast 出现。
8. 参数面板开着且预览收起时按 `Ctrl+Shift+D`：预览展开、`data-compare="1"`；再按一次退出。ROI 行「拖框」时 `data-compare="0"` 且 `data-roi-edit` > 0。
9. 两侧都只有值的节点（`transform.make`，`gap.mjs` 那组用过）：两张 `viewer-values` 并排，差异表有 `m` 行。

外加：`pnpm check`、`pnpm typecheck`、`pnpm --filter @lyflow/editor test`（含 S1 / S2 的两个新文件）、带 `LYFLOW_PACKS=gap;dts` 的 `pnpm e2e` 与 `pnpm e2e:http` 全绿；手工确认对比模式 + 4 个点云 Peek 窗不掉上下文（预算没变，但双视口是新路径）。

按 `docs/testing.md` 的规矩：差异表**每一种类型的数值**只在 `compare-diff.test.mjs` 里钉，e2e 只验 2 与 5 两行的存在与 Δ 文本。

---

## 7. 明确不做

- **交换 A / B**：A 跟随选中、B 固定，交换等于「把 A 钉住到 B 的节点 + B 换成旧 A」，两个动作用户自己做得到；做成一个按钮反而要解释「冻结跟着谁走」。
- **A 也能冻结**、**三栏以上**、**多个基准**。
- **两栏各自独立的相机**（分离模式）—— C4 的单相机是设计核心，不留开关。
- **点云级差异**（最近邻距离、逐点着色差异）：那是一个算子（core 里做，前端只显示），不是预览功能。
- **差异导出**（CSV / 复制）：表格能选中复制，本轮够用。
- **`peek/CloudView.tsx` 改用 `lib/cloudScene.ts`**：值得做，但它是 Edge Peek 的验收面，与本计划无关，另起一小片。
- **历史 run 浏览**：受 C3 的硬约束（桥接层只留一个 run 句柄），与 Edge Peek §8 同一条。
- **对比模式下的 ROI 拖框 / 底图**（C8）。

---

## 8. 风险与待定

**风险**

1. **S3 / S4 是对 1190 行组件的两次搬家**。风险不在逻辑而在依赖数组 —— `Viewer3D.tsx:645-646` 那 15 个依赖少一个就是「切节点不刷新」这类难查的 bug。缓解：先搬后改，每片单独提交，e2e 全跑一遍再进下一片。
2. **`Layers` 与 `OrbitControls`**：`controls.object` 在 2D 时换成正交相机（`Viewer3D.tsx:135-149`），`camera.layers` 要在**当前活动相机**上设，两台相机都要设。写成 `scene.active().layers.set(n)`。
3. **右侧栏太窄**：44% 高 × 一半宽，每栏可能只有 200 px。§1.2 的自动上下叠 + 60% 高是缓解，不够就得让用户拖分栏（`LyFlowEditor` 已有 `rightPane.width`）。
4. **快照占内存**：一片 200 万点的云约 24 MB 坐标 + 8 MB 强度，只有一份，且退出对比就释放。可接受；`maxPoints` 8M 的快照约 128 MB，标签上写点数就够了，不另设上限。

**已确认（2026-09-28，按默认值）**

1. 快捷键 `Ctrl+Shift+D`（D = diff）。`Ctrl+Shift+C` 在 WebView2 里会撞开发者工具。
2. 差异 Δ 的方向 **A − B**（当前减基准）：正数 = 比基准大。
3. 容差 `absTol = 1e-9`、`relTol = 1e-6`，不给用户调。
4. 对比开着时视图 44% → 60% 高，自动。
5. 「只看不同」默认关。
6. 右键项名「设为对比基准（B）」，换 B 时**解冻**。要不要多一项「与基准对比（冻结）」—— 目前不加，冻结在视图上点。
