# dts — LyFlow 领域算子包：车门胶条面差

版本 0.1.0，**默认关闭**（ADR-0015）。打开它：`-DLYFLOW_PACKS=dts`。

把车门密封胶条的**面差**（胶条底部到旁边钣金基准面的垂距）测量主路径拆成八个
`dts.*` 算子。一次 run 处理**一条**激光轮廓：进来一片剖面点云，出去一个
`Measurement` 加一份诊断 Record。分段、分箱、跨样车统计与 OK/NG 判定都在宿主
`dts-check` 那边（见 [不做](#不做)）。

算子包机制本身见 [docs/op-packs.md](../../docs/op-packs.md) 与 ADR-0013 / ADR-0014；
算法为什么住在这里见 [ADR-0015](../../docs/adr/0015-algorithms-live-in-lyflow-packs.md)；
源数据怎么从宿主进来见 [ADR-0017](../../docs/adr/0017-graph-outputs-injection-importers.md)。

## 目录

```
packs/dts/
  ops/     8 个 dts.* 算子（register.cpp 按链的顺序注册）
  algo/    算法源码：滑动中值、分片、分面、圆/直线拟合、找凸起。零第三方依赖
  graphs/  default.lyflow.json（基准取外板那一族面）
           adjacent.lyflow.json（基准取紧贴胶条那一族，只差 angleToApexDeg）
  tests/   随包走的 doctest + 实测轮廓数据头
```

`algo/` 里只有领域逻辑，**不链 PCL**：直线与圆的最小二乘、滑动中值、按 x 间断与
z 跳变的分片、滑窗找平、凸起搜索全是包内实现。产线上带一个 `lyflow_core.dll` 就够。

## 构建

```powershell
cmake -S core -B build-dts -G Ninja `
  -DLYFLOW_STD_PACKS=0 -DLYFLOW_PACKS=dts -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-dts --target lyflow_core lyflow-core-tests
build-dts\bin\lyflow-core-tests.exe
```

`LYFLOW_STD_PACKS=0` 把 `packs/*` 全关掉，`LYFLOW_PACKS=dts` 再单独点名这一个，
于是这份构建 = core + dts，manifest 里是四个 core 算子（`gen.synthetic`、
`util.reroute`、`flow.fallback`、`flow.select`）加八个 `dts.*`。
`dumpbin /dependents build-dts\bin\lyflow_core.dll` 只见 `KERNEL32.dll` 与 CRT ——
不用装 PCL、不用拷 onnxruntime、不用 vcpkg。

外部宿主（按 cargo **git 依赖**引 `bridge` crate 的那种，`dts-check` 就是）走同一条路：
`bridge/build.rs` 把 `LYFLOW_STD_PACKS` / `LYFLOW_PACKS` 原样 `-D` 转给 cmake，
所以宿主侧只要设这两个环境变量。

## 算子

链是固定的一条，`default.lyflow.json` 就是它：

```
dts.profile_in → profile_clean → split_faces → seal_dome → pick_metal
                                                → seal_root → flush → profile_bundle
```

| id | 输入 → 输出 | 关键参数（默认值） |
|---|---|---|
| `dts.profile_in` | → `profile: PointCloud` | `source` = `injected`（另一种 `file`）、`path`（`source=file` 时才显示） |
| `dts.profile_clean` | `profile: PointCloud` → `clean: PointCloud` | `smoothPoints` = 5（滑动中值窗宽，取奇数，1 = 不平滑）、`minIntensity` = 0.5 |
| `dts.split_faces` | `clean: PointCloud` → `faces: Record(DtsFaces)` | `gapMm` = 1.0、`jumpMm` = 1.5、`minPoints` = 15、`fitLenMm` = 5.0、`maxRmsMm` = 0.05、`angleBreakDeg` = 6.0、`minLenMm` = 3.0 |
| `dts.seal_dome` | `clean`, `faces` → `dome: Record(DtsDome)`, `apex: Line2D` | `sealBulge` = `+z`、`minLenMm` = 5.0、`maxLenMm` = 30.0、`minHeightMm` = 1.5、`maxRadiusMm` = 20.0、`walkTolMm` = 0.15、`apexFitHalfMm` = 6.0、`radiusMinMm` = 7.0、`radiusMaxMm` = 14.0 |
| `dts.pick_metal` | `clean`, `faces`, `dome` → `metal: Line2D`, `pick: Record(DtsMetalPick)` | `angleToApexDeg` = −55.0、`angleTolDeg` = 40.0、`fitLenMm` = 5.0、`sealSide` = `+x` |
| `dts.seal_root` | `clean`, `faces`, `dome`, `pick` → `root: Record(DtsRoot)`, `point: Point2D` | `sealBulge` = `+z`、`promSmallMm` = 0.03、`promMetalMm` = 0.30、`footReachMm` = 3.0 |
| `dts.flush` | `root`, `metal` → `flush: Measurement`, `foot: Point2D` | `sealBulge` = `+z`、`positiveDirection` = `deeper` |
| `dts.profile_bundle` | `faces`, `dome`, `pick`, `root`, `metal`, `flush`, `foot`（可选） → `bundle: Record(DtsProfileBundle)` | 无 |

几条容易踩的：

- **`profile_clean` 保持列序**，不按 x 排序。分片靠相邻列的 x 间断与 z 跳变判断，
  排过序的轮廓会把重叠面交错起来。
- **`split_faces` 的面表是挑基准面的依据**。挑面从藏在启发式里变成「看着这张表挑」，
  落选候选连同落选理由一起进 `DtsMetalPick.rejected`，在画布上点开那个节点看得到。
- **`seal_dome` 的顶点轴是圆心 → 顶点**，不是两端脊点的连线：圆拟合吃顶部两百多个点，
  实测绝对角 std 1.9° 对 14.8°。半径落在 `[radiusMinMm, radiusMaxMm]` 之外直接判
  「这不是胶条」（`no_apex_axis`）。
- **`pick_metal` 只在选中面朝胶条那一端取 `fitLenMm` 一小段拟合**。整块面可能几十毫米长
  且远端微弯，拿整块拟合会把弯曲摊进基准线。
- **同一截面钣金侧常有两族面**（实测夹角差约 70°，是同一道屋脊的两侧），
  `angleToApexDeg` 决定取哪一族：外板那一族在 −34..−75°，紧贴胶条那一族在 −116..−145°。
  `adjacent.lyflow.json` 与 `default.lyflow.json` 的唯一差别就是这一个参数（−120 对 −55）。
- **`seal_root` 的搜索区间锚在「基准面朝胶条那一端 → 胶条顶点」**，不锚在凸包端点上 ——
  端点会滑进台肩，窗口整个偏到钣金上去。

**单位**：端口上流动的 x/z 一律**毫米**，与参数同单位，不换算。
这一点与 `packs/gap` 相反（那边端口是米），因为传感器给的就是毫米，
而这条链上没有任何一步需要和米制的点云算子拼接。y 恒为 0。

## 注入契约

产线与回放时轮廓由**宿主注入**，不从磁盘读：

- 注入的目标是**节点** `dts.profile_in`，端口 `profile`，类型 `PointCloud`
  （`kind = LYFLOW_INPUT_POINT_CLOUD`）。节点被注入后在编译期标 `provided`，
  **整个 compute 不调用**，`source` / `path` 两个参数一并作废（ADR-0017）。
- **`y` 恒为 0**，`x` / `z` 单位**毫米**。x 是沿轮廓的横向坐标，z 是到传感器的方向，
  `sealBulge = +z` 意思是胶条朝传感器鼓。
- **`intensity` 必给且与点数等长**：有效点写 `1.0`，无效点写 `0.0`。
  传感器的原始亮度**不往里放** —— 门限 `minIntensity = 0.5` 只用来区分这两档，
  把原始亮度放进去等于让图上的一个浮点参数去挑传感器的曝光，那是宿主的事。
- **宿主按 op id 找唯一源节点**：图里 `op == "dts.profile_in"` 的节点必须**恰好一个**，
  宿主拿它的 id 去填 `lyflow_run_input.node_id`。节点 id 不是契约，op id 才是 ——
  所以改图时可以随便改节点 id，不能加第二个 `dts.profile_in`。
- 注入数据的 xxh3 摘要进 cacheKey，换一片云不会命中上一片云的结果。

离线调图时把 `source` 改成 `file`，`path` 指一个 CSV：每行 `x,z[,亮度]`
（逗号、分号、Tab、空格都行；`#` 开头是注释），亮度缺省 1.0，**0 表示无效点**。
这条路只为调参存在，产线不用。

## 输出

顶层 `outputs` 六个（两张图一致）：

| 名字 | 来自 | 类型 |
|---|---|---|
| `flush` | `n_flush.flush` | `Measurement`，单位 mm，`verdict` 恒空 |
| `metal` | `n_metal.metal` | `Line2D`，带拟合段两个端点 |
| `root` | `n_root.root` | `Record(DtsRoot)` |
| `bundle` | `n_bundle.bundle` | `Record(DtsProfileBundle)` |
| `faces` | `n_faces.faces` | `Record(DtsFaces)` |
| `dome` | `n_dome.dome` | `Record(DtsDome)` |

宿主落库与画界面只需要 `bundle` 一个，其余五个是给画布上点开看的。

### `DtsProfileBundle`

| 字段 | 内容 |
|---|---|
| `ok` | `Measurement.ok` |
| `flushMm` | 面差；`ok` 为假时是 `null` |
| `root` | `DtsRoot` 整份：`x` / `z` / `method`（`bump` 或 `valley`）/ `nBumps` / `promMetalMm` / `promDomeMm` / `searchX0` / `searchX1` / `bumpLimitX` |
| `dome` | `DtsDome` 整份：`xPeak` / `zPeak` / `xLo` / `xHi` / `heightMm` / `widthMm` / `radiusMm` / `centerX` / `centerZ` / `apexAngleDeg` |
| `pick` | `DtsMetalPick` 整份：`faceIndex` / `gapMm` / `relDeg` / `edgeX` / `angleDeg` / `rmsMm` / `nInliers` / `faceX0` / `faceX1` / `faceLenMm` / `sealSign` / `rejected[]` |
| `faces` | 面表，逐面 `x0` `x1` `z0` `z1` `lenMm` `angleDeg` `rmsMm` `n` `px` `pz` `dx` `dz` |
| `pieces` | 分片表，逐片 `i0` `i1` `n` `x0` `x1` |
| `metal` | 基准线段：`x0` `z0` `x1` `z1` `angleDeg` `rmsMm` |
| `perpendicular` | 垂足 `x` / `z`，`foot` 端口接上了才有（可选输入） |

## 不做

以下都在宿主 `dts-check`，不在这张图里：

- **沿边分段**（一次扫描切成 N 段）与**站位分箱**：图一次只看一条轮廓，
  第几条、属于哪一段、哪个站位，图里既拿不到也不该拿。
- **跨样车基线统计**（逐站位的均值 / MAD / 漂移）：同理，那是多次 run 之上的事。
- **OK/NG 判定**：标准表按 `(段号, 站位号)` 存在宿主的库里。所以 `dts.flush` 出的
  `Measurement` 的 `verdict` 与 `hasLimits` / `nominal` / `upper` / `lower` **一律留空**，
  判定由宿主拿 `flushMm` 去查表。
- **读 PLY / 相机取流**：ABI 上只能注入**点云**（`LYFLOW_INPUT_POINT_CLOUD`），
  解析 PLY、丢无效槽、按行切轮廓都发生在宿主，注进来的已经是一条剖面。

## 与宿主 Python 旧算法的对照

宿主 `dts-check` 的 Python 旧算法在三份现场 PLY 上有逐轮廓基线
（`crates/dts-core/tests/fixtures/measurement_reference/*.csv`）。
拿各样本**头三条**轮廓写成 CSV 跑 `lyflow eval`（`data/lyflow-eval/dts/`，不进本仓库）：

```powershell
lyflow eval packs\dts\graphs\default.lyflow.json `
  --samples-glob "<...>\data\lyflow-eval\dts\*.csv" --bind n_in.path `
  --set 'n_in.source="file"' --metric outputs.flush
```

2026-09-17，本包默认参数对 Python 各自的参数组：

| 样本 | 轮廓 | dts (mm) | Python (mm) | Δ (mm) |
|---|---|---|---|---|
| A `DP2240-01P车门上方1.ply` | 0 | 4.2494 | 4.0378 | **+0.2116** |
| A | 1 | 4.0743 | 4.0363 | +0.0380 |
| A | 2 | 4.0795 | 4.0364 | +0.0431 |
| B `车门上方2.ply` | 0 | 3.6315 | 3.7414 | **−0.1099** |
| B | 1 | 3.6294 | 3.7288 | −0.0994 |
| B | 2 | 3.6264 | 3.7360 | −0.1096 |
| D `Image_Profile_20260809_17_22_58_462.ply` | 0 | −4.1787 | 55.4610 | **−59.64** |
| D | 1 | 55.1099 | 55.4587 | −0.3488 |
| D | 2 | 55.1075 | 7.5183 | +47.59 |

A / B 用 Python 的 `default` 参数组，D 用 `d_field`（工艺确认值：A 4.04、B 3.73、D 55.46）。

**A 与 B 是同一个量。** 两边挑的是同一族钣金面（A：dts 的面 x∈[16.1, 27.1] 对
Python 的 [22.6, 27.5]，边缘都落在 27.6 附近；B：dts [−2.1, 4.8] 对 Python [0.6, 5.6]），
剩下的差来自**底部点的取法**：Python 走 `foot_arc_apex`，这里走「凹-凸-凹」的凸起顶点。
B 的 −0.11 mm 是稳定偏置（三条一致）；A 第 0 条的 +0.21 mm 是底部点在同一个凸起上
抖了一格（root z 76.10 对另两条的 76.28），另两条只差 0.04 mm。
**doctest 里对 A / B 第 0 条写死了 `|Δ| < 0.3 mm` 的容差。**

### 已知差距

**D 型（V 形脚）对不上，且两边各自都不稳。** 不在这一轮修。

- D 第 0 条差 59.6 mm：这条轮廓切出 6 块面，其中 x∈[−11.6, −6.6]、与顶点轴夹角
  −46.3° 的那一块落在 `angleToApexDeg = −55 ± 40` 里，而它离胶条更近
  （`gapMm` 8.9 对 57.2），于是 `pick_metal` 的「取最近的合格面」就取了它，
  基准面整个挪到 V 形脚内侧。第 1、2 条切出 5 块面（那一块没切出来），
  取到了与 Python 同一块远端面，于是只差 0.35 mm。
- D 第 2 条差 47.6 mm 是**Python 那一侧**翻了：它的 `seal_side = auto` 在这条上判成
  `−1`，基准面跳到 x ≈ +78.9 那一头去了（读数 7.52），而本包的 `sealSide` 是写死的 `+x`。
- **原因是参数不是算法**：Python 的 `d_field` 参数组给了显式窗口
  `metal_x_range = (−80.3, −60.3)`、`foot_x_range = (−8.8, −0.1)`，等于人工把基准面和
  底部点各钉死在一个 x 区间里；本包没有这两个参数，靠「最近的合格面」+「凹-凸-凹」自己找。
  V 形脚的截面上合格候选不止一个，找法就分岔了。
- 可能的收敛方向（**都没做**）：给 `pick_metal` 加一个可选的 x 窗口参数；
  或者把「最近」改成「最长」/「rms 最小」；或者对 D 这一类单独出一张图，
  用 `angleToApexDeg` 把 V 形脚内侧那一族排除掉。改算法是另一个任务。

复现用的 9 份 CSV 在宿主仓库 `data/lyflow-eval/dts/`（`x,z,1.0`，4 位小数，
无效槽已剔除），`index.json` 记着每份来自哪个 PLY 的第几条。

## 测试

`tests/test_dts_ops.cpp` 随包编进 `lyflow-core-tests`（`lyflow_op_pack.cmake` 的
`TEST_SOURCES` glob 扫 `tests/*.cpp`）。数据头两份：

- `tests/data/real_profiles.h` — 两条 3200 点的实测轮廓，等间距重采样过
  （`kX0Mm` + k·`kDxMm`），带原始亮度 `kI1000` / `kI3000`（0..255，0 是无效点）。
- `tests/data/field_profiles.h` — A / B / D 三份现场 PLY 的第 0 条轮廓，
  连同 Python 旧算法在同一条上的读数，用来跑上面那张对照表里的三行。

用例覆盖：整条链跑到底（两条实测轮廓）、`minIntensity` 的裁点行为、
现场样本对照与 A/B 的容差、以及四条失败路径（空轮廓、z 全 NaN、亮度全 0、
一条没有胶条的直线 → 停在 `seal_dome` 的 `no_dome`）。
