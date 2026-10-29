# glue 测量算子契约（1.2 系列）

[文档首页](README.md) · [glue 包说明](../packs/glue/README.md) · [测试地图](testing.md)

状态：**契约草案，lyFlow 与 GlueSight 并行开发中**。改动契约前先在两边汇报，再改本文件与实现。

背景见 GlueSight 仓库 `docs/decisions/lyflow-operators.md`（LF-1 / LF-2）：lyFlow 只提供确定性的图像测量算子，
所有门限都是参数，输出完整的中间结果；流程编排、有无胶 / 像不像胶 / 方向回退 / 每站取哪一段等判定，
以及参数分层与发布冻结都归 GlueSight（下称 GS）。

本文件列出 1.2 系列新增的 6 个算子：每个算子的 id、端口、全部参数（名称、类型、单位、默认值、范围）与输出字段。
1.1 的 `glue.bead_path`、`glue.taught_path`、`glue.bead_width`、`glue.bead_breaks`、`glue.edge_distance`、`glue.judge`
与飞拍的 `glue.locate`、`glue.station_calipers` **原样保留**（语义、参数、输出一位不变），供示例图、飞拍宿主和旧工具继续使用；
GS 从 1.2 起不再使用前三个的语义。

## 总览

```
                    ┌→ glue.fan_scan ──fan──┐
io.load_image → glue.response ─response─────┼→ glue.track_path ─line─→ glue.lateral_runs
                    └──gray / level─────────┴──────────────────────────↗
glue.polyline（示教折线）─line─→ glue.lateral_runs / glue.part_edges
io.load_image ──image──────────────────────────────────────────────→ glue.part_edges
```

| id | 名称 | 输入 → 输出 | 一步做什么 |
|---|---|---|---|
| `glue.response` | 响应图 | `image` → `response`、`gray`、`level` | 整幅（或 roi）黑 / 白顶帽，附带陡度用的平滑灰度与局部亮度 |
| `glue.fan_scan` | 扇形扫描 | `response` → `scan`、`overlay` | 每个方向一条射线上的平均响应；可选前 N 个峰 |
| `glue.track_path` | 粗跟踪与拟合 | `response`、`gray`、`fan?` → `line`、`track`、`overlay` | 沿一个方向逐步找胶点，稳健拟合光滑胶路 |
| `glue.lateral_runs` | 横向卡尺 | `response`、`gray`、`line`、`level?` → `runs`、`overlay` | 沿胶路布站，每站卡尺上的全部暗段 |
| `glue.part_edges` | 找零件边 | `image`、`line` → `edges`、`overlay` | 沿胶路布站，每站两侧的全部合格零件边跳变 |
| `glue.polyline` | 折线胶路 | （无）→ `line`、`overlay` | 把一条参数给定的折线重采样成胶路（示教走廊用） |

算子只测量：**不输出 ok、不判有无胶、不挑候选、不算参考宽、不做站间跟踪**。没有胶、找不到峰都不是执行错误，
而是输出里的空数组或 `null`（ADR-0016 错误即值）。执行错误只有两类：输入图不合格（`bad_input`，指到端口）与加载期参数非法（`bad_param`，指到参数）。

## 约定

- **坐标**：图像像素，x 向右、y 向下，像素中心在整数坐标。所有长度、位置的单位是 px；新算子**不接标定**，换算 mm 由 GS 做。
- **角度**：度，图像坐标，0° 朝右、90° 朝下、−90° 朝上（与 1.1 的 `bead_path` 一致）。
- **胶路**：一条 `glue.Polyline`（见下）。`s` 是沿胶路的弧长（px），升序；`tangent` 是单位切向，
  法向 `n = (−t.y, t.x)` 指向行进方向的**右侧**。卡尺与找边上的偏移（`lo`、`hi`、`mid`、`t`）都沿 `n` 量，右正左负
  （`part_edges` 的两侧各自从胶路往外量，见该节）。
- **布站**：`lateral_runs` 与 `part_edges` 用同一套规则，同一条胶路、同样的 `stationStep` / `sRange` 得到逐一对齐的站：
  `sRange = [0, 0]` 表示整条胶路 `[s₀, s_last]`；否则取 `[a, b] ∩ [s₀, s_last]`。站在 `s = a + k·stationStep`（k = 0, 1, …，`s ≤ b + 1e-6`）。
  站上的点与切向按 `s` 在折线上线性插值（切向重新归一化）。胶路为空或交集为空时没有站。
- **f32 图**：`glue.response` 输出的三张图都是单通道 f32、与输入同尺寸；roi 之外的像素是 NaN。下游采样（双线性）
  四个邻点里有 NaN 或落出图外即视为**无效采样**。
- **输入图**：`glue.response` 与 `glue.part_edges` 的 `image` 接受全分辨率 u8 灰度 / RGB / RGBA（三通道按 BT.601 转灰度，与 1.1 相同）；
  其他位深先接 `image.normalize`；预览缩小图（`scale > 1`）报 `bad_input`。`response` / `gray` / `level` 端口要求单通道 f32、`scale = 1`，
  否则 `bad_input`。
- **JSON 数值**：坐标、偏移保留 3 位小数；取不到的值是 `null`，不用 NaN 或 −1。
- **确定性**：全部算子 `deterministic = true`，同输入同参数逐位相同。

### `glue.Polyline`（Record，沿用 1.1 格式）

```json
{ "count": 151, "spacing": 2, "s": [0, 2, …], "points": [[x, y], …], "tangents": [[tx, ty], …] }
```

`count = 0` 时三个数组为空（例如 `track_path` 取的峰不存在）。

## 1. `glue.response` 响应图

**为什么做成独立算子**：扇形扫描、粗跟踪（3 个候选方向 + 回退方向）和横向卡尺都要同一张响应图。1.1 里 `bead_path` 与 `bead_width`
各算一次；做成下游参数会让每个下游节点各算一遍（整幅顶帽一次约几毫秒，见文末实测）。独立算子只算一次，经 `shared_ptr` 零拷贝传给下游，
在编辑器里也能直接看。陡度用的平滑灰度与局部亮度在同一节点顺带算出，极性只在这里设一次，下游不再有 `polarity` 参数。

| 端口 | 类型 | 说明 |
|---|---|---|
| 入 `image` | Image | 见「约定 / 输入图」 |
| 出 `response` | Image（f32 × 1） | 暗胶：闭运算 − 原图（黑顶帽）；亮胶：原图 − 开运算（白顶帽）；再做 5×5 高斯（`responseSigma`）。≥ 0 |
| 出 `gray` | Image（f32 × 1） | 原图做 5×5 高斯（`graySigma`）；亮胶时取 255 − g，使「胶比背景暗」恒成立。边缘陡度用它 |
| 出 `level` | Image（f32 × 1） | 局部亮度：暗胶取闭运算、亮胶取原图，再做 5×5 高斯（`levelSigma`）。处处 ≥ response（σ 相同时） |

| 参数 | 类型 | 单位 | 默认 | 范围 | 说明 |
|---|---|---|---|---|---|
| `polarity` | enum `dark` / `bright` | — | `dark` | — | 胶比背景暗（黑顶帽）还是亮（白顶帽） |
| `diameter` | int | px | 91 | 3–401 | 结构元直径。比它窄的暗带才有完整响应，应大于期望最大胶宽（1.1 用的是 `widthMax + 1`） |
| `roi` | vec4 `[x, y, w, h]` | px | `[0, 0, 0, 0]` | w、h 同为 ≤ 0（整幅）或同为 > 0 | 只算这一块（与图像求交）；块内的值与整幅计算逐位相同，块外为 NaN |
| `responseSigma` | float | px | 1.2 | 0–3 | 响应图的 5×5 高斯 σ；0 = 不平滑 |
| `graySigma` | float | px | 1.0 | 0–3 | `gray` 的 5×5 高斯 σ；0 = 不平滑 |
| `levelSigma` | float | px | 1.2 | 0–3 | `level` 的 5×5 高斯 σ；0 = 不平滑 |

固定（不开放）：结构元是近似圆盘的正八边形（方形 ⊕ 两条对角线段，比 OpenCV 椭圆核快三十多倍）；高斯核固定 5×5；roi 内部按两倍结构元半径 + 4 外扩计算。
八边形拆成三次形态学串着做，离图边两倍半径以内闭运算可能比原图低一点（1.1 的 `responseField` 也是这样，那里会出负响应）；
`glue.response` 把负值截到 0 再平滑，所以 `response` 处处 ≥ 0，离图边远的地方与 1.1 逐位相同。

## 2. `glue.fan_scan` 扇形扫描

| 端口 | 类型 | 说明 |
|---|---|---|
| 入 `response` | Image（f32 × 1） | `glue.response.response` |
| 出 `scan` | Record `glue.FanScan` | 见下 |
| 出 `overlay` | Record `lyflow.overlay2d` | role：`anchor`、`sector`、`radial`（两段圆弧）、`peak`（峰方向的射线，label 为序号与得分） |

| 参数 | 类型 | 单位 | 默认 | 范围 | 说明 |
|---|---|---|---|---|---|
| `anchor` | vec2 | px | `[640, 512]` | — | 锚点（出胶点） |
| `sector` | vec2 `[from, to]` | deg | `[-180, 180]` | from < to，跨度 ≤ 360 | 扫描的方向范围 |
| `radial` | vec2 `[r0, r1]` | px | `[100, 250]` | 0 ≤ r0 < r1 | 每条射线上采样的径向区间 |
| `angleStep` | float | deg | 1 | 0.1–30 | 方向步长 |
| `radialStep` | float | px | 2 | 0.5–50 | 射线上的采样步长 |
| `invalidMax` | float | — | 0.3 | 0–1 | 一条射线上无效采样占比 ≥ 它时，这个方向的得分为 `null` |
| `peakCount` | int | — | 3 | 0–20 | 输出前几个峰；0 = 不输出峰 |
| `peakSeparation` | float | deg | 15 | 0–180 | 峰之间的最小角差（严格大于，见下） |

**计算**：方向 `aₖ = from + k·angleStep`（`aₖ ≤ to + 1e-6`）；每个方向在 `t = r0 + j·radialStep`（`t < r1`）上双线性采样响应，
得分 = 有效采样之和 / 总采样数（无效采样按 0 计入分母）。**峰**：把有得分的方向按得分从高到低排（同分保持角度顺序），
依次取与已取的每个峰角差都 **> `peakSeparation`** 的（与 1.1 `bead_path` 的取法相同），取满 `peakCount` 个为止。
角差按圆周取（−170° 与 170° 相差 20°）。

`glue.FanScan`：

| 字段 | 说明 |
|---|---|
| `anchor`、`sector`、`radial`、`angleStep`、`radialStep` | 回显参数 |
| `samples` | 每条射线的采样数 |
| `angles[]` | 各方向（度） |
| `scores[]` | 各方向得分；无效方向 `null` |
| `invalid[]` | 各方向的无效采样数 |
| `peaks[]` | `{index, angle, score}`，按得分降序；`index` 是在 `angles` 里的下标 |

1.1 `bead_path` 的扇形搜索等价于 `radial = [zoneStart, zoneStart + min(150, zoneEnd − zoneStart)]`、其余参数取默认值；
给了 heading 的回退等价于 `sector = [heading − headingTol, heading + headingTol]`。

## 3. `glue.track_path` 粗跟踪与拟合

从锚点沿起始方向、在检测区 `zone` 里每 `coarseStep` 走一步，每步在法向卡尺上找离预测点最近的暗段作胶点，再局部线性稳健拟合、
高斯平滑成一条覆盖整个检测区的光滑胶路。**不输出 ok**：覆盖率、陡度够不够、选哪个候选方向都由 GS 判。

| 端口 | 类型 | 说明 |
|---|---|---|
| 入 `response` | Image（f32 × 1） | `glue.response.response` |
| 入 `gray` | Image（f32 × 1） | `glue.response.gray`（逐点陡度与重新接上时的陡度门） |
| 入 `fan`（可选） | Record `glue.FanScan` | 接了：起始方向 = `fan.peaks[peakIndex].angle`，`heading` 参数不用 |
| 出 `line` | Record `glue.Polyline` | 拟合胶路：第一个点离锚点恰好 `zone[0]`，之后按平滑胶路的真实弧长每 2 px 一点，最后一点 `s = zone[1]`；s 从锚点量起 |
| 出 `track` | Record `glue.Track` | 见下 |
| 出 `overlay` | Record `lyflow.overlay2d` | role：`anchor`、`coarse`（胶点）、`path`（拟合胶路）或 `ray`（没拟合时的直线） |

| 参数 | 类型 | 单位 | 默认 | 范围 | 说明 |
|---|---|---|---|---|---|
| `anchor` | vec2 | px | `[640, 512]` | — | 锚点（出胶点） |
| `heading` | float | deg | −90 | — | 起始方向（没接 `fan` 时用） |
| `peakIndex` | int | — | 0 | 0–19 | 接了 `fan` 时取第几个峰 |
| `zone` | vec2 `[start, end]` | px | `[100, 400]` | 0 ≤ start，end ≥ start + 8 | 检测区：沿胶路离锚点的弧长 |
| `bearingRange` | vec2 `[from, to]` | deg | `[-180, 180]` | from < to，跨度 ≤ 360 | 胶点相对锚点的方位范围；跨度 ≥ 360 时不查方位 |
| `bearingMargin` | float | deg | 20 | 0–180 | 胶点方位可超出 `bearingRange` 中心 ± 半跨度的余量 |
| `outwardRatio` | float | — | 0.75 | 0–1 | 胶点离锚点至少 `outwardRatio × s` |
| `searchHalf` | float | px | 90 | 4–400 | 卡尺半长（1.1 = `widthMax`） |
| `split` | enum `connected` / `half_max` | — | `connected` | — | 卡尺暗段的拆分方式，见第 4 节（1.1 跟踪用 `connected`） |
| `segmentMin` | float | 灰度级 | 8 | ≥ 0 | 分段门限：响应 > 它的相邻采样连成一段 |
| `peakMin` | float | 灰度级 | 16 | ≥ 0 | 段（及半高子段）的峰值至少这么高 |
| `mergeGap` | float | px | 8 | ≥ 0 | 合并缝 |
| `mergePeakRatio` | float | — | 0 | 0–1 | 合并峰值比：两段较淡者峰值 ≥ 它 × 较浓者才合并；0 = 不查 |
| `mergeWidthMax` | float | px | 90 | ≥ 1 | 合并后不宽于它才合并 |
| `widthMin` | float | px | 4 | ≥ 0 | 胶点暗段宽度下限 |
| `widthMax` | float | px | 90 | ≥ widthMin | 胶点暗段宽度上限 |
| `acceptOffsetRatio` | float | — | 0.6 | 0–1 | 胶点中心离预测点 < `acceptOffsetRatio × widthMax` 才接受（同时是退回射线的触发条件） |
| `coarseStep` | float | px | 8 | 1–100 | 步长 |
| `maxGap` | float | px | 120 | ≥ 0 | 找到过胶点后连续这么远没找到，就不再接受新胶点 |
| `gapSteps` | float | 步 | 1.5 | ≥ 1 | 离上一个胶点超过 `gapSteps × coarseStep` 算「断口之后」 |
| `historyMin` | int | 个 | 3 | 1–100 | 已有至少这么多胶点后，下面几道连续性门才生效 |
| `jumpRatio` | float | — | 0.5 | ≥ 0 | 紧接上一步时，胶点横向离预测点 ≤ max(`jumpRatio` × 已有胶宽中位数, `jumpMin`) |
| `jumpMin` | float | px | 8 | ≥ 0 | 同上 |
| `reacquirePeakRatio` | float | — | 0.5 | ≥ 0 | 断口之后重新接上的暗段峰值 ≥ 它 × 已有峰值中位数 |
| `reacquireWidthRatio` | float | — | 0.5 | ≥ 0 | 已有胶宽稳定时，重新接上的暗段宽度 ≥ 它 × 已有胶宽中位数 |
| `steadyIqrRatio` | float | — | 0.3 | ≥ 0 | 已有胶宽 IQR < 它 × 中位数算「稳定」（直胶） |
| `sharpMin` | float | — | 0.4 | — | 断口之后重新接上的暗段边缘陡度（可算出时）≥ 它；也用于下面的退回射线条件 |
| `rayFallback` | bool | — | true | — | 断口之后跟丢时退回起始方向的射线上找 |
| `rayFallbackCoverage` | float | — | 0.6 | 0–1 | 退回射线的前提：已有胶点 ≥ 它 × 到上一个胶点为止的步数，且已有胶点的陡度 P25 ≥ `sharpMin`（至少 3 个可算陡度） |
| `edgeProbe` | float | px | 3 | 0.5–20 | 陡度：每条边内外各取这么远的灰度差 |
| `backgroundProbe` | float | px | 8 | 1–50 | 陡度：暗度的背景取两边外这么远 |
| `dirWindow` | float | px | 48 | ≥ 0 | 用最近这么长 s 内的胶点（≥ 3 个）做主方向 |
| `minPoints` | int | 个 | 3 | 3–1000 | 胶点不少于它才拟合，否则输出沿起始方向的直线 |
| `fitWindow` | float | px | 40 | ≥ 1 | 局部线性拟合的半窗（沿 s） |
| `fitSigma` | float | px | 12 | 0–100 | 拟合后沿胶路的高斯平滑 σ（核半宽 ⌈3σ⌉）；0 = 不平滑 |

每步的卡尺按第 4 节的方法在 `t ∈ [−searchHalf, searchHalf]` 上取暗段（`split`、`segmentMin`、`peakMin`、`mergeGap`、`mergePeakRatio`、`mergeWidthMax`），
再只留宽度在 `[widthMin, widthMax]` 的；连续性门过滤后取中心离预测点最近的一段，中心离预测点 < `acceptOffsetRatio × widthMax`
且方位、外行条件成立就是胶点。跟踪逻辑与 1.1 `bead_path` 的 `coarseTrack` 一一对应，只是门限开放。

固定（不开放）：拟合 3 轮、残差 ≥ 3·MAD + 2 的胶点权重降到 0.1；窗内不足 3 点时取最近的 3–8 点；拟合网格 1 px、向两头多走 48 / 56 px；
第一个胶点离 `zone[0]` 超过 2 步时，之前那段取锚点到它的连线；主方向用 PCA；输出折线间距 2 px。

`glue.Track`：

| 字段 | 说明 |
|---|---|
| `headingSource` | `param` / `fan` |
| `peakIndex` | 接了 fan 时的峰序号，否则 `null` |
| `valid` | 接了 fan 但没有第 `peakIndex` 个峰时为 false：这时 `line` 为空，下面的统计全是 `null` / 空 |
| `heading` | 起始方向（度） |
| `anchor`、`zone`、`bearingRange` | 回显 |
| `steps` | 走了几步 |
| `found` | 胶点个数 |
| `coverage` | `found / steps` |
| `sharpness` | 各胶点陡度（可算出的）的第 25 百分位；可算出的不到 3 个为 `null` |
| `fitted` | 胶点 ≥ `minPoints` 时 true |
| `lineSource` | `fit` / `ray`（没拟合，`line` 是从锚点沿起始方向的直线，同样覆盖检测区）/ `none`（`valid = false`） |
| `residual` | 胶点到拟合胶路的距离中位数（px）；没拟合为 `null` |
| `stoppedAt` | 因 `maxGap` 停止接受胶点时的 s；没停为 `null` |
| `points[]` | `{s, point, mid, width, peak, sharpness, onRay}`：s 是这一步的 s，`point` 胶点坐标，`mid` 胶点相对预测点的横向偏移，`width` / `peak` 胶点暗段的宽度与峰值，`sharpness` 逐点陡度（取不到为 `null`），`onRay` 是否退回射线上找到 |

1.1 `bead_path` 的判定在 GS 侧可写成：`coverage ≥ minCoverage(0.3)`、`sharpness ≥ sharpMin(0.4)`、`found ≥ 3`；
候选得分 = `coverage × (像胶 ? 1 : 0.1)`，`fan.peaks[i].score < 0.25 × peaks[0].score` 时再 × 0.5，取最高者。

## 4. `glue.lateral_runs` 横向卡尺

沿胶路布站，每站在法向 `t ∈ [−searchHalf, searchHalf]` 上每 0.5 px 采样响应，输出**全部**暗段。不按宽度筛、不挑段、不算参考宽、不判有无胶。

| 端口 | 类型 | 说明 |
|---|---|---|
| 入 `response` | Image（f32 × 1） | `glue.response.response` |
| 入 `gray` | Image（f32 × 1） | `glue.response.gray`（边缘陡度） |
| 入 `line` | Record `glue.Polyline` | `track_path.line` 或 `polyline.line` |
| 入 `level`（可选） | Image（f32 × 1） | `glue.response.level`；不接时 `level` 字段为 `null` |
| 出 `runs` | Record `glue.LateralRuns` | 见下 |
| 出 `overlay` | Record `lyflow.overlay2d` | role：`path`、`station`（卡尺线）、`run`（每段从 lo 到 hi） |

| 参数 | 类型 | 单位 | 默认 | 范围 | 说明 |
|---|---|---|---|---|---|
| `stationStep` | float | px | 4 | 0.5–200 | 站距 |
| `sRange` | vec2 | px | `[0, 0]` | — | 布站范围，见「约定 / 布站」 |
| `searchHalf` | float | px | 60 | 1–600 | 卡尺半长 |
| `split` | enum `half_max` / `connected` | — | `half_max` | — | 拆分方式，见下 |
| `segmentMin` | float | 灰度级 | 8 | ≥ 0 | 分段门限 |
| `peakMin` | float | 灰度级 | 16 | ≥ 0 | 段峰值下限 |
| `mergeGap` | float | px | 8 | ≥ 0 | 合并缝 |
| `mergePeakRatio` | float | — | 0.4 | 0–1 | 合并峰值比；0 = 不查 |
| `mergeWidthMax` | float | px | 90 | ≥ 1 | 合并后宽度上限 |
| `edgeProbe` | float | px | 3 | 0.5–20 | 陡度：每条边内外各取这么远的灰度差 |
| `backgroundProbe` | float | px | 8 | 1–50 | 陡度：暗度的背景取两边外这么远 |

**分段与拆分**：

1. 有效且 > `segmentMin` 的相邻采样连成一个**连通段**；连通段峰值 < `peakMin` 的丢掉。
2. 在每个连通段里按它自己的半高 `lvl = 0.5 × 连通段峰值` 取边：
   - `connected`：从第一个 ≥ lvl 的采样到最后一个 ≥ lvl 的采样算**一段**（中间低于 lvl 的也包在里面）；
   - `half_max`：每一串连续 ≥ lvl 的采样各算一个**子段**（中间降到 lvl 以下就拆开）；子段峰值 < `peakMin` 的丢掉。
   边界在 lvl 处线性插值到亚像素。
3. 按 t 从小到大，相邻两段缝 `lo₂ − hi₁ ≤ mergeGap`、合并后宽度 ≤ `mergeWidthMax`、且（`mergePeakRatio > 0` 时）较淡者峰值 ≥ `mergePeakRatio` × 较浓者时合并
   （高光条把一条胶劈成两半的情形）。

1.1 的等价设置：拟合胶路上的 `bead_width` = `connected` + `mergePeakRatio = 0`；示教胶路（直胶）= `half_max` + `mergePeakRatio = 0.4`；
两者都是 `segmentMin = contrastMin / 2`、`peakMin = contrastMin`、`mergeWidthMax = widthRange 上限`，之后再按 `widthRange` 筛宽度（GS 做）。

`glue.LateralRuns`：

| 字段 | 说明 |
|---|---|
| `count`、`stationStep`、`sRange`（实际用的）、`searchHalf`、`split` | 回显 |
| `stations[]` | 每站 `{s, center, normal, validFraction, runs[]}`：`center` 站点坐标，`normal` 单位法向，`validFraction` 卡尺采样中有效的比例 |
| `stations[].runs[]` | 按 `lo` 升序，每段 `{lo, hi, width, mid, peak, parts, loComplete, hiComplete, edgeLo, edgeHi, depth, sharpness, level}` |

每段字段：

| 字段 | 说明 |
|---|---|
| `lo`、`hi` | 两条边沿法向的偏移（px），`lo < hi` |
| `width` | `hi − lo` |
| `mid` | 中心偏移 `(lo + hi) / 2`（相对胶路，沿法向） |
| `peak` | 段内响应峰值 |
| `parts` | 合并进来的子段个数（≥ 1） |
| `loComplete` / `hiComplete` | 这条边外侧的采样有效且已降到半高以下（没被卡尺端点或无效区截断）。1.1 的 `completeEdges` = 两者都为真 |
| `edgeLo` | `g(lo − edgeProbe) − g(lo + edgeProbe)`（g 为 `gray` 图，胶暗时为正） |
| `edgeHi` | `g(hi + edgeProbe) − g(hi − edgeProbe)` |
| `depth` | `max(1, (g(lo − backgroundProbe) + g(hi + backgroundProbe)) / 2 − 段中间一半上 5 个采样的均值)` |
| `sharpness` | `min(edgeLo, edgeHi) / depth`（1.1 的 D15 陡度）。任一采样无效时 `edgeLo`、`edgeHi`、`depth`、`sharpness` 全为 `null` |
| `level` | `level` 图在 `t = lo, lo + 1, …（< hi）` 与 `t = hi` 上采样的最大值；没接 `level` 或全无效时 `null`。1.1 示教胶路的对比度 = `peak / max(level, 1)` |

## 5. `glue.part_edges` 找零件边

沿胶路布站（规则同第 4 节），每站向两侧沿法向取灰度剖面，输出**全部**合格的零件边跳变。不投票选侧、不剔野、不算距离 —— 这些归 GS。

| 端口 | 类型 | 说明 |
|---|---|---|
| 入 `image` | Image | 与 `glue.response` 同一帧（原始灰度采样，不做二维平滑） |
| 入 `line` | Record `glue.Polyline` | 胶路 |
| 出 `edges` | Record `glue.PartEdges` | 见下 |
| 出 `overlay` | Record `lyflow.overlay2d` | role：`path`、`part`（每个跳变点，label 为序号） |

| 参数 | 类型 | 单位 | 默认 | 范围 | 说明 |
|---|---|---|---|---|---|
| `stationStep` | float | px | 4 | 0.5–200 | 站距 |
| `sRange` | vec2 | px | `[0, 0]` | — | 布站范围 |
| `sides` | enum `both` / `right` / `left` | — | `both` | — | 找哪一侧（right = +n） |
| `start` | float | px | 0 | ≥ 0 | 剖面从离胶路多远开始 |
| `searchLength` | float | px | 200 | 20–2000 | 剖面长度（从 `start` 起） |
| `polarity` | enum `bright_to_dark` / `dark_to_bright` | — | `bright_to_dark` | — | 从胶往外看，零件边是亮 → 暗还是暗 → 亮 |
| `contrastMin` | float | 灰度级 | 40 | ≥ 1 | 近侧均值 − 远侧均值至少这么大 |
| `farRun` | float | px | 12 | 1–200 | 远侧要持续这么长 |
| `farDropRatio` | float | — | 0.5 | 0–1 | 近侧均值 − 远侧最大值 ≥ 它 × `contrastMin`（远侧持续暗下去，划痕、反光条过不去） |
| `descentRatio` | float | — | 0.05 | 0–1 | 候选点的下降率 > 它 × `contrastMin` |

**计算**（每站每侧，与 1.1 `findPartEdge` 相同，只是不在第一个处停下）：从 `t = start` 起每 1 px 采样到 `start + searchLength`，出图即截断；
`dark_to_bright` 时取 255 − g；做 7 抽头 σ = 1.5 的一维高斯（两头按端值延拓）。对剖面下标 k（k ≥ 5，k + 2 + farRun ≤ 长度），
下降率 `d(k) = (p[k−1] − p[k+1]) / 2`；`d(k)` 是局部最大（`d(k−1) ≤ d(k)`、`d(k+1) < d(k)`）、`d(k) > descentRatio × contrastMin`，
近侧均值（p[k−5 … k−2]）− 远侧均值（p[k+2 … k+1+farRun]）≥ `contrastMin`，近侧均值 − 远侧最大值 ≥ `farDropRatio × contrastMin`，即为一个跳变；
位置按三点抛物线细化（±0.5）。

`glue.PartEdges`：

| 字段 | 说明 |
|---|---|
| `count`、`stationStep`、`sRange`、`sides`、`start`、`searchLength`、`polarity` | 回显 |
| `stations[]` | 每站 `{s, center, normal, right, left}`；没找的那一侧为 `null` |
| `right` / `left` | `{length, edges[]}`：`length` 实际采到的剖面长度（出图截断后），`edges[]` 按 t 升序 |
| `edges[]` | `{t, point, contrast, farMargin, descent}`：`t` 从胶路沿这一侧往外的距离（px，> 0），`point` 跳变点坐标，`contrast` 近侧 − 远侧均值，`farMargin` 近侧均值 − 远侧最大值，`descent` 下降率 |

1.1 `edge_distance` 的等价做法（GS 侧）：某侧胶边在 `e`（沿这一侧往外的距离，例如右侧 = 选中暗段的 `hi`、左侧 = `−lo`）时，取该侧第一个
`t ≥ e + 3 + 5` 的跳变（1.1 从 `e + 3` 起剖面、k ≥ 5；剖面起点不同只影响起点 3 px 内的一维平滑边界）；再按多数站选侧、7 站滑动中值 + 4·MAD + 2 剔野。

## 6. `glue.polyline` 折线胶路

| 端口 | 类型 | 说明 |
|---|---|---|
| 出 `line` | Record `glue.Polyline` | s 从第一个点沿折线量起，取 `zone` 那一段，每 2 px 一点（两端各一点），切向为中心差分 |
| 出 `overlay` | Record `lyflow.overlay2d` | role：`taught`（整条折线）、`path`（zone 那一段） |

| 参数 | 类型 | 单位 | 默认 | 范围 | 说明 |
|---|---|---|---|---|---|
| `points` | text（JSON `[[x, y], …]`） | px | `[]` | ≥ 2 个有限点，总长 ≥ 8 | 折线；相邻重合点跳过 |
| `zone` | vec2 `[start, end]` | px | `[0, 0]` | 0 ≤ start < end ≤ 总长；end = 0 表示到末端 | 取哪一段 |

与 1.1 `taught_path` 的重采样逐位相同（`resampleTaught`）；不带 tolerance、sharpMin 等示教语义。

## 一张逐帧定向图的连法（参考）

GS 的模板由 GS 维护；下面只说明算子怎么接。图参数经 `binds` 一处写入多个节点（例如 `anchor` 绑 `n_fan.anchor`、`n_t0.anchor`…）。

```
n_load(io.load_image) → n_resp(glue.response)
n_resp.response → n_fan(glue.fan_scan)
n_resp.response, n_resp.gray, n_fan.scan → n_t0 / n_t1 / n_t2 (glue.track_path, peakIndex 0 / 1 / 2)
n_resp.response, n_resp.gray, n_tK.line, n_resp.level → n_rK (glue.lateral_runs)
回退：n_resp.response → n_fanB(glue.fan_scan, sector = 示教方向 ± 容差) → n_tB → n_rB
找边：n_load.image, n_tK.line → n_eK (glue.part_edges)
```

一次运行算完所有候选，GS 读各 `track` / `runs` / `edges` 记录后自己判定与选取，不需要第二次运行。

## 嵌入编辑器：只读拓扑的流程调试页（GS LF-6）

GS 在候选工作区嵌入 `@lyflow/editor` 做流程调试页：拓扑锁住，只调参数。1.2.0 已实现，说明与完整拦截清单见
[packages/editor/README.md](../packages/editor/README.md)「Props」。

### `topologyLocked?: boolean`（默认 false）

拦截放在 graph store 的结构性动作里（不只是隐藏按钮），快捷键、粘贴、右键菜单、拖线都走同一道门；被拦时动作返回各自的「没做」值
（null / false / 空数组 / `{ok: false}`），`lastRejection` = 「拓扑已锁定」，编辑器弹提示。锁上时撤销 / 重做栈清空。

| 锁住（`topologyLocked = true`） | 照常可用 |
|---|---|
| 增删节点（含 Delete、剪切、粘贴、复制节点、Ctrl+D） | 节点参数表单（含多选同算子一起改） |
| 连线、断线、拖线改接、插入到连线中间、拖线松在空白处建节点 | 顶层图参数的取值与配方切换 |
| 改节点算子类型；打开图时只要改连线的迁移整条不写回（节点停在老版本），只改版本与参数的照写 | 运行、运行到此、仅此节点、强制重算、取消 |
| 算子面板与片段（左栏不渲染）、算子搜索弹层、从外部拖入、展开库算子 | 节点输出预览、连线内容查看器、Inspector、日志页 |
| 合成子图 / 解散子图、保存到库 | 进出子图查看、查找节点（Ctrl+F）、适配视图 |
| 静音 / 旁路节点（改变数据流）；「恢复这组参数」锁着时不恢复静音状态，参数照恢复 | 挪节点位置、自动布局（只改坐标，算不算 dirty 由宿主经 `onDocChange` 自己决定） |
| 图参数的增删、改名、改规格、提升 / 绑定 / 解绑，图级输出的标记与删除；参数面板的「改为只在本配方生效」（它会新建图参数） | 撤销 / 重做（只撤得到锁上之后的调参） |

React Flow 一侧 `nodesConnectable`、`edgesReconnectable` 关掉；节点 / 连线右键菜单里改结构的项置灰。工具栏的「新建」没拦（锁着时新建出的空图
加不了节点，要不要藏由宿主决定）；检查器与参数面板里「纳入配方」「解除绑定」「删图参数」这类按钮没有置灰，点了被 store 拒、doc 不变。
store 守卫的单元测试在 `packages/editor/test/graph-params-actions.test.mjs`、`migrations.test.mjs`；真界面（面板不出、端口拖不出线）没有 e2e。

### 自定义 Transport 要实现什么

接口在 [`packages/editor/src/transport/types.ts`](../packages/editor/src/transport/types.ts)，说明在
[packages/editor/README.md](../packages/editor/README.md)「Transport」与 [docs/embedding.md](embedding.md)「前端编辑器」「部分运行」。
TS 接口里非可选的方法都要有；调试页真正用到的是下面这些，其余可以给空实现。参考实现：
[`transport/tauri.ts`](../packages/editor/src/transport/tauri.ts)（前端）与 `bridge/src/commands.rs`、`bridge/src/execution.rs`（Rust 侧，基于同一个 `lyflow-client`）。

| 用途 | 方法 / 事件 | 对应 `lyflow-client` |
|---|---|---|
| 算子表 | `getManifest()`、`getCoreInfo()` | `manifest_json`、`version` |
| 打开图 | `loadGraph(path)` → `{doc, migrations: []}`。编辑器只按 `graphPath` 调它，GS 可以把 path 当成模板键，直接返回模板 doc（顶层图参数的 `default` 填成这一帧的有效值） | — |
| 校验 / stale | `validateGraph(doc, path, params)`、`planGraph(doc, path, targets, params)` | `validate_with_params`、`plan_with_params`；注入了图像时用 `plan_with_inputs` |
| 运行 / 部分运行 | `runGraph(doc, path, {targets, isolate, force, params, mode})` → runId，`cancelRun(runId)` | `RunHandle::start`（`RunSpec` 带 `params_json`、`image_inputs`、targets / isolate / force） |
| 执行事件 | `onExecutionEvent(cb)` | core 回调的事件 JSON 原样转发；被抢占或没起来的运行要自己补一对 `run_started` / `run_finished`（照 `bridge/src/execution.rs` 的 `never_started_event`） |
| 节点输出 | `getOutputInfo(runId, nodeId)`、`getRunOutputs(runId)`、`getOutputImage(runId, nodeId, port, level, row, rows)`（可选，但不给就看不到图） | `output_info`、`run_outputs`、`output_image`（LYIM 载荷布局见 http-transport.md「结果」） |
| 缓存 | `evictCache`、`clearCache`、`cacheStats` | `cache_evict` / `cache_evict_with_inputs`、`cache_clear`、`cache_stats` |
| 空实现即可 | `onManifestUpdated`、`onCoreReloadFailed`（返回空 unlisten）；最近文件、备份（`backupStatus` 返回 `exists: false`）、配方目录（`listRecipeDir` 返回 `exists: false`）、库（`editable: false`、`getLibraryDefinition` 返回 null）、`saveGraph`、`importGraph`、`saveAsLibrary`、`writeFileBytes`、`getOutputCloud` / `Tensor` / `Indices`（glue 图没有点云、张量） | — |

要注意两点：

- **图像注入**：GS 的图读图是 `io.load_image` 的 `source = inputs`，编辑器的 `runGraph` 不带图像。GS 的 Transport 要在 `runGraph`、`planGraph`、`evictCache` 里
  自己注入当前调试帧，三处注入同一份，否则缓存键对不上，stale 也不准。
- **结果寿命**：`getOutputInfo` 等按 runId 取结果，GS 要像 bridge 一样留住最近的运行（在算的一份 + 上一次完成的一份），不能 run 完就丢。

### peer 依赖

与宿主共用同一份实例（Vite 写进 `resolve.dedupe`）：

| 包 | 版本范围 |
|---|---|
| `react`、`react-dom` | `^18.3.1 \|\| ^19.0.0` |
| `@xyflow/react` | `^12.11.6` |
| `three` | `>=0.160.0 <1` |
| `zustand` | `^5.0.2` |
| `@tauri-apps/api` | `^2`，可选，只有用 `TauriTransport` 才需要 |

普通依赖 `@dagrejs/dagre`、`immer`、`motion` 由包管理器装；`@lyflow/visualization` 在发布的 tarball 里已内联。包按 TypeScript 源码发布，
宿主的打包器要能编译 TS/TSX。一页只能挂一个编辑器（store 与 transport 是模块级单例）。

## 版本与兼容

- 产品版本升到 **1.2.0**（开发中，未发布）；glue 包版本 0.2.0；新算子各自 1.0.0。
- C ABI 没有变化；按 [packaging.md](deployment/packaging.md) 的系列规则，1.2 客户端只加载 1.2.x 引擎，GS 需要同步升级 `crates/lyflow-client` 的引用。
- `packaging/release.json` 的 `requiredOperators` 增加上面 6 个算子。

## 实测耗时

2026-10-29，本机（Windows 11、MSVC 14.44、RelWithDebInfo），seg-new14 cam2 录制的 22 帧 1280×1024 灰度图，`lyflow eval --no-cache`、
默认节点并行度，每组跑两遍取后一遍（两遍相差 < 0.5 ms）。图是 `packs/glue/graphs/measure.lyflow.json`：响应图 → 扇形扫描（取 3 个峰）→
3 × 粗跟踪 → 3 × 横向卡尺，一次运行算完全部候选。参数：出胶点 (629.1, 890.7)、扇区 [−146.44, −116.44]°、检测区 [0, 300]、
径向区间 [0, 150]、`diameter` 61、跟踪胶宽上限 60、站距 10、卡尺半长 150、`half_max`。「整图」= `run_finished.durationMs` − 读图节点
（宿主注入图像，不读文件）。

| 节点 | 整幅响应图 p50 / p95（ms） | `roi` = 出胶点 ± 368 px p50 / p95（ms） |
|---|---|---|
| `glue.response` | 9.6 / 10.8 | 5.9 / 6.6 |
| `glue.fan_scan` | 0.1 / 0.1 | 0.1 / 0.2 |
| `glue.track_path`（每个候选） | 0.8 / 1.1 | 0.8 / 1.3 |
| `glue.lateral_runs`（每个候选，31 站） | 0.9 / 1.1 | 0.9 / 1.4 |
| 节点合计 | 13.1 / 14.9 | 9.9 / 11.3 |
| **整图** | **18.2 / 19.3** | **14.5 / 16.3** |

对照：同一批帧上 1.1 的 `bead_path` → `bead_width`（只有搜索、不含回退那张图）整图 8.7 / 10.1 ms。GS 现有的逐帧定向含回退 p50 / p95 约 16 / 31 ms
（要跑第二张图）；新图一次运行把三个候选和每个候选的卡尺都算完，宿主再加一个回退分支（扇区收窄的扇形扫描 + 跟踪 + 卡尺，共用同一张响应图）
约多 2 ms。**宿主应给 `glue.response` 设 `roi`**（出胶点 ± 检测区外沿 + 胶宽上限 + 8，与 1.1 `bead_path` 只算的那一块相同）：整幅计算多花约 4 ms。

响应图节点之外还有约 1.5 ms 花在执行器给三张 f32 输出做的逐通道统计（事件里的 min / max / mean）上；这次把图像统计改成按位深直接读像素，
结果不变、整幅图每张约快 1 ms。
