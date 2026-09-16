# gap — LyFlow 领域算子包：间隙 / 段差测量

版本 0.2.0，**默认关闭**（ADR-0015）。打开它：`$env:LYFLOW_PACKS = "gap"`。

把线扫双头剖面的测量主路径拆成 `gap.*` 算子，每个中间阶段（裁剪、ICP、
分割、有效 ROI、拟合）都能在画布上点开看。两条路径都在：

- **配置 / 模板 + ICP**：`gap_batch_runner run --manifest dataset.yml` 那条；
- **模型 ROI**：现场真正在用的那条，ONNX 逐槽分割 → 四框 → 跟随零件的裁剪窗，无模板无 ICP。

算子包机制本身见 [docs/op-packs.md](../../docs/op-packs.md) 与 ADR-0013 / ADR-0014；
算法为什么住在这里见 [ADR-0015](../../docs/adr/0015-algorithms-live-in-lyflow-packs.md)；
接进去的来龙去脉见 `docs/gap-integration-plan.md` 与 `docs/gap-acceptance.md`。

## 目录

```
packs/gap/
  ops/     26 个 gap.* 算子 + StandardGap.yml 导入器
  algo/    算法源码，从 xyz-gap-inspector 的 src/ 复制而来，命名空间不改
  tools/   图生成器、两条 A/B、回退图 A/B 与导入器等价性脚本
  tests/   随包走的 doctest
```

`algo/` 里**只有领域逻辑**：双迹线截取、端点与最近点、距离与求值、roll 裁剪窗、
掩膜精修、框推导、`MeasurementEngine` 黑盒。直线/圆拟合、2D ICP、盒裁剪
已经进 `packs/std-pointcloud/algo/`，本包经 `lyflow_std_algo` 调它们
（签名适配在 `algo/std_bridge.hpp`，那里一行算法都没有）。

## 构建

```powershell
cd D:\project\LyFlow
C:\vcpkg\vcpkg.exe install yaml-cpp:x64-windows            # 一次就够
powershell -ExecutionPolicy Bypass -File scripts/fetch-onnxruntime.ps1

$env:LYFLOW_PACKS = "gap"
pnpm check          # 或 pnpm core:build / pnpm dev
pnpm check:gap      # 门禁 + 两条 A/B
```

依赖：PCL 与 `lyflow_std_algo` 来自 `packs/std-pointcloud`（本包不自己 `find_package(PCL)`），
onnxruntime 来自 `packs/std-ml` 解析好的 `LYFLOW_ONNXRUNTIME_ROOT`，
yaml-cpp 来自 `C:\vcpkg`。缺哪个 configure 就直接报哪个，并打印补齐的命令。
所以 `LYFLOW_STD_PACKS=0` 的纯平台构建带不动这个包 —— 那是设计。

`yaml-cpp.dll` 由 vcpkg 的 applocal 带进 core 的 `bin/`，
`onnxruntime*.dll` 由 `packs/std-ml` 拷进去；`bridge/build.rs` 整目录搬走那个 `bin/`，
所以 `pnpm tauri build` 打出来的安装包里也有它们（`pnpm e2e:packaged` 有断言盯着）。

## 算子

| id | 输入 → 输出 | 干什么 |
|---|---|---|
| `gap.load_profile_pair` | → primary, secondary | 读一个测点目录下的 Master/Slave 两片 PCD |
| `gap.to_measurement_frame` | cloud → cloud | 传感器 XZ 帧 → 测量 XY 帧（换轴，是反射不是旋转） |
| `gap.overall_roi` | primary, secondary → box | 整体 ROI，支持 auto_center |
| `gap.load_template` | → left, right | 读一对模板 PCD |
| `gap.align_template` | cloud, tplLeft, tplRight → alignment | 全局粗配 + 左右两侧 ICP + 信赖域 + 退化锁定 |
| `gap.select_alignment` | a,[b],[c],[d] → alignment | 按 `min(l,r)` ↓、`mean` ↓、配置顺序 ↑、id ↑ 选模板 |
| `gap.result_bundle` | gap, flush, 五个框, 三份 quality, cropStatus, alignment, fallback, 三片云 → bundle | 汇成一个 `GapResultBundle`，字段对齐旧 `QualityMetrics` |
| `gap.business_rois` | alignment → 四个 Box2D | 业务 ROI 按 ICP 变换搬到当前样本上 |
| `gap.fit_line` | cloud, box → line, inliers, innerEnd | 直线拟合 + 靠缝隙一端的截取重拟合 |
| `gap.selected_point` | cloud, box → point | 离 ROI min 角最近的点（取自整片云） |
| `gap.nearest_to_line` | cloud, line → point | 离基准线垂距最小的云点（`ref_type: nearest point`） |
| `gap.fit_gap_circles` | merged, primary, secondary, boxLeft, boxRight → 两个圆 | 圆拟合 + 重试 + 相机分开回退 + 按标称值挑候选 |
| `gap.flush` | baseLine, refPoint → value, segment | 段差 |
| `gap.gap` | left, right, [baseLine] → value, segment | 间隙（definition A / B） |
| `gap.corner_vertex` | lineLeft, lineRight, [baseLine], [alignment] → vertex, gap, flush, angle | 软装夹角：两翼面直线求交，顶点相对金件顶点沿基准面分解 |
| `gap.groove_joint` | primary, secondary → gap, flush, 四条线, groove, 三份 quality | 软装对接缝：最深点定槽心，两侧面逐相机拟合再平均，槽宽在低面下方 `gapDepth` 处量 |
| `gap.point_offset` | a, b → dx, dy, distance, segment | 两点分量：b 相对 a 的位移沿测量帧 x/y 两轴分解，面板边/翻边平台这类横向缝用 |
| `gap.notch_width` | primary, secondary → gap, flush, baseLine, refLine, gapSegment, flushSegment, anchor, 三份 quality | V 缝开口：两圆边贴合、缝底无槽时，基准翼面线下 `levelDepth` 处切基准侧圆边得 A，过 A 的水平刀切对面立边得 B，A→B 水平距离；逐相机，被遮挡的相机用 `camera` 排除 |
| `gap.camera_guard` | primary, secondary, box? → primary, secondary, quality | 双相机闸：比两台在**重叠段**上的高度差中位（不需要 ROI，所以站得到模型前面），超限时按 `onDisagree` 把不可信那台挡掉 —— 模型的张量是 [2,…] 一台一行，所以「只用一台」是把它填进两个端口。玻璃二次反射会让模型 ROI 和合并云一起被带偏，而拟合残差照样很小，只能这样查 |
| `gap.judge` | value → value | 公差判定 |
| `gap.measure_reference` | primary, secondary → gap, flush | 黑盒对照：直接调 `MeasurementEngine::measure`；`useModel` 开了就先跑一次模型 ROI |

裁剪用标准算子 `filter.crop_box2d`（`bounds: open` 就是四边严格开区间），
原来的 `gap.crop_box` 已删除；直线与圆的拟合内核也已经是 `lyflow_std_algo` 的。

模型 ROI 路径另加六个：

| id | 输入 → 输出 | 干什么 |
|---|---|---|
| `gap.profile_tensor` | primary, secondary → tensor | 两片**原始 1280 槽**剖面 → `[2, 6, 1280]` 模型输入张量 |
| `gap.labels_from_logits` | tensor → labels | `[2, 类别数, 1280]` 的 logits 逐槽 argmax |
| `gap.roi_from_labels` | primary, secondary, labels, [backdrop] → 四个 Box2D + refinements + backdrop | 标签 → 四个业务 ROI（带掩膜精修 refine-v1）；`backdrop` 原样透传，给四框叠一片同帧底图 |
| `gap.labels_to_cloud` | cloud, labels → cloud | 按类上色，只为了在 3D 视图里看分割结果；接**测量帧**的云 |
| `gap.drop_non_finite` | cloud → cloud | 剔除无效槽 —— 模型路径的 load 必须保留它们，所以单独一步 |
| `gap.roll_anchored_crop` | primary, secondary, gapLeft, gapRight → 两片云, window, status | 跟随零件的整体裁剪窗，五种失效保护 + 点数回退 |

中间那一步推理是**通用算子** `ml.onnx_run`（`packs/std-ml`）：
`gap.profile_tensor` → `ml.onnx_run` → `gap.labels_from_logits`。
通道构造与 argmax 是领域约定，推理本身不是（ADR-0015）。

**单位**：算子参数一律**毫米**，与 `StandardGap.yml` 一模一样；端口上流动的坐标一律是**米**，
与点云同单位。换算在算子内部做（LyFlow 的 G4）。

**复刻而不是修正**：ROI 只变换对角两角点仍按轴对齐解释、flush 恒为 `|d| + offset`、
合并顺序 secondary 在前、盒裁剪四边严格开区间 —— 这些都照原样保留。
正确性标准是与 `gap_batch_runner` 的基线逐值一致，不是「更合理」。

## 模型 ROI 路径

现场跑的就是这条。推理在测量**之前**：拿两片原始 1280 槽的传感器帧剖面（NaN 槽必须保留）
逐槽分类，四段（`left_surface` / `left_roll` / `right_roll` / `right_surface`）各推一个框，
写成四个业务 ROI；之后**不做整体 ROI 裁剪、不做 ICP、不用模板**，改由
`gap.roll_anchored_crop` 用两个 roll 框重新框住零件。之后与模板路径完全相同。

```powershell
# 生成一张模型路径的图
python packs\gap\tools\lyflow_graph_from_config.py `
    <StandardGap.yml> --primary <Master.pcd> --secondary <Slave.pcd> `
    --model C:\...\v12s0.onnx -o graph.lyflow.json

# 39 个样本的模型 A/B（基线是 gap_batch_runner --roi-model 那一份）
python packs\gap\tools\lyflow_ab.py --dataset <dataset.yml> `
    --baseline "$env:TEMP\lyflow-gap-baseline-model" --model C:\...\v12s0.onnx
```

几条要记住的：

- **`gap.load_profile_pair` 的 `dropNonFinite` 必须关掉**：模型输入是原始 1280 槽，
  点数不是 1280 就直接报 `bad_input`。剔无效槽的活交给 `gap.drop_non_finite`，排在推理之后。
- `ml.onnx_run` 按「模型路径 + mtime + 线程数」缓存会话，一次运行不会重载 10 MB 的模型；
  `intraOpThreads` 默认 1（inter 也是 1），归约顺序固定，同输入逐位可复现。
- 框在 `gap.roi_from_labels` 里就换成**测量帧的米**（`x→x, z→y, /1000`），
  下游的裁剪与叠画一行都不用改。
- `missing_segments` 非空 → `model_roi_failed`，消息里点名缺了哪一段；
  诊断（`refinements`）照样出来，图上点开那个节点看得到。
- **四框与着色剖面共用测量帧底图**：`gap.labels_to_cloud` 接
  `gap.to_measurement_frame` 的输出（换轴只换轴不删点，槽位与标签仍一一对应，
  必须排在 `gap.drop_non_finite` **之前**；槽数不是 1280 直接报 `bad_input`），
  它的云再接进 `gap.roi_from_labels.backdrop`（可选输入，同一个 shared_ptr 透传到同名输出）。
  这样选中四框那个节点时，框叠的是自己那片按类着色的测量帧剖面 ——
  不接的话 LyFlow 的底图规则会往上游借一片**传感器帧**的云（y≡0），
  2D 剖面俯视 XY 时它退化成一条线，框飘在别处，两者对不上。
- 模型失败在**算子层面**不回退模板路径：一张图一条路径，失败就红框（H6）。
  回退是调度策略不是算法 —— 它由导入器生成的 `flow.fallback` 节点表达（见下一节）。

## 导入器：StandardGap.yml → 图

`lyflow import` 直接把一份 `StandardGap.yml` 转成图，不必装 Python
（A1-7，实现在 `ops/import_standard_gap.cpp`，是 `tools/lyflow_graph_from_config.py`
的 `build()` 的逐字移植）。三个 kind 共用同一份实现：

| kind | 走哪条路径 |
|---|---|
| `StandardGap.yml` | auto：看 `setting.yml` 的 `model_roi.enabled` |
| `StandardGap.yml:template` | 强制模板 / ICP 路径 |
| `StandardGap.yml:model` | 强制模型 ROI 路径 |

```powershell
lyflow import <StandardGap.yml> --kind StandardGap.yml -o graph.lyflow.json
```

导入器只拿得到 (文本, baseDir) 两样东西，所以模式与路径都从**约定**推导：

- **auto 的推导**：在 `baseDir` 与它的父目录里找 `setting.yml`，读顶层
  `model_roi.enabled`。为真走模型路径并带 `flow.fallback` 的模板备用闭包；
  为假或**找不到 `setting.yml`** 一律退回模板路径。
- **ONNX 路径**：`setting.yml` 的 `model_roi.model_path`；它没有就找 `baseDir` 下
  **唯一**的 `*.onnx`；都没有报 `bad_param`。
- **点云**：`gap.load_profile_pair.dir = "."`（相对图文件目录解析），即 baseDir 就是测点目录。
- **模板目录 / 配置路径**：`"StandardGap"` 与 `"StandardGap.yml"`，相对 baseDir ——
  kind 的名字就是那个文件名，与 Python 版的 `<配置目录>/<配置主名>` 同一条规则。
- Python 版 `raise SystemExit(...)` 的每一处，这里是
  `Status::Error(Validate, bad_input|bad_param, 同一句中文)`，经 `lyflow import` 出成诊断数组。

三种 kind 产出的图都声明 `outputs: {gap, flush, bundle}`。

### 带 `flow.fallback` 的完整图

`model_roi.enabled` 为真时，模型路径与模板路径同在一张图里，十二个 `flow.fallback`
选择（`ref_type` 不是 `line end` 时十一个）。备用闭包的节点 id 一律带 `b_` 前缀，
且**只**经 fallback 的惰性 `b` 端口流出 —— 主路径成功时它们一个 compute 都不跑。

回退发生在**两层**上，因为两条路径不是处处同参：

- **ROI 与云**这一层：四个业务框、合并云、两片裁剪云、整体窗，八个 fallback；
- **段差的拟合结果**这一层：`gap.fit_line` 的 `endpoints` 在两条路径上不同
  （模型是 `inlier_ends`，模板是 `roi_intersection`，见 §3 与生成器里同一行），
  所以段差那一段的裁剪与拟合在备用闭包里**各有一份**，fallback 挪到拟合结果上。
  只有一套 fit 的话，真回退时基准线会用模型路径的端点语义，
  `definition: A` 的间隙跟着错 —— 实测 R4_2 差 0.0087 mm，容差是 0.002 mm。

间隙那一段不用分身：`gap.fit_gap_circles` 的参数在两条路径上一模一样，
它接的三片云与两个框已经在 fallback 后面了。

```
n_load ─┬─ n_frame_p/s ─ n_drop_p/s ─┬─ n_roll ─ n_merge ─ n_filter        （模型，a 路）
        │                            │            └─ n_crop_flushBase/Ref ─ n_fit_base/ref
        └─ n_tensor ─ n_infer ─ n_seg ─ n_rois

b_n_load ─ b_n_frame_p/s ─ b_n_overall ─ b_n_crop_p/s ─ b_n_merge ─ b_n_filter （惰性，b 路）
                                          ├─ b_n_align_* ─ b_n_select ─ b_n_rois
                                          └─ b_n_crop_flushBase/Ref ─ b_n_fit_base/ref

a = 模型侧                       b = 模板侧                    →  下游只认 fallback 的 out
n_fb_flushBase     n_rois.flushBase          b_n_rois.flushBase
n_fb_flushRef      n_rois.flushRef           b_n_rois.flushRef
n_fb_gapLeft       n_rois.gapLeft            b_n_rois.gapLeft
n_fb_gapRight      n_rois.gapRight           b_n_rois.gapRight
n_fb_merged        n_filter.cloud            b_n_filter.cloud
n_fb_crop_p        n_roll.primary            b_n_crop_p.cloud
n_fb_crop_s        n_roll.secondary          b_n_crop_s.cloud
n_fb_overall       n_roll.window             b_n_overall.box
n_fb_line          n_fit_base.line           b_n_fit_base.line
n_fb_ref_point     参考点端口                 备用侧同名端口
n_fb_quality_base  n_fit_base.quality        b_n_fit_base.quality
n_fb_quality_ref   n_fit_ref.quality         b_n_fit_ref.quality
```

`n_fb_line.out` 接 `gap.flush.baseLine`；`n_fb_ref_point.out` 接 `gap.flush.refPoint`
（`ref_type: line end` 时两侧都取 `gap.fit_line.innerEnd`，另两种取 `point`）；
两个 quality 接 `gap.result_bundle` 的 `fitBase` / `fitRef`。`n_fb_quality_ref`
只在 `ref_type: line end` 时存在。`gap.gap` 的 `baseLine` 仍取 `gap.flush` 那一份
（并进垂足之后的线段），与另外两种模式一致。

主路径的 `n_crop_flushBase` / `n_crop_flushRef` / `n_fit_base` 因此**直接接模型侧**的
`n_filter` 与 `n_rois`，不再接 fallback 的 `out` —— 接了的话模型失败时它们照样算得出来，
`n_fb_line` 永远选 a，回退就是个摆设。

`n_fb_flushBase.choice` 接进 `gap.result_bundle.fallback`，回退与否与原因因此进 bundle。
`n_bundle` 不直接接任何 `b_` 节点 —— 接了那条闭包就不再是惰性的。

## 脚本

```powershell
# 从一份 StandardGap.yml 生成一张图（加 --model 就是模型路径）
python packs\gap\tools\lyflow_graph_from_config.py `
    <StandardGap.yml> --primary <Master.pcd> --secondary <Slave.pcd> -o graph.lyflow.json

# 39 个样本的 A/B：生成图 → lyflow run → 与基线 results.csv 比对
python packs\gap\tools\lyflow_ab.py `
    --dataset <dataset.yml> --baseline <放 results.csv 的目录> [--model <onnx>]
```

```powershell
# 逐节点等价：C++ 导入器 vs Python 生成器（六种组合，全部一致退出 0）
python packs\gap\tools\compare_importer.py [--dataset <dataset.yml>]

# 回退图的 A/B：lyflow import 产回退图 -> lyflow run -> 与模型基线比 gap/flush
python packs\gap\tools\ab_fallback.py `
    --dataset <dataset.yml> --baseline "$env:TEMP\lyflow-gap-baseline-model"

# 把 n_infer.modelPath 指到不存在的文件，逼出回退分支，与**模板**基线比
python packs\gap\tools\ab_fallback.py `
    --dataset <dataset.yml> --baseline "$env:TEMP\lyflow-gap-baseline" `
    --break-model --only <sample_id>
```

`compare_importer.py` 比节点集合（id + op + 参数值）与边集合，忽略 ui 坐标与 meta；
路径参数两边形态不同是预期的（Python 写绝对路径，导入器写相对路径），比之前统一规范化成绝对路径。
数据集里的配置目录没有 `setting.yml`，所以模型与回退两种形态用一份暂存夹具造出来 ——
auto 的推导规则因此与 Python 生成器完全无关地被单独测到。

`ab_fallback.py` 跑的是**导入器产的回退图**（`lyflow_ab.py` 跑的是 Python 生成器的图）。
它同样为每份配置暂存一份带 `model_roi.enabled: true` 的夹具，点云不进夹具 ——
`n_load` 与 `b_n_load` 两个读盘节点都用 `--set` 覆盖成 `source=files` 加两个绝对路径。
它顺带统计**回退真的触发了**的样本（`n_fb_flushBase.choice == "b"` 或出现 `plan_extended`），
与批测器基线 `diagnostics.jsonl` 里 `fallback_reason` 非空的那一份比对。
`--break-model` 把 `n_infer.modelPath` 指到一个不存在的文件，主路径必失败，
这时基线要换成**模板基线** —— 那是唯一能验到备用闭包数值的跑法。

`lyflow_ab.py` 全部一致时退出 0，否则 1 并列出不一致的样本。
它默认挑 `bridge/target/{release,debug}` 里**最新**的那个 `lyflow.exe` ——
按固定顺序挑会在 release 留着旧产物时静默地把整份 A/B 跑成「算子不存在」。
它还会顺手对一次数据目录的 `manifest.csv`（现场值，两位小数，容差 0.006 mm），
只报不判 —— 那是另一套口径，用来回答「这条路径是不是现场跑的那条」。

两条 A/B 都在 `pnpm check:gap` 里，数据与基线的位置用
`LYFLOW_GAP_DATASET` / `LYFLOW_GAP_MODEL` / `LYFLOW_GAP_BASELINE` /
`LYFLOW_GAP_BASELINE_MODEL` 覆盖；数据集不在就跳过 A/B 只跑门禁。

## 不做

region growing 分割、强度门限、圆补偿（`compensation.enabled`）、
`2-points line` / `circle tangent` 两种几何类型、模型失败回退模板路径。
