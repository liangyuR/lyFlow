# 阶段5：填洞、缺陷、空间圆、截面与盒体采样

## 外部语义核对

逐项只读核对 `C:/Users/11601/project/xyz-vision-lib` 下同名 hpp/cpp、`property_settings.yml` 的默认值与参数声明，并核对 ProfileSlicer 调用的组织网格生成器。该目录没有可核验 Git 修订，本文件不声明其版本或完整第三方底层实现。外部文件带有专有版权声明；这里仅核对端口、模式及可观察行为，算法和测试重新实现，没有复制函数体。

| 原算子 | 实际源模式 | LyFlow 实现与优化 |
| --- | --- | --- |
| CloudFillHoles | 组织相机云；有效掩膜闭运算定位候选；原观测邻域深度中值；相机反投影 | `cloud.fill_holes`；显式组织映射支持针孔和正交；原观测只读，新云身份及追加补点；不级联使用本次补值；深度跨度门控、外边界策略、独立补值状态 |
| DefectDetector | UP/DOWN/BOTH；有符号平面残差；可选闭运算；长度、宽度、面积、体积四项独立启用，任一严格超限判退 | `depth.defects` 提取并测量候选，`depth.classify_defects` 判定；桥接像素不计为缺陷支持 |
| FindCircles | 自动/手动 Canny；弧方向与相邻长度比；椭圆评分；二维圆偏心率过滤；已知半径恢复两歧义三维圆；Canny/椭圆显示 | `image.find_circles_3d`；独立弧段最小二乘、角覆盖评分、去重、K/D 校正和圆锥二次型截面；不截配半径数组；歧义保留 |
| ProfileSlicer | x/y 多截面；组织点云建三角网格并求平面交线；A/B 各六种特征；x-x/y-y；上下限；截面与曲线显示 | `depth.profile_slices` 提取截面，`depth.measure_profile_features` 测量 A/B 特征并绘图，`depth.classify_profile_features` 执行范围判定 |
| GenerateBoxCloud | 每面随机点、可见面、线框；盒中心锚定；局部/工件坐标；front 虽出现在选项中但源显式报不支持 | `cloud.box_sample`；中心锚定、确定性种子、表面/体积、可见面、精确端点线框、法线/RGB及分组；不声称支持源未实现的 front 锚定，不冒充 Mesh |

源 Profile 特征类型全部支持：`left-right-inflection`、`right-left-inflection`、`top`、`bottom`、`left-most`、`right-most`。前两者表示沿指定扫描方向的第一个显著绝对梯度，阈值为当前兴趣区域同链梯度最大值的比例，并非曲率过零点；命名保留此源业务含义。属性 `x-x` 是横向距离，`y-y` 是相对于基准面的高度差，均使用米。

## 原生数据与身份

盒体采样属于 std-pointcloud，注册钩子 `registerP5CloudInspection`。其余四项依赖标定、轮廓或形态学，属于 std-image，注册钩子 `registerP5Inspection`。图像包仅使用 OpenCV 小矩阵，不引入 Eigen/PCL 隐式依赖。

三个组织深度算子成组消费 `cloud:PointCloud`、`depth:Image`、`status:Image`、`indexMap:Tensor[H,W]`、`mapping:vision.DepthMapping.v1`，复用投影算子的校验、哈希和反投影，不从云长度猜测宽高。深度为 f32 米制；有效性由 status 决定，正交有效高度允许 0 或负数，只有透视相机 Z 必须为正。映射区分二维 imageFrame、投影三维 gridFrame、源云 cloudFrame，gridToCloud 为刚体变换。

status 的 observed=1、interpolated=2、filtered=4、collision=8 与 indexMap 的整数索引共同限定当前栅格视图。算法不遍历云中未投影、遮挡或视野外点作为可见支持。输入的三份载荷摘要、sourceCloudId、点数和可选通道必须一致。所有深度检测、填洞、截面和空间圆使用全分辨率，显式拒绝 scale>1；不得把缩小图沿用原标定。

基准平面是原生 `Plane`，`planeFrame` 必须明确等于 cloudFrame；法向和偏置一起归一化。它是无限参考面，不被用作有限平面片的占据边界。区域约束由像素 Box2D/mask 或旋转 Pose2D 指定。

## 填洞

输入状态 0 才是候选未知像素。闭运算只决定候选位置；有效值取原输入 observed 邻域的中值，不用本次补点扩散。`closeKernel`、`fillKernel` 为奇数，`minNeighbors` 和 `maxDepthSpread` 分别限制有效支持量与跨层跨度。`fillBoundary=false` 时排除连接图像外边界的未知区域，开启后才允许这些候选。

输出云默认构造新身份后逐通道复制原云并追加补点；旧点顺序保持，旧 Indices 不能绑定新云。`indexMap` 更新为追加点索引，mapping 重新绑定身份、点数和三个载荷摘要并保留父身份。`filledIndices` 指新云中本次追加点；`filledMask` 为 u8 255；status=2，不能当作直接观测。

XYZ 使用中值深度与正式映射反投影。补点 intensity/normals/RGB 作为同一个通道元组，从稳定排序后的中值深度原观测点复制；不捏造不存在的通道。`quality.channelPolicy` 说明此策略。未能补值的候选保持未知，正常返回并报告数量。

## 深度缺陷

有符号高度为 `normal·point+d`，up/down 按该法向定义，不暗自根据相机朝向翻转。支持集合为 ROI/mask 内满足方向和严格距离阈值的有效像素。可选闭运算只连接分量，量测和可视化支持仍来自真实超阈值像素，桥接的零残差像素不增加面积或体积。

每个支持像素取四个半像素边界角，使用该中心像素的实际观测深度反投影，然后沿基准面法向正交投影，累加投影四边形面积；体积为 `Σ cellArea × abs(signedHeight)`。这是中心深度在单元内常值的离散法向柱积分，不声称恢复像素内部的未知曲面，不使用“视线与基准面相交”足迹代替真实观测足迹。结果明确标记 `areaModel=observed_depth_orthogonal_footprint` 与 `volumeModel=center_signed_height_times_projected_cell_area`。足迹退化会明确拒绝。

相机 fx=fy=120、基准面 z=1m 时，深度恰为 1m 的参考单元面积为 1/14400 m²；观测 Z=1.02m 的每像素面积为 1.02²/14400 m²，42 像素凸起体积为 42×.02×1.02²/14400 m³。观测 Z=.985m 的凹陷则使用 .985²/14400 m²。正交映射下深度不改变 XY 单元尺寸，两种平面水平足迹一致。永久回归另外以 Z=2、fx=fy=100 的 3×3 观测证明面积为 .0036 m²，不能低估成基准视线面积 .0009 m²；法向 [0,3,4] 的倾斜基准面还验证实际法向投影面积和体积。

长度/宽度是支持像素完整四角足迹在平面基底上的最小面积矩形边长，单位 m；面积 m²、体积 m³。每个连通缺陷以首个实测支持点在基准面上的投影建立局部原点，四角在double中先减该原点，再转float调用最小面积矩形；拟合后的中心重新加回该局部原点。这样平面内的世界坐标偏移不会先量化角点并缩小尺寸，既有有限/计算范围校验和double面积/体积积分仍保留。四属性分别启用，任一严格大于阈值即缺陷；相等不判超限。结果 `inspection.Defects.v1.items` 保留每个候选的物理量、原支持量及超限项，标准 Poses3D 与记录 ID 对应，不授予 pickable。

判阈值和积分使用 double 有符号高度，f32 height 图仅是原生显示/后续图像载荷，不能复读量化图来降低物理积分精度。

默认只纳入 observed，`includeInterpolated=true` 才显式纳入补值，并单独统计。`minCoverage` 约束区域内有效覆盖率：有已证缺陷为 defect；覆盖足够且无缺陷为 clear；缺少观测时为 unknown，good=null，不能把空区域判为合格。`valid` 单独表示覆盖是否完整；即使覆盖不足也可报告已有支持证明的 defect。

`depth.defects@2.0.0` 交付 `candidates`、掩膜、有符号高度图和叠图。`distanceThreshold` 在这一步定义候选支持。`depth.classify_defects` 接收同一份 `candidates`，可配置不同尺寸限值与覆盖率，生成 `inspection.Defects.v1` 和带判定标记的位姿集合。

## 空间圆

自动 Canny 取图像强度中位数生成阈值，手动模式明确使用 low/high。独立弧段按三点方向偏差和长度比断开，使用相隔三像素的方向减少像素阶梯干扰。短弧两两形成有界假设，再只吸收几何兼容弧；不把整张图所有边缘作一次全局拟合。`edgeTolerance` 为像素合并/重复边界容差。最小二乘椭圆以角度分箱覆盖、径向支持比例和 20° 局部切向一致性共同评分，稳定去重后按原图 y/x 排序。完整边界和分离弧片均有正例，矩形与空背景有反例。二维圆只在偏心率小于 circleThreshold 时输出，椭圆轴长、二维圆半径、已知物理圆半径分别命名，不混用直径。

边缘点使用现有零 skew 相机 K 和 0/4/5/8/12/14 项畸变标定去畸变到归一化平面，再拟合椭圆二次型。反畸变复用共享有界 Newton/线搜索射线函数，必须通过原 K/D 正投影的像素残差验证；不是默认五次迭代后无条件接受。各点共用正投影工作预算并检查取消，不能收敛则明确诊断。对其圆锥矩阵作对称特征分解，构造限制为圆的平面截面；中心由 Q 逆作用于法向并按已知半径定尺度。透视下两组合法截面均保留，不能根据绘图顺序选出一组冒充唯一真值。法向采用正相机 Z 的无向圆约定；验证时 n 与 -n 表示同一个圆平面。正视退化为唯一截面时去除重复解。

输入 `radii` 为可选一维米制 Tensor，可单值广播或恰好对应稳定检测顺序；数量不匹配明确诊断，不按 min(N,radii) 静默丢弃。无端口时使用 `radius` 参数。每个解交付标准 Poses3D，localZ 与 normal 一致，size=[2r,2r,thickness]，attributes 包含 shape=circle、radius/circleRadius、normal、ellipseId、ambiguous 与径向误差，便于后接真实圆盘投影。`geometry.Circles3D.v1` 保留两解、实际半径和径向质量。已知半径对应实际观测边界；有厚度黑环的内外边界半径不同，不能把不可观测的环中线半径强加给任一边界。物理回归使用按视线与三维圆盘相交生成的实心孔，真值直接描述该可见边界。

支持 u8/u16/f32 灰度、RGB、RGBA；f32 要求有限 0..1。叠图保留原位深、通道和 alpha，Canny 显示则明确输出 u8 单通道。输入只读。结果数量超限报 resource_limit，不静默截断。

## 轮廓截面

旋转区域是同 imageFrame、px 单位的 Pose2D；缺省为完整栅格。其四角先与基准面求交，按区域局部 x/y 排布指定数量截面。每条截面是包含基准面法向与横向轴的真实三维切平面。

组织栅格顶点通过 indexMap 引用源云，构成有限三角面；`meshPixelDistance=1` 默认不跨缺测像素，显式增大可连接源模式允许的短缺测间隔，`maxEdgeLength` 限制跨深度跳变的物理边长。三角形与切平面求真实边交点，共享端点去重并按连通链组织；梯度不能跨不同链。平面无交、兴趣区域无点、平坦区域没有有效梯度时，属性有效性为 0、结果 unknown，而非沿用上次特征或零值判 good。

`profiles` 为 `[M,4]` 原生 Tensor，列为 sliceIndex/x/height/chainId；`attributes[S]` 与 `attributeValid[S]` 配对，无效位置的数值 0 不是测量结果。`sliceCloud` 是三角交线生成的新点云，`indices/groups` 绑定它的新身份；`supports[M,3]` 则指向父输入 cloud 的三个支持顶点。两种身份不能交换使用。

`depth.profile_slices@2.0.0` 的 `profiles` 与 `sliceInfo` 可以同时连接多组特征测量。特征选择与绘图读取公开的 float32 截面坐标。原有截面交点和父三角输出沿用。`depth.classify_profile_features` 消费测量节点的 `features`，以闭区间判断各切片。存在有效的超限测量时，汇总为 `ng`。全部切片有效且在范围内时为 `good`。其余情况为 `unknown`。

旧图的离线请求示例

```json
{
  "schemaVersion": 1,
  "rules": [
    {"kind": "depth.defects.v1", "source": {"node": "defects"}},
    {"kind": "depth.profile_slices.v1", "source": {"node": "profiles"}}
  ]
}
```

将来源节点 ID 改为旧图中的 ID，保存为 `upgrade.json` 并运行 `lyflow migrate <图文件> --request upgrade.json --write`。旧连线、图参数绑定、图级输出改接到对应的测量或判定端口。

交点是插值几何，不声称实测；`features.derivation` 和 measured=false 明示。intensity/RGB 使用源三角重心插值，RGB 四舍五入；normal 重心插值后归一化，抵消时选择最近非零源法线，全部不可用则输出零向量并统计 unresolvedNormalCount，不伪造方向。输入所有可选通道在交点云中保持对齐。截面图和曲线图仅作几何显示，不依赖字体渲染。

## 盒体采样

输入标准 Poses3D 中心锚定尺寸，所有尺寸为米。`sampling=surface` 每个可见面生成 pointsPerFace 个均匀点；`volume` 生成六倍数量的内部点，不存在表面法线或可见面概念，必须关闭 normals，不能同时启用 visibleOnly。线框单独输出，按 wireSpacing 均匀分段并包含精确端点，交汇处可重复角点，不是 Mesh 拓扑。

`coordinates=pose` 将局部点和法线变换到工件父帧；`local` 显式标为 box_local，quality 逐组保留 sourcePose。不同工件局部组不是统一世界坐标，消费者需按组应用其 sourcePose。每份新云各有独立原生 Indices/Groups，组 ID 保留输入盒体 ID。

可见性按面外法向与面中心到 viewPoint 的视线夹角判定，minVisibleAngle 限制非掠射程度；默认观察点为源父帧原点。颜色和法线显式控制，确定性 MT19937 种子避免重复运行漂移。质量记录 measured=false，不把合成表面作为传感器测量。

f32 交付前验证八角点互异，角点及实际采样点的量化误差不得超过最小边长的 1e-4；超限返回 precision_loss，而不是把远距离小盒体塌成一个点仍报成功。此限制作用于所选输出坐标；大世界偏移下无法保真的 pose 模式，可以显式选择保留 sourcePose 的局部坐标。合法属性中的 null 原样保留，不误判成资源超限。

## 预算、取消和永久回归

组织检测输入最多 2Mi 像素/源点，空间圆最多 2Mi 像素、4096 轮廓/候选和 128 最终椭圆，盒体最多 128 个和累计 1M 点，截面最多 1024 条和 1M 交点。图像检测累计工作量不超过 100M，形态学窗口、邻域、轮廓、三角/切平面、逆畸变正投影次数和输出整理成本共同计账；超限明确拒绝，不交付半份结果。输入校验、行/块、候选和输出发布前检查取消，五个新算子统一返回 code=cancelled 且不发布端口；正常无目标或空集合仍返回成功。调度器的运行取消事件继续由 Run cancelFlag 决定，不修改范围外旧算子的取消惯例。

永久回归包含原观测不变/身份和可选通道、负值/零正交高度、孔洞边界和跨层不补、严格阈值、四属性和末行末列真实面积、缺观测 unknown、三角截面解析高度/跨度、六特征与 x/y、旋转区域、孔洞分链、真实投影空间圆两解及无目标/矩形反例、RGBA alpha、非法半径/映射/帧、预算与取消，以及确定性变换盒体、可见面和体积模式。

缺陷足迹精度回归使用同一1×1正交像素、分辨率0.0001220703125m和XY平移0/1024m，只启用长度阈值0.0001m。旧实现于1024m偏移将边长缩半并错误判clear，精确红证16个断言失败；局部拟合后两组均判defect，长度/宽度、面积/体积、世界中心与全部四角满足原真值，输入不变。独立检查/盒云探针18组6815断言全部执行通过、0跳过，日志build/p5-inspection-native/footprint-red.log与footprint-green.log；最终共享调度器/SDK门禁由主集成另行执行。
