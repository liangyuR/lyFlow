# gap 逐算子迁移清单

状态：实施中；当前 39 项。原251/7规划基线通过机器清单baselineReference追溯。返回[清单总览](../operator-package-audit.md)。

下列顺序是迁移/验收顺序，不是执行图调度顺序。精确端口和参数见[机器清单](../operator-package-audit.json)。

## 074 · gap.fit_line

- 当前：`gap` / `1.1.0` / 二维剖面直线拟合。目标：`std-profile`；拆公共能力/适配；**M4**。
- 源码：[packs/gap/ops/fit.cpp:1232](../../packs/gap/ops/fit.cpp#L1232)。
- 端口：`cloud:PointCloud`, `box:Box2D`, `toward:Box2D`, `refLine:Line2D?` → `line:Line2D`, `inliers:Indices`, `innerEnd:Point2D`, `quality:Record`。
- 参数单位标注：distThresh=mm, dirNominalDeg=°, dirTolDeg=°。
- 依赖：当前直接：PCL RANSAC/Eigen，经 lyflow_std_algo；含领域端点/方向约束；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：轮廓基础模块不引PCL/OpenCV/ONNX；明确标注的原PCL拟合方法作为可选后端隔离。
- 当前契约：输入 cloud:PointCloud, box:Box2D, toward:Box2D, refLine:Line2D? → 输出 line:Line2D, inliers:Indices, innerEnd:Point2D, quality:Record。米制云/Box2D/toward/refLine→Line2D/Indices/innerEnd/quality；参数mm；先拟合后取靠缝端内点再以1/3阈值重拟合。
- 目标契约：轮廓受约束线提取属于 std-profile；PCL RANSAC 保留可选后端，朝缝默认由 solution-gap 适配；Indices必须绑定具体裁后云与源列映射。
- 迁移前置：M1；M2；M3
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：minInliers、free/band/fixed方向、端点模式、segmentApplied、RMS/点序和输出内点索引基线。
- 说明：新轻量包默认不能强绑PCL；保留方法作为可选能力并声明依赖，算法替换另开任务。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 075 · gap.fit_gap_circles

- 当前：`gap` / `1.4.0` / 间隙双侧圆拟合。目标：`solution-gap`；拆公共能力/适配；**M4**。
- 源码：[packs/gap/ops/fit.cpp:1375](../../packs/gap/ops/fit.cpp#L1375)。
- 端口：`merged:PointCloud`, `primary:PointCloud`, `secondary:PointCloud`, `boxLeft:Box2D`, `boxRight:Box2D`, `refLine:Line2D?`, `refLineRight:Line2D?` → `left:Circle2D`, `right:Circle2D`, `leftCloud:PointCloud`, `rightCloud:PointCloud`, `leftInliers:Indices`, `rightInliers:Indices`, `quality:Record`。
- 参数单位标注：distThresh=mm, retryDistance=mm, nominal=mm, offset=mm, leftRadiusValue=mm, leftRadiusMin=mm, leftRadiusMax=mm, leftRadiusPrior=mm, leftRadiusPriorSigma=mm, rightRadiusValue=mm, rightRadiusMin=mm, rightRadiusMax=mm, rightRadiusPrior=mm, rightRadiusPriorSigma=mm, leftCenterAbove=mm, leftCenterTol=mm, rightCenterAbove=mm, rightCenterTol=mm, leftTangentAbove=mm, leftTangentTol=mm, rightTangentAbove=mm, rightTangentTol=mm, leftArcBearingDeg=°, leftArcBearingTolDeg=°, rightArcBearingDeg=°, rightArcBearingTolDeg=°, leftMinArcDeg=°, rightMinArcDeg=°。
- 依赖：当前直接：PCL RANSAC/Eigen/受约束LM；双相机回退/按标称间隙挑候选；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 merged:PointCloud, primary:PointCloud, secondary:PointCloud, boxLeft:Box2D, boxRight:Box2D, refLine:Line2D?, refLineRight:Line2D? → 输出 left:Circle2D, right:Circle2D, leftCloud:PointCloud, rightCloud:PointCloud, leftInliers:Indices, rightInliers:Indices, quality:Record。双头/合并米制云、左右ROI与可选参考线→圆/云/内点/quality；参数mm；含重试、固定半径、软先验、相切带和角域门。
- 目标契约：单圆受约束拟合内核下沉 std-profile 可选PCL后端；双侧/双相机回退和nominal候选规则留场景原生编排。
- 迁移前置：M1；M2；M3；gap.fit_line
- 关联算子：gap.fit_line
- 验收：所有回退分支、固定半径/软先验、LM边界钳制、相切带、ArcBearing/MinArc与Indices归属回归。
- 说明：不能替换为一个默认 fit.circle_2d；业务quality字段与算法报告要分层。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 076 · gap.flush

- 当前：`gap` / `1.1.0` / 面差测量。目标：`std-metrology`；拆公共能力/适配；**M4**。
- 源码：[packs/gap/ops/measure.cpp:300](../../packs/gap/ops/measure.cpp#L300)。
- 端口：`baseLine:Line2D`, `refPoint:Point2D` → `value:Measurement`, `segment:Line2D`, `baseLine:Line2D`。
- 参数单位标注：offset=mm。
- 依赖：当前直接：Eigen/GapUtils点线几何；度量内核本身无需PCL；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：std-geometry + core的小型测量数据；PCL/OpenCV/ONNX不得成为测量内核依赖。
- 当前契约：输入 baseLine:Line2D, refPoint:Point2D → 输出 value:Measurement, segment:Line2D, baseLine:Line2D。米制Line2D+Point2D→毫米Measurement/米制segment/baseLine；canonical方向后上方为正，scale+offset；输出基准段会扩展到垂足。
- 目标契约：同帧有符号面差+垂足/扩展基准段，输入单位和法向显式；mm参数及canonical符号由迁移适配；无公差判定。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：水平/竖直线、反转方向、负面差、scale/offset和baseLine扩展都保持；与DTS符号分别验证。
- 说明：不能仅迁value而丢掉后续gap definition A使用的扩展baseLine。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 077 · gap.gap

- 当前：`gap` / `1.0.0` / 间隙测量。目标：`std-metrology`；拆公共能力/适配；**M4**。
- 源码：[packs/gap/ops/measure.cpp:348](../../packs/gap/ops/measure.cpp#L348)。
- 端口：`left:Circle2D`, `right:Circle2D`, `baseLine:Line2D?` → `value:Measurement`, `segment:Line2D`。
- 参数单位标注：offset=mm。
- 依赖：当前直接：Eigen/GapUtils两圆距离；可抽纯几何内核；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：std-geometry + core的小型测量数据；PCL/OpenCV/ONNX不得成为测量内核依赖。
- 当前契约：输入 left:Circle2D, right:Circle2D, baseLine:Line2D? → 输出 value:Measurement, segment:Line2D。米制两圆+[带端点基准线]→毫米Measurement+米制segment；A按基准方向切线间距，B按圆心连线圆边间距；交叉失败语义不同。
- 目标契约：通用圆边间距与沿基准投影间隙，模式分别定义；保留左/右顺序、offset以及A报invalid_geometry/B输出非有限且ok=false。
- 迁移前置：M1；M2；gap.flush
- 关联算子：gap.flush
- 验收：两种定义、圆相交/左右接反、零间隙、缺基准端点及scale换算基线。
- 说明：不得把B误写成纯圆心距离或统一成绝对距离掩盖交叉。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 078 · gap.groove_joint

- 当前：`gap` / `1.0.0` / 槽形对接缝测量。目标：`solution-gap`；拆公共能力/适配；**M4**。
- 源码：[packs/gap/ops/groove.cpp:458](../../packs/gap/ops/groove.cpp#L458)。
- 端口：`primary:PointCloud`, `secondary:PointCloud` → `gap:Measurement`, `flush:Measurement`, `baseLine:Line2D`, `refLine:Line2D`, `gapSegment:Line2D`, `flushSegment:Line2D`, `groove:Point2D`, `quality:Record`, `qualityBase:Record`, `qualityRef:Record`。
- 参数单位标注：grooveDepthMin=mm, leftNear=mm, leftFar=mm, rightNear=mm, rightFar=mm, lineDistThresh=mm, gapDepth=mm, gapOffset=mm, flushOffset=mm。
- 依赖：当前直接：PCL/Eigen轮廓拟合；逐相机均值与槽宽专用流程；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 primary:PointCloud, secondary:PointCloud → 输出 gap:Measurement, flush:Measurement, baseLine:Line2D, refLine:Line2D, gapSegment:Line2D, flushSegment:Line2D, groove:Point2D, quality:Record, qualityBase:Record, qualityRef:Record。双头米制云→mm槽宽/面差+几何/quality；深槽中位找心，两台分开拟面后平均，在低面下gapDepth切片量宽。
- 目标契约：轮廓切片/线拟合可下沉std-profile，点线测量下沉std-metrology；深槽前提、双相机平均和gapDepth规则留场景。
- 迁移前置：M3；gap.fit_line；gap.flush
- 关联算子：gap.fit_line；gap.flush
- 验收：无槽/缝闭合、圆角/槽底干扰、单/双相机点数不足、gapDepth边界与读数质量。
- 说明：不是通用孔槽测宽；不能与notch_width自动互换。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 079 · gap.judge

- 当前：`gap` / `1.0.0` / 间隙面差判定。目标：`solution-gap`；拆公共能力/适配；**M4**。
- 源码：[packs/gap/ops/measure.cpp:470](../../packs/gap/ops/measure.cpp#L470)。
- 端口：`value:Measurement` → `value:Measurement`。
- 参数单位标注：nominal=mm, upper=mm, lower=mm, margin=mm。
- 依赖：当前直接：标准库Measurement公差比较，无PCL/ONNX算法调用；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 value:Measurement → 输出 value:Measurement。毫米Measurement→带limits/verdict的Measurement；非有限或ok=false判fail；patrol把超差记margin，0外余量边界按现实现。
- 目标契约：通用区间比较可用std-value；nominal/上下偏差/margin/patrol及Measurement verdict写入留场景判定，std-metrology只产量值质量。
- 迁移前置：M1；gap.flush；gap.gap
- 关联算子：gap.flush；gap.gap
- 验收：上下限包含边界、margin=0/正值、patrol超差、上游无效值及原始量值不被覆盖。
- 说明：不要把可计算值与业务OK/NG合成同一通用测量定义。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 080 · gap.load_profile_pair

- 当前：`gap` / `1.0.0` / 双相机剖面加载。目标：`solution-gap`；拆公共能力/适配；**M4**。
- 源码：[packs/gap/ops/io.cpp:173](../../packs/gap/ops/io.cpp#L173)。
- 端口：无 → `primary:PointCloud`, `secondary:PointCloud`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前直接：PCL PCD I/O；标准库目录查找；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 无 → 输出 primary:PointCloud, secondary:PointCloud。双头 Master/Slave PCD→传感器 XZ 米；layout=profile 将 y 高度搬至 z；dropNonFinite 默认开，模型链必须关。
- 目标契约：通用轮廓/点云读取下沉 std-profile/std-pointcloud；双头命名、文件选择及布局适配留场景，必须显式保槽策略。
- 迁移前置：M1；M3
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：目录多采集的确定性选文件、layout 两种布局、1280槽 NaN 与颜色/强度保持。
- 说明：只改包归属不能改变文件自动选择策略。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 081 · gap.load_template

- 当前：`gap` / `1.0.0` / 剖面模板加载。目标：`solution-gap`；拆公共能力/适配；**M4**。
- 源码：[packs/gap/ops/io.cpp:282](../../packs/gap/ops/io.cpp#L282)。
- 端口：无 → `left:PointCloud`, `right:PointCloud`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前直接：PCL PCD I/O；标准库路径/文件状态；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 无 → 输出 left:PointCloud, right:PointCloud。左右模板PCD→PointCloud，模板已在测量 XY 米；与当前配置对应，不再换轴。
- 目标契约：文件读取下沉 std-pointcloud；双模板槽/配置绑定留场景，模板帧/单位和内容标识必须显式。
- 迁移前置：M1；M3
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：旧路径解析、双模板配对、错误文件和禁止二次换轴的迁移用例。
- 说明：模板身份并非仅文件路径。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 082 · gap.make_roi_set

- 当前：`gap` / `1.1.0` / 角色 ROI 集组装。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[packs/gap/ops/bundle_ops.cpp:134](../../packs/gap/ops/bundle_ops.cpp#L134)。
- 端口：`datum:Box2D`, `target:Box2D`, `seamLeft:Box2D`, `seamRight:Box2D`, `alignment:Record?`, `overall:Box2D?`, `cropStatus:Record?` → `rois:Bundle<gap.RoiSet>`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前直接：core Bundle/Record装拆，标准库；无PCL/ONNX求解；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 datum:Box2D, target:Box2D, seamLeft:Box2D, seamRight:Box2D, alignment:Record?, overall:Box2D?, cropStatus:Record? → 输出 rois:Bundle<gap.RoiSet>。四角色Box2D/seam加可选信息封装为Bundle<gap.RoiSet>；角色和datumSide为Gap业务约定。
- 目标契约：保留场景Bundle适配；共用显式单位/坐标与数据引用规则，不能直接改名成通用Profile或RoiSet。
- 迁移前置：M1；M3
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：构造/拆解往返，必需字段检查、可选info与米/mm边界；类型不匹配拒绝。
- 说明：Bundle类型名称属于保存图/manifest/SDK契约，迁移包含引用它的全部积木。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 083 · gap.make_scan_pair

- 当前：`gap` / `1.0.0` / 剖面对组装。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[packs/gap/ops/bundle_ops.cpp:85](../../packs/gap/ops/bundle_ops.cpp#L85)。
- 端口：`primary:PointCloud`, `secondary:PointCloud`, `merged:PointCloud` → `scan:Bundle<gap.ScanPair>`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前直接：core Bundle/Record装拆，标准库；无PCL/ONNX求解；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 primary:PointCloud, secondary:PointCloud, merged:PointCloud → 输出 scan:Bundle<gap.ScanPair>。双头/合并云封装为 Bundle<gap.ScanPair>，primary/secondary/merged三者均米；可选参数不代表外部注入已校准。
- 目标契约：保留场景Bundle适配；共用显式单位/坐标与数据引用规则，不能直接改名成通用Profile或RoiSet。
- 迁移前置：M1；M3
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：构造/拆解往返，必需字段检查、可选info与米/mm边界；类型不匹配拒绝。
- 说明：Bundle类型名称属于保存图/manifest/SDK契约，迁移包含引用它的全部积木。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 084 · gap.nearest_to_line

- 当前：`gap` / `1.0.0` / 直线最近点提取。目标：`std-profile`；迁移归属；**M4**。
- 源码：[packs/gap/ops/geometry.cpp:462](../../packs/gap/ops/geometry.cpp#L462)。
- 端口：`cloud:PointCloud`, `line:Line2D` → `point:Point2D`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前直接：PCL/Eigen 适配；点线距离内核可无PCL；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：轮廓基础模块不引PCL/OpenCV/ONNX；明确标注的原PCL拟合方法作为可选后端隔离。
- 当前契约：输入 cloud:PointCloud, line:Line2D → 输出 point:Point2D。米制云+Line2D→Point2D；按无限直线垂距，不限线段端点；平局取原点序靠前项。
- 目标契约：轮廓到几何基准的最近样本；显式无限线/线段模式与原列索引；需ROI时由上游裁剪。
- 迁移前置：M1；M2；M3
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：线段外最近点、反向线、平局稳定性和裁前/裁后索引关系。
- 说明：移动代码时保留无限线语义。

## 085 · gap.notch_width

- 当前：`gap` / `1.0.0` / V 形缝开口测量。目标：`solution-gap`；拆公共能力/适配；**M4**。
- 源码：[packs/gap/ops/notch.cpp:631](../../packs/gap/ops/notch.cpp#L631)。
- 端口：`primary:PointCloud`, `secondary:PointCloud` → `gap:Measurement`, `flush:Measurement`, `baseLine:Line2D`, `refLine:Line2D`, `gapSegment:Line2D`, `flushSegment:Line2D`, `anchor:Point2D`, `quality:Record`, `qualityBase:Record`, `qualityRef:Record`。
- 参数单位标注：levelDepth=mm, baseNear=mm, baseFar=mm, lineDistThresh=mm, gapOffset=mm, refNear=mm, refFar=mm, flushOffset=mm。
- 依赖：当前直接：PCL/Eigen轮廓走线/拟合；遮挡相机选择；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 primary:PointCloud, secondary:PointCloud → 输出 gap:Measurement, flush:Measurement, baseLine:Line2D, refLine:Line2D, gapSegment:Line2D, flushSegment:Line2D, anchor:Point2D, quality:Record, qualityBase:Record, qualityRef:Record。双头米制云→mm开口/面差及几何/quality；V缝在基准下levelDepth求穿出点，逐相机后平均；浅角报notch_too_shallow。
- 目标契约：轮廓等高交点/插值下沉std-profile，距离下沉std-metrology；V缝、camera排除与闭合归零策略留场景。
- 迁移前置：M3；gap.fit_line；gap.flush
- 关联算子：gap.fit_line；gap.flush
- 验收：遮挡视角、闭合固有宽度、levelDepth超cornerDepth与单相机策略基线。
- 说明：不能把固有V角宽度当真实张开量或通用槽宽。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 086 · gap.overall_roi

- 当前：`gap` / `1.0.0` / 整体 ROI 构建。目标：`solution-gap`；拆公共能力/适配；**M4**。
- 源码：[packs/gap/ops/geometry.cpp:332](../../packs/gap/ops/geometry.cpp#L332)。
- 端口：`primary:PointCloud`, `secondary:PointCloud` → `box:Box2D`。
- 参数单位标注：roi=mm。
- 依赖：当前直接：PCL 点云适配；逐坐标中位数与框构造；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 primary:PointCloud, secondary:PointCloud → 输出 box:Box2D。双头米制云→Box2D；参数roi用mm；auto_center保持宽高，中心取两个稳健中心中点。
- 目标契约：稳健中心估计下沉 std-profile/std-pointcloud；双头中心平均和配置宽高留场景；不改变背景拖动风险语义。
- 迁移前置：M1；M3
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：空/单边缺失、两头点数不均、背景干扰和固定宽高对照。
- 说明：不是任意云的通用自动 ROI 分割器。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 087 · gap.align_template

- 当前：`gap` / `1.1.0` / 模板配准。目标：`solution-gap`；拆公共能力/适配；**M4**。
- 源码：[packs/gap/ops/align.cpp:339](../../packs/gap/ops/align.cpp#L339)。
- 端口：`cloud:PointCloud`, `tplLeft:PointCloud`, `tplRight:PointCloud` → `alignment:Record`。
- 参数单位标注：maxMatchingDist=mm, maxFitnessDist=mm, trustTranslation=mm, trustRotation=deg, roiFlushBase=mm, roiGapLeft=mm, roiFlushRef=mm, roiGapRight=mm。
- 依赖：当前直接：PCL/Eigen 2D ICP 与 lyflow_std_algo；领域信赖域/退化锁定；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 cloud:PointCloud, tplLeft:PointCloud, tplRight:PointCloud → 输出 alignment:Record。合并米制云+双模板→GapAlignment；全局粗配+逐侧ICP；信赖域钳制不报错，低分仍返回候选。
- 目标契约：通用 ICP 继续 std-pointcloud 后端；双侧复用、钳制、质量字段和模板角色保留场景；不替换求解器。
- 迁移前置：gap.load_template；gap.overall_roi
- 关联算子：gap.load_template；gap.overall_roi；register.icp_2d
- 验收：长直边退化、trustClamped、左右同模板复用、低分候选和最终选模结果。
- 说明：分包迁移不等于改用 KISS 或另一个 ICP。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。 关联的register.icp_2d在本波次使用既有接口；其M6隔离验收不是本项迁移前置，后续隔离须保持该接口。

## 088 · gap.corner_vertex

- 当前：`gap` / `1.0.0` / 夹角接缝间隙测量。目标：`solution-gap`；拆公共能力/适配；**M4**。
- 源码：[packs/gap/ops/measure.cpp:390](../../packs/gap/ops/measure.cpp#L390)。
- 端口：`lineLeft:Line2D`, `lineRight:Line2D`, `baseLine:Line2D?`, `alignment:Record?` → `vertex:Point2D`, `gap:Measurement`, `flush:Measurement`, `angle:Measurement`, `segment:Line2D`, `quality:Record`。
- 参数单位标注：originX=mm, originY=mm, gapOffset=mm, flushOffset=mm, minAngle=deg, maxAngle=deg。
- 依赖：当前直接：Eigen 直线求交/基准分解；领域软装/金件顶点约定；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 lineLeft:Line2D, lineRight:Line2D, baseLine:Line2D?, alignment:Record? → 输出 vertex:Point2D, gap:Measurement, flush:Measurement, angle:Measurement, segment:Line2D, quality:Record。米制两翼线+基准/可选GapAlignment→虚拟顶点及mm间隙/面差、角度Measurement和quality；近90度时不可分别辨识。
- 目标契约：线求交归std-geometry，基准分量度量归std-metrology；金件原点/配准绑定及不可辨识警示留场景。
- 迁移前置：M1；M2；gap.align_template
- 关联算子：gap.align_template
- 验收：平行/小角退化、约90度不可辨识、对齐前后原点和gap/flush符号对照。
- 说明：几何有数值不代表两项业务量都可判定；patrol语义留场景。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 089 · gap.point_offset

- 当前：`gap` / `1.0.0` / 二维位移分解。目标：`std-metrology`；迁移归属；**M4**。
- 源码：[packs/gap/ops/offset.cpp:92](../../packs/gap/ops/offset.cpp#L92)。
- 端口：`a:Point2D`, `b:Point2D` → `dx:Measurement`, `dy:Measurement`, `distance:Measurement`, `segment:Line2D`。
- 参数单位标注：dxOffset=mm, dyOffset=mm, scaleDx=mm, scaleDy=mm。
- 依赖：当前直接：标准库/Eigen两点差分，无需PCL求解；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：std-geometry + core的小型测量数据；PCL/OpenCV/ONNX不得成为测量内核依赖。
- 当前契约：输入 a:Point2D, b:Point2D → 输出 dx:Measurement, dy:Measurement, distance:Measurement, segment:Line2D。米制Point2D a/b→mm dx/dy/distance及米制segment；沿测量帧x/y，不沿拟合面；各分量scale/offset。
- 目标契约：显式坐标帧/轴方向的两点位移测量，长度单位与标定补偿独立；上游点是否属于同一缝由场景保障。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：轴旋转、正负dx/dy、重合点、独立scale/offset与线段方向回归。
- 说明：scaleDx/scaleDy现有参数单位标注需单独审计，比例因子不能因标mm就作长度换算。

## 090 · gap.result_bundle

- 当前：`gap` / `2.1.0` / 间隙检测结果汇总。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[packs/gap/ops/result_bundle.cpp:398](../../packs/gap/ops/result_bundle.cpp#L398)。
- 端口：`gap:Measurement`, `flush:Measurement?`, `rois:Bundle<gap.RoiSet>?`, `scan:Bundle<gap.ScanPair>?`, `roiOverall:Box2D?`, `fits:Record?`, `fitBase:Record?`, `fitRef:Record?`, `cropStatus:Record?`, `alignment:Record?`, `fallback:Record?` → `bundle:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前直接：core Bundle/Record/Measurement封装，标准库；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 gap:Measurement, flush:Measurement?, rois:Bundle<gap.RoiSet>?, scan:Bundle<gap.ScanPair>?, roiOverall:Box2D?, fits:Record?, fitBase:Record?, fitRef:Record?, cropStatus:Record?, alignment:Record?, fallback:Record? → 输出 bundle:Record。GapRoiSet/ScanPair与量值/质量→GapResultBundle；graphSha256由宿主传；未接记inactive，点数取定位后scan。
- 目标契约：旧QualityMetrics/results.csv适配留场景；结果引用原生数据与版本化字段；缺项、未执行、失败不能变合格。
- 迁移前置：gap.make_scan_pair；gap.make_roi_set；gap.judge
- 关联算子：gap.make_scan_pair；gap.make_roi_set；gap.judge
- 验收：宿主必需字段、roi_source、graph_hash、左右点数既有含义与inactive缺项回归。
- 说明：left/right_point_count当前均按合并云统计，迁移不应无声纠正历史字段意义。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 091 · gap.role_line

- 当前：`gap` / `1.0.0` / 角色区域直线拟合。目标：`solution-gap`；拆公共能力/适配；**M4**。
- 源码：[packs/gap/ops/blocks.cpp:1019](../../packs/gap/ops/blocks.cpp#L1019)。
- 端口：`scan:Bundle<gap.ScanPair>`, `rois:Bundle<gap.RoiSet>`, `refLine:Line2D?` → `line:Line2D`, `innerEnd:Point2D`, `quality:Record`。
- 参数单位标注：distThresh=mm, dirNominalDeg=°, dirTolDeg=°。
- 依赖：当前直接：复合：PCL受约束拟合；Gap datum/target角色选取；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 scan:Bundle<gap.ScanPair>, rois:Bundle<gap.RoiSet>, refLine:Line2D? → 输出 line:Line2D, innerEnd:Point2D, quality:Record。ScanPair+RoiSet+[refLine]→Line2D/innerEnd/quality；角色决定框，toward来自两缝框中点。
- 目标契约：角色解引用和靠缝选择留场景；执行std-profile拟线能力；不把Gap角色塞进通用Profile类型。
- 迁移前置：gap.fit_line；gap.make_roi_set；gap.make_scan_pair
- 关联算子：gap.fit_line；gap.make_roi_set；gap.make_scan_pair
- 验收：datum/target、缺refLine、左右datumSide、继承拟线quality与segment截取。
- 说明：不能只复用无toward的直线拟合。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 092 · gap.seam_circles

- 当前：`gap` / `1.2.0` / 缝隙双侧圆拟合。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[packs/gap/ops/blocks.cpp:1131](../../packs/gap/ops/blocks.cpp#L1131)。
- 端口：`scan:Bundle<gap.ScanPair>`, `rois:Bundle<gap.RoiSet>`, `refLine:Line2D?`, `refLineRight:Line2D?` → `left:Circle2D`, `right:Circle2D`, `quality:Record`。
- 参数单位标注：distThresh=mm, retryDistance=mm, nominal=mm, offset=mm, leftRadiusValue=mm, leftRadiusMin=mm, leftRadiusMax=mm, leftRadiusPrior=mm, leftRadiusPriorSigma=mm, rightRadiusValue=mm, rightRadiusMin=mm, rightRadiusMax=mm, rightRadiusPrior=mm, rightRadiusPriorSigma=mm, leftCenterAbove=mm, leftCenterTol=mm, rightCenterAbove=mm, rightCenterTol=mm, leftTangentAbove=mm, leftTangentTol=mm, rightTangentAbove=mm, rightTangentTol=mm, leftArcBearingDeg=°, leftArcBearingTolDeg=°, rightArcBearingDeg=°, rightArcBearingTolDeg=°, leftMinArcDeg=°, rightMinArcDeg=°。
- 依赖：当前直接：复合：PCL/Eigen圆拟合与双相机/角色选择；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 scan:Bundle<gap.ScanPair>, rois:Bundle<gap.RoiSet>, refLine:Line2D?, refLineRight:Line2D? → 输出 left:Circle2D, right:Circle2D, quality:Record。ScanPair+RoiSet+[左右refLine]→left/right Circle2D和quality；转发精细圆拟合参数及两边角色框。
- 目标契约：双侧缝角色包装留场景；单圆算法下沉后仍保留全部约束、相机回退和独立参考线。
- 迁移前置：gap.fit_gap_circles；gap.make_roi_set
- 关联算子：gap.fit_gap_circles；gap.make_roi_set
- 验收：参数透传、左右参考线、固定半径/先验/相切带及quality原子失败。
- 说明：底层generic参数结构迁移必须覆盖此节点的paramOf拷贝。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 093 · gap.select_alignment

- 当前：`gap` / `1.0.0` / 配准结果选择。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[packs/gap/ops/align.cpp:426](../../packs/gap/ops/align.cpp#L426)。
- 端口：`a:Record`, `b:Record?`, `c:Record?`, `d:Record?` → `alignment:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前直接：标准库 Record 排序/筛选，无 PCL 求解；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 a:Record, b:Record?, c:Record?, d:Record? → 输出 alignment:Record。最多4个 GapAlignment；先两侧门限，再min/mean降序、配置序/id升序；全不合格报 icp_score_low。
- 目标契约：保持Gap候选选择策略；底层可复用 std-value 排序，但不能丢 templateId、配置序和全部失败诊断。
- 迁移前置：gap.align_template
- 关联算子：gap.align_template
- 验收：全打平、端口交换、缺候选/全低分、稳定id决定平局的结果一致。
- 说明：这是领域决策，不进入 std-metrology。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 094 · gap.business_rois

- 当前：`gap` / `1.2.0` / 测量 ROI 映射。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[packs/gap/ops/geometry.cpp:386](../../packs/gap/ops/geometry.cpp#L386)。
- 端口：`alignment:Record` → `flushBase:Box2D`, `gapLeft:Box2D`, `flushRef:Box2D`, `gapRight:Box2D`, `seam:Box2D`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前直接：Eigen 2D 坐标运算；领域角色框；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 alignment:Record → 输出 flushBase:Box2D, gapLeft:Box2D, flushRef:Box2D, gapRight:Box2D, seam:Box2D。GapAlignment→四个角色Box2D+seam，米；仅移动中心，保持宽高/轴对齐，左右可用不同配准。
- 目标契约：角色框绑定保留场景；通用变换下沉 std-geometry 但必须支持中心变换保持尺寸的旧模式。
- 迁移前置：gap.select_alignment；M2
- 关联算子：gap.select_alignment
- 验收：大旋转不扩框、datumSide与左右变换、seam中心及框尺寸逐项对照。
- 说明：不能直接替换成旋转包围盒变换。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 095 · gap.datum_window

- 当前：`gap` / `1.0.0` / 方向基准区域构建。目标：`solution-gap`；拆公共能力/适配；**M4**。
- 源码：[packs/gap/ops/geometry.cpp:277](../../packs/gap/ops/geometry.cpp#L277)。
- 端口：`anchor:Box2D`, `heightAnchor:Box2D?` → `box:Box2D`。
- 参数单位标注：startMm=mm, lengthMm=mm, heightMm=mm。
- 依赖：当前直接：标准库/Eigen 2D 框运算，无 PCL 求解；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 anchor:Box2D, heightAnchor:Box2D? → 输出 box:Box2D。Box2D 锚框+可选高度锚→轴对齐长窗，输入/输出米，startMm/lengthMm/heightMm 毫米，窗口避开锚框本身。
- 目标契约：通用锚框偏置窗口可下沉 std-geometry；用缝定位邻基准面的规则与方向先验留场景。
- 迁移前置：M1；M2；gap.business_rois
- 关联算子：gap.business_rois
- 验收：左右侧、单独heightAnchor、锚框漂移和开区间边界案例。
- 说明：领域默认不能提升为通用框构造默认。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 096 · gap.datum_direction

- 当前：`gap` / `1.0.0` / 方向基准线提取。目标：`solution-gap`；拆公共能力/适配；**M4**。
- 源码：[packs/gap/ops/blocks.cpp:1181](../../packs/gap/ops/blocks.cpp#L1181)。
- 端口：`scan:Bundle<gap.ScanPair>`, `rois:Bundle<gap.RoiSet>` → `line:Line2D`, `quality:Record`。
- 参数单位标注：startMm=mm, lengthMm=mm, heightMm=mm, distThresh=mm。
- 依赖：当前直接：复合：窗口构造+PCL裁剪/拟合；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 scan:Bundle<gap.ScanPair>, rois:Bundle<gap.RoiSet> → 输出 line:Line2D, quality:Record。ScanPair+RoiSet→方向Line2D/quality；邻近长面窗口，开区间裁剪，不截取内点端，inlier_ends端点。
- 目标契约：基准方向先验编排留场景；窗构造/裁剪/拟线调用新能力，保留开边界及segmentPoints=0。
- 迁移前置：gap.datum_window；gap.fit_line；gap.business_rois
- 关联算子：gap.datum_window；gap.fit_line；gap.business_rois
- 验收：短台肩/邻长面、左右窗、开区间点和端点模式、少内点诊断。
- 说明：必须和gap.datum_window、role_line方向约束一起迁。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 097 · gap.selected_point

- 当前：`gap` / `1.0.0` / 区域锚点近邻提取。目标：`std-profile`；迁移归属；**M4**。
- 源码：[packs/gap/ops/geometry.cpp:437](../../packs/gap/ops/geometry.cpp#L437)。
- 端口：`cloud:PointCloud`, `box:Box2D` → `point:Point2D`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前直接：PCL/Eigen 点云适配；最近点扫描本身标准库；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：轮廓基础模块不引PCL/OpenCV/ONNX；明确标注的原PCL拟合方法作为可选后端隔离。
- 当前契约：输入 cloud:PointCloud, box:Box2D → 输出 point:Point2D。米制PointCloud+Box2D→Point2D，取整片云距离框min角最近点，不先裁ROI；框尺寸不影响选择。
- 目标契约：轮廓对显式锚点的最近样本提取；输出原列索引；旧Box2D.min适配留场景，明确整轮廓范围。
- 迁移前置：M1；M3
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：最近点在ROI外、相同距离、原列映射和非有限样本处理保持。
- 说明：不能把旧算子误迁为框内选点。

## 098 · gap.ref_point

- 当前：`gap` / `1.0.0` / 参考点提取。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[packs/gap/ops/blocks.cpp:1074](../../packs/gap/ops/blocks.cpp#L1074)。
- 端口：`scan:Bundle<gap.ScanPair>`, `rois:Bundle<gap.RoiSet>`, `baseLine:Line2D?` → `point:Point2D`, `line:Line2D`, `quality:Record`。
- 参数单位标注：distThresh=mm。
- 依赖：当前直接：复合：PCL轮廓点筛选/可选拟线；三种业务参考点模式；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 scan:Bundle<gap.ScanPair>, rois:Bundle<gap.RoiSet>, baseLine:Line2D? → 输出 point:Point2D, line:Line2D, quality:Record。ScanPair+RoiSet+[baseLine]→point/line/quality；line_end/selected_point/nearest_point模式不同。
- 目标契约：参考点模式与角色留场景；底层最近点、拟线等能力归std-profile，保留模式对应的line未激活语义。
- 迁移前置：gap.role_line；gap.selected_point；gap.nearest_to_line
- 关联算子：gap.role_line；gap.selected_point；gap.nearest_to_line
- 验收：三种ref_type、ROI外选点与无限线最近点、未激活输出和失败quality对照。
- 说明：参考点不能一律抽成同一最近点算法。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 099 · gap.split_roi_set

- 当前：`gap` / `1.0.0` / 角色 ROI 集解包。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[packs/gap/ops/bundle_ops.cpp:197](../../packs/gap/ops/bundle_ops.cpp#L197)。
- 端口：`rois:Bundle<gap.RoiSet>` → `datum:Box2D`, `target:Box2D`, `seamLeft:Box2D`, `seamRight:Box2D`, `seam:Box2D`, `info:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前直接：core Bundle/Record装拆，标准库；无PCL/ONNX求解；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 rois:Bundle<gap.RoiSet> → 输出 datum:Box2D, target:Box2D, seamLeft:Box2D, seamRight:Box2D, seam:Box2D, info:Record。Bundle<gap.RoiSet>拆四角色Box2D/seam与info，不移动框，不改变角色。
- 目标契约：保留场景Bundle适配；共用显式单位/坐标与数据引用规则，不能直接改名成通用Profile或RoiSet。
- 迁移前置：M1；M3
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：构造/拆解往返，必需字段检查、可选info与米/mm边界；类型不匹配拒绝。
- 说明：Bundle类型名称属于保存图/manifest/SDK契约，迁移包含引用它的全部积木。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 100 · gap.split_scan_pair

- 当前：`gap` / `1.0.0` / 剖面对解包。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[packs/gap/ops/bundle_ops.cpp:111](../../packs/gap/ops/bundle_ops.cpp#L111)。
- 端口：`scan:Bundle<gap.ScanPair>` → `primary:PointCloud`, `secondary:PointCloud`, `merged:PointCloud`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前直接：core Bundle/Record装拆，标准库；无PCL/ONNX求解；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 scan:Bundle<gap.ScanPair> → 输出 primary:PointCloud, secondary:PointCloud, merged:PointCloud。Bundle<gap.ScanPair>拆成primary/secondary/merged三份PointCloud，不重算、不换轴。
- 目标契约：保留场景Bundle适配；共用显式单位/坐标与数据引用规则，不能直接改名成通用Profile或RoiSet。
- 迁移前置：M1；M3
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：构造/拆解往返，必需字段检查、可选info与米/mm边界；类型不匹配拒绝。
- 说明：Bundle类型名称属于保存图/manifest/SDK契约，迁移包含引用它的全部积木。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 101 · gap.to_measurement_frame

- 当前：`gap` / `1.0.0` / 测量坐标系转换。目标：`std-profile`；拆公共能力/适配；**M4**。
- 源码：[packs/gap/ops/io.cpp:256](../../packs/gap/ops/io.cpp#L256)。
- 端口：`cloud:PointCloud` → `cloud:PointCloud`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前直接：PCL 点序转换适配；坐标交换本身无第三方；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：轮廓基础模块不引PCL/OpenCV/ONNX；明确标注的原PCL拟合方法作为可选后端隔离。
- 当前契约：输入 cloud:PointCloud → 输出 cloud:PointCloud。PointCloud 米的 y/z 互换，XZ→XY；这是 det=-1 的反射，保持槽序和其他通道。
- 目标契约：有序轮廓显式平面/轴映射，支持反射并携带单位；旧 gap 轴定义由场景迁移；不能用仅支持刚体的 Transform 替换。
- 迁移前置：M1；M3；gap.load_profile_pair
- 关联算子：gap.load_profile_pair
- 验收：正负高度、NaN槽、强度/RGB 和映射两次还原；手性被明确记录。
- 说明：与 std-geometry 的 source→target 刚体必须区分。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 102 · gap.drop_non_finite

- 当前：`gap` / `1.0.0` / 非有限点剔除。目标：`std-profile`；迁移归属；**M4**。
- 源码：[packs/gap/ops/model_roi.cpp:688](../../packs/gap/ops/model_roi.cpp#L688)。
- 端口：`cloud:PointCloud` → `cloud:PointCloud`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前直接：PCL 类型适配；仅坐标有限性筛选，无统计滤波；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：轮廓基础模块不引PCL/OpenCV/ONNX；明确标注的原PCL拟合方法作为可选后端隔离。
- 当前契约：输入 cloud:PointCloud → 输出 cloud:PointCloud。PointCloud→PointCloud，删非有限坐标后相对点序保留但1280槽号失效，禁止放在模型标签操作之前。
- 目标契约：有序轮廓无效样本处理明确保槽/压缩与源槽映射；通用点云有限点过滤可复用同一无依赖内核。
- 迁移前置：M1；M3；gap.to_measurement_frame
- 关联算子：gap.to_measurement_frame
- 验收：NaN/Inf 坐标删除、RGB/强度长度匹配；压缩云不得误接1280槽模型链。
- 说明：该节点不能当离群点滤波。

## 103 · gap.profile_tensor

- 当前：`gap` / `1.0.0` / 剖面特征张量构建。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[packs/gap/ops/model_roi.cpp:505](../../packs/gap/ops/model_roi.cpp#L505)。
- 端口：`primary:PointCloud`, `secondary:PointCloud` → `tensor:Tensor`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前直接：领域六通道特征构造（标准库/Eigen 类型适配）；不直接执行 ONNX；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 primary:PointCloud, secondary:PointCloud → 输出 tensor:Tensor。双头原始1280槽→float32 Tensor[2,6,1280]，通道 z_norm/x_diff/normal_angle/curvature/intensity_norm/valid，必须保留 NaN 槽。
- 目标契约：六通道、双视角顺序与训练版本留场景模型适配；推理仍由 std-ml 执行；轮廓数据契约只提供显式平面/单位/槽。
- 迁移前置：M1；M3；gap.to_measurement_frame
- 关联算子：gap.to_measurement_frame
- 验收：逐通道与训练侧向量核对；点数错误拒绝；无效槽 valid 掩码正确且批次主副顺序稳定。
- 说明：名字含 tensor 不代表通用推理能力。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 104 · gap.camera_guard

- 当前：`gap` / `1.0.0` / 双相机一致性校验。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[packs/gap/ops/camera_guard.cpp:213](../../packs/gap/ops/camera_guard.cpp#L213)。
- 端口：`primary:PointCloud`, `secondary:PointCloud`, `box:Box2D?` → `primary:PointCloud`, `secondary:PointCloud`, `quality:Record`。
- 参数单位标注：maxDeltaMm=mm, sampleStep=mm, halfWindow=mm。
- 依赖：当前直接：标准库高度采样/中位数；PCL类型随领域头携带；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 primary:PointCloud, secondary:PointCloud, box:Box2D? → 输出 primary:PointCloud, secondary:PointCloud, quality:Record。双头米制云+可选ROI→双头+quality；自动认y/z高度，重叠段差中位数门；keep策略复制可信云到两个端口供[2,6,1280]模型。
- 目标契约：通用重叠残差可下沉std-profile；可信相机策略/双行复制留场景；目标显式平面与视角来源，禁止把复制云当两个独立证据。
- 迁移前置：M1；M3；gap.profile_tensor
- 关联算子：gap.profile_tensor
- 验收：fail/keepPrimary/keepSecondary/record/off、不同采样密度、重叠不足和复制后点数翻倍。
- 说明：不是三维配准器或自动相机校准。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 105 · gap.labels_from_logits

- 当前：`gap` / `1.0.0` / Logits 标签解码。目标：`solution-gap`；拆公共能力/适配；**M4**。
- 源码：[packs/gap/ops/model_roi.cpp:545](../../packs/gap/ops/model_roi.cpp#L545)。
- 端口：`tensor:Tensor` → `labels:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前直接：Tensor 遍历/argmax 为标准库，不直接调用 ONNX；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 tensor:Tensor → 输出 labels:Record。Tensor[2,C,1280]→GapLabels 两行1280项；并列留最小类id，不按置信度丢槽。
- 目标契约：通用 argmax 可下沉 std-ml 轻量前后处理；双头槽布局、标签类型和类语义留场景适配，不能改变并列/低置信度行为。
- 迁移前置：gap.profile_tensor
- 关联算子：gap.profile_tensor；ml.onnx_run
- 验收：并列logits、全部低置信度、形状错误、主副顺序与旧 GapLabels 一致。
- 说明：不能给原流程悄悄增加置信度门限。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。 关联的ml.onnx_run在本波次使用既有接口；其M6隔离验收不是本项迁移前置，后续隔离须保持该接口。

## 106 · gap.labels_to_cloud

- 当前：`gap` / `1.0.0` / 点云标签可视化。目标：`solution-gap`；拆公共能力/适配；**M4**。
- 源码：[packs/gap/ops/model_roi.cpp:643](../../packs/gap/ops/model_roi.cpp#L643)。
- 端口：`cloud:PointCloud`, `labels:Record` → `cloud:PointCloud`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前直接：点云通道分配与标准库标签遍历；不直接执行 ONNX；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 cloud:PointCloud, labels:Record → 输出 cloud:PointCloud。测量 XY 米的1280槽 PointCloud+GapLabels→上色云；固定八类色表，intensity 写类id；槽数不符报 bad_input。
- 目标契约：通用按索引/标签着色可下沉 std-pointcloud；八类配色、标签批次选择留场景；原有 intensity 覆盖行为必须显式迁移。
- 迁移前置：gap.labels_from_logits；gap.to_measurement_frame
- 关联算子：gap.labels_from_logits；gap.to_measurement_frame
- 验收：NaN槽对齐、RGB色表、intensity覆盖和压缩云误用拒绝。
- 说明：这是可视化适配，不是模型预测。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 107 · gap.read_scan

- 当前：`gap` / `1.1.0` / 双相机剖面读取。目标：`solution-gap`；拆公共能力/适配；**M4**。
- 源码：[packs/gap/ops/blocks.cpp:755](../../packs/gap/ops/blocks.cpp#L755)。
- 端口：`primary:PointCloud?`, `secondary:PointCloud?` → `scan:Bundle<gap.ScanPair>`。
- 参数单位标注：outlierRadiusMm=mm。
- 依赖：当前直接：PCL读取/合并/半径去噪；内部调用标准点云算子；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 primary:PointCloud?, secondary:PointCloud? → 输出 scan:Bundle<gap.ScanPair>。目录/文件或宿主双头输入→ScanPair；原双头保1280槽，merged按secondary在前并可去噪；inputs模式不读盘。
- 目标契约：读取/换轴/过滤下沉能力包；双头ScanPair构造与原槽/处理后合并云的区别留场景工作流或薄原生编排。
- 迁移前置：gap.load_profile_pair；gap.to_measurement_frame；gap.make_scan_pair
- 关联算子：gap.load_profile_pair；gap.to_measurement_frame；gap.make_scan_pair
- 验收：注入不读盘、主副连接缺项、原始槽和合并顺序、半径去噪结果与通道长度。
- 说明：不能将一份压缩后merged回填原双头。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 108 · gap.locate_template

- 当前：`gap` / `2.1.0` / 模板定位。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[packs/gap/ops/blocks.cpp:836](../../packs/gap/ops/blocks.cpp#L836)。
- 端口：`scan:Bundle<gap.ScanPair>` → `rois:Bundle<gap.RoiSet>`, `alignment:Record`, `scan:Bundle<gap.ScanPair>`。
- 参数单位标注：overallRoi=mm, template1DatumRoi=mm, template1TargetRoi=mm, template1SeamLeftRoi=mm, template1SeamRightRoi=mm, template2DatumRoi=mm, template2TargetRoi=mm, template2SeamLeftRoi=mm, template2SeamRightRoi=mm, template3DatumRoi=mm, template3TargetRoi=mm, template3SeamLeftRoi=mm, template3SeamRightRoi=mm, template4DatumRoi=mm, template4TargetRoi=mm, template4SeamLeftRoi=mm, template4SeamRightRoi=mm, maxMatchingDist=mm, maxFitnessDist=mm, trustTranslation=mm, trustRotation=deg。
- 依赖：当前直接：复合：PCL读模板/ICP/裁剪及Gap候选策略；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 scan:Bundle<gap.ScanPair> → 输出 rois:Bundle<gap.RoiSet>, alignment:Record, scan:Bundle<gap.ScanPair>。ScanPair→RoiSet/alignment/ScanPair；最多四模板槽各带角色框；自动整体ROI、裁剪、对齐、选模、业务ROI。
- 目标契约：保留可见的场景组合入口；优先用组合工作流表达，需性能或上下文时保留薄原生编排；模型/模板切换仍显式。
- 迁移前置：gap.align_template；gap.select_alignment；gap.business_rois；gap.read_scan
- 关联算子：gap.align_template；gap.select_alignment；gap.business_rois；gap.read_scan
- 验收：单/多模板、全部失败、各模板独立角色框、scan透传/裁剪与诊断来源。
- 说明：内部参数拷贝paramOf和Step节点id是迁移依赖，不只是顶层id改名。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 109 · gap.roi_from_labels

- 当前：`gap` / `1.2.0` / 标签区域提取。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[packs/gap/ops/model_roi.cpp:576](../../packs/gap/ops/model_roi.cpp#L576)。
- 端口：`primary:PointCloud`, `secondary:PointCloud`, `labels:Record`, `backdrop:PointCloud?` → `flushBase:Box2D`, `gapLeft:Box2D`, `flushRef:Box2D`, `gapRight:Box2D`, `seam:Box2D`, `refinements:Record`, `backdrop:PointCloud`。
- 参数单位标注：splitStepMm=mm, linkMm=mm, anchorGapMm=mm。
- 依赖：当前直接：PCL/Eigen 轮廓适配；领域掩膜精修和 ROI 推导，不直接执行 ONNX；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 primary:PointCloud, secondary:PointCloud, labels:Record, backdrop:PointCloud? → 输出 flushBase:Box2D, gapLeft:Box2D, flushRef:Box2D, gapRight:Box2D, seam:Box2D, refinements:Record, backdrop:PointCloud。原始双头1280槽+GapLabels→四角色 Box2D/seam/refinements；框转换为测量 XY 米；missing_segments 报 model_roi_failed。
- 目标契约：类别→四角色、精修规则留场景；明确输入原槽、输出坐标换算和 backdrop 已在测量帧，不借重命名自动通用化。
- 迁移前置：gap.profile_tensor；gap.labels_from_logits；gap.to_measurement_frame
- 关联算子：gap.profile_tensor；gap.labels_from_logits；gap.to_measurement_frame
- 验收：缺分段诊断、左右角色、米/毫米转换、同帧 backdrop 透传和精修结果基线。
- 说明：refinements 是小型诊断，不得携带完整图像/云数组。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 110 · gap.roll_anchored_crop

- 当前：`gap` / `1.1.0` / 零件随动裁剪。目标：`solution-gap`；拆公共能力/适配；**M4**。
- 源码：[packs/gap/ops/model_roi.cpp:716](../../packs/gap/ops/model_roi.cpp#L716)。
- 端口：`primary:PointCloud`, `secondary:PointCloud`, `gapLeft:Box2D`, `gapRight:Box2D`, `merged:PointCloud?` → `primary:PointCloud`, `secondary:PointCloud`, `merged:PointCloud`, `window:Box2D`, `status:Record`。
- 参数单位标注：halfWidth=mm, halfHeight=mm, maxRollBoxHeight=mm。
- 依赖：当前直接：PCL 点云裁剪；领域 roll 框保护策略；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 primary:PointCloud, secondary:PointCloud, gapLeft:Box2D, gapRight:Box2D, merged:PointCloud? → 输出 primary:PointCloud, secondary:PointCloud, merged:PointCloud, window:Box2D, status:Record。XY 米云+两个框→裁后双头/可选合并云、Box2D/status；参数mm；五种失效透传，点数不足回退无界框。
- 目标契约：裁剪内核归 std-profile/std-pointcloud；roll锚点、失效保护及原云回退留场景，保留 status 的 disabled/不可信/点数不足区分。
- 迁移前置：M3；gap.roi_from_labels
- 关联算子：gap.roi_from_labels
- 验收：五类保护与 minPointsKept 边界；未裁不能变空云；合并云与双头同窗。
- 说明：不能以简单 crop_box 节点取代整套保护。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 111 · gap.locate_model

- 当前：`gap` / `1.0.0` / 分割模型定位。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[packs/gap/ops/blocks.cpp:958](../../packs/gap/ops/blocks.cpp#L958)。
- 端口：`scan:Bundle<gap.ScanPair>` → `rois:Bundle<gap.RoiSet>`, `scan:Bundle<gap.ScanPair>`。
- 参数单位标注：splitStepMm=mm, linkMm=mm, anchorGapMm=mm, cropHalfWidth=mm, cropHalfHeight=mm, cropMaxRollBoxHeight=mm。
- 依赖：当前直接：复合：ONNX Runtime推理、领域六通道/ROI、PCL裁剪；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 scan:Bundle<gap.ScanPair> → 输出 rois:Bundle<gap.RoiSet>, scan:Bundle<gap.ScanPair>。ScanPair→RoiSet/ScanPair；原始双头保槽，内部张量/ONNX/argmax/精修ROI，之后剔NaN并跟随裁窗。
- 目标契约：模型场景组合；std-ml负责推理，std-profile负责显式槽/平面/裁剪，专用类语义与保护仍留场景。
- 迁移前置：gap.profile_tensor；gap.labels_from_logits；gap.roi_from_labels；gap.roll_anchored_crop
- 关联算子：gap.profile_tensor；gap.labels_from_logits；gap.roi_from_labels；gap.roll_anchored_crop
- 验收：六通道与标签槽对齐、模型初始化复用、ROI失败、原云/裁后merged分流和取消。
- 说明：模型能力必须可选，禁用模板模式不能仍隐式加载模型。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 256 · gap.measure_reference

- 当前：`gap` / `1.0.0` / 参考基线测量。目标：`solution-gap`；保留场景职责；**M7**。
- 源码：[packs/gap/ops/measure_reference.cpp:204](../../packs/gap/ops/measure_reference.cpp#L204)。
- 端口：`primary:PointCloud`, `secondary:PointCloud` → `gap:Measurement`, `flush:Measurement`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前直接：PCL/Eigen/yaml-cpp MeasurementEngine；useModel时直接ONNX Runtime；当前随包携带：PCL/Eigen、yaml-cpp、ONNX Runtime；不表示此节点逐项调用这些库。；目标：按节点需求启用std-profile/std-pointcloud/std-metrology；仅模型路径启用std-ml/ONNX，旧配置适配才需yaml-cpp。
- 当前契约：输入 primary:PointCloud, secondary:PointCloud → 输出 gap:Measurement, flush:Measurement。双头PointCloud→mm gap/flush，黑盒MeasurementEngine对照路径；模型模式内部执行ROI模型。
- 目标契约：标记历史对拍参考能力；核对所有调用者后在M7决定退役或作为独立reference可选模块，不能默认变新主路径。
- 迁移前置：M4；M6
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：历史A/B工具明确用途，未使用时不带额外模型/配置依赖；退役前覆盖保存图和调用者。
- 说明：当前gap算法已主动偏离旧基线，不能用数值完全相等作全部迁移的唯一正确性标准。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。
