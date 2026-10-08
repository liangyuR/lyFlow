# glue — LyFlow 领域算子包：涂胶检测

版本 0.1.0，**默认关闭**（ADR-0015）。打开它：`$env:LYFLOW_PACKS = "glue"`（与别的领域包一起：`"gap;dts;glue"`）。

对胶枪相机的一帧图像，用几个叫得出名字的积木量出胶宽、查出断胶、量出胶到零件边的距离，并判 OK / NG
（[docs/glue-plan.md](../../docs/glue-plan.md) §2，G2）。一张检测图 6 个节点：

```
io.load_image → glue.bead_path → glue.bead_width → glue.bead_breaks ─┐
   读图            定胶路            量胶宽      └→ glue.edge_distance → glue.judge
                                                      量边距            判定
```

本包已适配主线 Image ABI v15，复用 `lyflow::cvx`。完整运行接受 **u8 灰度、RGB、RGBA**；其他位深先经 `image.normalize`。
缩小预览图无法与示教模板、站点文件及标定对齐，会明确报错，请用完整运行。
包机制见 [docs/op-packs.md](../../docs/op-packs.md)，迁移及验收见 [集成记录](../../docs/glue-integration.md)。

## 目录

```
packs/glue/
  algo/      算法内核（D1–D16）：响应图、卡尺、定胶路、逐站、零件边、人造断胶。纯函数，吃 cv::Mat
  ops/       8 个算子 + 两种 Bundle + 片段注册；Record / Bundle 的读写在 common.cpp
  tests/     doctest（合成图：已知宽度、断口、边距、标定，§4 第 13 条）
  snippets/  随包的片段「涂胶检测」（glue.bead_inspect）
  graphs/    glue1 / glue2（随动单帧检测）、flyshot（飞拍定位 + 逐点卡尺）
  tools/     真实帧的批量评估、人造断胶数据集、叠画缩略图（只调 lyflow CLI，D14）
```

**算法只有一份**（D11）：积木算子、doctest、CLI（`lyflow run / eval / dump`）走的都是 `algo/` 里的同一组函数；
积木之间经 Bundle / Record 交接，下游积木不重算上游的结论（`bead_width` 自己再算一次它那一块的响应图，
与 `bead_path` 用同一个函数、同一个结构元）。

## 构建

```powershell
$env:LYFLOW_PACKS = "glue"
pnpm check          # 或 pnpm core:build / pnpm dev
pnpm check:glue     # LYFLOW_PACKS=glue 的 pnpm check；设了 LYFLOW_GLUE_DATA 时再跑真实帧评估
```

依赖只有 OpenCV，而且**只经 std-image 的 `lyflow_opencv_support`**（D13）：本包不自己 `find_package`，
configure 时没有这个目标就 FATAL（`LYFLOW_STD_PACKS=0` 的纯平台构建带不动这个包 —— 那是设计）。

## 约定（glue-plan §2.1）

- **坐标**：图像像素，x 向右、y 向下、像素中心在整数坐标。所有位置、长度默认 px。
- **胶路参数 s**：从喷嘴出胶点沿胶路的弧长（px）。胶路的第一个点离喷嘴恰好 `zone.start`，之后按这条（平滑过的）
  胶路的真实弧长累加；**检测区** `zone = [start, end]` 是 s 的一段。
- **站**：检测区里每隔 `stationStep` 一站；切向 `t` 指向远离喷嘴的方向，法向 `n = (−t.y, t.x)` 指向行进方向的右侧。
  卡尺沿 n 布，站上的 `lo / hi` 是胶两边沿 n 的偏移。
- **单位**：没接标定一律 px（`Measurement.unit = "px"`）。`bead_width` / `edge_distance` 有可选输入
  `calib: Record<image.PlaneCalib>`（格式见 [std-image 的 README](../std-image/README.md)「工作平面标定」）：
  接了之后宽度、距离、断口长度都按映射后两点的距离（断口是映射后的胶路长度）出 mm。**判定与断口的阈值跟着 bead 的单位走**
  （`bead_breaks.minLength`、`judge` 的限值在接了标定时按 mm 解释）。叠画永远是像素。

## 积木

| id | 输入 → 输出 | 人要填的（其余在「高级」） |
|---|---|---|
| `glue.bead_path` 定胶路 | `image` → `path: Bundle<glue.Path>`、`overlay` | `nozzle`（喷嘴像素坐标）、`zone`、`sector`；高级：`headingSource`/`heading`/`headingTol`、`widthMax`、`polarity`、`maxGap`、`minCoverage`、`contrastMin`、`sharpMin` |
| `glue.bead_width` 量胶宽 | `image`、`path`、`calib?` → `bead: Bundle<glue.Bead>`、`widthMean`、`widthMin`、`widthMax`、`coverage`、`overlay` | `form`（straight / swirl）、`widthRange`；高级：`stationStep`、`searchHalf`、`mergeGap`、`window`、`contrastMin`、`presentRatio`、`centerRatio`、`contrastRatio` |
| `glue.bead_breaks` 查断胶 | `bead` → `breaks: Record<glue.Breaks>`、`count`、`longest`、`overlay` | `minLength`（20 px）；高级：`countAtZoneEnds` |
| `glue.edge_distance` 量边距 | `image`、`bead`、`calib?` → `edge: Record<glue.Edge>`、`distanceMean`、`distanceMin`、`distanceMax`、`overlay` | `searchLength`（200）；高级：`reference`、`edgePolarity`、`contrastMin`（40）、`farRun`（12） |
| `glue.judge` 判定 | `bead`、`breaks`、`edge?` → `verdict: Record<glue.Verdict>`、`ok`、`overlay` | `widthLimits`、`distanceLimits`（0 = 不判那一侧）、`maxBreak`（允许的断口长度，0 = 一处都不许，负数 = 不判断胶）、`minDefectLength`（20） |
| `glue.synth_break` 人造断胶（工具） | `image`、`bead` → `image`、`info: Record<glue.SynthBreak>`、`overlay` | `sStart`、`length`；高级：`margin`、`shiftExtra`、`dirtyMax`、`contrastMin`、`feather` |

加载期校验（`validate`）：`zone` 起止倒了、`sector` 从大到小或跨度超过 360°（只在扇形搜索时查）、`widthRange` 倒了、
`searchLength` 不比 `farRun` 长 10 px、判定的上下限倒了或是负数 —— 图根本跑不起来，诊断指到那个参数。
`searchHalf` 放不下最宽的胶、螺旋胶的 `window` 比站距还短是警告。

### 两种 Bundle（manifest 的 `bundles` 段）

- **`glue.Path`** = `line`（Record `glue.Polyline`）+ `info`（Record `glue.PathInfo`）
- **`glue.Bead`** = `line`（同上：Bundle 不嵌套，折线本身带过来）+ `stations`（Record `glue.Stations`）+ `info`（Record `glue.BeadInfo`）

### Record 的形状

| type | data |
|---|---|
| `glue.Polyline` | `count`、`spacing`（2）、`s[]`、`points[[x,y]]`、`tangents[[tx,ty]]`（单位向量） |
| `glue.PathInfo` | `ok`、`message`（没找到胶时是「检测区内没找到胶」）、`reason`（为什么，给人看）、`heading`（度）、`headingSource`（search / param）、`coverage`、`sharpness`（D15 的 P25）、`beadlike`、`residual`、`zone`、`nozzle`、`polarity`、`widthMax`、`lineSource`（fit / fallback）、`coarseCount`、`candidates[{heading, fanScore, coverage, sharpness, beadlike, score}]` |
| `glue.Stations` | 数组形式，逐站对齐：`s[]`、`center[]`、`normal[]`、`present[]`、`lo[]` / `hi[]`（沿法向的偏移，px）、`left[]` / `right[]`（像素点）、`width[]`（按 `unit`）、`widthPx[]`、`contrast[]`；另有 `count`、`step`、`form`、`unit`。无胶的站这些字段是 `null` |
| `glue.BeadInfo` | `form`、`stationStep`、`unit`、`pathOk`、`message`、`zone`、`stations`、`present`、`coverage`、`wRef`、`peakRef`、`envelopeHalf`、`searchHalf`、`polarity`、`widthMax`、`calib`（接了标定时那一份，否则 `null`） |
| `glue.Breaks` | `count`、`longest`、`unit`、`minLength`、`stationStep`、`countAtZoneEnds`、`pathOk`、`breaks[{sStart, sEnd, length, lengthPx, start, end, stations, atZoneStart, atZoneEnd}]`、`ignored[]`（countAtZoneEnds = false 时贴着端点的）、`shortGaps` |
| `glue.Edge` | `side`（right / left / none）、`votes{right, left}`、`reference`、`edgePolarity`、`unit`、`count` / `searched` / `found` / `kept`，逐站 `s[]`、`edge[]`、`from[]`、`distance[]`（按 `unit`）、`distancePx[]`、`status[]`（ok / outlier / none / nobead） |
| `glue.Verdict` | `ok`、`message`、`unit`、`pathOk`、`defectCount`、`counts{missing, break, narrow, wide, near, far}`、`defects[{type, sStart, sEnd, length, value, limit, unit, start, end, message}]`、`warnings[]`、`limits` |
| `glue.SynthBreak` | `ok`、`reason`、`side`（源带在哪一侧）、`dirtyRight` / `dirtyLeft`、`stations`、`sStart` / `sEnd` / `length`（断口的真值，原帧胶路的 s）、`start` / `end`（断口两端在图上的点） |

**判定的约定**：`verdict.message` 是一句话 —— 全部合格是 `OK`；胶路没找到时**恰好是**「检测区内没找到胶」（只报这一条
`missing`，整段无胶这件事不再重复报成断胶）；否则是 `NG：断胶 1 处（最长 32.0 px）；窄胶 2 处` 这样按类型数一遍。
缺陷的 `type` 六种：`missing` / `break` / `narrow` / `wide` / `near` / `far`。断胶以外都是「连续 ≥ minDefectLength 的同类站」
（D7）；断胶由 `bead_breaks` 按它的 `minLength` 连好，比 `maxBreak` 长的判 NG。设了边距限值却没接 `edge`、或两侧都没找到
零件边时，边距不判，`warnings` 里写一句（不判 NG）。
**`ok` 输出**是 Measurement：`value` 1 = OK、0 = NG，`ok = true`（判定本身总是做得出来），`verdict` 字段 `ok` / `ng`，
`message` 同 `verdict.message`。

**失败语义**（ADR-0016 错误即值）：没有胶本身就是一个 NG 结果，不是执行错误 —— `bead_path` 覆盖率低于 `minCoverage`
或不像胶（D15）时 `info.ok = false`，折线是沿选中方向的一条直线；下游照常跑：`bead_width` 全部判无胶、`bead_breaks`
报出整个检测区一段、`judge` 报「检测区内没找到胶」。

## 固定相机飞拍后检（glue-plan §5，G4）

打胶之后机器人持工件在固定相机下连续运动、按位置触发，一件分 N 帧（拍照点 k）。宿主（ly-TuJiaoVision）持有名义胶路与
每个测量点归哪一帧，逐帧把这一帧负责的测量点交给图；图只做定位修正与逐点卡尺，**判定与断胶的跨帧合并在宿主**。
示例图 `graphs/flyshot.lyflow.json`：

```
io.load_image ─→ glue.locate ─→ glue.station_calipers ←─ image.load_calib
  （宿主注入）      定位   pose     逐点卡尺          calib   工位标定
```

顶层参数 `template`、`anchor`、`stations`、`calib` 由宿主逐帧给（拍照点 k 的模板、模板锚点、测量点文件、工位标定文件）。

| id | 输入 → 输出 | 人要填的（其余在「高级」） |
|---|---|---|
| `glue.locate` 定位 | `image` → `pose: Record<glue.Pose2D>`、`score`、`overlay` | `template`（Path）、`anchor`（point）、`searchRadius`（80 px）、`minScore`（0.6）；高级：`angleRange`（3°）、`angleStep`（0.5°） |
| `glue.station_calipers` 逐点卡尺 | `image`、`pose?`、`calib?` → `measure: Record<glue.StationMeasure>`、`coverage`、`distanceMean`、`overlay` | `stations`（Path）、`searchHalf`（48 px）、`innerSearch`（[−40, −3]）、`beadSearch`（[−15, 40]）、`innerPolarity`、`innerSelect`；高级：`caliperWidth`、`beadPolarity`、`contrastMin`（25）、`widthMin`（3 px） |

**站的约定**（K3）：站点在名义胶条中线上，法向从内边（开口一侧）指向翻边，卡尺上 `t < 0` 是内边一侧。
测量点文件：`{"points": [[x, y], …], "normals": [[nx, ny], …], "ids": […]}`（示教图坐标，px；`ids` 可省，省了是下标）。

| type | data |
|---|---|
| `glue.Pose2D` | `ok`、`message`、`score`、`angle`（度，屏幕上顺时针为正）、`center`、`teachCenter`、`offset`、`templateSize`、`minScore`；`T(p) = R(angle)·(p − teachCenter) + center` 把示教图坐标映射到这一帧 |
| `glue.StationMeasure` | `count`、`unit`、`poseGiven`、`poseOk`、`counts{ok, noInner, noBead, incompleteBead, poseFail, outOfImage}`、`calib`，逐站对齐：`ids[]`、`point[]`、`normal[]`（这一帧里的）、`status[]`（ok / no_inner / no_bead / incomplete_bead / pose_fail / out_of_image）、`inner[]` / `near[]` / `far[]`（沿法向的偏移 px）、`innerNear[]` / `innerCenter[]` / `width[]`（按 `unit`）、`innerNearPx[]` / `innerCenterPx[]` / `widthPx[]`、`innerContrast[]`、`beadContrast[]`；不成立的是 `null` |

**失败语义**：定位分数低于 `minScore` 时 `pose.ok = false`，逐点卡尺全部 `pose_fail`（错误即值，宿主报定位失败）；
内边没找到是 `no_inner`（测不了），找到内边但没有胶是 `no_bead`（断胶的证据）。胶边被窗口截断是 `incomplete_bead`，宿主应作为无效量测处理，不能算断胶或 OK。纯色/低对比度模板返回 `pose.ok=false`。

## 叠画（lyflow.overlay2d 的 role）

算子以 `lyflow.overlay2d` Record 输出下列语义角色。宿主或离线工具按 role 上色；当前主线编辑器尚未渲染这种 Record：

| 积木 | role |
|---|---|
| bead_path | `nozzle`（喷嘴圆）、`sector`（扇区的两条边）、`zone`（检测区两端的弧）、`coarse`（粗找到的胶点）、`path`（胶路）；没找到胶时 `missing`（那条直线）+ `ng`（文字） |
| bead_width | `path`、`station`（有胶的站：左边 → 右边）、`missing`（无胶的站）、`edge.left` / `edge.right`（两条胶边） |
| bead_breaks | `break`（每处断口沿胶路一段，label「断胶 32 px」）、`coarse`（countAtZoneEnds = false 时不计的端点段） |
| edge_distance | `distance`（量起点 → 零件边）、`part`（零件边点）、`outlier`（被中值 + MAD 剔掉的点） |
| judge | `path`（或 `missing`）、`edge.left` / `edge.right`、`part`、`defect`（每处缺陷沿胶路一段，label 是那一条的 message）、结论文字 `ok` / `ng` |
| synth_break | `break`（盖掉的那一段）或 `coarse`（没做成） |

像素叠画格式见 `schema/overlay2d.schema.json`，底图、缩放及呈现由宿主处理。

## 算法（D1–D16）与实现上的取舍

参考实现是会话草稿里的 `bead3.py`（Python，不进仓库）；C++ 版语义一致，下面几处改了具体做法（理由与实测见
[集成记录](../../docs/glue-integration.md)；G2 原始验收保留在 `feat/glue` 分支 `effa2ed`）：

- **D3 响应图**：黑顶帽，结构元直径 widthMax + 1 —— 用**正八边形**（方形 ⊕ 两条对角线段）近似圆盘，整幅 1280×1024
  约 4 ms（OpenCV 的椭圆核 120 ms）；只算用得到的那一块（外扩两倍半径，块里的值与整幅图逐位相同）。
- **D1 定胶路**：扇形搜索取前三个峰（D6），每个方向从 `zone.start` 起 8 px 一步粗找（卡尺上离预测点最近的暗段），
  局部线性稳健拟合（±40 px，三轮 MAD 降权）。另加：胶点要往外走（离喷嘴 ≥ 0.75·s）、方位不出搜索范围 20°；一步接一步时
  横着不超过半个胶宽（断口一开头卡尺会够到胶旁边的阴影）；断口后重新接上的胶点要与此前的胶一样浓、边缘一样陡（直胶还要
  一样宽），跟丢了退回选中的那条射线上找；射线本身几乎没响应的候选打对折；
  拟合后再做 σ = 12 px 的高斯平滑，s 按平滑后胶路的弧长算（螺旋胶的胶点左右摆，不平滑弧长会长出一截）。
- **D4 / D16 逐站**：半高取边、缝小合并；参考宽 / 参考峰值取全检测区候选暗段的中位数，另加一条**对比度一致**：
  峰值不到参考峰值 0.3 倍的站判无胶（断口两头模糊出来的残影）。
- **D9 零件边**：第一个「近侧亮、远侧 farRun px 都暗下去、两侧差 ≥ contrastMin」的跳变，位置取跳变里下降最快的一点（亚像素）；
  出图的采样截断剖面，不把图像边界当零件边。
- **人造断胶**：源带跨过零件边也算脏（零件边在黑顶帽里没有响应，只看响应挡不住它）；羽化带里用同一个核做加权平均，不拉暗。

## 片段与示例图

- **片段**「涂胶检测」（`glue.bead_inspect`，`snippets/bead-inspect.lyflow-snippet.json`）：上面那 6 个节点、9 条边。
  插入后填写读图路径和 nozzle 像素坐标；相机输入将 `source` 改为 `inputs`，宿主注入 `n_load.image`。
- **示例图** `graphs/glue1.lyflow.json`（直胶）、`graphs/glue2.lyflow.json`（螺旋胶，`form = swirl`、检测区 [150, 400]）：
  喷嘴 (712, 598)、扇区 −130°…−10°，判定只判断胶与无胶（没有工艺的胶宽 / 边距规格）。读图节点是 `source = inputs`
  （宿主逐帧注入）；拿文件跑：

  ```powershell
  lyflow run packs\glue\graphs\glue1.lyflow.json --set 'n_load.source="file"' --set 'n_load.path="<帧.jpg>"' --outputs
  lyflow eval packs\glue\graphs\glue1.lyflow.json --set 'n_load.source="file"' `
      --samples-glob "<目录>\Frame*_1.jpg" --bind n_load.path --metric outputs.ok --metric outputs.breakCount --summary
  ```

  图级输出：`ok`、`verdict`、`pathInfo`、`coverage`、`widthMean` / `widthMin` / `widthMax`、`breakCount`、`longestBreak`、
  `breaks`、`distanceMean`、`overlay`（判定的叠画）。没找到胶的帧里胶宽、边距是 `null` —— `lyflow eval` 在第一个样本上
  取不到值的指标路径会报错退出，逐帧的胶宽、断口位置从 `--summary` 的图级输出读（tools 就是这么做的）。

## 工具（`tools/`，D14：只调 lyflow CLI，不自己做检测）

数据由 `LYFLOW_GLUE_DATA` 给（演示数据的解压目录，下面有 `Glue1/`、`Glue2/`）。真实帧与它们的叠画都是客户数据（D12），
**输出只写到仓库外**（`--out`，默认 `%TEMP%\lyflow-glue-*`；给了仓库里的目录会直接拒绝）。

```powershell
$env:LYFLOW_GLUE_DATA = "<演示数据解压目录>"
python packs\glue\tools\glue_eval.py      # 第 14 / 15 / 18 条：lyflow eval 跑全部帧，逐帧明细 + 满胶段误报、无胶帧、耗时
python packs\glue\tools\glue_synth.py     # 第 16 条：满胶帧 × {15, 30, 60} px 人造断口，检出率、起止误差、误报
python packs\glue\tools\glue_thumbs.py    # 第 17 条：每帧画上判定的叠画 + 几张总览图
```

- `glue_eval.py` 与 `glue_synth.py` 没过门槛时退出码 1，报告是 `--out` 下的 `report.md` / `report.json`（带逐帧 / 逐样本表）。
- `glue_synth.py` 由示例图拼一张「两条链」的图（读图 → 定胶路 → 量胶宽 → **人造断胶** → 定胶路 → 量胶宽 → 查断胶，外加原帧
  自己的查断胶）写在 `--out` 下，样本集 `--seed` 固定；断口真值是原帧胶路上的两个端点，投到这一帧自己的胶路上再比
  （两条检测链的 s 不是同一把尺子）。`--dump-failures` 把没过的样本 dump 出图、画上叠画。
- `glue_thumbs.py` 的配色与编辑器同一张 role 表（`glue_draw.py`），中文用 PIL + 系统字体写。
- 找 CLI：`--lyflow` / `LYFLOW_EXE`，否则 `bridge/target/{release,debug}/lyflow.exe` 里最新的、manifest 里有 glue 的那一个。

## 测试

- **doctest**（`tests/test_glue.cpp`，进 `lyflow-core-tests`）：合成图（渐变背景、σ = 2 的高斯噪声、σ = 1 的模糊、一侧零件边、
  喷嘴暗影；螺旋胶是「沿法向左右摆的一股胶」）上直胶与螺旋胶各四档宽度 {8, 20, 40, 70} 的胶宽 / 边距中位误差 ≤ 1 px、
  零误报；断口 {10, 20, 40, 80} 一帧一个，`minLength = 20` 恰好报出 ≥ 20 的，起止误差 ≤ 4 px；没有胶、只有压痕 → 「检测区内
  没找到胶」；合成单应下宽度 / 边距 / 断口长度按映射后两点的距离；坏的标定是 `bad_input`；加载期校验；响应图分块与整幅
  逐位相同；人造断胶挑干净的一侧、另一条链查得出；判定的叠画写一份样例给 `pnpm check` 对着 schema 校验。
- **飞拍宿主链路**（`tests/test_flyshot.cpp`）：实际载入随包产线图，C ABI 注入带行填充的相机帧、绑定图参数、读取毫米点表，并验证换帧不会误用旧缓存。
- **标定**（`packs/std-image/tests/test_calib.cpp`）：棋盘真值、中文路径往返、非法角点参数和错误 Record 类型。

## 已知限制

- **收胶帧**（Glue2 #69–71：胶已离开喷嘴）：旁边零件的锐利接缝可能被当成胶，D15 挡不住；G3 的 `heading` 把方向钉住就不会，
  现场这几帧本来也由机器人程序屏蔽（glue-plan §2.4）。
- **螺旋胶的断口起点**：断口从一个螺圈的外沿开始时，前一站的外包络里还有回卷过来的胶，起点会报晚；人造断胶里 60 px 的断口
  最多晚 20 px（不到一个螺距，统计见验收记录）。
- 只做单帧；整条胶的拼接、深度学习分割、胶高、起胶收胶段的自动屏蔽都不在 G2（glue-plan §2.4）。
