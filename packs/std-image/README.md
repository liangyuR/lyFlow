# packs/std-image

图像域的标准算子包（[docs/当前指南](README.md) 阶段 2，[ADR-0026](../../docs/adr/0026-image-data-domain.md)）。
默认开，依赖 vcpkg 的 OpenCV 4（core / imgproc / imgcodecs / calib3d）：

```powershell
C:\vcpkg\vcpkg.exe install opencv4:x64-windows
```

## 边界（照 ADR-0005）

1. `core/include/` 下零 OpenCV 头；公共数据模型是 core 的 `lyflow::Image`（行主序、通道交错、行紧排、RGB(A)）。
2. `cv::` 只出现在这个包的 `ops/`、`src/` 与私有的 `include/lyflow_cv/`。
3. 进出 OpenCV 一律经 `lyflow_cv/adapter.h`：
   - `view` 零拷贝、只读；
   - `fromMat` 零拷贝交给 core（别名 `shared_ptr` 持有 Mat）；
   - BGR ↔ RGB 只在 `io.load_image` / `io.save_image` 里转。
4. 尽量出掩膜、几何与测量，少出「又一张彩色图」。几何是**像素坐标**，`unit = px`（image-plan Q2）。

文件读写不用 `cv::imread` / `imwrite`：它们走窄字符串路径，中文路径打不开。
这里用 `std::filesystem` 读写字节，再交给 `imdecode` / `imencode`。

## 算子

| 算子 | 做什么 |
|---|---|
| `io.load_image` / `io.save_image` | PNG / JPEG / BMP / TIFF；u16 保留，f32 只能存 TIFF |
| `image.to_gray` | RGB(A) → 单通道（BT.601），已是灰度的原样传下去 |
| `image.resize` | 按倍数或尺寸；缩小默认区域平均 |
| `image.crop` | 像素矩形，或接一个像素单位的 `Box2D` |
| `image.blur` | 高斯 / 中值 / 均值 |
| `image.normalize` | 位深转换：按通道拉满量程，或 × scale + offset |
| `image.threshold` | 固定 / Otsu / 自适应 → u8 掩膜；输入契约 `shape: [-1, -1, 1]` |
| `image.morphology` | 腐蚀 / 膨胀 / 开 / 闭 |
| `image.find_circle` | 霍夫找圆，取最强的一个 → `Circle2D`（px） |
| `image.region_stats` | 掩膜内的均值、面积、外接框（px，可接 `image.crop`） |
| `image.convert_channels` | 灰度 / RGB / RGBA 互转，保留位深，补入的 alpha 为满量程 |
| `image.arithmetic` | 同尺寸、位深、通道的加 / 减 / 绝对差；整数饱和，浮点保留符号 |
| `image.change_stats` | 差异阈值、可选十字开运算、最小变化面积判定与统计 |
| `image.difference_mask` | 正 / 负 / 双向差分、共同边缘抑制、可选有界平移配准 |
| `image.range_mask` | 灰度 / RGB / HSV 范围掩膜，支持色相环绕和反选 |
| `image.roi_mask` | 矩形、旋转矩形、圆、圆环弧和多边形的显式掩膜 |
| `image.statistics` | 掩膜内指定通道的最小 / 最大 / 均值 / 中位数 / 众数 |
| `image.components` | 连通域、孔洞、像素面积、质心、旋转框、长宽比及有界轮廓 |
| `image.edge_positions` | 全 ROI 均值投影后选择单边、内外双边或多个亚像素梯度峰 |
| `image.line_caliper` | 平行扫描、极性边缘、亚像素定位和有质量门限的鲁棒直线拟合 |
| `image.width_range` | 外侧极端边对 / 中心邻近内侧边对的宽度最小、最大和均值 |
| `image.to_tensor` / `tensor.to_image` | 推理前后的显式转换：scale、mean/std、NCHW 等布局 |
| `cloud.from_depth` / `cloud.to_depth_image` | 深度图 ↔ 点云（针孔内参，x 右、y 下、z 前，米）：无效深度不出点，可选彩色图上色；投回去按 z 缓冲取最近。同一组内参、同一个 depthScale 时两者互逆（[当前指南](README.md) §4.1）。不用 OpenCV |

测试在 `tests/test_image_ops.cpp`，图一律现造（合成渐变、`cv::circle` 画的圆），仓库不进图片。

## 通用区域与量测契约

新增算子的回归在 `tests/test_image_p0.cpp`。颜色顺序始终是 RGB(A)，不隐式按 BGR 解释；u8/u16 的强度满量程分别为 255/65535，f32 的 HSV 颜色转换约定输入 0..1。新算子不改写输入像素。需要灰度的卡尺和连通域使用单通道输入，彩色图先显式连接 `image.to_gray`；统计则显式选择 `channel`，不会偷偷转换灰度。

| 能力 | u8 / u16 / f32 | 数值与边界规则 |
|---|---|---|
| 通道转换 / 图像算术 | 三种均支持，1/3/4 通道 | 转换不改变位深；整数加减饱和，浮点不裁零，非有限值继续保留，由后续统计/掩膜排除 |
| 范围掩膜 | 三种均支持 | Gray 仅用 lower/upper 的第一个分量；RGB 用原强度，HSV 用 H=0..360°、S/V=0..1，H 下界大于上界表示跨零环绕；闭区间，HSV 转换比较容差为 H 的 1e-5°、S/V 的 1e-6；反选仍排除 NaN/Inf |
| 统计 / 变化 | 三种均支持 | 掩膜有限非零处有效；NaN/Inf 不参与；变化忽略 alpha，可选 BT.601 亮度差或最大通道差；`different` 按开运算后原图像素面积严格大于 `minArea` 判定 |
| 连通域 / 边缘 / 卡尺 | 三种均支持，单通道 | 非零有限前景；梯度阈值使用输入原始强度单位，未统一除以 255；建议按实际位深设置阈值 |
| 抗边缘差分 | 三种均支持，1/3/4 通道 | 两输入尺寸/通道/位深必须相同，不隐式裁交集；彩色按 RGB 亮度；可选有界平移配准，不含旋转/尺度配准 |

`image.statistics` 输出 `image.Statistics.v1` Record：`valid`、`sampleCount`、`invalidSamples`、`channel`、`scale`、`min/max/mean/median/mode`。空区域 `valid=false`，五个统计字段为 null；选择该 null 字段做数值运算应报诊断，而非当作零。u8/u16 分别以 256/65536 桶累计直方图，f32 对有界有效值排序；众数按精确数值，次数相同取较小值，中位数取排序后中间两数的均值。浮点众数不做隐藏分箱。

`image.change_stats` 输出 `image.Change.v1` Record：`sampleCount`、`changedSamples` 为开运算前采样统计，`filteredChangedSamples` 为滤波后采样变化数，`different` 为面积判定布尔值。`changedFraction`、平均绝对差、RMS 和最大差按未滤波有效像素计算。空 ROI 的数值统计为 null、`valid=false`、`different=false`；可通过 `flow.select` 的 Record 字段路径读取 `different`。

`image.components` 输出 `image.Regions.v1` Record：`count`、`regions`，每域包含 `id`、`area`、`centroid`、半开 `bbox`、`aspectRatio`、`rotatedBox` 和可选 `contours`。面积是实际前景像素数，孔洞不计入，**不是轮廓多边形面积**。旋转框是像素中心轮廓的最小框，单像素/线状域可能有零边长，长宽比在退化时为零。`scan` 使用 SAUF 行优先标签；面积降序保持相同面积的标签顺序。默认超出 `maxRegions` 报资源诊断；仅显式开启 `takeLargest` 时按所选排序截取前 N 项。该 Record 不是工件 Pose2D 或相机标定数据。

P0 图像分析领域 Record 坐标显式写 `coordinateSpace=source`、`unit=px`、`scale`，几何坐标和面积已换算至原图；`sampleCount` 等采样数量不冒充全分辨率真实计数。原生 Line2D/Measurement 则按执行器的现有像素缩放契约换算，不能再次手动缩放。预览的插值、降采样可能改变小目标和边缘，正式量测须跑全分辨率。P1 的相机、平面、工件位姿和模板各自使用下述版本化契约，不混用图像统计 Record。

ROI 掩膜保持输入图像尺寸，不携带外部 ROI 父层/工件位姿等隐式元数据。矩形使用半开 `[x0,y0,x1,y1)`；圆和旋转矩形按像素中心判定；正角从 +X 朝 +Y。圆环弧角度跨零有效，跨度至少 360° 为完整圆环。多边形 `points` 为原图坐标的 JSON 点数组，使用整数像素栅格化。所有形状最终与可选输入 mask 相交；`invert` 先反选几何，再相交，不会反选出 mask 外的像素。

直线卡尺的 `axis` 指扫描轴，`direction` 定义极性方向，`selection` 可选首/末/最强峰。梯度以中心差分计算，峰顶用抛物线亚像素细化；先 Huber 拟合，再按 `fitTolerance` 筛内点并重新拟合，低于 `minValidFraction` 或退化时返回 `edge_not_found`，不生成假直线。`position` 是沿扫描轴的平均边缘位置。

投影边缘定位把整个 ROI 横向平均后取峰，与多条卡尺拟合不同；无目标有效返回空位置数组。宽度的 `outer` 模式取最左开边与最右闭边；`inner` 模式反转极性，取 ROI 中心两侧最近边。输出沿采样轴的宽度，非任意旋转形状的全局最窄宽度；无足够有效边对时返回 `ok=false`、NaN 的 Measurement 和可诊断样本。

抗边缘差分先按配置做平移对齐，再输出正/负/绝对差阈值掩膜；Canny 距离抑制仅移除同时靠近两幅图边缘的差异，保留单幅图新出现的边缘。配准响应或位移不合格返回 `alignment_failed`，不静默退回无配准差分。其首版不宣称与外部 ORB/直接法的旋转及尺度模式等价。

## 资源限制

- 新增基础分析/卡尺输入不超过 `16 × 1024²` 像素；精确 f32 全统计、连通域及抗边缘差分不超过 `4 × 1024²` 像素。预算限制是显式诊断，不静默降采样。
- 连通域最多 4096 项，局部包围框扫描总和最多 `32 × 1024²` 像素，推导轮廓共最多 262144 点，导出轮廓共最多 32768 点，并受每域 `maxContourPoints` 约束；Record 最多 1 MiB。只在局部标签框取轮廓，嵌套/重叠包围框同样计预算。
- 卡尺最多 256 条、平均带宽最多 31；扫描数量 × 扫描长度 × 带宽不超过 `32 × 1024²`。投影边缘输出最多 256 峰。非矩形 ROI 循环、逐像素统计/扫描和逐域处理轮询取消；OpenCV 单次调用由上述规模上限限制，不宣称可抢占其内部线程。
- 多边形 JSON 最多 64 KiB、4096 点；不支持通过图像 Record 嵌入大数组、图像字节或进程指针。

## 相机输入与工作平面标定

`io.load_image` 默认 `source=file`，保留文件读图行为。`source=inputs` 时接收可选 `image` 输入（上游连线或宿主 `RunImageInput`），不访问 path；mode 照常控制原样 / 灰度 / RGB，位深保留。

`image.board_calib` 从全分辨率 u8 棋盘图求 `image.PlaneCalib`。pattern 是 3–100 之间的整数内角点数，square 是格长 mm。输出 H（行主序 3×3）、unit、rms、maxError、mmPerPx、pattern、square、imageSize。

`image.load_calib` 从 JSON 读取整个 `Record` 或 data 对象，验证类型、有限矩阵与非奇异性，支持中文路径；文件大小或修改时间变化后重读。量测算子连接 calib 后按 H 映射两点的距离，未接时使用 px。

## P1 图像、标定与定位

逐项参考源码和优化选择见 [P1 图像分析](../../docs/p1-analysis-image.md)。本切片按清晰输入/输出拆分能力，保留外部设备协议、任意形状工件mask、固定半径圆拟合、直径模式及形状匹配等边界。

| 算子 | 契约 |
|---|---|
| `image.concat` | 两图水平/垂直排布，显式填充值，同位深/通道/比例；多图可组合。 |
| `image.color_adjust` | u8/u16/f32灰度/RGB/RGBA的亮度、对比度、Gamma、HSV和RGB增益/偏移；alpha保持。 |
| `image.shading_correct` | 显式平场或高斯背景，除法/差分校正；小分母失败，不生成无限增益。 |
| `image.instances_mask` | 像素Pose2D旋转矩形→u16标签/u8二值图/稳定ID表，重叠策略显式。 |
| `image.instances_morphology` | 每个实例独立形态学，再按first/last/error合成；膨胀可保护原标签。 |
| `image.load_camera_calib` | 1MiB/深度16内的 `vision.CameraCalib.v1` JSON，中文路径，严格格式。 |
| `image.undistort` | 相同全分辨率的K/D去畸变，输出零畸变相机记录及完整采样有效mask；映射缓存按K/D/尺寸失效。 |
| `image.warp_perspective` | 平面H、物理原点、像素密度和固定输出尺寸的重采样，输出有效mask及映射记录。 |
| `image.calibrated_distance` | 原图像素Point2D经平面H求米制距离。 |
| `image.board_pose` | 棋盘角点/显式角点+相机K/D→三维PnP、实际float32输出矩阵的重投影RMS、正深度与候选歧义门控。 |
| `image.template_build` | 原生灰度模板与 `vision.NCCTemplate.v1` 小型元数据，拒绝常量模板。 |
| `image.template_match` | 掩膜NCC、离散角度/尺度、稳定有界候选及旋转框NMS，返回统一Pose2D列表。 |
| `image.circle_caliper` | 圆/弧径向卡尺、亚像素边缘、确定性RANSAC及SVD，验证内点、RMS和曲率证据。 |
| `image.ring_width` | 径向亮/暗环带的周向min/max/mean宽度；有效比例不足时Measurement无效。 |

相机记录保存 `imageSize:[w,h]`、行主序 `cameraMatrix:[9]`、畸变系数 `distortion`、`frame`、`unit:"m"`；支持零skew针孔K和0/4/5/8/12/14项畸变。它不包含机器人手眼外参，也不等于 `image.PlaneCalib` 的像素→m/mm平面H。图像尺寸已知时禁止默默套用另一分辨率的标定。

实例使用 `vision.Poses2D.v1`，位置是中心、角度为从+X朝+Y的弧度。实例标签 `vision.InstanceLabels.v1` 保存原图尺寸、帧、单位及ID顺序；原生u16标签图0为背景、1..N对应ID，最多4096项。像素与大型数组始终走原生Image，不进入Record。

实例标签的id/frame与Pose共用64KiB字符串、1MiB Record总限额，掩膜生产与形态学消费遵循同一边界。

NCC最多81种角度/尺度组合、64Mi响应像素和4096个有界候选；常量搜索区域不会被归一化成满分匹配，旋转画布padding不计分。结果位置使用像素中心，尺寸反映实际整数重采样，attributes保留请求和实际尺度。无匹配返回有效空集合，不等于错误。

圆弧质量Record中的坐标恢复到原图，原生Circle2D和像素Measurement仍由执行器恢复。圆拟合要求圆RMS显著优于同点集直线RMS，以拒绝窄ROI内直边误拟合为圆；径向梯度平台合并后取中心，避免正反向选择造成半像素环宽偏差。扫描、RANSAC、实例处理和候选搜索均有资源上限及取消检查。物理精度和现场重复性仍需真实标定与样本验收。

圆弧和环宽的 `radialStep` 必须位于原图 `[0.25,4]` 像素，预览按图像比例同步缩小步长与扫描范围，校验仍以还原后的原图步长为准。每条径向采样上限及64Mi总采样预算在Full/Preview中一致执行。

## P1 YOLO 检测链

`image.yolo_preprocess → ml.onnx_run(modelContract=yolo-detection) → image.yolo_decode` 支持 YOLOv8、YOLO11、YOLO26。预处理为 u8 灰度/RGB/RGBA→RGB/255、居中 letterbox；modelFamily 与模型必须匹配，classNames 可显式设置，静态输入宽高为 32 的倍数、32..2048。常规头 `[1,4+C,N]` 无 objectness、按模型空间 IoU 做稳定类别感知 NMS；端到端 `[1,N,6]` 不重复 NMS。NMS 后回映并裁剪到原图/ROI，零面积框不输出。

YOLOX-Nano 使用独立 `image.yolox_preprocess/decode` 与 modelContract=yolox-nano-0.1.1rc0：BGR 0..255、416²右下补114、网格与objectness解码。模型映射 Record 与原生 Tensor 必须配对；不猜测转置、动态、FP16、分割或 OBB 导出格式。结果为像素 `vision.Poses2D.v1`，空目标为有效空集合，pickable=false。候选与输出最多4096，Tensor最多16Mi元素。

四个真实模型的签名、许可、准备命令和示例见 [检测示例](../../examples/p1-detection-README.md) 与 [逐项分析](../../docs/p1-analysis-detection.md)。权重由用户显式准备，不随运行库分发。
