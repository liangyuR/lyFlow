# 版本变更

## 1.4.4（开发中）

- M4b新增6项公共Profile选择/稳健拟合、3项度量及`geometry.box_2d`，完整审计组合为271项原生算子；稳健拟合显式依赖PCL可选模块，轻量选择/几何/度量不引入视觉后端。
- 原槽/物理源索引保留在原生Profile，Indices绑定实际输入cloud.id，保槽不虚报移除数。
- 旧Gap直线/面差/间隙/位移复用公共静态核，4项patch版本隔离缓存；42已有定义直线、281已有定义量测逐位一致，12非法输入行为更严格，不能宣称全部历史输入兼容。
- 旧图迁移改为离线执行：编辑器和宿主加载时不再自动升级，图中带`contractUpgrade`或已退役的DTS/Gap节点时报`migration_required`并提示命令，运行`lyflow migrate <图> --write`（或`--request <文件>`）迁移后保存。迁移规则由提供算子的包注册，C ABI新增`lyflow_migrate`；执行状态不再返回`graphUpdate`，SDK头`graph_contract_upgrade.h`改为`graph_migration.h`。
- 执行器对算子输出的Bundle逐字段做与顶层输出相同的Profile/点云/图像自洽校验，字段非法时报`internal`并指明字段；算子不再重复检查执行器已保证的参数枚举、范围与端口类型。
- Profile公共算子删除来源集合、`profile.CompositeSources.v1`、`provenance`输入输出及内容指纹；几何上下文统一为只记录坐标帧、分析平面与手性的`profile.Geometry.v3`，几何内容不再与上下文绑定。合并轮廓的`sourceId`改为可读的`derived:composite/a+b`。相关算子升为2.0.0，旧图运行`lyflow migrate`删除连到已退役端口的边。Gap与DTS同步删除来源端口、来源闸门和各类指纹及读取时的重算比对：Gap场景、特征、模板与模型Bundle升为v3，`GapResultBundle`升为schemaVersion 4并去掉`source_evidence`，`gap.labels_from_logits`去掉`modelInput`，DTS的`dts.flush`、`dts.profile_bundle`去掉只用于指纹比对的`metal`/`foot`输入；涉及的算子升主版本。
- 点到直线有符号距离只保留公共`metrology::signedPointLine`一份实现：点线面差、稳健直线ROI求交、轮廓直线最近样本和Gap旧引擎都改用它的双精度计算，结果相对原float计算只差舍入，`metrology.flush_point_line`、`profile.nearest_to_line`升为2.0.1、`gap.measure_reference`升为1.0.1隔离缓存；正法向参考与直线法向的正交判定改为精确。删除无调用方的`profile_kernel` `Line::signedDistance`。
- std-profile升级0.5.0、std-metrology升级0.4.0、std-geometry升级0.7.0、gap与dts升级0.2.0；产品1.4.4保持1.4系列C ABI布局。旧8ID、场景/Bundle及六导入器迁移留M4c，整个M4尚未完成。

## 1.4.3（M4a已合并）

- M4a将双迹线RANSAC、轴向重试、圆LM细化、固定半径与软先验抽为公开的`profile_robust_kernel`静态核；旧点云接口仅做数据适配，保持原算子ID、版本、端口与参数。
- 包支持声明`OPTIONAL_MODULES`，构建可用`LYFLOW_MODULES`显式选择后端；默认Profile/DTS链继续使用轻量内核，PCL拟合SDK组件显式加载后端依赖。
- 产品1.4.3沿用1.4系列C ABI布局。M4公共Profile节点与Gap场景/模型/导入器迁移继续按后续切片验收，不以本次公共核提取视为M4完成。

## 1.4.2（开发中）

- M3新增原生Profile输入/保槽清洗/分面/拟合/成对变换与公共米制度量；旧轮廓分析三算子迁归属，原ID/版本/数值契约保持。
- DTS场景移至`solutions/dts`，显式`solution-dts`包；五场景节点v2使用完整Profile上下文和真实米制几何，业务测量保留毫米/原符号。旧清洗/分面ID通过显式来源图迁移退役，不保留重复算法。
- CLI增加完整LYPR输入与显式`dtsProfile.v1`迁移保存，Rust/编辑器共享原子整图替换并遵守拓扑锁；未知来源或缺元数据拒绝猜单位。
- SDK交付轮廓两静态核及Eigen5.0.1许可；Eigen仅编译期使用，安装后静态消费者不需要Eigen/PCL/OpenCV/ORT或core运行库。产品为1.4.2，C ABI布局沿用1.4系列。

## 1.3.11（开发中）

- P2阶段5补齐有标定的深度/正交栅格、原生像素与点索引关联、五类深度处理、显式补值、物理缺陷、三角网格截面及已知半径三维圆孔链。
- 增加有限/连续对称轴处理、跨帧位姿比较、刚体参考模板与实际残差（检查与要求可观测变换的配准入口分开）、二维升维、三维形状/掩膜投影及平面回投、批量几何裁云/移除和确定性盒体采样。
- std-image0.8.0、std-pointcloud0.4.0、std-geometry0.3.0；产品/ABI字符串1.3.11，复用既有原生端口与C ABI，保持1.3系列兼容。

## 1.3.10

- P2阶段4新增原生语义分割、逐ROI掩膜保留/替换、完整OCR检测/方向/CTC链、条码解码/二维码单元增强，以及显式时间戳标注加载。大数组沿用原生Tensor和Image，上游输入只读。
- ONNX增加串行批处理与固定模型契约复核；公开模型及自有夹具单独准备，不内嵌权重。ZXing-C++固定3.1.1，私有C++20静态后端不改变C++17 SDK要求。
- std-image0.7.0、std-ml0.3.0；产品与ABI字符串1.3.10，不新增C ABI符号或数据种类，保持1.3系列兼容。

## 1.3.9

- P2阶段3新增Marker解码/PnP、显式工作空间帧文件，以及原生特征Tensor的梯度形状模板/多实例匹配；输入只读，输出通过单位、坐标帧、歧义和质量门控。
- 默认OpenCV升级到5.1.0固定源码最小模块，迁移geometry/objdetect接口；ONNX Runtime CPU升级1.32.0、oneTBB升级2023.2.0，KISS固定正式v1.0.2。PCL1.15.1已为最新稳定版；完整版本依据见[依赖审计](dependency-stable-audit.md)。
- std-image0.6.0、std-ml0.2.1、std-pointcloud0.3.1；产品与ABI字符串1.3.9，复用现有端口和C ABI，保持1.3系列兼容。STag仅交付MIT字典，自有解码器不编入原ED/LSD源码。

## 1.3.8

- P2 阶段2新增高度/清框/平面片规则、正交深度高度图，以及加权几何约束拟合和六轴局部位姿搜索。
- Windows CPU 三维匹配采用固定修订的 KISS-Matcher max-core/GNC 与 small_gicp GICP；新增模型模板、原场景 ROI、多实例/显式对称质量、点云边缘、可见面采样与标定投影组合。FoundationPose 单列可选 GPU 方向，本轮不编入运行库。
- std-pointcloud 0.3.0、std-image 0.5.0，产品与 ABI 字符串 1.3.8；复用原生数据和现有 C ABI，保持 1.3 系列兼容。固定 CPU 依赖源码、Windows 补丁与许可证，排除 PMC 和 LLVM 非再分发运行库。
- 新增显式参数档位及高度/拟合/匹配示例。毫米级合成样本验证与真实工业精度、遮挡容忍和现场节拍分开记录，见[后端选择](p2-3d-matching-backends.md)。

## 1.3.7

- P2 六阶段交付的第一项 A05：std-image 0.4.0 新增高光亮脊与直线段检测、碎片分组、旋转区域和支持点质量输出；灰度/RGB/RGBA、u8/u16/f32、ROI及预览坐标契约明确。
- 复用 Pose2D 集合表达旋转区域，原生图像提供亮脊掩膜及叠图；低对比、退化或质量不足不生成虚假成功，候选/支持点/计算量限制显式报错。
- 新增可运行示例与跨接口回归；产品及 ABI 字符串 1.3.7，保持 1.3 系列兼容，不增加数据种类或依赖。

具体算法、支持矩阵和剩余验收边界见 [高光源码分析](p2-highlights-analysis.md)；后续阶段及合并门禁见[六阶段跟踪](p2-sequential-delivery.md)。

## 1.3.6

- std-value 0.2.0 新增 `value.expression` 受限表达式计算，支持有界数值/布尔公式、白名单函数、单位校验及公式内部短路；图与 CLI 校验会检查完整语法和资源限制。
- 提供均值、阈值与 `flow.select` 组合示例及执行器/C ABI/C++ SDK 一致性回归。固定八个可选值输入，沿用现有图调度，不动态重建端口或增加 ABI。
- 产品和 ABI 字符串为 1.3.6，保持 1.3 系列兼容。本次交付 P2 的 A01 首项，其余扩展链仍按计划推进。

## 1.3.5

- P1 新增 55 个节点：原生点云集合、统计/PCA/包围盒、聚类与多平面、三维配准、剖面、PLY/STL；受校验位姿及集合、坐标变换；图像颜色/拼接/实例矩形掩膜、圆弧/环宽、相机标定、棋盘外参、离散 NCC 模板匹配。
- 检测链支持 YOLOv8、YOLO11、YOLO26，以及固定 YOLOX-Nano；显式区分常规检测头与端到端输出，复用原生 Tensor、ONNX Runtime 和 Pose2D 结果。SDK 提供固定公开模型准备脚本，运行库不内置权重。
- std-pointcloud、std-geometry、std-ml 为 0.2.0，std-image 为 0.3.0；产品及 ABI 字符串为 1.3.5，不新增 Data::Kind 或 C ABI 符号，保持 1.3 系列兼容。
- 增加真实模型、三维链与接口一致性门禁；小型版本化 Record 仅承载元数据，点云、索引、图像和 Tensor 保持原生数据。配准与剖面质量以实际 float32 输出重新验证。

详细支持范围和仍未覆盖的外部模式见 [P1 点云](p1-analysis-cloud.md)、[位姿](p1-analysis-pose.md)、[图像](p1-analysis-image.md)、[检测](p1-analysis-detection.md)。软件回归不代表工业现场精度、节拍或设备验收。

## 1.3.4

- 新增无 PCL/OpenCV 依赖的 std-value、std-geometry 包：有限数值/布尔/文本、有界列表、算术/比较/逻辑、原生点线圆/平面、距离/垂足/求交。采用受校验的版本化 Record，不增加 Data::Kind 或 C ABI 符号。
- std-image 0.2.0 新增通道转换、区域/颜色掩膜、整数桶统计、稳定连通域、差分/变化判定、投影选边、直线卡尺及内外宽度量测；位深、坐标、无目标和资源预算见包指南。
- 逐项核对 23 个 P0 外部算子的本机源码后独立实现；有符号间隙、垂足、变化开运算、共同边缘抑制等采用明确模式。外部工件对象、原生云集合、完整 ORB/SE(2) 差分配准及旋转工件全局宽度仍保留边界。
- 新增合成条纹的完整流程示例及执行器/C ABI 回归，CI 增加点名包的图像组合门禁；正式包清单加入两个新包和必需算子。产品与 ABI 字符串为 1.3.4，兼容 1.3 系列既有 C ABI。

契约、支持矩阵和可运行示例见 [P0 源码分析](p0-operator-analysis.md)，不代表设备或生产质量验收。

## 1.3.3

- `widthPolicy=extent` 下，彩色卡尺与粗跟踪把外侧是饱和或过暗背景（打磨面反光、黑色零件）的胶边按真边处理：边外的无信息区之后先遇到明确背景，或一直延伸到观察边界且宽度不小于邻站参考 0.55 倍时，`loComplete`/`hiComplete` 为 true，新增 `loBackground`/`hiBackground` 说明外侧背景。半宽、高光后又紧接胶色的仍为不完整。
- 宽胶面上全无信息、且不宽于两侧胶段的镜面反光带并回一条胶，可宽过 `mergeUnknownGap`；紧挨着的两段（胶里浅谷被半高拆开）直接并；合并不再按邻站参考宽设上限，局部鼓包按实际宽度输出；隔着比可见胶更宽的无信息区才出现的胶色视为另一处胶，高光与暗区 / 背景交界处一窄条过渡色（不宽于 max(4 px, 可见胶宽 / 10)）不算胶，均不再阻止认边。
- 彩色粗跟踪在一侧真边、另一侧走出 ROI / 图像时照样出胶点（有稳定宽度时取真边加半个参考宽），宽度不进历史。
- 产品与 C ABI 版本 1.3.3，glue 包 0.3.3，`glue.color_runs`/`glue.color_track` 1.2.0；默认 `range` 策略行为不变，C ABI 与 1.3 系列兼容。

验证范围为公开算子图的饱和背景认边、反光带合并、贴画面边跟踪及已有彩色回归；现场件的离线对比只说明测量完整度改善，不代表宿主生产判定或全件验收已完成。

## 1.3.2

- `glue.color_track` 与 `glue.color_runs` 新增显式 `widthPolicy=extent`，在固定搜索范围内保留超过预期宽度的完整胶条。默认 `range` 继续执行原宽度与合并上限，旧图无需迁移。
- 彩色跟踪将同等候选歧义和不完整边界保留为无信息证据，不以观测片段中点拟合胶路。真实背景仍阻断合并。
- 彩色卡尺在新策略中区分搜索端点、ROI、图像、高光和暗区原因；既有外边完成标记保持有效。详见[彩色算子契约](glue-measure-ops.md#134-gluecolor_track-彩色粗跟踪)。
- 产品与 C ABI 版本统一为 1.3.2，glue 包为 0.3.2；C ABI 与 1.3 系列兼容。算子策略需由宿主明确选择，不依据 DLL 版本静默改变测量行为。

验证范围为公开算子图的宽胶/局部鼓包、反光裂分/背景双胶、边界截断回归及已有彩色回归；不代表宿主生产判定或现场全件验收已完成。
