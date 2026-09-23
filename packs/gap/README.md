# gap — LyFlow 领域算子包：间隙 / 段差测量

版本 0.2.0，**默认关闭**（ADR-0015）。打开它：`$env:LYFLOW_PACKS = "gap"`。

把线扫双头剖面的测量主路径拆成 `gap.*` 算子，每个中间阶段（裁剪、ICP、
分割、有效 ROI、拟合）都能在画布上点开看。两条路径都在：

- **配置 / 模板 + ICP**：`gap_batch_runner run --manifest dataset.yml` 那条；
- **模型 ROI**：现场真正在用的那条，ONNX 逐槽分割 → 四框 → 跟随零件的裁剪窗，无模板无 ICP。

算子包机制本身见 [docs/op-packs.md](../../docs/op-packs.md) 与 ADR-0013 / ADR-0014；
算法为什么住在这里见 [ADR-0015](../../docs/adr/0015-algorithms-live-in-lyflow-packs.md)。
算子的正确性由本包 doctest 与样本集上的读数评审来定，**不以与 `gap_batch_runner` 基线数值相同为标准**。

## 目录

```
packs/gap/
  ops/     26 个 gap.* 算子 + StandardGap.yml 导入器
  algo/    算法源码，从 xyz-gap-inspector 的 src/ 复制而来，命名空间不改
  tools/   历史对拍工具：Python 图生成器、两条 A/B、回退图 A/B 与导入器等价性脚本
  tests/   随包走的 doctest
  snippets/ 随包的片段（*.lyflow-snippet.json，M8b）
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
pnpm check:gap      # LYFLOW_PACKS=gap 下的门禁
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
| `gap.datum_window` | anchor, heightAnchor? → box | 由锚框推出一条长窗，用来在**旁边那张长面**上拟方向基准线。x 锚在缝的框上（模型对缝定位最稳），y 锚在基准面框上 |
| `gap.load_template` | → left, right | 读一对模板 PCD |
| `gap.align_template` | cloud, tplLeft, tplRight → alignment | 全局粗配 + 左右两侧 ICP + 信赖域 + 退化锁定 |
| `gap.select_alignment` | a,[b],[c],[d] → alignment | 按 `min(l,r)` ↓、`mean` ↓、配置顺序 ↑、id ↑ 选模板 |
| `gap.result_bundle` | gap, flush, rois (RoiSet), scan (ScanPair), roiOverall?, 三份 quality, cropStatus?, alignment?, fallback → bundle | 汇成一个 `GapResultBundle`，字段对齐旧 `QualityMetrics`。四个框与 roi_source 取自 `rois`，整体框 / 对齐结果 / 裁剪状态不接端口时取 `rois.info`，点数取自 `scan`（定位之后的那一对云） |
| `gap.business_rois` | alignment → 四个 Box2D, seam | 业务 ROI 按 ICP 变换搬到当前样本上：只搬框中心，宽高保持配置里的原值。`datumSide` 决定 base ROI 用哪一侧的变换，默认 `auto` 由模板坐标系里的框推出（M8a）；`seam` 是两个缝框中心连线的中点，接 `gap.fit_line.toward` |
| `gap.fit_line` | cloud, box, toward, refLine? → line, inliers, innerEnd | 直线拟合 + 靠 `toward` 一端的截取重拟合（`toward` 接缝那一侧的 ROI）。`dirMode` 可把方向锚到 `refLine` 上（`band` 只兜底，`fixed` 一律钉死、只拟法向偏移）—— ROI 只有两三毫米宽时它自己拟出来的方向基本是噪声，还会偶尔整条歪几十度而残差很小。`minInliers` 是唯一拦得住「ROI 跑偏、照样拟出一条没意义的线」的地方 |
| `gap.selected_point` | cloud, box → point | 离 ROI min 角最近的点（取自整片云） |
| `gap.nearest_to_line` | cloud, line → point | 离基准线垂距最小的云点（`ref_type: nearest point`） |
| `gap.fit_gap_circles` | merged, primary, secondary, boxLeft, boxRight, refLine? → 两个圆 | 圆拟合 + 重试 + 相机分开回退 + 按标称值挑候选。另有两个**逐侧**的收紧手段：`leftCamera`/`rightCamera` 把某一侧钉到单台相机（两台锁在不同界面上时，合并云里是相距一两毫米的两层点），`centerAbove`/`centerTol` 要求圆心落在 `refLine` 上方的一条窄带里（夹胶玻璃：玻璃面不成像，圆心该在 flush 线上方）|
| `gap.flush` | baseLine, refPoint → value, segment | 段差，默认带符号（`signed` 默认 true） |
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
| `gap.roi_from_labels` | primary, secondary, labels, [backdrop] → 四个 Box2D + seam + refinements + backdrop | 标签 → 四个业务 ROI（带掩膜精修 refine-v1）；`seam` 同 `gap.business_rois`；`backdrop` 原样透传，给四框叠一片同帧底图 |
| `gap.labels_to_cloud` | cloud, labels → cloud | 按类上色，只为了在 3D 视图里看分割结果；接**测量帧**的云 |
| `gap.drop_non_finite` | cloud → cloud | 剔除无效槽 —— 模型路径的 load 必须保留它们，所以单独一步 |
| `gap.roll_anchored_crop` | primary, secondary, gapLeft, gapRight, [merged] → 三片云, window, status | 跟随零件的整体裁剪窗，五种失效保护 + 点数回退；接了 `merged`（合并并去噪之后的云）就跟着同一个窗一起裁 |

中间那一步推理是**通用算子** `ml.onnx_run`（`packs/std-ml`）：
`gap.profile_tensor` → `ml.onnx_run` → `gap.labels_from_logits`。
通道构造与 argmax 是领域约定，推理本身不是（ADR-0015）。

**单位**：算子参数一律**毫米**，与 `StandardGap.yml` 一模一样；端口上流动的坐标一律是**米**，
与点云同单位。换算在算子内部做。

### M7 起的行为

这几条与 `gap_batch_runner` 的原算法有意不同，读数会跟着变：

- **`gap.business_rois`**：参数 `datumSide`（`left` / `right`，默认 `left`）——
  基准件在缝的哪一侧。决定 base ROI 用哪一侧的 ICP 变换；**不改变哪个框是 `flushBase`**。
  ROI 变换只把框中心按 ICP 变换搬过去，宽高保持配置里的原值，仍是轴对齐框 ——
  人调好的 ROI 尺寸不随转角撑大或压扁。
- **`gap.roi_from_labels`**：没有 `baseSide` 参数。四个框按标签语义直接出到同名端口。
- **`gap.fit_line`**：没有 `side` 参数，改为必接输入 `toward: Box2D`（缝那一侧的 ROI，用其中心）。
  `innerEnd` 是内点中沿直线方向投影最靠近 `toward` 中心的那一个；截取按同一投影保留最靠近
  `toward` 的 `segmentPoints` 个内点，与点序无关。quality 带 `segmentApplied: bool`，
  没截取时为 false；`segmentPoints` 不小于内点数（多半不是本意）时另发一条 warn 日志，
  `segmentPoints: 0` 表示有意不截（方向基准线就是这样），不发。
- **`gap.fit_gap_circles`**：`leftRadiusFixed` / `rightRadiusFixed` 在所有路径上都生效，
  包括圆心高度带约束的拟合与相机分开的回退。
- **`gap.flush`**：`signed` 默认 true，输出带符号垂距：参考点在基准线**上方**（测量帧里 y 更小）
  为正，基准线竖直时在右侧（x 更大）为正。符号只看参考点在哪一侧，与拟合给出的方向正反无关
  （本包出端口的 `Line2D.dir` 一律朝 +x、竖直时朝 +y，`gap.flush` 自己也再统一一次）。
  要绝对值显式写 `signed: false`。

只依赖参数与连接关系的检查（例如 `fit_line` 的 `dirMode` 不是 `free` 却没接 `refLine`、
`fit_gap_circles` 配了 `*CenterTol` 却没接对应参考线、半径上下限倒置）在加载期的 `validate`
里报 `bad_param`，图根本跑不起来；依赖数据的约束看 quality 字段与 warn 日志。
合并顺序 secondary 在前、`filter.crop_box2d` 的 `bounds: open` 仍是现状。

## 积木算子与 Bundle（M8a）

人在空白画布上拼一个测点，用的是七个**积木算子**（`间隙/积木` 分类）。每个对应一个人会说出口的
步骤，复杂度消化在两种 Bundle 与算子本身里；图始终是平的，每个节点都看得见、改得动。

| id | 输入 → 输出 | 干什么 |
|---|---|---|
| `gap.read_scan` | [primary, secondary] → scan | 读剖面：目录 + 前缀（或两个文件），换到测量帧，合并（secondary 在前）并按需半径去噪。primary / secondary 保留原始 1280 槽（NaN 槽还在）。**两个输入接上或被宿主注入时直接用它们、不读目录**（M8b / L18）；`source=inputs` 是「不配目录」的写法 |
| `gap.locate_template` | scan → rois, alignment, scan | 模板定位：整体框 → 裁 → 模板 → ICP → 选模板 → 业务框。四个角色框写在模板坐标系里；四个固定模板槽，每槽可覆盖四框 |
| `gap.locate_model` | scan → rois, scan | 模型定位：剖面张量 → ONNX → argmax → 推框（精修）→ 剔 NaN → 跟随裁剪窗（合并云跟着裁） |
| `gap.role_line` | scan, rois, [refLine] → line, innerEnd, quality | 按角色（datum / target）拟合直线，「靠缝那一端」取两个缝框中心的中点 |
| `gap.ref_point` | scan, rois, [baseLine] → point, line, quality | 取参考点：`line_end` / `selected_point` / `nearest_point` |
| `gap.seam_circles` | scan, rois, [refLine], [refLineRight] → left, right, quality | 拟合缝两侧圆（`gap.fit_gap_circles` 的全部参数，常用的六个露在外面） |
| `gap.datum_direction` | scan, rois → line, quality | 方向基准：角色框背离缝的那一侧推长窗、在长面上拟线，接 `role_line.refLine` |

两种 Bundle（manifest 的 `bundles` 段）：

- **`gap.ScanPair`** = `primary`、`secondary`、`merged`（PointCloud，测量帧）。
- **`gap.RoiSet`** = `datum`（段差基准面）、`target`（参考面）、`seamLeft`、`seamRight`（缝两侧）
  + `info`（`GapRoiInfo`：`datumSide`、`source: template|model`、`alignment`、`overallMm`、`cropStatus`）。
  四个框按**角色**命名，不再有 base / ref。

**方向全部由 RoiSet 推出**（m8-plan L7）：积木算子上一个 `side` / `toward` / `baseSide` / `datumSide`
都没有。基准件在哪一侧 = datum 框中心在两个缝框中心连线中点的哪一边（模板定位在模板坐标系里推，
模型定位在样本上推）；「靠缝那一端」= 两个缝框中心的中点。`locate_template` 的加载期校验拦住：
框退化、缝框左右颠倒或重叠、datum 与 target 落在缝的同一侧、各模板槽的基准件不同侧。

**积木算子不写第二份算法**（L6）：每一步都用 `Step` 原样调一个细粒度算子的 compute —— 包内的
在 `ops/gap_fine.h` 的 `namespace fine` 里导出，别的包的（`filter.crop_box2d`、`util.merge`、
`filter.radius_outlier`、`ml.onnx_run`）取注册表里那个函数指针。参数与细粒度算子**同名同义**，
声明也从那边拷（`paramOf`）。细粒度算子全部保留，两种可以在一张图里混用：细粒度链末端用
`gap.make_scan_pair` / `gap.make_roi_set` 收成 Bundle，积木链中途用 `gap.split_scan_pair` /
`gap.split_roi_set` 拆成散线（后者另出 `seam`）。

M8a 的两处行为变化（两种图一致）：

- **去噪挪到合并云上、裁剪之前**：以前是「整体框裁 → 合并 → 去噪」，现在是「合并 → 去噪 → 裁」
  （`gap.read_scan` 的 `removeOutliers`）。框边上的点不再因为被裁掉邻居而被当成离群点。
- **基准件在哪一侧按框的几何推**，`flush.base_side` 不再参与；配置与几何不符时导入器在
  `meta.importNotes` 里记一笔（天幕 L5 / L6：base 框在缝右侧而配置写 left）。

## 在编辑器里拼一个测点（M8b）

- **片段**（`snippets/*.lyflow-snippet.json`，构建时编进包、经 manifest 的 `snippets` 段给出，
  格式见 [schema/snippet.schema.json](../../schema/snippet.schema.json)，机制见
  [docs/op-packs.md](../../docs/op-packs.md)「随包的片段」）：

  | id | 名字 | 内容 | 对外输入 |
  |---|---|---|---|
  | `gap.measure_skeleton` | 测点骨架 | 基准线 + 线端点参考点 + 段差、缝两侧圆 + 间隙、两个判定、结果汇总（8 个节点） | 各节点的 `scan` / `rois`（含结果汇总的两个可选输入） |
  | `gap.flush_line_end` | 段差 · 线端点 | role_line(datum) + ref_point(line_end) + flush + judge | `scan` / `rois` |
  | `gap.flush_selected_point` | 段差 · 选点 | 同上，ref_point(selected_point) | `scan` / `rois` |
  | `gap.gap_circles` | 间隙 · 圆 | seam_circles + gap + judge | `scan` / `rois` |
  | `gap.locate_model_template_fallback` | 模型定位 + 模板回退 | locate_model ∥ locate_template，rois 与 scan 各一个 `flow.fallback` | 两个定位的 `scan` |
  | `gap.backup_camera` | 备用相机回退 | 两个 read_scan（主 / 备用相机）+ 一个 `flow.fallback` | 无 |

  典型拼法：拖入 `gap.read_scan`、`gap.locate_template`（scan 自动接上），插入「测点骨架」
  （八个对外输入自动接到 locate_template 那唯一的一对输出上），填目录、拖四个框、跑。
- **2D 拖框**：`gap.locate_template` 的四个角色框（以及各槽的覆盖框）带 `semantic: "roi"` 与
  `roiBackdrop`（那个槽的左右模板），选中它、把视图切到「2D 剖面」就能直接拖；`gap.overall_roi.roi`
  也带 roi 标记（数据坐标系）。`locate_template.overallRoi` 的默认值是一个大到不裁的框：空白画布上
  拼出来的图不填高级参数也能跑（导入器总是显式写这一项）。

## 模型 ROI 路径

现场跑的就是这条。推理在测量**之前**：拿两片原始 1280 槽的传感器帧剖面（NaN 槽必须保留）
逐槽分类，四段（`left_surface` / `left_roll` / `right_roll` / `right_surface`）各推一个框，
写成四个业务 ROI；之后**不做整体 ROI 裁剪、不做 ICP、不用模板**，改由
`gap.roll_anchored_crop` 用两个 roll 框重新框住零件。之后与模板路径完全相同。

```powershell
# 生成一张模型路径的图（ONNX 路径按「导入器」一节的约定推导）
lyflow import <StandardGap.yml> --kind StandardGap.yml:model -o graph.lyflow.json

# 历史对拍：39 个样本对 gap_batch_runner --roi-model 基线（行为已有意偏离，不一致是预期的）
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
- 模型失败在**算子层面**不回退模板路径：一张图一条路径，失败就红框。
  回退是调度策略不是算法 —— 它由导入器生成的 `flow.fallback` 节点表达（见下一节）。

## 导入器：StandardGap.yml → 图

`lyflow import` 直接把一份 `StandardGap.yml` 转成图，不必装 Python
（A1-7，实现在 `ops/import_standard_gap.cpp`）。`tools/lyflow_graph_from_config.py` 只为历史对拍保留，
不跟着算子参数改动同步，生成图一律用导入器。**默认产出积木图**（M8a，m8-plan L12）；
`--fine`（等价于 kind 后面加 `:fine`）产出细粒度图，每一步一个节点。六个 kind 共用同一份实现：

| kind | 走哪条路径 |
|---|---|
| `StandardGap.yml` / `StandardGap.yml:fine` | auto：看 `setting.yml` 的 `model_roi.enabled` |
| `StandardGap.yml:template` / `…:template:fine` | 强制模板 / ICP 路径 |
| `StandardGap.yml:model` / `…:model:fine` | 强制模型 ROI 路径 |

```powershell
lyflow import <StandardGap.yml> --kind StandardGap.yml -o graph.lyflow.json          # 积木图
lyflow import <StandardGap.yml> --kind StandardGap.yml --fine -o fine.lyflow.json    # 细粒度图
```

模板路径的积木图是 10 个节点：`n_scan`（read_scan）→ `n_locate`（locate_template）→ `n_line`
（role_line）/ `n_ref_point`（ref_point）→ `n_flush`，`n_circles`（seam_circles）→ `n_gap`，两个判定、
`n_bundle`；配了方向基准多一个 `n_datum`（写不成积木的锚配置退回细粒度的「窗 → 裁 → 拟」，经
`gap.split_*` 混用）；配了双相机闸，读剖面前面是 `n_load` → `n_camera_guard`、`n_scan` 用
`source=inputs`。模型 + 回退是 `n_model` 与 `b_n_locate` 并联，`n_fb_rois` / `n_fb_scan` 两个
fallback，段差那一支与细粒度图一样整条备一份、在拟合结果上回退。两种图都**不再生成**黑盒对照
`gap.measure_reference`；四框全 0 的模板候选（没配框）两种图都跳过并记进 `meta.importNotes`。

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

导入器按 M7 的算子参数写图：

- 细粒度图的每个 `gap.fit_line` 都接 `toward`：基准线与参考线接业务框节点的 **`seam`**
  （两个缝框中心的中点，与 `gap.role_line` 同一个判据），方向基准线（`n_fit_datum`）接它的锚框。
- `gap.business_rois` 不写 `datumSide`（默认 `auto`，由框推出）。
- 顶层图参数（见 [graph-doc.md](../../docs/graph-doc.md)「顶层图参数」）：
  `gapOffset` 绑定 `n_gap.offset` 与 `n_circles.offset`（备用分支只备份到基准线/参考点，
  间隙与圆拟合节点只有这一份）；模型模式另有 `modelPath`，binds 逐个列出每个用到模型的节点
  —— 积木图是 `n_model`（`gap.locate_model`），细粒度图是 `n_infer`（`ml.onnx_run`）——
  不用按算子类型的通配。宿主用 `--param gapOffset=0.1` 或 C ABI 的 `params_json` 传值。

### `flush.base_direction`：方向锚到旁边那张长面上

基准面是一道很窄的台肩时，它自己拟出来的方向基本是噪声 —— 而且会偶尔整条歪几十度，
**残差反而很小，任何质量指标都看不出来**（天幕 L4 的两帧：内点率 0.500 / 0.577 正好落在
正常中位 0.533 上，rms 0.030 也在正常范围里，只有方向是 +26° 和 −21°）。
台肩外面那张长面通常有十几毫米、两百个点，方向稳得多，两张面之间的相对倾角是零件的
固有量，量一次定下来就行。

不写这一段就是不启用，`n_fit_base` 上一个方向相关的参数都不生成，取算子默认的 `free`。

```yaml
flush:
  base_direction:
    datum: long_plane      # off（默认）/ long_plane
    side: left             # 长面在基准面的哪一侧（背离缝的那一侧）
    start_mm: 0.6          # 窗口离缝的 ROI 那条边的让开量
    length_mm: 13.4
    height_mm: 2.5         # 以基准面 ROI 的 y 范围为中心上下各撑开
    anchor: gap_left       # x 锚挂哪个缝框；默认按 base_side 推
    height_anchor: flush_base   # y 锚挂哪个段差框
    fit_distance: 0.35     # 长面拟合的内点距离
    min_inliers: 30        # 少于它就报失败，别拿没意义的线当基准
    mode: fixed            # fixed（一律钉死）/ band（只在出界时钉）
    nominal_deg: 2.75      # 基准面相对长面的标称倾角
    tolerance_deg: 12.0    # 只有 band 用
```

导入器由此生成三个节点：`n_datum_box`（`gap.datum_window`）→ `n_crop_datum` →
`n_fit_datum`（`gap.fit_line`，`toward` 接锚框），把 `line` 接到 `n_fit_base.refLine`；模板备用分支
有自己的一份（`b_` 前缀）。

**x 锚在缝的 ROI 上，不是基准面 ROI 上。** 一开始两个都锚在基准面上，结果有几帧
基准面框整个跑偏十来毫米，窗口跟着飞到没点的地方，拟出一条没意义的线还被当成基准 ——
比不加约束更糟。所以既分了锚，也配了 `min_inliers`。两个锚都能显式指定：基准面落在
长边上时（天幕 R4/R5），要防的那条线在缝的另一侧，`anchor` 和 `height_anchor` 得一起换边。

**挂之前先量「谁更稳」，别想当然。** 判据是两条线各自的**绝对**角度散布（相对夹角会把
两条线的噪声混在一起，看不出是谁在歪）。天幕四个点实测：

| | 基准面 ROI 宽 | 基准线内点 | **基准线绝对角 std** | 长面绝对角 std | 该不该锚 |
|---|---:|---:|---:|---:|---|
| L4 | 2.10 mm | 20 | 1.12° | — | **该**（已锚） |
| L5 | 2.09 mm | 28 | 1.43° | **0.94°** | **该** |
| R4 | 14.49 mm | 113 | **0.18°** | 1.23° | **不该** |
| R5 | 10.73 mm | 182 | 0.82° | — | 不该 |

R4 试过了：锚过去等于把 std 0.18° 的线钉到 std 1.23° 的线上，实测有一帧
flush 从 1.403 变成 2.732、gap 从 4.488 变成算不出 —— **已撤回**。

`nominal_deg` **必须量**，填 0 等于假设两张面平行。L4 的定法：扫一遍偏置取 flush 散布
最小的那个，再看均值有没有贴着标称；一半帧定、另一半验证得到 +2.95° 与 +3.20°，
取 +2.75°（std 最小值附近，均值正好落在标称 1.0 上）。332 帧上
std 0.468 → **0.284**、超差 2 → **0**、一帧不丢；`definition A` 的 u 也取自这条线，
gap 跟着 0.648 → 0.637（超差 7 不变）。

### `gap:` 下的几个非原版键

原版 `StandardGap.yml` 没有这几个键，是这边为难点位加的；**不写就是不启用**，
导入器不生成对应参数，算子取默认值。

| 键 | 作用 |
|---|---|
| `gap.left_circle_camera` / `gap.right_circle_camera` | `Both`（默认）/ `Primary` / `Secondary`。把这一侧的圆钉到单台相机。两台锁在不同界面上时（夹胶玻璃：一台看表面、一台看夹胶层），合并云里是相距一两毫米的**两层点**，拟出来的圆没有意义。钉死之后这一侧不再走相机分开的回退 |
| `gap.center_band.reference` | `flush_ref`（默认，段差的参考线）或 `flush_base`（基准线）。`flush_ref` 要求 `flush.ref_type` 是 `line end`，否则导入报错 |
| `gap.center_band.left_above` / `right_above` | 圆心应当高出那条参考线多少毫米 |
| `gap.center_band.left_tolerance` / `right_tolerance` | 容差，**<= 0 就是不启用**。启用时只接受圆心落在这条窄带里的候选 |
| `gap.weak_fit.left_min_arc_deg` / `right_min_arc_deg` | 这一侧内点覆盖的**圆弧角度**低于它就算没拟出来。0（默认）= 不检查。**钉了单相机时先退回合并云重拟一次**，两边都弱才算失败 |
| `gap.weak_fit.left_min_inliers` / `right_min_inliers` | 同上，按内点数。比弧长钝：合并云重拟不一定凑得够内点，天幕 L4 上用它会掉帧，用弧长不会 |
| `gap.center_band.left_mode` / `right_mode` | `always`（默认，一律走带约束的拟合）或 `guard`（先按原样拟，只有圆心落到带外才重来）。**「本来就拟得对、只是想上个保险」的点位该用 guard** —— 带内的帧不重拟，读数与不挂带时相同 |

窄带是在 RANSAC 里**筛候选**，不是把圆心焊到那个高度；带比真实散布还窄时合格候选
被筛光，那一侧直接拟不出。先量一批正常帧的圆心高度再定带宽。

**窄带管不了「弧太短」。** 圆拟合本身 3 个内点就算成功，而短弧上三个参数的圆本来就定
不住 —— 圆心能跑到点云外面去，而残差和内点率照样正常，窄带也可能照样满足。天幕 L4 有
一帧右圆只吃到 **22.5° 的弧、6 个内点**，圆心落在 ROI 左边界外 0.44 mm，读数 2.173
（公差 [2.50, 5.50]）。332 帧里第二短的弧是 67.9°，p1 是 70.0° —— **弧长把它单独分了出来**，
而内点率 0.194 与 rms 0.012 都看不出异常。配 `weak_fit.right_min_arc_deg: 50` 之后，
那一帧退回合并云重拟（Slave 那台在这一帧上是两条乱缠的轨迹，Master 是干净的），
读数 3.269，**全库只有这一个格子变了**。

天幕 L4 是这两个键的来由：右侧 ROI 只有 2.29 mm 宽，玻璃表面不成像、成的是夹胶层，
圆心本该在 flush 线**上方**。332 帧上 `right_above: 0.23 / right_tolerance: 0.45`
把 std 0.667 → 0.639、超差 9 → 7 且一帧不丢；再叠 `right_circle_camera: Secondary`
是 0.648 / 7（单钉 Secondary 不加窄带会丢 8 帧）。

### 带 `flow.fallback` 的完整图

> 这一节讲的是 `--fine` 的细粒度图。M8a 起：备用闭包不再自己读一遍文件（`b_n_load` / `b_n_frame_*`
> 没了），与模型那一支共用剔过 NaN 的两片云与去噪之后的合并云；`n_fb_overall` 换成了
> `n_fb_roi_set` / `n_fb_scan_set`（整体窗随 RoiSet 的 info 走），`n_fb_roi_set.choice` 接
> `gap.result_bundle.fallback`。积木图的回退见上面「导入器」一段。

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

## 历史对拍工具（`tools/`）

这些脚本是把算法从 gap-inspector 迁进来时用的对拍工具。**行为已有意偏离基线**
（见上文「M7 起的行为」），出现不一致是预期的；它们**不再是验收门槛**，
只在想看「偏离落在哪些样本上、偏了多少」时手动跑。

```powershell
# Python 版图生成器（不跟算子参数改动同步；生成图请用 lyflow import）
python packs\gap\tools\lyflow_graph_from_config.py `
    <StandardGap.yml> --primary <Master.pcd> --secondary <Slave.pcd> -o graph.lyflow.json

# 39 个样本的 A/B：生成图 → lyflow run → 与基线 results.csv 比对
python packs\gap\tools\lyflow_ab.py `
    --dataset <dataset.yml> --baseline <放 results.csv 的目录> [--model <onnx>]

# C++ 导入器 vs Python 生成器的逐节点对比
python packs\gap\tools\compare_importer.py [--dataset <dataset.yml>]

# 回退图的 A/B：lyflow import 产回退图 -> lyflow run -> 与模型基线比 gap/flush
python packs\gap\tools\ab_fallback.py `
    --dataset <dataset.yml> --baseline "$env:TEMP\lyflow-gap-baseline-model"

# 把 n_infer.modelPath 指到不存在的文件，逼出回退分支，与**模板**基线比
python packs\gap\tools\ab_fallback.py `
    --dataset <dataset.yml> --baseline "$env:TEMP\lyflow-gap-baseline" `
    --break-model --only <sample_id>
```

- `compare_importer.py` 比节点集合（id + op + 参数值）与边集合，忽略 ui 坐标与 meta。
  导入器按 M7 的参数写图而 Python 生成器没有，两边不再逐节点相同。
- `ab_fallback.py` 跑的是**导入器产的回退图**，为每份配置暂存一份带
  `model_roi.enabled: true` 的夹具，`n_load` / `b_n_load` 用 `--set` 覆盖成 `source=files`
  加两个绝对路径；它顺带统计回退真的触发了的样本（`n_fb_flushBase.choice == "b"`
  或出现 `plan_extended`）。`--break-model` 是唯一能跑到备用闭包数值的跑法，这时基线要换成模板基线。
- `lyflow_ab.py` 默认挑 `bridge/target/{release,debug}` 里**最新**的那个 `lyflow.exe`，
  并顺手对一次数据目录的 `manifest.csv`（现场值，两位小数），只报不判。
- 数据与基线的位置用 `LYFLOW_GAP_DATASET` / `LYFLOW_GAP_MODEL` / `LYFLOW_GAP_BASELINE` /
  `LYFLOW_GAP_BASELINE_MODEL` 给。

## 不做

region growing 分割、强度门限、圆补偿（`compensation.enabled`）、
`2-points line` / `circle tangent` 两种几何类型、模型失败回退模板路径。
