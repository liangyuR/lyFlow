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
  ops/     21 个 gap.* 算子
  algo/    算法源码，从 xyz-gap-inspector 的 src/ 复制而来，命名空间不改
  tools/   图生成器与 A/B 脚本
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
| `gap.business_rois` | alignment → 四个 Box2D | 业务 ROI 按 ICP 变换搬到当前样本上 |
| `gap.fit_line` | cloud, box → line, inliers, innerEnd | 直线拟合 + 靠缝隙一端的截取重拟合 |
| `gap.selected_point` | cloud, box → point | 离 ROI min 角最近的点（取自整片云） |
| `gap.nearest_to_line` | cloud, line → point | 离基准线垂距最小的云点（`ref_type: nearest point`） |
| `gap.fit_gap_circles` | merged, primary, secondary, boxLeft, boxRight → 两个圆 | 圆拟合 + 重试 + 相机分开回退 + 按标称值挑候选 |
| `gap.flush` | baseLine, refPoint → value, segment | 段差 |
| `gap.gap` | left, right, [baseLine] → value, segment | 间隙（definition A / B） |
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
- 模型失败**不回退模板路径**：一张图一条路径，失败就红框（H6）。回退是调度策略，不是算法。

## 脚本

```powershell
# 从一份 StandardGap.yml 生成一张图（加 --model 就是模型路径）
python packs\gap\tools\lyflow_graph_from_config.py `
    <StandardGap.yml> --primary <Master.pcd> --secondary <Slave.pcd> -o graph.lyflow.json

# 39 个样本的 A/B：生成图 → lyflow run → 与基线 results.csv 比对
python packs\gap\tools\lyflow_ab.py `
    --dataset <dataset.yml> --baseline <放 results.csv 的目录> [--model <onnx>]
```

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
