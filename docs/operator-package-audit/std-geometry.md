# std-geometry 逐算子迁移清单

基线 `9940c8c`；共 35 项。状态：规划，未实施。返回[清单总览](../operator-package-audit.md)。

下列顺序是本计划内的迁移/验收顺序，不是执行图调度顺序。`?` 表示可选输入；没有`?`的输入按当前manifest默认为必需。精确参数、默认值、端口Record契约及来源指纹见[机器清单](../operator-package-audit.json)。

## 020 · geometry.circle_2d

- 当前：`std-geometry` / `1.0.0` / 二维圆构造。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/geometry.cpp:706](../../packs/std-geometry/ops/geometry.cpp#L706)。
- 端口：`x:Record?`, `y:Record?`, `radius:Record?` → `circle:Circle2D`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：有限圆心和正半径，单位m/px，动态值单位一致
- 目标契约：有限圆心和正半径，单位m/px，动态值单位一致；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：非正半径和混单位拒绝；后续几何运算不丢单位
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 021 · geometry.distance_2d

- 当前：`std-geometry` / `1.0.0` / 二维几何距离。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/geometry.cpp:771](../../packs/std-geometry/ops/geometry.cpp#L771)。
- 端口：`a:Any`, `b:Any` → `distance:Measurement`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：同单位px/m基本几何距离；minimum非负，signed_clearance及first_anchor有独立定义
- 目标契约：同单位px/m基本几何距离；minimum非负，signed_clearance及first_anchor有独立定义；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：点线圆/有限线段退化和非对称anchor回归；不改成业务gap定义
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 022 · geometry.extend_2d

- 当前：`std-geometry` / `1.0.0` / 二维几何与工件嵌入三维平面。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/p5_pose_geometry.cpp:718](../../packs/std-geometry/ops/p5_pose_geometry.cpp#L718)。
- 端口：`poses:Record?`, `features:Record?`, `point:Point2D?`, `line:Line2D?`, `circle:Circle2D?`, `transform:Transform?` → `poses:Record`, `features:Record`。
- 参数单位标注：offset=m, thickness=m。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：二维几何/工件嵌入明确三维平面；px显式pixelToMeters，offset/thickness为m
- 目标契约：二维几何/工件嵌入明确三维平面；px显式pixelToMeters，offset/thickness为m；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：无深度信息不恢复3D；新轴/单位/变换保留几何含义
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 023 · geometry.intersect_2d

- 当前：`std-geometry` / `1.0.0` / 二维几何求交。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/geometry.cpp:820](../../packs/std-geometry/ops/geometry.cpp#L820)。
- 端口：`a:Any`, `b:Any` → `intersections:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：同单位px/m求交，有限点按x/y排序；无交为空，重合标coincident
- 目标契约：同单位px/m求交，有限点按x/y排序；无交为空，重合标coincident；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：相切、平行、连续重合与数值极端回归；不伪造交点
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 024 · geometry.line_2d

- 当前：`std-geometry` / `1.0.0` / 二维直线与线段构造。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/geometry.cpp:637](../../packs/std-geometry/ops/geometry.cpp#L637)。
- 端口：`x:Record?`, `y:Record?`, `angle:Record?` → `line:Line2D`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：无限线或有限线段，方向归一化；角度显式deg/rad；长度按m/px
- 目标契约：无限线或有限线段，方向归一化；角度显式deg/rad；长度按m/px；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：零方向、重合端点拒绝；端点与角度两种模式不混用
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 025 · geometry.midpoint_2d

- 当前：`std-geometry` / `1.0.0` / 二维点中点。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/geometry.cpp:857](../../packs/std-geometry/ops/geometry.cpp#L857)。
- 端口：`a:Point2D`, `b:Point2D` → `point:Point2D`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：同单位二维点中点，保留单位
- 目标契约：同单位二维点中点，保留单位；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：混单位拒绝；大数坐标有限且对称
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 026 · geometry.plane

- 当前：`std-geometry` / `1.0.0` / 三维平面构造。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/geometry.cpp:735](../../packs/std-geometry/ops/geometry.cpp#L735)。
- 端口：无 → `plane:Plane`。
- 参数单位标注：offset=m。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：n·p+d=0，d为m；法向及d同时除以法向模长
- 目标契约：n·p+d=0，d为m；法向及d同时除以法向模长；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：缩放输入系数得到相同平面；零法向拒绝
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 027 · geometry.point_2d

- 当前：`std-geometry` / `1.0.0` / 二维点构造。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/geometry.cpp:618](../../packs/std-geometry/ops/geometry.cpp#L618)。
- 端口：`x:Record?`, `y:Record?` → `point:Point2D`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：有限二维点，单位m或px；动态数值须匹配该单位
- 目标契约：有限二维点，单位m或px；动态数值须匹配该单位；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：非有限和单位不符拒绝；构造/分解一致
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 028 · geometry.point_at

- 当前：`std-geometry` / `1.0.0` / 交点索引提取。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/geometry.cpp:914](../../packs/std-geometry/ops/geometry.cpp#L914)。
- 端口：`intersections:Record` → `point:Point2D`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：从受校验交点Record按零起点索引取Point2D
- 目标契约：从受校验交点Record按零起点索引取Point2D；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：空、coincident及越界诊断；有限点单位不变
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 029 · geometry.project_point_2d

- 当前：`std-geometry` / `1.0.0` / 二维点正交投影。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/geometry.cpp:877](../../packs/std-geometry/ops/geometry.cpp#L877)。
- 端口：`point:Point2D`, `line:Line2D` → `point:Point2D`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：点在线/线段的正交投影，线段默认钳到端点，可显式延长
- 目标契约：点在线/线段的正交投影，线段默认钳到端点，可显式延长；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：端外投影在两种clamp模式符合契约；保留px/m
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 030 · geometry.transform_2d

- 当前：`std-geometry` / `1.0.0` / 二维几何相似变换。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/geometry_transform.cpp:126](../../packs/std-geometry/ops/geometry_transform.cpp#L126)。
- 端口：`geometry:Any`, `transform:Transform` → `geometry:Any`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：Point2D/Line2D/Circle2D同单位px/m相似变换；线段端点与圆半径同步更新
- 目标契约：Point2D/Line2D/Circle2D同单位px/m相似变换；线段端点与圆半径同步更新；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：圆拒绝非相似仿射；方向无平移，原类型与单位保留
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 031 · geometry.transform_plane

- 当前：`std-geometry` / `1.0.0` / 三维平面仿射变换。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/geometry_transform.cpp:140](../../packs/std-geometry/ops/geometry_transform.cpp#L140)。
- 端口：`plane:Plane`, `transform:Transform` → `plane:Plane`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：米制Plane在有限非奇异仿射下用逆转置变换，法向与d重归一化
- 目标契约：米制Plane在有限非奇异仿射下用逆转置变换，法向与d重归一化；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：非均匀缩放、平移与近奇异矩阵回归；不附带外部帧树协议
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 032 · pose.compare_2d

- 当前：`std-geometry` / `1.0.0` / 二维工件位姿比较。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/pose_ops.cpp:701](../../packs/std-geometry/ops/pose_ops.cpp#L701)。
- 端口：`a:Record`, `b:Record` → `distance:Record`, `angle:Record`, `matched:Record`, `difference:Record`。
- 参数单位标注：symmetryPeriod=rad, angleTolerance=rad。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：同帧同单位二维工件，显式旋转等价周期和位置/角容差
- 目标契约：同帧同单位二维工件，显式旋转等价周期和位置/角容差；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：临界容差及跨周期角差；纯几何matched不充当生产OK/NG
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 033 · pose.compare_3d

- 当前：`std-geometry` / `1.0.0` / 三维跨帧位姿对称比较。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/p5_pose_geometry.cpp:683](../../packs/std-geometry/ops/p5_pose_geometry.cpp#L683)。
- 端口：`a:Record`, `b:Record`, `bToA:Transform?` → `result:Record`, `matched:Record`。
- 参数单位标注：angleTolerance=rad。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：显式bToA跨帧对齐后按m/rad容差及声明对称比较；alarm为诊断策略
- 目标契约：显式bToA跨帧对齐后按m/rad容差及声明对称比较；alarm为诊断策略；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：跨帧非交换链、对称等价及临界容差；不推断工件可抓或生产许可
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 034 · pose.concat

- 当前：`std-geometry` / `1.0.0` / 工件实例集合拼接。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/pose_ops.cpp:599](../../packs/std-geometry/ops/pose_ops.cpp#L599)。
- 端口：`a:Record`, `b:Record` → `poses:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：同维度/帧/单位的单体或集合按输入序拼接，拒绝重复ID
- 目标契约：同维度/帧/单位的单体或集合按输入序拼接，拒绝重复ID；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：混帧、重复ID拒绝；合法空集合和顺序保持
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 035 · pose.construct_2d

- 当前：`std-geometry` / `1.0.0` / 二维工件位姿构造。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/pose_ops.cpp:567](../../packs/std-geometry/ops/pose_ops.cpp#L567)。
- 端口：`x:Record?`, `y:Record?`, `angle:Record?` → `pose:Record`。
- 参数单位标注：angle=rad。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：二维工件Record，位置/尺寸为显式px或m，角度rad，动态std.Value须同单位
- 目标契约：二维工件Record，位置/尺寸为显式px或m，角度rad，动态std.Value须同单位；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：拒绝混单位、非正尺寸和非有限输入；身份与显式frame保留
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 036 · pose.construct_3d

- 当前：`std-geometry` / `1.0.0` / 三维工件位姿构造。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/pose_ops.cpp:580](../../packs/std-geometry/ops/pose_ops.cpp#L580)。
- 端口：`x:Record?`, `y:Record?`, `z:Record?`, `rx:Record?`, `ry:Record?`, `rz:Record?` → `pose:Record`。
- 参数单位标注：rx=rad, ry=rad, rz=rad。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：三维工件Record，位置/尺寸m；xyzw单位四元数或内禀XYZ弧度，R=RxRyRz
- 目标契约：三维工件Record，位置/尺寸m；xyzw单位四元数或内禀XYZ弧度，R=RxRyRz；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：q与-q规范化、零四元数拒绝；动态m/rad与常量一致
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 037 · pose.decompose_2d

- 当前：`std-geometry` / `1.0.0` / 二维工件信息分解。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/pose_ops.cpp:693](../../packs/std-geometry/ops/pose_ops.cpp#L693)。
- 端口：`poses:Record` → `x:Record`, `y:Record`, `angle:Record`, `width:Record`, `height:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：从选定二维工件分解位置、rad角、宽高到带单位std.Value
- 目标契约：从选定二维工件分解位置、rad角、宽高到带单位std.Value；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：输出单位继承px/m；索引越界失败；与construct往返
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 038 · pose.filter

- 当前：`std-geometry` / `1.0.0` / 工件实例条件筛选。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/pose_ops.cpp:613](../../packs/std-geometry/ops/pose_ops.cpp#L613)。
- 端口：`poses:Record` → `matched:Record`, `rejected:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：保持输入序，缺失可选字段不匹配；distance按已接受项贪心，area为单位平方
- 目标契约：保持输入序，缺失可选字段不匹配；distance按已接受项贪心，area为单位平方；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：相同项重排时贪心结果符合旧契约；matched/rejected互补；不赋予抓取许可
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 039 · pose.flip_axis_2d

- 当前：`std-geometry` / `1.0.0` / 二维工件对称轴规范化。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/pose_ops.cpp:670](../../packs/std-geometry/ops/pose_ops.cpp#L670)。
- 端口：`poses:Record` → `poses:Record`。
- 参数单位标注：targetAngle=rad。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：仅按用户显式声明n重旋转对称选择等价朝向，angle为rad
- 目标契约：仅按用户显式声明n重旋转对称选择等价朝向，angle为rad；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：不自动推断矩形/物体对称；目标角和边界相位一致
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 040 · pose.flip_axis_3d

- 当前：`std-geometry` / `1.0.0` / 三维有限旋转对称轴优化。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/p5_pose_geometry.cpp:661](../../packs/std-geometry/ops/p5_pose_geometry.cpp#L661)。
- 端口：`poses:Record` → `poses:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：米制工件在显式有限旋转对称群内选等价朝向
- 目标契约：米制工件在显式有限旋转对称群内选等价朝向；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：q/-q、有限枚举上限与非法轴回归；身份和位置不变
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 041 · pose.frame_from_points

- 当前：`std-geometry` / `1.0.0` / 定位点右手坐标系构造。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/pose_ops.cpp:681](../../packs/std-geometry/ops/pose_ops.cpp#L681)。
- 端口：`a:Record`, `b:Record`, `c:Record?` → `poses:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：同帧m位置按序配对构建右手架；三点优先，二点用up正交化
- 目标契约：同帧m位置按序配对构建右手架；三点优先，二点用up正交化；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：重合/共线/不等长失败；X/Y/Z正交且det为+1
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 042 · pose.from_text

- 当前：`std-geometry` / `1.0.0` / 工件位姿JSON解析。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/pose_ops.cpp:720](../../packs/std-geometry/ops/pose_ops.cpp#L720)。
- 端口：`text:Record` → `poses:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：std.Value字符串解码为受校验的二维/三维工件集合
- 目标契约：std.Value字符串解码为受校验的二维/三维工件集合；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：拒绝未知type、重复ID和混单位；与to_text往返
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 043 · pose.get

- 当前：`std-geometry` / `1.0.0` / 工件实例索引提取。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/pose_ops.cpp:606](../../packs/std-geometry/ops/pose_ops.cpp#L606)。
- 端口：`poses:Record` → `pose:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：对工件集合用零起点非负索引提取，保留原工件身份
- 目标契约：对工件集合用零起点非负索引提取，保留原工件身份；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：空集合和越界明确失败；不生成新身份
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 044 · pose.offset_3d

- 当前：`std-geometry` / `1.0.0` / 三维工件位置偏移。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/pose_ops.cpp:660](../../packs/std-geometry/ops/pose_ops.cpp#L660)。
- 端口：`poses:Record`, `x:Record?`, `y:Record?`, `z:Record?` → `poses:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：m偏移，local先乘工件旋转再加位置，world直接相加
- 目标契约：m偏移，local先乘工件旋转再加位置，world直接相加；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：local/world在旋转工件上产生预期不同结果；不改姿态与frame
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 045 · pose.rigid_check

- 当前：`std-geometry` / `1.0.0` / 刚体多点距离与配准残差检查。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/p5_pose_geometry.cpp:703](../../packs/std-geometry/ops/p5_pose_geometry.cpp#L703)。
- 端口：`poses:Record`, `template:Record` → `result:Record`, `accepted:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：按多点间距离或配准残差检验刚体关系；距离模式不能确定绝对朝向
- 目标契约：按多点间距离或配准残差检验刚体关系；距离模式不能确定绝对朝向；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：镜像与共线案例不能误报可观姿态；正常空配对不造工件
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 046 · pose.rigid_register

- 当前：`std-geometry` / `1.0.0` / 刚体定位点可观测配准。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/p5_pose_geometry.cpp:711](../../packs/std-geometry/ops/p5_pose_geometry.cpp#L711)。
- 端口：`poses:Record`, `template:Record` → `result:Record`, `accepted:Record`, `transform:Transform`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：小型m位置集合可观测刚体配准，输出Transform/残差/accepted
- 目标契约：小型m位置集合可观测刚体配准，输出Transform/残差/accepted；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：共线和非唯一旋转拒绝；误差阈值在float32交付后仍成立
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 047 · pose.rigid_template

- 当前：`std-geometry` / `1.0.0` / 刚体定位点模板记录。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/p5_pose_geometry.cpp:697](../../packs/std-geometry/ops/p5_pose_geometry.cpp#L697)。
- 端口：`poses:Record` → `template:Record`。
- 参数单位标注：tolerance=m。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：m位置集合记录刚体模板及显式容差，含身份与帧
- 目标契约：m位置集合记录刚体模板及显式容差，含身份与帧；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：重复点/退化/超限拒绝；模板序列化及ID一致
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 048 · pose.rotate_axis_3d

- 当前：`std-geometry` / `1.0.0` / 三维连续旋转对称轴优化。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/p5_pose_geometry.cpp:671](../../packs/std-geometry/ops/p5_pose_geometry.cpp#L671)。
- 端口：`poses:Record` → `poses:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：仅声明连续对称时绕固定轴优化另一轴朝向，角度rad
- 目标契约：仅声明连续对称时绕固定轴优化另一轴朝向，角度rad；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：未声明对称拒绝；固定轴约束和退化方向回归
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 049 · pose.to_text

- 当前：`std-geometry` / `1.0.0` / 工件位姿JSON序列化。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/pose_ops.cpp:712](../../packs/std-geometry/ops/pose_ops.cpp#L712)。
- 端口：`poses:Record` → `text:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：完整type/data、身份、frame、unit与属性JSON往返；64KiB上限
- 目标契约：完整type/data、身份、frame、unit与属性JSON往返；64KiB上限；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：Unicode/浮点往返；超过文本上限明确失败而非截断
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 050 · pose.to_transform

- 当前：`std-geometry` / `1.0.0` / 工件位姿米制矩阵提取。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/pose_ops.cpp:727](../../packs/std-geometry/ops/pose_ops.cpp#L727)。
- 端口：`poses:Record` → `transform:Transform`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：从选定工件提取米制Transform；像素工件须先物理映射
- 目标契约：从选定工件提取米制Transform；像素工件须先物理映射；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：拒绝px直接转米制矩阵、非法索引；与pose位置/方向一致
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 051 · pose.to_world_plane

- 当前：`std-geometry` / `1.0.0` / 二维工件已知平面位姿映射。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/pose_ops.cpp:736](../../packs/std-geometry/ops/pose_ops.cpp#L736)。
- 端口：`poses:Record`, `calib:Record`, `planeToWorld:Transform` → `poses:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：已知平面H及planeToWorld将像素位置/局部雅可比朝向映到m；size3D显式给定
- 目标契约：已知平面H及planeToWorld将像素位置/局部雅可比朝向映到m；size3D显式给定；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：标定mm/m一致；不把像素框猜成物理尺寸，不恢复未知深度
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID；只消费公开PlaneCalib Record并计算H，无OpenCV/YAML依赖；标定记录schema共享，不依赖std-image私有头

## 052 · pose.transform_2d

- 当前：`std-geometry` / `1.0.0` / 二维工件位姿变换。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/pose_ops.cpp:642](../../packs/std-geometry/ops/pose_ops.cpp#L642)。
- 端口：`poses:Record`, `transform:Transform` → `poses:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：source→target左乘二维无镜像相似变换，平移单位显式px/m，尺寸同步缩放
- 目标契约：source→target左乘二维无镜像相似变换，平移单位显式px/m，尺寸同步缩放；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：平移/旋转非交换链、混单位与镜像拒绝；不改变属性
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 053 · pose.transform_3d

- 当前：`std-geometry` / `1.0.0` / 三维工件位姿变换。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/pose_ops.cpp:642](../../packs/std-geometry/ops/pose_ops.cpp#L642)。
- 端口：`poses:Record`, `transform:Transform` → `poses:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：米制source→target右手刚体左乘，拒绝非刚体和错帧
- 目标契约：米制source→target右手刚体左乘，拒绝非刚体和错帧；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：非交换链、反射/缩放拒绝；保持身份与尺寸
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 054 · pose.update

- 当前：`std-geometry` / `1.0.0` / 工件实例属性更新。目标：`std-geometry`；保留并验边界；**M2**。
- 源码：[packs/std-geometry/ops/pose_ops.cpp:632](../../packs/std-geometry/ops/pose_ops.cpp#L632)。
- 端口：`poses:Record` → `poses:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：仅更新白名单属性；不更改ID、frame和pose；attributes整体替换
- 目标契约：仅更新白名单属性；不更改ID、frame和pose；attributes整体替换；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0；M1
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：非法补丁在空集合仍失败；同对象位置/身份保持
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID
