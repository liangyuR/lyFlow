# 预览里点选点、测距离 —— 实施计划

> 状态：**已确认，按 §1 的建议值实施**（2026-09-28）。C 档第 2 项；m2-plan「延后」一节里的「3D 视图里的选点、测量」。
> 前置：compare-plan S1–S6（`lib/cloudScene` 的两栏、`hooks/useViewerSource`）已落地，本计划直接用它们。

## 0. 用例

1. **看一个点的坐标**：调 ROI 时想知道「这条边在 x 等于多少」，现在只能拿框去蹭。
2. **量两点距离**：缝隙宽度、台阶高度的肉眼复核 —— 与 `gap.measure` 的结果对一下量级。
3. **对比模式下量位移**（与 #35 联动）：A 栏点一下、B 栏同一特征点一下 = 两次运行之间这个特征挪了多少。

## 1. 定死的决定（建议值，§6 待你确认）

| # | 决定 | 理由 |
|---|---|---|
| M1 | **工具开关**：工具栏「测量」按钮（`data-testid="viewer-measure"`）+ 快捷键 `M`（scope global，不在输入框里响应）。开着时**单击**选点，**拖动**照旧转视角 | 不抢现有手势：左键拖 = 旋转是肌肉记忆；修饰键点击（Ctrl/Alt）在 WebView2 与画布多选上都有占用 |
| M2 | **一组测量、两个点**：第 1 次点 = P1（显示坐标），第 2 次 = P2（显示距离），第 3 次重新开始一组。「清除」按钮与再按一次工具开关都清掉 | 多组常驻的列表要管理界面、要持久化，收益不大；够用再说 |
| M3 | **只吸附到画面上显示的点**（抽稀后的 `cloud.xyz`），不去后端要全分辨率 | 前端只有这些点；为了拾取回后端取邻域要新 IPC，且抽稀后的点本来就是原云里的真点。readout 标一句「吸附到显示的 N 点」 |
| M4 | **屏幕空间拾取**：把显示的点逐个投到屏幕，取离光标 ≤ 8 CSS px 的最近者；同距（差 < 1 px）取离相机近的 | 与 `THREE.Raycaster` 的世界单位阈值相比，屏幕像素阈值不随缩放、正交 / 透视都一致；200 万点一次约 20 ms，只在单击时算，不做悬停 |
| M5 | **读数**：P1 / P2 的 `x y z`，距离 `|d|`、`Δx Δy Δz`；2D 剖面下再给一个 XY 平面距离。单位按**米**读，同时显示毫米（`0.01235 m · 12.35 mm`），6 位有效数字（`lib/format` 的 `num`） | 点云约定是米（ROI 参数的 mm 标注按 ×0.001 换算，`roiFrames`）；缝隙类数值人习惯看 mm |
| M6 | **显示**：两个标记点（固定像素大小、不被点云遮挡，1 绿 2 粉，与 ROI 框同色系）+ 一条连线；读数放在画面右下角的固定小框（`data-testid="measure-readout"`），不做跟随标记的浮动标签 | 浮动标签在两栏下要按栏各投一次、还要避让；固定框一处就够，标记点本身有编号色 |
| M7 | **生命周期**：换节点（A 的 `display.nodeId` 变）清掉；同一节点重跑**保留**坐标（世界坐标仍有意义），readout 标「云已更新，点位是之前选的」；进入 2D 拖框时关掉工具（与 C8 同理：拖框独占指针） | 重跑后常常就是想对比同一位置；换节点后旧点位没有语境 |
| M8 | **对比模式**：在哪一栏点，就吸附到那一栏的云；标记点**两栏都画**（同一台相机、同一个世界坐标），readout 标 P1 / P2 各来自 A 还是 B | 用例 3 直接成立：A 点一下、B 点一下 = 位移 |

## 2. 实现

### 2.1 纯函数 `lib/pick.ts`

```ts
/** 屏幕空间最近点。matrix = camera.projectionMatrix × camera.matrixWorldInverse（点云的 world 矩阵恒为单位阵）。
 *  viewport 是这一栏在画布里的矩形（CSS px，左上为原点）；click 同坐标系。非有限坐标（NaN 槽）跳过。 */
export function pickNearest(
  xyz: Float32Array, count: number, matrix: ArrayLike<number>,
  viewport: { x: number; y: number; w: number; h: number },
  click: { x: number; y: number }, radiusPx?: number,
): { index: number; distPx: number } | null;

export interface Measure { p1: Pick | null; p2: Pick | null; stale: boolean; }
export interface Pick { xyz: [number, number, number]; pane: 0 | 1; }
/** readout 的几行文字；2D 模式多一行 XY 距离。 */
export function measureLines(m: Measure, mode: CameraMode): { key: string; text: string }[];
```

不依赖 three（矩阵传 16 个数），node:test 直接测。

### 2.2 场景（`lib/cloudScene.ts`）

- 新增 `measure: THREE.Group`（不属于任何一栏的 `overlays[i]`，两栏都可见）：两个点（`PointsMaterial`，`sizeAttenuation: false`、`depthTest: false`、`renderOrder` 高于底图）与一条 `Line`（同样 `depthTest: false`）。
- `paneAt(clientX, clientY) → { pane, rect } | null`：单栏返回 0；两栏按 `split` 判左右 / 上下。`paneRect` 已经有了，导出即可。
- `dispose` 里一起释放。

### 2.3 `Viewer3D`

- state：`measuring: boolean`、`measure: Measure`。
- 画布上 `pointerdown` 记位置与时间，`pointerup` 位移 < 4 px 且 < 400 ms 才算单击 → `paneAt` → 取那一栏的 `scene.points[pane]` 的 `position` 数组 → `pickNearest` → 写 P1 / P2。与 OrbitControls 并存（它照常收到事件，单击不产生旋转）。
- 标记点随 `measure` 重建；`display.nodeId` 变 → 清；`cloud` 变而节点不变 → `stale = true`。
- `roiEditing` 为真时按钮置灰、工具关；`stageContent === "value"` 时按钮不显示。
- 根上的数据属性：`data-measure="0|1|2"`（已选几个点）、`data-measure-p1` / `-p2`（`x,y,z` round3 前的 6 位有效数字）、`data-measure-dist`、`data-measure-panes`（如 `A,B`）。
- 快捷键 `M`：`lib/keymap.ts` 加 `{ id: "measure", keys: ["M"], label: "测量（选点 / 测距）", scope: "global", group: "视图" }`，`useShortcuts` 经 ui store 的一个开关转给 `Viewer3D`（`ui.viewerMeasuring`，与 `viewerMode` 同处，参数面板进拖框时顺手关掉）。

## 3. 实施顺序

| 片 | 内容 | 测试 |
|---|---|---|
| P1 | `lib/pick.ts`（`pickNearest`、`measureLines`） | 新 `test/pick.test.mjs`，表驱动：半径内最近、半径外 null、相机背后跳过、NaN 跳过、同距取近、两栏时 B 栏视口偏移、2D 多一行 XY、单位换算 |
| P2 | 场景标记组 + `paneAt`；`Viewer3D` 的工具开关、单击判定、readout；keymap 一行 | `pnpm typecheck`；现有 e2e `m3 / m4 / compare / m8b` 回归（指针事件与拖框共存） |
| P3 | e2e + 文档 | `scripts/e2e/m3.mjs` 的视图分组里加一组（开工具 → 在画布中心附近两次单击 → `data-measure="2"`、距离 = 两点坐标之差的模、拖动不选点、换节点清掉、2D 下多 XY 行）；`compare.mjs` 加一句（A 栏、B 栏各点一次 → `data-measure-panes="A,B"`）；interaction-checklist 登记、`docs/testing.md` |

约 1–1.5 天。

## 4. 明确不做

- 多组测量常驻、测量结果导出 / 写回参数（「把这个点填进 ROI」是另一个功能）。
- 角度、面积、点到平面距离。
- 悬停高亮 / 悬停读数（每次 mousemove 投 200 万点不划算）。
- 全分辨率邻域拾取（M3）。
- Edge Peek 的 `CloudView` 里测量（它有自己那份场景代码，compare-plan §7 已记「改用 cloudScene 另起一片」，做了那片之后自然就有）。

## 5. 风险

1. **单击与 OrbitControls 的阻尼**：`enableDamping` 下松手后相机还会滑一小段，单击判定只看按下到松开之间的位移，不受影响；但单击后标记点坐标是世界坐标，不随滑动错位。
2. **大云拾取耗时**：8M 点约 80 ms，单击可接受；超过 2M 时在 readout 旁显示「计算中…」一帧即可，不做 worker。
3. **NaN 槽**：gap 的有序云里有 NaN（底图代码已处理过同一个坑），`pickNearest` 必须跳过非有限值。

## 6. 已确认（2026-09-28，按建议值）

1. 开关：工具按钮 + `M`。
2. 一组两点，第 3 次重来。
3. 米 + 毫米并列。
4. 重跑后保留点位并标「云已更新」。
