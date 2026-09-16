# 连线内容查看器（Edge Peek）实施计划

目标：**双击图上任意一条连线，就地弹出一个浮窗，看见这条线上正在传输的东西。**
点云给 3D，2D 几何与剖面给正交视图，张量给图像，其余给键值表加原始 JSON。

前提：M4 已落地（子图展开、live preview）。本计划要动 C ABI（v7 → v8），
因为张量与下标集合**目前根本取不出来**：`previewCloud` 只认 PointCloud，
张量只有 shape 与 min/max/mean 随事件走。

---

## 0. 定死的决定

| # | 决定 | 一句话理由 |
|---|---|---|
| P1 | **双击边 = 打开查看器**；原来的「在边上插入 Reroute」挪到**边的右键菜单** | 「我想看看这里流的是什么」比「我想理线」高频得多；边的右键菜单现在还是空的（只有节点有），顺手补上「在此插入 Reroute / 删除连线」 |
| P2 | 浮窗开在边中点旁，**之后屏幕固定**，可拖动、可多开 | 既有「从这条线弹出来」的直觉，又不用逐帧跟 viewport 重算位置 —— 与上一版「GraphCanvas 去掉逐帧重建」同一条教训 |
| P3 | 视图四种：**3D 点云 / 正交 2D（剖面 + 2D 几何）/ 张量图像 / 文本表格 + 原始 JSON** | 覆盖现有 13 个端口类型的全部可视形态 |
| P4 | 补两个 C ABI 入口取**张量**与**下标**，ABI v7 → v8 | 不补这两个，「Tensor 当图像看」和「Indices 看得见」都是空话 |
| P5 | 张量：后端只给 **原始 float + 完整 shape**，布局由前端推断并允许手动改 | 色带、归一化、切片这些是看图的人一秒改一次的东西，不该每改一次走一趟 IPC |
| P6 | Indices：**只显示数量与下标列表**（文本），不做「在上游点云上高亮」 | 本轮范围收敛；高亮要回溯上游云 + 双缓冲着色，单独一档工作量 |
| P7 | 没跑过 / 节点还没出结果：只显示「未运行」占位，**不自动发 run** | 与现有 3D 视图行为一致；双击一条要重算 30 秒的边不该卡住界面 |
| P8 | 窗口默认**跟随最新 run**；窗上有「锁定快照」按钮 | 调参时几个窗一起变是调试最想要的；锁一个下来才能对比改参前后 |
| P9 | 边上**不做**常驻摘要徽标 | 边渲染保持不动，风险最小；密集图里满屏徽标反而更难读 |
| P10 | 工具栏动作：**复制 JSON / 导出 PNG / 跳到源节点 / 在主 3D 视图打开** | 导出数据文件（PLY/CSV/NPY）本轮不做 |

P4/P5 合写一份 ADR：[0019-output-tensor-and-indices-over-abi](adr/0019-output-tensor-and-indices-over-abi.md)（0018 已被 editor-as-package 占用）。

---

## 1. 数据通路：从一条边到一个值

### 1.1 边的内容 = 源端口的输出

一条 `GraphEdge` 的内容就是 `edge.from`（`{node, port}`，当前层级的局部 id）的输出。
唯一需要小心的是**子图**：源节点可能是个 `sub:` 节点，它的输出端口名是子图对外声明的名字，
而结果仓里存的是展开后的叶子路径 id。

所以**统一走一次解析**，后面所有取数都只认解析后的叶子：

```ts
const resolved = resolveOutput(doc, path, edge.from.node, edge.from.port);
// → { nodeId: "outer/inner/leaf", port: "cloud" } | null
```

解析不出来（库算子的定义在库文件里）就显示「这个算子的内部结果查不到」，与 Viewer3D 现有行为一致。

### 1.2 类型判定：运行时优先

边本身不带类型。两个来源，按优先级：

1. **运行时类型** —— `stats.outputs[].type`（`node_state` 事件带的）。`util.reroute` 这种 `Any → Any` 的算子只有这一条路。
2. **声明类型** —— manifest 里源算子该输出端口的 `type`。没跑过时只有它。

`Any` 且没跑过 → 直接「未运行」占位（P7）。

### 1.3 三条取数路径

| 类型 | 数据在哪 | 怎么拿 |
|---|---|---|
| `PointCloud` | 结果仓，二进制 | 已有 `getOutputCloud(runId, node, port, maxPoints)` |
| `Tensor` | 结果仓，二进制 | **新增** `getOutputTensor(runId, node, port, offset, count)` |
| `Indices` | 结果仓，二进制 | **新增** `getOutputIndices(runId, node, port, offset, count)` |
| 其余 10 种 | **已经在前端了** | `node_state` 事件的 `stats.outputs[].value`，Record 的 `data` 是全量不截断 |

最后一行值得强调：`Box2D / Line2D / Circle2D / Point2D / Measurement / Record / Transform / Plane / Error`
的完整 JSON 随事件早就到了前端（`core/src/data.cpp:304`），缓存命中的 `skipped` 节点也一样带
（`core/src/exec/executor.cpp:569`）。**文本、表格、2D 几何三种视图零 IPC**，打开即有。
只有极端情况（宿主注入的结果、事件被丢）才回退到 `getOutputInfo(runId, nodeId)` 补一次。

---

## 2. C++ / C ABI v8

### 2.1 两个新入口

```c
typedef struct {
  uint32_t rank;
  uint32_t count;        // 本次返回的元素数
  uint64_t offset;       // 本次切片的起始元素下标
  uint64_t total;        // 张量总元素数
  const int64_t* shape;  // rank 个，永远是**完整**形状
  const float* data;     // count 个
  void* handle;
} lyflow_tensor_view;

// offset 越界返回 count=0；count=0 表示「从 offset 取到末尾」。
// 返回 0 = 成功；1 = 没有这个结果 / 该输出不是张量；2 = out 为空；3 = 异常。
LYFLOW_API int lyflow_output_tensor(const char* run_id, const char* node_id, const char* port,
                                    uint64_t offset, uint32_t count, lyflow_tensor_view* out);
LYFLOW_API void lyflow_tensor_view_free(lyflow_tensor_view* view);

typedef struct {
  uint32_t count;
  uint32_t total;
  uint64_t source_cloud_id;   // 指向哪片云；前端只显示，不校验
  const int32_t* values;
  void* handle;
} lyflow_indices_view;

LYFLOW_API int lyflow_output_indices(const char* run_id, const char* node_id, const char* port,
                                     uint64_t offset, uint32_t count, lyflow_indices_view* out);
LYFLOW_API void lyflow_indices_view_free(lyflow_indices_view* view);
```

实现要点：
- `handle` 里放一份 `Data` 的拷贝（`shared_ptr` 浅拷贝），`data`/`values` 直接指进结果仓里那份张量，
  **零拷贝**，与 `CloudPreview` 不同 —— 点云要抽稀所以必须复制，张量切片不需要。
- `shape` 永远给完整形状，不随 `offset/count` 变 —— 前端要靠它算切片。
- 张量**不做抽稀**：抽稀过的图像没有意义。数据量靠切片控制。
- `LYFLOW_ABI_VERSION` 7 → 8，同步 `bridge/src/core_ffi.rs:17` 的 `ABI_VERSION`。

### 2.2 core 单测（`core/tests/`）

- 张量：`(2,3,4)` 的张量，`offset=6,count=6` 拿到第二片，shape 仍是 `(2,3,4)`，`total=24`。
- 张量：`count=0` 取到末尾；`offset` 越界得到 `count=0` 而不是崩。
- 下标：`sourceCloudId` 原样带出；`offset/count` 分页；对非 Indices 端口返回 1。

---

## 3. Rust 桥接 / HTTP 契约

### 3.1 二进制帧

沿用点云那套（`'LYPC'` → `CLOUD_MAGIC`）的风格，两个新 magic：

**`LYTN`（0x4E54594C）张量**

| 偏移 | 类型 | 含义 |
|---|---|---|
| 0 | u32 | magic |
| 4 | u32 | rank |
| 8 | u32 | flags（保留 0） |
| 12 | u32 | count |
| 16 | u64 | offset |
| 24 | u64 | total |
| 32 | i64 × rank | shape（完整） |
| 32+8·rank | f32 × count | 数据 |

**`LYIX`（0x5849594C）下标**

| 偏移 | 类型 | 含义 |
|---|---|---|
| 0 | u32 | magic |
| 4 | u32 | count |
| 8 | u32 | total |
| 12 | u32 | flags（保留 0） |
| 16 | u64 | sourceCloudId |
| 24 | i32 × count | 下标 |

两份布局都保证 `shape`（8 字节）与数值数组（4 字节）自然对齐，前端可以直接开
`BigInt64Array` / `Float32Array` / `Int32Array` 视图，零拷贝。

### 3.2 Tauri command 与 HTTP 端点

```rust
#[tauri::command] pub fn get_output_tensor(runId, nodeId, port, offset: Option<u64>, count: Option<u32>) -> Result<tauri::ipc::Response, String>
#[tauri::command] pub fn get_output_indices(runId, nodeId, port, offset: Option<u64>, count: Option<u32>) -> Result<tauri::ipc::Response, String>
```

桥接层把 `count` clamp 到 **4 194 304**（16 MB f32 / 16 MB i32）—— core 不设上限，
上限是「一次 IPC 该多大」的产品判断，属于桥接层。

| 方法 | 路径 | Transport 方法 | C ABI |
|---|---|---|---|
| GET | `/lyflow/runs/:runId/tensors/:nodeId/:port?offset=&count=` | `getOutputTensor` | `lyflow_output_tensor` |
| GET | `/lyflow/runs/:runId/indices/:nodeId/:port?offset=&count=` | `getOutputIndices` | `lyflow_output_indices` |

`docs/http-transport.md` 的端点表一起更新。`static` 传输两个方法都走 `browserOnly("取运行结果")`。

`packages/editor/test-server/server.mjs` 这个**桩**服务器覆盖不到这两个端点：它每次请求起一次
`lyflow` CLI，没有常驻结果仓，点云能work 全靠 CLI 的 `dump` 会写出 PCD 文件，而张量与下标
没有对应的落盘格式。两个端点在桩里返回 **501** 并在契约文档上照实标注（理由见 ADR-0019 的「代价」）。

---

## 4. 前端

### 4.1 新文件与抽取

新增：
```
store/peek.ts                 浮窗集合（zustand，独立于 ui store）
components/EdgePeekLayer.tsx  绝对定位的浮窗层，挂在 GraphCanvas 之上
components/EdgePeek.tsx       单个浮窗：标题栏 + 工具栏 + 视图槽
components/peek/CloudView.tsx     3D / 正交点云 + 2D 几何叠加
components/peek/TensorView.tsx    canvas 2D 上色的张量图像
components/peek/ValueView.tsx     键值表 + 原始 JSON
components/peek/IndicesView.tsx   数量 + 虚拟滚动的下标列表
styles.peek.css
```

从 `Viewer3D.tsx` 抽出来共享（**行为不变**，只是搬家，注释原样带走）：
```
lib/cloudCache.ts   cloudCache / putCache / dropOtherRuns，加 pin 支持
lib/ramps.ts        VIRIDIS / viridisRamp / grayRamp / jetRamp
lib/shapes2d.ts     shapesOf / extentOf / polyline
```

`peek` 单独开 store 而不是塞进 `ui`：浮窗位置在拖动时每帧都变，塞进 `ui` 会让所有订阅
`ui` 的组件（画布、Inspector、工具栏）跟着重渲染。

### 4.2 窗口模型

```ts
interface PeekWindow {
  id: string;
  edgeId: string;
  path: SubPath;                       // 开在哪一层，切层级时用它决定显隐
  from: PortRef;                       // 局部 id，用来判断边还在不在
  screen: { x: number; y: number };
  size: { w: number; h: number };
  z: number;
  locked: { runId: string } | null;    // P8 的快照锁
  view: "cloud3d" | "cloud2d" | "tensor" | "value" | "indices";
  opts: {
    maxPoints: number;                 // 点云
    shading: ShadingMode; ramp: RampName;
    layout: "auto" | "HWC" | "CHW" | "NHWC" | "NCHW";   // 张量
    sliceIndex: number; channel: number | "rgb";
    range: [number, number] | "auto";
  };
}
```

### 4.3 视图选择矩阵

| 运行时类型 | 默认视图 | 可切到 |
|---|---|---|
| `PointCloud` | 3D | 正交 2D、文本（点数/包围盒） |
| `Box2D` `Line2D` `Circle2D` `Point2D` | 正交 2D（沿上游借底图云，复用 `findBaseCloud`） | 文本 |
| `Tensor` | rank ≥ 2 图像；rank 1 折线 | 文本（shape + min/max/mean） |
| `Indices` | 文本列表 | — |
| `Measurement` `Record` `Transform` `Plane` `Error` | 键值表 | 原始 JSON |
| `Any` | 按运行时类型走；没跑过 → 「未运行」 | — |

2D 几何视图必须带底图云：只画一个 `Box2D` 的空白框，看不出它压在剖面的哪里 —— 这正是
Viewer3D 接 gap 时踩过的那条（`docs/gap-acceptance.md §7`）。

### 4.4 张量布局推断

后端只给形状，前端按这套规则猜，猜的结果显示在下拉框里，可手动改：

- rank 1 → 折线图
- rank 2 → `(H, W)` 灰度
- rank 3 → 末维 ∈ {1,3,4} 判 `(H,W,C)`；否则首维 ∈ {1,3,4} 判 `(C,H,W)`；再否则当 `(N,H,W)` 取第 0 张
- rank 4 → 末维 ∈ {1,3,4} 判 `(N,H,W,C)`，否则 `(N,C,H,W)`
- rank ≥ 5 → 不出图，只给文本

控件：布局、N 索引、通道（单通道灰度 / RGB）、归一化（自动 min-max / 手动范围）、色带。
**取数只取当前切片**：先从事件里的 `shape` 算出 `offset/count`，再发请求。
一片超过 16 MB 就提示「这一片太大，缩小切片或换通道」，不强取。

### 4.5 缓存、快照锁与一条硬约束

`cloudCache` 现在是 `Viewer3D` 的模块级 Map，换 run 时 `dropOtherRuns(runId)` 全清。抽到
`lib/cloudCache.ts` 后加一个 pin 集合：**被锁定窗口引用的 runId 不参与清理**，256 MB 预算共享。

⚠️ **硬约束（必须在 UI 上诚实反映）**：桥接层只留**最近一个**已完成的 run 句柄
（`bridge/src/execution.rs:122`），新 run 一开始，旧句柄 drop → `lyflow_run_free` →
`ResultStore::freeRun` 把旧 runId 的索引抹掉（`core/src/exec/result_store.cpp:274`）。
也就是说**锁定快照只能冻结前端已经取到的数据**，锁定之后不能再向后端要这个 run 的新数据。

所以锁定态下：
- 已解码的点云 / 张量切片 / JSON 值照常显示，重跑不变。**冻结的是整个 `PeekSource`**
  （`stat` / `type` / `resolved` / `status` 一起冻），不只是状态文字 —— 只冻状态的话，
  重跑一开始 `src.stat` 就没了，锁定的 `Measurement` 窗会立刻变空，而 `Any` 端口
  （`util.reroute`）的锁定点云窗还会因为类型变回 `null` 被自动纠正推去 `value` 视图。
  顺带的好处：视图取数 effect 的依赖在锁定期间全部恒定，「锁定后不再发请求」是自然结果而不是额外判断；
- 「maxPoints」「换切片」「换 N 索引」这类**需要回后端**的控件置灰，tooltip 写明
  「快照已冻结：这次运行的结果已被回收」；
- 色带、归一化、着色模式、3D↔2D 这些**纯前端**的控件照常可用。

（若日后要「锁定后仍能换切片」，得让桥接层保留最近 N 个 run 句柄 —— 那会按 N 倍占住 core 的
结果仓内存，是另一档决策，不在本轮。）

### 4.6 WebGL 上下文预算

每个 3D/2D 点云窗口一个 `WebGLRenderer` = 一个 WebGL context，浏览器上限约 16 个，
超了会**静默**丢掉最旧的那个（画面变黑，还很难查）。

限额：**同时最多 6 个窗口，其中带 WebGL 的最多 4 个**（主 Viewer3D 自己占 1，总数 5，安全）。
开第 5 个 WebGL 窗时，自动关掉最早的**未锁定** WebGL 窗并 toast 提示。
（上限不够用的话，升级路径是 three.js 官方的「一个 renderer + 每窗一个 2D canvas 做 blit」，
本轮不做。）

---

## 5. 交互细节

**打开**：双击边。该边已有窗口则前置 + 闪一下标题栏，不重复开。

**边的右键菜单**（新增 `onEdgeContextMenu`，现在只有节点有菜单）：
- 查看内容
- 在此插入 Reroute ← 原双击行为搬到这里
- 删除连线

**关闭**：标题栏 ×；`Esc` 关最前面的一个；边被删 / 撤销掉 → 对应窗口自动关闭；
切换子图层级 → 不属于当前层级的窗口自动关闭（与 `enterSubgraph` 清选中同一理由）。

**层叠**：点击窗口任意处置顶。拖标题栏移动，限制在画布可视区内，不允许拖出屏幕。

**刷新**：未锁定窗口订阅 `runId` 与**解析后叶子节点**的 `state`，`done`/`skipped` 后自动重取。
运行中显示「正在计算…」，出错显示该节点的错误。

**工具栏**（P10）：复制 JSON、导出 PNG（复用 `writeFileBytes`）、跳到源节点（设选中 + 画布居中）、
在主 3D 视图打开、锁定快照、关闭。

「在主 3D 视图打开」需要把 `Viewer3D` 的 `pinnedId` 从组件 state（`Viewer3D.tsx:458`）提到
`ui` store —— 一个 `pinnedNode: string | null` 加一个 setter，其余不动。

`docs/interaction-checklist.md` 第 24 条的描述要改（双击插 Reroute → 右键菜单插 Reroute），
并新增一条「双击连线查看内容」。`ShortcutPanel` 补上 Esc 关窗。

---

## 6. 实现顺序（可切给子代理的片）

| 片 | 内容 | 产出可独立验收 |
|---|---|---|
| S1 | core：两个 ABI 入口 + `LYFLOW_ABI_VERSION=8` + doctest | `check:gap` / core 单测 |
| S2 | bridge：`core_ffi` + 两个 command + 两个二进制编码 + rust 测试；`http-transport.md` + test-server | `cargo test`、`e2e:http` |
| S3 | 前端基建：transport 两个方法 + `decodeTensor`/`decodeIndices`；从 Viewer3D 抽出 `cloudCache`/`ramps`/`shapes2d`（行为不变） | `typecheck`，现有 e2e 全绿 |
| S4 | 窗口壳：`peek` store + 浮窗层 + 拖动/层叠/生命周期 + 双击改造 + 边右键菜单 + `ValueView` | 双击任意 JSON 类型的边能看到表格 |
| S5 | `CloudView`：3D / 正交 + 2D 几何叠加 + 底图云 + `IndicesView` | 双击点云边、Box2D 边、Indices 边 |
| S6 | `TensorView`：布局推断 + 切片取数 + 色带归一化 | 双击 `std-ml` 的张量输出边 |
| S7 | 工具栏动作 + 快照锁 + WebGL 预算 + 文档与交互清单更新 + e2e | 全量验收 |

S1/S2 与 S3/S4 之间没有依赖（S3 的 transport 方法可以先按契约写、等 S2 落地再联调），可以并行。

---

## 7. 验收

`scripts/e2e/` 新增 `peek.mjs`：
1. 双击一条 `PointCloud` 边 → 浮窗出现，点数与 Inspector 里那个节点的一致；
2. 双击一条 `Measurement` 边 → 键值表里的值与 Inspector 的 `formatOutputValue` 一致；
3. 双击一条 `Box2D` 边 → 正交视图里几何的包围盒与底图云在同一平面（沿用 gap 那条断言）；
4. 张量边 → 取回的帧头 magic/rank/shape/total 与 `stats` 里的 `shape` 对得上；
5. `Indices` 边 → 列表条数 = `elementCount`；
6. 锁定一个窗 → 改参数重跑 → 锁定窗数值不变，未锁定窗跟着变；
7. 删掉那条边 → 窗口自动关闭；进子图 → 顶层的窗口消失。

外加：`pnpm check`、`pnpm typecheck`、现有 `e2e` 与 `e2e:http` 全绿；
手工确认连开 4 个点云窗 + 主视图不掉 WebGL context。

**不写 UI 单元测试**（项目规则），验收全部走 e2e 与手工。

---

## 8. 明确不做

- 边上常驻摘要徽标（P9）
- 导出数据文件：PLY / CSV / NPY（`lyflow_output_save` 已有能力，但本轮不接）
- Indices 在上游点云上高亮（P6）
- 浮窗跟随画布平移缩放（P2）
- 在浮窗里编辑数据、回写图
- 历史 run 浏览器（受 §4.5 的硬约束限制，要做得先改桥接层的 run 保留策略）

---

## 9. 我替你定的默认值（不同意就改）

1. **rank-1 张量画折线** —— 一维张量在剖面场景里常见，canvas 折线成本极低。不要就退成纯文本。
2. **窗口上限 6，其中 WebGL 窗 4**。
3. **点云窗默认 maxPoints = 200 000**，可选 100k / 200k / 500k / 2M（比主视图少一档 8M：浮窗就那么大。
   200k 必须是正式一档 —— 默认值不在选项里的话，下拉框会显示成 100K 而实际取了 20 万）。
4. **锁定快照 = 冻结已取数据 + 禁用需要回后端的控件**（§4.5 的硬约束所迫）。
