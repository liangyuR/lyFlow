# B07 三维工件几何、裁云与平面投影

本轮参考为本机 `C:/Users/11601/project/xyz-vision-lib` 中实际可读文件；目录没有可验证的 Git 元数据，因此没有把它宣称为计划中的 `72d…` 修订。仅分析接口、参数和几何用途，以下实现独立编写，不复制专有函数体。底层 `xyz_vision_base`、全局帧树及部分掩膜/平面拟合助手并不完整可得，不声称已审阅其实现。

## 实际参考与全部运行模式

| 原项与实际源 | 已读模式、问题及本轮替代 |
| --- | --- |
| `3D_Processing/CloudCutByPrimitives.cpp/.hpp`，`property_settings.yml` | cuboid/cylinder/ball，参数尺寸、局部 Z 圆柱轴、逐工件内部云/合并内部云/外部云/按点数分碰撞。参考圆柱用有限角度的包围框会漏真实圆周，输出还依赖端口连接状态。`cloud.crop_primitives` 用精确有限体判定，支持每工件尺寸或显式覆盖，边界 open/closed 显式，重叠分组独立但合并云源索引去重，所有声明输出完整计算。 |
| `3D_Processing/CloudRemovalByPrimitives.cpp/.hpp`，参数源 | model 半径邻域移除、cube 体积移除、表面/中心锚点、XY/Z 外扩、未知高度替代、关联 2D mask 与每列顶端向上扩展、保点序/紧凑结果。参考中心分支 Z 比较方向相反，mask 分支丢 OOB 点和可选通道。`cloud.remove_primitives` 支持 shape/model_radius、每侧 padding、top/center、heightUnknown/maxHeight；`cloud.remove_mask_primitives` 可先做 mask 移除，再串接 shape 或 model_radius，不引入 std-pointcloud→OpenCV 依赖。 |
| `Primitive_Processing/FlipPrimitiveAxis.cpp/.hpp`，参数源 | X/Y/Z 被优化轴、局部参考旋转轴、六个有符号父帧方向、有限 object_sides。`pose.flip_axis_3d` 在显式 n 重局部旋转等价类内最大化目标点积，支持任意父帧方向，不把轴名索引误当实际空间平行关系。 |
| `Primitive_Processing/RotatePrimitiveAxis.cpp/.hpp`，参数源 | 局部固定轴/旋转轴，Point/Direction，两种指向、use_z。`pose.rotate_axis_3d` 保持固定轴，把目标投影到其正交平面后求有符号最短转角；须明确 continuousSymmetry，零向量、重合点、平行轴诊断错误，不交付 NaN 或假成功原姿态。 |
| `Flow_Data_Operation/Extend2DInformation.cpp/.hpp`，参数源 | 单个/集合 Geometry2D，point/line/circle，新轴 X/Y/Z，frame type/id；包装还声明 Primitives2D，但实际运行分支未实现。`geometry.extend_2d` 真正支持原生 Point2D/Line2D/Circle2D、几何集合和 Poses2D；线段保端点，圆有真实法向/半径，工件有完整右手姿态和已知厚度，px→m 显式比例，另可乘原生 rigid Transform。 |
| `3D_Processing/ProjectPrim3DTo2D.cpp/.hpp`，参数源 | separate/merge、full/front_only、dilate；参考 Pose 是顶面锚点、投影八角点后全局 convex hull，可能填满对象间空隙且未守正深度。`pose.project_3d` 支持 individual/union、full/top、center/top 锚点，逐像素反畸变射线与有限实体相交，union 为真实支持并集，可选择独立支持或 nearest 遮挡。也消费显式 circle/cylinder/sphere 形状。 |
| `Primitive_Processing/ProjectToPlane.cpp/.hpp`，参数源 | input_plane/tote_bottom/tote_upper，单平面广播/逐实例平面，flip_z，cal_dim，轴可视化。`pose.project_to_plane` 使用实际 CameraCalib 反畸变射线、任意平面、Workspace region 上下表面、ID 对应平面集合；center 用已知尺寸，dimensions 由真实掩膜边界（或裁到传感器内的旋转框边界）投到平面后求最小面积外接矩形。已读参考的失败 identity/零尺寸输出、第一平面帧标签复用不保留。输出本身可直接被现有 Pose、wireframe、box sampler 消费；可视化由现有叠画节点消费 Pose，不修改原图。 |
| `CommonScripts/xvl_compare_2_primitive3d.py`，参数源 | 两输入首项、XYZ 位置差、三个同名轴夹角 `acos(Rii)`（不是 Euler 分量）、阈值与报警。`pose.compare_3d` 保留此轴夹角检查，增加 geodesic 旋转角、ID/index 批次配对、显式 b→a 刚体、父帧/局部平移差及有限/连续对称；角度统一 rad。 |
| `CommonScripts/xvl_rigid_distance_check.py`，参数源 | register 写位置/点对距离及默认公差，distance 全点对检查，SVD 对应点刚体配准残差检查，日志与报警。`pose.rigid_template` 记录 ID、位置、逐点及点对公差；`pose.rigid_check` 两种检查完整实施，只输出结果和 accepted；`pose.rigid_register` 仅在可观测时交付真实原生 Transform，反射不得当旋转。模板显式 Record/GraphDoc 输入，无隐式全局文件或日志副作用。 |

## 坐标与身份

所有三维位置、尺寸和平移均为 m，旋转为 xyzw quaternion，尺寸为完整长宽高，标准 Pose 锚点为中心。旧顶面坐标通过 `anchor=top` 换算：`center=position-R*(0,0,sizeZ/2)`。有限对称/连续轴对称均是调用者对物体的显式声明；不能从任意不等边 box 推断对称。新 Pose 保留原 ID、名称、字符串类别、置信度和 attributes，计算结果统一 `pickable=false`。

`CameraCalib.frame` 是三维相机帧；`imageFrame` 是二维原像素帧，通常分别为 `camera`、`image`。名字相同也不构成维度相同的证据。跨三维帧必须提供 source→target 的原生右手 rigid Transform。`posesToCloud`、`posesToCamera`、`bToA`、`planeToCamera`、`cloudToCamera` 的方向均由端口名和节点的源/目标帧确定。

`pose.project_3d` 没有 Image 输入，输出固定为 CameraCalib 的完整图像网格，scale=1。Full/Preview 均执行这份几何网格，不把完整 K/D 随预览悄悄缩放。显式空 `rois` 产出空支持，不回退整图。二维 Pose/MaskInfo 的坐标与 extent 都是原图像素；出界被裁掉，射线交点须在 nearDepth 之后。畸变支持 0/4/5/8/12/14 参数，反畸变后重新正投影检查像素残差，不能稳定求逆就失败。

反畸变现统一消费 projection5::normalizedRay 的有界 Newton/线搜索和全次调用共享 RayBudget/取消上下文，正投影残差不超过1e-7px；不使用固定次数 undistort 失败后的错误射线。只对选中 ROI 的像素求逆，空 ROI 不尝试域外射线。像素支持框的中心为 `x+(width-1)/2`、`y+(height-1)/2`，尺寸为完整像素 cell 足迹，避免投影→反投影引入半像素偏移。

`attributes.shape` 可为 box/cylinder/sphere/circle，未提供时按 box。circle 是 local XY 有限圆盘，sizeXY=2r、normal 若提供须同 Pose.localZ，不能把圆盘伪成外接立方体；cylinder 的 localZ 为长轴，sphere 三轴直径一致。circle 的 thickness 是已有实体元数据，不改变圆盘的二维几何。top 对 box/cylinder 是 local+Z 面；球体没有单独平面 top，仍投真实球面支持。union 保留 `attributes.sourcePoses3D`；individual 保留原属性及 `attributes.sourcePose3D`，不丢合并成员的类别信息。

## 原生数组与小记录

投影使用标准 `vision.Poses2D.v1` + float32 Tensor `[N,H,W]`（0/1、允许重叠）+ `vision.Masks2D.v1`：`{frame,unit:'px',width,height,layout:'NHW',items:[{id,index,hasMask,source}]}`。空为 `[0,H,W]`。不把像素数组放入 JSON，也不新增 C ABI 类型。尺寸反投影输出的 shape 明确为 box，并把原二维形状保留在 sourcePose2D，不能让旧圆半径/normal 冒充新矩形尺寸。反投影及 mask 移除严格检查 scale1、shape、extent、逐项 ID/index、Pose.hasMask/maskIndex、二值数值、实际非零支持及其 Pose 范围。调用者负责把同一来源的数组与元数据配线；同尺寸不能证明同一图像身份。

任意平面可用原生 Plane + planeFrame，或 `geometry.Planes3D.v1={frame,unit:'m',items:[{id,normal:[nx,ny,nz],d}]}`。平面满足 `n·p+d=0`，法向和 d 同时归一化。一个 id=`*` 广播，其他情况与 Pose ID 严格一一对应。Workspace 模式消费既有 workspace.load 输出的中心 Pose3D region，不读取全局帧树；上/下表面中心为 region.center±localZ*sizeZ/2。

dimensions 模式中，若 Pose 声明 hasMask=true，缺少原生 masks/maskInfo 必须失败，不能退回外接框；明确没有 mask 的实例仍可用框边界拟合。center 模式使用声明的已知物理尺寸，不依赖分割支持。三轴夹角通过单位轴的 atan2(cross范数,dot) 求值，避免 acos 在同一姿态的舍入范数附近放大成虚假角差，不放宽调用者的容差。

`geometry.extend_2d` 五类输入互斥：poses/point/line/circle/features。集合 `geometry.Features2D.v1={frame,unit:'px'|'m',items}`，每项 id/type/position，line 加 direction、可成对给 start/end，circle 加 radius；可保留标准名称/类别/置信度/attributes。输出 `geometry.Features3D.v1` 内含真正三维 point、单位化 line/有限端点、circle 中心/单位法向/米制半径，不用占位矩阵代表几何。新增 Y 轴时输入 XY 映到 XZ，其右手平面法向为 -Y；新增 X/Z 时分别为 +X/+Z。Pose2D 则输出可消费的标准 Pose3D。每次成功调用始终写出 poses 与 features 两个端口；非适用端口为同帧、带类型的空集合，不使用缺失 Data 或空矩阵。

裁云 `groupIndices` 绑定原云 ID，`groups` 为既有 cloud.Groups.v1，offset/count 对应按输入 Pose 顺序的独立支持，重叠组可包含同一源点。innerIndices 为唯一源索引升序；outerIndices 是其补集，包含未知坐标。所有 select 经 PointCloud.select，RGB、normals、intensity 不丢失。`cloud.crop_primitives.cloud` 为 outer，inner/outer 显式另出。model_radius 的模型是 Pose 局部坐标米制采样点，对变换后实际 float32 query 做距离复核；坐标计算范围绝对值不超过1e12m，半径1e-12..1e6m，避免上游float距离溢出/下溢。

preserve_order 是保留源点序并对移除 XYZ 写 NaN，不宣称 PointCloud 具有 H/W；validity `[pointCount]` 为保留且有限=1，其余0。云本身没有网格元数据，有组织消费必须另传正确 mapping/indexMap。原 NaN 点进入 invalidIndices；若存在未知坐标且已知支持未达碰撞阈值，或该有限体完全没有已知支持（包括空云），该工件归 unknownPoses，不归 freePoses。已知支持超过 maxCount 仍可确定 collision。free/collision/unknown 均无抓取许可，术语仅指给定云的点计数，quality 明确 basis=observed_cloud_point_count、physicalCollisionSafetyVerified=false。有限体输出 Pose 描述实际计数体（中心、override/padding 后尺寸），原始 Pose 备份到 sourcePose3D；再次裁云时先去掉旧备份，避免递归增长。

关联 mask 移除要求每个输入 Pose3D 的 `attributes.primitive2DId` 严格指向 detections 的 ID，重复关联/缺失 ID 拒绝。按每列真实最顶非零像素向上 topExpand，不填充其他空洞。仅移除投影落入支持的正深度点；负深度/非有限坐标归 unknown 并保留，传感器 OOB 点保留并单独计数，源云和全部可选通道只读。该节点可与 shape/model_radius 按原用途顺序组合。

## 刚体模板与质量

`geometry.RigidTemplate3D.v1={frame,unit:'m',pairTolerance,items:[{id,position,tolerance}]}`，2..256 项。模板来自参考 Poses 的真实位置，不依赖输入顺序配错 ID。可把参考 Pose 的完整 JSON 放在 `pose.from_text` 的 GraphDoc 参数中，再连 rigid_template；保存/重载 GraphDoc 即持久化参考基准。SDK 也可直接提供模板 Record。仓库当前没有通用 io.save_record/load_record，本文不宣称这些文件节点存在。原注册文件/日志副作用由显式宿主管理，不伪造文件已写入。

distance 检查所有点对的长度差；它刻意不判绝对姿态，也无法区分镜像，这是该不变量本身的边界。registration 对应点闭式求 proper SO(3)（无 Eigen/OpenCV/PCL 的4×4对称 Jacobi quaternion 解），要求源/目标非共线及最优特征值有可辨识间隔；拒绝反射作为刚体捷径。实际拟合矩阵量化为 native float32 后重新计算每点残差，用原公差判断 accepted。`pose.rigid_check` 始终交付 result 和 accepted，distances 的 transformAvailable=false、transform=null；registration 退化时 observable=false、maximumError=null、residualCount=0，不伪造 identity。需消费原生变换时使用独立 `pose.rigid_register`：仅接受可观测的 registration，交付 transform、result、accepted，result.transform 与原生矩阵逐值一致。不可观测为 unobservable 原子失败，无任何输出；可观测但超容差在 alarm=false 时仍交付真实 best-fit 和 accepted=false。两个节点的 alarm=true 超差均为原子失败。

比较的 result 区分 XYZ translation、三轴夹角、总 angle、relativeRotation 和 matched；空配对为空结果，不生成虚构工件。dimensions 反投影用真实 mask 的边界像素角点，或传感器裁剪后的框边界，求平面上的外接矩形并重新扩到包含实际支持；不把未知物理尺寸默认成像素宽高。center 模式尺寸必须是显式已知 m。

## 预算、取消与验证边界

统一小 Record 受既有 1MiB/4096项/深度16约束；所有结果先在临时 Outputs 完成并校验，成功且未取消才发布，JSON异常/内存不足不会交付部分结果。几何对称枚举最多1,000,000次；模板最多256点；裁云最多4Mi点、源payload128MiB、逐体扫描/模型查询与命中64Mi工作量、分组支持16Mi索引；投影完整相机网格最多4Mi像素、Masks32Mi float、核心几何64Mi工作量；膨胀另按核面积预检，反投影边界最多2Mi采样点/实例且总射线预算64Mi。超限明确 resource_limit，不返回假空/假质量。

永久回归在三个 pack 的 test_p5_pose_geometry/test_p5_primitive_crop/test_p5_pose_projection 中，独立探针位于 ignored build/p5-*-review。首轮冻结的独立 Windows MSVC/OpenCV5.1/PCL1.15.1 探针实际通过20组350断言，包括几何集合、取消、预算、非零支持与绑定。同行复核后，本轮只选择新增共享逆畸变/14D、像素中心往返、ROI求逆隔离、真实mask缺失、极小轴角及此前A/B反例复跑，三个探针合计9组119断言通过；未把筛选排除的既有组称为重新全跑，最终整体门禁以阶段日志为准。完整调度器 Full/Preview、C ABI、C++ SDK、GraphDoc 参数覆盖/保存重载及安装 CLI 由本轮整体门禁补验，不能从 compute 探针推断已通过。本文不声称工业碰撞安全、抓取许可或现场吞吐已获验证。
