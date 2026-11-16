# solution-gap 逐算子迁移清单

状态：实施中；当前 40 项。原251/7规划基线通过机器清单baselineReference追溯。返回[清单总览](../operator-package-audit.md)。

下列顺序是迁移/验收顺序，不是执行图调度顺序。精确端口和参数见[机器清单](../operator-package-audit.json)。

## 079 · gap.load_profile_pair

- 当前：`solution-gap` / `2.0.0` / 双路原生轮廓加载。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_scan.cpp:458](../../solutions/gap/ops/profile_scan.cpp#L458)。
- 端口：无 → `primary:Profile`, `secondary:Profile`, `sourceInfo:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：显式文件unit/frame/layout，输出传感器原槽Profile，NaN掩码与真实sourceId不伪造另一相机。
- 目标契约：显式文件unit/frame/layout，输出传感器原槽Profile，NaN掩码与真实sourceId不伪造另一相机。
- 迁移前置：M1；M3
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：中文路径/缺文件/显式m与mm、sensor与measurement布局；NaN/raw1280槽及通道只读，物理sourceId按实际文件。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/io.cpp:173` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 080 · gap.load_template

- 当前：`solution-gap` / `2.0.0` / 模板轮廓读取。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_template.cpp:1112](../../solutions/gap/ops/profile_template.cpp#L1112)。
- 端口：无 → `templates:Bundle<gap.TemplatePair.v2>`, `left:Profile`, `right:Profile`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：solution-gap + 可选gap-template/PCL模板ICP；不要求独立matching-cpu模块，复用Profile/Geometry内容及来源契约。
- 当前契约：显式单位/帧与真实模板来源输出TemplatePair.v2；Profile原槽与native字段支持ROI backdrop。
- 目标契约：显式单位/帧与真实模板来源输出TemplatePair.v2；Profile原槽与native字段支持ROI backdrop。
- 迁移前置：M1；M3
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：真实模板路径/m与mm/NaN和通道；TemplatePair.v2字段及上下文，legacy裸云输入需显式整图升级。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap + 可选gap-template/PCL模板ICP；不要求独立matching-cpu模块，复用Profile/Geometry内容及来源契约。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/io.cpp:282` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 081 · gap.make_roi_set

- 当前：`solution-gap` / `2.0.0` / 角色区域集组装。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_bundle_ops.cpp:190](../../solutions/gap/ops/profile_bundle_ops.cpp#L190)。
- 端口：`datum:Box2D`, `datumContext:Record`, `target:Box2D`, `targetContext:Record`, `seamLeft:Box2D`, `seamLeftContext:Record`, `seamRight:Box2D`, `seamRightContext:Record`, `info:Record?`, `overall:Box2D?`, `overallContext:Record?`, `cropStatus:Record?` → `rois:Bundle<gap.RoiSet.v2>`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：四角色Box及各v2内容上下文组装RoiSet.v2，拒绝错空间或来源不相交。
- 目标契约：四角色Box及各v2内容上下文组装RoiSet.v2，拒绝错空间或来源不相交。
- 迁移前置：M1；M3
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：四角色米制Box与v2上下文成对，source集合和geometry hash严格；错框/错空间/超预算失败原子。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/bundle_ops.cpp:134` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 082 · gap.make_scan_pair

- 当前：`solution-gap` / `2.0.0` / 原生轮廓对组装。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_bundle_ops.cpp:72](../../solutions/gap/ops/profile_bundle_ops.cpp#L72)。
- 端口：`sensorPrimary:Profile`, `sensorSecondary:Profile`, `primary:Profile`, `secondary:Profile`, `merged:Profile`, `mergedProvenance:Record`, `info:Record?`, `sourceInfo:Record?`, `guardQuality:Record?`, `cropStatus:Record?`, `primaryProvenance:Record?`, `secondaryProvenance:Record?` → `scan:Bundle<gap.ScanPair.v2>`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：严格七字段ScanPair.v2，保传感器/量测/merged及来源和真实处理状态，不接受伪造阶段。
- 目标契约：严格七字段ScanPair.v2，保传感器/量测/merged及来源和真实处理状态，不接受伪造阶段。
- 迁移前置：M1；M3
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：七字段不嵌套、真实sensor→measurement变换和merged来源重算一致；错guard/阶段/64KiB预算拒绝。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/bundle_ops.cpp:85` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 083 · gap.overall_roi

- 当前：`solution-gap` / `2.0.0` / 原生整体范围构建。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_regions.cpp:450](../../solutions/gap/ops/profile_regions.cpp#L450)。
- 端口：`primary:Profile`, `secondary:Profile`, `primaryProvenance:Record?`, `secondaryProvenance:Record?` → `box:Box2D`, `context:Record`, `status:Record`。
- 参数单位标注：roi=mm。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：固定或双路有效中心整体ROI；原毫米参数转米几何，严格联合原生Profile与参考来源。
- 目标契约：固定或双路有效中心整体ROI；原毫米参数转米几何，严格联合原生Profile与参考来源。
- 迁移前置：M1；M3
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：fixed/auto_center及左右/双路中心与原float规则对照；mm/m和显式平面、空/错空间/取消不发布。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/geometry.cpp:332` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 084 · gap.align_template

- 当前：`solution-gap` / `2.0.0` / 模板配准。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_template.cpp:1140](../../solutions/gap/ops/profile_template.cpp#L1140)。
- 端口：`scan:Bundle<gap.ScanPair.v2>`, `templates:Bundle<gap.TemplatePair.v2>` → `alignment:Bundle<gap.Alignment.v2>`。
- 参数单位标注：maxMatchingDist=mm, maxFitnessDist=mm, trustTranslation=mm, trustRotation=deg, roiFlushBase=mm, roiGapLeft=mm, roiFlushRef=mm, roiGapRight=mm。
- 依赖：solution-gap + 可选gap-template/PCL模板ICP；不要求独立matching-cpu模块，复用Profile/Geometry内容及来源契约。
- 当前契约：TemplatePair.v2和原生scan按唯一模板配准算法输出Alignment.v2；保候选与前后变换语义，不制造采样来源。
- 目标契约：TemplatePair.v2和原生scan按唯一模板配准算法输出Alignment.v2；保候选与前后变换语义，不制造采样来源。
- 迁移前置：gap.load_template；gap.overall_roi
- 关联算子：gap.load_template；gap.overall_roi；register.icp_2d
- 验收：真实模板ICP候选/阶段变换与方向；错frame/hash/输入只读，ROI backdrop native Profile字段路径验证。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap + 可选gap-template/PCL模板ICP；不要求独立matching-cpu模块，复用Profile/Geometry内容及来源契约。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/align.cpp:339` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 085 · gap.corner_vertex

- 当前：`solution-gap` / `2.0.0` / 原生轮廓夹角接缝测量。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_inspection.cpp:316](../../solutions/gap/ops/profile_inspection.cpp#L316)。
- 端口：`lineLeft:Line2D`, `lineLeftContext:Record`, `lineRight:Line2D`, `lineRightContext:Record`, `baseLine:Line2D?`, `baseLineContext:Record?`, `origin:Point2D`, `originContext:Record` → `vertex:Point2D`, `vertexContext:Record`, `gap:Measurement`, `flush:Measurement`, `angle:Measurement`, `segment:Line2D`, `segmentContext:Record`, `quality:Record`。
- 参数单位标注：gapOffset=mm, flushOffset=mm, minAngle=deg, maxAngle=deg。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：原生Profile角点两侧拟合与交点沿用唯一旧核；origin须Point及内容上下文，拒绝隐式Alignment Record猜空间。
- 目标契约：原生Profile角点两侧拟合与交点沿用唯一旧核；origin须Point及内容上下文，拒绝隐式Alignment Record猜空间。
- 迁移前置：M1；M2；gap.align_template
- 关联算子：gap.align_template
- 验收：60°与120°、近平行/越界和origin显式Point上下文；隐式Alignment Record不再猜单位/空间。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/measure.cpp:341` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 086 · gap.select_alignment

- 当前：`solution-gap` / `2.0.0` / 配准结果选择。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_template.cpp:1169](../../solutions/gap/ops/profile_template.cpp#L1169)。
- 端口：`a:Bundle<gap.Alignment.v2>`, `b:Bundle<gap.Alignment.v2>?`, `c:Bundle<gap.Alignment.v2>?`, `d:Bundle<gap.Alignment.v2>?` → `alignment:Bundle<gap.Alignment.v2>`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：solution-gap + 可选gap-template/PCL模板ICP；不要求独立matching-cpu模块，复用Profile/Geometry内容及来源契约。
- 当前契约：严格Alignment.v2候选选择，分数/变换/实际来源整体选取，失败不发布另一分支几何。
- 目标契约：严格Alignment.v2候选选择，分数/变换/实际来源整体选取，失败不发布另一分支几何。
- 迁移前置：gap.align_template
- 关联算子：gap.align_template
- 验收：候选可用性/分数及阈值边界、winner对应变换和来源，失败候选不能留另侧成功的错来源字段。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap + 可选gap-template/PCL模板ICP；不要求独立matching-cpu模块，复用Profile/Geometry内容及来源契约。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/align.cpp:426` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 087 · gap.business_rois

- 当前：`solution-gap` / `2.0.0` / 业务区域定位。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_template.cpp:1198](../../solutions/gap/ops/profile_template.cpp#L1198)。
- 端口：`alignment:Bundle<gap.Alignment.v2>` → `rois:Bundle<gap.RoiSet.v2>`, `flushBase:Box2D`, `flushBaseContext:Record`, `gapLeft:Box2D`, `gapLeftContext:Record`, `flushRef:Box2D`, `flushRefContext:Record`, `gapRight:Box2D`, `gapRightContext:Record`, `seam:Box2D`, `seamContext:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：solution-gap + 可选gap-template/PCL模板ICP；不要求独立matching-cpu模块，复用Profile/Geometry内容及来源契约。
- 当前契约：实际模板配准和量测来源构造四角色米制框，完整内容绑定；参考变换不冒充观测。
- 目标契约：实际模板配准和量测来源构造四角色米制框，完整内容绑定；参考变换不冒充观测。
- 迁移前置：gap.select_alignment；M2
- 关联算子：gap.select_alignment
- 验收：实际alignment+Profile完整父来源交集、模板移动额外来源；错框hash/错空间/超64来源明确拒绝。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap + 可选gap-template/PCL模板ICP；不要求独立matching-cpu模块，复用Profile/Geometry内容及来源契约。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/geometry.cpp:386` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 088 · gap.datum_window

- 当前：`solution-gap` / `2.0.0` / 原生方向基准范围构建。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_regions.cpp:482](../../solutions/gap/ops/profile_regions.cpp#L482)。
- 端口：`anchor:Box2D`, `anchorContext:Record`, `heightAnchor:Box2D?`, `heightContext:Record?` → `box:Box2D`, `context:Record`, `status:Record`。
- 参数单位标注：startMm=mm, lengthMm=mm, heightMm=mm。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：保基准范围偏移/窗口定义，米制Box与严格来源上下文；不凭坐标猜平面。
- 目标契约：保基准范围偏移/窗口定义，米制Box与严格来源上下文；不凭坐标猜平面。
- 迁移前置：M1；M2；gap.business_rois
- 关联算子：gap.business_rois
- 验收：方向窗口原边界和偏置、米制ROI内容绑定；无有效样本/帧错配/引用来源缺失及取消明确失败。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/geometry.cpp:277` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 089 · gap.split_roi_set

- 当前：`solution-gap` / `2.0.0` / 角色区域集解包。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_bundle_ops.cpp:291](../../solutions/gap/ops/profile_bundle_ops.cpp#L291)。
- 端口：`rois:Bundle<gap.RoiSet.v2>` → `datum:Box2D`, `datumContext:Record`, `target:Box2D`, `targetContext:Record`, `seamLeft:Box2D`, `seamLeftContext:Record`, `seamRight:Box2D`, `seamRightContext:Record`, `info:Record`, `seam:Box2D`, `seamContext:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：严格读取四角色物理框和上下文；字段成对且绑定实际几何，不将框Record当采样身份。
- 目标契约：严格读取四角色物理框和上下文；字段成对且绑定实际几何，不将框Record当采样身份。
- 迁移前置：M1；M3
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：所有角色框与绑定上下文逐字段还原；不能以sourceId相同绕过完整ProfileContext不相交。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/bundle_ops.cpp:197` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 090 · gap.split_scan_pair

- 当前：`solution-gap` / `2.0.0` / 原生轮廓对解包。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_bundle_ops.cpp:290](../../solutions/gap/ops/profile_bundle_ops.cpp#L290)。
- 端口：`scan:Bundle<gap.ScanPair.v2>` → `sensorPrimary:Profile`, `sensorSecondary:Profile`, `primary:Profile`, `secondary:Profile`, `merged:Profile`, `mergedProvenance:Record`, `info:Record`, `primaryProvenance:Record`, `secondaryProvenance:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：严格读取ScanPair.v2再拆分，原生Profile及各角色来源和guard/crop声明必须与内容一致。
- 目标契约：严格读取ScanPair.v2再拆分，原生Profile及各角色来源和guard/crop声明必须与内容一致。
- 迁移前置：M1；M3
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：通过strict reader证明原字段、来源与通道共享；伪造派生源/错hash/错误阶段及取消不发布。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/bundle_ops.cpp:111` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 091 · gap.profile_tensor

- 当前：`solution-gap` / `2.0.0` / 原生剖面特征张量构建。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_model_ops.cpp:333](../../solutions/gap/ops/profile_model_ops.cpp#L333)。
- 端口：`primary:Profile`, `secondary:Profile` → `tensor:Tensor`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：按ScanPair.v2实际sensor raw1280槽构建张量，保模型特征顺序和掩码，不使用压缩measurement槽冒充。
- 目标契约：按ScanPair.v2实际sensor raw1280槽构建张量，保模型特征顺序和掩码，不使用压缩measurement槽冒充。
- 迁移前置：M1；M3；profile.from_cloud；profile.remap_axes
- 关联算子：gap.to_measurement_frame
- 验收：两路1280槽/NaN/有限无效mask与特征通道顺序；raw槽被压缩、非sensor空间或错误来源拒绝。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/model_roi.cpp:505` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 092 · gap.camera_guard

- 当前：`solution-gap` / `2.0.0` / 双相机轮廓一致性校验。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_guard.cpp:301](../../solutions/gap/ops/profile_guard.cpp#L301)。
- 端口：`primary:Profile`, `secondary:Profile`, `box:Box2D?`, `boxContext:Record?` → `primary:Profile`, `secondary:Profile`, `quality:Record`。
- 参数单位标注：maxDeltaMm=mm, sampleStep=mm, halfWindow=mm。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：显式Profile平面/单位的窗口中位重叠比较，保旧float步进、阈值和off/record/keep/fail；回退共享可信原轮廓身份。
- 目标契约：显式Profile平面/单位的窗口中位重叠比较，保旧float步进、阈值和off/record/keep/fail；回退共享可信原轮廓身份。
- 迁移前置：M1；M3；gap.profile_tensor
- 关联算子：gap.profile_tensor
- 验收：重叠窗口开边界、float步进、delta符号、阈值与少样本、keepPrimary/Secondary身份、预算停滞/非有限参数拒绝。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/camera_guard.cpp:213` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 093 · gap.labels_from_logits

- 当前：`solution-gap` / `2.0.0` / 原生逐槽标签解码。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_model_ops.cpp:347](../../solutions/gap/ops/profile_model_ops.cpp#L347)。
- 端口：`tensor:Tensor`, `primary:Profile`, `secondary:Profile`, `modelInput:Tensor?` → `labels:Bundle<gap.LabelRows.v2>`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：模型张量严格解码为原槽LabelRows.v2，输出与实际sensor来源和logits内容绑定。
- 目标契约：模型张量严格解码为原槽LabelRows.v2，输出与实际sensor来源和logits内容绑定。
- 迁移前置：gap.profile_tensor
- 关联算子：gap.profile_tensor；ml.onnx_run
- 验收：实际logits形状/位深/类别范围及通道顺序、source sensor绑定和取消；无效数值不静默取类别0。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/model_roi.cpp:545` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 094 · gap.labels_to_cloud

- 当前：`solution-gap` / `2.0.0` / 原生标签诊断着色。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_model_ops.cpp:408](../../solutions/gap/ops/profile_model_ops.cpp#L408)。
- 端口：`scan:Bundle<gap.ScanPair.v2>`, `labels:Bundle<gap.LabelRows.v2>` → `cloud:PointCloud`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：按显式逻辑row对真实传感器槽诊断着色，严格标签来源；不得将逻辑相机名替换物理来源。
- 目标契约：按显式逻辑row对真实传感器槽诊断着色，严格标签来源；不得将逻辑相机名替换物理来源。
- 迁移前置：gap.labels_from_logits；profile.from_cloud；profile.remap_axes
- 关联算子：gap.labels_from_logits；gap.to_measurement_frame
- 验收：primary/secondary角色明确、重复坐标仍按原slot着色，完整sensor来源/标签hash校验，不改原云。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/model_roi.cpp:643` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 095 · gap.read_scan

- 当前：`solution-gap` / `2.0.0` / 双路原生场景读入。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_scan.cpp:476](../../solutions/gap/ops/profile_scan.cpp#L476)。
- 端口：`primary:Profile?`, `secondary:Profile?`, `sourceInfo:Record?`, `guardQuality:Record?` → `scan:Bundle<gap.ScanPair.v2>`, `sensorPrimary:Profile`, `sensorSecondary:Profile`。
- 参数单位标注：outlierRadiusMm=mm。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：原生ScanPair.v2保sensor与measurement双视图、实际guard/crop/origin证据；显式轴变换和m工作单位。
- 目标契约：原生ScanPair.v2保sensor与measurement双视图、实际guard/crop/origin证据；显式轴变换和m工作单位。
- 迁移前置：gap.load_profile_pair；profile.from_cloud；profile.remap_axes；gap.make_scan_pair
- 关联算子：gap.load_profile_pair；gap.to_measurement_frame；gap.make_scan_pair
- 验收：原sensor计数和当前选槽计数分开；guard实际来源、crop状态及角色provenance严格绑定，错元数据/取消失败原子。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/blocks.cpp:755` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 096 · gap.locate_template

- 当前：`solution-gap` / `3.0.0` / 模板区域定位。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_locate_template.cpp:355](../../solutions/gap/ops/profile_locate_template.cpp#L355)。
- 端口：`scan:Bundle<gap.ScanPair.v2>` → `scan:Bundle<gap.ScanPair.v2>`, `rois:Bundle<gap.RoiSet.v2>`, `alignment:Bundle<gap.Alignment.v2>`。
- 参数单位标注：overallRoi=mm, template1DatumRoi=mm, template1TargetRoi=mm, template1SeamLeftRoi=mm, template1SeamRightRoi=mm, template2DatumRoi=mm, template2TargetRoi=mm, template2SeamLeftRoi=mm, template2SeamRightRoi=mm, template3DatumRoi=mm, template3TargetRoi=mm, template3SeamLeftRoi=mm, template3SeamRightRoi=mm, template4DatumRoi=mm, template4TargetRoi=mm, template4SeamLeftRoi=mm, template4SeamRightRoi=mm, maxMatchingDist=mm, maxFitnessDist=mm, trustTranslation=mm, trustRotation=deg。
- 依赖：solution-gap + 可选gap-template/PCL模板ICP；不要求独立matching-cpu模块，复用Profile/Geometry内容及来源契约。
- 当前契约：模板定位v3消费原生TemplatePair/ScanPair，保原ICP与候选策略并输出原子Location可组装的scan/rois。
- 目标契约：模板定位v3消费原生TemplatePair/ScanPair，保原ICP与候选策略并输出原子Location可组装的scan/rois。
- 迁移前置：gap.align_template；gap.select_alignment；gap.business_rois；gap.read_scan
- 关联算子：gap.align_template；gap.select_alignment；gap.business_rois；gap.read_scan
- 验收：高层与细图模板定位同结果；native TemplatePair与显式旧图转换，局部错误不能混合scan和另一定位rois。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap + 可选gap-template/PCL模板ICP；不要求独立matching-cpu模块，复用Profile/Geometry内容及来源契约。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/blocks.cpp:836` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 097 · gap.roi_from_labels

- 当前：`solution-gap` / `2.0.0` / 原生标签区域提取。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_model_ops.cpp:371](../../solutions/gap/ops/profile_model_ops.cpp#L371)。
- 端口：`scan:Bundle<gap.ScanPair.v2>`, `labels:Bundle<gap.LabelRows.v2>`, `backdrop:PointCloud?` → `rois:Bundle<gap.RoiSet.v2>`, `flushBase:Box2D`, `flushBaseContext:Record`, `flushRef:Box2D`, `flushRefContext:Record`, `gapLeft:Box2D`, `gapLeftContext:Record`, `gapRight:Box2D`, `gapRightContext:Record`, `seam:Box2D`, `seamContext:Record`, `refinements:Record`, `backdrop:PointCloud`。
- 参数单位标注：splitStepMm=mm, linkMm=mm, anchorGapMm=mm。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：LabelRows.v2必须匹配实际ScanPair；保连通片/锚点细化与缺类失败，输出RoiSet.v2和逐框上下文。
- 目标契约：LabelRows.v2必须匹配实际ScanPair；保连通片/锚点细化与缺类失败，输出RoiSet.v2和逐框上下文。
- 迁移前置：gap.profile_tensor；gap.labels_from_logits；profile.from_cloud；profile.remap_axes
- 关联算子：gap.profile_tensor；gap.labels_from_logits；gap.to_measurement_frame
- 验收：四角色类别缺失、连通片/refine开关/锚点与原算法对照，标签source错配拒绝且不发布部分框。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/model_roi.cpp:576` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 098 · gap.roll_anchored_crop

- 当前：`solution-gap` / `2.0.0` / 原生零件随动裁剪。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_regions.cpp:511](../../solutions/gap/ops/profile_regions.cpp#L511)。
- 端口：`scan:Bundle<gap.ScanPair.v2>`, `gapLeft:Box2D`, `gapLeftContext:Record`, `gapRight:Box2D`, `gapRightContext:Record` → `scan:Bundle<gap.ScanPair.v2>`, `window:Box2D`, `windowContext:Record`, `status:Record`, `primaryProvenance:Record`, `secondaryProvenance:Record`。
- 参数单位标注：halfWidth=mm, halfHeight=mm, maxRollBoxHeight=mm。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：按角色锚点构造随动框并复用公共cropProfile选择，保sensor原槽与复合来源，不复制裁剪算法。
- 目标契约：按角色锚点构造随动框并复用公共cropProfile选择，保sensor原槽与复合来源，不复制裁剪算法。
- 迁移前置：M3；gap.roi_from_labels
- 关联算子：gap.roi_from_labels
- 验收：左右零件锚点与缺段语义、开闭ROI选择和raw槽；裁剪前后实际状态、通道/源索引及完整引用来源保留。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/model_roi.cpp:716` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 099 · gap.locate_model

- 当前：`solution-gap` / `2.0.0` / 原生分割模型定位。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_model_ops.cpp:456](../../solutions/gap/ops/profile_model_ops.cpp#L456)。
- 端口：`scan:Bundle<gap.ScanPair.v2>` → `rois:Bundle<gap.RoiSet.v2>`, `scan:Bundle<gap.ScanPair.v2>`, `labels:Bundle<gap.LabelRows.v2>`, `refinements:Record`。
- 参数单位标注：splitStepMm=mm, linkMm=mm, anchorGapMm=mm, cropHalfWidth=mm, cropHalfHeight=mm, cropMaxRollBoxHeight=mm。
- 依赖：solution-gap + gap-model显式ORT/ML会话；Profile原槽适配与类别几何无需OpenCV，不经Registry执行另一包。
- 当前契约：同一typed张量/推理/标签细化/随动裁剪链输出scan与rois；模型失败闭包由图显式原子回退。
- 目标契约：同一typed张量/推理/标签细化/随动裁剪链输出scan与rois；模型失败闭包由图显式原子回退。
- 迁移前置：gap.profile_tensor；gap.labels_from_logits；gap.roi_from_labels；gap.roll_anchored_crop
- 关联算子：gap.profile_tensor；gap.labels_from_logits；gap.roi_from_labels；gap.roll_anchored_crop
- 验收：真实ONNX夹具、原槽类别与四框数值及fine图一致；缺段/推理失败保持失败闭包，不伪造模型ready。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap + gap-model显式ORT/ML会话；Profile原槽适配与类别几何无需OpenCV，不经Registry执行另一包。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/blocks.cpp:958` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 106 · gap.fit_gap_circles

- 当前：`solution-gap` / `2.0.0` / 原生轮廓间隙双侧圆拟合。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_circles.cpp:286](../../solutions/gap/ops/profile_circles.cpp#L286)。
- 端口：`merged:Profile`, `primary:Profile`, `secondary:Profile`, `mergedProvenance:Record`, `primaryProvenance:Record?`, `secondaryProvenance:Record?`, `boxLeft:Box2D`, `boxLeftContext:Record`, `boxRight:Box2D`, `boxRightContext:Record`, `refLine:Line2D?`, `refLineContext:Record?`, `refLineRight:Line2D?`, `refLineRightContext:Record?` → `left:Circle2D`, `leftContext:Record`, `leftProfile:Profile`, `leftProvenance:Record`, `leftInliers:Indices`, `right:Circle2D`, `rightContext:Record`, `rightProfile:Profile`, `rightProvenance:Record`, `rightInliers:Indices`, `quality:Record`。
- 参数单位标注：distThresh=mm, retryDistance=mm, nominal=mm, offset=mm, leftRadiusValue=mm, leftRadiusMin=mm, leftRadiusMax=mm, leftRadiusPrior=mm, leftRadiusPriorSigma=mm, rightRadiusValue=mm, rightRadiusMin=mm, rightRadiusMax=mm, rightRadiusPrior=mm, rightRadiusPriorSigma=mm, leftCenterAbove=mm, leftCenterTol=mm, rightCenterAbove=mm, rightCenterTol=mm, leftTangentAbove=mm, leftTangentTol=mm, rightTangentAbove=mm, rightTangentTol=mm, leftArcBearingDeg=°, leftArcBearingTolDeg=°, rightArcBearingDeg=°, rightArcBearingTolDeg=°, leftMinArcDeg=°, rightMinArcDeg=°。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：保双相机优先/回退、弱重试、nominal/prior/center/tangent候选策略；并行slot追踪输出实际轮廓/源索引与米制圆上下文。
- 目标契约：保双相机优先/回退、弱重试、nominal/prior/center/tangent候选策略；并行slot追踪输出实际轮廓/源索引与米制圆上下文。
- 迁移前置：M1；M2；M3；profile.fit_line_robust
- 关联算子：gap.fit_line
- 验收：独立ref线高度带、guard内外/角带、弱地板/先验/双相机重试与nominal选云，原槽/重复点来源及参数前置拒绝。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/fit.cpp:1157` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 107 · gap.role_line

- 当前：`solution-gap` / `2.0.0` / 角色区域直线拟合。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_measurement_ops.cpp:377](../../solutions/gap/ops/profile_measurement_ops.cpp#L377)。
- 端口：`scan:Bundle<gap.ScanPair.v2>`, `rois:Bundle<gap.RoiSet.v2>`, `refLine:Line2D?`, `refLineContext:Record?` → `feature:Bundle<gap.LineFeature.v2>`, `line:Line2D`, `lineContext:Record`, `innerEnd:Point2D`, `pointContext:Record`, `profile:Profile`, `provenance:Record`, `quality:Record`, `inliers:Indices`, `weightedInliers:Indices`。
- 参数单位标注：distThresh=mm, dirNominalDeg=°, dirTolDeg=°。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：角色ROI选真实量测Profile并直接复用公共typed稳健直线；线/端点/质量/来源组成LineFeature.v2原子输出。
- 目标契约：角色ROI选真实量测Profile并直接复用公共typed稳健直线；线/端点/质量/来源组成LineFeature.v2原子输出。
- 迁移前置：profile.fit_line_robust；gap.make_roi_set；gap.make_scan_pair
- 关联算子：gap.fit_line；gap.make_roi_set；gap.make_scan_pair
- 验收：free/axis与toward近段÷3重拟合、free/band/fixed及重复权重；feature/quality/geometry来源不可分开回退。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/blocks.cpp:1019` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 108 · gap.seam_circles

- 当前：`solution-gap` / `2.0.0` / 缝隙双侧圆拟合。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_measurement_ops.cpp:377](../../solutions/gap/ops/profile_measurement_ops.cpp#L377)。
- 端口：`scan:Bundle<gap.ScanPair.v2>`, `rois:Bundle<gap.RoiSet.v2>`, `refLine:Line2D?`, `refLineContext:Record?`, `refLineRight:Line2D?`, `refLineRightContext:Record?` → `left:Circle2D`, `leftContext:Record`, `leftProfile:Profile`, `leftProvenance:Record`, `leftInliers:Indices`, `right:Circle2D`, `rightContext:Record`, `rightProfile:Profile`, `rightProvenance:Record`, `rightInliers:Indices`, `quality:Record`, `feature:Bundle<gap.CirclePair.v2>`。
- 参数单位标注：distThresh=mm, retryDistance=mm, nominal=mm, offset=mm, leftRadiusValue=mm, leftRadiusMin=mm, leftRadiusMax=mm, leftRadiusPrior=mm, leftRadiusPriorSigma=mm, rightRadiusValue=mm, rightRadiusMin=mm, rightRadiusMax=mm, rightRadiusPrior=mm, rightRadiusPriorSigma=mm, leftCenterAbove=mm, leftCenterTol=mm, rightCenterAbove=mm, rightCenterTol=mm, leftTangentAbove=mm, leftTangentTol=mm, rightTangentAbove=mm, rightTangentTol=mm, leftArcBearingDeg=°, leftArcBearingTolDeg=°, rightArcBearingDeg=°, rightArcBearingTolDeg=°, leftMinArcDeg=°, rightMinArcDeg=°。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：严格scan/rois直接调用唯一场景双圆helper，保独立两侧参考约束和实际来源，CirclePair.v2整体输出。
- 目标契约：严格scan/rois直接调用唯一场景双圆helper，保独立两侧参考约束和实际来源，CirclePair.v2整体输出。
- 迁移前置：gap.fit_gap_circles；gap.make_roi_set
- 关联算子：gap.fit_gap_circles；gap.make_roi_set
- 验收：高层与细粒度唯一候选算法相同；双侧actual来源与CirclePair原子，单次失败不得暗增圆回退策略。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/blocks.cpp:1131` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 109 · gap.datum_direction

- 当前：`solution-gap` / `2.0.0` / 方向基准线提取。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_measurement_ops.cpp:377](../../solutions/gap/ops/profile_measurement_ops.cpp#L377)。
- 端口：`scan:Bundle<gap.ScanPair.v2>`, `rois:Bundle<gap.RoiSet.v2>` → `feature:Bundle<gap.LineFeature.v2>`, `line:Line2D`, `lineContext:Record`, `innerEnd:Point2D`, `pointContext:Record`, `profile:Profile`, `provenance:Record`, `quality:Record`, `inliers:Indices`, `weightedInliers:Indices`, `window:Box2D`, `windowContext:Record`。
- 参数单位标注：distThresh=mm, startMm=mm, lengthMm=mm, heightMm=mm。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：方向基准角色范围与同一公共typed直线，保持约束/中位回退策略并输出原子LineFeature.v2。
- 目标契约：方向基准角色范围与同一公共typed直线，保持约束/中位回退策略并输出原子LineFeature.v2。
- 迁移前置：gap.datum_window；profile.fit_line_robust；gap.business_rois
- 关联算子：gap.datum_window；gap.fit_line；gap.business_rois
- 验收：方向约束、中位fallback、无样本与最少内点阈值保旧；窗口ROI/线/端点来源及取消原子。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/blocks.cpp:1181` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 112 · gap.groove_joint

- 当前：`solution-gap` / `2.0.0` / 原生轮廓槽形对接缝测量。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_inspection.cpp:365](../../solutions/gap/ops/profile_inspection.cpp#L365)。
- 端口：`primary:Profile`, `secondary:Profile`, `primaryProvenance:Record?`, `secondaryProvenance:Record?` → `gap:Measurement`, `flush:Measurement`, `baseLine:Line2D`, `refLine:Line2D`, `gapSegment:Line2D`, `flushSegment:Line2D`, `groove:Point2D`, `baseLineContext:Record`, `refLineContext:Record`, `gapSegmentContext:Record`, `flushSegmentContext:Record`, `pointContext:Record`, `quality:Record`, `qualityBase:Record`, `qualityRef:Record`。
- 参数单位标注：grooveDepthMin=mm, leftNear=mm, leftFar=mm, rightNear=mm, rightFar=mm, lineDistThresh=mm, gapDepth=mm, gapOffset=mm, flushOffset=mm。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：原生Profile明确分析平面，保槽口左右斜线/夹角/近平行及越界规则、毫米深度与面差符号；输出米制几何上下文。
- 目标契约：原生Profile明确分析平面，保槽口左右斜线/夹角/近平行及越界规则、毫米深度与面差符号；输出米制几何上下文。
- 迁移前置：M3；profile.fit_line_robust；metrology.flush_point_line
- 关联算子：gap.fit_line；gap.flush
- 验收：斜面夹角/近平行/交点越界、深度0合法、levelDepth/offset及相机遮挡/面差符号原fixture对照。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/groove.cpp:458` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 113 · gap.notch_width

- 当前：`solution-gap` / `2.0.0` / 原生轮廓V形缝测量。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_inspection.cpp:365](../../solutions/gap/ops/profile_inspection.cpp#L365)。
- 端口：`primary:Profile`, `secondary:Profile`, `primaryProvenance:Record?`, `secondaryProvenance:Record?` → `gap:Measurement`, `flush:Measurement`, `baseLine:Line2D`, `refLine:Line2D`, `gapSegment:Line2D`, `flushSegment:Line2D`, `anchor:Point2D`, `baseLineContext:Record`, `refLineContext:Record`, `gapSegmentContext:Record`, `flushSegmentContext:Record`, `pointContext:Record`, `quality:Record`, `qualityBase:Record`, `qualityRef:Record`。
- 参数单位标注：levelDepth=mm, baseNear=mm, baseFar=mm, lineDistThresh=mm, gapOffset=mm, refNear=mm, refFar=mm, flushOffset=mm。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：保无槽/浅V/levelDepth/offset/相机遮挡原策略；参考拟合失败仍保有效gap与invalid flush，显示几何上下文明确Unavailable。
- 目标契约：保无槽/浅V/levelDepth/offset/相机遮挡原策略；参考拟合失败仍保有效gap与invalid flush，显示几何上下文明确Unavailable。
- 迁移前置：M3；profile.fit_line_robust；metrology.flush_point_line
- 关联算子：gap.fit_line；gap.flush
- 验收：无槽/浅V/深度0/offset/遮挡、参考失败仍保gap与invalid flush；真实executor拒绝Unavailable线作测量。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/notch.cpp:631` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 115 · gap.judge

- 当前：`solution-gap` / `1.0.0` / 间隙面差判定。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/measure.cpp:443](../../solutions/gap/ops/measure.cpp#L443)。
- 端口：`value:Measurement` → `value:Measurement`。
- 参数单位标注：nominal=mm, upper=mm, lower=mm, margin=mm。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：保既有Measurement上下限与无效判定，不把未知/invalid测量变为业务合格；只迁包不改算子契约。
- 目标契约：保既有Measurement上下限与无效判定，不把未知/invalid测量变为业务合格；只迁包不改算子契约。
- 迁移前置：M1；metrology.flush_point_line；metrology.circle_gap
- 关联算子：gap.flush；gap.gap
- 验收：既有上下界/边界等于/valid=false Measurement判定和输出单位不变；不将未知状态显示为合格。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/measure.cpp:421` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 116 · gap.result_bundle

- 当前：`solution-gap` / `3.0.0` / 原生间隙检测结果汇总。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/result_bundle.cpp:615](../../solutions/gap/ops/result_bundle.cpp#L615)。
- 端口：`gap:Measurement`, `flush:Measurement?`, `rois:Bundle<gap.RoiSet.v2>?`, `scan:Bundle<gap.ScanPair.v2>?`, `baseFeature:Bundle<gap.LineFeature.v2>?`, `refFeature:Bundle<gap.ReferenceFeature.v2>?`, `circleFeature:Bundle<gap.CirclePair.v2>?`, `roiOverall:Box2D?`, `roiOverallContext:Record?`, `fits:Record?`, `fitBase:Record?`, `fitRef:Record?`, `cropStatus:Record?`, `fallback:Record?`, `fallbackLine:Record?`, `fallbackReference:Record?`, `fallbackCircles:Record?`, `alignment:Bundle<gap.Alignment.v2>?` → `bundle:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：schemaVersion3严格原生Scan/Roi/Feature质量及局部FallbackChoice；m/mm显式转CSV毫米，未知计数/质量用null而非伪造0ready。
- 目标契约：schemaVersion3严格原生Scan/Roi/Feature质量及局部FallbackChoice；m/mm显式转CSV毫米，未知计数/质量用null而非伪造0ready。
- 迁移前置：gap.make_scan_pair；gap.make_roi_set；gap.judge
- 关联算子：gap.make_scan_pair；gap.make_roi_set；gap.judge
- 验收：旧CSVweighted内点口径、SI残差转mm与新增unique字段、局部fallback证据、未知计数null和invalid flush；完整executor输出。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/result_bundle.cpp:398` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 119 · gap.ref_point

- 当前：`solution-gap` / `2.0.0` / 参考点提取。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_measurement_ops.cpp:377](../../solutions/gap/ops/profile_measurement_ops.cpp#L377)。
- 端口：`scan:Bundle<gap.ScanPair.v2>`, `rois:Bundle<gap.RoiSet.v2>`, `baseLine:Line2D?`, `baseLineContext:Record?` → `feature:Bundle<gap.ReferenceFeature.v2>`, `line:Line2D`, `lineContext:Record`, `point:Point2D`, `pointContext:Record`, `profile:Profile`, `provenance:Record`, `quality:Record`, `selection:Record`。
- 参数单位标注：distThresh=mm。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：line_end/selected_point/nearest_point保旧选点与来源语义，复用公共typed核；非拟合线显式Unavailable上下文。
- 目标契约：line_end/selected_point/nearest_point保旧选点与来源语义，复用公共typed核；非拟合线显式Unavailable上下文。
- 迁移前置：gap.role_line；geometry.box_anchor；profile.bind_geometry；profile.nearest_sample；profile.nearest_to_line
- 关联算子：gap.role_line；gap.selected_point；gap.nearest_to_line
- 验收：三种方法原slot/等距顺序和线段外最近点；nonfit placeholder的Unavailable context必须被度量consumer拒绝。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/blocks.cpp:1074` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。

## 124 · gap.infer_profile

- 当前：`solution-gap` / `1.0.0` / 原生剖面模型推理。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_model_ops.cpp:429](../../solutions/gap/ops/profile_model_ops.cpp#L429)。
- 端口：`input:Tensor` → `output:Tensor`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：solution-gap + gap-model显式ORT/ML会话；Profile原槽适配与类别几何无需OpenCV，不经Registry执行另一包。
- 当前契约：严格ScanPair.v2的实际两路1280传感器原槽构建特征张量并调用共享ML会话，返回绑定原槽与模型输出的LabelRows.v2。
- 目标契约：严格ScanPair.v2的实际两路1280传感器原槽构建特征张量并调用共享ML会话，返回绑定原槽与模型输出的LabelRows.v2。
- 迁移前置：M1；M2；M3
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：模型/布局/类型/类别预算错误和取消不发布；传感器来源、掩码及raw槽不因选点压缩；不以模型成功表示现场ready。
- 说明：M4c显式公共边界或场景职责；不新增旧ID薄别名，不声称整个M4或后续阶段已经验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap + gap-model显式ORT/ML会话；Profile原槽适配与类别几何无需OpenCV，不经Registry执行另一包。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。

## 125 · gap.make_line_feature

- 当前：`solution-gap` / `1.0.0` / 组装line_feature。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_feature_bundle.cpp:644](../../solutions/gap/ops/profile_feature_bundle.cpp#L644)。
- 端口：`profile:Profile`, `line:Line2D`, `lineContext:Record`, `innerEnd:Point2D`, `pointContext:Record`, `inliers:Indices`, `weightedInliers:Indices`, `quality:Record`, `provenance:Record` → `feature:Bundle<gap.LineFeature.v2>`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：LineFeature.v2九字段原子绑定轮廓、线/端点及上下文、唯一/加权内点、质量和来源；整体fallback避免混源。
- 目标契约：LineFeature.v2九字段原子绑定轮廓、线/端点及上下文、唯一/加权内点、质量和来源；整体fallback避免混源。
- 迁移前置：M1；M2；M3
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：严格完整字段/geometry hash/空间/来源/质量/索引校验，错误及取消不发布；真实executor整体回退及不兼容上下文拒绝。
- 说明：M4c显式公共边界或场景职责；不新增旧ID薄别名，不声称整个M4或后续阶段已经验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。

## 126 · gap.split_line_feature

- 当前：`solution-gap` / `1.0.0` / 拆分line_feature。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_feature_bundle.cpp:644](../../solutions/gap/ops/profile_feature_bundle.cpp#L644)。
- 端口：`feature:Bundle<gap.LineFeature.v2>` → `profile:Profile`, `line:Line2D`, `lineContext:Record`, `innerEnd:Point2D`, `pointContext:Record`, `inliers:Indices`, `weightedInliers:Indices`, `quality:Record`, `provenance:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：LineFeature.v2九字段原子绑定轮廓、线/端点及上下文、唯一/加权内点、质量和来源；整体fallback避免混源。
- 目标契约：LineFeature.v2九字段原子绑定轮廓、线/端点及上下文、唯一/加权内点、质量和来源；整体fallback避免混源。
- 迁移前置：M1；M2；M3
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：严格完整字段/geometry hash/空间/来源/质量/索引校验，错误及取消不发布；真实executor整体回退及不兼容上下文拒绝。
- 说明：M4c显式公共边界或场景职责；不新增旧ID薄别名，不声称整个M4或后续阶段已经验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。

## 127 · gap.make_reference_feature

- 当前：`solution-gap` / `1.0.0` / 组装reference_feature。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_feature_bundle.cpp:644](../../solutions/gap/ops/profile_feature_bundle.cpp#L644)。
- 端口：`profile:Profile`, `line:Line2D?`, `lineContext:Record?`, `point:Point2D`, `pointContext:Record`, `selection:Record`, `quality:Record?`, `provenance:Record` → `feature:Bundle<gap.ReferenceFeature.v2>`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：ReferenceFeature.v2八字段；line_end保真实拟合，选点方法统一生成Unavailable线上下文，不能伪装可测拟合线。
- 目标契约：ReferenceFeature.v2八字段；line_end保真实拟合，选点方法统一生成Unavailable线上下文，不能伪装可测拟合线。
- 迁移前置：M1；M2；M3
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：严格完整字段/geometry hash/空间/来源/质量/索引校验，错误及取消不发布；真实executor整体回退及不兼容上下文拒绝。
- 说明：M4c显式公共边界或场景职责；不新增旧ID薄别名，不声称整个M4或后续阶段已经验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。

## 128 · gap.split_reference_feature

- 当前：`solution-gap` / `1.0.0` / 拆分reference_feature。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_feature_bundle.cpp:644](../../solutions/gap/ops/profile_feature_bundle.cpp#L644)。
- 端口：`feature:Bundle<gap.ReferenceFeature.v2>` → `profile:Profile`, `line:Line2D`, `lineContext:Record`, `point:Point2D`, `pointContext:Record`, `selection:Record`, `quality:Record`, `provenance:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：ReferenceFeature.v2八字段；line_end保真实拟合，选点方法统一生成Unavailable线上下文，不能伪装可测拟合线。
- 目标契约：ReferenceFeature.v2八字段；line_end保真实拟合，选点方法统一生成Unavailable线上下文，不能伪装可测拟合线。
- 迁移前置：M1；M2；M3
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：严格完整字段/geometry hash/空间/来源/质量/索引校验，错误及取消不发布；真实executor整体回退及不兼容上下文拒绝。
- 说明：M4c显式公共边界或场景职责；不新增旧ID薄别名，不声称整个M4或后续阶段已经验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。

## 129 · gap.make_circle_pair

- 当前：`solution-gap` / `1.0.0` / 组装circle_pair。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_feature_bundle.cpp:644](../../solutions/gap/ops/profile_feature_bundle.cpp#L644)。
- 端口：`left:Circle2D`, `leftContext:Record`, `leftProfile:Profile`, `leftProvenance:Record`, `leftInliers:Indices`, `right:Circle2D`, `rightContext:Record`, `rightProfile:Profile`, `rightProvenance:Record`, `rightInliers:Indices`, `quality:Record` → `feature:Bundle<gap.CirclePair.v2>`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：CirclePair.v2十一字段保两侧实际选定轮廓、圆及上下文、来源与内点和共享质量，不逐字段回退混源。
- 目标契约：CirclePair.v2十一字段保两侧实际选定轮廓、圆及上下文、来源与内点和共享质量，不逐字段回退混源。
- 迁移前置：M1；M2；M3
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：严格完整字段/geometry hash/空间/来源/质量/索引校验，错误及取消不发布；真实executor整体回退及不兼容上下文拒绝。
- 说明：M4c显式公共边界或场景职责；不新增旧ID薄别名，不声称整个M4或后续阶段已经验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。

## 130 · gap.split_circle_pair

- 当前：`solution-gap` / `1.0.0` / 拆分circle_pair。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_feature_bundle.cpp:644](../../solutions/gap/ops/profile_feature_bundle.cpp#L644)。
- 端口：`feature:Bundle<gap.CirclePair.v2>` → `left:Circle2D`, `leftContext:Record`, `leftProfile:Profile`, `leftProvenance:Record`, `leftInliers:Indices`, `right:Circle2D`, `rightContext:Record`, `rightProfile:Profile`, `rightProvenance:Record`, `rightInliers:Indices`, `quality:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：CirclePair.v2十一字段保两侧实际选定轮廓、圆及上下文、来源与内点和共享质量，不逐字段回退混源。
- 目标契约：CirclePair.v2十一字段保两侧实际选定轮廓、圆及上下文、来源与内点和共享质量，不逐字段回退混源。
- 迁移前置：M1；M2；M3
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：严格完整字段/geometry hash/空间/来源/质量/索引校验，错误及取消不发布；真实executor整体回退及不兼容上下文拒绝。
- 说明：M4c显式公共边界或场景职责；不新增旧ID薄别名，不声称整个M4或后续阶段已经验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。

## 131 · gap.make_location

- 当前：`solution-gap` / `1.0.0` / 组装location。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_feature_bundle.cpp:644](../../solutions/gap/ops/profile_feature_bundle.cpp#L644)。
- 端口：`scan:Bundle<gap.ScanPair.v2>`, `rois:Bundle<gap.RoiSet.v2>`, `scanChoice:Record?`, `roiChoice:Record?`, `datumChoice:Record?`, `targetChoice:Record?`, `seamLeftChoice:Record?`, `seamRightChoice:Record?` → `location:Bundle<gap.Location.v2>`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：Location.v2扁平十六字段原子组装ScanPair.v2与RoiSet.v2；ROI必须引用实际完整量测来源，选择证据不同拒绝。
- 目标契约：Location.v2扁平十六字段原子组装ScanPair.v2与RoiSet.v2；ROI必须引用实际完整量测来源，选择证据不同拒绝。
- 迁移前置：M1；M2；M3
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：严格完整字段/geometry hash/空间/来源/质量/索引校验，错误及取消不发布；真实executor整体回退及不兼容上下文拒绝。
- 说明：M4c显式公共边界或场景职责；不新增旧ID薄别名，不声称整个M4或后续阶段已经验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。

## 132 · gap.split_location

- 当前：`solution-gap` / `1.0.0` / 拆分location。目标：`solution-gap`；保留场景职责；**M4**。
- 源码：[solutions/gap/ops/profile_feature_bundle.cpp:644](../../solutions/gap/ops/profile_feature_bundle.cpp#L644)。
- 端口：`location:Bundle<gap.Location.v2>` → `scan:Bundle<gap.ScanPair.v2>`, `rois:Bundle<gap.RoiSet.v2>`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 当前契约：Location.v2扁平十六字段原子组装ScanPair.v2与RoiSet.v2；ROI必须引用实际完整量测来源，选择证据不同拒绝。
- 目标契约：Location.v2扁平十六字段原子组装ScanPair.v2与RoiSet.v2；ROI必须引用实际完整量测来源，选择证据不同拒绝。
- 迁移前置：M1；M2；M3
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：严格完整字段/geometry hash/空间/来源/质量/索引校验，错误及取消不发布；真实executor整体回退及不兼容上下文拒绝。
- 说明：M4c显式公共边界或场景职责；不新增旧ID薄别名，不声称整个M4或后续阶段已经验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：solution-gap公开Profile/Geometry/Metrology和唯一场景typed helper；包必需gap-geometry/PCL，模型/模板/导入/兼容重依赖均独立可选，无OpenCV依赖。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。

## 277 · gap.measure_reference

- 当前：`solution-gap` / `1.0.0` / 参考基线测量。目标：`solution-gap`；保留场景职责；**M7**。
- 源码：[solutions/gap/ops/measure_reference.cpp:204](../../solutions/gap/ops/measure_reference.cpp#L204)。
- 端口：`primary:PointCloud`, `secondary:PointCloud` → `gap:Measurement`, `flush:Measurement`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：可选gap-compat历史MeasurementEngine，PCL/模板匹配/YAML/ONNX完整闭包仅显式兼容构建；不作为新公共算法入口。
- 当前契约：保既有reference流程声明与宿主契约，迁入solution-gap；不以M4c来源清单核验替代M6整体业务验收。
- 目标契约：保既有reference流程声明与宿主契约，迁入solution-gap；不以M4c来源清单核验替代M6整体业务验收。
- 迁移前置：M4；M6
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：仅显式gap-compat路径保旧黑盒入口/参数/传感器XZ裸云；不宣称旧黑盒与新分解全路径在所有策略上等价。
- 说明：M4c逐项边界；原规划与M4b契约保留在contractHistory。仅描述和来源复核，不代表现场算法、吞吐或后续阶段验收。
- M4c边界验收（manifest_verified）：原生场景/公共ROI选择/复合来源和原子Bundle切片；旧8ID仅显式整图升级，不恢复旧别名。
- 阶段依赖边界：可选gap-compat历史MeasurementEngine，PCL/模板匹配/YAML/ONNX完整闭包仅显式兼容构建；不作为新公共算法入口。
- 公开接口：packs/std-profile/include/lyflow_profile/provenance.h；packs/std-profile/include/lyflow_profile/features.h；packs/std-profile/include/lyflow_profile/selection.h；core/src/graph_contract_upgrade_gap.h
- 阶段验收边界：完整277项manifest与六导入器/七片段/十三Bundle契约核对；算法、宿主、SDK及完整CI由单独运行证据验收，不宣称整个M4/M5–M7完成。
- M4c迁包来源：`gap` / `packs/gap/ops/measure_reference.cpp:204` @ `f48aa13199a55b29088d439b8a7dbe090734a6eb`；原目标包和依赖规划保留。
