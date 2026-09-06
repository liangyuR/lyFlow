# 通用算法进标准包，gap 领域包搬进 LyFlow

前提：[std-pack-plan.md](std-pack-plan.md) 已落地（core 零依赖，`packs/std-pointcloud/`）。
gap 领域包目前在 `D:\project\xyz-gap-inspector` 的 `lyflow-ops` 分支 `lyflow/` 下，链接该仓库 `build/Release/lib/` 的
`xyz_gap_core.lib` / `xyz_gap_ml.lib` / `xyz_gap_io.lib`；两条 A/B（模板路径、模型路径）39/39，基线在
`%TEMP%\lyflow-gap-baseline` 与 `%TEMP%\lyflow-gap-baseline-model`。

用户已拍的两条：**算法源码搬进 LyFlow**（gap-inspector 暂留自己那份，等接入时删）；**core 新增 `Tensor` 类型**。

## 1. 定死的决定

| # | 决定 | 理由 |
|---|---|---|
| T1 | 新增 `packs/std-ml/`：`Tensor` 相关的通用推理算子，onnxruntime 依赖只在这个包 | 推理引擎是重依赖，不该和点云包绑死 |
| T2 | 通用 2D 几何算子进 `packs/std-pointcloud/`：`fit.line_2d`、`fit.circle_2d`、`register.icp_2d`、`filter.crop_box2d`。算法实现放在包内 `algo/`（`fit2d.*`、`icp2d.*`、`profile_geometry.*`、`crop2d.*`），并经 `lyflow_pcl_support` 之外的第二个 INTERFACE 目标 `lyflow_std_algo` 导出头文件给其他包调用 | gap 包的算子改成调用这些函数，而不是把算法逻辑复制两份 |
| T3 | `Icp2D` 与 `ProfileGeometry` 的源码从 gap-inspector 迁入 `std-pointcloud/algo/`，命名空间改为 `lyflow::std_pc`；`fitLine2D` / `fitCircle2D` 用与 `GapUtils::fitLine`/`fitCircle` **完全相同**的 PCL 调用（`SampleConsensusModelLine` + RANSAC 1000 次、`SACSegmentation` CIRCLE2D 10000 次、`random=false`、`setOptimizeCoefficients`、定半径最小二乘重定圆心） | A/B 逐位一致是硬约束；参数外露但默认值就是原值 |
| T4 | `packs/gap/` 收纳：`ops/`（`gap.*`）、`algo/`（从 gap-inspector 复制的 `src/gap_core`、`src/gap_detection`、`src/gap_ml`、`src/gap_io`、`src/domain/detection`、`src/domain/config` 中被引用的文件，命名空间不改）、`tools/`（图生成器、A/B 脚本）、`tests/`、`README.md`。不再有 `GAP_BUILD_DIR` | LyFlow 拥有算法；复制而非移动，业务仓库现阶段不受影响 |
| T5 | gap 的 `algo/` 里凡是 std 已提供的函数（直线/圆拟合、ICP、盒裁剪），gap 算子改调 std 的；`GapUtils.cpp` 里对应函数删除。保留的只有领域逻辑：双迹线选择、靠缝隙截取、端点/最近点、距离与求值、roll 裁剪窗、掩膜精修、框推导、`MeasurementEngine` 黑盒（供 `gap.measure_reference`） | 「通用进 std」要真的减少 gap 里的代码，不是加一层转发 |
| T6 | ONNX 拆三步：`gap.profile_tensor`（primary, secondary → tensor:Tensor [2,6,1280]）→ `ml.onnx_run`（tensor → tensor [2,8,1280]）→ `gap.labels_from_logits`（tensor → labels:Record）。`ml.onnx_run` 参数：modelPath、inputName、outputName、intraOpThreads（默认 1）；会话按路径 + mtime 缓存 | 推理本身通用；通道构造与 argmax 是领域约定 |
| T7 | `Tensor`：`shape: vector<int64>` + `float32` 数据；`valueJson` 给形状与 min/max/mean；Inspector 显示；不进二进制 IPC | Image 域以后也用它 |
| T8 | 包的启用：`packs/*` 里每个包在 `lyflow_op_pack()` 声明 `DEFAULT ON|OFF`。`std-pointcloud`、`std-ml` 默认 ON；`gap` 默认 OFF，用 `LYFLOW_PACKS=gap`（分号分隔的仓库内包名）打开。`LYFLOW_STD_PACKS=0` 仍表示关掉所有仓库内包 | 领域包不该拖累纯平台开发者；但它在同一仓库 |
| T9 | 第三方：yaml-cpp 装进 `C:\vcpkg`（`vcpkg install yaml-cpp:x64-windows`），gap 包 `find_package(yaml-cpp)`；onnxruntime 走 `LYFLOW_ONNXRUNTIME_ROOT`，缺省 `third_party/onnxruntime/`（gitignore），由 `scripts/fetch-onnxruntime.ps1` 准备：优先从 `D:\project\xyz-gap-inspector\3rdparty\onnxruntime\onnxruntime-win-x64-1.19.2` 复制，否则从 GitHub release 下载同版本。缺失时 CMake FATAL 并打印这条命令 | 不把 10 MB 二进制放进 git；不静默跳过 |
| T10 | 算子 id 沿用类别前缀惯例，不加 `std.`：`fit.line_2d`、`fit.circle_2d`、`register.icp_2d`、`filter.crop_box2d`、`ml.onnx_run`；gap 算子 id 不变（生成的图与 e2e 零改动，除 ONNX 三步） | 与现有 16 个算子一致 |
| T11 | gap-inspector 的 `lyflow-ops` 分支：删除 `lyflow/` 目录，留一份 `lyflow/README.md` 指向新家；tools 一并迁走 | 单一真实来源 |

ADR-0015：algorithms-live-in-lyflow-packs。

## 2. std 新算子

| id | 输入 → 输出 | 参数 | 备注 |
|---|---|---|---|
| `fit.line_2d` | cloud → line:Line2D, inliers:Indices | distThresh(m)、maxIterations=1000、optimize=true、clipTo:Box2D 可选输入（有则 line 带与框的两个交点作端点） | XY 平面；PCL `SampleConsensusModelLine` + `RandomSampleConsensus`，`random=false` |
| `fit.circle_2d` | cloud → circle:Circle2D, inliers | distThresh、rMin、rMax、maxIterations=10000、fixedRadius=0 | `SACSegmentation` CIRCLE2D；fixedRadius>0 时最小二乘重定圆心并重筛内点 |
| `register.icp_2d` | source, target, [init:Transform] → transform:Transform, result:Record{fitness, iterations, converged} | maxMatchingDist、fitnessDist、maxIterations、normalKnn、smoothLength、minDiffRot、minDiffTrans | 原 `Icp2D` 全部选项外露，默认值不变 |
| `filter.crop_box2d` | cloud, box:Box2D → cloud | bounds: open / closed（默认 closed） | open = 四边严格不等（gap 语义）；空结果报 `roi_empty` |
| `ml.onnx_run` | input:Tensor → output:Tensor | modelPath、inputName、outputName、intraOpThreads=1 | 会话缓存；形状不符报 `bad_input` 带 portName |

`lyflow_std_algo` 导出的函数（gap 算子直接调用）：`fitLine2D`、`fitCircle2D`、`fitCircleFixedRadius2D`、`cropBox2D(open)`、`Icp2D` 类、`estimateProfileNormals`。

## 3. gap 包改动

- `gap.fit_line`：双迹线两次拟合、伴线搜索、`optimizeModelCoefficients`、按配置阈值重收内点，全部改调 `fitLine2D`；靖缝隙截取与 `getEndPointofCloud` 保留在 gap。
- `gap.fit_gap_circles`：`fitCircle2D` / `fitCircleFixedRadius2D`；重试与相机回退逻辑保留。
- `gap.align_template`：用 std 的 `Icp2D`；trust region、退化锁定、global/left/right 编排保留。
- `gap.crop_box`：删除，生成器改用 `filter.crop_box2d(bounds=open)`；A/B 图因此结构变化，但算子 id 变化仅此一处与 ONNX 三步。
- `gap.onnx_segment` 删除，换 T6 三步；`gap.roi_from_labels` 不变。
- `gap.measure_reference` 保留（链 `algo/` 里的 `MeasurementEngine`），模型模式用 `algo/gap_ml` 的 `OnnxRoiPredictor`。

## 4. 同步

- `bridge/build.rs`、`scripts/*.ps1`：透传 `LYFLOW_PACKS`、`LYFLOW_ONNXRUNTIME_ROOT`；`check.ps1` 默认跑默认包；新增 `pnpm check:gap`（`LYFLOW_PACKS=gap` 的门禁 + 两条 A/B）。
- `scripts/e2e/gap.mjs`：图路径环境变量不变；ONNX 三步与 `filter.crop_box2d` 的节点 id 由生成器决定，断言按 op 查节点。
- 文档：ADR-0015、`docs/op-packs.md`（仓库内领域包、DEFAULT、`LYFLOW_PACKS`）、`packs/gap/README.md`、`packs/std-ml/README.md`、`packs/std-pointcloud/README.md`、`docs/architecture.md`、`docs/roadmap.md`、`core/README.md`（依赖准备步骤）。
- `.gitignore`：`third_party/onnxruntime/`。

## 5. 验收

- [ ] 默认 `pnpm check`：std-pointcloud 18 个算子 + std-ml 1 个 + core 2 个 = 21；manifest 里原 16 个算子的描述逐字节不变
- [ ] `LYFLOW_STD_PACKS=0`：仍只有 2 个算子
- [ ] `pnpm check:gap`：全绿；两条 A/B 39/39，|Δ| ≤ 0.002 mm（期望仍为 0）
- [ ] std 新算子 doctest：`fit.line_2d`/`fit.circle_2d` 对合成数据的内点数与系数；`register.icp_2d` 平移已知量的恢复；`filter.crop_box2d` 开/闭区间边界点；`ml.onnx_run` 用 v12s0.onnx 跑一个 [2,6,1280] 零张量得到 [2,8,1280]
- [ ] `packs/gap/algo/` 里不再有 `fitLine`、`fitCircle`、`Icp2D`、`filterCloudByRoi` 的实现（grep 为空），gap 算子调用的是 `lyflow_std_algo`
- [ ] 默认与 gap 两种模式 `pnpm e2e` 全绿（gap 模式用重新生成的 R5 模板图与 R1 模型图）
- [ ] gap-inspector `lyflow-ops` 分支 `lyflow/` 只剩指路 README
- [ ] 注释扫描两仓库为 0

## 6. 不做

- gap-inspector 业务仓库接入 LyFlow（下一阶段设计）
- 删除 gap-inspector 里的算法源码
- Image 域
