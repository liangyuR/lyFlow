# 外部算子能力对照

[文档中心](README.md) · [补齐计划](operator-completion-plan.md) · [外部原始清单](<xyz-vision-lib 算子清单.md>)

审计日期：2026-11-14。初始基线：`32a3700e435c753df46916e1dc029a9d59b8d95e`；本表已更新至 P0/P1 与 P2 A01、A05 实现版本 **1.3.7**。条目状态表示限定用途覆盖，不是外部流程或协议兼容声明。

## 结论与口径

逐项核对原始清单的 **198 项**：178 个 C++ 节点、4 个预置组、16 个 Python 脚本。每项只计一次；一个外部节点的多个算法模式不另拆计数。P0 已补通用值/有界值列表、二维几何、图像区域统计和直线卡尺链；P1 新增55个算子，提供受校验云分组/工件集合、三维统计/局部配准、标定/定位和选定模型检测链。P1的47个清单条目均已核对限定切片，不把尚缺的模式和外部协议算作完成；实施入口与边界见[P1索引](p1-operator-analysis.md)。

不以名称相似、存在某数据类型、底层库可调用或“可以自己写 Python”作为已实现证据。表中的 LyFlow 入口包括现有算子与明确的平台组合；记录的是源码能力上界，实际安装组合仍以 `lyflow manifest` 为准，不假定可选包和用户节点已安装。

| 结论 | 判定标准 |
|---|---|
| 已有对应 | 已交付算子覆盖清单描述的核心用途；列明支持的模式和限制，不表示端口或配置兼容。 |
| 可组合替代 | 现有算子或宿主/编辑器能力已能完成明确用途；不依赖待开发算法。替代使用目标，不保证原协议。 |
| 部分覆盖 | 有实际可复用子能力，但缺关键模式、数据模型、输出或完整链；不能直接当作现成替代。 |
| 缺失 | 当前没有该功能的可用完整实现；数据容器、参数控件和扩展接口不计为算法。 |
| 待核实 | 清单未给足输入输出或业务定义，无法负责任地判断差距；先定契约，不能直接排实现工期。 |

这些是**清单条目数**，不折算“功能覆盖率”：例如部分覆盖可能仅有一个基础模式，预置组又依赖多个算法。现有/组合项的“无需补齐”只针对表中限定用途；新格式、外部协议和现场验收另立需求。

## 分类统计

| 外部分类 | 已有对应 | 可组合替代 | 部分覆盖 | 缺失 | 待核实 | 合计 |
|---|---:|---:|---:|---:|---:|---:|
| 输入 | 7 | 0 | 4 | 4 | 0 | 15 |
| 2D图像处理 | 22 | 1 | 5 | 9 | 3 | 40 |
| 3D点云处理 | 7 | 2 | 13 | 33 | 1 | 56 |
| 位姿处理 | 6 | 0 | 5 | 13 | 0 | 24 |
| 流数据运算 | 7 | 0 | 5 | 2 | 0 | 14 |
| 检查 | 0 | 0 | 0 | 5 | 0 | 5 |
| 深度学习 | 0 | 0 | 4 | 2 | 1 | 7 |
| 输出 | 1 | 2 | 2 | 4 | 0 | 9 |
| 可视化 | 0 | 2 | 4 | 2 | 0 | 8 |
| 预置组节点 | 0 | 0 | 0 | 4 | 0 | 4 |
| 内置 Python 脚本 | 6 | 0 | 3 | 7 | 0 | 16 |
| **合计** | **56** | **7** | **45** | **85** | **5** | **198** |

## 数据与证据边界

- 外部原文自述为 `xyz-vision-lib master@72d7885c`，本次不改写原清单。另抽查本机 xyz-vision-lib 辅助源码的加载、投影、输出、聚类、平面和 ICP；该目录无 Git 元数据，**无法证明与清单修订一致**。表中出现“辅助外部源码”时仅用于说明需核对的差异。
- LyFlow 证据来自实际注册与 compute、[数据模型](../core/include/lyflow/data.h)、[图契约](graph-doc.md)、[嵌入接口](embedding.md)和查看器。缺失判定结合注册表与类型/执行路径，不能通过一次关键词未命中推定。
- 原生包、工作流片段、编辑器/宿主能力、Python SDK 分别注明。`Record` 能承载 JSON，不表示已有工件、列表或相机标定的类型化操作；固定字段 `Bundle<kind>` 也不是动态列表。
- 原始清单另列的 30 个转换器、23 个查看器及未注册属性项不在这 198 项中；本次不将其悄悄计为已迁移或缺失。

## 最容易误判的对应

1. 二维 `register.icp_2d` / `segment.region_growing_2d` 不能代替三维处理；P1分别提供 `register.icp_3d` 和 `segment.euclidean_clusters`。后者仍不包含法线条件聚类，前者是需要初值的局部配准。
2. `segment.multi_planes` 已有多平面/原始组索引和剩余云；平面系数集合不等于外部GeometryVector、工件中心姿态或箱面识别。
3. 平面H、相机K/D与PnP分别由 `image.load_calib`、`image.load_camera_calib` / `image.undistort`、`image.board_pose` 提供；它们不等于完整手眼/工作空间标定或全局帧树。
4. `ml.onnx_run` 仍是单float32 Tensor输入/输出底座；P1围绕选定YOLOX及YOLOv8/11/26导出契约交付检测前后处理，不能推定实例分割、旋转检测、OCR或任意ONNX格式已支持。
5. 通用 `image.template_match` NCC与业务 `glue.locate` 都不是轮廓形状匹配或多视角三维定位。通用几何继续使用明确单位和类型的geometry节点，不推广业务角点约束。
6. `vision.Pose2D/3D.v1` 和对应集合已有带身份/帧/尺寸的受校验契约，仍与原生Transform分开；云分组用原生云/索引及小Record，不把数组塞JSON。针孔深度投影不是正交高度图，缓存不是会话FIFO。
7. Python 节点可扩展实现，但当前 SDK 仅直接交换 PointCloud、Image、Measurement、Record，不能把外部 16 个脚本计为已经移植。

## 逐项对照

“下一步”保留初始优先级并链接到任务；已交付的 P0/P1 行列明新入口、回归入口及剩余边界。P2 A01首项受限表达式另见[实现分析](p2-expression-analysis.md)，不表示整批P2完成。初始优先级不改写，P2条目BestFit/SearchPoseLocally的基础能力虽在P1先交付，其高级模式仍按原计划保留。

### 输入

| 外部项 | 结论 | LyFlow 当前入口 | 语义边界与证据 | 下一步 |
|---|---|---|---|---|
| `CloudLoader`<br>加载点云 | 已有对应 | `io.load_cloud`、`cloud.group_pack` | 单文件PCD、点顶点PLY及STL表面采样，保留支持的强度/法线/RGB、显式m/mm和初始刚体；字段类型、正文和资源先预检。输出云可包装分组；不持有Mesh拓扑、不移植目录查找或外部pose元数据协议。 [源码与边界](p1-analysis-cloud.md) · [回归](../packs/std-pointcloud/tests/test_p1_cloud.cpp) | P1 限定切片已实现 · [B06](operator-completion-plan.md#b06) |
| `GenerateGeometry2D`<br>生成2D几何特征 | 已有对应 | `geometry.point_2d`、`geometry.line_2d`、`geometry.circle_2d` | 点、无限线/线段、圆常量与可连接的带单位标量覆写；显示由查看器负责，不复制原绘图端口。 [1](../packs/std-geometry/ops/geometry.cpp) [2](../packs/std-geometry/tests/test_geometry.cpp) | P0 已交付限定用途 · [分析与边界](p0-operator-analysis.md) |
| `ImageLoader`<br>加载图像 | 已有对应 | `io.load_image` | 覆盖从指定文件路径读取图像的核心用途；按 LyFlow 的 RGB/RGBA 和位深约定工作，不承诺外部属性及路径策略兼容。 [1](../packs/std-image/ops/io_image.cpp) | 无需补齐 |
| `InputInfo`<br>输入信息 | 部分覆盖 | `value.string`、`value.field` | 顶层图参数按次绑定/覆盖文本与数值，可提取领域记录字段；公开 C ABI 直接注入仅支持点云/图像。外部 Request 工件对象及协议仍缺失。 [1](../packs/std-value/ops/register.cpp) [2](../packs/std-value/tests/test_values.cpp) | P0 已交付限定用途 · [分析与边界](p0-operator-analysis.md) |
| `InputNumber`<br>输入数字 | 已有对应 | `value.number` | 有限 float64 数值与显式单位；可通过图参数按次覆盖，无跨请求残留。 [1](../packs/std-value/ops/register.cpp) [2](../packs/std-value/tests/test_values.cpp) | P0 已交付限定用途 · [分析与边界](p0-operator-analysis.md) |
| `InputPlane`<br>生成平面 | 已有对应 | `geometry.plane` | 有限非零法向，连同d归一化；宿主通过顶层参数覆写常量。原生Plane不是外部PlaneVector/Request协议。 [1](../packs/std-geometry/ops/geometry.cpp) [2](../packs/std-geometry/tests/test_geometry.cpp) | P0 已交付限定用途 · [分析与边界](p0-operator-analysis.md) |
| `InputPrimitive2D`<br>生成2D位姿 | 已有对应 | `pose.construct_2d` | 受校验Pose2D：ID/帧、px/m、中心、弧度角、正尺寸及可选元数据；不是原Primitive2D端口协议。 [源码与边界](p1-analysis-pose.md) · [回归](../packs/std-geometry/tests/test_pose.cpp) | P1 限定切片已实现 · [B07](operator-completion-plan.md#b07) |
| `InputPrimitive3D`<br>生成3D位姿 | 已有对应 | `pose.construct_3d` | 受校验米制Pose3D，内禀XYZ弧度或xyzw单位四元数，规范q/-q并保留ID/帧/尺寸/元数据；不复制全局FrameTree。 [源码与边界](p1-analysis-pose.md) · [回归](../packs/std-geometry/tests/test_pose.cpp) | P1 限定切片已实现 · [B07](operator-completion-plan.md#b07) |
| `LoadCalibrationInfo`<br>加载标定信息 | 部分覆盖 | `image.load_calib`、`image.load_camera_calib`、显式Transform链 | 已分离平面H、相机K/D及已知外参；未实现外部机器人/手眼/工作空间文件格式和全局帧树更新。 [源码与边界](p1-analysis-image.md) · [回归](../packs/std-image/tests/test_image_p1.cpp) | P1 限定切片已实现 · [B03](operator-completion-plan.md#b03) |
| `LoadCameraCalibInfo`<br>加载相机标定信息 | 部分覆盖 | `image.load_camera_calib` | 已加载并校验版本化vision.CameraCalib.v1的分辨率、K/D、帧和单位；设备YAML、虚拟相机、手眼外参和全局设备帧树副作用仍未移植。 [源码与边界](p1-analysis-image.md) · [回归](../packs/std-image/tests/test_image_p1.cpp) | P1 限定切片已实现 · [B03](operator-completion-plan.md#b03) |
| `LoadWorkspaceInfo`<br>加载工作空间标定信息 | 缺失 | — | 缺少工作空间标定文件、坐标帧关联、边界及标定版本的加载和校验。 [1](../core/include/lyflow/data.h) [2](../docs/embedding.md) | P2 · [B03](operator-completion-plan.md#b03) |
| `ProfileProjector`<br>轮廓仪投影 | 部分覆盖 | `profile.project_pair` | 已知master/slave→common及可选common→output外参显式组合，保留通道并旋转法线；不猜原生云不存在的pose元数据，不自动推断主云中心或反写配方。 [源码与边界](p1-analysis-cloud.md) · [回归](../packs/std-pointcloud/tests/test_p1_cloud.cpp) | P1 限定切片已实现 · [B03](operator-completion-plan.md#b03) |
| `XYZCamera`<br>相机 | 缺失 | — | 宿主可注入 Image/PointCloud，但未交付该相机驱动、采集、重连、同步和标定关联；输入注入接口不计为设备支持。 [1](../docs/embedding.md) [2](../core/include/lyflow/data.h) | P3 · [D01](operator-completion-plan.md#d01) |
| `XYZLaserProfiler`<br>激光轮廓仪 | 缺失 | — | 宿主可注入 Image/PointCloud，但未交付该激光轮廓仪驱动、采集、重连、同步和标定关联；输入注入接口不计为设备支持。 [1](../docs/embedding.md) [2](../core/include/lyflow/data.h) | P3 · [D01](operator-completion-plan.md#d01) |
| `XYZLidar`<br>雷达 | 缺失 | — | 宿主可注入 Image/PointCloud，但未交付该雷达驱动、采集、重连、同步和标定关联；输入注入接口不计为设备支持。 [1](../docs/embedding.md) [2](../core/include/lyflow/data.h) | P3 · [D01](operator-completion-plan.md#d01) |

### 2D图像处理

| 外部项 | 结论 | LyFlow 当前入口 | 语义边界与证据 | 下一步 |
|---|---|---|---|---|
| `BarcodeDetector`<br>检测条形码/二维码 | 缺失 | — | 没有一维条码/二维码定位、解码及内容输出；图像读取和阈值处理不构成识码器。 [1](../packs/std-image/ops/register.cpp) | P2 · [B05](operator-completion-plan.md#b05) |
| `BaselineCalibration`<br>基准校正 | 待核实 | — | 清单未定义基准面、校正量及输入输出；平面 H 标定与模板偏移定位不能直接证明等价，先确认是透视校正、基准对齐还是测量补偿。 [1](../packs/std-image/ops/board_calib.cpp) [2](../packs/glue/ops/locate.cpp) | 待定义 · [U01](operator-completion-plan.md#u01) |
| `BlobDetector`<br>斑点检测 | 已有对应 | `image.components` | 稳定连通域、孔洞、像素面积、质心/包围框/旋转框、长宽比、排序和有界轮廓；不是外部Primitive2D对象，像素面积不等于轮廓多边形面积。 [1](../packs/std-image/README.md) [2](../packs/std-image/tests/test_image_p0.cpp) | P0 已交付限定用途 · [分析与边界](p0-operator-analysis.md) |
| `CalculateArea`<br>计算区域大小 | 可组合替代 | `image.threshold`、`image.region_stats` | 非空区域可用非零二值 mask 统计前景面积；黑色区域先反相阈值。单位为 px²，不分连通域。空区域当前返回 NaN Measurement，不是数值 0；物理面积需另有标定。 [1](../packs/std-image/ops/image_segment.cpp) | 无需补齐 |
| `CalculateEdgePosition`<br>计算边缘位置 | 已有对应 | `image.edge_positions` | 矩形ROI均值投影，正反扫描、极性及单/双/多峰亚像素位置；旋转ROI须先显式变换，不继承外部ROI对象。 [1](../packs/std-image/README.md) [2](../packs/std-image/tests/test_image_p0.cpp) | P0 已交付限定用途 · [分析与边界](p0-operator-analysis.md) |
| `CalculateGeometryDistance`<br>计算2D几何特征距离 | 已有对应 | `geometry.distance_2d` | 点/线/线段/圆组合的最短距离，并显式提供signed_clearance、first_anchor及圆心模式；拒绝混单位，不静默混用不同距离定义。 [1](../packs/std-geometry/ops/geometry.cpp) [2](../packs/std-geometry/tests/test_geometry.cpp) | P0 已交付限定用途 · [分析与边界](p0-operator-analysis.md) |
| `CalculateRingWidth`<br>计算圆环宽度范围 | 部分覆盖 | `image.ring_width` | 已实现亮/暗环带径向内外扫描、相邻边对/首末包络、周向min/max/mean及有效比例；原过圆心直径及全部四种模式未覆盖。 [源码与边界](p1-analysis-image.md) · [回归](../packs/std-image/tests/test_image_p1.cpp) | P1 限定切片已实现 · [A05](operator-completion-plan.md#a05) |
| `CalculateWidthRange`<br>计算宽度范围 | 已有对应 | `image.width_range` | 多条带宽扫描、内/外尺寸边对、最小/最大/平均宽度与有效比例；输出px。支持轴对齐ROI，不声称任意旋转工件的全局极值。 [1](../packs/std-image/README.md) [2](../packs/std-image/tests/test_image_p0.cpp) | P0 已交付限定用途 · [分析与边界](p0-operator-analysis.md) |
| `CalibBoardPoseEstimation`<br>标定板定位 | 已有对应 | `image.board_pose` | 棋盘SB/子像素角点、显式K/D和米制格距、PnP候选正深度/重投影/歧义门控；board→camera Transform原点是首内角点，Pose3D为板中心。Marker/Charuco另列后续。 [源码与边界](p1-analysis-image.md) · [回归](../packs/std-image/tests/test_image_p1.cpp) | P1 限定切片已实现 · [B03](operator-completion-plan.md#b03) |
| `CheckImageChange`<br>检查图像变化 | 已有对应 | `image.change_stats` | 差分阈值、可选掩膜/形态开运算、面积门限和different布尔；luma/max_abs通道策略显式。 [1](../packs/std-image/README.md) [2](../packs/std-image/tests/test_image_p0.cpp) | P0 已交付限定用途 · [分析与边界](p0-operator-analysis.md) |
| `ConcatImages`<br>图片拼接 | 已有对应 | `image.concat` | 两图水平/垂直排布可组合多图网格，明确填充值及同位深/通道/比例条件；支持u8/u16/f32和RGBA，不做全景配准或隐式颜色转换。 [源码与边界](p1-analysis-image.md) · [回归](../packs/std-image/tests/test_image_p1.cpp) | P1 限定切片已实现 · [A04](operator-completion-plan.md#a04) |
| `ConvertImageType`<br>转换图片类型 | 已有对应 | `image.convert_channels`、`image.normalize` | 灰度/RGB/RGBA互转及现有位深/范围转换；遵守RGB顺序，位深支持矩阵见包指南。 [1](../packs/std-image/README.md) [2](../packs/std-image/tests/test_image_p0.cpp) | P0 已交付限定用途 · [分析与边界](p0-operator-analysis.md) |
| `CropImageByROI`<br>图片ROI裁剪 | 已有对应 | `image.crop` | 按参数或像素 Box2D 裁剪轴对齐矩形 ROI，越界取交集；不宣称支持旋转框、多边形或逐实例 mask，亦不保证外部 ROI 数据结构兼容。 [1](../packs/std-image/ops/image_basic.cpp) | 无需补齐 |
| `DefectDetector`<br>深度图缺陷检测 | 缺失 | — | 已有针孔深度反投影与灰度阈值/面积基础，但无产品平面残差图、毫米高度阈值、缺陷连通域及物理面积统计的完整深度缺陷流程。 [1](../packs/std-image/ops/image_cloud.cpp) [2](../packs/std-image/ops/image_segment.cpp) | P2 · [C01](operator-completion-plan.md#c01) |
| `DetectCircleEdge`<br>检测圆形边缘 | 部分覆盖 | `image.circle_caliper` | 已实现圆/圆弧ROI径向亚像素采样、极性/方向、确定性RANSAC和中心化拟合及直边拒绝；固定半径约束拟合模式仍未覆盖。 [源码与边界](p1-analysis-image.md) · [回归](../packs/std-image/tests/test_image_p1.cpp) | P1 限定切片已实现 · [A05](operator-completion-plan.md#a05) |
| `DetectHighLights`<br>检测高光直线 | 已有对应 | `image.highlights` | 亮脊检测、真实支持点分组/TLS有限线段及长度/RMS/最大误差/覆盖率/对比质量；支持水平、垂直、斜线及像素轴对齐/旋转ROI，输出同ID的旋转Pose2D区域、线段记录、候选mask与保位深叠图。原生Box2D仍轴对齐，不兼容外部Qt配置协议。 [源码与边界](p2-highlights-analysis.md) · [回归](../packs/std-image/tests/test_highlights.cpp) · [跨接口](../core/tests/integration/p2_highlights.cpp) | P2 A05 限定用途已实现 · [A05](operator-completion-plan.md#a05) |
| `DetectLineEdge`<br>检测直线边缘 | 已有对应 | `image.line_caliper` | 轴对齐ROI的带宽扫描、方向/极性、亚像素点与鲁棒直线拟合；返回有效比例和诊断，失败不造Line2D。 [1](../packs/std-image/README.md) [2](../packs/std-image/tests/test_image_p0.cpp) | P0 已交付限定用途 · [分析与边界](p0-operator-analysis.md) |
| `EdgePoseRefiner`<br>配准工件2D边缘 | 缺失 | — | 局部图像 NCC 与 XY 平面 ICP 均不解决图像边缘约束下的相机 3D 位姿优化；P1已有相机参数，仍缺模型边缘投影与6DoF残差优化。 [1](../packs/glue/ops/locate.cpp) [2](../packs/std-pointcloud/ops/register_icp_2d.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `FindCircles`<br>圆孔检测 | 缺失 | — | image.find_circle 只输出图像平面的二维圆，不能从椭圆/圆投影恢复空间圆孔的 3D 中心、法向和尺度；本项不能按名字映射为已有。 [1](../packs/std-image/ops/image_segment.cpp) | P2 · [C01](operator-completion-plan.md#c01) |
| `GeneratePrimitivesMask`<br>生成图像掩膜 | 部分覆盖 | `image.instances_mask` | Pose2D旋转矩形光栅化为原生u16标签/u8二值图与稳定ID表，重叠first/last/error显式；任意形状资产mask、对象自带mask和3D相机投影未覆盖。 [源码与边界](p1-analysis-image.md) · [回归](../packs/std-image/tests/test_image_p1.cpp) | P1 限定切片已实现 · [A04](operator-completion-plan.md#a04) |
| `Geometry2DIntersection`<br>两几何体中点/交点 | 已有对应 | `geometry.midpoint_2d`、`geometry.project_point_2d`、`geometry.intersect_2d`、`geometry.point_at` | 中点、垂足与求交拆为独立操作；有限多解、无解、重合分开表达，支持线段及圆。几何单位为m/px。 [1](../packs/std-geometry/ops/geometry.cpp) [2](../packs/std-geometry/tests/test_geometry.cpp) | P0 已交付限定用途 · [分析与边界](p0-operator-analysis.md) |
| `GetMaskFeature`<br>获取掩膜特征 | 待核实 | `image.region_stats` | 已有 mask 的选中像素数、均值和轴对齐包围框；清单未列出特征集合，不能据此判定是否还要求质心、周长、孔洞、圆度、方向或逐实例特征。 [1](../packs/std-image/ops/image_segment.cpp) | 待定义 · [U01](operator-completion-plan.md#u01) |
| `GrayScaleStatistics`<br>灰度统计 | 已有对应 | `image.statistics` | 通道/掩膜统计min/max/mean/median/mode；u8/u16桶统计，f32有界精确值统计。空结果valid=false，众数并列取较小值，ROI/范围通过掩膜组合。 [1](../packs/std-image/README.md) [2](../packs/std-image/tests/test_image_p0.cpp) | P0 已交付限定用途 · [分析与边界](p0-operator-analysis.md) |
| `ImageColorAdjustment`<br>图像颜色调整 | 已有对应 | `image.color_adjust` | 亮度/对比度/Gamma、RGB增益偏移、HSV色相/饱和度，保持位深和alpha；原Auto直方图均衡模式未移植，转换范围仍复用现有节点。 [源码与边界](p1-analysis-image.md) · [回归](../packs/std-image/tests/test_image_p1.cpp) | P1 限定切片已实现 · [A04](operator-completion-plan.md#a04) |
| `ImageDifference`<br>图像差分 | 部分覆盖 | `image.arithmetic`、`image.difference_mask` | 正/负/双向差分及共同Canny边缘距离抑制，可选有响应/位移门限的平移配准；原ORB/SE(2)完整配准模式仍缺失。 [1](../packs/std-image/README.md) [2](../packs/std-image/tests/test_image_p0.cpp) | P0 已交付限定用途 · [分析与边界](p0-operator-analysis.md) |
| `ImageFilter`<br>图像滤波 | 已有对应 | `image.blur` | 已实现 Gaussian、median、mean 三种经典二维滤波；不宣称包含全部滤波核、双边/导向滤波或深度图边缘保持/无效值处理。 [1](../packs/std-image/ops/image_basic.cpp) | 无需补齐 |
| `ImageHomography`<br>图像透视校正 | 已有对应 | `image.warp_perspective` | 显式原图像素→m/mm平面H、固定输出尺寸/原点/像素密度及有效采样mask；不把H伪装成刚体Transform，不自动推导外部工作空间/虚拟相机。 [源码与边界](p1-analysis-image.md) · [回归](../packs/std-image/tests/test_image_p1.cpp) | P1 限定切片已实现 · [B03](operator-completion-plan.md#b03) |
| `ImageMorphology`<br>图像形态学操作 | 已有对应 | `image.morphology`、`image.instances_morphology` | 既有常规形态学，加逐标签独立处理、稳定ID、明确冲突策略和原标签保护；不沿用输入顺序不明确的多实例吞并规则。 [源码与边界](p1-analysis-image.md) · [回归](../packs/std-image/tests/test_image_p1.cpp) | P1 限定切片已实现 · [A04](operator-completion-plan.md#a04) |
| `ImageResize`<br>图像缩放 | 已有对应 | `image.resize` | 支持比例或目标尺寸缩放，area/linear/cubic/nearest 插值；参数名称、默认插值和外部图片结构仍非兼容承诺。 [1](../packs/std-image/ops/image_basic.cpp) | 无需补齐 |
| `ImageThresholdOperation`<br>图像二值化 | 已有对应 | `image.to_gray`、`image.threshold`、`image.range_mask` | 固定/Otsu/自适应灰度阈值与Gray/RGB/HSV范围掩膜均有；颜色顺序及各位深限制明确。 [1](../packs/std-image/README.md) [2](../packs/std-image/tests/test_image_p0.cpp) | P0 已交付限定用途 · [分析与边界](p0-operator-analysis.md) |
| `LoadForeground`<br>加载料箱区域信息 | 待核实 | — | 没有专用料箱区域加载器；清单未说明文件格式、二维/三维区域与坐标系，不能把 image.PlaneCalib 读取视为对应，先确认是否纳入机器人场景。 [1](../packs/std-image/ops/register.cpp) [2](../packs/std-image/ops/load_calib.cpp) | 待定义 · [U01](operator-completion-plan.md#u01) |
| `MarkerPoseEstimation`<br>识别码定位 | 缺失 | — | 没有标记码解码和带相机内参的空间位姿估计；棋盘格平面 H 不能替代 marker 检测和 PnP。 [1](../packs/std-image/ops/register.cpp) [2](../packs/std-image/ops/board_calib.cpp) | P2 · [B03](operator-completion-plan.md#b03) |
| `MultiViewMatcher`<br>多视角定位工件 | 缺失 | — | 已有单图NCC、局部三维ICP及相机内参，但无多视角模型融合与工件三维位姿联合求解；不等同二维模板匹配。 [1](../packs/glue/ops/locate.cpp) [2](../packs/std-pointcloud/ops/register_icp_2d.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `ProfileSlicer`<br>轮廓截面器 | 缺失 | — | glue 卡尺采样的是二维灰度，gap 读取和处理二维轮廓点；均没有从线扫深度图连续截取高度截面及计算截面属性的图像算子。 [1](../packs/glue/ops/part_edges.cpp) [2](../packs/gap/ops/io.cpp) [3](../packs/gap/ops/blocks.cpp) | P2 · [C01](operator-completion-plan.md#c01) |
| `QrCodeRefine`<br>二维码增强 | 缺失 | — | 通用滤波和阈值不具备二维码结构约束、定位/重采样与识别效果反馈；首轮应依赖条码解码能力并确认具体增强目标。 [1](../packs/std-image/ops/image_basic.cpp) [2](../packs/std-image/ops/register.cpp) | P2 · [B05](operator-completion-plan.md#b05) |
| `ROIGenerator`<br>生成感兴趣区域 | 已有对应 | `image.roi_mask` | 矩形、旋转矩形、圆、圆环弧、多边形及反选/掩膜相交；像素工厂不等于相机/工件参考系变换。 [1](../packs/std-image/README.md) [2](../packs/std-image/tests/test_image_p0.cpp) | P0 已交付限定用途 · [分析与边界](p0-operator-analysis.md) |
| `SBMEstimator`<br>2D形状匹配 | 缺失 | — | glue.locate 是灰度归一化互相关及小角度扫描，不是形状/边缘模型匹配；缺形状模型创建、尺度/角度搜索及遮挡鲁棒匹配。 [1](../packs/glue/ops/locate.cpp) | P2 · [B04](operator-completion-plan.md#b04) |
| `ShadingCorrection`<br>背景修正 | 部分覆盖 | `image.shading_correct` | 已提供显式平场/有界高斯背景、差分/除法、保持平均亮度和小分母拒绝；原方向性顶帽/黑帽与噪声阈值模式未等价移植。 [源码与边界](p1-analysis-image.md) · [回归](../packs/std-image/tests/test_image_p1.cpp) | P1 限定切片已实现 · [A04](operator-completion-plan.md#a04) |
| `TemplateMatcher`<br>2D模板匹配 | 已有对应 | `image.template_build`、`image.template_match` | 原生模板图像+小型元数据，掩膜零均值NCC、离散旋转/尺度、多实例及旋转框NMS；使用实际重采样尺寸。无目标为空集合，不宣称亚像素或轮廓形状匹配。 [源码与边界](p1-analysis-image.md) · [回归](../packs/std-image/tests/test_image_p1.cpp) | P1 限定切片已实现 · [B04](operator-completion-plan.md#b04) |
| `UndistortImage`<br>图像去畸变 | 已有对应 | `image.load_camera_calib`、`image.undistort` | 校验K/D与分辨率后remap，输出零畸变内参和有效mask；缓存按完整参数及尺寸有界管理，拒绝不匹配预览比例，不复用同ID旧映射。 [源码与边界](p1-analysis-image.md) · [回归](../packs/std-image/tests/test_image_p1.cpp) | P1 限定切片已实现 · [B03](operator-completion-plan.md#b03) |

### 3D点云处理

| 外部项 | 结论 | LyFlow 当前入口 | 语义边界与证据 | 下一步 |
|---|---|---|---|---|
| `BestFit`<br>最佳适应算法 | 部分覆盖 | `register.rigid_fit` | 已提供等长三维对应点的double中心化Kabsch、反射修正和退化拒绝，质量以交付float32变换重算；混合点线面和加权3-2-1专用规则仍未覆盖。 [源码与边界](p1-analysis-cloud.md) · [回归](../packs/std-pointcloud/tests/test_p1_cloud.cpp) | P2 · [B02](operator-completion-plan.md#b02)；P1已补基础切片 |
| `BoxEstimator`<br>计算包围盒 | 部分覆盖 | `cloud.statistics`、`cloud.bounds_obb` | 已输出AABB/PCA方向包围盒、中心、尺寸及变换；外部实际为平面箱面最小面积矩形并带边缘去噪/法向约束，PCA盒不能冒充该专项估计或最小体积盒。 [源码与边界](p1-analysis-cloud.md) · [回归](../packs/std-pointcloud/tests/test_p1_cloud.cpp) | P1 限定切片已实现 · [B01](operator-completion-plan.md#b01) |
| `CalObjCoverRate`<br>工件遮挡率计算 | 缺失 | — | 已有Pose3D/相机内参基础，但缺少可见区域、遮挡模型及覆盖率算法；点数与包围盒不能替代遮挡率。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `CalculateContainerBox`<br>计算集装箱内切盒 | 缺失 | — | 暂无由集装箱点云估计相机系内切盒的检测与约束求解；filter.crop_box 仅消费已知边界。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-pointcloud/ops/filter_crop_box.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `CalculateHeightByNeighborCloud`<br>计算物体高度 | 缺失 | — | 已有Pose3D及全云统计，但缺少按工件邻域选择基准面并估计相对高度的完整链；gap.flush不能替代该三维规则。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/gap/ops/measure.cpp) | P2 · [B01](operator-completion-plan.md#b01) |
| `CalculateInstancePrimitivePose`<br>计算实例工件位姿 | 缺失 | — | 已有二维检测和Pose3D契约，但尚无结合深度、相机与工件先验恢复实例三维姿态的通用桥接；检测框不等于6DoF估计。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `CalculatePalletPose`<br>垛型匹配 | 待核实 | — | 清单仅给出“垛型匹配”而无描述；需确认输入、输出、匹配目标及失败条件后才能判定替代关系和投入。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | 待定义 · [U01](operator-completion-plan.md#u01) |
| `CalculatePlacingPosition`<br>计算放置点位 | 缺失 | — | 已有Pose3D尺寸字段，但无放置规划、支撑/碰撞/可达性算法；工件模型和Transform不提供规划能力。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `CalculatePrimitiveDimension`<br>计算3D尺寸 | 缺失 | — | 已有工件实例及基础统计，但缺少抓取前后场景绑定、对象关联和高度差尺寸估计；Measurement只是结果载荷。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `CalculateTopLayerHeight`<br>计算最高层高度 | 缺失 | — | 无按层厚切分并返回最高层高度及点云的算子；范围滤波需预先给定阈值，不能自动估计层。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-pointcloud/ops/filter_passthrough.cpp) | P2 · [B01](operator-completion-plan.md#b01) |
| `CloudCutByPrimitives`<br>用工件切割点云 | 缺失 | `filter.crop_box` | 已有带 Transform 的盒裁剪可覆盖单个已知长方体；P1已有三维工件列表，仍缺少消费该列表的形状适配、批量裁剪和碰撞语义。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-pointcloud/ops/filter_crop_box.cpp) | P2 · [B07](operator-completion-plan.md#b07) |
| `CloudCutByRoi`<br>ROI裁剪点云 | 已有对应 | `filter.crop_box` | filter.crop_box 按米制三维 min/max 盒过滤，可接可选 Transform 实现局部框并保留内部/外部；外部 ROI 元数据仍需显式转换，不承诺结果逐点一致。 不包含外部 Roi/Workspace 对象和 organized/cropped-organized 输出。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-pointcloud/ops/filter_crop_box.cpp) | 无需补齐 |
| `CloudCutByTote`<br>去除料框外点云 | 可组合替代 | `transform.make`、`filter.crop_box` | 在宿主明确提供料框边界与坐标变换时，可用盒裁剪保留料框内部点云；不包含料框自动识别、壁厚估计或机器人坐标树。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-pointcloud/ops/transform_make.cpp) [4](../packs/std-pointcloud/ops/filter_crop_box.cpp) | 无需补齐 |
| `CloudDraggingNoiseRemoval`<br>去除拖拽噪声 | 缺失 | `filter.statistical_outlier`、`filter.radius_outlier` | 统计/半径离群滤波已有，但未实现利用可抓取箱体几何识别其周围拖拽噪声的业务算法。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-pointcloud/ops/filter_statistical_outlier.cpp) [4](../packs/std-pointcloud/ops/filter_radius_outlier.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `CloudEdgeExtract`<br>点云边缘提取 | 缺失 | `features.normals` | 暂无通用三维点云边缘/深度不连续边界提取；features.normals 只提供法线，二维图像边缘不能自动保留点云对应关系。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-pointcloud/ops/features_normals.cpp) | P2 · [B02](operator-completion-plan.md#b02) |
| `CloudFillHoles`<br>点云孔洞填补 | 缺失 | `image.blur` | 清单要求以闭运算定位孔洞并补深度；已有图像中值平滑，但无深度无效值/孔洞掩码/点云像素对应和重建契约。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-image/ops/image_basic.cpp) [4](../packs/std-image/ops/image_cloud.cpp) | P2 · [C01](operator-completion-plan.md#c01) |
| `CloudFilter`<br>点云过滤 | 已有对应 | `filter.passthrough`、`transform.apply` | filter.passthrough 支持 X/Y/Z/intensity 范围及反选，覆盖参考坐标轴阈值过滤的核心用途；任意参考系应先显式变换，阈值边界包含性与外部实现须单独验收。 阈值为参数常量，尚无动态 Double 阈值端口、严格不等式及双云输出。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-pointcloud/ops/filter_passthrough.cpp) [4](../packs/std-pointcloud/ops/transform_apply.cpp) | 无需补齐 |
| `CloudICP`<br>3D ICP匹配 | 部分覆盖 | `register.icp_3d`、`cloud.group_get` | 真正三维点点/点面SE(3) ICP，显式source→target初值、收敛/匹配比例/实际float32输出RMSE门限及退化拒绝；外部工件初姿更新和CloudVector批处理协议未整体移植。 [源码与边界](p1-analysis-cloud.md) · [回归](../packs/std-pointcloud/tests/test_p1_cloud.cpp) | P1 限定切片已实现 · [B02](operator-completion-plan.md#b02) |
| `CloudNoiseRemoval`<br>去除点云噪声 | 已有对应 | `filter.statistical_outlier`、`filter.radius_outlier` | 统计离群点和半径邻域离群点剔除均已实现，输出保留可选通道；覆盖清单离群点去噪用途，参数含义和边界数值仍需迁移验收。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-pointcloud/ops/filter_statistical_outlier.cpp) [4](../packs/std-pointcloud/ops/filter_radius_outlier.cpp) | 无需补齐 |
| `CloudPCA`<br>点云主成分分析 | 已有对应 | `cloud.pca` | 全体有限输入的double中心化协方差、质心/主轴/特征值/右手姿态；符号规范化，重复特征值与退化用orientationStable标记，不冒充稳定工件朝向。 [源码与边界](p1-analysis-cloud.md) · [回归](../packs/std-pointcloud/tests/test_p1_cloud.cpp) | P1 限定切片已实现 · [B01](operator-completion-plan.md#b01) |
| `CloudROIExtract`<br>根据2D检测结果提取点云 | 缺失 | `segment.extract_indices` | 已有实例标签和云分组载荷，仍缺少organized cloud与图像ROI/实例掩码的逐像素索引映射；extract_indices 只消费既有同源索引，无法从检测结果生成映射。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-pointcloud/ops/segment_extract_indices.cpp) [4](../packs/std-image/ops/image_cloud.cpp) | P2 · [C01](operator-completion-plan.md#c01) |
| `CloudRemovalByPrimitives`<br>去除识别工件的点云 | 缺失 | `filter.crop_box` | 反选盒裁剪可删除单个已知长方体内点，P1已有三维工件数组，仍无按其任意形状批量删除点云的执行节点。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-pointcloud/ops/filter_crop_box.cpp) | P2 · [B07](operator-completion-plan.md#b07) |
| `CloudSample`<br>降采样点云 | 已有对应 | `filter.voxel_grid`、`filter.random_sample` | filter.voxel_grid 和 filter.random_sample 提供体素/随机降采样；保留可选通道的规则见实际实现，不承诺包含外部所有采样模式。 不复刻外部自适应 leaf 二次采样策略。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-pointcloud/ops/filter_voxel_grid.cpp) [4](../packs/std-pointcloud/ops/filter_random_sample.cpp) | 无需补齐 |
| `CloudToGeometry`<br>点云构建几何体 | 部分覆盖 | `segment.ransac_plane`、`segment.extract_indices` | 可提取单一平面参数及内点；尚无清单要求的有限矩形平面片/Geometry3D 输出和姿态尺寸估计。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-pointcloud/ops/segment_ransac_plane.cpp) [4](../packs/std-pointcloud/ops/segment_extract_indices.cpp) | P2 · [B01](operator-completion-plan.md#b01) |
| `CloudTrans`<br>变换点云坐标系 | 已有对应 | `transform.make`、`transform.apply` | transform.apply 可按显式 4x4 矩阵变换 XYZ 并旋转法线，保留强度/RGB；相机/料框/机器人外参和方向由宿主显式提供，不含命名坐标树自动查找。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-pointcloud/ops/transform_make.cpp) [4](../packs/std-pointcloud/ops/transform_apply.cpp) | 无需补齐 |
| `CloudViewSampling`<br>点云视点采样 | 缺失 | `filter.voxel_grid`、`filter.random_sample` | 体素/随机采样不判断视点遮挡，暂无模型表面可见点采样或隐面移除。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-pointcloud/ops/filter_voxel_grid.cpp) [4](../packs/std-pointcloud/ops/filter_random_sample.cpp) | P2 · [B02](operator-completion-plan.md#b02) |
| `ClustersToPrimitives`<br>聚类结果生成工件 | 缺失 | `segment.ransac_plane` | 已有三维簇、逐组统计和Pose3D契约；仍无把逐簇箱面矩形拟合及业务工件姿态组合为完整输出的节点，PCA盒不自动等于箱面。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-pointcloud/ops/segment_ransac_plane.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `ConstructHeightMap`<br>生成高度图 | 缺失 | `cloud.to_depth_image` | cloud.to_depth_image 是相机针孔 Z-buffer 深度投影，不能替代向料框底面投影的正交高度图；需定义栅格分辨率、原点、参考面、聚合及无效值。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-image/ops/image_cloud.cpp) | P2 · [C01](operator-completion-plan.md#c01) |
| `ConstructOrthographicProjectionMap`<br>生成正交投影图 | 缺失 | `cloud.to_depth_image` | 尚无工作空间底面正交栅格和多特征图输出；相机针孔投影及普通灰度化不等价。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-image/ops/image_cloud.cpp) | P2 · [C01](operator-completion-plan.md#c01) |
| `ConvexPrimitiveOptimization`<br>凸起优化 | 缺失 | — | 已有基础实例模型，仍无凸起箱/袋的抓取姿态优化和SKU业务约束。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `CorrectPrimitivePitch`<br>校正工件3D位姿pitch角度 | 缺失 | `transform.make` | 无基于箱体顶面凹陷与邻域点云的工件 pitch 校正；手动 Transform 只能应用已知角度。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-pointcloud/ops/transform_make.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `DepthFilter`<br>深度图滤波 | 部分覆盖 | `image.blur` | image.blur 提供常规均值/高斯/中值平滑，支持的图像位深见实现；尚无明确深度无效值、孔洞保持、边缘保持和点云标定联动契约，不能宣称与外部深度滤波数值等价。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-image/ops/image_basic.cpp) | P2 · [C01](operator-completion-plan.md#c01) |
| `DimensionBasedPoseGenerator`<br>已知尺寸Pose生成器 | 缺失 | — | 已有Pose3D尺寸字段，仍无结合SKU与膜包表面约束生成三维候选的算法。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `FilmWrapPrimitiveOptimization`<br>膜包工件优化 | 缺失 | — | 未实现膜包工件已知尺寸与表面噪声约束的位姿/尺寸优化。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `FilterByClustering`<br>点云聚类滤波 | 部分覆盖 | `segment.euclidean_clusters` | 三维欧式连通分量、显式簇大小范围与任意有效簇结果；保留原生成员索引。organized像素邻接及独立XY/Z阈值DFS模式未覆盖。 [源码与边界](p1-analysis-cloud.md) · [回归](../packs/std-pointcloud/tests/test_p1_cloud.cpp) | P1 限定切片已实现 · [B01](operator-completion-plan.md#b01) |
| `FilterGroupCloudAndPrimitives`<br>对分组（分区）的点云和工件进行过滤 | 缺失 | — | 已有云分组和工件集合，仍无两者按分区绑定、按高度排序和跨组一致过滤的业务算法。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `FindCloudPlane`<br>拟合平面 | 部分覆盖 | `segment.multi_planes` | 顺序RANSAC多平面、规范系数/质心/组ID、原始索引和剩余云已实现；未直接交付外部GeometryVector/工件中心姿态及organized云专用协议。 [源码与边界](p1-analysis-cloud.md) · [回归](../packs/std-pointcloud/tests/test_p1_cloud.cpp) | P1 限定切片已实现 · [B01](operator-completion-plan.md#b01) |
| `GapFlushDetection`<br>间隙面差检测 | 可组合替代 | `gap.read_scan`、`gap.locate_template`、`gap.locate_model`、`gap.role_line`、`gap.ref_point`、`gap.seam_circles`、`gap.flush`、`gap.gap`、`gap.measure_reference` | 已有双线扫读取/测量帧变换/模板或模型定位/角色拟合/间隙面差测量链；须提供现有 gap 标定与 ROI 配置并按缝型选择算法，不承诺外部高层节点端口或结果逐项兼容。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/gap/ops/blocks.cpp) [4](../packs/gap/ops/measure.cpp) [5](../packs/gap/ops/measure_reference.cpp) | 无需补齐 |
| `GenerateBoxCloud`<br>生成Box点云 | 缺失 | `gen.synthetic` | gen.synthetic 生成合成测试点云，不消费三维工件长方体；虽已有Pose3D尺寸/姿态，仍缺少据此采样箱体表面点云的算法。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../core/src/builtin_ops.cpp) | P2 · [B06](operator-completion-plan.md#b06) |
| `GenerateFOVBlindAreaObstacle`<br>生成视野盲区内碰撞体 | 缺失 | — | 无相机视野盲区/前壁推断及机器人碰撞体输出；点云可视化不等于安全碰撞模型。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `GetCloudClusters`<br>点云聚类 | 部分覆盖 | `segment.euclidean_clusters`、`cloud.group_statistics` | 已实现真正XYZ欧式聚类、大小筛选、逐组统计与几何规范排序ID；超过组数上限报错。法线条件聚类仍缺失，ID不承诺跨帧跟踪。 [源码与边界](p1-analysis-cloud.md) · [回归](../packs/std-pointcloud/tests/test_p1_cloud.cpp) | P1 限定切片已实现 · [B01](operator-completion-plan.md#b01) |
| `GetCloudHeight`<br>计算点云的最大高度 | 部分覆盖 | `cloud.statistics` | 已输出输入帧最大Z的米制Measurement及有界统计，空云无效；Request点邻域ROI、workspace安全高度和相对高度规则仍未覆盖。 [源码与边界](p1-analysis-cloud.md) · [回归](../packs/std-pointcloud/tests/test_p1_cloud.cpp) | P1 限定切片已实现 · [B01](operator-completion-plan.md#b01) |
| `GroupCloudByPartition`<br>根据分区对点云分组 | 缺失 | `filter.crop_box` | 没有分区几何/分区标识输入与点云组集合输出；单框裁剪可手工处理固定单区，不能替代动态分组。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-pointcloud/ops/filter_crop_box.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `GroupPrimitivesByPartition`<br>根据分区对工件分组 | 缺失 | — | 已有三维工件列表，但未提供分区类型及把工件按空间分区分组的执行节点。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `HeightExtraction`<br>深度图高度抽取 | 缺失 | `image.normalize` | image.normalize 仅按像素值映射/位深转换；未实现基于标定参考平面的深度到实际高度计算。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-image/ops/image_basic.cpp) [4](../packs/std-image/ops/image_cloud.cpp) | P2 · [B01](operator-completion-plan.md#b01) |
| `HomogeneousPrimitiveOptimization`<br>单拆工件优化 | 缺失 | — | 未实现单拆场景箱体补全/重拟合和工件姿态优化；需明确工件几何与抓取业务约束。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `InsertionCheck`<br>纸箱插入Pose优化 | 缺失 | — | 缺少邻域工件碰撞、插入方向可行性与纸箱姿态优化；不能由普通裁剪/刚体变换直接代替。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `MergeClouds`<br>合并两个点云 | 已有对应 | `util.merge` | util.merge 拼接两个 PointCloud；不做配准或重采样，可选通道只保留两侧兼容交集并在丢弃时提示，外部元数据需转换。 不会自动清 NaN、配准或去重。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-pointcloud/ops/util_merge.cpp) | 无需补齐 |
| `MonoGapFlushDetection`<br>单线间隙面差测量 | 部分覆盖 | `profile.gap_flush` | 单PointCloud、明确平面与双区间，输出采样边缘gap/同referenceX有符号flush及实际Line2D拟合质量；少点、竖直、超限外推和残差拒绝。圆角/槽/斜投影等专用缝型仍未整体覆盖。 [源码与边界](p1-analysis-cloud.md) · [回归](../packs/std-pointcloud/tests/test_p1_cloud.cpp) | P1 限定切片已实现 · [A05](operator-completion-plan.md#a05) |
| `ProjectCloudToCamera`<br>投影点云到相机坐标系 | 部分覆盖 | `transform.apply`、`cloud.to_depth_image` | transform.apply 加 cloud.to_depth_image 可完成已知相机外参下的针孔深度投影；PointCloud 无 width/height 或像素映射，未恢复 organized cloud。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-pointcloud/ops/transform_apply.cpp) [4](../packs/std-image/ops/image_cloud.cpp) | P2 · [C01](operator-completion-plan.md#c01) |
| `ProjectPrim3DTo2D`<br>投影3D几何体 | 缺失 | `cloud.to_depth_image` | 已有点云到深度图投影，但没有三维几何体输入、轮廓/边框投影及遮挡/裁剪策略。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-image/ops/image_cloud.cpp) | P2 · [B07](operator-completion-plan.md#b07) |
| `SearchPoseLocally`<br>局部搜索最匹配位姿 | 部分覆盖 | `register.local_search_3d` | 已实现原始初值、XYZ正负平移及Z正负旋转至多9个ICP候选，按实际输出质量筛选；外部全部轴组合网格/分级搜索及工件姿态协议仍未覆盖。 [源码与边界](p1-analysis-cloud.md) · [回归](../packs/std-pointcloud/tests/test_p1_cloud.cpp) | P2 · [B02](operator-completion-plan.md#b02)；P1已补基础切片 |
| `SignalEvaluator`<br>轮廓分析 | 部分覆盖 | `profile.analyze`、现有二维几何/gap量测 | 已提供XY/XZ/YZ米制单剖面正交直线和统计，实际原生输出残差门控；多点/线/圆组合和全部外部信号分析模式尚未整体移植。 [源码与边界](p1-analysis-cloud.md) · [回归](../packs/std-pointcloud/tests/test_p1_cloud.cpp) | P1 限定切片已实现 · [A05](operator-completion-plan.md#a05) |
| `SurfaceBasedEstimator`<br>3D模板匹配 | 缺失 | — | 尚无三维表面模板注册、特征/候选生成和姿态评分；单平面分割或二维 ICP 都不能替代三维全局模板匹配。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-pointcloud/ops/register_icp_2d.cpp) | P2 · [B02](operator-completion-plan.md#b02) |
| `SurfaceBasedEstimatorV2`<br>3D模板匹配（易用版） | 缺失 | — | 清单声明为易用版三维模板匹配，底层能力同样缺失；应复用统一三维匹配引擎加预设/组合封装，避免独立重复实现。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-pointcloud/ops/register_icp_2d.cpp) | P2 · [B02](operator-completion-plan.md#b02) |
| `TubeDirectionCorrection`<br>圆管朝向矫正 | 缺失 | — | 暂无利用圆管高光/成像特征校正三维朝向的专用算法；需明确相机、材质与光照前提。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |

### 位姿处理

| 外部项 | 结论 | LyFlow 当前入口 | 语义边界与证据 | 下一步 |
|---|---|---|---|---|
| `ConcatPrimitives`<br>合并两组工件3D | 已有对应 | `pose.concat` | 同维/同帧/同单位集合拼接并保留顺序和元数据；拒绝重复ID而非静默改名。2D/3D均可使用。 [源码与边界](p1-analysis-pose.md) · [回归](../packs/std-geometry/tests/test_pose.cpp) | P1 限定切片已实现 · [B07](operator-completion-plan.md#b07) |
| `ExtendOccludedBox`<br>纸箱面自动补全 | 缺失 | — | 没有纸箱面对象、可见性或遮挡推断节点；点云平面分割不是纸箱面自动补全。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `FilterWrongBoxFace`<br>过滤错误纸箱面 | 缺失 | — | 没有纸箱面集合及合并后面一致性过滤；已有点云滤波作用于采样点。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `FlipPrimitiveAxis`<br>翻转坐标轴3D | 缺失 | — | 已有工件集合/刚体变换和二维对称轴规则，但没有三维物体对称性约束与轴翻转优化。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/transform_make.cpp) | P2 · [B07](operator-completion-plan.md#b07) |
| `FlipPrimitiveAxis2D`<br>翻转坐标轴2D | 已有对应 | `pose.flip_axis_2d` | 显式对称阶数和目标弧度方向，按周期选择等价朝向；不猜测物体对称性，不扩展为三维轴优化。 [源码与边界](p1-analysis-pose.md) · [回归](../packs/std-geometry/tests/test_pose.cpp) | P1 限定切片已实现 · [B07](operator-completion-plan.md#b07) |
| `FrameFromPoints`<br>多点定位 | 已有对应 | `pose.frame_from_points` | 连接第三点优先三点模式；两点up正交化、三点右手系，首点/质心原点可选；重合/近共线/平行up诊断。 [源码与边界](p1-analysis-pose.md) · [回归](../packs/std-geometry/tests/test_pose.cpp) | P1 限定切片已实现 · [B07](operator-completion-plan.md#b07) |
| `GeneralPlacePositionFinder`<br>寻找放置位置 | 缺失 | — | 没有以高度图、工件尺寸和优先模式搜索放置位姿的规划器，属于机器人放置业务。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `GeometryTransformation`<br>变换3D几何体 | 部分覆盖 | `geometry.transform_plane`、`geometry.transform_2d` | 平面逆转置及补偿偏移、二维点线圆相似变换已实现；方向不吃平移、圆拒绝变椭圆。通用三维GeometryVector与全局FrameTree协议未实现。 [源码与边界](p1-analysis-pose.md) · [回归](../packs/std-geometry/tests/test_pose.cpp) | P1 限定切片已实现 · [A03](operator-completion-plan.md#a03) |
| `MappingHeightFromSKU`<br>映射SKU尺寸 | 缺失 | — | 已有工件尺寸字段，但没有SKU尺寸表、匹配规则及按纸箱长宽补全高度的算法。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `MergeBoxFaces`<br>合并纸箱面 | 缺失 | — | 没有纸箱面到同一箱体的关联、朝向/尺寸一致性和多面合并节点。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `MergeBoxesFromViews`<br>合并多视角纸箱 | 缺失 | — | 点云变换后可拼接采样点，但没有工作空间内多视角箱体身份关联、重复消除及对象融合。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-pointcloud/ops/transform_apply.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `MergePrimitivesByDimension`<br>根据尺寸合并工件 | 缺失 | — | 没有根据尺寸兼容性合并纸箱检测对象的实现；基础对象/尺寸已具备，仍需定义并实现业务兼容规则及容差。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `ObjectTransformation2d`<br>2D->3D位姿转换 | 已有对应 | `pose.to_world_plane` | 已知image.PlaneCalib下位置由H、朝向由局部雅可比，再乘显式plane→world刚体；已知物理size3D必须配置。不恢复任意深度，不回退未知外参。 [源码与边界](p1-analysis-pose.md) · [回归](../packs/std-geometry/tests/test_pose.cpp) | P1 限定切片已实现 · [B07](operator-completion-plan.md#b07) |
| `PickableBoxPredictor`<br>纸箱可抓预测 | 缺失 | — | 没有纸箱可抓性属性、夹具约束和碰撞判断；通用检测或点云分割不能推出绝对可抓。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `PrimitiveFilter`<br>过滤工件3D | 部分覆盖 | `pose.filter` | 已实现分数、类别/名称、pickable、XY面积、位置范围、最小间距和属性数值范围，保留通过/未通过顺序；三维碰撞NMS、旋转及抓取面积规则仍缺失。 [源码与边界](p1-analysis-pose.md) · [回归](../packs/std-geometry/tests/test_pose.cpp) | P1 限定切片已实现 · [B07](operator-completion-plan.md#b07) |
| `PrimitiveFilter2D`<br>过滤工件2D | 已有对应 | `pose.filter` | 按名称及明确字段筛选，缺可选字段不匹配；受校验Pose2D集合和未通过集合可继续连线，不把未设置字段当作已确认状态。 [源码与边界](p1-analysis-pose.md) · [回归](../packs/std-geometry/tests/test_pose.cpp) | P1 限定切片已实现 · [B07](operator-completion-plan.md#b07) |
| `PrimitiveTransformation`<br>变换工件位姿3D | 部分覆盖 | `pose.transform_3d`、`pose.offset_3d` | source→target左乘刚体、帧校验及属性保留，局部/世界平移显式；原多变换×工件笛卡尔展开、任意右乘和FrameTree模式未整体移植。 [源码与边界](p1-analysis-pose.md) · [回归](../packs/std-geometry/tests/test_pose.cpp) | P1 限定切片已实现 · [B07](operator-completion-plan.md#b07) |
| `PrimitiveTransformation2D`<br>变换工件位姿2D | 部分覆盖 | `pose.transform_2d` | 无镜像相似变换同步位置/弧度朝向/尺寸并保留身份；外部透视四角恢复与逐像素mask重采样未覆盖，不将透视四边形当旋转矩形。 [源码与边界](p1-analysis-pose.md) · [回归](../packs/std-geometry/tests/test_pose.cpp) | P1 限定切片已实现 · [B07](operator-completion-plan.md#b07) |
| `ProjectToPlane`<br>投影2D位姿到平面 | 部分覆盖 | `image.load_calib`、`image.board_calib` | 已有平面单应标定，但没有从2D对象位姿投影到给定空间平面并生成3D对象的节点；投影相机模型及平面坐标约定需补齐。 [1](../core/include/lyflow/data.h) [2](../packs/std-image/ops/load_calib.cpp) [3](../packs/std-image/ops/board_calib.cpp) | P2 · [B07](operator-completion-plan.md#b07) |
| `ResetPrimitives`<br>更新工件属性 | 已有对应 | `pose.update` | 允许名称/尺寸/元数据等明确字段更新，attributes整体替换；ID/帧/位置/旋转不在此节点修改，空集合也验证补丁。 [源码与边界](p1-analysis-pose.md) · [回归](../packs/std-geometry/tests/test_pose.cpp) | P1 限定切片已实现 · [B07](operator-completion-plan.md#b07) |
| `RotatePrimitiveAxis`<br>旋转坐标轴3D | 缺失 | — | transform.make 只构造给定欧拉角，不会按圆柱对称性优化轴朝向或修改已有工件坐标系。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/transform_make.cpp) | P2 · [B07](operator-completion-plan.md#b07) |
| `SetPickableProperty`<br>根据IOU设置2D位姿属性 | 缺失 | — | 没有两组旋转工件位姿 IOU 计算及 pickable 属性标记节点；轴对齐 Box2D 不能覆盖完整对象语义。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `SetWorkspaceByPrimitive`<br>重设工作空间 | 缺失 | — | 没有带位姿及尺寸的工作空间对象或由工件重设工作空间的节点；裁剪参数不等同业务工作空间。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `ShrinkBoxesDimension`<br>收缩纸箱尺寸V2 | 缺失 | — | 没有箱体集合及碰撞几何，也没有基于碰撞结果收缩纸箱尺寸的实现。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |

### 流数据运算

| 外部项 | 结论 | LyFlow 当前入口 | 语义边界与证据 | 下一步 |
|---|---|---|---|---|
| `BasicArithmetic`<br>基础数学运算 | 已有对应 | `value.arithmetic` | 加减乘除与无单位幂；明确单位规则，拒绝真正零分母、非法定义域及非有限结果。负分母与合法小分母可用。 [1](../packs/std-value/ops/register.cpp) [2](../packs/std-value/tests/test_values.cpp) | P0 已交付限定用途 · [分析与边界](p0-operator-analysis.md) |
| `CacheData`<br>数据缓存队列 | 缺失 | — | 引擎缓存复用同输入结果，不是先入先出数据队列；缺少容量、队首淘汰、会话状态、重置和取消语义。 [1](../core/include/lyflow/data.h) [2](../core/src/exec/disk_cache.cpp) | P2 · [D01](operator-completion-plan.md#d01) |
| `CloudVectorToCloud`<br>合并点云数组 | 已有对应 | `cloud.group_merge`、`cloud.group_concat` | 云+原生Indices+cloud.Groups.v1替代视图，按组序平坦化、保留重复点和全部通道；消费者校验来源/索引/帧，合法空集合输出空云，不兼容原CloudVector端口。 [源码与边界](p1-analysis-cloud.md) · [回归](../packs/std-pointcloud/tests/test_p1_cloud.cpp) | P1 限定切片已实现 · [A02](operator-completion-plan.md#a02) |
| `CompareNumbers`<br>比较数字 | 已有对应 | `value.compare` | 绝对/相对容差，等于和不等为补集；有序比较在容差内视同等，拒绝跨单位。 [1](../packs/std-value/ops/register.cpp) [2](../packs/std-value/tests/test_values.cpp) | P0 已交付限定用途 · [分析与边界](p0-operator-analysis.md) |
| `ConcateVector`<br>连接数组 | 部分覆盖 | `list.concat`、`value.concat` | 值列表/字符串拼接沿用P0；P1另有cloud.group_concat和pose.concat保留各自身份与帧校验，通用原生几何集合和原协议仍未覆盖。 [1](../packs/std-value/ops/register.cpp) [2](../packs/std-value/tests/test_values.cpp) | P0 已交付限定用途 · [分析与边界](p0-operator-analysis.md) |
| `Construct2DPose`<br>构造2D工件 | 已有对应 | `pose.construct_2d` | 常量和std.Value.v1动态坐标/角度，px/m及rad显式；动态值覆盖常量，输出带身份/帧/尺寸的受校验Pose2D。 [源码与边界](p1-analysis-pose.md) · [回归](../packs/std-geometry/tests/test_pose.cpp) | P1 限定切片已实现 · [B07](operator-completion-plan.md#b07) |
| `CustomPythonScript`<br>自定义python模块 | 部分覆盖 | `Python 节点 SDK/worker` | 已交付文件/包加载、装饰器注册、参数、日志和执行隔离，可编写自定义算子；接口不同于外部脚本，SDK端口限 PointCloud/Image/Measurement/Record，未交付16个外部脚本或原生几何端口适配。 [1](../python/lyflow_python/api.py) [2](../python/lyflow_python/data.py) [3](../core/src/python_runtime.cpp) [4](../docs/python-nodes.md) | P2 · [C02](operator-completion-plan.md#c02) |
| `Decompose2DInformation`<br>提取2D信息 | 已有对应 | `pose.decompose_2d` | 显式零基index提取x/y/angle/width/height带单位值，空/越界诊断；不静默只取首项或把任意几何当作工件。 [源码与边界](p1-analysis-pose.md) · [回归](../packs/std-geometry/tests/test_pose.cpp) | P1 限定切片已实现 · [B07](operator-completion-plan.md#b07) |
| `Extend2DInformation`<br>扩展2D信息 | 缺失 | — | 没有通用2D几何/对象升维为3D的数据契约与节点；需定义Z值、法向、坐标系和单位。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P2 · [B07](operator-completion-plan.md#b07) |
| `FetchElement`<br>获取数组元素 | 部分覆盖 | `list.get` | P0有界值列表索引提取，加P1的cloud.group_get和pose.get；越界有诊断，不产默认值。通用原生几何集合及原协议仍未覆盖。 [1](../packs/std-value/ops/register.cpp) [2](../packs/std-value/tests/test_values.cpp) | P0 已交付限定用途 · [分析与边界](p0-operator-analysis.md) |
| `GeneralArithmetic`<br>自定义数学运算 | 已有对应 | `value.expression` | 固定a..h可选值口，受限算术/比较/布尔/条件式及9个白名单函数，单位与数学域校验、内部短路和资源/取消限制；不重建动态端口、不兼容任意变量名/原Qt配置或宿主代码。 [源码分析与边界](p2-expression-analysis.md) · [回归](../packs/std-value/tests/test_expression.cpp) | P2 A01 限定切片已实现 · [A01](operator-completion-plan.md#a01) |
| `LogicOperation`<br>逻辑运算 | 已有对应 | `value.logic` | AND/OR/XOR/NOT/NAND/NOR/XNOR；布尔严格输入，NOT不需B。惰性调度使用flow.select。 [1](../packs/std-value/ops/register.cpp) [2](../packs/std-value/tests/test_values.cpp) | P0 已交付限定用途 · [分析与边界](p0-operator-analysis.md) |
| `MergeVector`<br>元素合并数组 | 部分覆盖 | `list.pack`、`list.concat` | P0值/记录列表保留；P1另有cloud.group_pack/concat与pose.concat。各领域使用明确载荷，仍无通用Plane/GeometryVector或外部动态集合协议。 [1](../packs/std-value/ops/register.cpp) [2](../packs/std-value/tests/test_values.cpp) | P0 已交付限定用途 · [分析与边界](p0-operator-analysis.md) |
| `VectorSubset`<br>取数组子集 | 部分覆盖 | `list.subset` | P0值列表子集保留顺序/重复；P1另有cloud.group_subset，保留组ID并拒绝重复身份，pose.get/filter提供实例选择。通用原生几何集合仍未覆盖。 [1](../packs/std-value/ops/register.cpp) [2](../packs/std-value/tests/test_values.cpp) | P0 已交付限定用途 · [分析与边界](p0-operator-analysis.md) |

### 检查

| 外部项 | 结论 | LyFlow 当前入口 | 语义边界与证据 | 下一步 |
|---|---|---|---|---|
| `CheckPalleteChangeByPrimitives`<br>检查垛是否变化 | 缺失 | — | 缺少垛型工件布局模型和跨次运行状态/容差比较契约，普通节点输出缓存不是场景变化检测。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `CheckPrimitivesChange`<br>检查工件位姿是否变化 | 缺失 | — | 无抓取前后工件关联、剩余工件姿态比较及阈值诊断；Transform 类型本身不含实例身份。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `CloudBasedEmptyToteChecker`<br>清框检测 | 缺失 | `filter.crop_box`、`filter.passthrough` | 盒内/高度滤波可以预处理，但没有点数统计到图内标量、与识别结果联合判断及清框专用错误策略；空点云不自动等于清框结论。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-pointcloud/ops/filter_crop_box.cpp) [4](../packs/std-pointcloud/ops/filter_passthrough.cpp) | P2 · [B01](operator-completion-plan.md#b01) |
| `DimensionConsistencyCheck`<br>工件长宽一致性检查 | 缺失 | `gap.judge` | 暂无工件尺寸前后关联与长宽一致性比较；gap.judge 仅消费现有 Measurement 并判阈值，不能生成工件尺寸或身份。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/gap/ops/measure.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `ToplayerCompletenessCheck`<br>顶层点云覆盖度检查 | 缺失 | — | 缺少最高层估计、工件几何投影及点云被实例覆盖率计算，不能由全点数替代漏检检测。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |

### 深度学习

| 外部项 | 结论 | LyFlow 当前入口 | 语义边界与证据 | 下一步 |
|---|---|---|---|---|
| `AIAssistant`<br>AI助手 | 待核实 | — | 清单只说深度学习模块后的辅助操作，未定义任务、端口和规则；不能按名字理解为聊天助手，也不能把 gap 专用 logits 后处理直接映射为对应。 [1](../packs/std-ml/ops/register.cpp) [2](../packs/gap/ops/model_roi.cpp) | 待定义 · [U01](operator-completion-plan.md#u01) |
| `Detection2D`<br>目标检测 | 部分覆盖 | `image.yolox_preprocess/decode`、`image.yolo_preprocess/decode`、`ml.onnx_run` | 已按明确契约支持YOLOX及YOLOv8/11/26检测：预处理、CPU float32推理、解码、NMS/端到端输出及原图坐标回映，返回vision.Poses2D.v1。不是任意YOLO导出格式通用解码；实例分割、可抓区域、旋转检测框、多ROI批处理和原Workspace/Request协议仍缺失。 [模型契约](p1-analysis-detection.md) · [解码回归](../packs/std-image/tests/test_detection_p1.cpp) | P1 限定切片已实现 · [B05](operator-completion-plan.md#b05) |
| `InstanceDetector`<br>目标检测V2 | 部分覆盖 | `image.yolox_preprocess/decode`、`image.yolo_preprocess/decode`、`ml.onnx_run` | 已按明确契约支持YOLOX及YOLOv8/11/26检测：预处理、CPU float32推理、解码、NMS/端到端输出及原图坐标回映，返回vision.Poses2D.v1。不是任意YOLO导出格式通用解码；实例分割、可抓区域、旋转检测框、多ROI批处理和原Workspace/Request协议仍缺失。 [模型契约](p1-analysis-detection.md) · [解码回归](../packs/std-image/tests/test_detection_p1.cpp) | P1 限定切片已实现 · [B05](operator-completion-plan.md#b05) |
| `LoadAnnotations`<br>加载标注信息 | 缺失 | — | 没有按时间戳检索数据集标注及对应图片/实例的加载节点；gap 专用类别标签记录不等于通用数据集标注读取。 [1](../packs/std-ml/ops/register.cpp) [2](../packs/gap/ops/model_roi.cpp) | P2 · [B05](operator-completion-plan.md#b05) |
| `OCR`<br>字符识别 | 部分覆盖 | `image.to_tensor`、`ml.onnx_run` | 通用 ONNX 推理只能承载部分模型执行；无文字检测、方向/透视校正、识别字典/CTC 解码、文本排序及文字位置/置信度输出的 OCR 流程。 [1](../packs/std-ml/ops/ml_onnx_run.cpp) [2](../packs/std-image/ops/image_tensor.cpp) | P2 · [B05](operator-completion-plan.md#b05) |
| `PredictPoseFromROI`<br>预测工件位姿(ROI) | 缺失 | — | 没有从二维目标 ROI 结合深度/相机/尺寸先验恢复逐工件 3D Pose 的算子；深度反投影或执行未知 ONNX 模型不能证明该算法已有。 [1](../packs/std-ml/ops/register.cpp) [2](../packs/std-image/ops/image_cloud.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `SemanticSegmentor`<br>语义分割 | 部分覆盖 | `image.to_tensor`、`ml.onnx_run`、`tensor.to_image` | 有张量推理和图像转换；gap.labels_from_logits 是轮廓序列专用后处理，缺通用二维语义 logits/类别 mask 解码、ROI 回贴、类别映射和逐图结果契约。 [1](../packs/std-ml/ops/ml_onnx_run.cpp) [2](../packs/std-image/ops/image_tensor.cpp) [3](../packs/gap/ops/model_roi.cpp) | P2 · [B05](operator-completion-plan.md#b05) |

### 输出

| 外部项 | 结论 | LyFlow 当前入口 | 语义边界与证据 | 下一步 |
|---|---|---|---|---|
| `BasicDatatypesPub`<br>发布基本数据类型 | 部分覆盖 | `图级命名输出、Measurement/Record、C ABI` | 可读取现有类型的命名结果；没有通用标量端口及 xyz-viz 基本数据发布协议适配。 [1](../docs/graph-doc.md) [2](../docs/embedding.md) [3](../core/include/lyflow/data.h) | P2 · [D01](operator-completion-plan.md#d01) |
| `CloudPub`<br>发布点云 | 可组合替代 | `图级命名输出、C ABI/Transport、点云预览` | 用宿主取回结果和 LyFlow 点云查看器替代查看用途；不能直接向 xyz-viz 发布，不具备其协议兼容性。 [1](../docs/graph-doc.md) [2](../docs/embedding.md) [3](../packages/editor/src/components/Viewer3D.tsx) | 无需补齐 |
| `ImagePub`<br>发布图像 | 可组合替代 | `图级命名输出、C ABI/Transport、图像预览` | 用宿主取回结果和 LyFlow 图像查看器替代查看用途；不能直接向 xyz-viz 发布。 [1](../docs/graph-doc.md) [2](../docs/embedding.md) [3](../packages/editor/src/components/EdgePeek.tsx) | 无需补齐 |
| `ImageSaveModel`<br>保存图像 | 已有对应 | `io.save_image` | 已有按路径保存图像；日志目录、时间戳命名和归档保留策略由宿主提供，不承诺外部目录约定。 [1](../packs/std-image/ops/io_image.cpp) | 无需补齐 |
| `MeshPub`<br>发布模型 | 缺失 | — | 没有 Mesh 数据类型、网格结果载荷及模型发布或查看链；点云显示不能等同网格模型。 [1](../core/include/lyflow/data.h) [2](../packages/editor/src/components/Viewer3D.tsx) | P3 · [C03](operator-completion-plan.md#c03) |
| `OutputPub`<br>输出结果 | 部分覆盖 | `图级命名输出、C ABI/Transport` | 可向宿主返回已有类型的结果；辅助外部源码的 xyz bridge Response 含工件位姿等结构，当前无相应协议和对象适配。 [1](../docs/graph-doc.md) [2](../docs/embedding.md) [3](../core/include/lyflow/data.h)（辅查外部源码；修订未验证） | P2 · [D01](operator-completion-plan.md#d01) |
| `Primitive2DPub`<br>发布2D位姿 | 缺失 | — | P1已有受校验Pose2D集合，可由现有命名结果返回宿主；外部Primitive2DPub发布协议/端点适配仍未实现。 [1](../core/include/lyflow/data.h) [2](../docs/embedding.md) | P2 · [B07](operator-completion-plan.md#b07) |
| `Primitive3DPub`<br>发布3D位姿 | 缺失 | — | P1已有受校验Pose3D集合并与Transform分开；外部Primitive3DPub发布协议/端点适配仍未实现。 [1](../core/include/lyflow/data.h) [2](../docs/embedding.md) | P2 · [B07](operator-completion-plan.md#b07) |
| `UploadTimestamp`<br>上传异常3d工件位姿时间戳 | 缺失 | — | 运行事件带时间信息，但未交付关联异常工件、采集帧和外部上传服务的业务链。应由宿主负责。 [1](../docs/embedding.md) [2](../core/include/lyflow/data.h) | P3 · [D01](operator-completion-plan.md#d01) |

### 可视化

| 外部项 | 结论 | LyFlow 当前入口 | 语义边界与证据 | 下一步 |
|---|---|---|---|---|
| `DataShow`<br>打印信息 | 部分覆盖 | `结果检查器、Measurement/Record 预览` | 可检查当前Record/Measurement结果，含P1工件载荷；仍缺少外部工件高度等字段的专用显示与原查看协议。 [1](../packages/editor/src/components/Inspector.tsx) [2](../packages/editor/src/components/EdgePeek.tsx) [3](../core/include/lyflow/data.h) | P2 · [C03](operator-completion-plan.md#c03) |
| `ImageShowModel`<br>可视化图像 | 可组合替代 | `图像预览、EdgePeek` | 编辑器已能显示图像结果，不必为单纯查看额外放置计算节点；叠加图元遵循现有场景契约。 [1](../packages/editor/src/components/EdgePeek.tsx) [2](../packages/editor/src/components/EdgePeekLayer.tsx) | 无需补齐 |
| `SVGShowModel`<br>可视化矢量图 | 部分覆盖 | `共享 SceneSpec → SVG/PNG` | 可将支持的二维点、线、圆及图像场景输出 SVG；没有任意 SVG 文档输入端口或通用 SVG 浏览节点。 [1](../packages/visualization/README.md) [2](../packages/visualization/index.d.ts) | P2 · [C03](operator-completion-plan.md#c03) |
| `StartRerun`<br>（未翻译） | 缺失 | — | 没有 Rerun 启动或协议连接器；现有点云查看器不是 Rerun 协议实现，按宿主需求选做。 [1](../packages/editor/src/components/Viewer3D.tsx) [2](../packages/visualization/README.md) | P3 · [C03](operator-completion-plan.md#c03) |
| `Visualize2DCloud`<br>可视化2D点云 | 可组合替代 | `点云二维预览、共享场景导出` | 可选择投影平面显示点云并导出矢量场景；需显式保持坐标帧、单位和投影轴，不依赖外部 SVG 数据类型。 [1](../packages/editor/src/components/EdgePeek.tsx) [2](../packages/editor/src/lib/cloudScene.ts) [3](../packages/visualization/README.md) | 无需补齐 |
| `VisualizeCloudVector`<br>可视化点云数组 | 部分覆盖 | `单云预览、util.merge` | 已有云分组数据与提取/合并节点；查看器仍缺少按组身份着色、选择及相机投影叠加，不等于完整集合查看器。 [1](../packages/editor/src/components/Viewer3D.tsx) [2](../packs/std-pointcloud/ops/util_merge.cpp) [3](../core/include/lyflow/data.h) | P2 · [C03](operator-completion-plan.md#c03) |
| `VisualizePrimitives2D`<br>可视化工件位姿2D | 部分覆盖 | `二维几何场景` | 已有点线圆等几何叠加和P1 Pose2D数据；仍未交付带实例身份的旋转工件集合专用显示。 [1](../packages/visualization/README.md) [2](../core/include/lyflow/data.h) | P2 · [C03](operator-completion-plan.md#c03) |
| `VisualizePrimitives3D`<br>可视化工件位姿3D | 缺失 | — | 已有带实例/尺寸/帧的Pose3D集合数据；缺少其坐标轴、尺寸盒或工件模型的专用显示，点云查看器不能替代。 [1](../packages/editor/src/components/Viewer3D.tsx) [2](../core/include/lyflow/data.h) | P2 · [C03](operator-completion-plan.md#c03) |

### 预置组节点

| 外部项 | 结论 | LyFlow 当前入口 | 语义边界与证据 | 下一步 |
|---|---|---|---|---|
| `FlatPartitionDetection`<br>平面隔板检测 | 缺失 | `segment.ransac_plane` | 现有单平面拟合可作为底层步骤，但没有最高层点云自动提取+三维 PCA+隔板对象姿态的完整预置组。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-pointcloud/ops/segment_ransac_plane.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `RelocateWorkspace`<br>工作空间重定位 | 缺失 | `register.icp_2d` | 该预置链依赖正交高度图、二维形状匹配、三维 ICP 和工作空间姿态更新；当前缺少这些关键链路，二维 ICP 不足以替代。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-pointcloud/ops/register_icp_2d.cpp) [4](../packs/std-image/ops/image_cloud.cpp) | P3 · [B04](operator-completion-plan.md#b04) |
| `ToteLocalization`<br>托盘定位 | 缺失 | — | 缺少 RGB 透视补偿+二维形状模板匹配到托盘位姿的预置流程；单纯标定/图像变换不提供目标定位。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `ToteDetector`<br>托盘检测 | 缺失 | — | 缺少指定托盘检测后处理及异常时间戳上传协议的预置链；通用模型推理、日志或工作流执行不能等同业务检测器。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |

### 内置 Python 脚本

| 外部项 | 结论 | LyFlow 当前入口 | 语义边界与证据 | 下一步 |
|---|---|---|---|---|
| `LogString`<br>输出字符串到文件 | 部分覆盖 | `ExecContext.log`、`Python Context.log` | 已有运行日志基础设施与Python日志API，但没有带时间戳、接收字符串端口并指定文件输出的现成脚本；不能假定日志格式与外部兼容。 [1](../core/include/lyflow/operator.h) [2](../python/lyflow_python/api.py) [3](../core/src/python_runtime.cpp) | P2 · [D01](operator-completion-plan.md#d01) |
| `LogPrimitive3D`<br>输出3D位姿到文件 | 缺失 | — | 已有Pose3D与JSON序列化，仍无带时间戳/归档策略的位姿文件导出节点或原脚本；SDK扩展能力不算现成算法。 [1](../core/include/lyflow/data.h) [2](../python/lyflow_python/data.py) [3](../python/examples/offset.py) | P2 · [B07](operator-completion-plan.md#b07) |
| `LogPrimitive2D`<br>输出2D位姿到文件 | 缺失 | — | 已有Pose2D与JSON序列化，仍无标准时间戳/归档位姿文件输出脚本；通用日志不是文件导出。 [1](../core/include/lyflow/data.h) [2](../python/lyflow_python/data.py) [3](../python/examples/image_metrics/nodes.py) | P2 · [B07](operator-completion-plan.md#b07) |
| `Primitive3DtoString`<br>转换3D位姿为字符串 | 已有对应 | `pose.to_text` | 原生版本化工件JSON序列化，保留单体/完整集合、帧/单位/属性和数值精度；64KiB上限明确拒绝超限。不声称原脚本分隔符/任意欧拉顺序协议或脚本本身已移植。 [源码与边界](p1-analysis-pose.md) · [回归](../packs/std-geometry/tests/test_pose.cpp) | P1 限定切片已实现 · [B07](operator-completion-plan.md#b07) |
| `Primitive2DtoString`<br>转换2D位姿为字符串 | 已有对应 | `pose.to_text` | 原生版本化工件JSON序列化，保留单体/完整集合、帧/单位/属性和数值精度；64KiB上限明确拒绝超限。不声称原脚本分隔符/任意欧拉顺序协议或脚本本身已移植。 [源码与边界](p1-analysis-pose.md) · [回归](../packs/std-geometry/tests/test_pose.cpp) | P1 限定切片已实现 · [B07](operator-completion-plan.md#b07) |
| `StringtoPrimitive3D`<br>转换字符串为3D位姿 | 已有对应 | `pose.from_text` | 只接受完整type/data及受校验v1工件JSON，类型/版本/深度/尺寸/旋转非法均拒绝；不是旧分隔文本的通用解析器或原脚本移植。 [源码与边界](p1-analysis-pose.md) · [回归](../packs/std-geometry/tests/test_pose.cpp) | P1 限定切片已实现 · [B07](operator-completion-plan.md#b07) |
| `Compare2Primitive3D`<br>比较两个3D位姿 | 缺失 | — | 没有两个3D工件位姿输入的平移/旋转误差与阈值报警节点；构造 Transform 不等于位姿比较。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) [3](../packs/std-pointcloud/ops/transform_make.cpp) | P2 · [B07](operator-completion-plan.md#b07) |
| `Compare2Primitive2D`<br>比较两个2D位姿 | 已有对应 | `pose.compare_2d` | 同帧同单位单实例的位置距离、周期角差和双门限判断；显式角度周期，不做跨帧或多实例关联。由原生节点提供用途，不执行原脚本。 [源码与边界](p1-analysis-pose.md) · [回归](../packs/std-geometry/tests/test_pose.cpp) | P1 限定切片已实现 · [B07](operator-completion-plan.md#b07) |
| `CheckPickablePrimtives2D`<br>检查2D位姿是否为Pickable | 缺失 | — | 没有基于两组2D位姿 IOU 的 pickable 标记脚本，与同名原生清单项应合并为一个能力任务。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `ShrinkBoxDimension`<br>收缩纸箱尺寸 | 缺失 | — | 没有箱体碰撞及尺寸收缩脚本，与 ShrinkBoxesDimension 归并规划，避免按名称重复建设。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `RecordReplayer`<br>（未翻译） | 部分覆盖 | `CLI 输出快照/样本回放` | 现有输出快照保存及样本重跑可支持LyFlow调试，但没有外部视觉日志录制数据格式、时间序列重放及对应源节点；需核定录制格式后做适配。 [1](../bridge/src/cli/snapshot.rs) [2](../docs/cli.md) | P2 · [D01](operator-completion-plan.md#d01) |
| `MappingBoxDimension`<br>映射纸箱尺寸 | 缺失 | — | 没有纸箱尺寸表映射及现成Python脚本；与 MappingHeightFromSKU 共享SKU尺寸补齐任务。 [1](../core/include/lyflow/data.h) [2](../packs/std-pointcloud/ops/register.cpp) | P3 · [D02](operator-completion-plan.md#d02) |
| `MakeOffset`<br>3D位姿添加偏移 | 已有对应 | `pose.offset_3d` | 米制局部/世界平移；local先乘工件旋转，world直接相加，保留尺寸/朝向/属性；不是点云Z偏移，也不是旧毫米脚本协议。 [源码与边界](p1-analysis-pose.md) · [回归](../packs/std-geometry/tests/test_pose.cpp) | P1 限定切片已实现 · [B07](operator-completion-plan.md#b07) |
| `ConstructPrimitive3D`<br>构造3D位姿 | 已有对应 | `pose.construct_3d` | 常量/动态位置及旋转输入明确单位，生成带身份/帧/尺寸及可选属性的Pose3D；动态值覆盖而非隐式相加，不宣称原脚本代码已移植。 [源码与边界](p1-analysis-pose.md) · [回归](../packs/std-geometry/tests/test_pose.cpp) | P1 限定切片已实现 · [B07](operator-completion-plan.md#b07) |
| `MoveDebugImage`<br>整理debug图像 | 缺失 | — | 没有按时间戳匹配并归档已有debug图像的现成脚本；建议作为宿主诊断归档工具，文件布局和时间戳规则需另定。 [1](../python/lyflow_python/api.py) [2](../python/examples/image_metrics/nodes.py) [3](../bridge/src/cli/snapshot.rs) | P3 · [D01](operator-completion-plan.md#d01) |
| `RigidDistanceCheck`<br>刚体校验 | 部分覆盖 | `gap.point_offset`、`gap.judge` | 可组合点对距离与固定公差判定，覆盖简单2D距离监测；没有已注册刚体模板、多点一致性、3D变形校验或外部脚本的报警契约。 [1](../core/include/lyflow/data.h) [2](../packs/gap/ops/offset.cpp) [3](../packs/gap/ops/measure.cpp) | P2 · [B07](operator-completion-plan.md#b07) |

## 维护规则

新增能力后，只将满足本行限定语义和验收条件的条目升级为“已有对应”或“可组合替代”。同时更新证据、统计和计划状态；改名、增加描述、注册空壳或开放 Python 扩展不构成完成。待核实项先补外部端口、样本与预期结果，再重新评估，保留外部类名/脚本名作为稳定检索键。
