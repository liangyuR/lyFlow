# std-profile 逐算子迁移清单

状态：实施中；当前 10 项。原251/7规划基线通过机器清单baselineReference追溯。返回[清单总览](../operator-package-audit.md)。

下列顺序是迁移/验收顺序，不是执行图调度顺序。精确端口和参数见[机器清单](../operator-package-audit.json)。

## 020 · profile.from_cloud

- 当前：`std-profile` / `1.0.0` / 点云显式转换轮廓。目标：`std-profile`；保留并验边界；**M1**。
- 源码：[packs/std-profile/ops/register.cpp:171](../../packs/std-profile/ops/register.cpp#L171)。
- 端口：`cloud:PointCloud` → `profile:Profile`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core公开Profile/几何/Record契约与C++标准库；无PCL/OpenCV/ONNX Runtime直接依赖
- 当前契约：显式声明frame、m/mm、plane、handedness、sampleOrder与sourceId；共享只读PointCloud，保留全部槽与可选通道。有限XYZ标validity=1，NaN标0，来源索引0..N-1；不排序、不换轴、不压缩。
- 目标契约：原生Profile公共适配，8字段描述与validity/sourceIndices独立数组；调用方确认acquisition，不从点序猜测采集语义。
- 迁移前置：M0
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：1280槽/NaN/可选通道保留；缺单位/帧拒绝，Inf及非法通道拒绝；共享输入不变；轻量无PCL/CV/ORT。
- 说明：M1新增显式契约适配；不替代M3轮廓与DTS场景算法迁移。

## 021 · profile.remap_axes

- 当前：`std-profile` / `1.0.0` / 轮廓显式轴映射。目标：`std-profile`；保留并验边界；**M1**。
- 源码：[packs/std-profile/ops/register.cpp:218](../../packs/std-profile/ops/register.cpp#L218)。
- 端口：`profile:Profile` → `profile:Profile`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core公开Profile/几何/Record契约与C++标准库；无PCL/OpenCV/ONNX Runtime直接依赖
- 当前契约：按明确轴排列及+1/-1符号映射XYZ和法线，更新目标frame、plane与handedness；单位、有效性、槽序、sourceId和sourceIndices不变。
- 目标契约：反射与轴置换通过显式Profile适配，不伪装右手刚体Transform；不排序、不丢NaN或有限无效槽。
- 迁移前置：M0
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：6种轴排列及符号奇偶、反射往返、法线和源索引一致；非法符号或目标帧失败不发布输出；输入不变。
- 说明：M1新增显式契约适配；不替代M3轮廓与DTS场景算法迁移。

## 022 · profile.to_cloud

- 当前：`std-profile` / `1.0.0` / 轮廓显式导出点云。目标：`std-profile`；保留并验边界；**M1**。
- 源码：[packs/std-profile/ops/register.cpp:199](../../packs/std-profile/ops/register.cpp#L199)。
- 端口：`profile:Profile` → `cloud:PointCloud`, `descriptor:Record`, `profile:Profile`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core公开Profile/几何/Record契约与C++标准库；无PCL/OpenCV/ONNX Runtime直接依赖
- 当前契约：显式m/mm换算XYZ，法线不缩放且轴不旋转；裸PointCloud无效槽置NaN，同时输出完整Profile与仅含小元数据的profile.Descriptor.v1 Record。
- 目标契约：标准裸云消费者使用明确米制；完整Profile维持原始无效载荷、validity和sourceIndices，禁止宣称裸云+小Record可无损往返。
- 迁移前置：M0
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：mm↔m往返、有限无效槽仅裸云置NaN、通道/索引保留；溢出与取消不发布部分结果，上游缓冲不变。
- 说明：M1新增显式契约适配；不替代M3轮廓与DTS场景算法迁移。

## 063 · profile.input

- 当前：`std-profile` / `1.0.0` / 原生轮廓输入。目标：`std-profile`；保留并验边界；**M3**。
- 源码：[packs/std-profile/ops/input.cpp:253](../../packs/std-profile/ops/input.cpp#L253)。
- 端口：无 → `profile:Profile`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：公开Profile/几何/Record契约与C++17；分析内核仅编译期Eigen5.0.1，无PCL/OpenCV/ORT
- 当前契约：显式描述的原生Profile输入；CSV v1严格解析指定平面两轴与可选强度，NaN保槽，Inf/非法行拒绝；宿主可注入完整LYPR。
- 目标契约：显式描述的原生Profile输入；CSV v1严格解析指定平面两轴与可选强度，NaN保槽，Inf/非法行拒绝；宿主可注入完整LYPR。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：CSV各平面/m和mm/NaN/通道/预算/取消/中文路径；完整LYPR跨CLI与独立宿主一致，坏输入不运行不落部分输出。
- 说明：M3明确新增公共能力；保留既有数值方法与源契约，非旧ID别名或第二份场景算法。
- M3边界验收（manifest_verified）：原生Profile公共链、DTS场景v2与显式旧图迁移；旧源输入壳保留至M7以支持既有宿主注入。
- 阶段依赖边界：仅标准库和编译期Eigen；无PCL/OpenCV/ORT；场景DTS依赖公开轮廓/度量核及原生Profile契约。
- 公开接口：kernels/include/lyflow/kernel/profile.h；kernels/include/lyflow/kernel/profile_analysis.h；kernels/include/lyflow/kernel/metrology.h；packs/std-profile/include/lyflow_profile/context.h
- 阶段验收边界：真实全包manifest描述核验；完整数值、SDK、CLI与图升级回归另由运行证据证明。

## 064 · profile.clean

- 当前：`std-profile` / `1.0.0` / 有序轮廓清洗。目标：`std-profile`；保留并验边界；**M3**。
- 源码：[packs/std-profile/ops/processing.cpp:238](../../packs/std-profile/ops/processing.cpp#L238)。
- 端口：`profile:Profile` → `clean:Profile`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：公开Profile/几何/Record契约与C++17；分析内核仅编译期Eigen5.0.1，无PCL/OpenCV/ORT
- 当前契约：原始槽邻域中值后按强度与有限性降有效掩码；默认preserve_slots，显式compact保留原sourceCount/sourceIndices和可选通道，不排序。
- 目标契约：原始槽邻域中值后按强度与有限性降有效掩码；默认preserve_slots，显式compact保留原sourceCount/sourceIndices和可选通道，不排序。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：无效中心不补值；缺样/有限无效/端点/even窗口/附加通道/输入只读/取消；旧DTS压缩有效数值等价。
- 说明：M3明确新增公共能力；保留既有数值方法与源契约，非旧ID别名或第二份场景算法。
- M3边界验收（manifest_verified）：原生Profile公共链、DTS场景v2与显式旧图迁移；旧源输入壳保留至M7以支持既有宿主注入。
- 阶段依赖边界：仅标准库和编译期Eigen；无PCL/OpenCV/ORT；场景DTS依赖公开轮廓/度量核及原生Profile契约。
- 公开接口：kernels/include/lyflow/kernel/profile.h；kernels/include/lyflow/kernel/profile_analysis.h；kernels/include/lyflow/kernel/metrology.h；packs/std-profile/include/lyflow_profile/context.h
- 阶段验收边界：真实全包manifest描述核验；完整数值、SDK、CLI与图升级回归另由运行证据证明。

## 065 · profile.split_faces

- 当前：`std-profile` / `1.0.0` / 有序轮廓分片与直线面。目标：`std-profile`；保留并验边界；**M3**。
- 源码：[packs/std-profile/ops/processing.cpp:263](../../packs/std-profile/ops/processing.cpp#L263)。
- 端口：`clean:Profile` → `faces:Record`。
- 参数单位标注：gapMm=mm, jumpMm=mm, fitLenMm=mm, maxRmsMm=mm, angleBreakDeg=deg, minLenMm=mm。
- 依赖：公开Profile/几何/Record契约与C++17；分析内核仅编译期Eigen5.0.1，无PCL/OpenCV/ORT
- 当前契约：acquisition轮廓转mm工作视图，压缩有效样本后按相邻采样间断分片，片内稳定x排序；profile.Faces.v1绑定完整源契约/hash及槽/源索引。
- 目标契约：acquisition轮廓转mm工作视图，压缩有效样本后按相邻采样间断分片，片内稳定x排序；profile.Faces.v1绑定完整源契约/hash及槽/源索引。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：mm/m及各分析平面、缺槽/重复x/重叠面/RMS与角度严格边界；不修改源数组，faces与另一轮廓混配拒绝。
- 说明：M3明确新增公共能力；保留既有数值方法与源契约，非旧ID别名或第二份场景算法。
- M3边界验收（manifest_verified）：原生Profile公共链、DTS场景v2与显式旧图迁移；旧源输入壳保留至M7以支持既有宿主注入。
- 阶段依赖边界：仅标准库和编译期Eigen；无PCL/OpenCV/ORT；场景DTS依赖公开轮廓/度量核及原生Profile契约。
- 公开接口：kernels/include/lyflow/kernel/profile.h；kernels/include/lyflow/kernel/profile_analysis.h；kernels/include/lyflow/kernel/metrology.h；packs/std-profile/include/lyflow_profile/context.h
- 阶段验收边界：真实全包manifest描述核验；完整数值、SDK、CLI与图升级回归另由运行证据证明。

## 066 · profile.fit_line

- 当前：`std-profile` / `1.0.0` / 原生轮廓正交直线拟合。目标：`std-profile`；保留并验边界；**M3**。
- 源码：[packs/std-profile/ops/native_analysis.cpp:224](../../packs/std-profile/ops/native_analysis.cpp#L224)。
- 端口：`profile:Profile` → `line:Line2D`, `statistics:Record`, `lineContext:Record`。
- 参数单位标注：range=m, maxResidual=m。
- 依赖：公开Profile/几何/Record契约与C++17；分析内核仅编译期Eigen5.0.1，无PCL/OpenCV/ORT
- 当前契约：按原生Profile声明平面和m/mm显式换算SI，TLS拟合；默认拒绝无效槽，valid_only显式过滤；输出米制Line2D及源绑定统计/几何上下文。
- 目标契约：按原生Profile声明平面和m/mm显式换算SI，TLS拟合；默认拒绝无效槽，valid_only显式过滤；输出米制Line2D及源绑定统计/几何上下文。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：TLS浮点收窄后RMS、退化/范围/残差/NaN策略、通道有限性；统计与线hash对应、输入只读。
- 说明：M3明确新增公共能力；保留既有数值方法与源契约，非旧ID别名或第二份场景算法。
- M3边界验收（manifest_verified）：原生Profile公共链、DTS场景v2与显式旧图迁移；旧源输入壳保留至M7以支持既有宿主注入。
- 阶段依赖边界：仅标准库和编译期Eigen；无PCL/OpenCV/ORT；场景DTS依赖公开轮廓/度量核及原生Profile契约。
- 公开接口：kernels/include/lyflow/kernel/profile.h；kernels/include/lyflow/kernel/profile_analysis.h；kernels/include/lyflow/kernel/metrology.h；packs/std-profile/include/lyflow_profile/context.h
- 阶段验收边界：真实全包manifest描述核验；完整数值、SDK、CLI与图升级回归另由运行证据证明。

## 067 · profile.transform_pair

- 当前：`std-profile` / `1.0.0` / 原生双轮廓已知外参变换。目标：`std-profile`；保留并验边界；**M3**。
- 源码：[packs/std-profile/ops/native_analysis.cpp:254](../../packs/std-profile/ops/native_analysis.cpp#L254)。
- 端口：`master:Profile`, `slave:Profile`, `masterToCommon:Transform`, `slaveToCommon:Transform`, `commonToOutput:Transform?` → `master:Profile`, `slave:Profile`, `metadata:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：公开Profile/几何/Record契约与C++17；分析内核仅编译期Eigen5.0.1，无PCL/OpenCV/ORT
- 当前契约：显式共同帧、输出帧和平面及三段刚体变换；输出SI Profile并保槽/掩码/通道/源索引，valid_only无效槽仅换单位而不冒充观测。
- 目标契约：显式共同帧、输出帧和平面及三段刚体变换；输出SI Profile并保槽/掩码/通道/源索引，valid_only无效槽仅换单位而不冒充观测。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：变换组合顺序、proper rigid、法线旋转、源身份/手性、无效槽策略及失败原子性。
- 说明：M3明确新增公共能力；保留既有数值方法与源契约，非旧ID别名或第二份场景算法。
- M3边界验收（manifest_verified）：原生Profile公共链、DTS场景v2与显式旧图迁移；旧源输入壳保留至M7以支持既有宿主注入。
- 阶段依赖边界：仅标准库和编译期Eigen；无PCL/OpenCV/ORT；场景DTS依赖公开轮廓/度量核及原生Profile契约。
- 公开接口：kernels/include/lyflow/kernel/profile.h；kernels/include/lyflow/kernel/profile_analysis.h；kernels/include/lyflow/kernel/metrology.h；packs/std-profile/include/lyflow_profile/context.h
- 阶段验收边界：真实全包manifest描述核验；完整数值、SDK、CLI与图升级回归另由运行证据证明。

## 076 · profile.analyze

- 当前：`std-profile` / `1.0.0` / 单剖面直线分析。目标：`std-profile`；迁移归属；**M3**。
- 源码：[packs/std-profile/ops/analysis.cpp:69](../../packs/std-profile/ops/analysis.cpp#L69)。
- 端口：`cloud:PointCloud` → `line:Line2D`, `statistics:Record`。
- 参数单位标注：maxResidual=m。
- 依赖：Eigen二维TLS/PCA；p1_common校验，算法不使用PCL也不依赖采样顺序；当前std-pointcloud整包传递PCL；目标std-profile轻量模块
- 当前契约：cloud:PointCloud → line:Line2D, statistics:Record。单云选择XY/XZ/YZ平面做全点正交TLS；米制Line2D与profile.Line.v1含frame/plane/range/RMS；少于3点、各向同性或残差大拒绝。
- 目标契约：迁入std-profile的单扫描分析；Profile声明平面/单位与sample-order能力，此旧算法接受order=unknown且不擅自排序，保留TLS质量门。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：旧PointCloud输入和显式Profile适配读数一致；点序置换、三平面、退化、RMS门限与米/毫米显式转换。
- 说明：分析不要求列序，不可因std-profile处理有序轮廓就给旧算法新增隐含前提。
- M3边界验收（manifest_verified）：原生Profile公共链、DTS场景v2与显式旧图迁移；旧源输入壳保留至M7以支持既有宿主注入。
- 阶段依赖边界：仅标准库和编译期Eigen；无PCL/OpenCV/ORT；场景DTS依赖公开轮廓/度量核及原生Profile契约。
- 公开接口：kernels/include/lyflow/kernel/profile.h；kernels/include/lyflow/kernel/profile_analysis.h；kernels/include/lyflow/kernel/metrology.h；packs/std-profile/include/lyflow_profile/context.h
- 阶段验收边界：真实全包manifest描述核验；完整数值、SDK、CLI与图升级回归另由运行证据证明。
- M3迁包来源：`std-pointcloud` / `packs/std-pointcloud/ops/profile_analysis.cpp:84` @ `36c969149e119614b7d221c8006d1a311d6d2995`；原目标包和依赖规划保留。

## 078 · profile.project_pair

- 当前：`std-profile` / `1.0.0` / 双剖面已知外参变换。目标：`std-profile`；迁移归属；**M3**。
- 源码：[packs/std-profile/ops/analysis.cpp:110](../../packs/std-profile/ops/analysis.cpp#L110)。
- 端口：`master:PointCloud`, `slave:PointCloud`, `masterToCommon:Transform`, `slaveToCommon:Transform`, `commonToOutput:Transform?` → `master:PointCloud`, `slave:PointCloud`, `metadata:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：Eigen/原生数组刚体变换；无PCL搜索；目标std-profile输入适配复用公共几何接口
- 当前契约：master:PointCloud, slave:PointCloud, masterToCommon:Transform, slaveToCommon:Transform, commonToOutput:Transform → master:PointCloud, slave:PointCloud, metadata:Record。主/副米制点云及显式masterToCommon/slaveToCommon/commonToOutput；应用output←common←sensor，保留属性、旋转法线与点序，不猜传感器外参。
- 目标契约：迁std-profile的多扫描帧适配；每条轮廓独立保持sample-order，明确两个源帧及公共/输出帧，不自动换配方中心或单位。
- 迁移前置：M1；M2；transform.make
- 关联算子：transform.make
- 验收：非交换三段外参、两个扫描顺序与属性、法线、未知帧/单位拒绝和输入只读。
- 说明：属于传感器轮廓数据适配，不能简单以两个独立transform.apply丢失双源元数据。
- M3边界验收（manifest_verified）：原生Profile公共链、DTS场景v2与显式旧图迁移；旧源输入壳保留至M7以支持既有宿主注入。
- 阶段依赖边界：仅标准库和编译期Eigen；无PCL/OpenCV/ORT；场景DTS依赖公开轮廓/度量核及原生Profile契约。
- 公开接口：kernels/include/lyflow/kernel/profile.h；kernels/include/lyflow/kernel/profile_analysis.h；kernels/include/lyflow/kernel/metrology.h；packs/std-profile/include/lyflow_profile/context.h
- 阶段验收边界：真实全包manifest描述核验；完整数值、SDK、CLI与图升级回归另由运行证据证明。
- M3迁包来源：`std-pointcloud` / `packs/std-pointcloud/ops/profile_analysis.cpp:201` @ `36c969149e119614b7d221c8006d1a311d6d2995`；原目标包和依赖规划保留。
