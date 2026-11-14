# P1 图像算子源码分析与实现选择

[补齐计划](operator-completion-plan.md) · [能力对照](operator-coverage.md) · [图像包](../packs/std-image/README.md)

基于用户提供的本机 `xyz-vision-lib` 源码，逐项核对输入、参数、主要算法调用、结果和异常路径，再按 LyFlow 数据模型独立实现。参考源码带 proprietary 声明，本实现没有复制其实现体；本机目录没有 Git 元数据，不能声明匹配清单修订或旧配置协议。模型检测链另由其专门文档和模型验收覆盖。

## 逐项分析

表中的参考路径均相对 `xyz-vision-lib/src/xyz_vision_lib/models/`。

| 外部条目 / 源码 | 核实行为和局限 | 本次选择、优化及边界 |
|---|---|---|
| ConcatImages / `2D_Processing/ConcatImages.cpp` | 动态输入数，灰度隐式转RGB，以最大单图尺寸自动排网格，画布固定u8三通道。 | `image.concat` 接两幅图，显式水平/垂直、填充值；要求同位深/通道/预览比例，支持u8/u16/f32及RGBA，保持源图只读。可组合多图网格，不隐式转换颜色，也不声称全景配准。 |
| ImageColorAdjustment / `2D_Processing/ImageColorAdjustment.cpp` | u8灰度/RGB，Auto逐通道均衡，Manual通过Gamma/对比度LUT。 | `image.color_adjust` 明确归一化亮度、对比度、Gamma、RGB增益/偏移及HSV色相/饱和度；保留alpha、位深，不重写已存在的范围转换。原Auto直方图均衡模式未移植。 |
| ShadingCorrection / `2D_Processing/ShadingCorrection.cpp` | 平均/中间灰度参考和方向性形态学背景，明暗模式及噪声阈值，内部按u8访问。 | `image.shading_correct` 使用显式平场或有界高斯背景；差分和除法各有公式，除法拒绝非正/小分母，逐通道保持平均亮度，alpha不受校正。原方向性顶帽/黑帽和噪声阈值模式未等价移植。 |
| GeneratePrimitivesMask / `2D_Processing/GeneratePrimitivesMask.cpp` | 从工件附带mask或按名称加载图片，缩放/旋转/膨胀后合并显示；依赖工件自带mask与文件目录约定。 | `image.instances_mask` 消费统一Pose2D中心/尺寸/弧度角，旋转矩形光栅化为原生u16标签、u8二值图及稳定ID表。重叠显式first/last/error；不把mask数组写JSON。任意形状资产mask及三维工件投影仍未覆盖。 |
| ImageMorphology / `2D_Processing/ImageMorphology.cpp` | 常规形态学和u8实例膨胀；实例膨胀采用局部候选频次，源码提示多实例缠绕可能异常。 | `image.instances_morphology` 对每个标签独立处理，再按明确冲突策略合成，支持4096标签，膨胀默认保护原标签。奇数椭圆核非法时失败，不静默调整核；缺失ID和标签越界失败。 |
| LoadCameraCalibInfo / `Input/LoadCameraCalibInfo.cpp` | 涉及设备ID、虚拟相机、YAML、机器人/手眼/世界帧树更新，给图像/深度/云附加外参。 | `image.load_camera_calib` 仅读取版本化相机内参记录，不修改全局设备或帧树。K、畸变、尺寸、帧、单位严格验证；1MiB/深度16限制，中文路径和文件修改感知。设备YAML、手眼和全局帧树不属于本切片。 |
| LoadCalibrationInfo / `Input/LoadCalibrationInfo.cpp` | 旧版相机/机器人/工作空间外参文件及全局帧树副作用，并非单独的内参文件。 | 平面H继续用已有 `image.load_calib`；相机K/D用独立记录，已知外参由显式Transform/Pose链处理。两种标定不可互相冒充；外部旧协议保持部分覆盖。 |
| UndistortImage / `2D_Processing/UndistortImage.cpp` | Brown-Conrady remap；缓存主要按camera_id判断，保持ID但更改K/D时可能沿用旧映射。 | `image.undistort` 按分辨率、完整K/D建立最多4项/64MiB的有界映射缓存，避免同ID调参后陈旧映射；拒绝标定/图像尺寸或预览比例不符。输出零畸变标定和采样有效mask，不把边界填0当有效观察。 |
| ImageHomography / `2D_Processing/ImageHomography.cpp` | 根据工作空间平面、设备外参和虚拟相机推导H，再warpPerspective；将H嵌入工件矩阵输出。 | `image.warp_perspective` 显式消费原图像素→m/mm平面H，输出原点、像素密度和固定宽高；不把非刚体H当Transform。记录含图像尺寸时核对尺寸；完整插值支撑才标有效。虚拟相机/工作空间协议不在本节点内推断。 |
| CalibBoardPoseEstimation / `2D_Processing/CalibBoardPoseEstimation.cpp` | 外部BoardDetector封装棋盘/标记板，随后solvePnP；无内参时只记录警告，未统一验证收敛、正深度与重投影门限。 | `image.board_pose` 的棋盘SB角点经子像素细化，显式相机K/D、米制方格，IPPE和迭代候选统一验证正深度/RMS；输出前用实际float32矩阵逐点变换后再施加畸变模型，重算RMS并用同值报告和门控，拒绝原生精度不足的结果；检查全部不同旋转候选的误差歧义，默认拒绝歧义。native Transform为board首内角点→camera；Pose3D位置为板面中心。Marker/Charuco属于后续范围。 |
| TemplateMatcher / `2D_Processing/TemplateMatcher.cpp` | 内部模板匹配库训练/搜索，角度、最高层面积、最大数量、SIMD和亚像素开关，返回旋转工件。 | `image.template_build` 保持模板为原生图像，小Record只存ID、尺寸和方法；`image.template_match` 独立实现掩膜零均值NCC、离散旋转/尺度搜索、全局有界候选及旋转框NMS。旋转padding不计分，常量模板拒绝，无目标为空集合。使用像素中心旋转，输出实际重采样尺寸；不声称亚像素或轮廓形状匹配。 |
| DetectCircleEdge / `2D_Processing/DetectCircleEdge.cpp` | 圆/圆弧ROI径向条带平均，极性/方向选峰，PCL圆RANSAC，可选LS及固定半径拟合，内部将像素统一除255。 | `image.circle_caliper` 使用原值单位梯度、双线性径向采样、平台峰中心及亚像素插值；确定性RANSAC和中心化SVD拟合不引PCL。有效扫描比例、内点比例、RMS、圆心偏移和圆/直线残差比共同门控，防止直边被当圆。固定半径约束模式未移植。 |
| CalculateRingWidth / `2D_Processing/CalculateRingWidth.cpp` | 圆/弧ROI内的内外宽度与直径四种模式，径向或过圆心对边扫描，导出最小/最大宽度。 | `image.ring_width` 本切片明确为径向亮/暗环带，首对相邻或首末包络边缘，支持内外扫描方向，输出周向min/max/mean和配对诊断。有效比例不足时Measurement无效而非0；原过圆心直径及全部四种模式未宣称覆盖。 |

## 数据与数值约定

- 仅相机内参记录使用 `vision.CameraCalib.v1`：`imageSize:[w,h]`、行主序 `cameraMatrix:[9]`、`distortion`、`frame`、`unit:"m"`。畸变长度为0/4/5/8/12/14，正焦距、零skew、末行 `[0,0,1]`。内参不是手眼外参，也不是平面H。
- 平面标定沿用 `image.PlaneCalib`：H将原图像素映到其声明的m/mm平面单位。`image.calibrated_distance` 只接受像素Point2D并输出米；H穿越无穷远、损坏或奇异时报错。
- 实例标签图是原生单通道u16，0为背景，1..N对应 `vision.InstanceLabels.v1.ids`；ID不随形态操作重编号。标签/图尺寸、帧和单位必须匹配。
- NCC返回统一 `vision.Poses2D.v1`。中心是像素中心坐标，正角从+X朝+Y；尺寸来自实际整像素重采样而非未落地的浮点目标尺度，attributes同时保留请求尺度与实际X/Y尺度。模板像素和标签数组不放入JSON。
- 图像参考帧由调用方显式声明。输入若已被裁剪，返回的是该裁剪图的坐标系；需要映回父图时，应保留裁剪偏移并使用显式位姿变换，不能仅靠 `Image.scale` 推断已丢失的原点。
- 圆弧的原生Circle2D/Measurement仍由执行器换回原图坐标；质量Record中的点、半径、宽度、RMS自行恢复原图并写明 `coordinateSpace=source`、`scale`。正式物理量测需要全分辨率与已校验标定，预览重采样结果不作现场精度保证。
- 棋盘PnP输出的板厚是显式 `thickness` 显示元数据，不参与位姿估计；`quality.sourceFrame=board` 的原点位于第一内角点，Pose3D中心与该Transform原点不同且转换明确。

## 资源、失败和验证

基础图像上限16Mi像素，输入字节上限256MiB；NCC单图4Mi像素、模板1Mi像素，最多81种角度/尺度组合、64Mi响应像素，候选和实例最多4096。实例处理的实例数×像素数不超过128Mi；径向采样不超过64Mi，圆拟合轮数不超过1024。Record沿用1MiB、深度16限制；实例标签的id/frame沿用Pose的64KiB字符串上限，生产和消费无额外256字节限制。循环协作取消；单次OpenCV调用受规模约束，但不声称可抢占其内部线程。

永久回归位于 [test_image_p1.cpp](../packs/std-image/tests/test_image_p1.cpp)。覆盖原图只读、u8/u16/f32、RGB/alpha、平场小分母、实例身份及冲突、中文路径/错误相机契约、零与非零畸变、部分插值有效域、平面mm→m距离、带非零3D旋转/平移的PnP、全部候选歧义门限、完整和透视棋盘角点、重复点退化、NCC多实例/旋转/实际尺度/亮度变化/无目标、完整圆/圆弧/遮挡、直边拒绝、环宽方向和无效结果。

真值回归曾复现径向梯度平台使环宽最小值偏0.5px，改成平台中心而非放宽阈值；曾复现90°模板旋转的像素中心偏移，改成像素中心旋转及实际采样尺寸。额外直边场景揭示仅RMS门限仍可能把窄ROI直边拟合成圆，因此增加直线残差对照与曲率证据门限。合成图的误差阈值是软件回归边界，不代表实物、光学/相机误差、节拍或长期稳定性验收。

2026-11-14 本机 Windows/MSVC、OpenCV 4.12 独立构建启用std-image/std-value，新增图像回归18组均通过；该构建总计68组、5188断言通过，manifest自检65算子通过。该独立构建未启用std-ml，旧的可选推理用例会明确打印未提供测试模型；不能将其计作真实模型验证，模型链由集成构建单独验收。当前格式检查和diff检查通过，最终合并门禁以PR对应修订的CI为准。
