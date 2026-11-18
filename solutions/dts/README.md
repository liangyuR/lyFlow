# solution-dts：车门胶条面差

本场景处理一条激光轮廓，输出测量有效性与毫米面差。胶条主体、半径带、两族钣金、根部优先级和业务正号属于此场景；公共清洗、分片、TLS/代数圆拟合和有符号点线测量分别由 `std-profile` 与公开静态内核提供。

统一运行库包含本包。开发时可用以下命令构建轻量轮廓组合。

```powershell
cmake -S core -B build/profile-dts -G Ninja `
  -DLYFLOW_STD_PACKS=OFF '-DLYFLOW_PACKS=std-profile;std-metrology;std-geometry;solution-dts' `
  -DBUILD_TESTING=ON
cmake --build build/profile-dts --target lyflow_core lyflow-core-tests lyflow-kernel-tests
ctest --test-dir build/profile-dts --output-on-failure
```

这是无 PCL、OpenCV、ORT 的轻量组合；`std-profile` 原标准 TLS 方法仍使用仅编译期 Eigen 头。是否满足目标机器导入表、SDK 与现场质量要求，应以对应构建验证为准。

## 原生轮廓与算法

两张图从 `profile.input` 的 `profile` 输出读取原生轮廓，先用 `profile.clean` 保槽清洗，再用 `profile.split_faces` 分面。默认文件输入声明毫米、XZ 轮廓、右手坐标和采集顺序。使用设备输入时由宿主声明实际单位、坐标帧、手性、采样顺序和来源。

DTS 计算节点接受 `Profile`，要求 `XZ/acquisition`，按声明将有效槽显式转换到毫米工作坐标，保留原描述和源列映射。清洗在原槽窗中运行中值，无效中心不补洞；分片比较相邻有效采样列，分片内部才建立按 x 排序的索引视图。公共 `profile.Faces.v2` 的 `i0/i1` 是 `active_samples` 偏移，不能直接当原采集列。

TLS、代数圆、滑窗、硬半径带与退化阈值继续使用原毫米数值标度；不替换为 PCL/RANSAC。`default-profile.lyflow.json` 选择默认的 `angleToApexDeg=-55` 面族。`adjacent-profile.lyflow.json` 选择 `-120` 面族。

## 节点与单位

| 节点                     | 版本  | 契约                                                                                              |
| ------------------------ | ----- | ------------------------------------------------------------------------------------------------- |
| `dts.seal_dome`          | 3.0.0 | `clean:Profile,faces:Record` → `dome:DtsDome.v3,apex:Line2D`。                                    |
| `dts.pick_metal`         | 3.0.0 | `clean,faces,dome` → `metal:Line2D,pick:DtsMetalPick.v3,metalContext:Record`。                    |
| `dts.seal_root`          | 3.0.0 | `clean,faces,dome,pick` → `root:DtsRoot.v3,point:Point2D,pointContext:Record`。                   |
| `dts.flush`              | 3.0.0 | `root` → `flush:Measurement,foot:Point2D,footInfo:DtsFlush.v3`。                                  |
| `dts.profile_bundle`     | 3.0.0 | 汇总 `faces,dome,pick,root,flush,footInfo` → `DtsProfileBundle.v3`。                              |

裸 `Line2D` 和 `Point2D` 的位置和端点输出为米，方向保持单位长度。场景先形成毫米 float 工作坐标，再将原生几何转米。`Measurement` 的业务读数以 `unit=mm` 发布。

场景 Record 均含 `schemaVersion=3` 与 `workingUnit=mm`，不嵌入轮廓上下文或内容指纹；下游按 Record 类型读取，不再核对它们与当前轮廓的绑定。`DtsRoot.v3` 另外区分 `legacySortedIndex/activeIndex/slotIndex/sourceIndex`。

`DtsMetalPick.v3` 保存旧 float 毫米 `metalLineMm`，根记录携带同一份基准线；面差只读根记录里的这条线，调用公共 `signedPointLine(...,DeclaredUnit)`，不将米制 float 回算毫米，也不重新归一化旧 float 单位方向。`DtsFlush.v3.footMm` 保存旧毫米 float 垂足，面差有效时汇总从它写出 `perpendicular`。`pointContext/metalContext` 是 `profile.Geometry.v3`，只声明所在坐标帧、分析平面和手性，供公共测量节点组合。

## 宿主、升级与结果

使用 `default-profile.lyflow.json` 或 `adjacent-profile.lyflow.json` 时，宿主将完整 Profile 注入 `profile.input` 的 `profile` 输出。输入保留 NaN 槽、有限无效槽、全部可用通道和原源索引。为历史图重搭输入和几何连线时，先确认数据契约。[DTS 原生图切换](../../docs/dts-profile-migration.md)

两张图发布六个顶层输出 `flush`、`metal`、`root`、`bundle`、`faces` 和 `dome`。`bundle` 含 `ok`、`flushMm`、`root`、`dome`、`pick`、`faces`、`pieces` 和 `metal`，面差有效时还含 `perpendicular`。嵌入坐标使用毫米工作坐标。

合格标准、跨轮廓汇总、跨站位统计、设备采集与生产许可继续属于宿主。`Measurement` 的 verdict 与公差不在本图填写，失败或未运行不会被汇总为合格。

## 与宿主 Python 旧算法的对照

宿主 `dts-check` 的 Python 旧算法在三份现场 PLY 上有逐轮廓基线
（`crates/dts-core/tests/fixtures/measurement_reference/*.csv`）。
拿各样本**头三条**轮廓写成 CSV 跑 `lyflow eval`（`data/lyflow-eval/dts/`，不进本仓库）：

```powershell
lyflow eval solutions\dts\graphs\default-profile.lyflow.json `
  --samples-glob "<...>\data\lyflow-eval\dts\*.csv" --bind n_in.path `
  --set 'n_in.source="file"' --metric outputs.flush
```

2026-09-17，本包默认参数对 Python 各自的参数组：

| 样本                                        | 轮廓 | dts (mm) | Python (mm) | Δ (mm)      |
| ------------------------------------------- | ---- | -------- | ----------- | ----------- |
| A `DP2240-01P车门上方1.ply`                 | 0    | 4.2494   | 4.0378      | **+0.2116** |
| A                                           | 1    | 4.0743   | 4.0363      | +0.0380     |
| A                                           | 2    | 4.0795   | 4.0364      | +0.0431     |
| B `车门上方2.ply`                           | 0    | 3.6315   | 3.7414      | **−0.1099** |
| B                                           | 1    | 3.6294   | 3.7288      | −0.0994     |
| B                                           | 2    | 3.6264   | 3.7360      | −0.1096     |
| D `Image_Profile_20260809_17_22_58_462.ply` | 0    | −4.1787  | 55.4610     | **−59.64**  |
| D                                           | 1    | 55.1099  | 55.4587     | −0.3488     |
| D                                           | 2    | 55.1075  | 7.5183      | +47.59      |

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

`tests/test_dts_ops.cpp` 保留两条 3200 点实测、A/B/D 现场读数、A/B 与原 Python 差值门槛以及空/NaN/暗/无胶条失败路径；新增原生槽与通道/源索引、配套几何、缺字段/跨帧拒绝、八种 float 正号组合、两种基准图的原生轮廓注入、退役诊断和取消回归。数据夹具原样迁移，没有为让 D 样本对齐而改算法。

公共核方法回归在 `kernels/tests/test_profile.cpp`，公共处理节点回归在 `packs/std-profile/tests/test_processing.cpp`。软件图与宿主验证不代替扫描器、机器人、现场质量、吞吐或耐久验收。
