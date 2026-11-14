# xyz-vision-lib 算子清单

Nov 13, 2026 · @Liangyu Ran

## 概览

xyz-vision-lib 当前（master，提交 72d7885c）共注册 **178 个 C++ 算子节点**，另有 4 个预置组节点和 16 个内置 Python 脚本，合计 198 个可在流程中拖用的节点。

| 类别（界面分类） | 算子数 |
| --- | --- |
| 3D点云处理 | 56 |
| 2D图像处理 | 40 |
| 位姿处理 | 24 |
| 输入 | 15 |
| 流数据运算 | 14 |
| 输出 | 9 |
| 可视化 | 8 |
| 深度学习 | 7 |
| 检查 | 5 |
| 预置组节点（Custom Group） | 4 |
| 内置 Python 脚本 | 16 |

分类按界面显示的 `category`（取自 property\_settings\_zh.yml），与源码目录不完全一致，差异见各表的“源码目录”列。

## 算子清单

C++ 算子按界面分类列出，名称与简述取自 property\_settings\_zh.yml / property\_settings.yml；“源码目录”只在与该分类的默认目录不同时标注。

### 输入（15）

默认目录：models/Input

| 类名 | 中文名 | 英文名 | 简述 | 源码目录 |
| --- | --- | --- | --- | --- |
| CloudLoader | 加载点云 | Load Cloud | 加载指定路径下的点云或模型，支持3种模式。 |  |
| GenerateGeometry2D | 生成2D几何特征 | Generate 2D Geometry | 生成2D几何特征，包括点，线，和圆。 |  |
| ImageLoader | 加载图像 | Load Image | 加载指定文件路径的图像。 |  |
| InputInfo | 输入信息 | Input Info | 接收输入信息，可从参数中读取，也可以是客户端发送的信息。 |  |
| InputNumber | 输入数字 | Input Number | 输入一个常数。 |  |
| InputPlane | 生成平面 | Input Plane | 生成一个平面。 |  |
| InputPrimitive2D | 生成2D位姿 | Input Primitive2D | 生成一个2D位姿。 |  |
| InputPrimitive3D | 生成3D位姿 | Input Primitive3D | 生成一个3D位姿。 | Primitive\_Processing |
| LoadCalibrationInfo | 加载标定信息 | Load Calibration Info | 加载标定文件中的标定信息。 |  |
| LoadCameraCalibInfo | 加载相机标定信息 | Load Camera Calibration Info | 加载标定文件中的相机标定信息。 |  |
| LoadWorkspaceInfo | 加载工作空间标定信息 | Load Workspace Calibration Info | 加载文件中的工作空间标定信息。 |  |
| ProfileProjector | 轮廓仪投影 | LaserProfile Projector | 将一台激光轮廓仪（从）的点云投影到另一台（主）坐标系。 |  |
| XYZCamera | 相机 | Camera | 相机模块，可用于连接相机拍照。 |  |
| XYZLaserProfiler | 激光轮廓仪 | LaserProfiler | XYZ线扫：从相机或者本地获取点云数据。 |  |
| XYZLidar | 雷达 | Lidar | 从真实/虚拟雷达获取点云。 |  |

### 2D图像处理（40）

默认目录：models/2D\_Processing

| 类名 | 中文名 | 英文名 | 简述 | 源码目录 |
| --- | --- | --- | --- | --- |
| BarcodeDetector | 检测条形码/二维码 | Detect 1D/2D Barcode | 检测条形码/二维码。 |  |
| BaselineCalibration | 基准校正 | Baseline Calibration | 2D检测中相对于基准面的校正。 |  |
| BlobDetector | 斑点检测 | Detect Blob | 从二值图中检测白色斑点。 |  |
| CalculateArea | 计算区域大小 | Calculate Area | 计算黑白图片中黑色或白色区域大小。 |  |
| CalculateEdgePosition | 计算边缘位置 | Calculate Edge Position | 检测目标区域指定方向和变化趋势的边缘像素位置。 |  |
| CalculateGeometryDistance | 计算2D几何特征距离 | Calculate 2D Geometry Distance | 计算点、线、圆之间的距离。 |  |
| CalculateRingWidth | 计算圆环宽度范围 | Calculate Ring Width Range | 检测圆、圆环、圆弧类工件的最小和最大宽度。 |  |
| CalculateWidthRange | 计算宽度范围 | Calculate Width Range | 检测目标区域的最小和最大宽度。 |  |
| CalibBoardPoseEstimation | 标定板定位 | Estimate CalibBoard Pose | 识别图像中标定板的位姿。 |  |
| CheckImageChange | 检查图像变化 | Check Image Change | 检查两幅图像之间的变化。 |  |
| ConcatImages | 图片拼接 | Concat Images | 将多张图片拼接成一张。 |  |
| ConvertImageType | 转换图片类型 | Convert Image Type | 灰度与彩色互转。 |  |
| CropImageByROI | 图片ROI裁剪 | Crop Image By ROI | 使用ROI裁剪图片。 |  |
| DefectDetector | 深度图缺陷检测 | Defect Detector From Depth | 将深度图投影到产品平面，统计超阈值区域以检测缺陷（单位 mm）。 |  |
| DetectCircleEdge | 检测圆形边缘 | Detect Circle Edge | 检测目标区域的圆或圆弧边缘。 |  |
| DetectHighLights | 检测高光直线 | Detect HighLights | 检测横向高光直线并生成2D旋转框。 |  |
| DetectLineEdge | 检测直线边缘 | Detect Line Edge | 检测目标区域的边缘直线。 |  |
| EdgePoseRefiner | 配准工件2D边缘 | Refine Pose by 2D Edge | 使用工件2D边缘进行3D位姿配准。 |  |
| FindCircles | 圆孔检测 | Find Circles | 在2D图像中寻找3D圆（低离心率椭圆）。 |  |
| GeneratePrimitivesMask | 生成图像掩膜 | Generate Primitives Mask | 为每个输入工件生成图像掩膜。 |  |
| Geometry2DIntersection | 两几何体中点/交点 | Calculate 2D Mid-point/Intersection | 获取两几何体中点/交点。 |  |
| GetMaskFeature | 获取掩膜特征 | Get Mask Feature | 获取掩膜的特征。 |  |
| GrayScaleStatistics | 灰度统计 | Gray Scale Statistics | 统计灰度最小/最大/均值/中值/众值，可限定ROI。 |  |
| ImageColorAdjustment | 图像颜色调整 | Adjust Image Color | 2D图像颜色调整。 |  |
| ImageDifference | 图像差分 | Image Difference | 图像差分，可跟踪轮廓并抗噪。 |  |
| ImageFilter | 图像滤波 | Filter Image | 经典2D图像滤波算子。 |  |
| ImageHomography | 图像透视校正 | Correct Perspective Image | 用于斜拍场景的透视校正。 |  |
| ImageMorphology | 图像形态学操作 | Image Morphology | 膨胀、腐蚀、开、闭、实例膨胀。 |  |
| ImageResize | 图像缩放 | Resize Image | 对图像进行缩小或放大。 |  |
| ImageThresholdOperation | 图像二值化 | Image Threshold | 抽取彩色图某种颜色或对灰度图二值化。 |  |
| LoadForeground | 加载料箱区域信息 | Load Foreground | 加载料箱所在区域信息。 |  |
| MarkerPoseEstimation | 识别码定位 | Estimate Markers' Pose | 检测识别码并估计其相对相机的位姿。 |  |
| MultiViewMatcher | 多视角定位工件 | Multi-View Matcher | 多视角定位工件的3D位姿。 |  |
| ProfileSlicer | 轮廓截面器 | Profile Slicer | 在线扫深度图上截取连续截面并计算属性，常用于涂胶测量。 |  |
| QrCodeRefine | 二维码增强 | Refine QrCode | 二维码精致化以提升识别效果。 |  |
| ROIGenerator | 生成感兴趣区域 | Generate ROI | 根据参数生成ROI。 |  |
| SBMEstimator | 2D形状匹配 | 2D Shape-based Matching | 2D形状匹配检测。 |  |
| ShadingCorrection | 背景修正 | Shading Correction | 修复背景和阴影干扰，提取稳定特征。 |  |
| TemplateMatcher | 2D模板匹配 | 2D Template Matching | 搜索与模板相似度高的工件。 |  |
| UndistortImage | 图像去畸变 | Undistort Image | 使用内参和畸变系数去畸变。 |  |

### 3D点云处理（56）

默认目录：models/3D\_Processing

| 类名 | 中文名 | 英文名 | 简述 | 源码目录 |
| --- | --- | --- | --- | --- |
| BestFit | 最佳适应算法 | BestFit | 根据工件点位计算使其最符合注册位姿的相对变换。 |  |
| BoxEstimator | 计算包围盒 | Estimate Box | 估计单个长方体的尺寸与位姿。 |  |
| CalObjCoverRate | 工件遮挡率计算 | Calculate Object Cover Rate | 计算工件的遮挡率。 |  |
| CalculateContainerBox | 计算集装箱内切盒 | Calculate Container Box | 计算集装箱内切盒（相机坐标系）。 |  |
| CalculateHeightByNeighborCloud | 计算物体高度 | Calculate Height by Neighbor Cloud | 通过工件附近点云估算工件高度。 |  |
| CalculateInstancePrimitivePose | 计算实例工件位姿 | Calculate Instance Primitive Pose | 用深度学习结果计算位姿，多用于拆码垛。 |  |
| CalculatePalletPose | 垛型匹配 | Pallet Matching |  |  |
| CalculatePlacingPosition | 计算放置点位 | Calculate Placing Position | 计算放置点位。 |  |
| CalculatePrimitiveDimension | 计算3D尺寸 | Calculate Primitive Dimension | 通过抓取前后高度差获取工件高度。 |  |
| CalculateTopLayerHeight | 计算最高层高度 | Calculate Top Layer Height | 按层厚分层，计算最高层点云高度。 |  |
| CloudCutByPrimitives | 用工件切割点云 | Cut Cloud by Primitives | 按工件切割点云，可检测碰撞。 |  |
| CloudCutByRoi | ROI裁剪点云 | Cut Cloud by Roi | 仅保留ROI裁剪框内的点云。 |  |
| CloudCutByTote | 去除料框外点云 | Cut Cloud by Tote | 仅保留料框内的点云。 |  |
| CloudDraggingNoiseRemoval | 去除拖拽噪声 | Remove Dragging Noise | 去除lidar在可抓箱子周围产生的拖拽噪声。 |  |
| CloudEdgeExtract | 点云边缘提取 | Edge Cloud Extract | 提取点云边缘。 |  |
| CloudFillHoles | 点云孔洞填补 | Cloud Fill Holes | 闭运算找孔洞、中值滤波补深度。 |  |
| CloudFilter | 点云过滤 | Cloud Filter | 过滤参考轴上小于等于阈值的点。 |  |
| CloudICP | 3D ICP匹配 | 3D ICP Matching | 用ICP将Model点云对齐到Scene点云。 |  |
| CloudNoiseRemoval | 去除点云噪声 | Remove Cloud Noise | 去除离群点。 |  |
| CloudPCA | 点云主成分分析 | CloudPCA | 通过PCA计算3D位姿。 |  |
| CloudROIExtract | 根据2D检测结果提取点云 | Extract Cloud by 2D Primitives or mask | 按掩膜/ROI将有序点云提取为逐工件点云数组。 |  |
| CloudRemovalByPrimitives | 去除识别工件的点云 | Remove Cloud around Objects | 去除识别工件的点云。 |  |
| CloudSample | 降采样点云 | DownSample Cloud | 降采样点云。 |  |
| CloudToGeometry | 点云构建几何体 | Cloud to Geometry | 将点云构建为几何体（目前仅矩形面片）。 |  |
| CloudTrans | 变换点云坐标系 | Transfrom Cloud | 相机/料框/机器人坐标系间变换，或按外部矩阵变换。 |  |
| CloudViewSampling | 点云视点采样 | Cloud ViewSampling | 对模型点云进行可见点采样。 |  |
| ClustersToPrimitives | 聚类结果生成工件 | Clusters to Primitives | 对每个点云拟合平面并用最小包围盒构造工件。 |  |
| ConstructHeightMap | 生成高度图 | Construct Height Map | 将料箱坐标系点云投影到底面形成高度图。 |  |
| ConstructOrthographicProjectionMap | 生成正交投影图 | Construct Orthographic Projection Map | 正交投影到工作空间底面并计算特征图。 |  |
| ConvexPrimitiveOptimization | 凸起优化 | ConvexPrimitiveOptimization | 拆垛中对凸起箱子或麻袋做抓取优化。 |  |
| CorrectPrimitivePitch | 校正工件3D位姿pitch角度 | Correct Primitive Pitch | 校正顶层凹陷纸箱的pitch角。 |  |
| DepthFilter | 深度图滤波 | Filter Depth | 对深度图滤波去噪。 |  |
| DimensionBasedPoseGenerator | 已知尺寸Pose生成器 | Dimension Based Suction Generator | 已知尺寸时由点云生成pose，用于膜包等场景。 |  |
| FilmWrapPrimitiveOptimization | 膜包工件优化 | Film Wrap Primitive Optimization | 已知尺寸时优化膜包工件。 |  |
| FilterByClustering | 点云聚类滤波 | Filter by Clustering | 聚类后按簇大小过滤。 |  |
| FilterGroupCloudAndPrimitives | 对分组（分区）的点云和工件进行过滤 | Filter Group Cloud And Primitives | 拆垛中按高度对单/多分区工件过滤。 |  |
| FindCloudPlane | 拟合平面 | Find Cloud Plane | 拟合点云中的一个或多个平面。 |  |
| GapFlushDetection | 间隙面差检测 | Gap Flush Detection | 由两台线扫相机点云计算间隙和面差。 |  |
| GenerateBoxCloud | 生成Box点云 | Generate Box Cloud | 根据3D工件生成立方体点云。 |  |
| GenerateFOVBlindAreaObstacle | 生成视野盲区内碰撞体 | Generate FOV Blind Area Obstacles | 在箱子前墙平面上为相机盲区生成碰撞体。 |  |
| GetCloudClusters | 点云聚类 | Get Cloud Clusters | 按距离和角度聚类为多个点云簇。 |  |
| GetCloudHeight | 计算点云的最大高度 | Get Cloud Height | 计算点云的最大高度。 |  |
| GroupCloudByPartition | 根据分区对点云分组 | Group Cloud By Partition | 按分区对点云分组，用于带分区拆垛。 |  |
| GroupPrimitivesByPartition | 根据分区对工件分组 | Group Primitives By Partition | 按分区对工件分组，用于带分区拆垛。 |  |
| HeightExtraction | 深度图高度抽取 | Height Extraction | 按参考面抽取高度生成灰度图。 |  |
| HomogeneousPrimitiveOptimization | 单拆工件优化 | Homogeneous Primitive Optimization | 以最高频工件为标准，补全或重算单拆场景3D工件。 |  |
| InsertionCheck | 纸箱插入Pose优化 | Insertion Pose Refine | 根据点云和相邻纸箱输出最优插入位置。 |  |
| MergeClouds | 合并两个点云 | Merge Two Clouds | 将两个点云合并为一个。 |  |
| MonoGapFlushDetection | 单线间隙面差测量 | Mono Gap Flush Detection | 由单根线扫点云计算面差和间隙。 |  |
| ProjectCloudToCamera | 投影点云到相机坐标系 | Project Cloud to Camera | 将无序点云恢复为有序点云并生成深度图。 |  |
| ProjectPrim3DTo2D | 投影3D几何体 | Project 3D Primitives to 2D | 将3D几何体投影为2D几何体。 |  |
| SearchPoseLocally | 局部搜索最匹配位姿 | Search Pose Locally | 对位姿做平移/旋转局部搜索，返回最匹配位姿。 |  |
| SignalEvaluator | 轮廓分析 | Evaluate Signal | 检测轮廓点云上的特征并测量几何距离。 | 2D\_Processing |
| SurfaceBasedEstimator | 3D模板匹配 | 3D Surface-based Matching | 在点云中找与模板匹配度最好的结果。 |  |
| SurfaceBasedEstimatorV2 | 3D模板匹配（易用版） | 3D Surface-based Matching(Easy mode) | 3D模板匹配易用版。 |  |
| TubeDirectionCorrection | 圆管朝向矫正 | Tube Direction Correction | 通过圆管高光矫正位姿误差。 |  |

### 位姿处理（24）

默认目录：models/Primitive\_Processing

| 类名 | 中文名 | 英文名 | 简述 | 源码目录 |
| --- | --- | --- | --- | --- |
| ConcatPrimitives | 合并两组工件3D | Concat Primitives 3D | 将两组工件合并为一组。 |  |
| ExtendOccludedBox | 纸箱面自动补全 | Extend Occluded Box | 自动补全被遮挡的纸箱面。 |  |
| FilterWrongBoxFace | 过滤错误纸箱面 | Filter Wrong Box Faces | 过滤合并后仍错误的纸箱面。 |  |
| FlipPrimitiveAxis | 翻转坐标轴3D | Flip Axis 3D | 翻转对称工件坐标轴，使其与规定朝向夹角最小。 |  |
| FlipPrimitiveAxis2D | 翻转坐标轴2D | Flip Axis 2D | 2D版翻转坐标轴。 |  |
| FrameFromPoints | 多点定位 | Frame From Points | 由两个或三个2D点获取工件位姿。 |  |
| GeneralPlacePositionFinder | 寻找放置位置 | Find Place Position | 按高度图、工件尺寸和优先模式决定放置位置。 |  |
| GeometryTransformation | 变换3D几何体 | Transform Geometries 3D | 3D几何体位姿变换。 |  |
| MappingHeightFromSKU | 映射SKU尺寸 | Map Height From SKU | 按纸箱尺寸列表补全3D位姿尺寸。 |  |
| MergeBoxFaces | 合并纸箱面 | Merge Primitives of Box Faces | 合并同一纸箱的不同面。 |  |
| MergeBoxesFromViews | 合并多视角纸箱 | Merge Boxes from Views | 合并工作空间坐标系下的多视角纸箱。 |  |
| MergePrimitivesByDimension | 根据尺寸合并工件 | Merge Primitives By Dimension | 合并纸箱检测结果。 |  |
| ObjectTransformation2d | 2D->3D位姿转换 | Transform 2D To 3D | 结合2D标定将图像位姿转为世界坐标系位姿。 | 2D\_Processing |
| PickableBoxPredictor | 纸箱可抓预测 | Pickable Box Predictor | 预测绝对可抓或绝对不可抓的纸箱。 |  |
| PrimitiveFilter | 过滤工件3D | Primitive Filter 3D | 按条件过滤工件。 |  |
| PrimitiveFilter2D | 过滤工件2D | Primitive Filter 2D | 按条件过滤2D工件。 |  |
| PrimitiveTransformation | 变换工件位姿3D | Transform Primitives 3D | 工件位姿变换。 |  |
| PrimitiveTransformation2D | 变换工件位姿2D | Transform Primitives 2D | 2D工件位姿变换。 |  |
| ProjectToPlane | 投影2D位姿到平面 | Project 2D Primitive To Plane | 将2D位姿投影到平面生成3D位姿。 |  |
| ResetPrimitives | 更新工件属性 | Reset Primitives 3D | 更新工件名字或尺寸。 |  |
| RotatePrimitiveAxis | 旋转坐标轴3D | Rotate Axis 3D | 类圆柱工件旋转坐标轴以贴合规定朝向。 |  |
| SetPickableProperty | 根据IOU设置2D位姿属性 | Set Property Base On IOU | 按两组位姿IOU阈值标记属性。 | 2D\_Processing |
| SetWorkspaceByPrimitive | 重设工作空间 | Set Workspace By Primitive | 用输入位姿重设工作空间位姿和尺寸。 |  |
| ShrinkBoxesDimension | 收缩纸箱尺寸V2 | Shrink Boxes Dimension | 根据碰撞检测收缩纸箱尺寸。 |  |

### 流数据运算（14）

默认目录：models/Flow\_Data\_Operation

| 类名 | 中文名 | 英文名 | 简述 | 源码目录 |
| --- | --- | --- | --- | --- |
| BasicArithmetic | 基础数学运算 | Basic Arithmetic | 两数之间的基础运算。 |  |
| CacheData | 数据缓存队列 | Data Queue | 先入先出缓存，满时弹出队首。 |  |
| CloudVectorToCloud | 合并点云数组 | Cloud Vector to Cloud | 合并点云数组输出无序点云。 | 3D\_Processing |
| CompareNumbers | 比较数字 | Compare Numbers | 数字比较运算。 |  |
| ConcateVector | 连接数组 | Concatenate Vectors | 按端口顺序连接数组。 |  |
| Construct2DPose | 构造2D工件 | Construct 2D Pose | 通过位置和角度创建2D工件。 | 2D\_Processing |
| CustomPythonScript | 自定义python模块 | Custom Python Script | 自定义python模块。 |  |
| Decompose2DInformation | 提取2D信息 | Decompose 2D Info | 将2D信息分解为坐标和角度。 | 2D\_Processing |
| Extend2DInformation | 扩展2D信息 | Extend 2D Info | 将2D几何体/物体扩展为3D。 |  |
| FetchElement | 获取数组元素 | Fetch Elements from Vector | 按参数从数组取元素。 |  |
| GeneralArithmetic | 自定义数学运算 | General Arithmetic | 按表达式计算并输出结果。 |  |
| LogicOperation | 逻辑运算 | Boolean Logic | 逻辑运算。 |  |
| MergeVector | 元素合并数组 | Merge Elements to Vector | 将元素合并成数组。 |  |
| VectorSubset | 取数组子集 | Get Subset of Vector | 获取数组子集，输入输出同类型。 |  |

### 检查（5）

默认目录：models/Check

| 类名 | 中文名 | 英文名 | 简述 | 源码目录 |
| --- | --- | --- | --- | --- |
| CheckPalleteChangeByPrimitives | 检查垛是否变化 | Check Pallete Change By Primitives | 检查垛型是否变化。 |  |
| CheckPrimitivesChange | 检查工件位姿是否变化 | Check Primitives Change | 检测抓取后剩余工件位姿是否变化。 |  |
| CloudBasedEmptyToteChecker | 清框检测 | Cloud Based Empty Tote Checker | 框内点云超阈值且无工件时报错。 | 3D\_Processing |
| DimensionConsistencyCheck | 工件长宽一致性检查 | Primitive Dimension Consistency Check | 单拆场景工件长宽一致性检查。 |  |
| ToplayerCompletenessCheck | 顶层点云覆盖度检查 | Top Layer Completeness Check | 检查最高层点云是否被已检工件完全覆盖（漏检）。 |  |

### 深度学习（7）

默认目录：models/Learning

| 类名 | 中文名 | 英文名 | 简述 | 源码目录 |
| --- | --- | --- | --- | --- |
| AIAssistant | AI助手 | AI Assistant | 深度学习相关辅助操作，一般接在深度学习模块后。 |  |
| Detection2D | 目标检测 | Detect Objects by Learning | 检测待抓目标并分割可抓区域，支持旋转框。 |  |
| InstanceDetector | 目标检测V2 | Instance Detector | 深度学习检测二维图像中的物体（支持RoI）。 |  |
| LoadAnnotations | 加载标注信息 | Load Annotations | 按时间戳加载数据集标注结果。 |  |
| OCR | 字符识别 | OCR | 识别图片中所有字符。 |  |
| PredictPoseFromROI | 预测工件位姿(ROI) | Predict Pose From ROI | 从2D检测结果预测工件3D位姿。 |  |
| SemanticSegmentor | 语义分割 | Semantic Segmenter | 对Primitives2D区域或整图做语义分割。 |  |

### 输出（9）

默认目录：models/Output

| 类名 | 中文名 | 英文名 | 简述 | 源码目录 |
| --- | --- | --- | --- | --- |
| BasicDatatypesPub | 发布基本数据类型 | Publish Basic Data Types | 发布基本数据类型信息。 |  |
| CloudPub | 发布点云 | Publish Cloud | 发布点云到xyz-viz查看。 |  |
| ImagePub | 发布图像 | Publish Image | 发布图像到xyz-viz查看。 |  |
| ImageSaveModel | 保存图像 | Save Image | 保存图像到日志目录。 |  |
| MeshPub | 发布模型 | Publish Mesh | 发布工件模型到xyz-viz查看。 |  |
| OutputPub | 输出结果 | Output Results | 将视觉结果返回给bridge。 |  |
| Primitive2DPub | 发布2D位姿 | Publish 2D Primitive | 发布2D工件位姿。 |  |
| Primitive3DPub | 发布3D位姿 | Publish 3D Primitive | 发布3D工件位姿。 |  |
| UploadTimestamp | 上传异常3d工件位姿时间戳 | Upload Timestamp | 上传异常3D位姿对应的时间戳。 |  |

### 可视化（8）

默认目录：models/Visualization

| 类名 | 中文名 | 英文名 | 简述 | 源码目录 |
| --- | --- | --- | --- | --- |
| DataShow | 打印信息 | Print Info | 显示工件位姿、高度等数据。 |  |
| ImageShowModel | 可视化图像 | Visualize Image | 图像可视化，用于观察检测结果。 |  |
| SVGShowModel | 可视化矢量图 | Visualize SVG | 矢量图可视化。 |  |
| StartRerun | （未翻译） | Start Rerun | 启动 Rerun 可视化。 |  |
| Visualize2DCloud | 可视化2D点云 | Visualize 2D Cloud | 用矢量图可视化2D点云。 |  |
| VisualizeCloudVector | 可视化点云数组 | Visualize Cloud Vector | 融合点云数组用于可视化，可投射到图片。 |  |
| VisualizePrimitives2D | 可视化工件位姿2D | Visualize Primitives 2D | 可视化工件旋转包围框。 |  |
| VisualizePrimitives3D | 可视化工件位姿3D | Visualize Primitives 3D | 可视化相机坐标系下每个工件的位姿。 |  |

### 预置组节点（4）

由已有算子组合而成，定义在 property\_settings.yml 的 groups 段。

| 类名 | 中文名 | 简述 |
| --- | --- | --- |
| FlatPartitionDetection | 平面隔板检测 | 最高层点云提取 + PCA，检测平面隔板位置。 |
| RelocateWorkspace | 工作空间重定位 | 高度图 + 2D形状匹配 + 3D ICP，计算料框位姿并重定位工作空间。 |
| ToteLocalization | 托盘定位 | RGB图像 + 透视变换 + 2D形状匹配，计算托盘位姿。 |
| ToteDetector | 托盘检测 | 目标检测V2 + 上传异常时间戳，检测托盘并上传数据。 |

### 内置 Python 脚本（16）

供 CustomPythonScript 选用，定义在 property\_settings.yml 的 pythonscript 段。

| 脚本名 | 中文名 | 英文名 | 简述 |
| --- | --- | --- | --- |
| LogString | 输出字符串到文件 | Log String | 带时间戳输出字符串到文件。 |
| LogPrimitive3D | 输出3D位姿到文件 | Log Primitive3D | 带时间戳输出3D位姿到文件。 |
| LogPrimitive2D | 输出2D位姿到文件 | Log Primitive2D | 带时间戳输出2D位姿到文件。 |
| Primitive3DtoString | 转换3D位姿为字符串 | Primitive3D to String | 3D位姿转字符串，可指定连接符。 |
| Primitive2DtoString | 转换2D位姿为字符串 | Primitive2D to String | 2D位姿转字符串。 |
| StringtoPrimitive3D | 转换字符串为3D位姿 | String to Primitive3D | 字符串转3D位姿。 |
| Compare2Primitive3D | 比较两个3D位姿 | Compare 2 Primitive3D | 按阈值比较两个3D位姿，可报警。 |
| Compare2Primitive2D | 比较两个2D位姿 | Compare 2 Primitive2D | 按阈值比较两个2D位姿，可报警。 |
| CheckPickablePrimtives2D | 检查2D位姿是否为Pickable | Check Pickable Prim2D | 按IOU阈值标记pickable。 |
| ShrinkBoxDimension | 收缩纸箱尺寸 | Shrink Box Dimension | 按碰撞检测收缩纸箱尺寸。 |
| RecordReplayer | （未翻译） | Record Replayer | 回放视觉日志中的录制数据。 |
| MappingBoxDimension | 映射纸箱尺寸 | Map Box Dimension | 按尺寸列表补全3D位姿尺寸。 |
| MakeOffset | 3D位姿添加偏移 | Make Offset | 给3D位姿添加位置偏移。 |
| ConstructPrimitive3D | 构造3D位姿 | Construct Primitive3D | 由端口和参数构造3D位姿（XYZ欧拉角）。 |
| MoveDebugImage | 整理debug图像 | Move Debug Image | 按时间戳归档现存debug图像。 |
| RigidDistanceCheck | 刚体校验 | Rigid Distance Check | 按注册值判断刚体形变是否超阈值并报警。 |

## 说明

算子以 `CLASS_LOADER_REGISTER_CLASS(<类名>, NodeDataModel)` 在 include/xyz\_vision\_lib/models/<目录>/<类名>.hpp 中注册，由 RegisterDataModel.hpp 的 `registerClassLoaderModels()` 统一加载。

**检索口径**

- 算子列表：grep 全部 `CLASS_LOADER_REGISTER_CLASS(..., NodeDataModel)`，共 178 处，无被注释的注册。
- 名称、分类、简述：property\_settings\_zh.yml 与 property\_settings.yml 的 `properties` 段；简述取 description 首句并压缩。
- 组节点与 Python 脚本：同文件的 `groups`、`pythonscript` 段。

**未计入的注册项**

- 30 个数据类型转换器（`DataTypeConverter`，如 ImageToGrayConverter、CloudToStringConverter）：连线时自动插入，不在算子面板中。
- 23 个数据查看器（viewers/ 下的 Cloud/Image/String/SVG NodeDataViewer 模板实例）。
- property\_settings.yml 中的 GroupNodeDataModel（空组基类）和 TotePartition（只有属性配置，无对应注册类）。

**检索中发现的数据问题**

- StartRerun 缺少中文名称、分类和描述。
- property\_settings\_zh.yml 中托盘检测组的键名写成 `ToteDector`，与 groups 中的 `ToteDetector` 不一致，导致中文名未生效。
- RecordReplayer 脚本缺中文名称；CalculatePalletPose 无描述；XYZLidar 的中文描述仍为英文。
- 8 个算子的界面分类与源码目录不一致（见各表“源码目录”列）。
