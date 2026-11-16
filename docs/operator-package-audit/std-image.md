# std-image 逐算子迁移清单

状态：实施中；当前 76 项。原251/7规划基线通过机器清单baselineReference追溯。返回[清单总览](../operator-package-audit.md)。

下列顺序是迁移/验收顺序，不是执行图调度顺序。精确端口和参数见[机器清单](../operator-package-audit.json)。

## 023 · image.upgrade_plane_calib

- 当前：`std-image` / `1.0.0` / 平面标定契约升级。目标：`std-image`；保留并验边界；**M1**。
- 源码：[packs/std-image/ops/calib_upgrade.cpp:8](../../packs/std-image/ops/calib_upgrade.cpp#L8)。
- 端口：`calib:Record` → `calib:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core公开Profile/几何/Record契约与C++标准库；无PCL/OpenCV/ONNX Runtime直接依赖
- 当前契约：image.PlaneCalib旧记录显式升级schemaVersion=1与imageFrame/planeFrame；缺unit必须选择m/mm；H齐次归一化保持映射；已有v1记录拒绝重标帧。
- 目标契约：共享core平面标定解析用于跨包消费；旧非版本化分支保持兼容，统一的版本化分支不猜单位或帧。
- 迁移前置：M0
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：缺单位/错误单位/版本/奇异H拒绝；m/mm映射一致、齐次尺度不敏感、幂等升级、帧冲突失败且输入输出原子。
- 说明：M1新增显式契约适配；不替代M3轮廓与DTS场景算法迁移。

## 154 · cloud.fill_holes

- 当前：`std-image` / `1.0.0` / 组织点云孔洞填补。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/p5_inspection.cpp:1185](../../packs/std-image/ops/p5_inspection.cpp#L1185)。
- 端口：`cloud:PointCloud`, `depth:Image`, `status:Image`, `indexMap:Tensor`, `mapping:Record` → `cloud:PointCloud`, `depth:Image`, `status:Image`, `indexMap:Tensor`, `mapping:Record`, `filledMask:Image`, `filledIndices:Indices`, `quality:Record`。
- 参数单位标注：maxDepthSpread=m。
- 依赖：当前算法：OpenCV core/imgproc及相机畸变/数值求解的geometry模块、LyFlow轻量云/位姿类型；无需PCL。；目标：投影/栅格适配使用图像后端，共享映射/射线数值核，不引点云包注册依赖。
- 当前契约：输入[cloud:PointCloud, depth:Image, status:Image, indexMap:Tensor, mapping:Record] → 输出[cloud:PointCloud, depth:Image, status:Image, indexMap:Tensor, mapping:Record, filledMask:Image, filledIndices:Indices, quality:Record]。输入绑定的云、F32米制depth、U8 status、[H,W] indexMap、DepthMapping；只以原观测邻域补未知，输出新云追加点及新映射，filledIndices指向新云。
- 目标契约：组织深度孔洞分析/形态学仍图像depth-mapping模块；新增点与补值观测身份必须显式，不宣称实测。
- 迁移前置：M1；M1映射组/父索引/观测状态/坐标帧契约
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证不跨大深度、边界开关、仅用原观测、追加点索引与mapping重绑。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 155 · cloud.from_depth

- 当前：`std-image` / `1.0.0` / 深度图转点云。目标：`std-image`；拆公共能力/适配；**M6**。
- 源码：[packs/std-image/ops/image_cloud.cpp:221](../../packs/std-image/ops/image_cloud.cpp#L221)。
- 端口：`depth:Image`, `color:Image?` → `cloud:PointCloud`。
- 参数单位标注：fx=px, fy=px, cx=px, cy=px, depthScale=m, minDepth=m, maxDepth=m, step=px。
- 依赖：当前计算：标准C++与LyFlow原生Image/Tensor/PointCloud类型，不调用OpenCV或PCL。；当前构建仍受std-image整包OpenCV链接约束；目标拆成无重依赖的显式能力模块。
- 当前契约：输入[depth:Image, color:Image?] → 输出[cloud:PointCloud]。按针孔内参把深度图反投影成点云（相机坐标系：x 右、y 下、z 前，米）。0、非有限值、深度范围外的像素不出点。接了 color（与深度图同样大、已配准的 RGB 图）就给点上色。要换到世界坐标系接 transform.apply。
- 目标契约：保留图像到米制相机云的portable-depth适配器；针孔数值核提取到共享轻量投影库，点云数据类型不构成PCL依赖。
- 迁移前置：M1；公共轻量针孔投影核，不修改无畸变针孔算法
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证内参、u16 depthScale、零/非有限剔除、RGB对齐和XYZ米制。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 156 · cloud.height_map

- 当前：`std-image` / `1.0.0` / 点云正交高度图。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/p5_projection.cpp:1222](../../packs/std-image/ops/p5_projection.cpp#L1222)。
- 端口：`cloud:PointCloud`, `gridToCloud:Transform?` → `cloud:PointCloud`, `sourceCloud:PointCloud`, `depth:Image`, `status:Image`, `indexMap:Tensor`, `sourceIndexMap:Tensor`, `mapping:Record`, `quality:Record`, `image:Image`, `pseudo:Image`, `normalAngle:Image`, `normalValid:Image`, `pointState:Tensor`。
- 参数单位标注：width=px, height=px, resolution=m, originX=m, originY=m, minHeight=m, maxHeight=m。
- 依赖：当前算法：OpenCV core/imgproc及相机畸变/数值求解的geometry模块、LyFlow轻量云/位姿类型；无需PCL。；目标：投影/栅格适配使用图像后端，共享映射/射线数值核，不引点云包注册依赖。
- 当前契约：输入[cloud:PointCloud, gridToCloud:Transform?] → 输出[cloud:PointCloud, sourceCloud:PointCloud, depth:Image, status:Image, indexMap:Tensor, sourceIndexMap:Tensor, mapping:Record, quality:Record, image:Image, pseudo:Image, normalAngle:Image, normalValid:Image, pointState:Tensor]。显式工作空间栅格按最高真实观测生成有符号高度，独立状态保留零/负高度与空像素。compact云及HW索引组成organized视图；父云索引和遮挡状态独立保留。
- 目标契约：正交云到像素高度栅格留图像depth-mapping模块，使用米制云和显式gridToCloud，不因为cloud前缀引入PCL。
- 迁移前置：M1；M1映射组/父索引/观测状态/坐标帧契约
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证零/负高度、最近/最高观测、可见/遮挡状态、父点和compact索引身份。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 157 · cloud.image_roi

- 当前：`std-image` / `1.0.0` / 图像区域选择点云。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/p2_model_projection.cpp:388](../../packs/std-image/ops/p2_model_projection.cpp#L388)。
- 端口：`cloud:PointCloud`, `mask:Image`, `calib:Record`, `cloudToCamera:Transform?` → `indices:Indices`, `cloud:PointCloud`, `quality:Record`。
- 参数单位标注：occlusionTolerance=m。
- 依赖：当前算法：OpenCV core/imgproc及相机畸变/数值求解的geometry模块、LyFlow轻量云/位姿类型；无需PCL。；目标：投影/栅格适配使用图像后端，共享映射/射线数值核，不引点云包注册依赖。
- 当前契约：输入[cloud:PointCloud, mask:Image, calib:Record, cloudToCamera:Transform?] → 输出[indices:Indices, cloud:PointCloud, quality:Record]。按标定和显式云到相机变换投影，输出关联原云的ROI索引与保通道子云。零mask为空ROI；可选z-buffer排除遮挡点，深度容差为米。
- 目标契约：带K/D的像素mask投影适配留std-image的投影模块；索引关联原云，公共轻量云类型不带PCL。
- 迁移前置：M1；M1映射组/父索引/观测状态/坐标帧契约
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证跨帧外参、零mask、z-buffer遮挡、通道保留及索引父身份。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 158 · cloud.orthographic_map

- 当前：`std-image` / `1.0.0` / 点云正交多特征图。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/p5_projection.cpp:1222](../../packs/std-image/ops/p5_projection.cpp#L1222)。
- 端口：`cloud:PointCloud`, `gridToCloud:Transform?` → `cloud:PointCloud`, `sourceCloud:PointCloud`, `depth:Image`, `status:Image`, `indexMap:Tensor`, `sourceIndexMap:Tensor`, `mapping:Record`, `quality:Record`, `image:Image`, `pseudo:Image`, `normalAngle:Image`, `normalValid:Image`, `pointState:Tensor`。
- 参数单位标注：width=px, height=px, resolution=m, originX=m, originY=m, minHeight=m, maxHeight=m, normalResolution=m。
- 依赖：当前算法：OpenCV core/imgproc及相机畸变/数值求解的geometry模块、LyFlow轻量云/位姿类型；无需PCL。；目标：投影/栅格适配使用图像后端，共享映射/射线数值核，不引点云包注册依赖。
- 当前契约：输入[cloud:PointCloud, gridToCloud:Transform?] → 输出[cloud:PointCloud, sourceCloud:PointCloud, depth:Image, status:Image, indexMap:Tensor, sourceIndexMap:Tensor, mapping:Record, quality:Record, image:Image, pseudo:Image, normalAngle:Image, normalValid:Image, pointState:Tensor]。显式工作空间栅格按最高真实观测生成有符号高度，独立状态保留零/负高度与空像素。compact云及HW索引组成organized视图；父云索引和遮挡状态独立保留。
- 目标契约：多特征正交栅格留图像depth-mapping模块，法线角弧度及独立valid与高度输出并存。
- 迁移前置：M1；M1映射组/父索引/观测状态/坐标帧契约
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证法线旋转/缺失、normalValid、伪彩及与height_map共有映射一致。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 159 · cloud.project_camera

- 当前：`std-image` / `1.0.0` / 点云相机栅格对齐。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/p5_projection.cpp:1265](../../packs/std-image/ops/p5_projection.cpp#L1265)。
- 端口：`cloud:PointCloud`, `image:Image`, `calib:Record`, `cloudToCamera:Transform?` → `cloud:PointCloud`, `sourceCloud:PointCloud`, `depth:Image`, `status:Image`, `indexMap:Tensor`, `sourceIndexMap:Tensor`, `mapping:Record`, `quality:Record`, `image:Image`, `pointState:Tensor`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgproc及相机畸变/数值求解的geometry模块、LyFlow轻量云/位姿类型；无需PCL。；目标：投影/栅格适配使用图像后端，共享映射/射线数值核，不引点云包注册依赖。
- 当前契约：输入[cloud:PointCloud, image:Image, calib:Record, cloudToCamera:Transform?] → 输出[cloud:PointCloud, sourceCloud:PointCloud, depth:Image, status:Image, indexMap:Tensor, sourceIndexMap:Tensor, mapping:Record, quality:Record, image:Image, pointState:Tensor]。按完整K/D和显式外参投影到相机工作栅格；最近Z-buffer、compact可见云、原生像素索引与全部父点状态，保留尺度和父索引，不制造PointCloud组织属性。
- 目标契约：K/D、相机栅格与Z-buffer投影留图像模块，云类型/通道不依赖PCL，跨帧外参显式。
- 迁移前置：M1；M1映射组/父索引/观测状态/坐标帧契约
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证畸变、正深度、遮挡、父云状态、相机法线和颜色来源。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 160 · cloud.remove_mask_primitives

- 当前：`std-image` / `1.0.0` / 按三维工件关联像素掩膜移除云。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/p5_pose_projection.cpp:1065](../../packs/std-image/ops/p5_pose_projection.cpp#L1065)。
- 端口：`cloud:PointCloud`, `poses:Record`, `detections:Record`, `calib:Record`, `masks:Tensor`, `maskInfo:Record`, `cloudToCamera:Transform?` → `cloud:PointCloud`, `keptIndices:Indices`, `removedIndices:Indices`, `unknownIndices:Indices`, `quality:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgproc及相机畸变/数值求解的geometry模块、LyFlow轻量云/位姿类型；无需PCL。；目标：投影/栅格适配使用图像后端，共享映射/射线数值核，不引点云包注册依赖。
- 当前契约：输入[cloud:PointCloud, poses:Record, detections:Record, calib:Record, masks:Tensor, maskInfo:Record, cloudToCamera:Transform?] → 输出[cloud:PointCloud, keptIndices:Indices, removedIndices:Indices, unknownIndices:Indices, quality:Record]。米制云+Pose3D+关联像素detections/masks/maskInfo+K/D与显式云到相机变换，输出保留云及kept/removed/unknown索引，不能丢失父云身份。
- 目标契约：三维实体与二维mask关联剔云留std-image投影模块；保留kept/removed/unknown三分索引及父云身份。
- 迁移前置：M1；M1映射组/父索引/观测状态/坐标帧契约
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证原图绑定、topExpand、不同父云拒绝、不可见unknown及输入云不变。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 161 · cloud.roi_extract

- 当前：`std-image` / `1.0.0` / 二维实例关联点云提取。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/p5_projection.cpp:1336](../../packs/std-image/ops/p5_projection.cpp#L1336)。
- 端口：`cloud:PointCloud`, `depth:Image?`, `status:Image?`, `indexMap:Tensor?`, `mapping:Record?`, `calib:Record?`, `cloudToCamera:Transform?`, `instances:Record?`, `masks:Tensor?`, `maskInfo:Record?`, `mask:Image?` → `cloud:PointCloud`, `sourceCloud:PointCloud`, `depth:Image`, `status:Image`, `indexMap:Tensor`, `sourceIndexMap:Tensor`, `mapping:Record`, `quality:Record`, `indices:Indices`, `sourceIndices:Indices`, `groups:Record`。
- 参数单位标注：offsetX=px, offsetY=px。
- 依赖：当前算法：OpenCV core/imgproc及相机畸变/数值求解的geometry模块、LyFlow轻量云/位姿类型；无需PCL。；目标：投影/栅格适配使用图像后端，共享映射/射线数值核，不引点云包注册依赖。
- 当前契约：输入[cloud:PointCloud, depth:Image?, status:Image?, indexMap:Tensor?, mapping:Record?, calib:Record?, cloudToCamera:Transform?, instances:Record?, masks:Tensor?, maskInfo:Record?, mask:Image?] → 输出[cloud:PointCloud, sourceCloud:PointCloud, depth:Image, status:Image, indexMap:Tensor, sourceIndexMap:Tensor, mapping:Record, quality:Record, indices:Indices, sourceIndices:Indices, groups:Record]。消费P4实例及原生掩膜绑定，支持组织索引视图或K/D投影的未组织源云。多ROI用Groups/Indices保留重叠和稳定ID，同时交付选定ROI的组织视图；显式空ROI成功空结果。
- 目标契约：实例mask与组织/未组织云关联留图像投影模块；Groups和Indices标准协议共享，重叠ROI不能破坏身份。
- 迁移前置：M1；M1映射组/父索引/观测状态/坐标帧契约
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证K/D未组织路径、映射组路径、重叠稳定ID、空ROI和sourceIndices父身份。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 162 · cloud.to_depth_image

- 当前：`std-image` / `1.0.0` / 点云转深度图。目标：`std-image`；拆公共能力/适配；**M6**。
- 源码：[packs/std-image/ops/image_cloud.cpp:271](../../packs/std-image/ops/image_cloud.cpp#L271)。
- 端口：`cloud:PointCloud` → `depth:Image`。
- 参数单位标注：fx=px, fy=px, cx=px, cy=px, width=px, height=px, depthScale=m。
- 依赖：当前计算：标准C++与LyFlow原生Image/Tensor/PointCloud类型，不调用OpenCV或PCL。；当前构建仍受std-image整包OpenCV链接约束；目标拆成无重依赖的显式能力模块。
- 当前契约：输入[cloud:PointCloud] → 输出[depth:Image]。按针孔内参把相机坐标系下的点云投影成深度图：一个像素落多个点取最近的，没落到点的像素是 0，z ≤ 0 与投影到图外的点丢掉；u16 输出时超出量程（取整后为 0 或大于 65535）的点也丢掉。与 cloud.from_depth 互逆：同一组内参、同一个 depthScale（f32 输出是米，对应 from_depth 的 depthScale = 1）来回一趟，有效像素不变。
- 目标契约：米制相机云到像素深度图保留在portable-depth适配器；最近Z和u16量化拒绝规则独立于PCL。
- 迁移前置：M1；公共轻量针孔投影核，不修改无畸变针孔算法
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证同像素最近点、相机背面、u16上下限及from_depth往返。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 163 · depth.defects

- 当前：`std-image` / `1.0.0` / 深度平面缺陷检测。目标：`std-image`；拆公共能力/适配；**M6**。
- 源码：[packs/std-image/ops/p5_inspection.cpp:1211](../../packs/std-image/ops/p5_inspection.cpp#L1211)。
- 端口：`cloud:PointCloud`, `depth:Image`, `status:Image`, `indexMap:Tensor`, `mapping:Record`, `plane:Plane`, `roi:Box2D?`, `mask:Image?` → `instances:Record`, `defects:Record`, `mask:Image`, `height:Image`, `overlay:Image`。
- 参数单位标注：distanceThreshold=m, lengthThreshold=m, widthThreshold=m, areaThreshold=m2, volumeThreshold=m3。
- 依赖：当前算法：OpenCV core/imgproc及相机畸变/数值求解的geometry模块、LyFlow轻量云/位姿类型；无需PCL。；目标：投影/栅格适配使用图像后端，共享映射/射线数值核，不引点云包注册依赖。
- 当前契约：输入[cloud:PointCloud, depth:Image, status:Image, indexMap:Tensor, mapping:Record, plane:Plane, roi:Box2D?, mask:Image?] → 输出[instances:Record, defects:Record, mask:Image, height:Image, overlay:Image]。输入完整DepthMapping组和同帧Plane，像素ROI/mask可选；按有符号米制距离及长度m/面积m2/体积m3阈值出缺陷，覆盖不足为unknown，good=null。
- 目标契约：栅格连通支持/真实观测证据包装留std-image；面积体积几何核下沉std-metrology，阈值策略与宿主最终判定分离。
- 迁移前置：M1；M1映射组/父索引/观测状态/坐标帧契约；M2物理长度/面积/体积测量核
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证defect/clear/unknown三态、缺观测good=null、补值选择、物理面积体积与平移不变性。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。 缺陷提取/覆盖证据留此适配器；通用量测核下沉std-metrology，最终工位OK/NG归宿主。

## 164 · depth.profile_slices

- 当前：`std-image` / `1.0.0` / 组织深度轮廓截面。目标：`std-image`；拆公共能力/适配；**M6**。
- 源码：[packs/std-image/ops/p5_inspection.cpp:1243](../../packs/std-image/ops/p5_inspection.cpp#L1243)。
- 端口：`cloud:PointCloud`, `depth:Image`, `status:Image`, `indexMap:Tensor`, `mapping:Record`, `plane:Plane`, `region:Record?` → `profiles:Tensor`, `attributes:Tensor`, `attributeValid:Tensor`, `supports:Tensor`, `sliceCloud:PointCloud`, `indices:Indices`, `groups:Record`, `features:Record`, `overlay:Image`, `plot:Image`。
- 参数单位标注：maxEdgeLength=m, min=m, max=m, aCenter=m, aWidth=m, bCenter=m, bWidth=m。
- 依赖：当前算法：OpenCV core/imgproc及相机畸变/数值求解的geometry模块、LyFlow轻量云/位姿类型；无需PCL。；目标：投影/栅格适配使用图像后端，共享映射/射线数值核，不引点云包注册依赖。
- 当前契约：输入[cloud:PointCloud, depth:Image, status:Image, indexMap:Tensor, mapping:Record, plane:Plane, region:Record?] → 输出[profiles:Tensor, attributes:Tensor, attributeValid:Tensor, supports:Tensor, sliceCloud:PointCloud, indices:Indices, groups:Record, features:Record, overlay:Image, plot:Image]。完整DepthMapping组与同帧Plane，可选像素区域；三角网格与截面求交，profiles[M,4]=slice/x/height/chain，supports[M,3]指父云顶点；属性数值须配attributeValid。
- 目标契约：组织三角网格截取及原始支持映射留std-image适配器；有序链特征抽取下沉std-profile，物理属性计算下沉std-metrology。
- 迁移前置：M1；M1映射组/父索引/观测状态/坐标帧契约；M3有序截面链/特征接口；M2物理属性测量接口
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证真实三角交线、断链不跨孔洞、[M,4]/[M,3]对齐、零基slice与未知属性valid。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。 这是有源三角网格支持的组织截面，不能直接等同单条激光轮廓；采样适配与profile算法拆开。

## 165 · image.arithmetic

- 当前：`std-image` / `1.0.0` / 图像算术运算。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_analysis.cpp:612](../../packs/std-image/ops/image_analysis.cpp#L612)。
- 端口：`a:Image`, `b:Image` → `image:Image`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[a:Image, b:Image] → 输出[image:Image]。同尺寸/通道/位深图像加、减及绝对差。整数饱和，f32 保留带符号与非有限值。
- 目标契约：同尺寸/通道/位深的图像算术保留整数饱和与f32带符号/非有限语义；标量算术仍std-value。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证饱和加减、绝对差、f32 NaN和形状不匹配。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 166 · image.barcode_decode

- 当前：`std-image` / `1.0.0` / 条码定位解码。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/p4_barcodes.cpp:468](../../packs/std-image/ops/p4_barcodes.cpp#L468)。
- 端口：`image:Image`, `roi:Box2D?`, `region:Record?` → `instances:Record`, `detections:Record`, `image:Image`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：ZXing-C++真实解码后端；OpenCV core/imgproc用于图像适配、ROI与原生叠画。；目标：barcode可选后端/许可证独立声明，不成为基础image必选依赖。
- 当前契约：输入[image:Image, roi:Box2D?, region:Record?] → 输出[instances:Record, detections:Record, image:Image]。ZXing-C++真实多格式解码，UTF-8文本与原始字节hex分别保留；valid独立于空内容。原图像素四角与非抓取位姿，叠图保留输入位深/通道/alpha。取消在库阶段边界检查。
- 目标契约：条码真实解码留std-image的可选barcode模块，ZXing后端仅模块启用时链接，原生叠图与结果协议公共。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证真实多格式、空内容有效码、UTF-8与原始hex、四角及取消。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 167 · image.blur

- 当前：`std-image` / `1.0.0` / 图像平滑滤波。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_basic.cpp:237](../../packs/std-image/ops/image_basic.cpp#L237)。
- 端口：`image:Image` → `image:Image`。
- 参数单位标注：ksize=px, sigma=px。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[image:Image] → 输出[image:Image]。高斯 / 中值 / 均值平滑。中值对椒盐噪声最好、保边；高斯最通用。
- 目标契约：保留高斯/中值/均值各自核和边界行为，图像平滑不下沉到有序轮廓包。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证不同核、奇数约束、通道及边界像素。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 168 · image.board_calib

- 当前：`std-image` / `1.0.0` / 棋盘格平面标定。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/board_calib.cpp:114](../../packs/std-image/ops/board_calib.cpp#L114)。
- 端口：`image:Image` → `calib:Record`, `rms:Measurement`, `overlay:Record`。
- 参数单位标注：square=mm。
- 依赖：当前算法：OpenCV core/imgproc/geometry（OpenCV4为calib3d）；标定数据类型由公共协议提供。；目标：相机几何数值核共享，图像特征检测/重采样留OpenCV模块。
- 当前契约：输入[image:Image] → 输出[calib:Record, rms:Measurement, overlay:Record]。棋盘格图 → 工作平面标定 image.PlaneCalib（图像 px → 板面 mm 的 3×3 单应）。findChessboardCornersSB 找内角点，全部角点一起求单应，报残差 RMS 与平均 mm/px。板要放在要量的那个平面上（涂胶飞拍：内边所在的高度）。原点在第一个角点。
  图里找不到棋盘是执行错误 no_board（这是一次性的离线步骤，要人去看）。
- 目标契约：图像角点检测与平面单应估计留在标定模块；PlaneCalib模型和验证器为公共数据契约。
- 迁移前置：M1；M1统一PlaneCalib单位及齐次矩阵验证，提供旧输入显式迁移
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证棋盘全分辨率u8、px到mm单应、RMS及未检测到板的诊断。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。 现有PlaneCalib有两套验证器：lyflow_cv/plane_calib.h默认mm且允许非空unit，core/calibration.h要求显式m/mm并归一化H。统一校验属于契约变更，不能在搬包时静默替换。

## 169 · image.board_pose

- 当前：`std-image` / `1.0.0` / 棋盘格三维位姿估计。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_p1_calib.cpp:420](../../packs/std-image/ops/image_p1_calib.cpp#L420)。
- 端口：`image:Image`, `calib:Record`, `corners:Record?` → `pose:Transform`, `instance:Record`, `rms:Measurement`, `corners:Record`, `quality:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgproc/geometry（OpenCV4为calib3d）；标定数据类型由公共协议提供。；目标：相机几何数值核共享，图像特征检测/重采样留OpenCV模块。
- 当前契约：输入[image:Image, calib:Record, corners:Record?] → 输出[pose:Transform, instance:Record, rms:Measurement, corners:Record, quality:Record]。全分辨率u8图+K/D及可选有序角点；PnP输出board→camera米制Transform和Pose3D，重投影RMS单位px，支持拒绝平面歧义。
- 目标契约：图像角点与K/D PnP保留图像标定模块；board到camera的Transform米制、RMS像素及歧义策略保留。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证完整棋盘、显式角点、正深度、平面PnP双解质量和歧义拒绝。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 170 · image.change_stats

- 当前：`std-image` / `1.0.0` / 图像变化统计。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_analysis.cpp:625](../../packs/std-image/ops/image_analysis.cpp#L625)。
- 端口：`a:Image`, `b:Image`, `mask:Image?` → `mask:Image`, `stats:Record`。
- 参数单位标注：openRadius=px, minArea=px²。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[a:Image, b:Image, mask:Image?] → 输出[mask:Image, stats:Record]。按灰度亮度或最大通道绝对差统计变化，支持十字开运算抗孤立噪声和最小变化面积判定；忽略非有限像素及 alpha。
- 目标契约：图像变化mask与统计留图像模块；区域变化阈值是显式算法参数，不推导产线最终OK/NG。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证alpha忽略、非有限像素、十字去噪及最小变化面积边界。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 171 · image.circle_caliper

- 当前：`std-image` / `1.0.0` / 圆弧边缘卡尺。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_p1_circle.cpp:385](../../packs/std-image/ops/image_p1_circle.cpp#L385)。
- 端口：`image:Image` → `circle:Circle2D`, `quality:Record`。
- 参数单位标注：center=px, radiusMin=px, radiusMax=px, radialStep=px, fitTolerance=px, maxRms=px, maxCenterOffset=px。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[image:Image] → 输出[circle:Circle2D, quality:Record]。圆环ROI径向扫描、切向平均、抛物线亚像素边缘和确定性鲁棒圆拟合；无目标、退化弧或质量不足报告诊断。
- 目标契约：图像圆环扫描、切向平均及鲁棒圆拟合留像素卡尺；数值核可共享但不能改成霍夫圆。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证弧覆盖、边缘极性、抛物线亚像素、退化/质量不足诊断。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 172 · image.color_adjust

- 当前：`std-image` / `1.0.0` / 图像颜色调整。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_p1_basic.cpp:315](../../packs/std-image/ops/image_p1_basic.cpp#L315)。
- 端口：`image:Image` → `image:Image`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[image:Image] → 输出[image:Image]。支持u8/u16/f32灰度、RGB、RGBA；归一化颜色结果裁到[0,1]，alpha保持原值。
- 目标契约：RGB归一化域亮度/对比度/gamma/HSV及通道增益留颜色模块；alpha原值必须保留。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证u8/u16/f32、灰度、RGB/RGBA、裁剪与alpha不变。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 173 · image.components

- 当前：`std-image` / `1.0.0` / 连通域分析。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_analysis.cpp:722](../../packs/std-image/ops/image_analysis.cpp#L722)。
- 端口：`image:Image`, `mask:Image?` → `regions:Record`, `mask:Image`。
- 参数单位标注：minArea=px², maxArea=px²。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[image:Image, mask:Image?] → 输出[regions:Record, mask:Image]。有限非零前景的连通域面积、质心、边界框和可选轮廓；空前景有效返回空集合，超预算报错而非截断。
- 目标契约：像素连通域提取保留面积/质心/轮廓和预算上限；空前景是有效空集合，禁止截断结果。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证4/8连接、空集合、稳定顺序、轮廓及预算失败原子性。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 174 · image.concat

- 当前：`std-image` / `1.0.0` / 图像排布拼接。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_p1_basic.cpp:304](../../packs/std-image/ops/image_p1_basic.cpp#L304)。
- 端口：`a:Image`, `b:Image` → `image:Image`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[a:Image, b:Image] → 输出[image:Image]。拼接两幅同位深/通道/比例图像，尺寸不足处填充，不做全景配准或隐式颜色转换。
- 目标契约：同位深/通道/比例图像排布留图像模块；不承担全景拼接或隐式颜色变换。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证横纵拼接、不等尺寸填充、通道/位深/scale不匹配。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 175 · image.convert_channels

- 当前：`std-image` / `1.0.0` / 图像通道转换。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_analysis.cpp:601](../../packs/std-image/ops/image_analysis.cpp#L601)。
- 端口：`image:Image` → `image:Image`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[image:Image] → 输出[image:Image]。灰度/RGB/RGBA 转换，保持 u8/u16/f32 位深；新增 alpha 为满量程。
- 目标契约：灰度/RGB/RGBA通道转换保留位深和新增alpha满量程规则。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证六种通道变换、u8/u16/f32及alpha满量程。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 176 · image.crop

- 当前：`std-image` / `1.0.0` / 图像裁剪。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_basic.cpp:208](../../packs/std-image/ops/image_basic.cpp#L208)。
- 端口：`image:Image`, `box:Box2D?` → `image:Image`。
- 参数单位标注：roi=px。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[image:Image, box:Box2D?] → 输出[image:Image]。按像素矩形 [x0, y0, x1, y1] 裁出一块（主预览里可以在输入图上拖框）；接了 box 就用框（像素单位的 Box2D）。超出图像的部分截掉。
- 目标契约：裁剪以像素Box2D和边界截取为契约；保留图像坐标原点/scale转换门槛，禁止直接接物理Box。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证部分越界、空交集、输入box单位错误及预览坐标。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 177 · image.depth_filter

- 当前：`std-image` / `1.0.0` / 物理深度感知滤波。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/p5_projection.cpp:1288](../../packs/std-image/ops/p5_projection.cpp#L1288)。
- 端口：`depth:Image`, `cloud:PointCloud?`, `status:Image?`, `indexMap:Tensor?`, `mapping:Record?`, `calib:Record?` → `cloud:PointCloud`, `sourceCloud:PointCloud`, `depth:Image`, `status:Image`, `indexMap:Tensor`, `sourceIndexMap:Tensor`, `mapping:Record`, `quality:Record`, `changed:Image`。
- 参数单位标注：kernelSize=px, sigma=px, threshold=m, maxDepthJump=m。
- 依赖：当前算法：OpenCV core/imgproc及相机畸变/数值求解的geometry模块、LyFlow轻量云/位姿类型；无需PCL。；目标：投影/栅格适配使用图像后端，共享映射/射线数值核，不引点云包注册依赖。
- 当前契约：输入[depth:Image, cloud:PointCloud?, status:Image?, indexMap:Tensor?, mapping:Record?, calib:Record?] → 输出[cloud:PointCloud, sourceCloud:PointCloud, depth:Image, status:Image, indexMap:Tensor, sourceIndexMap:Tensor, mapping:Record, quality:Record, changed:Image]。完整五模式深度滤波，支持带映射的有符号高度，或F32深度+标定直接建图。无变化保留真实源XYZ，改深度沿原射线；输出仅有效结果点，新索引重绑并保留父云源索引。maxDepthJump=0关闭平滑/补值的边缘门控。
- 目标契约：深度图邻域/填补仍图像depth-mapping模块，任何改深度按原射线重建并原子重绑映射组。
- 迁移前置：M1；M1映射组/父索引/观测状态/坐标帧契约
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证五模式、maxDepthJump=0、observed/interpolated、原XYZ保留与新索引绑定。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 178 · image.difference_mask

- 当前：`std-image` / `1.0.0` / 抗边缘图像差分。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_difference.cpp:142](../../packs/std-image/ops/image_difference.cpp#L142)。
- 端口：`a:Image`, `b:Image`, `mask:Image?` → `mask:Image`, `aligned:Image`, `stats:Record`。
- 参数单位标注：maxShift=px, edgeDistance=px。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[a:Image, b:Image, mask:Image?] → 输出[mask:Image, aligned:Image, stats:Record]。先按显式配置进行有置信度和位移上限的平移配准，再做正/负/双向差分；可排除两幅图共同边缘附近噪声。不进行旋转或尺度配准。
- 目标契约：平移配准和差分mask留图像模块；位移/置信门控、共同边缘排除与正负差分保持。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证已知平移、低置信拒绝、共同边缘和不支持旋转/尺度的边界。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 179 · image.edge_positions

- 当前：`std-image` / `1.0.0` / 投影边缘定位。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_caliper.cpp:481](../../packs/std-image/ops/image_caliper.cpp#L481)。
- 端口：`image:Image` → `positions:Record`。
- 参数单位标注：roi=px。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[image:Image] → 输出[positions:Record]。全 ROI 沿横向均值投影后，按方向和极性选择单边、双边或多个梯度峰。空目标有效返回 count=0，不返回 -1 哨兵坐标。
- 目标契约：像素亮度投影和峰选择留图像模块，空目标返回count=0而非哨兵坐标。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证方向/极性、单双多边、峰顺序及成功空结果。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 180 · image.find_circle

- 当前：`std-image` / `1.0.0` / 霍夫圆检测。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_segment.cpp:291](../../packs/std-image/ops/image_segment.cpp#L291)。
- 端口：`image:Image` → `circle:Circle2D`。
- 参数单位标注：minDist=px, accumulatorThreshold=px, minRadius=px, maxRadius=px。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[image:Image] → 输出[circle:Circle2D]。霍夫梯度法找圆，取累加值最高的一个。输出像素坐标的 Circle2D（unit = px）。边缘噪声多时先接 image.blur。
- 目标契约：保留霍夫梯度检测和像素Circle2D输出；与轮廓圆拟合、三维圆恢复分开命名和验收。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证霍夫正反例、最佳圆选择、空图和像素单位。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 181 · image.find_circles_3d

- 当前：`std-image` / `1.0.0` / 空间圆孔检测。目标：`std-image`；拆公共能力/适配；**M6**。
- 源码：[packs/std-image/ops/p5_inspection.cpp:1155](../../packs/std-image/ops/p5_inspection.cpp#L1155)。
- 端口：`image:Image`, `calib:Record`, `radii:Tensor?` → `instances:Record`, `circles:Record`, `ellipses:Record`, `circles2d:Record`, `image:Image`, `edges:Image`。
- 参数单位标注：thetaFsa=deg, radius=m, edgeTolerance=px, thickness=m。
- 依赖：当前算法：OpenCV core/imgproc及相机畸变/数值求解的geometry模块、LyFlow轻量云/位姿类型；无需PCL。；目标：投影/栅格适配使用图像后端，共享映射/射线数值核，不引点云包注册依赖。
- 当前契约：输入[image:Image, calib:Record, radii:Tensor?] → 输出[instances:Record, circles:Record, ellipses:Record, circles2d:Record, image:Image, edges:Image]。独立Canny弧段/最小二乘椭圆与角覆盖评分；已知米制半径、K/D校正后按圆锥二次型真实恢复两歧义空间圆，不选择一个冒充确定姿态。稳定原图顺序匹配半径；二维圆按偏心率独立筛选。
- 目标契约：像素椭圆检测及校准空间圆恢复留图像标定检测模块；纯几何圆锥求解可共享但观测包装不入轻量包。
- 迁移前置：M1；M1映射组/父索引/观测状态/坐标帧契约
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证Canny弧支持、K/D、已知半径数量、稳定排序及双歧义圆同时保留。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 182 · image.height_extract

- 当前：`std-image` / `1.0.0` / 正交深度高度提取。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/p2_height_extraction.cpp:199](../../packs/std-image/ops/p2_height_extraction.cpp#L199)。
- 端口：`image:Image` → `height:Image`, `reference:Image`, `mask:Image`, `image:Image`。
- 参数单位标注：pixelSizeX=m/px, pixelSizeY=m/px, originX=m, originY=m, depthScale=m, z=m, point0=px, point1=px, point2=px, radius=m, customDepth=m, noise=m。
- 依赖：当前算法：OpenCV core/imgproc及相机畸变/数值求解的geometry模块、LyFlow轻量云/位姿类型；无需PCL。；目标：投影/栅格适配使用图像后端，共享映射/射线数值核，不引点云包注册依赖。
- 当前契约：输入[image:Image] → 输出[height:Image, reference:Image, mask:Image, image:Image]。全分辨率正交深度。正高度沿+Z；平面输出有符号法向距离，曲面输出轴向差。low映射为正亮度，both零点128；无效值始终mask=0。
- 目标契约：正交深度参考面/曲面拟合与图像残差留std-image；物理高度数值核可提取，但u16尺度和亮度编码在适配器。
- 迁移前置：M1；M1映射组/父索引/观测状态/坐标帧契约
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证正/负高度、法向距离与轴向差区别、both零点128和mask无效性。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 183 · image.highlights

- 当前：`std-image` / `1.0.0` / 高光直线检测。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_highlights.cpp:665](../../packs/std-image/ops/image_highlights.cpp#L665)。
- 端口：`image:Image`, `roi:Box2D?`, `region:Record?` → `instances:Record`, `segments:Record`, `mask:Image`, `image:Image`。
- 参数单位标注：ridgeRadius=px, smoothSigma=px, minLength=px, maxGap=px, maxResidual=px, boxHeight=px, mergeDistance=px。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[image:Image, roi:Box2D?, region:Record?] → 输出[instances:Record, segments:Record, mask:Image, image:Image]。双轴亮脊与双侧对比度检测，按真实支持点拟合和分组高光直线。返回原图像素旋转区域、线段质量及只读源图叠画；confidence为算法质量分数。
- 目标契约：亮脊/双侧对比度与原图线段/旋转区域留图像特征；质量分数不解释成概率或抓取许可。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证真实支持点、双轴、多组线、像素ROI与只读源图叠画。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 184 · image.instances_mask

- 当前：`std-image` / `1.0.0` / 工件实例掩膜生成。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_p1_basic.cpp:342](../../packs/std-image/ops/image_p1_basic.cpp#L342)。
- 端口：`image:Image`, `instances:Record` → `mask:Image`, `labels:Image`, `index:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[image:Image, instances:Record] → 输出[mask:Image, labels:Image, index:Record]。像素Pose2D旋转矩形光栅化，输出二值mask、u16标签及稳定id映射。不是模型实例分割。
- 目标契约：像素Pose2D矩形实例光栅化留图像模块；实例Record标准由公共pose协议提供。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证稳定ID标签、重叠策略、u16容量及不是学习实例分割。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 185 · image.instances_morphology

- 当前：`std-image` / `1.0.0` / 逐实例形态学。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_p1_basic.cpp:353](../../packs/std-image/ops/image_p1_basic.cpp#L353)。
- 端口：`labels:Image`, `index:Record` → `mask:Image`, `labels:Image`, `index:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[labels:Image, index:Record] → 输出[mask:Image, labels:Image, index:Record]。u16标签图+vision.InstanceLabels.v1映射；逐标签椭圆核形态学，first/last/error冲突策略，显式保留原标签，输出mask/labels/index配套。
- 目标契约：逐标签形态学和冲突策略留图像模块；标签与稳定ID映射必须联合验证和交付。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证逐实例操作、重叠优先级、preserveOriginal与孤立标签。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 186 · image.line_caliper

- 当前：`std-image` / `1.0.0` / 直线卡尺。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_caliper.cpp:413](../../packs/std-image/ops/image_caliper.cpp#L413)。
- 端口：`image:Image` → `line:Line2D`, `position:Measurement`, `samples:Record`。
- 参数单位标注：roi=px, band=px, fitTolerance=px。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[image:Image] → 输出[line:Line2D, position:Measurement, samples:Record]。矩形 ROI 等间隔扫描，中心差分极性边缘与抛物线亚像素定位，Huber 拟合后按容差筛选。无目标返回 edge_not_found，不构造假直线。
- 目标契约：图像扫描/边缘定位和Huber直线拟合仍属像素卡尺模块；仅独立数值拟合核可复用，不改现有算法。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证极性、亚像素、鲁棒外点和edge_not_found而非假直线。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 187 · image.load_annotations

- 当前：`std-image` / `1.0.0` / 数据集标注加载。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/p4_annotations.cpp:506](../../packs/std-image/ops/p4_annotations.cpp#L506)。
- 端口：无 → `image:Image`, `overlay:Image`, `instances:Record`, `masks:Tensor`, `maskInfo:Record`, `rois:Record`, `roiMask:Image`, `quality:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：标准C++/JSON文件解析与OpenCV imgcodecs/imgproc读取/光栅化。；目标：外部数据集解析适配和标准图像/实例协议分层，无PCL或ORT。
- 当前契约：输入[无] → 输出[image:Image, overlay:Image, instances:Record, masks:Tensor, maskInfo:Record, rois:Record, roiMask:Image, quality:Record]。按显式时间戳读取数据集内的对应图片、实例和ROI。多polygon/洞保留原生NHW掩膜，缺mask不伪造bbox分割。源pickable仅供筛选，交付实例仍不可抓取。关闭结果缓存，每次重读文件。
- 目标契约：数据集图片/实例/掩膜加载归图像数据IO适配器；外部格式解析与标准实例模型分离，保留无缓存。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证显式时间戳、多polygon/洞、缺mask不补bbox及源pickable不授权抓取。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 188 · image.load_calib

- 当前：`std-image` / `1.0.0` / 平面标定读取。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/load_calib.cpp:60](../../packs/std-image/ops/load_calib.cpp#L60)。
- 端口：无 → `calib:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前计算：标准C++文件IO、JSON和公共calibration验证器，无求解/检测后端。；目标：标定文件适配与类型验证独立；不因加载标定强制启用Marker或ONNX。
- 当前契约：输入[无] → 输出[calib:Record]。读一份 image.PlaneCalib 的 JSON 文件（整个 Record 或只有 data 都行），出 Record。标定属于相机工位：宿主标一次存成文件，产线图经这个节点读，接到量测积木的 calib 口。当前旧验证器unit缺省mm，接受任意非空单位字符串；不能假定它已经等同只支持m/mm的新标定验证器。
- 目标契约：PlaneCalib文件IO归图像标定适配器；共享模型验证器供metrology使用，禁止metrology反向调用图像注册表。
- 迁移前置：M1；M1统一PlaneCalib单位及齐次矩阵验证，提供旧输入显式迁移
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证完整Record/data两种JSON、退化H、单位错误和文件重读。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。 现有PlaneCalib有两套验证器：lyflow_cv/plane_calib.h默认mm且允许非空unit，core/calibration.h要求显式m/mm并归一化H。统一校验属于契约变更，不能在搬包时静默替换。

## 189 · image.load_camera_calib

- 当前：`std-image` / `1.0.0` / 相机内参读取。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_p1_calib.cpp:383](../../packs/std-image/ops/image_p1_calib.cpp#L383)。
- 端口：无 → `calib:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前计算：标准C++文件IO、JSON和公共calibration验证器，无求解/检测后端。；目标：标定文件适配与类型验证独立；不因加载标定强制启用Marker或ONNX。
- 当前契约：输入[无] → 输出[calib:Record]。读取JSON相机K/D、全图大小和帧名，输出vision.CameraCalib.v1标准Record，拒绝退化/非有限标定；文件IO按其外部依赖策略读取。
- 目标契约：K/D与全图尺寸文件加载保留图像标定IO；公共CameraCalib验证器不依赖OpenCV。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证JSON类型、K/D长度、原图大小、退化内参与坏文件。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 190 · image.marker_pose

- 当前：`std-image` / `1.0.0` / 识别码相机位姿。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/p3_markers.cpp:285](../../packs/std-image/ops/p3_markers.cpp#L285)。
- 端口：`image:Image`, `calib:Record` → `instances:Record`, `detections:Record`, `image:Image`。
- 参数单位标注：markerSize=m, thickness=m, maxRms=px, ambiguityGap=px, minArea=px2。
- 依赖：当前算法：OpenCV core/imgproc/geometry（OpenCV4为calib3d）；标定数据类型由公共协议提供。 字典识别另需OpenCV objdetect/aruco及固定STag字典数据。；目标：相机几何数值核共享，图像特征检测/重采样留OpenCV模块。
- 当前契约：输入[image:Image, calib:Record] → 输出[instances:Record, detections:Record, image:Image]。全分辨率u8识别码真实字典解码与K/D校准PnP。米制边长；局部X向右、Y向上、Z向外，四角TL/TR/BR/BL。无解码为空，质量拒绝逐目标诊断。
- 目标契约：真实字典解码和K/D PnP留图像识别；字典后端显式可选，ID与物理边长不降为颜色模板。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证ArUco/AprilTag/STag实码、角点顺序、米制边长和无目标/质量拒绝。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 191 · image.morphology

- 当前：`std-image` / `1.0.0` / 形态学运算。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_segment.cpp:248](../../packs/std-image/ops/image_segment.cpp#L248)。
- 端口：`image:Image` → `image:Image`。
- 参数单位标注：ksize=px。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[image:Image] → 输出[image:Image]。腐蚀 / 膨胀 / 开 / 闭。开运算去掉小亮点，闭运算填掉小暗洞，通常接在二值化后面。
- 目标契约：结构元素与腐蚀/膨胀/开闭保留在图像模块，不与轮廓一维清洗混为同一算法。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证核形状、开闭次序和边界处理。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 192 · image.normalize

- 当前：`std-image` / `1.0.0` / 位深转换与拉伸。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_basic.cpp:267](../../packs/std-image/ops/image_basic.cpp#L267)。
- 端口：`image:Image` → `image:Image`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[image:Image] → 输出[image:Image]。转换位深。拉伸：每个通道的 min..max 拉到目标位深的满量程（f32 是 0..1）；不拉伸：输出 = 输入 × scale + offset，超出量程截断。
- 目标契约：显式位深、scale/offset、分通道拉伸及饱和策略归图像值域模块，禁止隐式归一化。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证u8/u16/f32、常量通道及拉伸与线性转换差异。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 193 · image.ocr_crops

- 当前：`std-image` / `1.0.0` / 文字透视裁切。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/p4_ocr.cpp:567](../../packs/std-image/ops/p4_ocr.cpp#L567)。
- 端口：`image:Image`, `quads:Tensor`, `regions:Record` → `recInput:Tensor`, `clsInput:Tensor`, `layout:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgproc的图像缩放、轮廓或光栅化，及公共Tensor/Record/Pose协议；没有ORT会话调用。；目标：图像前后处理模块仅消费标准张量，实际推理经独立std-ml推理能力。
- 当前契约：输入[image:Image, quads:Tensor, regions:Record] → 输出[recInput:Tensor, clsInput:Tensor, layout:Record]。对每个真实四边形执行透视校正，保持批次id顺序；识别按等比48高右填零，方向模型48×192。空区域生成零批次张量。
- 目标契约：四边形透视校正与识别/方向模型图像预处理留图像适配器，批次ID/validWidth元数据共享。
- 迁移前置：M1；公共模型映射/批次/掩膜/文本结果schema
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证旋转文本、48高等比右填零、方向48×192和空批次。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 194 · image.ocr_ctc_decode

- 当前：`std-image` / `1.0.0` / 文字CTC解码。目标：`std-ml`；迁移归属；**M6**。
- 源码：[packs/std-image/ops/p4_ocr.cpp:592](../../packs/std-image/ops/p4_ocr.cpp#L592)。
- 端口：`prediction:Tensor`, `layout:Record` → `text:Record`, `results:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前计算：标准C++张量/Record处理；与预处理共享含OpenCV的编译单元或协议头，没有ORT执行调用。；目标：纯张量适配模块只依赖公共模型协议/类型，不要求ORT、OpenCV或std-image注册。
- 当前契约：输入[prediction:Tensor, layout:Record] → 输出[text:Record, results:Record]。严格UTF-8字典和类别数配套，blank=0，先折叠CTC重复再删除blank；保留逐行rawText/置信/过滤结果。低分行不进入合并文本，但结果条目保留；无目标返回成功空文本。
- 目标契约：纯张量CTC/字典解码迁std-ml的OCR适配模块，UTF-8字典IO与结果协议独立于OpenCV/图像注册表。
- 迁移前置：M1；公共YOLO/OCR预处理映射与结果schema从图像实现头提取
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证NTC/TNC、blank=0、重复折叠、Unicode字典类数、置信过滤和空文本。
- 说明：算子ID的image前缀不决定目标包；Image输入预处理留std-image，纯张量模型解码在std-ml，ORT会话仅由推理模块提供。

## 195 · image.ocr_db_decode

- 当前：`std-image` / `1.0.0` / 文字区域检测解码。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/p4_ocr.cpp:553](../../packs/std-image/ops/p4_ocr.cpp#L553)。
- 端口：`image:Image`, `prediction:Tensor`, `mapping:Record` → `quads:Tensor`, `regions:Record`, `instances:Record`, `image:Image`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgproc的图像缩放、轮廓或光栅化，及公共Tensor/Record/Pose协议；没有ORT会话调用。；目标：图像前后处理模块仅消费标准张量，实际推理经独立std-ml推理能力。
- 当前契约：输入[image:Image, prediction:Tensor, mapping:Record] → 输出[quads:Tensor, regions:Record, instances:Record, image:Image]。DB类单头概率解码，轮廓内均值评分和area/perimeter距离扩张有限旋转矩形，非任意多边形offset。稳定行排序；原图坐标四边形与id配对。叠图只画框，Unicode文字由results呈现。
- 目标契约：DB概率图轮廓/框扩张与原图叠画留图像模块；共享OCR协议不与推理节点注册耦合。
- 迁移前置：M1；公共模型映射/批次/掩膜/文本结果schema
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证单头概率、框扩张边界、稳定行序、原图四角和不是任意多边形offset。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 196 · image.ocr_orient

- 当前：`std-image` / `1.0.0` / 文字方向校正。目标：`std-ml`；迁移归属；**M6**。
- 源码：[packs/std-image/ops/p4_ocr.cpp:582](../../packs/std-image/ops/p4_ocr.cpp#L582)。
- 端口：`recInput:Tensor`, `classification:Tensor`, `layout:Record` → `input:Tensor`, `layout:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前计算：标准C++张量/Record处理；与预处理共享含OpenCV的编译单元或协议头，没有ORT执行调用。；目标：纯张量适配模块只依赖公共模型协议/类型，不要求ORT、OpenCV或std-image注册。
- 当前契约：输入[recInput:Tensor, classification:Tensor, layout:Record] → 输出[input:Tensor, layout:Record]。仅按可信180度类别翻转有效文字像素，右侧padding位置保持；不重复应用方向，空批次接受ONNX serial的canonical empty输出。
- 目标契约：纯张量方向分类后处理迁std-ml的OCR适配模块；共享layout，翻转仅有效文字区，禁止引OpenCV/重复方向。
- 迁移前置：M1；公共YOLO/OCR预处理映射与结果schema从图像实现头提取
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证[N,2]归一化概率、180度翻转、padding位置、空批次及重复应用拒绝。
- 说明：算子ID的image前缀不决定目标包；Image输入预处理留std-image，纯张量模型解码在std-ml，ORT会话仅由推理模块提供。

## 197 · image.ocr_preprocess

- 当前：`std-image` / `1.0.0` / 文字检测预处理。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/p4_ocr.cpp:538](../../packs/std-image/ops/p4_ocr.cpp#L538)。
- 端口：`image:Image`, `roi:Box2D?` → `input:Tensor`, `mapping:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgproc的图像缩放、轮廓或光栅化，及公共Tensor/Record/Pose协议；没有ORT会话调用。；目标：图像前后处理模块仅消费标准张量，实际推理经独立std-ml推理能力。
- 当前契约：输入[image:Image, roi:Box2D?] → 输出[input:Tensor, mapping:Record]。全分辨率OCR检测输入，保持RGB(A)源只读；默认PP-OCRv6 BGR/0.5归一化，逆映射保留原图ROI坐标。
- 目标契约：OCR图像缩放/通道归一化留图像模型适配器，输出张量与原图逆映射schema公共化；不调用ORT。
- 迁移前置：M1；公共模型映射/批次/掩膜/文本结果schema
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证PP-OCRv6 BGR、32对齐、ROI/scale、源图只读与归一化。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 198 · image.qr_refine

- 当前：`std-image` / `1.0.0` / 二维码模块增强。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/p4_barcodes.cpp:496](../../packs/std-image/ops/p4_barcodes.cpp#L496)。
- 端口：`image:Image`, `roi:Box2D?`, `detections:Record?` → `image:Image`, `feedback:Record`。
- 参数单位标注：modulePixels=px。
- 依赖：当前算法：OpenCV core/imgproc的透视/阈值/栅格；refine本身不调用ZXing解码。；目标：已给QR四角或已裁区域时无需解码后端，保持与barcode模块可独立组合。
- 当前契约：输入[image:Image, roi:Box2D?, detections:Record?] → 输出[image:Image, feedback:Record]。已知标准QR格数的透视裁正、模块均值阈值及静区重绘；不发明纠错内容。输入detections时需有效标准QR四角，多目标指定symbolId。缺省输入已裁好的QR模块区域。before与after采用相同裁正/静区几何，输出u8单通道新坐标系及明确反馈。
- 目标契约：已知QR网格的图像透视/均值阈值/静区重绘留图像模块；无需ZXing即可处理已给四角，禁止发明纠错内容。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证相同before/after几何、四角/symbolId、已裁图模式和模块格数拒绝。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 199 · image.range_mask

- 当前：`std-image` / `1.0.0` / 颜色范围分割。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_analysis.cpp:646](../../packs/std-image/ops/image_analysis.cpp#L646)。
- 端口：`image:Image` → `mask:Image`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[image:Image] → 输出[mask:Image]。灰度/RGB 原值范围或 HSV 范围，闭区间；HSV 使用 H 度、S/V 0..1，H 下界大于上界表示跨红色色相环绕。
- 目标契约：原值与HSV范围分离，H度与S/V 0..1、红色色相环绕保留在颜色分割模块。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证RGB/灰度原值区间、H跨零和无效范围。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 200 · image.region_stats

- 当前：`std-image` / `1.0.0` / 区域统计。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_segment.cpp:338](../../packs/std-image/ops/image_segment.cpp#L338)。
- 端口：`image:Image`, `mask:Image?` → `mean:Measurement`, `area:Measurement`, `bbox:Box2D`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[image:Image, mask:Image?] → 输出[mean:Measurement, area:Measurement, bbox:Box2D]。掩膜里（没接掩膜就是整张图）某个通道的均值、像素面积与外接框。外接框是像素坐标的 Box2D，可以直接接给 image.crop。
- 目标契约：图像通道统计、像素面积及像素外接框留图像模块，物理面积必须另经标定。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证mask交集、通道均值、像素面积和包围框，禁止把像素面积标m2。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 201 · image.render_model_3d

- 当前：`std-image` / `1.1.0` / 三维模板投影叠图。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/p2_model_projection.cpp:412](../../packs/std-image/ops/p2_model_projection.cpp#L412)。
- 端口：`image:Image`, `model:PointCloud`, `instances:Record`, `calib:Record`, `sceneToCamera:Transform?` → `image:Image`, `mask:Image`, `edge_mask:Image`, `depth:Image`, `normal:Image`, `quality:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgproc及相机畸变/数值求解的geometry模块、LyFlow轻量云/位姿类型；无需PCL。；目标：投影/栅格适配使用图像后端，共享映射/射线数值核，不引点云包注册依赖。
- 当前契约：输入[image:Image, model:PointCloud, instances:Record, calib:Record, sceneToCamera:Transform?] → 输出[image:Image, mask:Image, edge_mask:Image, depth:Image, normal:Image, quality:Record]。按Pose3D及标定投影点云模板，最近相机Z决定遮挡。默认叠图保留位深/通道/alpha；另输出U8可见mask/四邻域实例边界、F32米制相机Z（背景0，mask为有效位）。有非零模型法线时输出F32相机单位XYZ法线；缺法线normal模式报错，普通模式不产normal。所有输出保留scale。不合成点云未覆盖的Mesh表面。
- 目标契约：模板云/CAD视觉投影和多模态栅格生成留图像投影模块，最近相机Z和mask是有效性依据。
- 迁移前置：M1；M1映射组/父索引/观测状态/坐标帧契约
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证遮挡、F32米制Z、单位法线缺失拒绝、源位深/alpha保留。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 202 · image.resize

- 当前：`std-image` / `1.0.0` / 图像缩放。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_basic.cpp:166](../../packs/std-image/ops/image_basic.cpp#L166)。
- 端口：`image:Image` → `image:Image`。
- 参数单位标注：width=px, height=px。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[image:Image] → 输出[image:Image]。按倍数或指定尺寸缩放。缩小默认用区域平均（不出摩尔纹），放大建议线性或三次。
- 目标契约：图像重采样留图像模块，保持显式尺寸/倍数、插值算法及输出scale语义。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证缩小/放大插值、预览scale和整数尺寸舍入。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 203 · image.ring_width

- 当前：`std-image` / `1.0.0` / 圆环周向宽度量测。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_p1_circle.cpp:417](../../packs/std-image/ops/image_p1_circle.cpp#L417)。
- 端口：`image:Image` → `minimum:Measurement`, `maximum:Measurement`, `mean:Measurement`, `quality:Record`。
- 参数单位标注：center=px, radiusMin=px, radiusMax=px, radialStep=px。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[image:Image] → 输出[minimum:Measurement, maximum:Measurement, mean:Measurement, quality:Record]。沿明确径向ROI配对内外边缘，输出周向最小/最大/平均宽度和配对质量；有效比例不足时Measurement.ok=false，不伪造零宽。
- 目标契约：径向配对内外边缘与周向宽度统计留像素卡尺；有效比例不足保留Measurement.ok=false。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证内外边配对、min/max/mean、有效比例及不伪造零宽。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 204 · image.roi_mask

- 当前：`std-image` / `1.0.0` / 几何区域掩膜。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_analysis.cpp:662](../../packs/std-image/ops/image_analysis.cpp#L662)。
- 端口：`image:Image`, `box:Box2D?`, `mask:Image?` → `mask:Image`。
- 参数单位标注：roi=px, center=px, size=px, radius=px, innerRadius=px。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[image:Image, box:Box2D?, mask:Image?] → 输出[mask:Image]。矩形、旋转矩形、圆、圆环弧或多边形生成掩膜，与可选输入掩膜相交，保持图像尺寸；不携带外部隐式 ROI 元数据。
- 目标契约：像素矩形/圆/环弧/多边形光栅化及输入mask相交留图像模块，不把ROI隐藏进图像元数据。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证各形状边界、旋转矩形、多边形、输入mask尺寸和scale。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 205 · image.segmentation_decode

- 当前：`std-image` / `1.0.0` / 语义分割解码。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/p4_segmentation.cpp:858](../../packs/std-image/ops/p4_segmentation.cpp#L858)。
- 端口：`image:Image`, `output:Tensor`, `mapping:Record`, `retainedInstances:Record?`, `retainedMasks:Tensor?`, `retainedMaskInfo:Record?` → `labels:Image`, `masks:Tensor`, `maskInfo:Record`, `instances:Record`, `overlay:Image`, `quality:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgproc的图像缩放、轮廓或光栅化，及公共Tensor/Record/Pose协议；没有ORT会话调用。；目标：图像前后处理模块仅消费标准张量，实际推理经独立std-ml推理能力。
- 当前契约：输入[image:Image, output:Tensor, mapping:Record, retainedInstances:Record?, retainedMasks:Tensor?, retainedMaskInfo:Record?] → 输出[labels:Image, masks:Tensor, maskInfo:Record, instances:Record, overlay:Image, quality:Record]。单float32 NCHW logits/概率→全图u16类别图(65535未知)、重叠NHW二值实例Tensor与同序ID描述、像素Poses2D及只读叠加。概率先双线性回贴再argmax；8连通分量是语义实例支持，不是学习实例分割，也不授权抓取。
- 目标契约：语义概率回贴/连通组件/原生mask与叠画留图像适配器；标准张量和实例协议共享，不依赖ORT节点实现。
- 迁移前置：M1；公共模型映射/批次/掩膜/文本结果schema
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证回贴后argmax、65535未知、重叠NHW绑定、稳定ID、空ROI和非抓取语义。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 206 · image.segmentation_preprocess

- 当前：`std-image` / `1.0.0` / 语义分割预处理。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/p4_segmentation.cpp:825](../../packs/std-image/ops/p4_segmentation.cpp#L825)。
- 端口：`image:Image`, `rois:Record?`, `roiMasks:Tensor?`, `roiMaskInfo:Record?` → `tensor:Tensor`, `mapping:Record`, `retainedInstances:Record`, `retainedMasks:Tensor`, `retainedMaskInfo:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgproc的图像缩放、轮廓或光栅化，及公共Tensor/Record/Pose协议；没有ORT会话调用。；目标：图像前后处理模块仅消费标准张量，实际推理经独立std-ml推理能力。
- 当前契约：输入[image:Image, rois:Record?, roiMasks:Tensor?, roiMaskInfo:Record?] → 输出[tensor:Tensor, mapping:Record, retainedInstances:Record, retainedMasks:Tensor, retainedMaskInfo:Record]。全分辨率u8灰度/RGB/RGBA按轴对齐ROI裁剪并拉伸到NCHW RGB [-1,1]。未连ROI使用整图，已连空ROI保持N=0；最多64ROI。PPHumanSeg固定192×192，ONNX节点使用pphumanseg_2023mar与batchMode=serial。
- 目标契约：图像/实例ROI裁剪到NCHW张量留图像适配器；已连接空ROI与未接整图模式明确区分。
- 迁移前置：M1；公共模型映射/批次/掩膜/文本结果schema
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证u8灰度/RGB/RGBA、64ROI预算、N=0及PPHumanSeg192和归一化。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 207 · image.shading_correct

- 当前：`std-image` / `1.0.0` / 空间光照校正。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_p1_basic.cpp:330](../../packs/std-image/ops/image_p1_basic.cpp#L330)。
- 端口：`image:Image`, `reference:Image?` → `image:Image`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[image:Image, reference:Image?] → 输出[image:Image]。平场除法或背景相减，reference可选；无参考时用奇数高斯核估计背景，epsilon按输入原值度量，非正或过小分母拒绝。
- 目标契约：平场除法/背景相减与可选高斯背景估计留图像模块；输入原值epsilon语义保留。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证参考图匹配、无参考背景、非正分母错误和输出值域。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 208 · image.shape_match

- 当前：`std-image` / `1.0.0` / 梯度形状匹配。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/p3_shape_matching.cpp:916](../../packs/std-image/ops/p3_shape_matching.cpp#L916)。
- 端口：`image:Image`, `features:Tensor`, `model:Record`, `mask:Image?`, `roi:Box2D?`, `region:Record?` → `instances:Record`, `quality:Record`, `image:Image`, `edges:Image`。
- 参数单位标注：smoothSigma=px, spatialStep=px, maxDistance=px, maxRmse=px。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[image:Image, features:Tensor, model:Record, mask:Image?, roi:Box2D?, region:Record?] → 输出[instances:Record, quality:Record, image:Image, edges:Image]。梯度方向中心投票、完整特征定向Chamfer质量门控、遮挡覆盖率、局部姿态精化和旋转IoU NMS；输出原图像素Pose2D与原生叠图。不是NCC。输入须先按需要去畸变。
- 目标契约：梯度Chamfer投票/覆盖率/局部精化留图像定位，像素Pose2D与叠图契约固定。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证遮挡、多角尺度、完整特征质量门控、旋转NMS及先去畸变约定。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 209 · image.shape_template

- 当前：`std-image` / `1.0.0` / 梯度形状建模。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/p3_shape_matching.cpp:894](../../packs/std-image/ops/p3_shape_matching.cpp#L894)。
- 端口：`image:Image`, `mask:Image?`, `roi:Box2D?`, `region:Record?` → `features:Tensor`, `model:Record`, `image:Image`。
- 参数单位标注：smoothSigma=px, featureSpacing=px。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[image:Image, mask:Image?, roi:Box2D?, region:Record?] → 输出[features:Tensor, model:Record, image:Image]。从图像或预先渲染的CAD投影建立梯度形状模型。Tensor储存原图像素特征，Record仅储存有界元数据；重新加载图像可确定性重建。
- 目标契约：梯度形状模型提取留图像定位；大特征Tensor与小Record拆分及CAD预渲染输入边界保持。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证特征Tensor/metadata绑定、mask与ROI、图像重建确定性。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 210 · image.statistics

- 当前：`std-image` / `1.0.0` / 灰度统计。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_analysis.cpp:709](../../packs/std-image/ops/image_analysis.cpp#L709)。
- 端口：`image:Image`, `mask:Image?` → `stats:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[image:Image, mask:Image?] → 输出[stats:Record]。对选定通道与掩膜计算 min/max/mean/median/mode；忽略 NaN/Inf，众数按精确值，次数相同取较小值。
- 目标契约：图像样本统计保留mask、非有限忽略和众数并列取小值；统计结果不自动成为物理Measurement。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证空样本诊断、NaN/Inf、median及mode并列规则。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 211 · image.template_build

- 当前：`std-image` / `1.0.0` / NCC模板构建。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_p1_match.cpp:269](../../packs/std-image/ops/image_p1_match.cpp#L269)。
- 端口：`image:Image` → `template:Image`, `model:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[image:Image] → 输出[template:Image, model:Record]。构建有灰度变化的原生单通道模板图和小型元数据；模板像素不写JSON，可在多次运行中复用。
- 目标契约：NCC灰度模板图与有界元数据归图像定位；模板大数组保持原生Image不进入JSON。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证常量模板拒绝、mask、模板复用和原生像素身份。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 212 · image.template_match

- 当前：`std-image` / `1.0.0` / 多目标NCC模板定位。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_p1_match.cpp:278](../../packs/std-image/ops/image_p1_match.cpp#L278)。
- 端口：`image:Image`, `template:Image`, `model:Record` → `instances:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[image:Image, template:Image, model:Record] → 输出[instances:Record]。掩膜零均值NCC，旋转padding不计入相似度；全图离散角度/尺度搜索、旋转框NMS。无目标返回有效空集合，不做亚像素或轮廓形状匹配。
- 目标契约：离散角度/尺度NCC与旋转IoU NMS留图像定位，输出像素Pose2D，空目标有效。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证旋转padding不计分、mask NCC、稳定多目标NMS和空结果。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 213 · image.threshold

- 当前：`std-image` / `1.0.0` / 图像二值化。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_segment.cpp:203](../../packs/std-image/ops/image_segment.cpp#L203)。
- 端口：`image:Image` → `mask:Image`。
- 参数单位标注：blockSize=px。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[image:Image] → 输出[mask:Image]。单通道图 → u8 掩膜（0 / 255）。固定阈值任何位深都行；Otsu 自动选阈值、自适应按邻域算，这两种只收 u8。
- 目标契约：阈值图像输入输出规则保持单通道到u8 0/255；Otsu/自适应的u8限制保留。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证三种方法、位深拒绝与二值mask集合一致。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 214 · image.to_gray

- 当前：`std-image` / `1.0.0` / 灰度转换。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_basic.cpp:150](../../packs/std-image/ops/image_basic.cpp#L150)。
- 端口：`image:Image` → `image:Image`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[image:Image] → 输出[image:Image]。RGB(A) 转单通道（按 BT.601 加权），alpha 丢掉；已经是灰度的原样传下去。位深不变。
- 目标契约：BT.601亮度变换归图像基础模块，灰度输入透传、alpha丢弃和位深规则显式保留。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证RGB已知色块权重、RGBA的alpha处理、灰度透传和位深。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 215 · image.to_tensor

- 当前：`std-image` / `1.0.0` / 图像转张量。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_tensor.cpp:233](../../packs/std-image/ops/image_tensor.cpp#L233)。
- 端口：`image:Image` → `tensor:Tensor`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前计算：标准C++与LyFlow原生Image/Tensor/PointCloud类型，不调用OpenCV或PCL。；当前构建仍受std-image整包OpenCV链接约束；目标拆成无重依赖的显式能力模块。
- 当前契约：输入[image:Image] → 输出[tensor:Tensor]。推理前的预处理：值 × scale，再按通道 (v − mean) / std，排成指定布局的 float32 张量。u8 图像配 ImageNet 模型的常见写法：scale = 1/255，mean = (0.485, 0.456, 0.406)，std = (0.229, 0.224, 0.225)。尺寸先接 image.resize 调好。
- 目标契约：图像到float32张量适配器保持布局、通道、scale/mean/std显式；实现拆为portable-tensor模块，不要求OpenCV或ORT。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证HWC/NCHW、RGB顺序、各位深以及mean/std无隐式默认替换。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 216 · image.undistort

- 当前：`std-image` / `1.0.0` / 相机图像去畸变。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_p1_calib.cpp:391](../../packs/std-image/ops/image_p1_calib.cpp#L391)。
- 端口：`image:Image`, `calib:Record` → `image:Image`, `valid:Image`, `calib:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgproc/geometry（OpenCV4为calib3d）；标定数据类型由公共协议提供。；目标：相机几何数值核共享，图像特征检测/重采样留OpenCV模块。
- 当前契约：输入[image:Image, calib:Record] → 输出[image:Image, valid:Image, calib:Record]。相机标定必须与全分辨率输入一致；输出相同K、零畸变标定，线性重采样，边界填0。
- 目标契约：K/D图像去畸变留图像模块，输出同K零畸变标定和有效mask，源输入不修改。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证畸变恢复、全分辨率大小匹配、填零边界及有效mask。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 217 · image.warp_perspective

- 当前：`std-image` / `1.0.0` / 平面透视重采样。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_p1_calib.cpp:400](../../packs/std-image/ops/image_p1_calib.cpp#L400)。
- 端口：`image:Image`, `calib:Record` → `image:Image`, `valid:Image`, `mapping:Record`。
- 参数单位标注：width=px, height=px。
- 依赖：当前算法：OpenCV core/imgproc/geometry（OpenCV4为calib3d）；标定数据类型由公共协议提供。；目标：相机几何数值核共享，图像特征检测/重采样留OpenCV模块。
- 当前契约：输入[image:Image, calib:Record] → 输出[image:Image, valid:Image, mapping:Record]。H是原图像素到平面m/mm，输出密度与原点显式；valid仅标记完整插值采样有效像素。
- 目标契约：图像重采样留std-image，H像素到m/mm、输出原点/密度及完整插值有效性显式。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证mm/m等价、输出网格密度、边界valid和非有限H拒绝。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 218 · image.width_range

- 当前：`std-image` / `1.0.0` / 平行卡尺宽度。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_caliper.cpp:446](../../packs/std-image/ops/image_caliper.cpp#L446)。
- 端口：`image:Image` → `minimum:Measurement`, `maximum:Measurement`, `mean:Measurement`, `samples:Record`。
- 参数单位标注：roi=px, band=px, minWidth=px, maxWidth=px。
- 依赖：当前算法：OpenCV core/imgproc；不直接执行ORT，不依赖PCL。；目标：仅所选图像能力模块链接对应OpenCV后端。
- 当前契约：输入[image:Image] → 输出[minimum:Measurement, maximum:Measurement, mean:Measurement, samples:Record]。矩形 ROI 沿 X/Y 扫描，外尺寸选择最外侧开/闭边对，内尺寸选择 ROI 中心两侧最近边对，输出采样宽度 min/max/mean。不是任意旋转形状的全局最窄宽度。
- 目标契约：像素扫描的内外边对与min/max/mean保留；物理宽度计算另接显式标定，不伪称全局最窄宽度。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证内外尺寸边对、ROI中心规则、少边无目标和px输出。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 219 · image.yolo_decode

- 当前：`std-image` / `1.0.0` / YOLO 检测结果解码。目标：`std-ml`；迁移归属；**M6**。
- 源码：[packs/std-image/ops/image_yolo.cpp:374](../../packs/std-image/ops/image_yolo.cpp#L374)。
- 端口：`prediction:Tensor`, `mapping:Record` → `detections:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前计算：标准C++张量/Record处理；与预处理共享含OpenCV的编译单元或协议头，没有ORT执行调用。；目标：纯张量适配模块只依赖公共模型协议/类型，不要求ORT、OpenCV或std-image注册。
- 当前契约：输入[prediction:Tensor, mapping:Record] → 输出[detections:Record]。8/11及26常规头：[1,4+C,N]最高类别概率，类别感知稳定IoU NMS；26默认或内置NMS导出：[1,N,6]，只筛选/回映，不重复NMS。支持自定义类别和原图ROI/预览坐标还原。
- 目标契约：YOLO输出布局/NMS/坐标回映的纯张量适配迁std-ml；协议头独立于图像包，26内置NMS输出不再二次NMS。
- 迁移前置：M1；公共YOLO/OCR预处理映射与结果schema从图像实现头提取
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：分别验证[1,4+C,N]和[1,N,6]、类别表、score与稳定NMS、原图ROI坐标。
- 说明：算子ID的image前缀不决定目标包；Image输入预处理留std-image，纯张量模型解码在std-ml，ORT会话仅由推理模块提供。

## 220 · image.yolo_preprocess

- 当前：`std-image` / `1.0.0` / YOLO 图像预处理。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_yolo.cpp:343](../../packs/std-image/ops/image_yolo.cpp#L343)。
- 端口：`image:Image`, `roi:Box2D?` → `tensor:Tensor`, `mapping:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgproc的图像缩放、轮廓或光栅化，及公共Tensor/Record/Pose协议；没有ORT会话调用。；目标：图像前后处理模块仅消费标准张量，实际推理经独立std-ml推理能力。
- 当前契约：输入[image:Image, roi:Box2D?] → 输出[tensor:Tensor, mapping:Record]。支持YOLOv8/11/26标准float32检测ONNX：RGB/255，居中letterbox、填充114，批量固定1；类别列表与输入宽高显式，不猜输出布局。
- 目标契约：YOLO8/11/26的Image到Tensor留图像模型适配器；RGB/255、居中letterbox和显式类别/尺寸契约固定。
- 迁移前置：M1；公共模型映射/批次/掩膜/文本结果schema
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证8/11/26输入配置、奇数padding舍入、ROI/预览坐标及RGB归一化。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 221 · image.yolox_decode

- 当前：`std-image` / `1.0.0` / YOLOX 检测结果解码。目标：`std-ml`；迁移归属；**M6**。
- 源码：[packs/std-image/ops/image_detection.cpp:284](../../packs/std-image/ops/image_detection.cpp#L284)。
- 端口：`prediction:Tensor`, `mapping:Record` → `detections:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前计算：标准C++张量/Record处理；与预处理共享含OpenCV的编译单元或协议头，没有ORT执行调用。；目标：纯张量适配模块只依赖公共模型协议/类型，不要求ORT、OpenCV或std-image注册。
- 当前契约：输入[prediction:Tensor, mapping:Record] → 输出[detections:Record]。固定未解码[1,3549,85]输出，每锚点最高分类；网格/步长解码、分数筛选、稳定候选截断、类别感知NMS、原图像素坐标回映。空目标是有效空列表，无实例分割或旋转框。
- 目标契约：纯张量YOLOX解码迁std-ml的可选模型适配模块；共享预处理Record协议，不链接std-image/OpenCV/ORT计算后端。
- 迁移前置：M1；公共YOLO/OCR预处理映射与结果schema从图像实现头提取
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证[1,3549,85]、网格/步长、稳定候选截断、类别NMS和空目标。
- 说明：算子ID的image前缀不决定目标包；Image输入预处理留std-image，纯张量模型解码在std-ml，ORT会话仅由推理模块提供。

## 222 · image.yolox_preprocess

- 当前：`std-image` / `1.0.0` / YOLOX 图像预处理。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_detection.cpp:267](../../packs/std-image/ops/image_detection.cpp#L267)。
- 端口：`image:Image`, `roi:Box2D?` → `tensor:Tensor`, `mapping:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgproc的图像缩放、轮廓或光栅化，及公共Tensor/Record/Pose协议；没有ORT会话调用。；目标：图像前后处理模块仅消费标准张量，实际推理经独立std-ml推理能力。
- 当前契约：输入[image:Image, roi:Box2D?] → 输出[tensor:Tensor, mapping:Record]。固定 YOLOX-Nano 0.1.1rc0：416×416、BGR、float32 NCHW、0..255，右/下填充114；保留实际缩放和ROI供坐标回映。
- 目标契约：YOLOX图像适配器留std-image：固定BGR/416/右下padding和逆映射；元数据schema移到公共模型协议。
- 迁移前置：M1；公共模型映射/批次/掩膜/文本结果schema
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证416输入、BGR与0..255、ROI边缘以及decode原图回映。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 223 · io.load_image

- 当前：`std-image` / `1.0.0` / 加载图像。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/io_image.cpp:153](../../packs/std-image/ops/io_image.cpp#L153)。
- 端口：`image:Image?` → `image:Image`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgcodecs及其图像编解码传递库。；目标：image-io显式能力，按支持格式交付许可证。
- 当前契约：输入[image:Image?] → 输出[image:Image]。从磁盘读取图像（PNG / JPEG / BMP / TIFF）。通道顺序转成 RGB(A)；16 位 PNG / TIFF 保留 u16。 source=inputs 时使用宿主经 RunImageInput 注入或上游连接的 image，不读文件。
- 目标契约：RGB/RGBA与灰度、u8/u16/f32及宿主注入模式由图像IO适配器负责；显式source=inputs禁止读盘。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证PNG/TIFF位深及RGB顺序、宿主输入绕过文件、损坏文件诊断。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 224 · io.save_image

- 当前：`std-image` / `1.0.0` / 保存图像。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/io_image.cpp:204](../../packs/std-image/ops/io_image.cpp#L204)。
- 端口：`image:Image` → 无。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前算法：OpenCV core/imgcodecs及其图像编解码传递库。；目标：image-io显式能力，按支持格式交付许可证。
- 当前契约：输入[image:Image] → 输出[无]。把图像写到磁盘，按扩展名选格式。u8 / u16 存 PNG；f32 只能存 TIFF。
- 目标契约：图像格式编码留在图像IO模块；保留u8/u16 PNG与f32 TIFF限制及写盘副作用标记。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证位深往返、格式不支持报错和输出路径错误不伪造成功。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 225 · pose.project_3d

- 当前：`std-image` / `1.0.0` / 三维有限工件原图掩膜投影。目标：`std-image`；拆公共能力/适配；**M6**。
- 源码：[packs/std-image/ops/p5_pose_projection.cpp:1027](../../packs/std-image/ops/p5_pose_projection.cpp#L1027)。
- 端口：`poses:Record`, `calib:Record`, `posesToCamera:Transform?`, `rois:Record?` → `poses:Record`, `masks:Tensor`, `maskInfo:Record`, `quality:Record`。
- 参数单位标注：nearDepth=m。
- 依赖：当前算法：OpenCV core/imgproc及相机畸变/数值求解的geometry模块、LyFlow轻量云/位姿类型；无需PCL。；目标：投影/栅格适配使用图像后端，共享映射/射线数值核，不引点云包注册依赖。
- 当前契约：输入[poses:Record, calib:Record, posesToCamera:Transform?, rois:Record?] → 输出[poses:Record, masks:Tensor, maskInfo:Record, quality:Record]。米制Pose3D集合+相机K/D+可选posesToCamera，输出像素Pose2D、NHW mask Tensor及maskInfo/quality；有full/top、遮挡/合并与实体锚点选项。
- 目标契约：带K/D像素掩膜投影留std-image，轻量std-geometry仅提供刚体/实体数学与标准Pose；两者单向依赖。
- 迁移前置：M1；M1映射组/父索引/观测状态/坐标帧契约
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证有限体/圆盘轮廓、full/top、independent/nearest、NHW绑定和米/px分离。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 226 · pose.project_to_plane

- 当前：`std-image` / `1.0.0` / 二维实例任意平面反投影及实体尺寸。目标：`std-image`；拆公共能力/适配；**M6**。
- 源码：[packs/std-image/ops/p5_pose_projection.cpp:1043](../../packs/std-image/ops/p5_pose_projection.cpp#L1043)。
- 端口：`poses:Record`, `calib:Record`, `plane:Plane?`, `planes:Record?`, `workspace:Record?`, `planeToCamera:Transform?`, `masks:Tensor?`, `maskInfo:Record?` → `poses:Record`, `quality:Record`。
- 参数单位标注：thickness=m, nearDepth=m。
- 依赖：当前算法：OpenCV core/imgproc及相机畸变/数值求解的geometry模块、LyFlow轻量云/位姿类型；无需PCL。；目标：投影/栅格适配使用图像后端，共享映射/射线数值核，不引点云包注册依赖。
- 当前契约：输入[poses:Record, calib:Record, plane:Plane?, planes:Record?, workspace:Record?, planeToCamera:Transform?, masks:Tensor?, maskInfo:Record?] → 输出[poses:Record, quality:Record]。像素Pose2D+K/D及显式平面/工作空间和跨帧Transform，可选mask；逆投影输出米制Pose3D及质量，center用已知size3D，dimensions按可见几何求尺寸。
- 目标契约：K/D逆射线与任意平面相交的视觉适配留std-image；共享几何数值核，实体尺寸不由未知深度猜测。
- 迁移前置：M1；M1映射组/父索引/观测状态/坐标帧契约
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证跨帧外参、平行射线、workspace顶底面、中心/尺寸模式与实际掩膜支持。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 227 · tensor.to_image

- 当前：`std-image` / `1.0.0` / 张量转图像。目标：`std-image`；保留并验边界；**M6**。
- 源码：[packs/std-image/ops/image_tensor.cpp:282](../../packs/std-image/ops/image_tensor.cpp#L282)。
- 端口：`tensor:Tensor` → `image:Image`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前计算：标准C++与LyFlow原生Image/Tensor/PointCloud类型，不调用OpenCV或PCL。；当前构建仍受std-image整包OpenCV链接约束；目标拆成无重依赖的显式能力模块。
- 当前契约：输入[tensor:Tensor] → 输出[image:Image]。把张量（分割掩膜、热力图、生成的图）读成图像。批里取一张；u8 时按这一张的范围拉到 0..255，f32 原值保留。通道要是 1 / 3 / 4。
- 目标契约：张量到图像适配器保持批次选择、1/3/4通道、u8拉伸与f32原值；portable-tensor不要求推理运行库。
- 迁移前置：M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证批次边界、通道拒绝、常量张量与f32不重归一化。
- 说明：当前std-image整包还传递链接yaml-cpp、条码和Marker资源；本项算法依赖与整包链接负担分开记录。

## 228 · workspace.load

- 当前：`std-image` / `1.0.0` / 三维工作空间加载。目标：`std-geometry`；迁移归属；**M6**。
- 源码：[packs/std-image/ops/p3_workspace.cpp:591](../../packs/std-image/ops/p3_workspace.cpp#L591)。
- 端口：`overrideRegion:Record?` → `region:Record`, `fromWorkspace:Transform`, `toWorkspace:Transform`, `workspace:Record`。
- 参数单位标注：maxTranslationError=m。
- 依赖：当前算法：yaml-cpp（含JSON/YAML文件读取）+标准C++与std-geometry私有pose_common头，无OpenCV/PCL调用。；目标：公共几何数据协议；workspace-io可选yaml-cpp，默认轻量几何模块不链接文件适配依赖。
- 当前契约：输入[overrideRegion:Record?] → 输出[region:Record, fromWorkspace:Transform, toWorkspace:Transform, workspace:Record]。加载显式版本帧树或显式选择的无版本外部workspace YAML，输出可消费体积Pose和双向Transform。底面锚点转换为标准体积中心；拒绝缺父帧、环、非刚体、未知协议及原生精度超限。每次读文件，无全局树副作用。
- 目标契约：工作空间/帧树文件加载迁std-geometry的显式workspace-io模块；JSON核心不需OpenCV，外部YAML适配可选yaml-cpp。
- 迁移前置：M1；几何schema与帧树验证器成为公共接口；M0显式能力与依赖声明
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证树环/父缺失/非刚体、底面到中心、单位声明、float32精度和每次文件重读。
- 说明：从图像包移出真实的非图像能力；每次文件重读/关闭结果缓存必须保留，禁止变成隐式全局帧树。
