# std-geometry

无 PCL、OpenCV 等额外依赖的基础几何算子包，版本 0.2.0。默认随标准包编入，也可通过外部包路径单独启用。复用已有原生二维几何和 Plane 数据类型，不增加 Data::Kind 或 C ABI。

| ID | 名称 | 输入 → 输出 |
|---|---|---|
| `geometry.point_2d` | 二维点构造 | 参数 / 可选 x,y → Point2D |
| `geometry.line_2d` | 二维直线与线段构造 | 参数 / 可选 x,y,angle → Line2D |
| `geometry.circle_2d` | 二维圆构造 | 参数 / 可选 x,y,radius → Circle2D |
| `geometry.plane` | 三维平面构造 | 参数 → Plane |
| `geometry.distance_2d` | 二维几何距离 | a, b → Measurement |
| `geometry.intersect_2d` | 二维几何求交 | a, b → Record |
| `geometry.midpoint_2d` | 二维点中点 | Point2D a, b → Point2D |
| `geometry.project_point_2d` | 二维点正交投影 | Point2D, Line2D → Point2D |
| `geometry.point_at` | 交点索引提取 | Record → Point2D |

二维坐标使用显式 `unit=m|px`。两个输入单位必须一致，不把毫米显示值混入米制几何。构造器的可选标量端口读取经过校验的 `std.Value.v1` number，x/y/radius 的单位必须精确匹配坐标 unit，覆盖对应参数；不隐式转换 mm 或无单位数。角度输入显式声明 deg 或 rad，覆盖方向向量/方向角参数。没有角度输入时，直线的 representation 选择方向向量或显式单位的角度参数。

几何值要求有限；圆半径为正，负值不静默截断为零；直线方向非零；线段端点不重合。线段模式的 start/end 为权威端点，不接受角度输入；无限线使用经过点和方向。平面使用 `n·p+d=0`，坐标为米，法向和 d 同除以法向长度。宿主可绑定顶层图参数、按次覆盖法向和 d，不在包中引入外部 request 单例状态。现有公开 C ABI 不支持直接注入 Plane，不能把原生内部数据类型误认为宿主注入接口已经具备该能力。

原生几何坐标为 float32；动态数值输入先转为最终存储坐标，再校验线段端点并计算方向。收窄后端点重合会产生诊断，不发布下游无法消费的零长度线段。

距离通过 mode 显式区分三种定义：

- `minimum`：默认，点、无限线、线段、圆之间的**无向最小边界距离**。圆表示圆周，不是实心圆；包含情形仍计算边界间距，相交/相切/线段重叠为零。
- `signed_clearance`：点/圆间为中心距减去半径和，圆线为圆心到线/线段的距离减去半径，因此重叠可为负；不接受两个直线输入。
- `first_anchor`：只接受两个直线输入，计算第一条直线参考点到第二条线/线段的距离；交换输入可能改变结果，不等于两条无限线的最短距离。

两个圆心开关可将对应圆显式视为点。返回 Measurement 的单位是 m 或 px，`ok=true`；非法输入是节点错误，不伪造距离。

最小距离与求交共用原始方向的平行判定和参数求解，不对分别单位化的方向做叉积，以免把精确平行误判成远处相交。线段保留原始端点，端点差和差积用带 `std::fma` 的浮点展开保留舍入低位，不使用固定角度阈值。非平行无限线即使夹角极小，最小距离仍为零；有限线段则检查解析交点是否位于端点区间，距离计算不套用求交的坐标容差。

有限线段判界分别使用相对原始起点和终点的投影，不将远端位置压成单个可能舍入为 1 的比例。共线重叠直接比较原始主轴坐标。垂足以原始方向的精确 dot 构造有理坐标分子，线线交点使用相应的精确差积有理分子，均消去大项后再做除法；避免从远锚点加上舍入参数，或固定从查询点减去近似单位法向所产生的抵消误差。

圆线求交按原始隐式直线方程，分别为两个坐标构造补偿二次式，用稳定的 q 公式及 C/q 保留局部小根，再按直线方向关联两个坐标的分支；不从已舍入的另一坐标相减恢复近零分量，也不使用“巨大圆心垂足 ± 几乎同大的弦长”求小根。最短距离与有符号间隙复用补偿的端点/垂直间隙，端点前、内部、端点后的小距离不会因另一端点或圆心很远而被吞掉。

圆圆求交使用补偿平方差计算外离/内含间隙与弦高，再将精确根轴方程交给同一隐式圆线求根器，不用弦中心加减偏移恢复局部小分量；输入交换不改变交点，近乎相切仍按坐标容差区分空、单点和双点。距离与相交判定复用这些间隙，避免大半径吞掉小圆半径，或 `hypot` 舍入后把实际相离误当相切。

交点使用版本化 Record：

```json
{
  "type": "geometry.Intersections2D.v1",
  "data": {
    "unit": "m",
    "relation": "finite",
    "points": [[-1, 0], [1, 0]]
  }
}
```

`relation=finite` 对应一至两个离散交点，按 x/y 升序输出；`empty` 是有效无交点；`coincident` 是无限线/圆重合或线段连续重叠，后两种均输出空 points，不伪造唯一交点。`tolerance` 为正的坐标距离容差，使用输入单位，不作为角度阈值；线方向叉积非零时保留近乎平行线的远处交点，与最短距离的非平行判定一致。交点超出 Point2D 可表示范围时报告诊断。无限延长线上的交点不算有限线段的交点。

`geometry.point_at` 校验整个 Record，按零基索引提取原生 Point2D，负索引和越界返回 `index_out_of_range`。空或连续重叠结果没有可提取的点。中点节点只接受两个 Point2D，不替任意两个几何体臆造“中心”。

`geometry.project_point_2d` 覆盖点到直线的正交垂足。对于有限线段，默认输出线段上的最近点；关闭 clampToSegment 才投影到延长线。

## 外部源码分析后的取舍

本实现逐项阅读了本机 xyz-vision-lib 的 GenerateGeometry2D、InputPlane、CalculateGeometryDistance 和 Geometry2DIntersection 源码，再按 LyFlow 数据模型独立实现。辅助源码没有 Git 元数据，不声明与清单修订兼容，也未复制外部实现代码。

原生成节点把几何和图像绘制耦合，并允许动态坐标/角度；本包保留严格单位的动态输入，把绘制交给已有查看器。原平面节点将单个平面包装为 PlaneVector，并缺少零法向检查；本包使用原生 Plane 且验证归一化。原距离包含带符号间隙和不对称的直线参考点定义，因此本包保留显式模式，默认另选常规最小边界距离。原“求交”也承担两点中点与点线垂足，本包拆成独立专业算子，扩展圆/线段求交并明确无解、重合、多解；避免依赖度数差判断平行和直接求逆。

测试覆盖构造、九种几何距离组合、有限端点、圆包含/相切/双交点、重合、不同单位、非法数据、Record 校验及真实调度中的混合几何输入。本节说明P0首批基础能力；P1变换与工件对象见下节。

## P1 工件对象与变换

P1 在不增加核心种类或 ABI 的前提下，提供以下节点。外部20项源码/脚本属性逐项分析及保留边界见[实现取舍](../../docs/p1-analysis-pose.md)。

| ID | 契约 |
|---|---|
| `pose.construct_2d` / `pose.construct_3d` | 带稳定ID、参考帧、尺寸及元数据的单个工件；动态位置与旋转标量显式单位。 |
| `pose.concat` / `pose.get` | 同维、同帧、同单位集合，保留顺序；重复ID和越界报错。 |
| `pose.filter` | 置信度、类别、名称、pickable、XY面积、位置、最小间距或数值字段范围；同时输出 matched/rejected。 |
| `pose.update` | 替换名称、分类、置信度、pickable、size或attributes，不修改身份和位姿。 |
| `pose.transform_2d` / `pose.transform_3d` | 显式 source→target 左乘；二维为无镜像相似变换，三维为右手刚体。 |
| `pose.offset_3d` | 局部或父参考帧下的米制位置偏移，不改变旋转和尺寸。 |
| `pose.flip_axis_2d` | 用户明确声明n重旋转对称等价，选最靠近给定方向的等价角度。 |
| `pose.frame_from_points` | 按序两/三点构造右手正交帧；三点输入优先，两点模式将up正交化，拒绝重合/共线。 |
| `pose.decompose_2d` / `pose.compare_2d` | 显式索引分解带单位数值；同帧同单位比较位置与显式周期角度。 |
| `pose.to_text` / `pose.from_text` | 完整type/data的版本化JSON往返，不丢ID、帧、单位、属性和列表项；文本最多64KiB。 |
| `pose.to_transform` | 显式提取所选米制工件局部→父参考帧的原生float32刚体矩阵。 |
| `pose.to_world_plane` | H映射像素中心，局部雅可比映射朝向，再乘已知plane→world；必须配置已知米制size3D，不猜透视框的物理尺寸。 |
| `geometry.transform_2d` | 点/线/圆保持类型的相似变换，方向只乘线性部分，线段端点同步更新。 |
| `geometry.transform_plane` | 可逆仿射的逆转置法向与平移d，拒绝归一化矩阵相对行列式≤1e-12的近奇异变换。 |

工件数据使用 `vision.Pose2D.v1` / `vision.Pose3D.v1`，集合使用 `vision.Poses2D.v1` / `vision.Poses3D.v1`。单体 `data` 的必需字段为非空 `id`、`frame`、`unit`、中心 `position`、正尺寸 `size`，以及二维弧度 `angle` 或三维xyzw单位四元数 `rotation`。二维单位为 `px` 或 `m`，三维仅为 `m`；不隐式换单位或变更帧。可选字段为字符串 `name/classId/classLabel`、`confidence∈[0,1]`、布尔 `pickable` 和有界对象 `attributes`。缺失可选字段没有默认业务判定；构造器的 `pickable` 默认false。

集合形如 `{"type":"vision.Poses2D.v1","data":{"frame":"image","unit":"px","items":[...]}}`，items中的每项保留完整单体data。最多4096项、1MiB、16层，要求同帧同单位且ID唯一，允许合法空集合。所有生产者和消费者均校验；重复ID不自动重命名。小型attributes可保留null元数据，尺寸、位置、旋转等计算字段不能为null或非有限数。不得将像素、点云、进程指针或大数组放入工件Record。

三维构造可选四元数或固定内禀XYZ弧度 `R=RxRyRz`。四元数采用缩放归一化，q/-q规范为同一符号，零/非有限输入报错；输出不通过欧拉角反解，避免万向节锁。变换矩阵使用行主序，旋转右手，source→target左乘；局部偏移先由工件朝向旋转到其父帧。二维矩阵的平移单位必须由参数明确匹配工件单位；原生米制Transform不能直接当像素坐标输出。

`pose.filter` 的区间包含端点；缺失可选字段不匹配。最小间距模式按原输入顺序，对已接受项贪心检查，保留首项且不重新排序；面积为sizeX×sizeY，单位是实例单位的平方，不替代抓取面积。`pose.update` 的attributes整体替换，非法补丁即使面对空列表也报错。文本序列化采用JSON可往返数值精度，完整集合超过64KiB时明确报错，调用者需先拆分，不能截断首项或丢失元数据。

`pose.to_world_plane` 仅适用于已知物理平面：`image.PlaneCalib.H` 的像素→mm/m先换算为米，再乘显式plane→world刚体。朝向由单应性在中心的局部雅可比求出；size3D为已知物理输入，默认零占位必须由用户配置为正数。它不是任意深度恢复、相机6DoF估计或机器人FrameTree协议适配。一个无图像/点云依赖的完整例子见[非交换位姿链](../../examples/p1-pose-flow.lyflow.json)。
