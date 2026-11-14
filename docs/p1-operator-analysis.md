# P1 主要视觉链：实现与边界

[文档中心](README.md) · [198项能力对照](operator-coverage.md) · [补齐计划](operator-completion-plan.md)

P1在1.3.5中新增 **55个算子**：点云/剖面18、工件/几何19、图像14、模型检测前后处理4。它们对应初始计划的47个P1外部条目，并提前提供BestFit、SearchPoseLocally两个P2条目的基础子能力；条目数和算子数不是一一对应，也不表示55个外部节点或全部模式已移植。

逐项核对后，47个P1条目中26项已有对应限定用途，21项保持部分覆盖。完整198项统计为已有54、可组合7、部分45、缺失87、待核实5。原始分类、标识和优先级保持不变，剩余模式在[逐项表](operator-coverage.md)中保留。

## 使用入口与源码分析

| 领域 | 当前链 | 分析与契约 |
|---|---|---|
| 云集合、统计与配准 | 原生云/索引+分组元数据 → 三维簇/多平面 → 逐组统计、取组和局部SE(3)配准 | [逐项分析](p1-analysis-cloud.md) · [std-pointcloud](../packs/std-pointcloud/README.md) |
| 工件与几何 | 受校验Pose2D/3D → 集合/筛选 → 显式帧变换、局部偏移和JSON往返 | [逐项分析](p1-analysis-pose.md) · [std-geometry](../packs/std-geometry/README.md) |
| 图像、标定与定位 | 相机K/D → 去畸变/有效域；平面H → 重采样/物理距离；棋盘PnP、NCC、圆弧与环宽 | [逐项分析](p1-analysis-image.md) · [std-image](../packs/std-image/README.md) |
| 选定模型检测 | YOLOX或YOLOv8/11/26明确导出契约 → 原生Tensor → ONNX推理 → 解码/NMS或端到端输出 → Pose2D集合 | [模型分析](p1-analysis-detection.md) · [资产与示例](../examples/p1-detection-README.md) |

本轮固定真实模型为 **YOLOX-Nano 0.1.1rc0、YOLOv8n、YOLO11n、YOLO26n**。YOLOX使用BGR/0..255、416输入及网格解码；v8n/11n使用RGB/255、640输入与raw类别头NMS；26n使用相同归一化及端到端输出，不重复NMS。权重/导出环境/许可分别管理，不能混接不同模型的预处理和解码；具体签名、资产与准备方法见[四模型示例](../examples/p1-detection-README.md)。

外部源码只用于核实语义和识别问题，LyFlow独立实现。本机参考目录没有Git元数据，不能声称等同原始清单的指定修订；脚本缺实现时只依据可获得的属性声明分析，不宣称阅读或移植了不存在的脚本。

## 本轮新增算子

| 所属包 | ID |
|---|---|
| std-pointcloud：统计/分组（10） | `cloud.statistics`、`cloud.pca`、`cloud.bounds_obb`、`cloud.group_pack`、`cloud.group_get`、`cloud.group_subset`、`cloud.group_merge`、`cloud.group_concat`、`cloud.group_statistics`、`io.load_cloud` |
| std-pointcloud：分割/配准/剖面（8） | `segment.euclidean_clusters`、`segment.multi_planes`、`register.rigid_fit`、`register.icp_3d`、`register.local_search_3d`、`profile.analyze`、`profile.gap_flush`、`profile.project_pair` |
| std-geometry：工件（17） | `pose.construct_2d`、`pose.construct_3d`、`pose.concat`、`pose.get`、`pose.filter`、`pose.update`、`pose.transform_2d`、`pose.transform_3d`、`pose.offset_3d`、`pose.flip_axis_2d`、`pose.frame_from_points`、`pose.decompose_2d`、`pose.compare_2d`、`pose.to_text`、`pose.from_text`、`pose.to_transform`、`pose.to_world_plane` |
| std-geometry：几何（2） | `geometry.transform_2d`、`geometry.transform_plane` |
| std-image：基础/实例（5） | `image.concat`、`image.color_adjust`、`image.shading_correct`、`image.instances_mask`、`image.instances_morphology` |
| std-image：标定（5） | `image.load_camera_calib`、`image.undistort`、`image.warp_perspective`、`image.calibrated_distance`、`image.board_pose` |
| std-image：定位/量测（4） | `image.template_build`、`image.template_match`、`image.circle_caliper`、`image.ring_width` |
| std-image：检测（4） | `image.yolox_preprocess`、`image.yolox_decode`、`image.yolo_preprocess`、`image.yolo_decode` |

`ml.onnx_run`是复用的既有节点，不重复计入55个新增算子。安装组合以所用引擎的manifest为准，启用std-image不等于已经安装模型或ONNX执行包。

## 数据、失败与精度

云集合保留原生PointCloud和Indices，只把来源ID、帧、单位及组区间写入`cloud.Groups.v1`。工件使用`vision.Pose2D.v1` / `vision.Pose3D.v1`及对应复数集合；相机内参使用`vision.CameraCalib.v1`。小型Record沿用1MiB/深度16边界，集合最多4096项；工件文本往返另受64KiB字符串上限约束。点、像素、张量和大数组不进入JSON，新能力不增加Data种类或改变C ABI。

变换方向为source→target，位置与尺寸单位显式，三维旋转右手、四元数xyzw。工件与原生Transform分开；二维像素位姿只有在已知平面H和明确物理尺寸下才能映到世界平面，不能据此恢复任意三维深度。

三维配准以**实际交付float32变换**的原生坐标运算重新计算质量，`aligned`的XYZ与下游`transform.apply`一致。单剖面也在Line2D收窄后重算RMS，并用实际直线计算面差。无法满足输出精度门限的结果拒绝，不用内部double解掩盖误差。几何仿射乘加补偿大平移抵消，点云文件按声明字段类型校验，实例/云索引校验来源和身份。

无目标可返回合法空集合；非法版本、尺寸、单位/帧、非有限值、退化或质量不达标返回诊断。检测置信度不等于可抓取，输出默认`pickable=false`；合成真值和公开模型图片不是工业现场质量、光学误差、节拍或长期稳定性验收。

## 保留的后续范围

- 云：法线条件/organized邻接聚类、箱面专用最小矩形、全局匹配、加权点线面拟合、完整轴组合搜索及workspace安全高度。
- 工件：三维对称/碰撞/抓取规则、动态变换笛卡尔展开、对象裁云、跨帧关联、专用集合查看器和外部发布协议。
- 图像：任意资产mask/三维投影、固定半径拟合、环带直径模式、方向性形态背景、Marker/Charuco、轮廓形状匹配及外部设备/工作空间格式。
- 模型：实例分割、可抓区域、旋转检测框、多ROI批处理、任意ONNX导出格式、OCR/条码和外部标注/工位协议。Detection2D与InstanceDetector继续标为部分覆盖。

上述边界沿用[原优先级](operator-completion-plan.md)，没有把其余P2/P3任务偷偷提升或宣告完成。

## 示例与验证入口

- [云分组与配准](../examples/p1-cloud.lyflow.json)：受控点云与三维真值链，资产为[PCD样例](../examples/p1-clusters.pcd)。
- [位姿非交换变换与往返](../examples/p1-pose-flow.lyflow.json)：无PCL/图像依赖的工件链。
- [YOLOX检测](../examples/p1-detection.lyflow.json)及[YOLO检测](../examples/p1-yolo-detection.lyflow.json)：模型资产、许可证、准备和运行方法见[模型示例说明](../examples/p1-detection-README.md)。
- 永久契约回归：[点云](../packs/std-pointcloud/tests/test_p1_cloud.cpp)、[位姿/几何](../packs/std-geometry/tests/test_pose.cpp)、[图像](../packs/std-image/tests/test_image_p1.cpp)、[检测](../packs/std-image/tests/test_detection_p1.cpp)。真实模型与执行入口验收见[集成测试](../core/tests/integration/p1_detection.cpp)及[测试地图](testing.md)。

本页列出实现、可运行资产和验证入口；最终测试数量与发布门禁结果以当前PR对应源码修订的CI及集成报告为准，不混用不同可选包构建的测试数字。
