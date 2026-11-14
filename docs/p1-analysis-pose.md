# P1 工件位姿与几何变换逐项分析

[补齐计划](operator-completion-plan.md) · [能力对照](operator-coverage.md) · [算子契约](../packs/std-geometry/README.md)

本页核实用户本机 xyz-vision-lib 的 20 个相关条目：14 个有对应 C++ 实现，6 个只有 `property_settings.yml` 中的脚本属性声明。参考目录没有 Git 元数据，不能确认清单中的指定修订；未找到的脚本实现不推测为已阅读。LyFlow 只根据可核实语义独立实现，不复制其 proprietary 源码、Qt 节点接口或全局 FrameTree 协议。

## 逐项取舍

路径以外部库 `src/xyz_vision_lib/models/` 为根。所有 LyFlow 工件节点位于 [pose_ops.cpp](../packs/std-geometry/ops/pose_ops.cpp)，几何变换位于 [geometry_transform.cpp](../packs/std-geometry/ops/geometry_transform.cpp)，共享契约位于 [pose.h](../core/include/lyflow/pose.h)，永久回归位于 [test_pose.cpp](../packs/std-geometry/tests/test_pose.cpp)。

| 外部条目 / 参考文件 | 核实的行为与问题 | LyFlow 实现和边界 |
|---|---|---|
| InputPrimitive2D / `Input/InputPrimitive2D.cpp` | 从名称、分数、中心、宽高和角度生成单元素 Primitive2D 集合；未在该节点内明确拒绝零/负尺寸和无效分数。 | `pose.construct_2d` 明确中心位置、弧度角、px/m、稳定 ID、参考帧和正尺寸；置信度严格为[0,1]，默认 pickable=false。 |
| InputPrimitive3D / `Primitive_Processing/InputPrimitive3D.cpp` | 米制平移、度制 RxRyRz 或 xyzw 四元数、frame 和尺寸；四元数直接归一化。 | `pose.construct_3d` 对零/非有限四元数报错，使用缩放归一化避免大/小数溢出；规范 q/-q 符号；内禀XYZ使用弧度并固定 R=RxRyRz。 |
| ConcatPrimitives / `Primitive_Processing/ConcatPrimitives.cpp` | 将两组 Primitive3D 依次插入同一数组。 | `pose.concat` 同时支持2D/3D的同维集合；保留顺序，拒绝重复 ID、混单位和混参考帧；不自动改名或静默去重。 |
| FlipPrimitiveAxis2D / `Primitive_Processing/FlipPrimitiveAxis2D.cpp` | 按 n 重对称候选旋转，选最靠近给定正/负坐标轴的方向；候选用 acos 排序，浮点点积存在定义域边界。 | `pose.flip_axis_2d` 显式声明对称阶数与目标弧度方向，以周期余数选择等价朝向，不通过 acos，也不推断物体天然具有某种对称性。 |
| FrameFromPoints / `Primitive_Processing/FrameFromPoints.cpp` | 两点用中点和 a→b，直接把全局Z与X组合；三点分支排在“两端口连接”判断之后而不可达，且共线只记录告警。 | `pose.frame_from_points` 连接 c 优先三点模式；两点将 up 正交化，三点使用 X=a→b、Z=(a→b)×(a→c)；重合/近共线/平行 up 返回错误；原点可选首点或质心。 |
| GeometryTransformation / `Primitive_Processing/GeometryTransformation.cpp` | 通过全局 FrameTree 取变换并更新几何坐标；对 direction 也应用带平移的 Isometry。 | `geometry.transform_2d` 保持原生点/线/圆类型，方向只乘线性部分；相似变换同步线段和半径，拒绝把圆默默变成椭圆。`geometry.transform_plane` 用逆转置并更新 d。外部通用三维 GeometryVector 及FrameTree协议仍未实现。 |
| ObjectTransformation2d / `2D_Processing/ObjectTransformation2d.cpp` | 目前只接受 n_points_2d，加载相机/机器人链并将中心和一个朝向偏移点映到平面；在线查询缺失时部分路径回落单位阵。 | `pose.to_world_plane` 接受已校验的 `image.PlaneCalib` 和显式 plane→world 刚体；位置由H、方向由局部雅可比映射。必须明确已知物理 size3D，默认零值会报配置错误，不从透视像素框猜尺寸；不回退未知外参，不求任意深度。 |
| PrimitiveFilter / `Primitive_Processing/PrimitiveFilter.cpp` | 包含分数、面积、顺序相关的最小间距、平移、旋转、属性及碰撞等多种过滤模式。 | `pose.filter` 提供置信度、类别、名称、pickable、XY面积、位置范围、最小间距及数值 JSON Pointer 范围；通过/未通过分别保留原顺序。三维碰撞NMS、旋转规则和抓取面积不是本切片能力。 |
| PrimitiveFilter2D / `Primitive_Processing/PrimitiveFilter2D.cpp` | 当前实现按名称分成匹配和不匹配集合，未知模式报错。 | 共用 `pose.filter`，缺失可选字段不匹配，不把缺失置信度当成0或把未设置 pickable 当成已经确认的状态。 |
| PrimitiveTransformation / `Primitive_Processing/PrimitiveTransformation.cpp` | FrameTree普通模式或输入对象变换，支持左乘/右乘，输入变换组与工件做笛卡尔展开。 | `pose.transform_3d` 固定 source→target 左乘，检查源帧、目标帧和右手刚体；局部平移通过 `pose.offset_3d` 显式表达。保持 ID/尺寸/属性，不隐式做笛卡尔展开。 |
| PrimitiveTransformation2D / `Primitive_Processing/PrimitiveTransformation2D.cpp` | 支持普通对象变换及透视恢复；透视时恢复四角后取轴对齐包围框，可能改变角度/尺寸含义；含逐像素mask重采样。 | `pose.transform_2d` 仅接受无镜像相似变换，位置、朝向和尺寸有闭合语义；图像透视重采样由图像包处理，P1不把透视四边形伪装成同一旋转矩形。 |
| ResetPrimitives / `Primitive_Processing/ResetPrimitives.cpp` | 可重设名称或三维尺寸。 | `pose.update` 对允许的属性做明确整体替换，不许改 ID/frame/position/rotation；空列表仍验证补丁，不让非法配置因无实例而被吞掉。 |
| Construct2DPose / `2D_Processing/Construct2DPose.cpp` | x/y/theta 可由动态标量端口替代参数，写入相机ID和名称。 | `pose.construct_2d` 使用现有 `std.Value.v1`，动态坐标必须匹配 px/m、角度为 rad，覆盖而非相加；尺寸与身份不能省略为隐式工件。 |
| Decompose2DInformation / `2D_Processing/Decompose2DInformation.cpp` | 对几何或Primitive2D提取 x/y/theta；集合只取第一个，空集合报错。 | `pose.decompose_2d` 显式零基 index 并输出 x/y/angle/width/height 的带单位值。原生几何的字段含义仍由相应原生类型表示，不混为工件。 |
| Primitive3DtoString / `property_settings.yml` | 只有脚本路径声明：只打印首个实例，支持多个欧拉顺序、长度/角度单位与分隔符；所引脚本未提供。 | `pose.to_text` 保存单体/集合的完整版本化 JSON，含ID/帧/单位/属性；不是旧分隔符或任意欧拉顺序协议兼容。 |
| Primitive2DtoString / `property_settings.yml` | 只有脚本描述，首个实例位置/角度文本和角度单位选项；没有脚本可供核验。 | 与3D共用 `pose.to_text`，不静默丢弃除首项以外的实例。 |
| StringtoPrimitive3D / `property_settings.yml` | 文本转工件的脚本声明，具体解析实现不可得。 | `pose.from_text` 只接受完整 type/data 和受校验的 v1 schema，拒绝未知版本、损坏 JSON、深度超限与未归一化旋转；完整数值精度往返。 |
| Compare2Primitive2D / `property_settings.yml` | 位姿比较脚本声明；实现不可得。 | `pose.compare_2d` 同帧同单位两个单实例，分别输出位置距离、周期角度差和双门限判断；对称周期必须显式给定，跨帧和多实例模糊匹配报错。 |
| MakeOffset / `property_settings.yml` | 动态/默认XYZ偏移，毫米单位；布尔指定局部或世界参考系；脚本实现不可得。 | `pose.offset_3d` 米制偏移，local先由实例旋转到父帧、world直接相加；保留旋转、帧、尺寸和属性。 |
| ConstructPrimitive3D / `property_settings.yml` | 动态输入与常量相加的米制/度制XYZ脚本声明，未提供脚本。 | 共用 `pose.construct_3d`，动态输入选择覆盖常量、弧度明确；不按名称猜测“相加”或旧脚本协议兼容。 |

## 本批契约与验证

- 单体类型为 `vision.Pose2D.v1` / `vision.Pose3D.v1`；集合为 `vision.Poses2D.v1` / `vision.Poses3D.v1`。小型对象使用 Record，不新增 Data::Kind 或 C ABI。列表最多4096项、1MiB、16层；属性不承载图像/点云/大数组。
- 单体带 id/frame/unit、中心 position、正 size；2D为弧度 angle，3D为右手 xyzw 单位四元数。集合要求同帧同单位且ID唯一，空集合是合法结果。
- 数值构造使用原始double参数，不经过 Vec3f/Vec4f 的float读取再计算；本批 `pose.to_transform` 是显式米制 Record→原生float32矩阵边界，像素工件必须先经过平面标定，超出float32范围有诊断。
- [非交换调度示例](../examples/p1-pose-flow.lyflow.json) 将工件先变换到世界帧，再沿新局部X轴偏移，预期中心为 `(10,2,0)m`，最后JSON往返保留结果。可由桌面、CLI或嵌入宿主使用同一GraphDoc，不需要图像/PCL。
- 永久回归覆盖动态单位、q/-q及极大/极小四元数、内禀XYZ顺序、重复ID、空筛选、缺属性、补丁不污染上游、非交换旋转/平移、局部/世界偏移、三点分支、up正交化、共线拒绝、显式角度周期、Unicode和完整JSON往返、已知平面mm→m、原生几何端点/半径和逆转置平面。仿射乘加将全部项统一补偿并保留乘积舍入误差，覆盖1e20平移抵消后的小距离及各轴/正负法向。
- 测试是软件语义与合成真值验证，不代替现场物体尺寸、相机/机器人标定、对称规则或生产抓取验收。3D物体对称轴优化、跨帧比较、裁云、任意深度反投影及旧发布字符串协议仍属后续范围。
