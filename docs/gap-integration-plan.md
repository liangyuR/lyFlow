# 接入 xyz-gap-inspector：线扫双头点云的间隙/段差测量

> **这份文档记的是当时的状态。** 算子包后来搬进了 LyFlow 的 `packs/gap/`，
> `xyz-gap-inspector/lyflow/` 只剩一份指路 README，`LYFLOW_OP_PACKS` 换成了
> `LYFLOW_PACKS=gap`，脚本在 `packs/gap/tools/`。见 ADR-0015 与
> [gap-pack-migration-acceptance.md](gap-pack-migration-acceptance.md)。


目标：把 `D:\project\xyz-gap-inspector` 的「配置/模板 + ICP」测量主路径拆成 LyFlow 算子，
输入是一个测点目录下的 Master/Slave 两片 PCD，输出 gap / flush 数值与判定；
每个中间阶段（裁剪、ICP、有效 ROI、拟合）都能在画布上点开看。

## 0. 调研结论（实现者不必重复调研）

- 数据：`C:\Users\11601\OneDrive\Documents\DTS\tianmu_0904` 是一份异常回放包。`pointclouds/KUN10/<serial>_<time>/device_0/<point>/` 下每个测点两个 PCD：
  `LaserProfile_L0_Master_<point>_*.pcd`（primary）与 `LaserProfile_R1_Slave_<point>_*.pcd`（secondary）。
  PCD 字段 `x y z rgb`，1280 槽、无效槽为 NaN、y≡0（传感器 XZ 帧，单位米）、`rgb` 三通道相同 = 强度 0–255。
- 逐点参数在 `database/KUN10/device_0/<point>/StandardGap.yml`，模板在同目录 `StandardGap/f1_left.pcd` 等（已是测量 XY 帧，z=0，left/right 内容相同）。
- 现场结果用的是 ONNX 模型 ROI 路径，模型文件不在包里，**无法复现现场值**。可复现的基线是
  `gap_batch_runner run --manifest dataset.yml`（配置/模板路径），已跑出 39 个样本的 `results.csv` 与
  `diagnostics.jsonl`（含 effective_roi、fits、icp、point_counts），放在 `%TEMP%\lyflow-gap-baseline\`。
  **本计划的正确性标准就是与这份基线逐值一致**（gap/flush |Δ| ≤ 0.002 mm，与 `docs/offline_batch.md` 的容差一致）。
- 我们的两簇（HXMK2A12XTA237802，R1、R5）基线：R1 flush 2.3737、gap 失败（左圆拟合失败）；R5 flush 3.8751、gap 6.4138。
- 工具链完全一致：两仓库都用 `C:\vcpkg`、x64-windows 动态三元组、PCL 1.15.1、Boost 1.92、Eigen 5.0.1、MSVC 14.51、/MD。
  `xyz_gap_core.lib`（36 MB 静态库）与 `xyz_gap_io.lib`（含 yaml-cpp 解析）在 `build/Release/lib/`，可直接链进 LyFlow 的 core DLL。
  头文件：`build/Release/installed/include/{gap_core,gap_io,detection}` 已安装；`src/gap_detection/*.hpp`（GapUtils、Converter、Alignment、GapDetection、DetectionConfigurationAdapter）**未安装**，要把仓库 `src/` 加进 include 路径。
- 主路径的精确语义已由源码调研确认（`GapDetection.cpp:155-758`），关键点在 §3。
- LyFlow 已能读入这对 PCD（1280 槽保留、rgb 进 `rgb` 通道），`util.merge` 可用。

## 1. 定死的决定

| # | 决定 | 理由 |
|---|---|---|
| G1 | 领域算子作为**算子包**放在 `D:\project\xyz-gap-inspector\lyflow\`（新分支 `lyflow-ops`），LyFlow 的 CMake 新增 `LYFLOW_OP_PACKS`（分号分隔的目录列表），每个包提供 `lyflow_op_pack.cmake` 把源文件、include、链接库追加到 `lyflow_core`，并导出 `registerPackOps(Registry&)` | 领域代码归领域仓库；LyFlow 保持通用；编进同一个 DLL 不需要运行时插件 ABI |
| G2 | 直接链接 `xyz_gap_core.lib` 复用 `detection::utils::*`、`conv::swapCloudAxis`、`gap::core::Icp2D`、`gap::core::MeasurementEngine`；**不**复制算法源码 | 逐位一致的唯一可靠方式；PCL 版本一致所以可行 |
| G3 | LyFlow 新增通用 Data 类型：`Box2D`、`Line2D`（含可选两端点）、`Circle2D`、`Point2D`、`Measurement`（值 + ok + 消息 + 判定），以及 `Record`（带类型标签的 JSON，供算子包定义领域结构如 `GapAlignment`） | 2D 几何是通用的；`Record` 让包不用改 core 就能加类型 |
| G4 | 算子参数用**毫米**（与 StandardGap.yml 一致），算子内部换算成米；LyFlow 内置算子仍用米 | 用户对着 YAML 调参不用心算 |
| G5 | 提供 `gap.measure_reference` 黑盒算子（`MeasurementEngine::measure`）与拆分算子并存 | 同一张图里 A/B，逐节点定位差异 |
| G6 | 图由脚本从 StandardGap.yml **生成**：`xyz-gap-inspector/tools/lyflow_graph_from_config.py <StandardGap.yml> <point_dir> -o graph.lyflow.json` | 60 多个参数手填必错；生成后再在 LyFlow 里改 |
| G7 | 3D 视图为选中节点叠画 `Box2D/Line2D/Circle2D/Point2D` 输出（细线、与端口同色），并加正交俯视 XY 的「2D 剖面」相机模式 | 不看见 ROI 框和拟合线，这个接入就没有意义 |
| G8 | 复刻**行为**而不是「修正」它：ROI 只变换对角两角点仍按轴对齐解释；flush 恒为 `|d| + offset`；合并顺序 secondary 在前；`filterCloudByRoi` 四边严格开区间；`random=false` 的 PCL SAC | 正确性标准是与基线一致 |

## 2. 算子清单（包前缀 `gap.`）

| id | 输入 → 输出 | 复用 | 备注 |
|---|---|---|---|
| `gap.load_profile_pair` | (dir 参数) → primary, secondary | PCL 读 | 按 `LaserProfile_L0_Master_` / `_R1_Slave_` 前缀配对；`dropNonFinite` 默认 true（对应 `NonFinitePointPolicy::kRemove`） |
| `gap.to_measurement_frame` | cloud → cloud | `conv::swapCloudAxis(kY,kZ)` | 反射，不是旋转 |
| `gap.overall_roi` | primary, secondary → box:Box2D | `robustCloudCenter` 逻辑 | 参数 roi[4] mm、mode fixed/auto_center、usingCamera；auto_center = 两云各自 (median_x, median_y) 的中点，保留配置宽高 |
| `gap.crop_box` | cloud, box → cloud | `utils::filterCloudByRoi` | 严格开区间；空结果报 `roi_empty` |
| `filter.radius_outlier`（LyFlow 内置） | cloud → cloud | — | 参数是米；生成器换算。作用于合并后的云 |
| `gap.load_template` | (dir, left, right 参数) → left, right | PCL 读 | 模板已在 XY 帧 |
| `gap.align_template` | cloud, tplLeft, tplRight → alignment:GapAlignment(Record) | `computeInitialPose`、`Icp2D`、trust region、退化锁定 | 复刻 `evaluate_pair`（global → left/right，side 初始位姿、trust region 钳制、`registerCloud2DICPOutcome` 的退化锁定、bidirection 仅在 fitness < minScore 时尝试）。参数：ICP 五项、minScore、bidirection、trustRegion、degenerateRatio、templateId、rois（四框 mm）。`cloudsIdentical2D` 时 right 复用 left |
| `gap.select_alignment` | a, [b], [c] → alignment | `selectBestTemplateByIcp` | 可选输入最多 3 个候选；全部低于 minScore → error `icp_score_low` |
| `gap.business_rois` | alignment → flushBase, gapLeft, flushRef, gapRight : Box2D | — | `base_side` 互换后，索引 0/1 用 tLeft、2/3 用 tRight 变换两角点 |
| `gap.fit_line` | cloud, box → line:Line2D, inliers:Indices, innerEnd:Point2D | `utils::fitLine`（4 参）、`getEndPointofCloud`、`getIntersectionRL` | 参数 distThresh mm、segmentPoints、side left/right；`segment_points` 截取靠缝隙一端后用 `dist/3` 二次拟合；`line` 带与 box 的两个交点作端点 |
| `gap.selected_point` | cloud, box → point:Point2D | `pointCloudDistance` | 取离 `box.min` 角最近的**整片云**上的点（不裁 ROI） |
| `gap.fit_gap_circles` | merged, primary, secondary, boxLeft, boxRight → left:Circle2D, right:Circle2D, leftInliers, rightInliers | `utils::fitCircle` | 先合并云拟合；失败按 retryDistance 重试；再 camera-separated 回退（8 内点门限、closest-nominal 的 2×2 组合选择）。参数两侧 rMin/rMax/fixed、distThresh、retryDistance、fallback、selectClosestNominal、nominal、offset |
| `gap.flush` | baseLine, refPoint → value:Measurement, segment:Line2D | `pointLineDistance` | `|d|·1000 + offset`，法线朝 −y |
| `gap.gap` | left, right, [baseLine] → value:Measurement, segment:Line2D | `circleCircleDistance` | definition A 需要 baseLine 的两端点得到 `u`（强制 +x）；切线交叉 → error；`start.x > end.x` → NaN |
| `gap.judge` | value → value | — | nominal / upper / lower / margin / patrol，输出 Measurement 的判定字段 |
| `gap.measure_reference` | primary, secondary → gap, flush : Measurement；日志里带 quality JSON | `MeasurementEngine::measure` | 参数 configPath、templateDir（写进 `configuration.common.save_template_path`）；`save_template`/`save_result` 强制关 |

生成器产出的图：load_profile_pair → to_measurement_frame ×2 → overall_roi → crop_box ×2 → util.merge(secondary, primary) → radius_outlier → load_template ×N → align_template ×N → select_alignment → business_rois → crop_box ×4 → fit_line(flush_base) / fit_line 或 selected_point(flush_ref) / fit_gap_circles → flush → gap → judge ×2；旁路一条 measure_reference 做对照。

## 3. 必须复刻的细节

1. `seg_mode: ROI`：不分割，`left/right_cloud_` 都是 `secondary + primary` 的同一份合并云；RadiusOutlierRemoval 作用其上。
2. `roll_anchored_crop` 在此路径**永不生效**（`skipped:no_model_roi`），不要实现。
3. ICP：`Icp2DOptions{max_matching_distance=mm/1000, fitness_distance=mm/1000, max_iteration_count, normal_knn=num_neighbor}`，其余默认；score = fitness × 100；`bidirectional` 记录的是「是否真的用了反向」。
4. 模板选择：过滤 left、right 都 ≥ minScore；键 `min(l,r)` ↓、`mean` ↓、配置顺序 ↑、id ↑。
5. `gap.fit_line` 的截取方向：`ascend = cloud[0].x <= cloud.back().x`；`ascend == isLeft` 保留索引尾部，否则头部；总是保留靠缝隙一端。
6. `line end` = 内点索引排序后取内侧端点的**真实云点**；`selected point` = 离 ROI `col(0)` 最近的整片云上的点。
7. flush：`pointLineDistance` 法线强制朝 −y；最终 `fabs(‖end−start‖·1000) + offset`，符号是死代码。
8. gap A：`u` 来自基准线与 flush_base 框的两个交点，`u.x<0` 则取反；`gap = (c2−c1)·u − r1 − r2`，`< 0` 报错。
9. camera-separated：`select_closest_nominal` 开启时强制自由半径；候选有效性 `isfinite && start.x <= end.x`；按 `|gap − nominal|` 最小选，平局取内点多。
10. 确定性：PCL SAC 都是 `random=false`；同输入同二进制逐位相同。

## 4. LyFlow 侧改动

- CMake：`LYFLOW_OP_PACKS`；`scripts/build-core.ps1`、`bridge/build.rs` 透传该变量（环境变量 `LYFLOW_OP_PACKS` 缺省空）；`core-watch.ps1` 同时监视包目录。
- Data：G3 的六种类型；`lyflow_output_info` 对非点云输出返回可读 JSON（几何参数、Measurement 值）；`lyflow_output_cloud` 不变。
- 前端：Inspector 显示非点云输出的值；3D 视图叠画几何（G7）与 2D 剖面相机；类型表加颜色。
- CLI：`lyflow run` 的 JSON Lines 里 `node_state.stats.outputs[]` 对 Measurement 带 `value`（A/B 脚本靠它）。
- 文档：`docs/op-packs.md`（如何写一个包）、ADR-0013 op-packs-static。

## 5. 验收（可机器断言）

- [ ] `pnpm check` 全绿（LyFlow 不带包时行为不变；带包时 manifest 自检干净，schema 校验过）
- [ ] core doctest：新类型 JSON 往返；`gap.crop_box` 开区间；`gap.fit_line` 截取方向四种组合
- [ ] **A/B 脚本** `xyz-gap-inspector/tools/lyflow_ab.py`：对 `dataset.yml` 全部 39 个样本生成图、`lyflow run`、与 `%TEMP%\lyflow-gap-baseline\results.csv` 比对：
  成功/失败状态一致，成功样本 gap、flush |Δ| ≤ 0.002 mm；同时 `gap.measure_reference` 与拆分算子之差 ≤ 0.002 mm
- [ ] R1、R5 两个测点的图在 LyFlow 桌面端打开、运行，3D 视图能看到有效 ROI 框、基准线、圆；R1 的 gap 节点红框指向 `gap_left` 圆拟合失败
- [ ] CDP：叠画几何、2D 剖面模式、Inspector 数值显示各至少一条断言
- [ ] 热重载：改包里一个算子 label，app 5 s 内换代

## 6. 明确不做

- ONNX 模型 ROI 路径（模型不在手上）；`roll_anchored_crop`；region growing 分割；强度门限；圆补偿（`compensation.enabled`）；`2-points line`、`circle tangent`、`nearest point` 类型
- 改进算法。所有已知缺口（`docs/roi_measurement_pipeline.md`）原样保留，先做到一致再谈优化

---

# 第二部分：模型 ROI 路径（现场真正在用的那条）

模型文件已到手：`C:\Users\11601\OneDrive\Documents\DTS\models\v12s0.onnx`。
用它跑 `gap_batch_runner run --manifest dataset.yml --roi-model <onnx>`，39 个样本里 41 个数值有 40 个与现场值
（`manifest.csv`，两位小数）在 0.006 mm 内一致；唯一例外 R4_16 与 R4_17 是同一测点相隔 50 秒的两次测量，
现场 CSV 两行都记的是第二次的值，所以不是算法差异。**这条路径的基线在 `%TEMP%\lyflow-gap-baseline-model\`**。

## 7. 模型路径的事实（源码已核对）

- 推理在 `MeasurementEngine::measure` **之前**，输入是**原始 1280 槽**的两片云（NaN 槽保留；`profileRowsFromSensorXzCloud` 取 `x*1000, z*1000, r, isFinite`），点数不是 1280 直接报错（`BatchRunner.cpp:627-657`）。
- `OnnxRoiPredictor::predict(primary, secondary, MaskRefineOptions{})` → `RoiBoxesResult{rois(四框，绝对传感器 mm，[x_lo,z_lo,x_hi,z_hi]), missing_segments, refinements}`；有 `missing_segments` 即失败 `model_roi_failed`。
- 四框写进 `configuration.roi_override`，`roi_override_source="model"`。之后 `GapDetection::run()`：
  - `override_short_circuit = true` → **不做 overall ROI 裁剪、不做 ICP、不用模板**；
  - `roll_anchored_crop.enabled` 时用 `computeRollAnchoredCropMm(gap_left, gap_right, cfg)` 得到跟随零件的窗（`box_mm` 或 `skip_reason`），`preprocess` 用它裁两片相机云；裁后点数 < `min_points_kept`（Both 模式看两片之和）则**回退无界框**并标 `reverted:min_points`（`Alignment.cpp:330-400`）；
  - 之后与模板路径完全相同：`secondary + primary` 合并、RadiusOutlierRemoval、四个业务 ROI 直接取 override（`base_side` 互换仍生效）、裁点、拟合、求值。
- 库：`build/Release/lib/xyz_gap_ml.lib`，依赖 `3rdparty/onnxruntime/onnxruntime-win-x64-1.19.2/{include,lib}`，运行时 `onnxruntime.dll` + `onnxruntime_providers_shared.dll`（在 `build/Release/bin/`）。头文件 `src/gap_ml/{OnnxRoiPredictor,RoiBoxes,RoiFeatures,SensorXzProfile}.hpp`。
- R1_10 模型基线：gap 3.7838、flush 2.3504；R5_11：gap 6.4685、flush 3.5173。

## 8. 定死的决定

| # | 决定 | 理由 |
|---|---|---|
| H1 | 模型路径算子进**同一个算子包**，链 `xyz_gap_ml.lib` + onnxruntime；`onnxruntime*.dll` 由包的 cmake 拷进 core `bin/`（与 yaml-cpp 同法） | 一份包、一份构建入口 |
| H2 | `OnnxRoiPredictor` 是长驻资源：算子内按「模型路径 + 文件 mtime」缓存实例，`predict` 内部已加锁 | 每次运行重新加载 10 MB 模型不可接受；live preview 会连续触发 |
| H3 | 模型输出的框在算子里就转成**测量帧、米**的 `Box2D`（`x→x, z→y, /1000`），与第一部分的 Box2D 同一约定 | 下游 `gap.crop_box` / 叠画零改动 |
| H4 | 复刻 `roll_anchored_crop` 的全部失效保护（`skip_reason` 五种、`min_points_kept` 回退无界框），状态以 `Record` 输出 | 与基线一致的前提；这也是现场最值得看的诊断 |
| H5 | 生成器加 `--model <onnx>`：产出模型路径的图（无模板、无 ICP）；A/B 脚本加 `--model`，对 `lyflow-gap-baseline-model` 比对 | 两条路径都能一键出图、一键对拍 |
| H6 | 批测器的「模型失败回退模板路径」**不在图里做** | 一张图一条路径，失败就红框；回退是调度策略不是算法 |

## 9. 新增算子

| id | 输入 → 输出 | 复用 | 备注 |
|---|---|---|---|
| `gap.onnx_segment` | primary, secondary（原始 1280 槽、传感器帧）→ labels:Record | `profileRowsFromSensorXzCloud`、`OnnxRoiPredictor::predictLabels` | 参数 modelPath；`Record{type:"GapLabels", row0[1280], row1[1280]}`；点数≠1280 报 `bad_input` 带 portName |
| `gap.roi_from_labels` | primary, secondary, labels → flushBase, gapLeft, flushRef, gapRight:Box2D, refinements:Record | `boxesFromRefinedLabels` | 参数 refine 开关与五个数值（`MaskRefineOptions`）；`missing_segments` 非空 → error `model_roi_failed`，消息列出缺的段 |
| `gap.labels_to_cloud` | cloud, labels → cloud | — | 按类别给 rgb 上色（8 类固定色表），只为了在 3D 视图里看分割结果 |
| `gap.drop_non_finite` | cloud → cloud | — | 对应 `NonFinitePointPolicy::kRemove`；模型路径里 load 必须保留 NaN，所以单独一步 |
| `gap.roll_anchored_crop` | primary, secondary（测量帧）, gapLeft, gapRight → primary, secondary, window:Box2D, status:Record | `computeRollAnchoredCropMm`、`filterCloudByRoi` | 参数 halfWidth、halfHeight、maxRollBoxHeight、minPointsKept、usingCamera；status 含 `applied/reverted/rejected:<reason>` 与前后点数；disabled 时原样透传 |
| `gap.measure_reference` | 加参数 modelPath（可空） | `OnnxRoiPredictor` + `MeasurementEngine` | 非空时复刻 `applyModelRoi` 再 measure |

生成器 `--model` 产出的图：`load_profile_pair(dropNonFinite=false)` → `onnx_segment` → `roi_from_labels` → `labels_to_cloud`（旁路，只为看）；
`to_measurement_frame ×2` → `drop_non_finite ×2` → `roll_anchored_crop` → `util.merge(secondary, primary)` → `filter.radius_outlier` →
`crop_box ×4` → 第一部分的拟合与求值算子 → `judge`；旁路 `measure_reference(modelPath)`。

## 10. 验收

- [ ] 带包 / 不带包 `pnpm check` 全绿；`onnxruntime*.dll` 随 core `bin/` 走，`pnpm e2e:packaged` 的干净目录里也有
- [ ] `tools/lyflow_ab.py --model <onnx> --baseline %TEMP%\lyflow-gap-baseline-model`：39/39 状态一致，gap/flush |Δ| ≤ 0.002 mm；同时对 `manifest.csv` 现场值 40/41 在 0.006 mm 内（R4_16 例外并注明原因）
- [ ] R1、R5 的模型路径图在桌面端运行：R1 gap 3.7838 / flush 2.3504，R5 gap 6.4685 / flush 3.5173；选中 `roi_from_labels` 看到四框叠在剖面上，选中 `labels_to_cloud` 看到按类着色的剖面，选中 `roll_anchored_crop` 看到窗与 status
- [ ] CDP：模型路径图的上述三个节点各一条断言（`LYFLOW_GAP_GRAPH_MODEL` 指向 R1 模型图；未设时跳过）
- [ ] 第一部分的 39/39 模板路径 A/B 仍然通过（回归）
