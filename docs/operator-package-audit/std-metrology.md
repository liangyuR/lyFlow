# std-metrology 逐算子迁移清单

状态：实施中；当前 4 项。原251/7规划基线通过机器清单baselineReference追溯。返回[清单总览](../operator-package-audit.md)。

下列顺序是迁移/验收顺序，不是执行图调度顺序。精确端口和参数见[机器清单](../operator-package-audit.json)。

## 060 · image.calibrated_distance

- 当前：`std-metrology` / `1.1.0` / 平面标定距离量测。目标：`std-metrology`；迁移归属；**M2**。
- 源码：[packs/std-metrology/ops/register.cpp:29](../../packs/std-metrology/ops/register.cpp#L29)。
- 端口：`a:Point2D`, `b:Point2D`, `calib:Record` → `distance:Measurement`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前compute：标准C++、公共calibration::Plane解析及标量H映射，不使用OpenCV；同TU其他节点使用OpenCV。；目标：轻量几何/标定模型公共核，不链接OpenCV/PCL/ORT。
- 当前契约：输入[a:Point2D, b:Point2D, calib:Record] → 输出[distance:Measurement]。两个原图像素Point2D经image.PlaneCalib映射到平面单位，再乘metersPerUnit，输出单位m的Measurement；单位/单应无效或非有限结果诊断。
- 目标契约：两个像素点经PlaneCalib得到米制距离迁std-metrology；公共单应标量映射核无OpenCV，不靠图像包注册。
- 迁移前置：M1；std-metrology量纲与Measurement契约；公共PlaneCalib验证与单应标量映射；M1统一PlaneCalib单位及齐次矩阵验证，提供旧输入显式迁移
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证m/mm标定等价、px点单位拒绝、单应奇点和米制Measurement。
- 说明：主职责为具有标定基准的物理距离，图像像素坐标是输入域；必须保留先映射再换米，不能直接用像素距离乘常数。 现有PlaneCalib有两套验证器：lyflow_cv/plane_calib.h默认mm且允许非空unit，core/calibration.h要求显式m/mm并归一化H。统一校验属于契约变更，不能在搬包时静默替换。
- M1契约更新（1.1.0）：新增显式frame参数，版本化PlaneCalib使用共享schemaVersion=1/frame/unit解析；旧非版本化分支保持兼容。；验收：v1帧缺失/不匹配拒绝；米/毫米一致；旧参数与旧标定路径不漂移。
- M2边界验收（manifest_verified）：M2公共核、轻量包归属与声明式依赖边界；原ID/版本/数据数值与错误语义保持。
- 阶段依赖边界：std-metrology -> metrology-kernel -> data-contract；C++17/std及公开core校验，无OpenCV/PCL/ORT。
- 公开接口：kernels/include/lyflow/kernel/metrology.h；core/include/lyflow/calibration.h
- 阶段验收边界：当前描述已与真实全包manifest核验；独立静态核、无重后端、既有算子与安装SDK数值回归由阶段验证证据记录，不代表M3场景算法迁移。
- M2迁包来源：`std-image` / `packs/std-image/ops/image_p1_calib.cpp:445` @ `f0c043c6341755e8f254ac499a1593df087b6d94`；原目标包和依赖规划保留。

## 068 · metrology.profile_gap_flush

- 当前：`std-metrology` / `1.0.0` / 轮廓拟合结果间隙与面差。目标：`std-metrology`；保留并验边界；**M3**。
- 源码：[packs/std-metrology/ops/profile_metrology.cpp:184](../../packs/std-metrology/ops/profile_metrology.cpp#L184)。
- 端口：`left:Line2D`, `right:Line2D`, `leftStatistics:Record`, `rightStatistics:Record` → `gap:Measurement`, `flush:Measurement`, `quality:Record`。
- 参数单位标注：referenceX=m, maxResidual=m, maxExtrapolation=m。
- 依赖：公开Profile/几何/Record契约与C++17；分析内核仅编译期Eigen5.0.1，无PCL/OpenCV/ORT
- 当前契约：消费米制左右拟合线与各自profile.LineFit.v1，验证线hash/帧/平面/手性；间隙取实采样边缘差，面差为referenceX处右减左，不当作法向距离。
- 目标契约：消费米制左右拟合线与各自profile.LineFit.v1，验证线hash/帧/平面/手性；间隙取实采样边缘差，面差为referenceX处右减左，不当作法向距离。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：左右/符号/采样范围/RMS/外推/竖直退化及上下文混配；Measurement单位m，质量不替生产判据。
- 说明：M3明确新增公共能力；保留既有数值方法与源契约，非旧ID别名或第二份场景算法。
- M3边界验收（manifest_verified）：原生Profile公共链、DTS场景v2与显式旧图迁移；旧源输入壳保留至M7以支持既有宿主注入。
- 阶段依赖边界：仅标准库和编译期Eigen；无PCL/OpenCV/ORT；场景DTS依赖公开轮廓/度量核及原生Profile契约。
- 公开接口：kernels/include/lyflow/kernel/profile.h；kernels/include/lyflow/kernel/profile_analysis.h；kernels/include/lyflow/kernel/metrology.h；packs/std-profile/include/lyflow_profile/context.h
- 阶段验收边界：真实全包manifest描述核验；完整数值、SDK、CLI与图升级回归另由运行证据证明。

## 069 · metrology.signed_point_line

- 当前：`std-metrology` / `1.0.0` / 有符号点线距离。目标：`std-metrology`；保留并验边界；**M3**。
- 源码：[packs/std-metrology/ops/profile_metrology.cpp:207](../../packs/std-metrology/ops/profile_metrology.cpp#L207)。
- 端口：`point:Point2D`, `line:Line2D`, `pointContext:Record`, `lineContext:Record` → `distance:Measurement`, `foot:Point2D`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：公开Profile/几何/Record契约与C++17；分析内核仅编译期Eigen5.0.1，无PCL/OpenCV/ORT
- 当前契约：米制点线及各自profile.Geometry.v1，验证帧/平面/手性/实际几何hash；指定正法向的有符号距离与垂足，公共核支持显式方向规范。
- 目标契约：米制点线及各自profile.Geometry.v1，验证帧/平面/手性/实际几何hash；指定正法向的有符号距离与垂足，公共核支持显式方向规范。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：符号/反向/垂直/非法方向/不同单位帧与非有限拒绝；DTS复用DeclaredUnit保持旧float方向数值路径。
- 说明：M3明确新增公共能力；保留既有数值方法与源契约，非旧ID别名或第二份场景算法。
- M3边界验收（manifest_verified）：原生Profile公共链、DTS场景v2与显式旧图迁移；旧源输入壳保留至M7以支持既有宿主注入。
- 阶段依赖边界：仅标准库和编译期Eigen；无PCL/OpenCV/ORT；场景DTS依赖公开轮廓/度量核及原生Profile契约。
- 公开接口：kernels/include/lyflow/kernel/profile.h；kernels/include/lyflow/kernel/profile_analysis.h；kernels/include/lyflow/kernel/metrology.h；packs/std-profile/include/lyflow_profile/context.h
- 阶段验收边界：真实全包manifest描述核验；完整数值、SDK、CLI与图升级回归另由运行证据证明。

## 077 · profile.gap_flush

- 当前：`std-metrology` / `1.0.0` / 单剖面间隙与面差。目标：`std-metrology`；拆公共能力/适配；**M3**。
- 源码：[packs/std-metrology/ops/profile_gap_flush.cpp:78](../../packs/std-metrology/ops/profile_gap_flush.cpp#L78)。
- 端口：`cloud:PointCloud` → `gap:Measurement`, `flush:Measurement`, `left:Line2D`, `right:Line2D`, `quality:Record`。
- 参数单位标注：leftRange=m, rightRange=m, referenceX=m, maxResidual=m, maxExtrapolation=m。
- 依赖：Eigen二维TLS与纯标量测量；不使用PCL；目标依赖std-profile的有界拟合结果与std-metrology测量内核，不反向依赖点云重后端
- 当前契约：cloud:PointCloud → gap:Measurement, flush:Measurement, left:Line2D, right:Line2D, quality:Record。单扫描XY/XZ/YZ米制；左右显式不相交横坐标区间拟合，gap=右采样最小x−左采样最大x，flush=referenceX处右线−左线；限制RMS/外推并拒绝竖直线。
- 目标契约：主归std-metrology，拆出轮廓区域拟合和采样边界定义；显式基准轴/符号/单位/外推范围，保持采样边缘间距与同横坐标面差两个定义。
- 迁移前置：M1；M2；profile.analyze
- 关联算子：profile.analyze
- 验收：左右区间/符号/换轴、采样稀疏边缘、竖直线、超外推、残差拒绝；拆分图与原节点读数/诊断一致。
- 说明：不能用最近点距离、法向垂距或gap领域圆角量测直接替换；不增加业务OK/NG规则。
- M3边界验收（manifest_verified）：原生Profile公共链、DTS场景v2与显式旧图迁移；旧源输入壳保留至M7以支持既有宿主注入。
- 阶段依赖边界：仅标准库和编译期Eigen；无PCL/OpenCV/ORT；场景DTS依赖公开轮廓/度量核及原生Profile契约。
- 公开接口：kernels/include/lyflow/kernel/profile.h；kernels/include/lyflow/kernel/profile_analysis.h；kernels/include/lyflow/kernel/metrology.h；packs/std-profile/include/lyflow_profile/context.h
- 阶段验收边界：真实全包manifest描述核验；完整数值、SDK、CLI与图升级回归另由运行证据证明。
- M3迁包来源：`std-pointcloud` / `packs/std-pointcloud/ops/profile_analysis.cpp:126` @ `36c969149e119614b7d221c8006d1a311d6d2995`；原目标包和依赖规划保留。
