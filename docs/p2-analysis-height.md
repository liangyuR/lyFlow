# B01：高度规则与有限平面片

本批以本机 `C:/Users/11601/project/xyz-vision-lib` 中五个算子的同名 `.cpp/.hpp` 为语义参考，独立设计和实现；该目录没有 Git 元数据，不声称对应某个源码修订。没有复制专有函数体，也没有引入其 Qt、全局坐标树、机器人工作空间或固定线程池。

| 参考算子 | 核对的行为与风险 | LyFlow 入口 |
| --- | --- | --- |
| CalculateHeightByNeighborCloud | 四邻域、相对/绝对尺寸、点面/层高、最高邻域选择、原始/最大高度、organized与投影索引分支、工作空间裁剪；旧辅助路径还含均值/截距。图像投影会丢失同像素后方点，未支持时以零/负数表达失败，且外部对象pose使用顶面锚点。 | `cloud.neighbor_height`：直接在物体局部米制空间选择四邻域；显式同帧workspace体积；统一支持点面、轴向交点、顶层、均值、最远点、指定高度。每实例有效性与支持数独立输出。 |
| CalculateTopLayerHeight | 云集合、顶层估计时可降采样、原云提取top/rest、organized输出用NaN占位、隐式世界/相机帧、无层负厚度sentinel。 | `cloud.top_layer`：标准原生分组，显式高度轴与原点；最高满足支持阈值的滑动高度窗；原索引保留采样来源，空/无支持为`valid=false`及`Measurement.ok=false`。 |
| CloudBasedEmptyToteChecker | 检测对象为空且有限点数超过阈值时抛“存在未识别物体”；没有输出或有效观测证据。 | `cloud.empty_tote`：由宿主提供显式观测有效性，输出unknown/empty/occupied/unrecognized及布尔Value。 |
| CloudToGeometry | 多平面、可选空间聚类、投影最小面积矩形、栅格膨胀与最大占据矩形迭代分解、正向表面分位偏移、支撑点覆盖及组合几何碰撞去重。不是无限平面系数输出。 | `cloud.plane_patches`：固定种子RANSAC加TLS、凸包边方向最小面积外包矩形或占据内接矩形分解，可选距离/法线连通分组、占据覆盖率门控、正向残差分位偏移、已接受几何的支撑点联合覆盖去重，输出真实有限三维矩形体。 |
| HeightExtraction | 仅正交f32深度；等深/三点平面/MLS曲面；无效深度替换与high/low/both映射。原曲面残差是轴向，平面残差是法向；原替换会丢失真实观察有效性。 | `image.height_extract`：显式正交物理标定，平面法向与局部加权多项式轴向高度；原始观测mask与替换值分离，输出原生f32米制高度和参考深度。 |

## 共同约定

点云坐标、尺寸、距离、轴向高度均为米；角度参数为度。`frame`必须显式对应上游数据，三维位姿Record遵循现有`vision.Poses3D.v1`，四元数xyzw、中心位置、正尺寸。跨帧变换由已有变换算子完成，绝不查询隐式坐标树。非有限XYZ/强度/法线或不齐通道明确拒绝；无目标和无充分支持是有效空结果，不能变成成功测量。

原生PointCloud没有organized宽高属性，因此没有伪造行列或根据`x+y*width`寻址。所有选择都在真实物理坐标中完成，对有序输入数组和同一批无序点具有相同几何语义；原数组下标仍通过Indices完整保留。输入点云和位姿均不被修改；子集通过`PointCloud::select`同步保留RGB、强度及法线。

点云上限100万点；拟合/聚类/矩形工作量上限1亿，单网格200万单元，有限片最多256个，邻域对象最多256个。拟合扫描、TLS与逐点复核、分位排序、聚类、凸包方向检验、网格和碰撞检查共同累计同一预算，不在拟合完成后重置；邻域有额外保守预检。超过预算报告`resource_limit`，不静默截断。长循环轮询取消，所有结果在成功末尾统一发布，取消不发布部分结果。候选遍历、拟合随机种子、相同面积矩形取舍和ID顺序固定；确定性针对相同有序输入，不承诺打乱点序后ID相同。

## 分组顶层高度

输入`cloud`和可选成对的`groups/indices`；省略分组时全云作为ID为`0`的一组。分组遵循`cloud.Groups.v1`，保持组ID及空组。`voxelSize=0`表示不降采样；正值只对估计窗的支持点进行稳定首点体素采样，提取始终使用原始点。`minPoints`因此表示估计样本数，不应当和降采样前点数混淆。

`heights:cloud.TopLayers.v1`每组包含id、valid、height（无效为null）、support与topCount。`height:Measurement`只代表第一组，单位始终m。`top/rest`是通道齐全的新原生子集，具有各自新身份；`topIndices/restIndices`及`topGroups/restGroups`则全部指向**输入cloud的身份**。消费分组时必须接输入cloud，不可把这些索引接到top/rest子集。重叠输入分组会在输出索引中按组保留相应重复成员。

## 邻域高度

输入`cloud/instances`，可选单个`workspace:vision.Pose3D.v1`限制场景点。方向top/left/bottom/right基于物体局部XY，而不是相机像素方向；all或except_bottom从有效方向选最大测量高度。相对模式由原始XY尺寸乘比例得到邻域尺寸，绝对模式直接使用米制width/length；margin仅在对应横向带内缩。`minHeight/maxHeight`限制相对顶面的向下轴向距离。

LyFlow位姿位置是体积中心，顶面锚点为`position+localZ*sizeZ/2`。所有尺寸更新保持该顶面不动。`point_plane`输出垂直于支撑面的距离，但尺寸更新使用沿物体Z轴与支撑平面相交的`axisHeight`，避免斜面时把两种距离混用。`axis_intercept`的height与axisHeight一致。`top_layer`选择最近的充分支持深度层；farthest保留最远观测点，mean使用均值。平面拟合有点数、内点比例与法向夹角门控。

`source=original`将输入位姿高度当作已经提供的高度保留，不重新测量；`method=provided`使用providedHeight；maximum取原尺寸与有效轴向估计的较大者。无有效邻域时保留原位姿，heights项valid=false、height/axisHeight为null、reason=insufficient_support；不会制造零尺寸实例。support是支持点数，rms是平面残差而非业务置信概率。

## 料箱观察状态

必填`observation`为boolean的`std.Value.v1`，表示采集和上游过滤是否足以支持业务判断，必须由调用方提供证据。可选单个位姿roi限制场景点与识别实例中心。有效观测时，ROI内识别实例优先判occupied；无识别且点数严格大于maxUndetectedPoints为unrecognized；其余为empty。无效观测始终unknown，empty/undetected均false，调用方必须先检查valid。仅仅点云为空不能自动证明观察有效或料箱为空。

## 有限平面片

先以可重复RANSAC找到充分支持的平面，再TLS优化并重选内点，随后最多8轮TLS与单调删点复核。最后发布模型的每个支持点都必须满足planeDistance；达到轮数上限时使用最后模型的合格子集，不在复核后再无条件拟合而让阈值漂移。共线点不构成平面。可选clusterDistance将同平面分离支持域拆开，normalAngle基于原生法线通道做无向法线连通约束，零法线拒绝。它是有界连通分解，不把专有DBSCAN的密度参数当成相同算法。

每组点投影到平面后求凸包，逐凸包边检验外包矩形面积，避免以PCA盒替代最小面积有限片。bounds输出外包矩形，coverage为占据单元数/外包网格单元数；inscribed迭代提取全占据最大矩形并清除已分配单元。栅格坐标决定成员归属，交付有限片的XY范围则取这些原生支持投影的真实min/max，避免以理想网格边界裁掉float32采样边界。栅格的占据语义是离散支撑，**不证明每个栅格内部连续表面均被扫描**。不使用膨胀填补缺测孔洞，cellSize应与扫描分辨率匹配。minSide/minArea/minPoints/minCoverage共同决定片是否有效。

surfaceOffset=quantile将拟合平面沿规范化正法向移动到正向残差分位位置（不向负法向移动），对应surfaceQuantile。thickness是明确的几何厚度，不能从二维平面中推断物体高度。偏移后先以`min(planeDistance, thickness/2)`筛选支持，再由合格点构造真实占据网格；每个候选交付前还按输出四元数和尺寸逐点检查，几何外点不进入indices，也不标记covered。片的rms以该片最终交付支持、对应normal/offset重新计算，不复用父平面的残差。支持被剔除后coverage同步重算，不能保留旧占据率。

XY尺寸仅增加显式记录的浮点舍入保护，半边增量为`64 * double-epsilon * max(1, 中心坐标绝对值, 原始XY跨度)`；patch的boundaryTolerance记录该值。它不改变planeDistance或Z厚度。最终支持仍须位于实际交付的有限Pose内。重复候选按支撑点是否被已接受矩形体（含collisionDistance容差）的联合体覆盖来筛除，比例与未覆盖点数同时满足才移除；该去重容差不参与当前片的支持归属。

输出instances为标准有限Pose3D；patches每项id与实例/组ID一一对应，含四角、面积、平面、rms、coverage、support、gridSize、occupiedCells、boundaryTolerance；coverage等于最终支持占据的单元数occupiedCells除以gridSize的面积。indices/groups绑定输入云；uncoveredIndices/uncovered返回未被接受支撑覆盖的原点及保通道子集。上游机器人或业务ROI过滤可由现有裁剪与变换算子明确组合；集成示例由本批主流程提供。

## 正交深度高度提取

仅接全分辨率单通道f32米制深度，或显式depthScale换算的u16；不接受普通归一化灰度图冒充深度。像素中心的正交物理坐标为`[originX+x*pixelSizeX, originY+y*pixelSizeY, depth]`。previewable=false；直接传scale>1明确拒绝。透视深度必须先经过对应的显式投影转换，不复用透视相机内参假装正交标定。

reference=z_plane使用z；three_points使用三个整数原图像素[x,y,0]和真实有效深度，拒绝重复/共线/深度轴平行平面。平面height是朝+Z法向的有符号距离，reference是该像素的轴向参考深度。surface使用米制radius邻域的高斯加权局部一次或二次多项式，SVD秩校验，不复制PCL MLS；sampleStep只控制邻域采样。曲面height是源深度减参考曲面深度的轴向残差。低样本/秩退化的参考面像素保持无效，不伪造曲面。

源深度非有限或非正视为无观测。mask是源有效性与参考面可计算性的交集。height/reference为有限f32；无效占位零，mask=0。invalid的on_plane/farthest/custom可替换显示值，但不改变mask；preserve保留零占位并在灰度图上显示黑色。无有效残差时farthest为零，不生成有效高度。

noise为米制零带，gain为每毫米灰度增益。high/low分别显示正/负残差的非负亮度，both以128为零点双向饱和映射；该低向映射统一为“差异越大越亮”，没有保留外部算子的反向亮度惯例。2Mi像素和50M邻域采样工作量预算，取消不发布四端口中的任何部分。

## 永久回归

`test_p2_height_rules.cpp`覆盖层高真值与高处孤点、分组原身份和全部可选通道、空测量、四邻域/多策略、保持顶面、斜面两种距离、workspace排除、unknown业务状态、L形孔洞有限片、稳定重复与非法输入/取消。解析噪声平面回归逐组验证最终平面阈值、交付Pose、支持RMS、真实占据率及uncovered互补，包含分位偏移和薄厚两种配置；合法有限巨大Pose的中间计算溢出必须原子失败。

`test_p2_height_extraction.cpp`覆盖正负米制高度、有效掩码与三种填充、方向映射、三点斜面真值、退化/越界/缩放拒绝、独立二次曲面边界真值与孔洞、u16尺度、资源上限及位深/通道错误。编译和执行结果由本批统一验证记录给出，不以测试文件存在代替通过。
