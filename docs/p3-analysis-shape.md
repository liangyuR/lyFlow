# B04 梯度形状匹配分析与实现边界

## 参考语义

逐项阅读外部 `SBMEstimator.cpp`、对应头文件、`property_settings.yml` 中运行参数和训练参数。参考代码属于专有实现，本仓库只分析其输入、输出和业务意图，算法函数体独立编写。给定源码目录不包含 `xyz_imgproc/matching/shape_based_matching.h` 对应底层实现，不能声明兼容其训练文件或复现其融合策略。

原模块包含 CAD 透视/正交投影、顶部裁剪、形状模板训练与文件持久化、按目标列表加载模型、梯度检测阈值、角度与尺度搜索、多实例 NMS、可选 ICP/融合、ROI 坐标恢复以及形状点/得分叠图。原代码依赖外部全局相机、工作空间和可变文件目录；模板路径缓存、重新训练副作用和 ROI 掩膜尺寸等必须与无状态图执行分离，不能直接搬入运行库。

本阶段用显式节点组合表达：CAD 经现有 STL 点云采样、坐标变换、深度投影与有效范围掩膜形成离散投影图，再建立模型；普通图像可直接建模。CAD 路径是采样点云投影，不宣称连续三角网格光栅、专有数据集兼容或自动推导工作空间。多模型由多个 `image.shape_match` 节点并联，结果各带模型标识；不伪造单个节点的多类别融合开关。

## 算法

`image.shape_template` 把灰度归一化为 0..1，在有效掩膜内归一化平滑，提取 float32 Sobel 梯度、沿法向非极大抑制与亚像素抛物线峰值位置，再做确定性最远点采样。图像强度只用于提取边缘和法向；搜索不使用灰度 NCC 或亮度模板相关。掩膜外像素不生成模型点，也不会把掩膜边界当作物体轮廓。

`image.shape_match` 对每个显式角度/尺度组合进行梯度方向兼容的中心投票；对局部峰值检查全部模板特征与有效场景边缘之间的定向 Chamfer 距离，按覆盖率、距离与法向相似度验收，再以有界坐标搜索精化 x、y、角度、尺度。候选只在声明搜索范围内精化。最后按旋转矩形 IoU 做 NMS。遮挡特征仍计入分母，不能用少量局部相似片段伪装成完整目标；超出图像的模型特征同样记为未匹配。

覆盖率是满足距离和法向门限的特征数 / 全部模型特征数；RMS 是匹配特征的欧氏距离均方根。score 是全部特征上的 `法向相似度 × max(0, 1 - distance/maxDistance)` 均值，未匹配项为 0。`confidence` 使用同一 score，是几何算法质量，不是分类概率。忽略极性时法向相似度取绝对值，仍要求边缘方向一致。

## 数据与坐标契约

`image.shape_template` 接收 image，输出 features、model、image。features 是 float32 Tensor，形状 N×4，依次为相对模型中心的 dx、dy 和单位梯度 nx、ny。dx/dy 始终是原图像素，Tensor 不依赖引擎的图像缩放规则。模型中心是已采样特征轴对齐包围区域的中心，size 为其跨度加一个工作像素换算到原像素；它不冒充 CAD 的物理重心或尺寸。

`vision.ShapeTemplate.v1` 仅包含 method、id、frame、unit、imageSize、center、size、featureCount、featuresHash。方法名为 `oriented-chamfer-v1`，单位为 px。featuresHash 是按 little-endian float32 数值字节计算的有界几何内容摘要，仅用来检查小元数据与大 Tensor 配对；它不是产品、模型文件、运行库或源码版本的身份指纹，也不是安全认证。Tensor 当前没有原生 id，不编造身份字段。

训练交付与模型读入还独立校验真实几何：至少12个相互分离0.25原像素的特征、XY各至少2原像素跨度，以及归一化协方差的非共线条件。实际特征极值须围绕声明中心对称，size减特征跨度须为两个方向一致的正整数工作像素宽度；据此复核原训练图尺寸、工作像素预算及声明包围范围。float32特征量化只使用随范围变化的舍入容差，不降低匹配残差或覆盖门限。即使重新计算正确摘要，重复点、共线点或与实际特征范围不一致的元数据仍报bad_input，且不发布任何输出。

模型可通过保留原训练图、掩膜和建模参数确定性重建。现有工作流只保存节点和参数；不要把未实现的 Tensor 文件格式称作可持久化模板资产。模型图像帧与待搜索图像帧可以不同，因为模型特征是局部形状；输出帧由 match.frame 明确声明。

两个节点均可接 u8 单通道 mask，以及互斥的像素 Box2D roi 或 `vision.Pose2D.v1` region。region 使用原图像素并显式除以输入 Image.scale；Box2D 由引擎按已有像素几何规则转换到节点比例。帧与单位需匹配。几何 ROI 与图像无交集报 bad_input；合法的全零 mask 在匹配时返回成功空结果，在训练时返回 degenerate_template。任意 ROI 均不回退全图。

搜索区域同时约束粗候选、每次精化试探和最终输出中心。亚像素中心按保守连续掩膜判断：其floor/ceil邻接工作像素全部有效才允许；整像素中心只检查自身。因此精化不能通过最近像素取整跨过ROI或掩膜边界。ROI限制中心并限制可用于打分的场景边缘，不要求整个匹配框都在ROI内，框外及被遮挡特征仍按未匹配计入覆盖率分母。

结果 instances 为原图像素 `vision.Poses2D.v1`，角度为图像 x 向右、y 向下坐标中的正向旋转，pickable=false。quality 为 `image.ShapeMatches.v1`，含每个结果的模型标识、尺度、分数、覆盖、RMS、支持数，以及实际变体数、峰值数、检查数和返回数量截断标识。image 是保留源图位深、通道和 alpha 的旋转框/特征叠图，edges 是有效区域内的 u8 候选边缘；两个输出 Image.scale 均与输入一致，源图只读。

## 参数

| 节点 | 参数 | 默认与范围 |
|---|---|---|
| template | id/frame | shape/image，1..256 字节 |
| 两者 | minContrast | 0.04，(0,1] 归一化梯度 |
| 两者 | smoothSigma | 0.8，原图 [0,8] px |
| template | maxFeatures | 256，[12,1024] |
| template | featureSpacing | 2，原图 [0.25,64] px |
| match | angleMin/angleMax/angleStep | -180/180/5 度，范围 [-180,180] |
| match | scaleMin/scaleMax/scaleStep | 1/1/0.1，尺寸比例 [0.1,4] |
| match | spatialStep | 2，原图 [0.5,16] px，工作网格最小 0.5 px |
| match | maxDistance | 2，原图 [0.25,64] px |
| match | orientationTolerance | 30，[0.1,90] 度 |
| match | ignorePolarity | true |
| match | minCoverage/minScore | 0.6/0.45，范围 [0.05,1]/[0,1] |
| match | maxRmse | 1.5，原图 [0.1,64] px |
| match | maxDetections | 32，[1,256]，NMS 后返回上限，quality 明示 limited |
| match | nmsIou | 0.3，[0,1] |
| match | maxCandidates | 4096，[1,16384]，峰值预算，超限报错 |
| match | refineIterations | 8，[0,16] |

## 支持矩阵与资源

灰度、RGB、RGBA 均支持 u8/u16/f32，RGB 遵循既有通道顺序，f32 所有通道必须为有限 0..1。matcher 工作在已提供的像素坐标系，不消费或忽略相机 K/D；如需去畸变，先显式接本阶段标定/去畸变节点。没有标定时不输出米制位置。

输入上限 8Mi 像素/128MiB，输入与处理临时内存按 512MiB 峰值保守预检。边缘最多262144点，中心投票/支持检查共享256Mi操作预算，角度×尺度最多721项，超预算显式 resource_limit，输出原子提交。maxFeatures 是明确的模型采样配置；maxCandidates 是失败边界，不能用于偷偷丢掉未检查的目标。maxDetections 是用户请求返回上限，截断在质量记录内明确。记录遵循既有1MiB/深度16约束。

长循环、每个变体、每轮精化检查取消。OpenCV 的单次滤波调用内部不承诺可抢占；本节点没有隐藏后台进程或固定取消毫秒保证。

独立 Windows MSVC19.51/OpenCV4.12 探针实际执行5组303断言，全部通过且0跳过。测试使用合成有孔 L 形轮廓，覆盖旋转/尺度真值、遮挡、对比反转、多个实例、旋转 NMS、形状干扰/无目标、ROI 与模型配对、位深和 alpha、0.001幅值的低对比F32（不量化为u8）、非搜索网格上的角度/尺度精化、预览坐标、取消和预算。最终执行结果以本阶段构建日志为准；这些不是工业相机实拍、长期稳定性、抓取安全或现场误报率验收。
