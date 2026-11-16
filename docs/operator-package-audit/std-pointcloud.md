# std-pointcloud 逐算子迁移清单

状态：实施中；当前 47 项。原251/7规划基线通过机器清单baselineReference追溯。返回[清单总览](../operator-package-audit.md)。

下列顺序是迁移/验收顺序，不是执行图调度顺序。精确端口和参数见[机器清单](../operator-package-audit.json)。

## 214 · cloud.bounds_obb

- 当前：`std-pointcloud` / `1.0.0` / 点云有向包围盒。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/cloud_analysis.cpp:162](../../packs/std-pointcloud/ops/cloud_analysis.cpp#L162)。
- 端口：`cloud:PointCloud` → `statistics:Record`, `height:Measurement`, `pose:Transform`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：Eigen定长/动态线性代数；原生PointCloud与小Record；当前整包传递PCL/匹配后端；目标只为实际需要搜索/配准的子模块引入
- 当前契约：cloud:PointCloud → statistics:Record, height:Measurement, pose:Transform。double点集统计；米制cloud.Statistics.v1绑定sourceCloudId与frame，输出height/Transform；空云valid=false，退化或重复特征值orientationStable=false。
- 目标契约：保留点云统计adapter；质心/协方差与基几何内核可共享，PCA轴右手及符号规范不代表跨帧身份跟踪。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：空云、重合/共线/等特征值、右手轴、米制尺寸、来源身份及large-offset数值稳定性。
- 说明：三者当前共享统计实现但公开意图不同；本轮不盲目合并节点或强行认定PCA姿态稳定。

## 215 · cloud.box_sample

- 当前：`std-pointcloud` / `1.0.0` / 长方体点云采样。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/p5_cloud_inspection.cpp:226](../../packs/std-pointcloud/ops/p5_cloud_inspection.cpp#L226)。
- 端口：`boxes:Record` → `cloud:PointCloud`, `indices:Indices`, `groups:Record`, `wireframe:PointCloud`, `wireIndices:Indices`, `wireGroups:Record`, `quality:Record`。
- 参数单位标注：wireSpacing=m, minVisibleAngle=deg。
- 依赖：Eigen定长/动态线性代数；原生PointCloud与小Record；当前整包传递PCL/匹配后端；目标只为实际需要搜索/配准的子模块引入
- 当前契约：boxes:Record → cloud:PointCloud, indices:Indices, groups:Record, wireframe:PointCloud, wireIndices:Indices, wireGroups:Record, quality:Record。米制长方体中心锚定，表面或体积确定性采样，可见面按观察点/法线选择；local模式每组需quality.sourcePose解释。
- 目标契约：保留点云生成adapter，有限盒几何与Pose校验经公共几何接口；显式local/world模式与不含真实观测的生成性质。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：表面/体积点数、稳定组ID、法线与可见面、local源位姿解释、线框索引与单位。
- 说明：几何参数是纯几何，采样产物仍属于点云；不把合成点当传感器有效观测。

## 216 · cloud.crop_primitives

- 当前：`std-pointcloud` / `1.0.0` / 按工件有限体裁云与碰撞检查。目标：`std-pointcloud`；拆公共能力/适配；**M6**。
- 源码：[packs/std-pointcloud/ops/p5_primitive_crop.cpp:464](../../packs/std-pointcloud/ops/p5_primitive_crop.cpp#L464)。
- 端口：`cloud:PointCloud`, `poses:Record`, `posesToCloud:Transform?` → `cloud:PointCloud`, `inner:PointCloud`, `outer:PointCloud`, `innerIndices:Indices`, `outerIndices:Indices`, `invalidIndices:Indices`, `groupIndices:Indices`, `groups:Record`, `freePoses:Record`, `collisionPoses:Record`, `unknownPoses:Record`, `validity:Tensor`, `quality:Record`。
- 参数单位标注：maxHeight=m。
- 依赖：Eigen与原生PointCloud/Indices/Pose几何判断；当前直接包含std-geometry/ops私有头；model_radius移除分支使用PCL kdtree；纯shape裁剪分支无需PCL查询；目标公共有限几何接口+点云adapter，清除跨包私有头包含
- 当前契约：cloud:PointCloud, poses:Record, posesToCloud:Transform → cloud:PointCloud, inner:PointCloud, outer:PointCloud, innerIndices:Indices, outerIndices:Indices, invalidIndices:Indices, groupIndices:Indices, groups:Record, freePoses:Record, collisionPoses:Record, unknownPoses:Record, validity:Tensor, quality:Record。米制有限box/cylinder/sphere与显式posesToCloud；输出内/外/invalid索引、组、碰撞/自由/未知Pose及validity Tensor；remove可model_radius和compact/preserve_order。
- 目标契约：保留std-pointcloud选择/移除；几何判定和Pose约束下沉公共std-geometry接口，原云索引、无效点和unknownHeight三态不可丢失。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：旋转/顶面锚定/表面开闭、未知高度、帧转换、model_radius、preserve_order NaN槽位、分组来源和三态结果。
- 说明：M2必须先移除../../std-geometry/ops/p5_pose_geometry_common.h私有头耦合；不能把unknown直接归到free。

## 217 · cloud.edges_3d

- 当前：`std-pointcloud` / `1.0.0` / 三维点云边缘提取。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/p2_global_matching.cpp:1181](../../packs/std-pointcloud/ops/p2_global_matching.cpp#L1181)。
- 端口：`cloud:PointCloud`, `validIndices:Indices?` → `cloud:PointCloud`, `indices:Indices`, `boundaryIndices:Indices`, `occludingIndices:Indices`, `occludedIndices:Indices`, `curvatureIndices:Indices`, `quality:Record`。
- 参数单位标注：neighborRadius=m, depthThreshold=m, boundaryAngle=rad, curvatureAngle=rad。
- 依赖：Eigen定长/动态线性代数；原生PointCloud与小Record；当前整包传递PCL/匹配后端；目标只为实际需要搜索/配准的子模块引入
- 当前契约：cloud:PointCloud, validIndices:Indices → cloud:PointCloud, indices:Indices, boundaryIndices:Indices, occludingIndices:Indices, occludedIndices:Indices, curvatureIndices:Indices, quality:Record。米制点云；organized须显式宽高及正Z深度，可用validIndices表达孔洞；unorganized按局部PCA角缺口找边界，不支持像素遮挡分类。
- 目标契约：保留点云布局敏感边缘提取；layout capability与sample-order区分，输出各类Indices始终绑定来源云，退化邻域写质量。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：organized尺寸/孔洞/Z、无组织点云、像素遮挡模式拒绝、退化邻域和索引身份。
- 说明：当前不隐式从点云数量推断图像布局；与图像边缘、轮廓分段分属不同契约。

## 218 · cloud.empty_tote

- 当前：`std-pointcloud` / `1.0.0` / 料箱观测状态判定。目标：`std-pointcloud`；拆公共能力/适配；**M6**。
- 源码：[packs/std-pointcloud/ops/p2_height_rules.cpp:509](../../packs/std-pointcloud/ops/p2_height_rules.cpp#L509)。
- 端口：`cloud:PointCloud`, `instances:Record`, `observation:Record`, `roi:Record?` → `status:Record`, `valid:Record`, `empty:Record`, `undetected:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：Eigen定长/动态线性代数；原生PointCloud与小Record；当前整包传递PCL/匹配后端；目标只为实际需要搜索/配准的子模块引入
- 当前契约：cloud:PointCloud, instances:Record, observation:Record, roi:Record → status:Record, valid:Record, empty:Record, undetected:Record。米制云、实例及必需observation布尔Value/可选roi→status及valid/empty/undetected布尔Value；显式unknown/occupied/unrecognized/empty状态，不能把无效观测当空箱。
- 目标契约：主保留点云有界ROI占用证据提取；把空箱业务命名/分支策略拆为场景组合，观测有效性语义继续强制区分。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：有效空观测、缺失观测、ROI无覆盖、实例存在/不在ROI、三类分支互斥且无虚构就绪状态。
- 说明：该节点含场景化命名，后续可拆occupied-region证据+实例分支；不把空箱结果升级为产线抓取许可。

## 219 · cloud.group_concat

- 当前：`std-pointcloud` / `1.0.0` / 点云集合拼接。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/cloud_analysis.cpp:310](../../packs/std-pointcloud/ops/cloud_analysis.cpp#L310)。
- 端口：`a:PointCloud`, `b:PointCloud`, `indicesA:Indices?`, `groupsA:Record?`, `indicesB:Indices?`, `groupsB:Record?` → `cloud:PointCloud`, `indices:Indices`, `groups:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：原生PointCloud/Indices与cloud_groups校验/选择；无需PCL搜索；当前TU经p1_common包含Eigen且整包传递PCL；目标portable分组模块清除非必要包含
- 当前契约：a:PointCloud, b:PointCloud, indicesA:Indices, groupsA:Record, indicesB:Indices, groupsB:Record → cloud:PointCloud, indices:Indices, groups:Record。同帧云/组拼接；可选indicesA/groupsA与indicesB/groupsB分别成对，ID加a/b前缀并重绑来源；通道不一致报错。
- 目标契约：std-pointcloud分组能力；强制同帧米制与sourceCloudId/点数/组区间校验，保留每节点的物化/共享及通道策略。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：错云身份、越界/重叠/重复ID、组顺序、空组、可选通道与拼接后来源重绑。
- 说明：组成员是原生索引，不移到通用小Record集合包；统计仅依赖Eigen，包装/选择本身不需PCL。

## 220 · cloud.group_get

- 当前：`std-pointcloud` / `1.0.0` / 点云分组提取。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/cloud_analysis.cpp:222](../../packs/std-pointcloud/ops/cloud_analysis.cpp#L222)。
- 端口：`cloud:PointCloud`, `indices:Indices`, `groups:Record` → `cloud:PointCloud`, `indices:Indices`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：原生PointCloud/Indices与cloud_groups校验/选择；无需PCL搜索；当前TU经p1_common包含Eigen且整包传递PCL；目标portable分组模块清除非必要包含
- 当前契约：cloud:PointCloud, indices:Indices, groups:Record → cloud:PointCloud, indices:Indices。按组序号index从输入三元组cloud/indices/groups提取成员并物化新云；返回indices仍绑定原始输入云，不能拿它索引新云。
- 目标契约：std-pointcloud分组能力；强制同帧米制与sourceCloudId/点数/组区间校验，保留每节点的物化/共享及通道策略。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：错云身份、越界/重叠/重复ID、组顺序、空组、可选通道与拼接后来源重绑。
- 说明：组成员是原生索引，不移到通用小Record集合包；统计仅依赖Eigen，包装/选择本身不需PCL。

## 221 · cloud.group_merge

- 当前：`std-pointcloud` / `1.0.0` / 点云分组合并。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/cloud_analysis.cpp:293](../../packs/std-pointcloud/ops/cloud_analysis.cpp#L293)。
- 端口：`cloud:PointCloud`, `indices:Indices`, `groups:Record` → `cloud:PointCloud`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：原生PointCloud/Indices与cloud_groups校验/选择；无需PCL搜索；当前TU经p1_common包含Eigen且整包传递PCL；目标portable分组模块清除非必要包含
- 当前契约：cloud:PointCloud, indices:Indices, groups:Record → cloud:PointCloud。把所选分组物化为一片点云，按组成员顺序选择，输出属性与成员一一对应。
- 目标契约：std-pointcloud分组能力；强制同帧米制与sourceCloudId/点数/组区间校验，保留每节点的物化/共享及通道策略。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：错云身份、越界/重叠/重复ID、组顺序、空组、可选通道与拼接后来源重绑。
- 说明：组成员是原生索引，不移到通用小Record集合包；统计仅依赖Eigen，包装/选择本身不需PCL。

## 222 · cloud.group_pack

- 当前：`std-pointcloud` / `1.0.0` / 点云分组构造。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/cloud_analysis.cpp:205](../../packs/std-pointcloud/ops/cloud_analysis.cpp#L205)。
- 端口：`cloud:PointCloud` → `cloud:PointCloud`, `indices:Indices`, `groups:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：原生PointCloud/Indices与cloud_groups校验/选择；无需PCL搜索；当前TU经p1_common包含Eigen且整包传递PCL；目标portable分组模块清除非必要包含
- 当前契约：cloud:PointCloud → cloud:PointCloud, indices:Indices, groups:Record。单云包装为组，PointCloud+原生Indices+cloud.Groups.v1；索引绑定sourceCloudId，不在JSON复制大点数组。
- 目标契约：std-pointcloud分组能力；强制同帧米制与sourceCloudId/点数/组区间校验，保留每节点的物化/共享及通道策略。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：错云身份、越界/重叠/重复ID、组顺序、空组、可选通道与拼接后来源重绑。
- 说明：组成员是原生索引，不移到通用小Record集合包；统计仅依赖Eigen，包装/选择本身不需PCL。

## 223 · cloud.group_statistics

- 当前：`std-pointcloud` / `1.0.0` / 点云逐组统计。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/cloud_analysis.cpp:388](../../packs/std-pointcloud/ops/cloud_analysis.cpp#L388)。
- 端口：`cloud:PointCloud`, `indices:Indices`, `groups:Record` → `statistics:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：Eigen定长/动态线性代数；原生PointCloud与小Record；当前整包传递PCL/匹配后端；目标只为实际需要搜索/配准的子模块引入
- 当前契约：cloud:PointCloud, indices:Indices, groups:Record → statistics:Record。验证组区间、ID、来源后逐组输出cloud.GroupStatistics.v1统计；组身份为本次输入内局部身份。
- 目标契约：std-pointcloud分组能力；强制同帧米制与sourceCloudId/点数/组区间校验，保留每节点的物化/共享及通道策略。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：错云身份、越界/重叠/重复ID、组顺序、空组、可选通道与拼接后来源重绑。
- 说明：组成员是原生索引，不移到通用小Record集合包；统计仅依赖Eigen，包装/选择本身不需PCL。

## 224 · cloud.group_subset

- 当前：`std-pointcloud` / `1.0.0` / 点云分组子集。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/cloud_analysis.cpp:249](../../packs/std-pointcloud/ops/cloud_analysis.cpp#L249)。
- 端口：`cloud:PointCloud`, `indices:Indices`, `groups:Record` → `cloud:PointCloud`, `indices:Indices`, `groups:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：原生PointCloud/Indices与cloud_groups校验/选择；无需PCL搜索；当前TU经p1_common包含Eigen且整包传递PCL；目标portable分组模块清除非必要包含
- 当前契约：cloud:PointCloud, indices:Indices, groups:Record → cloud:PointCloud, indices:Indices, groups:Record。按显式组选择保留成员区间，输出仍为来源云+原生索引+小型组Record，ID与顺序保留。
- 目标契约：std-pointcloud分组能力；强制同帧米制与sourceCloudId/点数/组区间校验，保留每节点的物化/共享及通道策略。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：错云身份、越界/重叠/重复ID、组顺序、空组、可选通道与拼接后来源重绑。
- 说明：组成员是原生索引，不移到通用小Record集合包；统计仅依赖Eigen，包装/选择本身不需PCL。

## 225 · cloud.neighbor_height

- 当前：`std-pointcloud` / `1.0.0` / 邻域支撑面高度估计。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/p2_height_rules.cpp:314](../../packs/std-pointcloud/ops/p2_height_rules.cpp#L314)。
- 端口：`cloud:PointCloud`, `instances:Record`, `workspace:Record?` → `instances:Record`, `heights:Record`。
- 参数单位标注：length=m, width=m, marginX=m, marginY=m, minHeight=m, maxHeight=m, layerThickness=m, planeDistance=m, maxPlaneAngle=deg, providedHeight=m。
- 依赖：Eigen定长/动态线性代数；原生PointCloud与小Record；当前整包传递PCL/匹配后端；目标只为实际需要搜索/配准的子模块引入
- 当前契约：cloud:PointCloud, instances:Record, workspace:Record → instances:Record, heights:Record。米制云+工件实例+可选workspace→实例与高度Record；邻域ROI、层/平面/显式高度模式受限，不直接给抓取许可。
- 目标契约：保留通用邻域高度测量adapter；显式局部ROI/高度轴、工作空间和提供值来源，工件元数据/ID保持。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：旋转工件ROI、层/平面退化、providedHeight、帧冲突及ID/属性不丢失。
- 说明：可共享高度计量的标量定义，但点云邻域与实例关联仍属std-pointcloud。

## 226 · cloud.pca

- 当前：`std-pointcloud` / `1.0.0` / 点云主成分分析。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/cloud_analysis.cpp:162](../../packs/std-pointcloud/ops/cloud_analysis.cpp#L162)。
- 端口：`cloud:PointCloud` → `statistics:Record`, `height:Measurement`, `pose:Transform`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：Eigen定长/动态线性代数；原生PointCloud与小Record；当前整包传递PCL/匹配后端；目标只为实际需要搜索/配准的子模块引入
- 当前契约：cloud:PointCloud → statistics:Record, height:Measurement, pose:Transform。double点集统计；米制cloud.Statistics.v1绑定sourceCloudId与frame，输出height/Transform；空云valid=false，退化或重复特征值orientationStable=false。
- 目标契约：保留点云统计adapter；质心/协方差与基几何内核可共享，PCA轴右手及符号规范不代表跨帧身份跟踪。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：空云、重合/共线/等特征值、右手轴、米制尺寸、来源身份及large-offset数值稳定性。
- 说明：三者当前共享统计实现但公开意图不同；本轮不盲目合并节点或强行认定PCA姿态稳定。

## 227 · cloud.plane_patches

- 当前：`std-pointcloud` / `1.0.0` / 有限平面片几何分解。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/p2_height_rules.cpp:651](../../packs/std-pointcloud/ops/p2_height_rules.cpp#L651)。
- 端口：`cloud:PointCloud` → `instances:Record`, `patches:Record`, `groups:Record`, `indices:Indices`, `uncoveredIndices:Indices`, `uncovered:PointCloud`。
- 参数单位标注：planeDistance=m, cellSize=m, minSide=m, minArea=m2, thickness=m, clusterDistance=m, normalAngle=deg, collisionDistance=m。
- 依赖：Eigen定长/动态线性代数；原生PointCloud与小Record；当前整包传递PCL/匹配后端；目标只为实际需要搜索/配准的子模块引入
- 当前契约：cloud:PointCloud → instances:Record, patches:Record, groups:Record, indices:Indices, uncoveredIndices:Indices, uncovered:PointCloud。米制云→平面矩形patch实例、groups/indices与未覆盖云；参数含距离/面积/角度，输出工件不是生产许可。
- 目标契约：保留点云面片提取；几何矩形/碰撞关系复用公共内核，组索引绑定源云，质量与未覆盖点显式返回。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：平面/多面/断裂面片、面积m²、法线朝向、边缘覆盖/碰撞、组索引和属性对应。
- 说明：实际算法为Eigen与原生邻接/栅格；当前整包PCL链接不等于每步都调用PCL。

## 228 · cloud.remove_primitives

- 当前：`std-pointcloud` / `1.0.0` / 按工件有限体或模型移除点云。目标：`std-pointcloud`；拆公共能力/适配；**M6**。
- 源码：[packs/std-pointcloud/ops/p5_primitive_crop.cpp:464](../../packs/std-pointcloud/ops/p5_primitive_crop.cpp#L464)。
- 端口：`cloud:PointCloud`, `poses:Record`, `posesToCloud:Transform?`, `model:PointCloud?` → `cloud:PointCloud`, `inner:PointCloud`, `outer:PointCloud`, `innerIndices:Indices`, `outerIndices:Indices`, `invalidIndices:Indices`, `groupIndices:Indices`, `groups:Record`, `freePoses:Record`, `collisionPoses:Record`, `unknownPoses:Record`, `validity:Tensor`, `quality:Record`。
- 参数单位标注：maxHeight=m, radius=m。
- 依赖：Eigen与原生PointCloud/Indices/Pose几何判断；当前直接包含std-geometry/ops私有头；model_radius移除分支使用PCL kdtree；纯shape裁剪分支无需PCL查询；目标公共有限几何接口+点云adapter，清除跨包私有头包含
- 当前契约：cloud:PointCloud, poses:Record, posesToCloud:Transform, model:PointCloud → cloud:PointCloud, inner:PointCloud, outer:PointCloud, innerIndices:Indices, outerIndices:Indices, invalidIndices:Indices, groupIndices:Indices, groups:Record, freePoses:Record, collisionPoses:Record, unknownPoses:Record, validity:Tensor, quality:Record。米制有限box/cylinder/sphere与显式posesToCloud；输出内/外/invalid索引、组、碰撞/自由/未知Pose及validity Tensor；remove可model_radius和compact/preserve_order。
- 目标契约：保留std-pointcloud选择/移除；几何判定和Pose约束下沉公共std-geometry接口，原云索引、无效点和unknownHeight三态不可丢失。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：旋转/顶面锚定/表面开闭、未知高度、帧转换、model_radius、preserve_order NaN槽位、分组来源和三态结果。
- 说明：M2必须先移除../../std-geometry/ops/p5_pose_geometry_common.h私有头耦合；不能把unknown直接归到free。

## 229 · cloud.statistics

- 当前：`std-pointcloud` / `1.0.0` / 点云三维统计。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/cloud_analysis.cpp:162](../../packs/std-pointcloud/ops/cloud_analysis.cpp#L162)。
- 端口：`cloud:PointCloud` → `statistics:Record`, `height:Measurement`, `pose:Transform`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：Eigen定长/动态线性代数；原生PointCloud与小Record；当前整包传递PCL/匹配后端；目标只为实际需要搜索/配准的子模块引入
- 当前契约：cloud:PointCloud → statistics:Record, height:Measurement, pose:Transform。double点集统计；米制cloud.Statistics.v1绑定sourceCloudId与frame，输出height/Transform；空云valid=false，退化或重复特征值orientationStable=false。
- 目标契约：保留点云统计adapter；质心/协方差与基几何内核可共享，PCA轴右手及符号规范不代表跨帧身份跟踪。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：空云、重合/共线/等特征值、右手轴、米制尺寸、来源身份及large-offset数值稳定性。
- 说明：三者当前共享统计实现但公开意图不同；本轮不盲目合并节点或强行认定PCA姿态稳定。

## 230 · cloud.template_3d

- 当前：`std-pointcloud` / `1.0.0` / 三维表面模板构建。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/p2_global_matching.cpp:1114](../../packs/std-pointcloud/ops/p2_global_matching.cpp#L1114)。
- 端口：`cloud:PointCloud` → `model:PointCloud`, `template:Record`。
- 参数单位标注：sampleDistance=m。
- 依赖：Eigen定长/动态线性代数；原生PointCloud与小Record；当前整包传递PCL/匹配后端；目标只为实际需要搜索/配准的子模块引入
- 当前契约：cloud:PointCloud → model:PointCloud, template:Record。非共面米制模型确定性栅格/最远点采样，按原AABB中心居中；template元数据绑定新modelCloudId/点数/帧，非抓取姿态。
- 目标契约：保留点云模板构造；模板身份与归一化转换显式，存取仍经点云I/O后重建绑定，不把Record变成模型大数组。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：点数预算、平面退化、居中变换、确定性、错云模板身份及重载重新绑定。
- 说明：仅模板构造不需要调用KISS/small_gicp；后端依赖由global_3d能力声明。

## 231 · cloud.top_layer

- 当前：`std-pointcloud` / `1.0.0` / 点云顶层高度。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/p2_height_rules.cpp:199](../../packs/std-pointcloud/ops/p2_height_rules.cpp#L199)。
- 端口：`cloud:PointCloud`, `groups:Record?`, `indices:Indices?` → `heights:Record`, `height:Measurement`, `top:PointCloud`, `rest:PointCloud`, `topIndices:Indices`, `restIndices:Indices`, `topGroups:Record`, `restGroups:Record`。
- 参数单位标注：layerThickness=m, extractRadius=m, voxelSize=m, minHeight=m, maxHeight=m。
- 依赖：Eigen定长/动态线性代数；原生PointCloud与小Record；当前整包传递PCL/匹配后端；目标只为实际需要搜索/配准的子模块引入
- 当前契约：cloud:PointCloud, groups:Record, indices:Indices → heights:Record, height:Measurement, top:PointCloud, rest:PointCloud, topIndices:Indices, restIndices:Indices, topGroups:Record, restGroups:Record。米制云及可选Groups/Indices→层高Record、Measurement、top/rest云和各自索引/组；高度轴/层厚与提取半径显式参数。
- 目标契约：保留点云层统计/选择；层高单位与来源云身份显式，提取后Groups重新绑定，空或无层不得伪造高度。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：轴向/层厚、噪点、空层、多组、上下层索引互补与输出组重新绑定。
- 说明：空间层提取是点云能力；最终工件是否可抓取属于场景规则。

## 232 · cloud.view_sample

- 当前：`std-pointcloud` / `1.0.0` / 点云可见面采样。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/p2_global_matching.cpp:1214](../../packs/std-pointcloud/ops/p2_global_matching.cpp#L1214)。
- 端口：`model:PointCloud`, `poses:Record` → `cloud:PointCloud`, `indices:Indices`, `groups:Record`, `cloudIndices:Indices`, `cloudGroups:Record`, `quality:Record`。
- 参数单位标注：angularResolution=rad, depthTolerance=m, nearPlane=m。
- 依赖：Eigen定长/动态线性代数；原生PointCloud与小Record；当前整包传递PCL/匹配后端；目标只为实际需要搜索/配准的子模块引入
- 当前契约：model:PointCloud, poses:Record → cloud:PointCloud, indices:Indices, groups:Record, cloudIndices:Indices, cloudGroups:Record, quality:Record。model→camera位姿投影；离散atan2角度深度缓冲保留最近深度层，输出cloud/groups与原模型indices/groups分别绑定，不混用身份。
- 目标契约：保留点云可见性采样；米/rad、相机正深度和离散近似显式声明，保持model和拼接输出的两套来源关系。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：前后遮挡、nearPlane、角栅格/深度容差、法线并不决定全部可见性、双套索引身份。
- 说明：不是连续网格精确可见性或图像渲染器；与CAD拓扑采样边界分开。

## 233 · edit.translate_region

- 当前：`std-pointcloud` / `1.0.0` / 点云选区平移。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/edit_translate_region.cpp:73](../../packs/std-pointcloud/ops/edit_translate_region.cpp#L73)。
- 端口：`cloud:PointCloud` → `cloud:PointCloud`。
- 参数单位标注：point=m, boxMin=m, boxMax=m, translation=m。
- 依赖：C++标准库与原生PointCloud/Indices；算法无需PCL/OpenCV；当前整包传递链接PCL、Eigen、匹配CPU后端；目标portable对象模块解除这些非必要依赖
- 当前契约：cloud:PointCloud → cloud:PointCloud。米制半空间或闭盒选区内XYZ平移；点数/点序和intensity/RGB/normals原样保留，纯几何选择不认识缝或锚点。
- 目标契约：保留std-pointcloud/portable；选区和位移显式同帧米制，操作保持采样顺序但不保证业务锚点不动。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：严格半空间/闭盒边界、未选点不变、选择跨锚点的示例和全部属性不变。
- 说明：用于扰动验收不等于业务测量算子，不能迁到gap或metrology。

## 234 · features.normals

- 当前：`std-pointcloud` / `1.0.0` / 点云法线估计。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/features_normals.cpp:111](../../packs/std-pointcloud/ops/features_normals.cpp#L111)。
- 端口：`cloud:PointCloud` → `cloud:PointCloud`。
- 参数单位标注：radius=m, viewpoint=m。
- 依赖：PCL normal_3d/kdtree与Eigen；当前并行循环设施；目标法线模块按需链接搜索后端
- 当前契约：cloud:PointCloud → cloud:PointCloud。估计XYZ的空间邻域法线；radius/viewpoint为m，法线无量纲并朝约定视点定向；保留intensity/RGB。
- 目标契约：std-pointcloud法线能力；声明邻域、法线朝向、退化点策略，不把缺失法线自动当作有效单位向量。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：平面/退化邻域、朝向视点、线程数变化、通道长度及非法法线。
- 说明：法线是空间点云特征，不因有2D应用迁入std-profile。

## 235 · filter.crop_box

- 当前：`std-pointcloud` / `1.0.0` / 三维包围盒裁剪。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/filter_crop_box.cpp:97](../../packs/std-pointcloud/ops/filter_crop_box.cpp#L97)。
- 端口：`cloud:PointCloud`, `pose:Transform?` → `cloud:PointCloud`。
- 参数单位标注：min=m, max=m。
- 依赖：C++标准库与原生PointCloud/Indices；算法无需PCL/OpenCV；当前整包传递链接PCL、Eigen、匹配CPU后端；目标portable对象模块解除这些非必要依赖
- 当前契约：cloud:PointCloud, pose:Transform → cloud:PointCloud。米制三维盒裁剪；可选Transform定义盒局部坐标，输出保留被选点属性。
- 目标契约：点云选择adapter保留；盒几何/逆变换校验复用公共几何内核，源帧和盒帧通过显式Transform连接。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：旋转盒、边界/反选、变换方向、属性对应关系和非法变换。
- 说明：纯几何盒判断可共享，PointCloud筛选节点仍归std-pointcloud。

## 236 · filter.crop_box2d

- 当前：`std-pointcloud` / `1.0.0` / 二维矩形裁剪。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/filter_crop_box2d.cpp:36](../../packs/std-pointcloud/ops/filter_crop_box2d.cpp#L36)。
- 端口：`cloud:PointCloud`, `box:Box2D` → `cloud:PointCloud`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：Eigen二维比较与原生PointCloud选择；当前algo/crop2d.h经PCL Cloud2D类型传递包含PCL；目标抽出不含PCL的矩形关系内核；点云adapter保留
- 当前契约：cloud:PointCloud, box:Box2D → cloud:PointCloud。XY米制Box2D裁剪，z不参与；open严格开边界、closed闭边界，裁空报roi_empty。
- 目标契约：保留std-pointcloud节点；共享std-geometry轻量矩形判断，Box2D单位/XY平面需显式匹配输入云。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：开闭四边、裁空诊断、非有限点、错单位/帧和属性选择一致。
- 说明：头文件依赖与算法依赖不同；清除Cloud2D/PCL泄漏后才可给轻量轮廓共用。

## 237 · filter.passthrough

- 当前：`std-pointcloud` / `1.0.0` / 直通滤波。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/filter_passthrough.cpp:89](../../packs/std-pointcloud/ops/filter_passthrough.cpp#L89)。
- 端口：`cloud:PointCloud` → `cloud:PointCloud`, `indices:Indices`。
- 参数单位标注：min=m, max=m。
- 依赖：C++标准库与原生PointCloud/Indices；算法无需PCL/OpenCV；当前整包传递链接PCL、Eigen、匹配CPU后端；目标portable对象模块解除这些非必要依赖
- 当前契约：cloud:PointCloud → cloud:PointCloud, indices:Indices。指定字段闭区间筛选/反选；坐标阈值m，intensity等字段按自身数值；indices绑定输入云，keepOrganized可保槽位。
- 目标契约：std-pointcloud/portable按字段裁剪；明确坐标和非坐标字段单位，保持筛选索引、原点序及organized模式。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：闭区间边界、反选、NaN、不同字段与organized槽位/原索引一致。
- 说明：无需PCL滤波器，当前重依赖来自包整体构建。

## 238 · filter.radius_outlier

- 当前：`std-pointcloud` / `1.0.0` / 半径离群点剔除。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/filter_radius_outlier.cpp:87](../../packs/std-pointcloud/ops/filter_radius_outlier.cpp#L87)。
- 端口：`cloud:PointCloud` → `cloud:PointCloud`, `removed:Indices`。
- 参数单位标注：radius=m。
- 依赖：PCL kdtree/search（邻域查询）与Eigen；原生点云适配；目标保留std-pointcloud搜索后端，portable子模块不得反向依赖
- 当前契约：cloud:PointCloud → cloud:PointCloud, removed:Indices。米制半径邻域的最少邻居数筛选；removed绑定原云，输出可选通道随点筛选。
- 目标契约：保留搜索后端与邻居计数语义；半径单位、边界和点序能力在契约中显式声明。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：半径边界、自点计数、空云和小云、NaN、removed身份与通道对应。
- 说明：与std-profile滑动中值按采样邻接处理的含义不同。

## 239 · filter.random_sample

- 当前：`std-pointcloud` / `2.0.0` / 随机采样。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/filter_random_sample.cpp:77](../../packs/std-pointcloud/ops/filter_random_sample.cpp#L77)。
- 端口：`cloud:PointCloud` → `cloud:PointCloud`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：C++标准库与原生PointCloud/Indices；算法无需PCL/OpenCV；当前整包传递链接PCL、Eigen、匹配CPU后端；目标portable对象模块解除这些非必要依赖
- 当前契约：cloud:PointCloud → cloud:PointCloud。按固定seed抽样，抽中点的可选通道随点对应；输出是空间点集，不保证原轮廓完整性。
- 目标契约：std-pointcloud/portable随机选择；保持被选点的原始相对顺序，但丢失完整采样槽位/organized布局，保持种子和抽样数量定义。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：同seed同结果、边界数量、空云及通道对应；原索引追踪策略显式。
- 说明：随机抽样不属于业务专用算法。

## 240 · filter.statistical_outlier

- 当前：`std-pointcloud` / `1.0.0` / 统计离群点剔除。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/filter_statistical_outlier.cpp:111](../../packs/std-pointcloud/ops/filter_statistical_outlier.cpp#L111)。
- 端口：`cloud:PointCloud` → `cloud:PointCloud`, `removed:Indices`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：PCL kdtree/search（邻域查询）与Eigen；原生点云适配；目标保留std-pointcloud搜索后端，portable子模块不得反向依赖
- 当前契约：cloud:PointCloud → cloud:PointCloud, removed:Indices。按K近邻平均距离及均值+系数×标准差剔除离群点；removed为原云索引，保留属性。
- 目标契约：保留PCL邻域搜索实现及阈值定义；点集筛选与原云索引来源单列，禁止偷偷改变近邻自点约定。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：小点数、相同点、NaN、保留/删除索引互补、并行结果与附加通道。
- 说明：算法是显式搜索依赖，不能仅改目录就宣称零PCL。

## 241 · filter.voxel_grid

- 当前：`std-pointcloud` / `1.0.0` / 体素降采样。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/filter_voxel_grid.cpp:379](../../packs/std-pointcloud/ops/filter_voxel_grid.cpp#L379)。
- 端口：`cloud:PointCloud` → `cloud:PointCloud`。
- 参数单位标注：leafSize=m。
- 依赖：C++标准库与原生PointCloud/Indices；算法无需PCL/OpenCV；当前整包传递链接PCL、Eigen、匹配CPU后端；目标portable对象模块解除这些非必要依赖
- 当前契约：cloud:PointCloud → cloud:PointCloud。米制XYZ按leafSize体素化；有界栅格实现，代表点和可选通道按本实现聚合，不承诺保留采样列序。
- 目标契约：std-pointcloud/portable空间降采样；标记会改变点数与点序，输入若携带轮廓列序须显式丢弃该能力。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：固定输入确定性、负坐标/体素边界、通道聚合、资源上限；禁止将输出继续当原采样槽位。
- 说明：不能把空间体素滤波作为std-profile保列序清洗的替代。

## 242 · fit.circle_2d

- 当前：`std-pointcloud` / `1.0.0` / 二维圆拟合。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/fit_circle_2d.cpp:72](../../packs/std-pointcloud/ops/fit_circle_2d.cpp#L72)。
- 端口：`cloud:PointCloud` → `circle:Circle2D`, `inliers:Indices`。
- 参数单位标注：distThresh=m, rMin=m, rMax=m, fixedRadius=m。
- 依赖：PCL SACMODEL_CIRCLE2D/RANSAC、Eigen；定半径细化与内点重筛；目标std-pointcloud/fit保留原方法
- 当前契约：cloud:PointCloud → circle:Circle2D, inliers:Indices。XY米制圆拟合；半径上下限与fixedRadius的重定圆心/重筛路径必须保持，输出原云inliers。
- 目标契约：保留RANSAC与定半径路径；轻量轮廓圆拟合另有方法标识，公用Circle关系不等于共用同一估计器。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：圆弧短段、固定半径、上下限、内点重筛、非法/退化云与原云索引。
- 说明：不得因抽公共内核把PCL估计替换为DTS的最小二乘。

## 243 · fit.line_2d

- 当前：`std-pointcloud` / `1.0.0` / 二维直线拟合。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/fit_line_2d.cpp:94](../../packs/std-pointcloud/ops/fit_line_2d.cpp#L94)。
- 端口：`cloud:PointCloud`, `clipTo:Box2D?` → `line:Line2D`, `inliers:Indices`。
- 参数单位标注：distThresh=m。
- 依赖：PCL SAC拟合及Cloud2D适配、Eigen；原生Indices；目标std-pointcloud/fit保留PCL后端
- 当前契约：cloud:PointCloud, clipTo:Box2D → line:Line2D, inliers:Indices。XY米制RANSAC直线；还含两条近似平行迹线的候选/伴线选择，clipTo为可选Box2D，输出inliers关联来源云。
- 目标契约：保留空间点集RANSAC与伴线策略；公共几何仅承担线参数关系，不把该节点替换成轮廓TLS。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：同输入确定性、双迹线/候选选择、ROI截线、退化输入及inliers身份。
- 说明：名字中的2D不表示需要有序采样；与DTS最小二乘拟合是不同算法契约。

## 244 · io.load_cloud

- 当前：`std-pointcloud` / `1.0.0` / 点云与三角网格采样读取。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/io_load_cloud.cpp:431](../../packs/std-pointcloud/ops/io_load_cloud.cpp#L431)。
- 端口：`initial:Transform?` → `cloud:PointCloud`, `metadata:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：PCL PLY/PCD格式适配；C++有界STL解析/面积采样、Eigen初姿；目标std-pointcloud/io与portable STL读取按子能力拆依赖
- 当前契约：initial:Transform → cloud:PointCloud, metadata:Record。PCD/点PLY/ASCII或小端二进制STL；显式m/mm换算后再施加初姿；STL按面积固定seed采样生成法线，不保留网格拓扑，非法字段格式拒绝。
- 目标契约：保持有界模型到点云适配；单位换算顺序和source→target初姿明确，STL纯解析不强制PCL但输出仍是PointCloud。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：m/mm与初姿非交换顺序、STL采样确定性/法线、拒绝mesh PLY/非法字段、路径和预算。
- 说明：不把STL采样输出宣传成仍有CAD拓扑的模型；与io.load_pcd差异保留。

## 245 · io.load_pcd

- 当前：`std-pointcloud` / `1.0.0` / 点云读取。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/io_load_pcd.cpp:110](../../packs/std-pointcloud/ops/io_load_pcd.cpp#L110)。
- 端口：无 → `cloud:PointCloud`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：PCL io与PCD/PLY格式适配、文件系统；目标std-pointcloud/io子模块；不强制启用KISS-Matcher或ONNX
- 当前契约：无 → cloud:PointCloud。按扩展名读取PCD/PLY，实际字段映射XYZ及可选intensity/normals/RGB；文件坐标数值不自动完成业务单位转换。
- 目标契约：归属点云I/O；单位/帧必须由输入适配或显式转换声明，保持既有格式、路径和字段策略。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：ASCII/二进制PCD/PLY及中文路径往返；缺字段、非法文件和可选通道长度检查。
- 说明：保留公开节点，抽出I/O能力依赖；不能因通用io前缀搬入core。

## 246 · io.save_pcd

- 当前：`std-pointcloud` / `1.0.0` / 点云保存。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/io_save_pcd.cpp:94](../../packs/std-pointcloud/ops/io_save_pcd.cpp#L94)。
- 端口：`cloud:PointCloud` → 无。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：PCL io与PCD/PLY格式适配、文件系统；目标std-pointcloud/io子模块；不强制启用KISS-Matcher或ONNX
- 当前契约：cloud:PointCloud → 无。按扩展名写PCD/PLY；写出实际存在的通道，输入只读；属于有副作用的文件输出。
- 目标契约：归属点云I/O；单位/帧必须由输入适配或显式转换声明，保持既有格式、路径和字段策略。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：全部可选通道往返、目标格式与写盘错误诊断；不得把文件输出误设为缓存纯函数。
- 说明：保留公开节点，抽出I/O能力依赖；不能因通用io前缀搬入core。

## 247 · register.constrained_fit_3d

- 当前：`std-pointcloud` / `1.0.0` / 三维加权几何约束拟合。目标：`std-pointcloud`；拆公共能力/适配；**M6**。
- 源码：[packs/std-pointcloud/ops/p2_advanced_fit.cpp:582](../../packs/std-pointcloud/ops/p2_advanced_fit.cpp#L582)。
- 端口：`constraints:Record`, `initial:Transform?`, `cloud:PointCloud?` → `transform:Transform`, `aligned:PointCloud`, `quality:Record`。
- 参数单位标注：huberDelta=m, maxRmse=m, maxError=m, translationTolerance=m, rotationTolerance=rad。
- 依赖：Eigen加权SE3/Huber与可观性求解；PointCloud对齐adapter；当前与含PCL最近邻的p2_fit公共TU耦合；目标分离小几何求解内核与点云适配
- 当前契约：constraints:Record, initial:Transform, cloud:PointCloud → transform:Transform, aligned:PointCloud, quality:Record。cloud.Constraints3D.v1+可选initial/cloud→Transform/aligned/quality；米制混合点点/点线/点面/轴向偏移，3-2-1结构、可观性和float32交付质量均校验。
- 目标契约：公开点云对齐节点保留std-pointcloud；公共约束求解可下沉std-geometry内部库，保持局部优化/质量门语义及可选cloud输出。
- 迁移前置：M1；M2；register.constraints_3d
- 关联算子：register.constraints_3d
- 验收：混合与3-2-1、权重/退化、Huber、未收敛、float32交付门限、无cloud模式及输入只读。
- 说明：主目标仍为点云配准；与register.constraints_3d通过公共schema接口连接，避免几何包依赖整个点云包。

## 248 · register.global_3d

- 当前：`std-pointcloud` / `1.0.0` / 三维表面模板全局匹配。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/p2_global_matching.cpp:1131](../../packs/std-pointcloud/ops/p2_global_matching.cpp#L1131)。
- 端口：`model:PointCloud`, `template:Record`, `scene:PointCloud`, `sceneIndices:Indices?`, `sceneGroups:Record?` → `instances:Record`, `indices:Indices`, `groups:Record`, `quality:Record`。
- 参数单位标注：voxelSize=m, maxCorrespondenceDistance=m, maxRmse=m, translationTolerance=m, rotationTolerance=rad, nmsDistance=m, nmsAngle=rad, directionTolerance=rad。
- 依赖：KISS-Matcher/Faster-PFH、兼容图max-core/GNC；可选small_gicp精配准；Eigen与PCL/fit3最近邻质量；依赖后端内部还含其固定传递库，详见依赖清单；目标std-pointcloud/matching-cpu可选能力；不成为portable/profile/metrology的传递依赖
- 当前契约：model:PointCloud, template:Record, scene:PointCloud, sceneIndices:Indices, sceneGroups:Record → instances:Record, indices:Indices, groups:Record, quality:Record。居中米制model+身份template+scene/可选场景组→多实例、场景支持索引和quality；任意SE3候选后按真实float32位姿重算覆盖/RMSE/非共面支持；空匹配为空集合。
- 目标契约：保留CPU全局匹配后端及显式对称规则；pickable=false，非确定性不缓存，阶段边界取消与调用/点数预算不能被包重构削弱。
- 迁移前置：M1；M2；cloud.template_3d
- 关联算子：cloud.template_3d
- 验收：小件/遮挡/对称、错template身份、空ROI/无匹配、多实例独立支持、质量拒绝/预算/取消；Windows CPU与安装SDK运行。
- 说明：全局匹配不并入通用几何构造；GPU后端未来单列能力，主线不因包整理引入CUDA。

## 249 · register.icp_2d

- 当前：`std-pointcloud` / `1.1.0` / 二维 ICP 配准。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/register_icp_2d.cpp:271](../../packs/std-pointcloud/ops/register_icp_2d.cpp#L271)。
- 端口：`source:PointCloud`, `target:PointCloud`, `init:Transform?` → `transform:Transform`, `result:Record`。
- 参数单位标注：maxMatchingDist=m, fitnessDist=m, minDiffRot=rad, minDiffTrans=m, robustScale=m, coarseVoxelSize=m, maxPlaneRmse=m。
- 依赖：PCL/Cloud2D邻域法线与近邻搜索、Eigen；lyflow_std_algo ICP2D；目标std-pointcloud/registration模块
- 当前契约：source:PointCloud, target:PointCloud, init:Transform → transform:Transform, result:Record。XY米制point-to-plane ICP，输出source→target刚体，z轴恒等；可选鲁棒损失/金字塔及质量门，拒绝registration_rejected。
- 目标契约：保留二维空间配准节点；显式源/目标帧、误差单位和未收敛语义；不依赖轮廓采样列序。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：已知非交换SE2、质量重算/拒绝、鲁棒损失、粗细层数与退化情况。
- 说明：虽然gap使用该内核，其本身不属于gap领域或轻量std-profile。

## 250 · register.icp_3d

- 当前：`std-pointcloud` / `1.0.0` / 三维迭代最近点配准。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/register_3d.cpp:284](../../packs/std-pointcloud/ops/register_3d.cpp#L284)。
- 端口：`source:PointCloud`, `target:PointCloud`, `initial:Transform?` → `transform:Transform`, `aligned:PointCloud`, `quality:Record`。
- 参数单位标注：maxCorrespondenceDistance=m, maxRmse=m, translationTolerance=m, rotationTolerance=rad。
- 依赖：PCL kdtree对应查询与Eigen SE3求解；目标std-pointcloud/registration后端
- 当前契约：source:PointCloud, target:PointCloud, initial:Transform → transform:Transform, aligned:PointCloud, quality:Record。source→target局部SE3；点面需目标法线及六自由度可观；匹配比例/RMSE/收敛不合格拒绝；local_search最多9个显式初值。
- 目标契约：保留局部搜索边界；显式米制/帧、法线与初值要求，质量按实际交付位姿计算，禁止伪装全局匹配。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：已知刚体、法线缺失/退化、差初值/未收敛、覆盖率和RMSE拒绝、批次原子失败。
- 说明：不因同为三维配准就改为KISS或small_gicp；算法切换属于独立行为变更。

## 251 · register.local_search_3d

- 当前：`std-pointcloud` / `1.0.0` / 三维局部多初值配准。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/register_3d.cpp:284](../../packs/std-pointcloud/ops/register_3d.cpp#L284)。
- 端口：`source:PointCloud`, `target:PointCloud`, `initial:Transform?` → `transform:Transform`, `aligned:PointCloud`, `quality:Record`。
- 参数单位标注：maxCorrespondenceDistance=m, maxRmse=m, translationTolerance=m, rotationTolerance=rad, searchAngle=rad。
- 依赖：PCL kdtree对应查询与Eigen SE3求解；目标std-pointcloud/registration后端
- 当前契约：source:PointCloud, target:PointCloud, initial:Transform → transform:Transform, aligned:PointCloud, quality:Record。source→target局部SE3；点面需目标法线及六自由度可观；匹配比例/RMSE/收敛不合格拒绝；local_search最多9个显式初值。
- 目标契约：保留局部搜索边界；显式米制/帧、法线与初值要求，质量按实际交付位姿计算，禁止伪装全局匹配。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：已知刚体、法线缺失/退化、差初值/未收敛、覆盖率和RMSE拒绝、批次原子失败。
- 说明：不因同为三维配准就改为KISS或small_gicp；算法切换属于独立行为变更。

## 252 · register.pose_search_3d

- 当前：`std-pointcloud` / `1.0.0` / 三维工件六轴局部搜索。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/p2_advanced_fit.cpp:605](../../packs/std-pointcloud/ops/p2_advanced_fit.cpp#L605)。
- 端口：`source:PointCloud`, `target:PointCloud`, `initial:Transform?`, `poses:Record?` → `poses:Record`, `aligned:PointCloud`, `indices:Indices`, `groups:Record`, `quality:Record`。
- 参数单位标注：maxCorrespondenceDistance=m, maxRmse=m, translationTolerance=m, rotationTolerance=rad。
- 依赖：Eigen位姿枚举与fit3/PCL最近邻质量计算；可选ICP；目标std-pointcloud/registration与公共Pose schema
- 当前契约：source:PointCloud, target:PointCloud, initial:Transform, poses:Record → poses:Record, aligned:PointCloud, indices:Indices, groups:Record, quality:Record。平移XYZ和Rz·Ry·Rx六轴笛卡尔组合/逐级缩小，米/rad；可选ICP可越网格边界；按实际float32覆盖率/RMSE择优，任何对象失败整批诊断。
- 目标契约：保留有界局部位姿搜索；显式搜索半径、候选预算、可选ICP及Pose帧，不能把它并称任意SE3全局匹配。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：候选总数预算、边界/非交换旋转、ICP开关、稳定择优、任一对象失败不输出部分伪成功。
- 说明：纯位姿枚举可共享几何层，但与点云对齐/质量有关的节点归属点云包。

## 253 · register.rigid_fit

- 当前：`std-pointcloud` / `1.0.0` / 对应点三维刚体拟合。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/register_3d.cpp:251](../../packs/std-pointcloud/ops/register_3d.cpp#L251)。
- 端口：`source:PointCloud`, `target:PointCloud` → `transform:Transform`, `aligned:PointCloud`, `quality:Record`。
- 参数单位标注：maxRmse=m。
- 依赖：Eigen SVD；对应点逐项拟合，无PCL近邻查询；当前整包传递PCL；目标std-pointcloud配准portable子模块
- 当前契约：source:PointCloud, target:PointCloud → transform:Transform, aligned:PointCloud, quality:Record。米制source/target逐项对应点，double中心化SVD修正反射，输出source→target Transform/aligned/quality；拒绝共线/重合。
- 目标契约：对应点配准adapter保留；SVD几何内核可共享但输入点对应关系不改成最近邻，明确帧与误差门限。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：大坐标小偏移、反射、共线/重合、点数不等、变换后float32误差与通道旋转。
- 说明：不与ICP或混合约束拟合合并为含糊的register节点。

## 254 · segment.euclidean_clusters

- 当前：`std-pointcloud` / `1.0.0` / 三维欧式聚类。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/cloud_analysis.cpp:420](../../packs/std-pointcloud/ops/cloud_analysis.cpp#L420)。
- 端口：`cloud:PointCloud` → `cloud:PointCloud`, `indices:Indices`, `groups:Record`。
- 参数单位标注：tolerance=m。
- 依赖：PCL EuclideanClusterExtraction/kdtree；Eigen质心与稳定排序；目标std-pointcloud/segmentation后端
- 当前契约：cloud:PointCloud → cloud:PointCloud, indices:Indices, groups:Record。XYZ米制欧式连通聚类；输出来源云、Indices和Groups；按质心/点数/完整坐标确定局部组ID，超组数上限报错。
- 目标契约：保留空间连通性和局部稳定ID规范；明确ID不代表跨帧追踪，不接受有序轮廓分片的替代实现。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：点序置换、同质心簇、数量上限、NaN与原生组身份；不静默丢簇。
- 说明：欧式聚类保留PCL后端，不能把确定性ID误表述为对象追踪。

## 255 · segment.extract_indices

- 当前：`std-pointcloud` / `1.0.0` / 点云索引提取。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/segment_extract_indices.cpp:27](../../packs/std-pointcloud/ops/segment_extract_indices.cpp#L27)。
- 端口：`cloud:PointCloud`, `indices:Indices` → `selected:PointCloud`, `rest:PointCloud`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：C++标准库与原生PointCloud/Indices；算法无需PCL/OpenCV；当前整包传递链接PCL、Eigen、匹配CPU后端；目标portable对象模块解除这些非必要依赖
- 当前契约：cloud:PointCloud, indices:Indices → selected:PointCloud, rest:PointCloud。输入cloud和Indices，输出selected/rest互补子云，点属性随索引选择。
- 目标契约：std-pointcloud/portable索引操作；来源云身份与范围在入口验证，定义重复索引和补集顺序。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：错云索引、越界、重复、空选择、selected/rest通道对应与顺序。
- 说明：原生索引选择本身不使用PCL；通用Record集合算子不能代替大数组索引。

## 256 · segment.multi_planes

- 当前：`std-pointcloud` / `1.0.0` / 多平面顺序提取。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/cloud_analysis.cpp:503](../../packs/std-pointcloud/ops/cloud_analysis.cpp#L503)。
- 端口：`cloud:PointCloud` → `cloud:PointCloud`, `indices:Indices`, `groups:Record`, `planes:Record`, `rest:PointCloud`。
- 参数单位标注：distanceThreshold=m。
- 依赖：PCL SAC平面逐次提取；Eigen系数/统计和原生Groups；目标std-pointcloud/segmentation后端
- 当前契约：cloud:PointCloud → cloud:PointCloud, indices:Indices, groups:Record, planes:Record, rest:PointCloud。按剩余点反复米制RANSAC，输出cloud/indices/groups、cloud.Planes.v1及rest；法线符号规范并稳定排序组。
- 目标契约：保留多平面检测与剩余点管理；Plane代数内核共享，但空间搜索/物化仍在点云包。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：多个平面/不足点/残余噪声、平面系数与组ID对齐、原云身份和排序。
- 说明：与已有Plane类型的构造和纯距离计算边界分开。

## 257 · segment.ransac_plane

- 当前：`std-pointcloud` / `1.0.0` / RANSAC 平面拟合。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/segment_ransac_plane.cpp:92](../../packs/std-pointcloud/ops/segment_ransac_plane.cpp#L92)。
- 端口：`cloud:PointCloud` → `inliers:Indices`, `plane:Plane`。
- 参数单位标注：distanceThreshold=m, epsAngle=deg。
- 依赖：PCL SACSegmentation/SACMODEL_PLANE及法线/方向约束适配；目标std-pointcloud/segmentation后端
- 当前契约：cloud:PointCloud → inliers:Indices, plane:Plane。米制PointCloud拟合平面，输出Plane系数及原云inliers；距离阈值m、角度门限deg。
- 目标契约：保留RANSAC模型和方向/优化参数；平面规范化共享几何层，输出索引必须绑定来源云。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：平面系数、退化输入、约束角度、原云索引身份和随机性声明。
- 说明：检测空间平面留在点云包，Plane的构造/纯代数关系属于几何包。

## 258 · segment.region_growing_2d

- 当前：`std-pointcloud` / `1.0.0` / 区域生长 2D。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/segment_region_growing_2d.cpp:85](../../packs/std-pointcloud/ops/segment_region_growing_2d.cpp#L85)。
- 端口：`cloud:PointCloud` → `left:PointCloud`, `right:PointCloud`, `result:Record`。
- 参数单位标注：angleThresh=deg。
- 依赖：PCL RegionGrowing/search、Eigen二维邻域法线；目标std-pointcloud/segmentation模块
- 当前契约：cloud:PointCloud → left:PointCloud, right:PointCloud, result:Record。XY空间邻域法线区域生长，取最大的两簇并按x均值分left/right；不足两簇或小于近邻数报split_failed。
- 目标契约：保留空间近邻分簇与簇选择，声明不是按采样相邻列切分；输入米制XY，输出按原点属性对应。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：双弧/单簇/噪声、多簇排名、角阈值、最少点数及左右定义。
- 说明：与DTS按列的gap/jump分片不同，不能迁入std-profile后偷换实现。

## 259 · transform.apply

- 当前：`std-pointcloud` / `1.0.0` / 点云变换。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/transform_apply.cpp:56](../../packs/std-pointcloud/ops/transform_apply.cpp#L56)。
- 端口：`cloud:PointCloud`, `transform:Transform` → `cloud:PointCloud`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：C++标准库与原生PointCloud/Indices；算法无需PCL/OpenCV；当前整包传递链接PCL、Eigen、匹配CPU后端；目标portable对象模块解除这些非必要依赖
- 当前契约：cloud:PointCloud, transform:Transform → cloud:PointCloud。4×4变换作用到XYZ，法线旋转后归一化；保留intensity/RGB，不改变点数/顺序。
- 目标契约：点云应用adapter保留；矩阵/刚体/帧校验来自公共几何层，明确source→target和法线异常策略。
- 迁移前置：M1；M2；transform.make
- 关联算子：transform.make
- 验收：非交换变换、法线旋转、点序/通道不变、输入只读与非法矩阵拒绝。
- 说明：Transform构造下沉几何包；作用于大点数组的节点仍属于点云包。

## 260 · util.merge

- 当前：`std-pointcloud` / `1.0.0` / 点云合并。目标：`std-pointcloud`；保留并验边界；**M6**。
- 源码：[packs/std-pointcloud/ops/util_merge.cpp:62](../../packs/std-pointcloud/ops/util_merge.cpp#L62)。
- 端口：`a:PointCloud`, `b:PointCloud` → `cloud:PointCloud`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：C++标准库与原生PointCloud/Indices；算法无需PCL/OpenCV；当前整包传递链接PCL、Eigen、匹配CPU后端；目标portable对象模块解除这些非必要依赖
- 当前契约：a:PointCloud, b:PointCloud → cloud:PointCloud。按a后b拼接两片云；仅保留双方共有可选通道，单侧通道丢弃并告警。
- 目标契约：保留点云拼接语义；同帧同单位由入口契约校验，通道求交/告警保持，输出不得冒充原轮廓列序。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：a/b顺序、单侧通道告警、空云、帧/单位冲突和原输入只读。
- 说明：与cloud.group_concat的通道不一致报错策略不同，不能把两者当同一实现直接替换。
