# std-geometry

无 PCL、OpenCV 等额外依赖的基础几何算子包，版本 0.1.0。默认随标准包编入，也可通过外部包路径单独启用。复用已有原生二维几何和 Plane 数据类型，不增加 Data::Kind 或 C ABI。

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

测试覆盖构造、九种几何距离组合、有限端点、圆包含/相切/双交点、重合、不同单位、非法数据、Record 校验及真实调度中的混合几何输入。通用几何坐标变换属于后续 P1，不在本包首批范围内。
