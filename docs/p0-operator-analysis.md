# P0 算子源码分析与实现取舍

[文档中心](README.md) · [能力对照](operator-coverage.md) · [补齐计划](operator-completion-plan.md) · [数据契约 ADR](adr/0029-p0-values-geometry-and-image-contracts.md)

逐项阅读用户提供的 xyz-vision-lib 源码、头文件和可获得的属性配置，再修订 LyFlow 实现。本页覆盖第一批 23 个外部条目，按实际语义拆分成通用算子。原源码位于用户本机 `xyz-vision-lib` 目录；没有 Git 元数据，因此仅能核对该份文件，不能声明与清单 `72d7885c` 完全一致。

本次独立实现通用数值、几何和 OpenCV 算法，保留可复核的行为，显式修正已确认的边界问题。原节点委托给外部私有依赖的内部算法仍有核验边界。下文的优化是算法结构、诊断和资源约束上的选择；未对原库给出未经测量的加速比。

## 值、信息与集合（9 项）

| 外部项 / 参考源码 | 核实的原行为 | 问题、优化与实现选择 | LyFlow 入口与回归 |
|---|---|---|---|
| `InputInfo`<br>`src/xyz_vision_lib/models/Input/InputInfo.cpp` | 输入source=params/request；输出Primitives2D、Primitives3D、String；setRequest从xyz Request拷贝工件信息，未带info的新请求保留旧info。 | 节点持有跨请求状态；request协议及工件模型强耦合，source参数之外没有清楚会话隔离。 文本/数值常量使用value.string/value.number；宿主信息通过顶层图参数绑定及公开params_json按次覆盖，不引入跨请求残留。公开C ABI仅云/图像注入，不支持直接Record注入；C++内部Data injection不是公开ABI承诺。不声称P0支持工件对象或xyz协议。 | value.string/value.number/value.field；公开 C ABI 图参数逐次覆盖及无状态回归见[组合流程](../core/tests/integration/p0_pipeline.cpp)。 [实现](../packs/std-value/ops/register.cpp) · [回归](../packs/std-value/tests/test_values.cpp) |
| `InputNumber`<br>`src/xyz_vision_lib/models/Input/InputNumber.cpp` | 参数input_number为double，输出单Double。 | 该run未显式检查非有限值或单位。 独立重实现有限数值Record，显式unit且单次纯函数输出。 | value.number；test_values常量/有限数/单位测试。 [实现](../packs/std-value/ops/register.cpp) · [回归](../packs/std-value/tests/test_values.cpp) |
| `BasicArithmetic`<br>`src/xyz_vision_lib/models/Flow_Data_Operation/BasicArithmetic.cpp` | 双Double输入，支持加减乘除和pow；除法条件b<=epsilon。 | 负分母以及极小正分母被误判成零；pow和四则结果未显式检查非有限/定义域；无量纲约束。 检查真正零分母，负数与合法小量可除；结果必须有限；幂仅无单位并拒绝0^非正；加减同单位，乘除显式量纲。无原代码复制。 | value.arithmetic；负分母、小量、溢出、pow定义域及单位测试。 [实现](../packs/std-value/ops/register.cpp) · [回归](../packs/std-value/tests/test_values.cpp) |
| `CompareNumbers`<br>`src/xyz_vision_lib/models/Flow_Data_Operation/CompareNumbers.cpp` | 双Double转Bool；>/<和<=/>=严格数值；=用abs(a-b)<epsilon，!=用abs(a-b)>epsilon。 | epsilon边界时=和!=同时false，epsilon为0时相等数的=也false；大数差可溢出。 统一等价阈值<=absTol+relTol*scale，!=严格补集；有序比较容差内视同等；缩放差避免溢出，跨单位拒绝，参数明确不追求兼容。 | value.compare；容差等号、严格补集、大数正负、不同单位测试。 [实现](../packs/std-value/ops/register.cpp) · [回归](../packs/std-value/tests/test_values.cpp) |
| `LogicOperation`<br>`src/xyz_vision_lib/models/Flow_Data_Operation/LogicOperation.cpp` | 双Bool输入；NOT只读a，其余AND/OR/XOR/NAND/NOR/XNOR。 | UI端口B固定但NOT不需B；二元节点本身并不负责调度短路。 完整7运算，NOT允许b不连接；二元显式要求bool，惰性执行交给flow.select避免伪短路声明。 | value.logic；真值组合、NOT缺B、二元缺B、非法数值输入测试。 [实现](../packs/std-value/ops/register.cpp) · [回归](../packs/std-value/tests/test_values.cpp) |
| `MergeVector`<br>`src/xyz_vision_lib/models/Flow_Data_Operation/MergeVector.cpp` | 按参数动态增加输入端口，按顺序push各元素成向量；支持Double/Plane/Cloud/Primitives3D/Geometry2D/3D；不去重。 property_settings.yml将element_number限制1..12。 | UI端口数有1..12上限，但向量元素或云的字节总量没有本节点统一预算；run未识别类型可能不更新res，依赖外部NodeData约束。 P0只接有界标量/小Record，固定a/b端口可链式list.concat；保留顺序和重复、记录type一致、大小/深度/字节有预算；点云/原生几何集合留后续，绝不序列化大云到JSON。 | list.pack；顺序重复/异型/单位/空集合/上限测试。 [实现](../packs/std-value/ops/register.cpp) · [回归](../packs/std-value/tests/test_values.cpp) |
| `ConcateVector`<br>`src/xyz_vision_lib/models/Flow_Data_Operation/ConcateVector.cpp` | 动态多端口；向量平铺拼接；Cloud克隆首云后拼点；String以connector拼接；不去重。 property_settings.yml将vector_number限制2..8。 | UI输入端口数有2..8上限，但拼接后的集合/字符串/云载荷未设统一本节点大小预算；Cloud通道和元信息依赖外部载荷实现。 P0 list.concat对同类型小列表按顺序拼接；value.concat保留字符串connector；已有util.merge处理双云，动态云集合不在P0。 | list.concat/value.concat；重复/空串/Unicode/connector/大小上限测试。 [实现](../packs/std-value/ops/register.cpp) · [回归](../packs/std-value/tests/test_values.cpp) |
| `FetchElement`<br>`src/xyz_vision_lib/models/Flow_Data_Operation/FetchElement.cpp` | 动态输出端口对应element_index数组；按非负索引取向量元素，越界alert=true抛错，false输出默认构造元素。 | alert=false会把越界伪装为默认值，数据错误易流到后续；参数索引数组与端口计数强耦合。 list.get仅固定单输出，越界始终可定位诊断；多项用list.subset，负索引明确拒绝。 | list.get；负/越界/单位保留/值不变测试。 [实现](../packs/std-value/ops/register.cpp) · [回归](../packs/std-value/tests/test_values.cpp) |
| `VectorSubset`<br>`src/xyz_vision_lib/models/Flow_Data_Operation/VectorSubset.cpp` | 按element_index顺序提取子向量，保留重复，任一越界抛错。 | 取输入向量和值可能额外拷贝；无输入/输出资源预算，列表类型动态切换。 保留显式索引/顺序/重复和错误语义，固定有界同类型Record载荷；空索引返回有效空列表，不改输入。 | list.subset；空/重复/乱序/浮点索引/负数/越界/只读输入测试。 [实现](../packs/std-value/ops/register.cpp) · [回归](../packs/std-value/tests/test_values.cpp) |

## 几何（4 项）

| 外部项 / 参考源码 | 核实的原行为 | 问题、优化与实现选择 | LyFlow 入口与回归 |
|---|---|---|---|
| `GenerateGeometry2D`<br>`src/xyz_vision_lib/models/Input/GenerateGeometry2D.cpp` | Point/Line/Circle 参数构造；Double端口覆写x/y和角度或半径；方向角为度并wrap360；负半径被截到0；可另输出绘制过的Image。 | 动态端口是实际能力，草稿只有常量，已修订为严格std.Value.v1覆写；绘制只在输出连接时clone图像，计算几何与图像绘制仍耦合；负半径静默修正可能隐藏配置错误，缺显式坐标单位；按原生类型拆为point/line/circle构造器，复用查看器而非clone图像；连接的x/y/radius要求精确m/px；angle显式deg/rad并覆盖参数方向；拒绝非有限、负半径、不可表示数；增加有限线段构造，端点定义清楚 | `geometry.point_2d`、`geometry.line_2d`、`geometry.circle_2d` [实现](../packs/std-geometry/ops/geometry.cpp) · [回归](../packs/std-geometry/tests/test_geometry.cpp) |
| `InputPlane`<br>`src/xyz_vision_lib/models/Input/InputPlane.cpp` | 从a/b/c/d参数或request.info.plane四元素读取单平面，法向和d一起归一化，输出单元素PlaneVector。 | 法向为零时原实现直接除零；请求缓存保存在节点状态，缺新请求时可能保留前次值；PlaneVector并不表示本节点支持任意多平面输入；只构造现有原生Plane，无新核心类型或ABI；有限非零法向，double范数避免float平方溢出，归一化d并检查输出范围；宿主来源绑定顶层图参数、按次覆盖normal/offset，不引入request共享状态；当前公开C ABI只支持已有注入种类，不声称Plane直接注入 | `geometry.plane` [实现](../packs/std-geometry/ops/geometry.cpp) · [回归](../packs/std-geometry/tests/test_geometry.cpp) |
| `CalculateGeometryDistance`<br>`src/xyz_vision_lib/models/2D_Processing/CalculateGeometryDistance.cpp` | 点/圆间用中心距减半径和；点/圆到线用投影法向距离减半径，结果可负；圆心开关把对应半径置0；线线定义为第一参考点到第二线，非对称。 | 不是一般的最短边界距离；相交/包含圆可能负；线线利用角度位复用圆半径再抵消，容易误读且不具几何对称性；没有有限线段或单位契约；默认minimum为专业常规无向边界距离，支持点/线/线段/圆所有组合；signed_clearance显式保留有符号间隙；first_anchor仅两线，明确非对称；圆心开关保留；坐标m/px不自动转换；double中间量，无Eigen/PCL依赖；拒绝不支持的模式组合 | `geometry.distance_2d` [实现](../packs/std-geometry/ops/geometry.cpp) · [回归](../packs/std-geometry/tests/test_geometry.cpp) |
| `Geometry2DIntersection`<br>`src/xyz_vision_lib/models/2D_Processing/Geometry2DIntersection.cpp` | 两点求中点；点线求垂足；两线求唯一交点。无圆和线段模式。 | 原名混合中点/投影/求交三种不同操作；角度差平行判定只看0/360附近，漏180度；直接2x2逆矩阵退化时不稳健；单一Geometry2D输出不足以表达多交点和重合；拆midpoint/project_point/intersect/point_at，保留点线垂足并给有限线段clamp选项；使用归一化方向叉积和参数式求交，无矩阵求逆；增加点/圆/线段组合；版本化geometry.Intersections2D.v1区分empty/finite/coincident，有限点按x/y排序；point_at检查整个结构及索引；拒绝非有限/混单位，坐标容差显式m/px；不伪造空结果中的(0,0) | `geometry.midpoint_2d`、`geometry.project_point_2d`、`geometry.intersect_2d`、`geometry.point_at` [实现](../packs/std-geometry/ops/geometry.cpp) · [回归](../packs/std-geometry/tests/test_geometry.cpp) |

## 图像与量测（10 项）

| 外部项 / 参考源码 | 核实的原行为 | 问题、优化与实现选择 | LyFlow 入口与回归 |
|---|---|---|---|
| `BlobDetector`<br>`src/xyz_vision_lib/models/2D_Processing/BlobDetector.cpp` | Mask→count/areas/Primitive2D/mask/viz；轮廓层级扣孔洞面积、旋转最小框、长宽比过滤、面积降序前N。 | 用SAUF稳定连通域的实际前景像素计面积并保留孔洞，避免轮廓面积截断和依赖P1工件类型；显式提供旋转框/长宽比、稳定排序及可选截取；局部框扫描总32MP、导出轮廓32768点、结果1MiB预算。 边界：像素面积与原多边形轮廓面积不同；不伪装Primitive2D或相机元数据。 | `image.components` · test_image_p0.cpp: P0 component analysis [实现](../packs/std-image/ops/) · [回归](../packs/std-image/tests/test_image_p0.cpp) |
| `CalculateEdgePosition`<br>`src/xyz_vision_lib/models/2D_Processing/CalculateEdgePosition.cpp` | 对ROI灰度均值投影，梯度峰值支持单边/内外双边/多个边、正反扫描及极性，输出位置向量。 | 专门交付均值投影edge_positions而非借line拟合替代；抛物线亚像素，峰值阈值原强度单位，空结果为count0，不返回-1哨兵。 边界：当前矩形ROI坐标；旋转工件ROI需先显式warp，不隐式继承ROI对象。 | `image.edge_positions` · test_image_p0.cpp: P0 projection edge positions [实现](../packs/std-image/ops/) · [回归](../packs/std-image/tests/test_image_p0.cpp) |
| `CalculateWidthRange`<br>`src/xyz_vision_lib/models/2D_Processing/CalculateWidthRange.cpp` | 在矩形/旋转ROI上横纵多条带宽平均扫描，外尺寸取最外开闭边、内尺寸取中心两侧最近边，输出min/max。 | 修正初稿首个边对算法，明确inner/outer；支持浮点/u16原强度，NaN跳过，最少有效比例及宽度范围；无目标返回无效Measurement。 边界：仅显式轴对齐扫描；原旋转ROI可由显式图像变换另行接入，非任意形状全局极值。 | `image.width_range` · test_image_p0.cpp: P0 width distinguishes [实现](../packs/std-image/ops/) · [回归](../packs/std-image/tests/test_image_p0.cpp) |
| `CheckImageChange`<br>`src/xyz_vision_lib/models/2D_Processing/CheckImageChange.cpp` | 同类型同尺寸图像，灰度绝对差阈值、ROI、十字开运算、变化像素面积大于min_area输出bool。 | 交付luma或最大通道差、可选开运算及严格面积判定different；跳过非有限像素、忽略alpha，保留raw/filtered统计；不复制原显示路径的越界绘线问题。 边界：形态学半径0可显式关闭，原代码强制>0；采样面积与原图面积标注。 | `image.change_stats` · test_image_p0.cpp: P0 image change combines [实现](../packs/std-image/ops/) · [回归](../packs/std-image/tests/test_image_p0.cpp) |
| `ConvertImageType`<br>`src/xyz_vision_lib/models/2D_Processing/ConvertImageType.cpp` | u8 RGB↔灰度两个模式。 | 扩大为u8/u16/f32灰度/RGB/RGBA保持位深，明确RGB非BGR、alpha满量程、不改输入。 边界：颜色转换本身无位深归一化；位深变换继续使用image.normalize。 | `image.convert_channels` · test_image_p0.cpp: P0 image channel conversion [实现](../packs/std-image/ops/) · [回归](../packs/std-image/tests/test_image_p0.cpp) |
| `DetectLineEdge`<br>`src/xyz_vision_lib/models/2D_Processing/DetectLineEdge.cpp` | 多条灰度平均卡尺，按方向/极性取首峰，PCL固定2px RANSAC拟合，输出直线Geometry2D。 | 不引入PCL到图像包；Huber初拟合+显式容差内点+L2重拟合，暴露有效比例、反向/首末最强选择及原图坐标样本；无目标明确诊断。 边界：轴对齐ROI扫描；全局Line2D单位px；不复刻原ROI父层位姿隐式传递。 | `image.line_caliper` · test_image_p0.cpp: P0 line calipers [实现](../packs/std-image/ops/) · [回归](../packs/std-image/tests/test_image_p0.cpp) |
| `GrayScaleStatistics`<br>`src/xyz_vision_lib/models/2D_Processing/GrayScaleStatistics.cpp` | 256桶直方图min/max/mean/mode/median；RGB路径用了BGR灰度，位深范围固定0..256。 | 整数u8/u16分别256/65536桶一次统计，避免全像素double排序；f32有界4MP精确值排序，mode并列取小值；选定通道或显式to_gray，正确RGB顺序；空/NaN明确无效。 边界：浮点众数按精确float值，不做隐含分箱；统计Record字段可通过value.field提取。 | `image.statistics` · test_image_p0.cpp: P0 exact gray statistics [实现](../packs/std-image/ops/) · [回归](../packs/std-image/tests/test_image_p0.cpp) |
| `ImageDifference`<br>`src/xyz_vision_lib/models/2D_Processing/ImageDifference.cpp` | 可选ORB特征/直接法SE2配准，正/负/双向灰度差分阈值，Canny共同边缘距离抑制。 | 独立difference_mask支持方向差分、共同轮廓距离抑制和有质量/位移门限的平移相位相关；拒绝类型/尺寸不符，配准失败明确报错，不静默回退或裁交集；避免原u8指针误读其他位深。 边界：未宣称ORB/旋转/尺度SE2配准等价；首版有界平移，原高阶配准另列后续。 | `image.difference_mask (+ image.arithmetic for plain add/sub/absdiff)` · test_image_p0.cpp: P0 contour-tolerant difference [实现](../packs/std-image/ops/) · [回归](../packs/std-image/tests/test_image_p0.cpp) |
| `ImageThresholdOperation`<br>`src/xyz_vision_lib/models/2D_Processing/ImageThresholdOperation.cpp` | HSV H0..360/S,V0..255含色相环绕，灰度区间及反色，输出Mask/彩色显示。 | 复用现有固定/Otsu/自适应阈值，新增gray/RGB/HSV闭区间，HSV统一H度/S,V0..1，u8/u16/f32均按明确满量程，反选仍排除NaN/Inf。 边界：参数范围不逐值兼容原HSV S/V0..255；不生成仅显示用途的冗余RGB mask。 | `image.range_mask (+ existing image.threshold)` · test_image_p0.cpp: P0 color range [实现](../packs/std-image/ops/) · [回归](../packs/std-image/tests/test_image_p0.cpp) |
| `ROIGenerator`<br>`src/xyz_vision_lib/models/2D_Processing/ROIGenerator.cpp` | 旋转矩形/圆/圆环弧/多边形，可叠加外部文件mask与Primitive2D变换，输出附带ROI元数据的Image。 | 显式同尺寸mask替代隐式ROI状态，支持所有参数几何形状与输入mask相交、可选Box2D；多边形有点数/文本预算；非矩形循环取消，三角函数移出循环。 边界：不导入Primitive2D或父ROI元数据；工件驱动ROI变换依赖P1位姿/标定。 | `image.roi_mask` · test_image_p0.cpp: P0 ROI masks [实现](../packs/std-image/ops/) · [回归](../packs/std-image/tests/test_image_p0.cpp) |

## 可运行示例与验收

[区域统计与宽度示例](../examples/p0-inspection.lyflow.json)使用仓库内合成的 [PGM 条纹](../examples/p0-stripe.pgm)，不依赖外部模型或硬件：读取 → ROI 掩膜 → 统计/连通域/宽度 → 数值比较 → 惰性条件选择。

确定结果：ROI 内 336 个有效像素、一个 96 px² 连通域、平均灰度 `255 × 8 / 28`、最小和最大宽度均为 8 px。默认阈值 50 返回“通过”，阈值改成 100 返回“不通过”；未选文本分支不执行。顶层参数支持按次覆盖阈值与文本，不在下一次请求残留。

```powershell
lyflow validate examples/p0-inspection.lyflow.json
lyflow run examples/p0-inspection.lyflow.json --outputs --summary
```

需要启用 `std-value`、`std-geometry`、`std-image`。C++ 执行器、动态加载运行库的 C++ SDK 与公开 C ABI 用同一示例、同一真值验收；CLI 使用相同运行库执行该文件。桌面复用现有 manifest/参数控件、Record/原生图像结果与 Transport，前端回归和构建单独执行，不将其等同于现场图像验收。

## 保留的边界

- 这一批完成的是 P0 的通用链。InputInfo 中的工件对象、集合节点的原生云/工件/几何列表仍需后续数据模型；公开 C ABI 现有直接注入仅支持点云和图像，数值/文本按图参数覆盖。
- 抗边缘差分支持有门限的平移配准；外部 ORB/SE(2) 完整模式仍属未覆盖。卡尺及宽度使用显式轴对齐扫描，旋转工件须先有明确图像变换，不能宣称已得到任意形状工件的全局宽度极值。
- 连通域面积按前景像素计数，与轮廓多边形面积不是同一量。标签为本次图像的稳定扫描标签，不承诺跨帧工件跟踪身份。
- 像素统计/卡尺不代表毫米精度；物理单位需要经过验收的标定。软件测试不代替实际设备、生产质量、节拍和长期稳定性验收。
