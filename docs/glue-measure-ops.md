# glue 测量算子契约（1.2 / 1.3 系列）

[文档首页](README.md) · [glue 包说明](../packs/glue/README.md) · [测试地图](testing.md)

状态：**契约草案，lyFlow 与 GlueSight 并行开发中**。改动契约前先在两边汇报，再改本文件与实现。

背景见 GlueSight 仓库 `docs/decisions/lyflow-operators.md`（LF-1 / LF-2）：lyFlow 只提供确定性的图像测量算子，
所有门限都是参数，输出完整的中间结果；流程编排、有无胶 / 像不像胶 / 方向回退 / 每站取哪一段等判定，
以及参数分层与发布冻结都归 GlueSight（下称 GS）。

本文件列出 1.2 系列新增的 6 个算子，与 1.3 系列新增的 5 个彩色测量算子（[1.3 彩色测量算子](#13-彩色测量算子)）：每个算子的 id、端口、
全部参数（名称、类型、单位、默认值、范围）与输出字段。
1.1 的 `glue.bead_path`、`glue.taught_path`、`glue.bead_width`、`glue.bead_breaks`、`glue.edge_distance`、`glue.judge`
与飞拍的 `glue.locate`、`glue.station_calipers` **原样保留**（语义、参数、输出一位不变），供示例图、飞拍宿主和旧工具继续使用；
GS 从 1.2 起不再使用前三个的语义。

## 总览

```
                    ┌→ glue.fan_scan ──fan──┐
io.load_image → glue.response ──maps────────┼→ glue.track_path ─line─→ glue.lateral_runs
                    └──maps─────────────────┴──────────────────────────↗
glue.polyline（示教折线）─line─→ glue.lateral_runs / glue.part_edges
io.load_image ──image──────────────────────────────────────────────→ glue.part_edges
```

| id | 名称 | 输入 → 输出 | 一步做什么 |
|---|---|---|---|
| `glue.response` | 响应图 | `image` → `maps` | 整幅（或 roi 那一块）黑 / 白顶帽，附带陡度用的平滑灰度与局部亮度，打包成一个 Bundle |
| `glue.fan_scan` | 扇形扫描 | `maps` → `scan`、`overlay` | 每个方向一条射线上的平均响应；可选前 N 个峰 |
| `glue.track_path` | 粗跟踪与拟合 | `maps`、`fan?` → `line`、`track`、`overlay` | 沿一个方向逐步找胶点，稳健拟合光滑胶路 |
| `glue.lateral_runs` | 横向卡尺 | `maps`、`line` → `runs`、`overlay` | 沿胶路布站，每站卡尺上的全部暗段 |
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
- **响应图（`maps`）**：`glue.response` 输出一个 `Bundle<glue.ResponseMaps>`，三张单通道 f32 图只覆盖 `frame` 那一块
  （roi ∩ 图像；没设 roi 时是整幅），下游按 `frame` 的偏移采样，**坐标仍是整幅图的像素坐标**。下游采样（双线性）四个邻点里有一个
  落在块外（或图外）即视为**无效采样**（1.2.0 联调前的版本是整幅图、块外填 NaN，采样结果相同）。
- **输入图**：`glue.response` 与 `glue.part_edges` 的 `image` 接受全分辨率 u8 灰度 / RGB / RGBA（三通道按 BT.601 转灰度，与 1.1 相同）；
  其他位深先接 `image.normalize`；预览缩小图（`scale > 1`）报 `bad_input`。`maps` 端口要求完整的 `glue.ResponseMaps`
  （三张单通道 f32、`scale = 1`、尺寸与 `frame` 一致），否则 `bad_input`。
- **JSON 数值**：新算子的输出**一律不舍入**（双精度原样写出，JSON 读回逐位相同）：宿主拿这些值做判定，舍入会让落在门限上的判断与 1.1 不一致，扇形峰的方向舍入后再喂给粗跟踪还会让起跑方向偏一点。只有 `glue.Polyline` 沿用 1.1 的格式（坐标 3 位、切向 5 位小数），1.1 的 `bead_width` 量的也是这条舍入过的折线。取不到的值是 `null`，不用 NaN 或 −1。
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
| 出 `maps` | Bundle `glue.ResponseMaps` | 下面四个字段 |

`glue.ResponseMaps` 的字段：

| 字段 | 类型 | 说明 |
|---|---|---|
| `response` | Image（f32 × 1，`frame` 大小） | 暗胶：闭运算 − 原图（黑顶帽）；亮胶：原图 − 开运算（白顶帽）；再做 5×5 高斯（`responseSigma`）。≥ 0 |
| `gray` | Image（f32 × 1，`frame` 大小） | 原图做 5×5 高斯（`graySigma`）；亮胶时取 255 − g，使「胶比背景暗」恒成立。边缘陡度用它 |
| `level` | Image（f32 × 1，`frame` 大小） | 局部亮度：暗胶取闭运算、亮胶取原图，再做 5×5 高斯（`levelSigma`）。处处 ≥ response（σ 相同时） |
| `frame` | Record `glue.Frame` | `{x, y, width, height, imageWidth, imageHeight}`，全是整数像素：`x`、`y` 是块的左上角像素在整幅图里的坐标，`width`、`height` 是块的宽高（= 三张图的宽高），`imageWidth`、`imageHeight` 是整幅输入图的宽高。三张图的 (0, 0) 像素就是整幅图的 (x, y) |

块 = `roi`（`[x, y, w, h]`，四舍五入到整数）与图像求交；没设 roi（w、h ≤ 0）就是整幅图。**roi 与图像不相交（交集为空）是执行错误
`bad_param`，指到 `roi`**：这只会是参数配错（例如出胶点填错），不当成值往下传。块里的值与整幅计算逐位相同。

| 参数 | 类型 | 单位 | 默认 | 范围 | 说明 |
|---|---|---|---|---|---|
| `polarity` | enum `dark` / `bright` | — | `dark` | — | 胶比背景暗（黑顶帽）还是亮（白顶帽） |
| `diameter` | int | px | 91 | 3–401 | 结构元直径。比它窄的暗带才有完整响应，应大于期望最大胶宽（1.1 用的是 `widthMax + 1`） |
| `roi` | vec4 `[x, y, w, h]` | px | `[0, 0, 0, 0]` | w、h 同为 ≤ 0（整幅）或同为 > 0 | 只算、只输出这一块（与图像求交，不相交是 `bad_param`）；块内的值与整幅计算逐位相同 |
| `responseSigma` | float | px | 1.2 | 0–3 | 响应图的 5×5 高斯 σ；0 = 不平滑 |
| `graySigma` | float | px | 1.0 | 0–3 | `gray` 的 5×5 高斯 σ；0 = 不平滑 |
| `levelSigma` | float | px | 1.2 | 0–3 | `level` 的 5×5 高斯 σ；0 = 不平滑 |

固定（不开放）：结构元是近似圆盘的正八边形（方形 ⊕ 两条对角线段，比 OpenCV 椭圆核快三十多倍）；高斯核固定 5×5；roi 内部按两倍结构元半径 + 4 外扩计算。
八边形拆成三次形态学串着做，离图边两倍半径以内闭运算可能比原图低一点（1.1 的 `responseField` 也是这样，那里会出负响应）；
`glue.response` 把负值截到 0 再平滑，所以 `response` 处处 ≥ 0，离图边远的地方与 1.1 逐位相同。

## 2. `glue.fan_scan` 扇形扫描

| 端口 | 类型 | 说明 |
|---|---|---|
| 入 `maps` | Bundle `glue.ResponseMaps` | `glue.response.maps`（用 `response`） |
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
| `includeInvalid` | bool | — | false | — | 有效方向凑不够峰时，得分为 `null` 的方向也拿来补（1.1 的挑法，见下） |

**计算**：方向从 `from` 起每次加 `angleStep`（累加，与 1.1 逐位相同），到 `to + 1e-6` 为止；每个方向在 `t = r0 + j·radialStep`（`t < r1`）上双线性采样响应，
得分 = 有效采样之和 / 总采样数（无效采样按 0 计入分母）。**峰**：把有得分的方向按得分从高到低排（同分保持角度顺序），
依次取与已取的每个峰角差都 **> `peakSeparation`** 的（与 1.1 `bead_path` 的取法相同），取满 `peakCount` 个为止。
`includeInvalid = true` 时得分为 `null` 的方向按 −1 一起排（同分保持角度顺序，所以排在全部有效方向之后），有效方向凑不够时它们也会成为峰，
这种峰的 `score` 是 `null` —— 1.1 `bead_path` 就是这样凑满三个候选的。默认 false：峰只从有得分的方向里取，这是有意的差异
（没有响应的方向不当候选）；要与 1.1 逐位等价就打开它。
角差按圆周取（−170° 与 170° 相差 20°）。

`glue.FanScan`：

| 字段 | 说明 |
|---|---|
| `anchor`、`sector`、`radial`、`angleStep`、`radialStep` | 回显参数 |
| `samples` | 每条射线的采样数 |
| `angles[]` | 各方向（度） |
| `scores[]` | 各方向得分；无效方向 `null` |
| `invalid[]` | 各方向的无效采样数 |
| `peaks[]` | `{index, angle, score}`，按得分降序；`index` 是在 `angles` 里的下标；`angle` 与 `angles[index]` 逐位相同；`includeInvalid` 补上的峰 `score` 为 `null` |

1.1 `bead_path` 的扇形搜索等价于 `radial = [zoneStart, zoneStart + min(150, zoneEnd − zoneStart)]`、`includeInvalid = true`、其余参数取默认值；
给了 heading 的回退等价于 `sector = [heading − headingTol, heading + headingTol]`。

## 3. `glue.track_path` 粗跟踪与拟合

从锚点沿起始方向、在检测区 `zone` 里每 `coarseStep` 走一步，每步在法向卡尺上找离预测点最近的暗段作胶点，再局部线性稳健拟合、
高斯平滑成一条覆盖整个检测区的光滑胶路。**不输出 ok**：覆盖率、陡度够不够、选哪个候选方向都由 GS 判。

| 端口 | 类型 | 说明 |
|---|---|---|
| 入 `maps` | Bundle `glue.ResponseMaps` | `glue.response.maps`（`response` 找胶点，`gray` 算逐点陡度与重新接上时的陡度门） |
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
| 入 `maps` | Bundle `glue.ResponseMaps` | `glue.response.maps`（`response` 分段、`gray` 算边缘陡度、`level` 取局部亮度） |
| 入 `line` | Record `glue.Polyline` | `track_path.line` 或 `polyline.line` |
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
| `level` | `level` 图在 `t = lo, lo + 1, …（< hi）` 与 `t = hi` 上采样的最大值；全部采样无效时 `null`。1.1 示教胶路的对比度 = `peak / max(level, 1)` |

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
n_load(io.load_image) → n_resp(glue.response, roi)
n_resp.maps → n_fan(glue.fan_scan)
n_resp.maps, n_fan.scan → n_t0 / n_t1 / n_t2 (glue.track_path, peakIndex 0 / 1 / 2)
n_resp.maps, n_tK.line → n_rK (glue.lateral_runs)
回退：n_resp.maps → n_fanB(glue.fan_scan, sector = 示教方向 ± 容差) → n_tB → n_rB
找边：n_load.image, n_tK.line → n_eK (glue.part_edges)
```

一次运行算完所有候选，GS 读各 `track` / `runs` / `edges` 记录后自己判定与选取，不需要第二次运行。

## 1.3 彩色测量算子

**为什么另起一组**：现场改用粉色 / 粉橙色胶，底材有发黑的铸件（暗、纹理重，去马赛克后带假色）和镀铝 / 镀锌面（中性灰、大片饱和高光）。
灰度下粉胶在两种底材上明暗方向可能相反，顶帽响应分不出来；按颜色量就没有这个问题。宿主（GS）在取图回调里去马赛克，按 RGB 三通道注入，
每台相机按配方选彩色或灰度链路。1.2 的 6 个算子一位不改；彩色链路是下面 5 个新算子，原则不变：**只测量，不判定**。

```
示教：io.load_image(RGB) ─image──┐
      glue.polyline（示教线）─line─┴→ glue.color_model ─model→（宿主存进配方）
生产：io.load_image(RGB) → glue.color_map（model）─maps─┬────────────────────────────────→ glue.color_runs（line = 示教线）
                                                        └→ glue.color_fan ─scan→ glue.color_track ─line→ glue.color_runs
```

| id | 名称 | 输入 → 输出 | 一步做什么 |
|---|---|---|---|
| `glue.color_model` | 胶色建模 | `image`、`line`、`prior?` → `model`、`overlay` | 沿示教线取胶与背景的色度样本，拟合颜色模型（示教时跑一次） |
| `glue.color_map` | 胶色得分图 | `image`、`model?` → `maps` | 按模型给 roi 里每个像素打「胶的面积占比」，三态分开，打包成一个 Bundle |
| `glue.color_fan` | 彩色扇形扫描 | `maps` → `scan`、`overlay` | `glue.fan_scan` 原样用在得分图上 |
| `glue.color_track` | 彩色粗跟踪 | `maps`、`fan?` → `line`、`track`、`overlay` | `glue.track_path` 的彩色版；高光、暗区挡住的步不计入 maxGap |
| `glue.color_runs` | 彩色横向卡尺 | `maps`、`line` → `runs`、`overlay` | 每站全部胶色段与无信息区间，输出 `glue.LateralRuns` 的超集 |

输入图要求全分辨率 u8 RGB / RGBA（RGBA 去掉 alpha），通道顺序 RGB；单通道、其他位深、预览缩小图都是 `bad_input`（「彩色算子需要 RGB」），
灰度相机走 1.2 的 `glue.response` 链路。坐标、角度、胶路、布站、JSON 数值（不舍入）、确定性的约定同上文「约定」。

### 像素：特征、得分与三态

- **特征**：RGB 各通道先做 σ = `blurSigma` 的高斯（核半宽 ⌈3σ⌉，压掉去马赛克的假色），取对数色度 `c1 = ln((R+1)/(G+1))`、`c2 = ln((B+1)/(G+1))`
  （与照明强弱无关）与 BT.601 亮度 `Y`。
- **模型**：胶是一个二维高斯，背景是 ≤ 3 个高斯；每个高斯另记这一类像素 R + G + B 的均值 `intensity`。
- **得分**（`score` ∈ [0, 1]）= 胶的**面积占比**估计 α × 离群门：
  1. 对每个背景分量 k，在合并协方差 ½(Σ胶 + Σk) + (chromaFloor² + (noiseK / Y)²)·I 下求像素到「胶均值—背景 k 均值」线段的马氏距离²，取最小的那个 k；
  2. 在归一化色度 (r, b) = (R+1, B+1) / (R+G+B+3) 上把像素投到胶与背景 k 两点的连线上，得到胶占的光通量 β ∈ [0, 1]；
  3. 按亮度比 ρ = intensity胶 / intensityk 换成面积：α = β / (β + ρ(1 − β))；
  4. 乘离群门 sigmoid(½(glueGate² − 第 1 步的最小距离²))：离胶与所有连线都远的颜色得分趋近 0。

  为什么按面积：亮胶压在暗铸件上时，色度在约 25 % 面积处就过了胶与铸件的中点，按色度（或按后验概率）取 0.5 的边，胶宽每边多量 1 px 以上；
  按面积算，卡尺的半高边落在 50 % 处。混合是在平滑后的线性 RGB 里发生的，所以 α 对两类之间的混合像素是线性的。
- **三态**（`state` 图，按下面的顺序判）：

| state | 名称 | 条件 | score |
|---|---|---|---|
| 1 | 高光 `highlight` | 任一通道（平滑前）≥ `satLevel`，掩膜膨胀 `satDilate` px | NaN（无信息） |
| 3 | 暗背景 `dark_bg` | `Y < darkRatio × 胶亮度 P5`（不看噪声：明显比见过的最暗的胶还暗，就不是胶） | 0 |
| 2 | 暗区 `dark` | 色度噪声 noiseK / Y > `noiseMax`（即 `Y < noiseK / noiseMax`） | NaN（无信息） |
| 0 | 有信息 | 其余 | α × 离群门 |

  下游采样时 NaN 与块外一样算**无效采样**（双线性四邻点有一个无效即无效）。**无信息不是没有胶**：高光 / 暗区挡住的站要由宿主判成「未检」，
  不能判断胶；`color_runs` 给出每站的无信息区间与原因，`color_track` 把这些步单独计数。

### `glue.ColorModel`（Record）

```json
{ "version": 1, "space": "log_chroma",
  "spec": { "blurSigma": 0.8, "satLevel": 250, "satDilate": 2, "noiseK": 4, "noiseMax": 0.25,
            "darkRatio": 0.5, "chromaFloor": 0.03, "glueGate": 4 },
  "glue": { "n": 3322, "mean": [c1, c2], "cov": [[xx, xy], [xy, yy]], "intensity": 408.1, "luma": [p5, p50, p95] },
  "background": [ { "weight": 0.56, "mean": [c1, c2], "cov": [[…], […]], "intensity": 335.2 }, … ],
  "stats": { "frames": 1,
             "glue": { "n", "sum": [Σc1, Σc2], "sumsq": [Σc1², Σc1c2, Σc2²], "sumRGB", "luma": [64 格，每格 4 级] },
             "background": { "n", "binWidth": 0.04, "range": [-2.5, 2.5], "bins": [[i, j, count, sumRGB], …] },
             "glueLike", "saturated", "dark" },
  "train": { "glueHalf", "bgBand", "stationStep", "recenterRounds", "shiftMax" },
  "quality": { "usable", "separation", "glueCount", "bgCount", "components", "glueSpread", "saturatedFraction",
               "darkFraction", "glueLikeFraction", "frames", "stations", "centerShift", "recentered" } }
```

- `spec` 是像素级参数：建模与出图用同一份，`color_map` 只读模型里的这一份。
- `glue` / `background` 由 `stats` 拟合：胶取矩；背景从二维直方图做确定性的 EM（初始中心按「计数 × 到已选中心的最小距离²」逐个挑，硬分 10 轮、软 EM 30 轮，
  权重 < 2 % 的分量去掉，按权重降序）。`stats` 全是可相加的量：接 `prior` 时两份 `stats` 逐项相加再拟合。
- **生产用的精简模型**：`stats`、`train`、`quality` 都可以去掉（只留 `version`、`space`、`spec`、`glue`、`background`，几百字节），`color_map` 打分逐位相同；
  不带 `stats` 的模型不能当 `prior`（`bad_input`）。宿主宜在配方里存一份完整模型（以后累加用）和一份精简模型（逐帧填 `n_map.model`）。
- `quality` **只测量不判定**（够不够用由宿主判）：`usable` = 胶样本 ≥ 2 且至少一个背景分量；`separation` = 胶与各背景分量在合并协方差（+ chromaFloor²）下
  两均值马氏距离的最小值；`glueSpread` = 胶协方差最大特征值的平方根；`saturatedFraction` / `darkFraction` / `glueLikeFraction` 是两条取样带里高光 /
  暗区 / 背景带里像胶的样本占比（分母是这些样本加上进了模型的样本）；`centerShift` 是重新居中后各站 |偏移| 的中位数（px），`recentered` 是最后一轮量到中心的站占比
  （这两个只看这一帧）。
- `color_map` 读模型时校验：version、space、spec 取值、胶与每个背景分量的 cov 对称正定、intensity > 0、背景 1–3 个分量、带 stats 时 stats 完整；不合格是 `bad_param`（参数）或 `bad_input`（端口）。

### 1.3.1 `glue.color_model` 胶色建模

| 端口 | 类型 | 说明 |
|---|---|---|
| 入 `image` | Image | 示教帧（u8 RGB / RGBA） |
| 入 `line` | Record `glue.Polyline` | 示教线（沿胶中心画），一般接 `glue.polyline.line` |
| 入 `prior`（可选） | Record `glue.ColorModel` | 之前的模型，统计累加进来；像素参数（`spec`）要与本节点相同，否则 `bad_input` |
| 出 `model` | Record `glue.ColorModel` | 见上 |
| 出 `overlay` | Record `lyflow.overlay2d` | role：`path`（示教线）、`center`（重新居中后的胶中心）、`glue`（胶样本带两边）、`background`（背景带四条边） |

| 参数 | 类型 | 单位 | 默认 | 范围 | 说明 |
|---|---|---|---|---|---|
| `glueHalf` | float | px | 4 | 0–100 | 胶样本：每站中心 ± glueHalf，每 1 px 一个（应小于胶的半宽） |
| `bgBand` | vec2 `[inner, outer]` | px | `[20, 40]` | glueHalf < inner < outer | 背景样本：离中心 [inner, outer]，两侧 |
| `stationStep` | float | px | 4 | 1–100 | 沿线取样与重新居中的站距（s 从示教线起点量起） |
| `recenterRounds` | int | — | 2 | 0–5 | 重新居中的轮数；0 = 不居中 |
| `shiftMax` | float | px | 20 | ≥ 0 | 重新居中的偏移上限 |
| `trimSigma` | float | — | 3 | 0.5–20 | 胶样本截尾：马氏距离（协方差 + chromaFloor²）超过它的去掉 |
| `bgExcludeSigma` | float | — | 3 | 0–20 | 背景带里离胶马氏距离小于它的样本当「像胶」，不进背景（计入 `glueLike`） |
| `maxComponents` | int | — | 3 | 1–3 | 背景高斯的最多个数 |
| `blurSigma` | float | px | 0.8 | 0–5 | 求色度前各通道的高斯 σ；0 = 不平滑 |
| `satLevel` | float | 灰度级 | 250 | 1–255 | 任一通道 ≥ 它即高光 |
| `satDilate` | int | px | 2 | 0–20 | 高光掩膜的膨胀半径（方形核） |
| `noiseK` | float | — | 4 | 0–100 | 色度噪声的标准差 ≈ noiseK / Y |
| `noiseMax` | float | — | 0.25 | 0.001–10 | 噪声大于它的像素是暗区 |
| `darkRatio` | float | — | 0.5 | 0–1 | Y < darkRatio × 胶亮度 P5 判暗背景 |
| `chromaFloor` | float | — | 0.03 | 0.001–1 | 协方差的下限（对数色度的标准差） |
| `glueGate` | float | — | 4 | 0.5–50 | 离群门的马氏距离 |

**计算**：

1. 站：示教线上 s = s₀, s₀ + stationStep, …；色度平面只算示教线外包框 ± (shiftMax + max(bgOuter, glueHalf) + 2) 那一块。
2. 每站在中心（初始偏移 0）± glueHalf 取胶样本、两侧 [inner, outer] 取背景样本（最近像素）；高光、暗区（Y < noiseK / noiseMax）的样本只计数。
3. 胶样本先拟合一次，截掉 trimSigma 外的（剩不到 2 个就全留）；背景样本离截尾后的胶不到 bgExcludeSigma 的不要；拟合（同上文 EM）。
4. 重新居中（`recenterRounds` 轮）：按这一帧的模型在每站卡尺 t ∈ ±(shiftMax + inner) 上每 0.5 px 打分，取得分 ≥ 0.5 的连续段里中点离当前偏移最近、
   |中点| ≤ shiftMax、不顶卡尺端点的那一段，五站滑动中值，量不到的站保留原偏移；回到第 2 步。
5. 最后一轮的统计是这一帧的统计；与 `prior` 的统计相加后拟合出输出模型。**每一帧的统计只由这一帧算**（重新居中不用 prior），
   所以 A 接 prior B 与 B 接 prior A 得到同一个模型（`glue` / `background` / `stats` 逐位相同）。

### 1.3.2 `glue.color_map` 胶色得分图

| 端口 | 类型 | 说明 |
|---|---|---|
| 入 `image` | Image | 生产帧（u8 RGB / RGBA） |
| 入 `model`（可选） | Record `glue.ColorModel` | 接了就不用 `model` 参数（编辑器、参考图里直接接 `color_model.model`） |
| 出 `maps` | Bundle `glue.ColorMaps` | 下面四个字段 |

`glue.ColorMaps`：`score`（f32，得分，NaN = 无信息）、`luma`（f32，平滑后的 Y）、`state`（f32，0 / 1 / 2 / 3 见三态表）、`frame`（`glue.Frame`，与 `glue.ResponseMaps` 相同）。
三张图只有 frame 那一块，下游按 frame 的偏移采样。

| 参数 | 类型 | 单位 | 默认 | 范围 | 说明 |
|---|---|---|---|---|---|
| `model` | text（`glue.ColorModel` 的 JSON） | — | `""` | 没接 `model` 端口时必须是合格的模型 | 宿主存在配方里逐帧给 |
| `roi` | vec4 `[x, y, w, h]` | px | `[0, 0, 0, 0]` | w、h 同为 ≤ 0（整幅）或同为 > 0 | 只算、只输出这一块；与图像不相交是 `bad_param` |

加载期校验（没接 `model` 端口时）：`model` 为空、不是 JSON、不是合格的模型都是 `bad_param`，指到 `model`。计算时块外扩 ⌈3·blurSigma⌉ + satDilate + 2 px 做平滑与膨胀，
**块里的值与整幅计算逐位相同**；行按执行器给的线程预算分块并行，**结果与线程数无关**。

### 1.3.3 `glue.color_fan` 彩色扇形扫描

与 `glue.fan_scan`（第 2 节）的参数、`glue.FanScan` 输出、取峰规则完全相同，只是射线上采样的是 `maps.score`；高光、暗区与块外的采样都算无效（`invalidMax`）。

### 1.3.4 `glue.color_track` 彩色粗跟踪

端口同 `glue.track_path`（`maps` 换成 `Bundle<glue.ColorMaps>`，`fan` 接 `glue.color_fan.scan`）。逐步的做法同第 3 节，差别：

- 卡尺在得分图上分段（`split` 默认 `connected`、`segmentMin` 0.3、`peakMin` 0.6），按第 1.3.5 节的背景约束统一处理短缝和高光内谷；胶点取合并段的中点。
- 有至少 `historyMin` 个历史宽度且四分位距小于 `steadyIqrRatio × 宽度中位数` 时，使用历史宽度中位数引导合并，中心参考为当前预测点。连续跟踪时，宽度小于 `reacquireWidthRatio × 参考宽度` 且中心偏移超过参考宽度的 15% 的片段不作为胶点，防止整胶被反光分裂后吸附单侧碎片；真正居中的窄胶仍保留。
- 没有陡度门、不退回射线：去掉 `sharpMin`、`edgeProbe`、`backgroundProbe`、`rayFallback`、`rayFallbackCoverage`、`reacquirePeakRatio`、`mergePeakRatio` 这几个参数；
- **无信息步**：这一步没找到胶、且接受窗 t ∈ ±min(acceptOffsetRatio × widthMax, searchHalf) 上（每 0.5 px）有效采样占比 < `infoMin`（默认 0.5，0–1）。
  无信息步走过的长度不计入 `maxGap`：停止条件是「s − 上一个胶点的 s − 其间无信息步的长度 > maxGap」。

其余参数（名称、默认值、范围）同第 3 节。`glue.Track` 另有：`unknownSteps`（无信息步数）、`coverageInformative` = found / (steps − unknownSteps)（分母为 0 时 `null`）、
`unknownS[]`（无信息步的 s）；`sharpness` 与 `points[].sharpness` 恒为 `null`，`points[].onRay` 恒为 false，`points[].peak` 是段内得分峰值。

`widthPolicy` 为 `range`（默认）或 `extent`。默认策略保留 `[widthMin, widthMax]` 筛选、`mergeWidthMax` 合并上限和原接受窗。
显式 `extent` 策略对完整候选保留宽度下限，不完整片段保留为无信息证据，不因完整胶宽超过 `widthMax` 丢弃胶点；搜索仍固定在 `±searchHalf`，合并总宽最多 `2 × searchHalf`，接受窗改为 `±acceptOffsetRatio × searchHalf`。
真实背景阻断、有限合并缝、邻站/历史合并证据及连续性门保持有效。两个候选都落在接受窗、中心距离差不超过 `max(1 px, 0.1 × widthMax)` 且次候选峰值至少为首候选的 80% 时，该步记为歧义，不任意选边。
不完整边界的候选也不以片段中点参与拟合，沿上一个可靠方向继续有限搜索。

`glue.Track` 回显 `widthPolicy`，并给出 `ambiguousS[]`（歧义步）和 `incompleteS[]`（非歧义但边界不完整的步，包含高光/暗区/观测边界，不等同可靠截断）。
这两类步都计入 `unknownS`/`unknownSteps`，不计 `maxGap`；宽度值只来自完整边界胶点。没有有效方向峰时两数组为空、原有统计为 `null`。

### 1.3.5 `glue.color_runs` 彩色横向卡尺

端口同 `glue.lateral_runs`（`maps` 换成 `Bundle<glue.ColorMaps>`），overlay 多一个 role `unknown`（无信息区间）。

| 参数 | 类型 | 单位 | 默认 | 范围 | 说明 |
|---|---|---|---|---|---|
| `stationStep`、`sRange`、`searchHalf`、`split`、`mergeWidthMax` | | | 同第 4 节 | | |
| `segmentMin` | float | — | 0.3 | 0–1 | 分段门限：得分 > 它的相邻采样连成一段 |
| `peakMin` | float | — | 0.6 | 0–1 | 段的得分峰值下限 |
| `mergeGap` | float | px | 8 | ≥ 0 | 无明确背景的短缝合并上限；不查峰值比 |
| `mergeUnknownGap` | float | px | 16 | ≥ 0 | 无明确背景的高光/低分内谷合并上限；大于 0 时启用邻站自适应，0 时关闭自适应。range 策略的总宽受 mergeWidthMax 限制 |
| `widthPolicy` | enum | — | range | range / extent | range 保留 mergeWidthMax；extent 在固定 searchHalf 内恢复完整宽度，合并上限为 2 × searchHalf |

分段与半高取边同第 4 节（`caliperRuns`），彩色分支不做无条件短缝合并。两段之间任何采样明确为背景（有限得分 < `segmentMin / 2`）都禁止合并；饱和高光和未饱和低分色谷本身不是明确背景。没有邻站参考时，允许缝宽为 `max(mergeGap, mergeUnknownGap)`。

`color_runs` 首遍按上述固定范围提取每站最近段，取得全局宽度中位数；同时保留原片段，按「相对全局中位数的宽度偏差 + 2 × 中心偏移」比较原片段与首遍合并段，选择参考候选，避免侧条并入后污染邻站参考。再筛掉宽度不在全局中位数 0.7–1.3 倍范围、或中心偏移超过其一半的站。每站从其余站选弧长最近的至多 5 个参考，至少需要 3 个。第二遍使用这些首遍候选的宽度和中心中位数，不用自适应合并后的结果回填参考。`color_track` 使用稳定历史宽度作为同类参考。

有参考时，缝宽上限可扩大到 `max(mergeGap, mergeUnknownGap, 参考宽度 / 2)`；合并宽度必须在参考的 0.7–1.3 倍内，中心偏差不超过参考宽度的 30%。合并结果的「宽度偏差 + 2 × 中心偏差」必须优于每个参与的原片段，避免吞掉原本正确的主胶。参考只约束合并，不能替代当站实测宽度；无法恢复的外边仍保留 incomplete 与 cause，不进行宽度插值或按标称值补齐。

输出 `glue.LateralRuns` 的超集：

- 顶层多 `source = "color"`、`mergeUnknownGap`；
- 每站多 `infoFraction`（卡尺采样中得分有效的占比）、`unknownSpans[{lo, hi, cause}]`（同一原因的连续无效采样，`lo` / `hi` 是首末采样的 t，
  cause = `highlight` / `dark` / `outside`：双线性四邻点里有出块的算 outside，否则有高光的算 highlight，否则 dark）；`validFraction` 是采样在块里的占比；
- 每段多 `loCause` / `hiCause`（`edge` 降到半高以下的真边 / `caliper` 顶到卡尺端点 / `highlight` / `dark` / `outside` 被无信息截断）、
  `meanScore`（段内有效采样得分的均值）、`unknownWidth`（段内无效采样的宽度，px）；`edgeLo`、`edgeHi`、`depth`、`sharpness` 为 `null`，`level` 恒为 1。

`glue.LateralRuns` 回显 `widthPolicy`。`extent` 不丢弃超预期宽候选；仍返回所有候选，由宿主关联目标及判规格。
`loComplete`/`hiComplete` 只有观测到真边才为 true。搜索端点原因保持 `caliper`；有合法原图尺寸元数据时，`extent` 将原先 `outside` 细分为 `roi` 与 `image`，同样用于 `unknownSpans[].cause`；旧模式或缺少原图尺寸仍为 `outside`。
`highlight` 与 `dark` 保持无信息原因，不能当作完整边界或宽度下界。被截断候选的 `width`/`mid` 是已观测片段几何，宿主不得直接解释为完整胶宽或胶中心。

### 参考连法与 GS 侧的判定

随包参考图 `packs/glue/graphs/color.lyflow.json`：示教帧 + 示教线 → `color_model`；生产帧 → `color_map`（接 `n_model.model`）→ 沿示教线的 `color_runs`，
以及 `color_fan` → `color_track` → `color_runs`。宿主生产时不带示教那两个节点，把配方里的模型 JSON 填进 `n_map.model`。

宿主判每站时先确认目标关联与双侧完整性：可信的完整边界段才能提供完整胶宽；截断片段只保留边界证据，可靠时可形成保守下界，不能以片段中点替代胶中心。
有多个无法区分的候选、中心落入 `unknownSpans` 或观察范围不足时为未知；只有充分观察到有效背景且没有目标，才可判为缺胶。`quality.separation`、`glueSpread`、`saturatedFraction` 可作示教验收的依据。

### 实测（2026-11-06）

合成场景（`packs/glue/tests/test_color.cpp`）：宽 {8, 20, 40} px × 底材 {铸件（马赛克再去马赛克）、镀锌、两侧各一} × 照明 {平、0.4–1.0 渐变}，每站一段、
胶宽中位误差 ≤ 1 px；得分若按色度 / 后验概率而不换算面积，铸件上胶宽偏宽 2.3 px。

真实帧（JHEM131GC-XS 去马赛克后的 1280×1024 RGB，`packs/glue/tools/glue_color_eval.py`，示教线手画，模型用第 35 帧建）：

| 段 | 有胶帧 | 每站有段 | 中心段宽 P10–P90 | 彩色跟踪的有信息覆盖率 | 无胶帧上的段 |
|---|---|---|---|---|---|
| cam1 直胶（纹理金属上） | #27–45（19 帧） | 100 % | 42.7–53.9 px | 1.00 | #1–14、#48–60：至多 0.5 % 的站有 ≤ 7.3 px 的碎点 |
| cam3 直胶（镀锌面旁） | #23–45（23 帧） | 90–100 % | 44.1–64.7 px | ≥ 0.98 | #1–15、#48–60：至多 1.7 % 的站有 ≤ 3.3 px 的碎点 |

这条胶是圆柱形的：亮侧粉、背光侧偏棕、中间一条没饱和的镜面反光，三部分都量进了胶宽（约 46–52 px）。示教时背景带要离开整条胶（这件用 `bgBand = [32, 52]`）：
贴着胶边取背景会把背光侧学成一个「偏粉的背景分量」，反光处的像素就近选它解混、得分掉到 0.1–0.3，胶被劈成两半（`quality.components`、`glueLikeFraction` 能看出来）。
绕圈胶丝段（cam3 #100–160）只做了观察，没有真值。

第二批（`20261105_234703` 一个检测周期，原始 Bayer 按 `COLOR_BayerRG2RGB` 去马赛克，0.077 mm/px），与颜色掩膜参考测量（每 1 mm 一站、离出胶点 3–33 mm）对比，
走 `color_fan` → `color_track` → `color_runs`（胶的方向逐帧在变，固定示教线不适用）：

| 段 | 帧 | 每站有段 | 胶宽与参考的中位差 / \|差\| P90 | 说明 |
|---|---|---|---|---|
| cam3 上行胶（左暗背景、右亮铝） | #111–190 | ≥ 95.7 %（只有 1 帧缺 4.3 % 的站） | −0.11 / 0.27 mm | 灰度链路在 #104–124、155、168–172、175、184 报的假断胶全部消失；#104–110 胶刚出来、只有胶丝 |
| cam1 左下行胶（铸件） | #21–47 | ≥ 92 % | +0.20 / 0.30 mm | #16–20 胶条还短 |
| cam3 左行胶 | #21–47 | ≥ 92 % | +0.24 / 0.35 mm | 同上 |
| 无胶段（只有胶丝） | #48–103 | 彩色跟踪覆盖率 0 | — | 沿射线碰到的胶丝宽 0.7–1.1 mm，宿主按宽度下限滤掉 |

调参要点（都是宿主给的参数）：`color_track` 的 `widthMin` 按期望胶宽给（约 0.4 倍，这里 26 px），否则出胶点旁的细胶丝会把跟踪拐走；检测区从出胶点外约 5 mm 起
（喷嘴、胶丝、胶根挤在一起的那一截不跟），`widthMax` 约 1.5 倍期望胶宽。胶边紧贴饱和的铝面时，那条边的 `loCause` / `hiCause` 是 `highlight`，
高光膨胀与平滑会吃掉约 3 px，宽度在这些站是下限（#170 比参考窄 0.7 mm）。#160–190 上段胶面有一条沿中线的反光带，跟踪按合并段的中点走，
拟合胶路离颜色掩膜中心线的偏差每帧中位 ≤ 0.9 px、P90 ≤ 3.8 px（只在出胶点外 6–10 mm 那一截大些，那里参考的中心线被旁边的胶丝拉偏）。

耗时（本机 RelWithDebInfo）：`color_map` + `color_runs`（站距 4、卡尺半长 66）在 cam1 的 roi（约 725×671）上 p50 8.7 ms，cam3（约 596×264）p50 4.0 ms；
`color_map` 整幅 1280×1024 单线程约 75 ms、8 线程约 20 ms；`color_model` 一次约 20 ms。

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

## 变更记录

- **2026-10-29（联调后）**：GS 在 seg-new14 NEW-14 cam1 段 3 帧 12 上发现与 1.1 不等价：`fan_scan` 的 `angles[]`、`peaks[].angle` 舍入到 3 位小数，
  `track_path` 接 fan 后用舍入的方向起跑（−45.670° 对 1.1 的 −45.66976°），同一个峰找到的胶点从 4 个变成 2 个，选中的候选随之不同。
  改为：新算子的全部输出不再舍入（`glue.Polyline` 除外）；`track_path` 从 fan 记录读到的就是完整精度的方向。`fan_scan` 加参数 `includeInvalid`
  （默认 false，打开即 1.1 的挑法）。端口与其余参数不变。
- **2026-10-29（联调后，提速）**：GS 实测单帧约为 1.1 的两倍，瓶颈在 `glue.response`（整幅输出 + 执行器给整幅图做统计）。按 GS 选的方案 A：
  `glue.response` 的三个 Image 输出端口合并成一个 `maps: Bundle<glue.ResponseMaps>`（`response`、`gray`、`level` 只覆盖 roi 那一块，加 `frame`
  给块在整幅图里的位置）；`fan_scan`、`track_path`、`lateral_runs` 的 `response` / `gray` / `level` 输入合并成一个 `maps` 输入，`lateral_runs`
  的 `level` 字段因此总有值。roi 与图像不相交是 `bad_param`。采样结果与之前逐位相同（块外原来是 NaN，现在是块外，都算无效采样）。
  执行器的图像统计提速：u8 / u16 结果精确，**f32 的 mean 只差在舍入的末位**。

## 版本与兼容

- 产品与 ABI 版本为 **1.3.1**，glue 包版本为 **0.3.1**。
- `glue.color_track` 与 `glue.color_runs` 为 **1.0.1**；参数和输出结构兼容，增加受参考宽度与中心约束的内谷合并，短缝也禁止跨明确背景。
- C ABI 布局没有变化；按 [packaging.md](deployment/packaging.md) 的系列规则，1.3 客户端可以加载同系列的 1.3.x 引擎。
- `packaging/release.json` 的 `requiredOperators` 包含 6 个测量算子和 5 个彩色算子；彩色接口使用 Bundle `glue.ColorMaps` 与 Record `glue.ColorModel`，保留既有测量算子的端口与输出。

## 实测耗时

2026-10-29，本机（Windows 11、MSVC 14.44、RelWithDebInfo），seg-new14 cam2 录制的 22 帧 1280×1024 灰度图，`lyflow eval --no-cache`、
节点并行度 4（宿主经 C ABI 运行时的默认），每组跑两遍、两遍都列出。「整图」= `run_finished.durationMs` − 读图节点（宿主注入图像，不读文件）。
对照组是同一台机器、同一批帧上的 1.1 算子。GS 的机器比这台快，绝对值以 GS 实测为准，看相对 1.1 的差。

| 图 | 1.2 整图 p50 / p95（ms） | 1.1 对照整图 p50 / p95（ms） |
|---|---|---|
| 示教走廊：`response`（roi = 走廊外包框 ± 卡尺半长 + 4）→ `polyline` → 2 × `lateral_runs`（站距 10、卡尺半长 150） | 6.0 / 7.9；5.6 / 6.5 | `taught_path` → `bead_width`：4.0 / 5.2；4.2 / 5.6 |
| 逐帧定向：`response`（roi = 出胶点 ± (300 + max(60 + 8, 150 + 5))）→ 扇形扫描 → 3 × 粗跟踪 → 3 × 卡尺，再加回退一支（扇形扫描 + 粗跟踪 + 卡尺），一次跑完 | 11.7 / 14.3；11.0 / 12.4 | 只有搜索那张图 `bead_path` → `bead_width`：6.8 / 9.3；6.7 / 8.5（1.1 回退要再跑一张图） |

逐帧定向的节点（p50，ms）：`glue.response` 4.5，`glue.fan_scan` 0.1，`glue.track_path` 每个约 0.8，`glue.lateral_runs`（31 站）每个约 1.0。
示教走廊的 `glue.response` 2.3。

这一轮的提速（2026-10-29 联调后）：
- `glue.response` 只输出 roi 那一块（`maps.frame` 给偏移），不再分配整幅图、填 NaN（约 3.5 ms），执行器给事件做的逐通道统计也只扫这一块；
  三次 5×5 平滑只在块外 2 px 以内做（结果一位不差）。
- 执行器的图像统计：u8 / u16 按整数累加（精确），f32 分 8 条累加链、不分支。整幅 u8 从 2.2 ms 到 0.5 ms（1.1 的链路一样受益），
  整幅 f32 每张从约 1.6 ms 到 1.0 ms。**f32 的 mean 只差在舍入的末位**，min / max 不变。
- 拟合胶路的高斯平滑权重只算一遍（原来逐点逐抽头调 `exp`），1.1 的 `bead_path` 一样受益，结果一位不差。

