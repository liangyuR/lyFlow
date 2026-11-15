# C01 投影、深度滤波与点云ROI分析

## 源码语义与改进边界

逐项阅读本机 xyz-vision-lib 中五份同名 cpp/hpp 和 property_settings.yml，仅分析接口与算法语义，不复制专有函数体。给定目录无可核对的源码修订。

`ConstructHeightMap` 在工作空间底面建立正交栅格，保留最高Z，交付高度、灰度/伪彩和organized关联。原实现以零初始化导致负高度丢失，灰度再次遍历原点会被最后一个点覆盖。新实现用独立状态图表示无效，不把零/负高度当孔洞，所有可视化从最终高度生成。

`ConstructOrthographicProjectionMap` 另外具有独立的法线采样/输出分辨率，以及高度、法线角、伪彩高度三种图。新实现真实估计栅格邻域法线角，边界/缺邻域显式无效；最终高度直接按输出栅格聚合真实观测，避免无效零值参与双线性插值制造观测。

`DepthFilter` 包含均值、中值、高斯、无效补偿(IDW/邻域最小/最大)及方向性凸起/凹陷/两者消除。新实现保持全部模式，独立处理有效性、阈值边缘和原观测/合成状态。无变化的点保留原XYZ；改变深度时沿原相机射线调整，正交图保留原XY；没有源点的补值才使用像素中心反投影。输出真实compact过滤云，不把旧噪声留在一个被误称为过滤结果的存储云中。

`ProjectCloudToCamera` 消费RGB与相机帧云，调用所给目录不可得的CloudAligner。这里使用既有CameraCalib、显式刚体、真实OpenCV畸变模型和最近Z-buffer独立实现，记录被遮挡、落图外和无效源点。不会宣称与不可得底层逐位等价。

`CloudROIExtract` 原生支持未组织云按K/D投影，以及organized像素关联两条路径；包含二维实例/掩膜、缩放区域、偏移和organized输出。新实现以原生索引图代替PointCloud不存在的width/height，直接消费P4 Poses2D+Masks2D/NHW，使用Groups+Indices表达多ROI，另交付选定ROI的拓扑视图。未组织云也支持显式标定投影，并区分可见点和同视线全部点。源dense-mask分支构造局部结果但疑未发布等行为不会复现。

## 共享深度关联契约

私有共享头为 `packs/std-image/ops/p5_projection_common.h`，不引入PCL/Eigen。`vision.DepthMapping.v1` 是小Record，原生 `depth` 为单通道F32米，`status` 为单通道U8，`indexMap` 为 `[H,W]` float32 Tensor；无效像素必须depth=0、index=-1。有效性由status表示，因此正交高度的合法零值和负值不会丢失。

status位为 observed=1、interpolated=2、filtered=4、collision=8。observed与interpolated互斥，filtered/collision只能附加于有效像素。索引必须为精确整数并引用当前cloud，sourcePointCount最多2²⁴，实际输入还受内存/运算预算限制。

mapping包含width/height、fullWidth/fullHeight、scale、imageFrame、gridFrame、cloudFrame、unit=m、sourceCloudId十进制字符串、sourcePointCount、projection、gridToCloud右手刚体与depthHash/statusHash/indexHash。imageFrame是二维像素参考帧，gridFrame是三维相机或工作空间帧，不能混为同一个字符串。透视映射包含既有CameraCalib.data且camera.frame必须等于gridFrame；正交映射包含像素0中心的originXY和米/原像素resolutionXY。完整块的工作像素中心恢复原像素为 `(p+0.5)*scale-0.5`；末尾不足scale的块使用实际覆盖原像素的中心。比如原宽9、scale2的末中心是8，末像素边界为7.5..8.5，不能向图外扩张。共享fullX/fullY恢复中心，fullBoundaryX/fullBoundaryY及backProjectBoundary用于物理单元四角。

新云重新绑定sourceCloudId/count及三份原生载荷摘要。过滤/抽取另外输出sourceIndexMap记录父云索引，并在mapping保存parentCloudId/parentPointCount/sourceIndexHash；新合成点无父索引。摘要仅防止组合中错误混搭几何载荷，不是产品身份或运行库指纹。源云、输入图及数组只读。

## 算子接口

五个节点均注册于std-image，hook为 `registerP5Projection`。

| 节点                   | 主要输入                                                                                             | 主要输出                                                                                                                           |
| ---------------------- | ---------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------- |
| cloud.height_map       | cloud，可选gridToCloud                                                                               | cloud/sourceCloud、depth、image灰度、pseudo、normalAngle/normalValid、status、indexMap/sourceIndexMap、mapping、pointState/quality |
| cloud.orthographic_map | 同上                                                                                                 | 同上，image按outputMode选择height/normal_angle/pseudo_height                                                                       |
| cloud.project_camera   | cloud、image、calib，可选cloudToCamera                                                               | 可见camera帧compact cloud、原sourceCloud、depth、原image、status、indexMap/sourceIndexMap、mapping、父云pointState/quality         |
| image.depth_filter     | depth，完整cloud/status/indexMap/mapping组或calib                                                    | 真实compact cloud、depth/status/indexMap/mapping、sourceIndexMap、changed/quality                                                  |
| cloud.roi_extract      | cloud，完整depth/status/indexMap/mapping组或calib，可选cloudToCamera；instances+masks+maskInfo或mask | compact cloud、sourceCloud、groups/indices/sourceIndices、选定ROI的depth/status/indexMap/sourceIndexMap/mapping、quality           |

高度/正交参数为width、height、resolution、originX/Y、minHeight/maxHeight、cloudFrame、gridFrame、imageFrame；正交另有normalResolution与outputMode。gridToCloud是输入源云与工作空间栅格的显式关联，不能靠宿主隐式全局工作空间。

相机投影使用cloudFrame和独立imageFrame，跨相机三维帧时必须给cloudToCamera；K/D始终以标定完整原图为准，允许RGB与深度工作尺度不同但须显式符合图像scale及标定完整尺寸。

滤波参数为mode(mean/median/gaussian/fill/eliminate)、kernelSize奇数、iterations、sigma、fillInvalid、fillMethod(idw/min/max)、idwPower、eliminateType(protrusion/depression/both)、direction(x/y/xy)、threshold、maxDepthJump和includeInterpolated。输出changed为原生U8变化掩膜，质量记录实际工作核及改变/填充/去除数量。

ROI参数为shrink=1、usingMask=true、usingOffset=false、visibility=visible、selectedRoi=0、offsetX/Y=0、cloudFrame=camera、imageFrame=image。显式空instances返回空分组与全无效所选视图，不退回全云。usingMask=true时，缺少可用掩膜的实例输出空组，quality.missingMaskRois明确标注；仅显式usingMask=false才使用旋转框几何。声明hasMask=true但缺少原生载荷为错误。

P4掩膜Tensor必须tensorScale=1、完整原图NHW二值0/1；MaskInfo六个字段frame/unit/width/height/layout/items，items逐层四字段id/index/hasMask/source与Pose属性一致。hasMask=false要求source=none且全零；hasMask=true要求非空支持且支持位于未收缩Pose内。零偏移要求掩膜完整尺寸等于映射fullWidth/fullHeight，不能把任意不等尺寸当作局部图。非零偏移必须显式usingOffset=true，语义为ROI全像素坐标=当前图像素+offset，且平移后的完整栅格被更大的ROI图覆盖。

pointState是原父云长度N的向量，不是HW图像，quality.pointStateScope固定sourceCloud-parent-points。引擎可能继承图像输入scale标记到该Tensor，但其元素与点索引始终一一对应，不按scale回投或重采样。

## 资源、数值与取消

单张栅格最多8Mi像素，宽高及标定完整宽高不超过32768；输入云最多2Mi点、可选通道合计128MiB。索引协议上限2²⁴，避免float32舍入歧义。算子在主要数组分配前估算512MiB工作内存；法线细栅格也单独检查8Mi像素。滤波以实际支持中心估算核访问，超过256Mi次拒绝，空支持不运行空耗迭代。ROI最多64实例、32Mi float掩膜元素、64Mi包含检查及16Mi分组索引，超过预算返回resource_limit，失败不输出部分载荷。

反投影的零畸变路径精确线性计算；非零畸变使用OpenCV相同K/D正投影及Jacobian，以最多32轮Newton、每轮12次阻尼线搜索求逆。只有真实回投影像素残差不超过1e-7才返回射线；不能收敛的合法有限参数也返回明确bad_input，不能把错误XYZ标为observed。每批像素共享RayBudget，最多64Mi次正投影评估，支持调用者累计预算及取消。这样覆盖径向、切向、薄棱镜和倾斜14D模型，避免默认固定点迭代在强畸变下错误成功。非单射模型不承诺全局唯一逆。

所有大循环周期检查取消，载荷摘要每64KiB检查；摘要计算、逆投影及节点guard均传播cancelled。OpenCV单次小批投影调用内部不宣称可抢占。

## 验证状态

独立MSVC14.51/OpenCV5.1 probe使用实际生产compute及core数据实现，13组34034断言全部执行通过、0skip，日志为build/p5-projection-probe/build.log与tests.log。覆盖五滤波模式、真实root生成PCD全部3066父点的3063可见关联及全通道、负/零高度、法线角、RGBA三位深输入只读、真实XYZ无变化保留、4/5/8/12/14D畸变、跨三维帧、scale2与odd末块、原生P4绑定坏载荷、缺mask空结果、裁剪偏移、强k1=1逆投影真值、不可逆拒绝、取消及内存/运算预算。

这是局部生产compute测试，不代表完整调度器、CLI/CABI/SDK的Full/Preview已经通过；这些由阶段总集成独立运行。合成/自有固定样本不构成传感器、现场精度或吞吐验收。
