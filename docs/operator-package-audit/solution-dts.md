# solution-dts 逐算子迁移清单

状态：实施中；当前 7 项。原251/7规划基线通过机器清单baselineReference追溯。返回[清单总览](../operator-package-audit.md)。

下列顺序是迁移/验收顺序，不是执行图调度顺序。精确端口和参数见[机器清单](../operator-package-audit.json)。

## 024 · dts.geometry_to_metric

- 当前：`solution-dts` / `2.0.0` / DTS几何显式契约适配。目标：`solution-dts`；保留场景职责；**M1**。
- 源码：[solutions/dts/ops/geometry_adapter.cpp:69](../../solutions/dts/ops/geometry_adapter.cpp#L69)。
- 端口：`geometry:Any` → `geometry:Any`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core公开Profile/几何/Record契约与C++标准库；无PCL/OpenCV/ONNX Runtime直接依赖
- 当前契约：调用方显式声明dts.geometry.v1及旧生产者；旧DTS点/线位置和端点毫米数值乘0.001，方向不缩放，保留原XZ解释；不处理Measurement。
- 目标契约：M1迁移桥接保留在场景适配层，M3按来源/旧版本插入或移除，不能将旧Meter标签视为真实米制或重复缩放。
- 迁移前置：M0
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：旧毫米+Meter夹具转米、方向/垂足/端点一致；Pixel/类型不匹配/非有限输入拒绝且无部分输出；旧业务符号保持。
- 说明：M1新增显式契约适配；不替代M3轮廓与DTS场景算法迁移。
- M3契约更新（2.0.0）：场景v2使用显式Profile的XZ/acquisition/frame/hand/source上下文；工作数值mm但裸几何真实m；Record与实际几何hash绑定。旧v1不自动猜单位，显式dtsProfile.v1整图升级保留资产。；验收：既有默认/相邻基准和失败行为不变；签名、源索引、单位及桥v1/v2避免重复缩放；独立宿主数值一致。
- M3边界验收（manifest_verified）：原生Profile公共链、DTS场景v2与显式旧图迁移；旧源输入壳保留至M7以支持既有宿主注入。
- 阶段依赖边界：仅标准库和编译期Eigen；无PCL/OpenCV/ORT；场景DTS依赖公开轮廓/度量核及原生Profile契约。
- 公开接口：kernels/include/lyflow/kernel/profile.h；kernels/include/lyflow/kernel/profile_analysis.h；kernels/include/lyflow/kernel/metrology.h；packs/std-profile/include/lyflow_profile/context.h
- 阶段验收边界：真实全包manifest描述核验；完整数值、SDK、CLI与图升级回归另由运行证据证明。
- M3迁包来源：`dts` / `packs/dts/ops/geometry_adapter.cpp:54` @ `36c969149e119614b7d221c8006d1a311d6d2995`；原目标包和依赖规划保留。

## 070 · dts.profile_in

- 当前：`solution-dts` / `1.0.0` / 激光轮廓输入。目标：`std-profile`；拆公共能力/适配；**M3**。
- 源码：[solutions/dts/ops/profile_in.cpp:69](../../solutions/dts/ops/profile_in.cpp#L69)。
- 端口：无 → `profile:PointCloud`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前直接：标准库 CSV I/O；宿主注入由 core 提供；当前构建：core + dts，无PCL/OpenCV/ONNX Runtime。；目标：轮廓基础模块不引PCL/OpenCV/ONNX；明确标注的原PCL拟合方法作为可选后端隔离。
- 当前契约：输入 无 → 输出 profile:PointCloud。CSV 行为 x,z[,亮度]，坐标 mm，y=0；宿主注入时整个 compute 跳过。
- 目标契约：通用轮廓 CSV 输入与注入共用 Profile 契约；携带单位、平面、列序/槽号。DTS 默认路径与旧注入目标由场景适配；禁止隐式改成米。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：CSV/注入同一轮廓得到一致值；NaN、亮度和原始列号保留；旧注入目标显式迁移。
- 说明：profile_in 不能只替换 id：宿主是按节点/端口注入，迁移包含宿主与保存图。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。
- M3边界验收（manifest_verified）：原生Profile公共链、DTS场景v2与显式旧图迁移；旧源输入壳保留至M7以支持既有宿主注入。
- 阶段依赖边界：仅标准库和编译期Eigen；无PCL/OpenCV/ORT；场景DTS依赖公开轮廓/度量核及原生Profile契约。
- 公开接口：kernels/include/lyflow/kernel/profile.h；kernels/include/lyflow/kernel/profile_analysis.h；kernels/include/lyflow/kernel/metrology.h；packs/std-profile/include/lyflow_profile/context.h
- 阶段验收边界：真实全包manifest描述核验；完整数值、SDK、CLI与图升级回归另由运行证据证明。
- M3迁包来源：`dts` / `packs/dts/ops/profile_in.cpp:69` @ `36c969149e119614b7d221c8006d1a311d6d2995`；原目标包和依赖规划保留。

## 071 · dts.seal_dome

- 当前：`solution-dts` / `2.0.0` / 胶条主体提取。目标：`solution-dts`；保留场景职责；**M3**。
- 源码：[solutions/dts/ops/seal_dome.cpp:74](../../solutions/dts/ops/seal_dome.cpp#L74)。
- 端口：`clean:Profile`, `faces:Record` → `dome:Record`, `apex:Line2D`。
- 参数单位标注：minLenMm=mm, maxLenMm=mm, minHeightMm=mm, maxRadiusMm=mm, walkTolMm=mm, apexFitHalfMm=mm, radiusMinMm=mm, radiusMaxMm=mm。
- 依赖：当前直接：标准库轮廓搜索/圆拟合；依赖 std-profile 计划能力；当前构建：core + dts，无PCL/OpenCV/ONNX Runtime。；目标：std-profile/std-metrology按实际调用启用；保持DTS轻量零重依赖。
- 当前契约：输入 clean:PointCloud, faces:Record → 输出 dome:Record, apex:Line2D。输入 DtsFaces；按胶条宽高和圆半径挑主体凸起，apex 为圆心到顶点的轴；失败有胶条专用诊断。 历史不一致：旧Point2D/Line2D载荷数值为毫米，但Unit2D仅有Meter/Pixel且这些节点未覆盖默认Meter标签，不能按标签直接消费。
- 目标契约：保留胶条主体选择与半径带规则；圆拟合内核下沉 std-profile，DtsDome/apex 由场景适配通用几何。 迁移按DTS来源/旧版本显式将位置/长度乘0.001并记录XZ平面映射，不能直接信任旧Meter标签；Measurement输出单位另按其自身unit处理。
- 迁移前置：M1；M2；dts.split_faces
- 关联算子：dts.split_faces
- 验收：多凸起、胶条遮挡、半径越界和无顶点轴案例保留诊断；基准面选择不变。 增加旧毫米数值+Meter标签夹具，验证0.001换算、往返和1000倍错量拒绝；方向不随长度缩放。
- 说明：胶条选择不是通用最大峰，不能用单个通用极值算子直接替换。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。
- M3契约更新（2.0.0）：场景v2使用显式Profile的XZ/acquisition/frame/hand/source上下文；工作数值mm但裸几何真实m；Record与实际几何hash绑定。旧v1不自动猜单位，显式dtsProfile.v1整图升级保留资产。；验收：既有默认/相邻基准和失败行为不变；签名、源索引、单位及桥v1/v2避免重复缩放；独立宿主数值一致。
- M3边界验收（manifest_verified）：原生Profile公共链、DTS场景v2与显式旧图迁移；旧源输入壳保留至M7以支持既有宿主注入。
- 阶段依赖边界：仅标准库和编译期Eigen；无PCL/OpenCV/ORT；场景DTS依赖公开轮廓/度量核及原生Profile契约。
- 公开接口：kernels/include/lyflow/kernel/profile.h；kernels/include/lyflow/kernel/profile_analysis.h；kernels/include/lyflow/kernel/metrology.h；packs/std-profile/include/lyflow_profile/context.h
- 阶段验收边界：真实全包manifest描述核验；完整数值、SDK、CLI与图升级回归另由运行证据证明。
- M3迁包来源：`dts` / `packs/dts/ops/seal_dome.cpp:64` @ `36c969149e119614b7d221c8006d1a311d6d2995`；原目标包和依赖规划保留。

## 072 · dts.pick_metal

- 当前：`solution-dts` / `2.0.0` / 钣金基准面提取。目标：`solution-dts`；保留场景职责；**M3**。
- 源码：[solutions/dts/ops/pick_metal.cpp:99](../../solutions/dts/ops/pick_metal.cpp#L99)。
- 端口：`clean:Profile`, `faces:Record`, `dome:Record` → `metal:Line2D`, `pick:Record`, `metalContext:Record`。
- 参数单位标注：angleToApexDeg=deg, angleTolDeg=deg, fitLenMm=mm。
- 依赖：当前直接：标准库局部线拟合；计划调用 std-profile；当前构建：core + dts，无PCL/OpenCV/ONNX Runtime。；目标：std-profile/std-metrology按实际调用启用；保持DTS轻量零重依赖。
- 当前契约：输入 clean:PointCloud, faces:Record, dome:Record → 输出 metal:Line2D, pick:Record。按相对胶条顶点轴的角度/距离挑钣金面，只在朝胶条的一小段拟合；保留 rejected 候选。 历史不一致：旧Point2D/Line2D载荷数值为毫米，但Unit2D仅有Meter/Pixel且这些节点未覆盖默认Meter标签，不能按标签直接消费。
- 目标契约：钣金族面挑选留场景；通用局部拟合可下沉；DtsMetalPick 保留候选、拒绝原因和基准来源。 迁移按DTS来源/旧版本显式将位置/长度乘0.001并记录XZ平面映射，不能直接信任旧Meter标签；Measurement输出单位另按其自身unit处理。
- 迁移前置：dts.seal_dome；dts.split_faces
- 关联算子：dts.seal_dome；dts.split_faces
- 验收：两族面约70度差的实测轮廓、局部弯曲和候选拒绝理由不丢失。 增加旧毫米数值+Meter标签夹具，验证0.001换算、往返和1000倍错量拒绝；方向不随长度缩放。
- 说明：不能用最近线段替代角度/胶条侧约束。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。
- M3契约更新（2.0.0）：场景v2使用显式Profile的XZ/acquisition/frame/hand/source上下文；工作数值mm但裸几何真实m；Record与实际几何hash绑定。旧v1不自动猜单位，显式dtsProfile.v1整图升级保留资产。；验收：既有默认/相邻基准和失败行为不变；签名、源索引、单位及桥v1/v2避免重复缩放；独立宿主数值一致。
- M3边界验收（manifest_verified）：原生Profile公共链、DTS场景v2与显式旧图迁移；旧源输入壳保留至M7以支持既有宿主注入。
- 阶段依赖边界：仅标准库和编译期Eigen；无PCL/OpenCV/ORT；场景DTS依赖公开轮廓/度量核及原生Profile契约。
- 公开接口：kernels/include/lyflow/kernel/profile.h；kernels/include/lyflow/kernel/profile_analysis.h；kernels/include/lyflow/kernel/metrology.h；packs/std-profile/include/lyflow_profile/context.h
- 阶段验收边界：真实全包manifest描述核验；完整数值、SDK、CLI与图升级回归另由运行证据证明。
- M3迁包来源：`dts` / `packs/dts/ops/pick_metal.cpp:95` @ `36c969149e119614b7d221c8006d1a311d6d2995`；原目标包和依赖规划保留。

## 073 · dts.seal_root

- 当前：`solution-dts` / `2.0.0` / 胶条根部定位。目标：`solution-dts`；保留场景职责；**M3**。
- 源码：[solutions/dts/ops/seal_root.cpp:79](../../solutions/dts/ops/seal_root.cpp#L79)。
- 端口：`clean:Profile`, `faces:Record`, `dome:Record`, `pick:Record` → `root:Record`, `point:Point2D`, `pointContext:Record`。
- 参数单位标注：promSmallMm=mm, promMetalMm=mm, footReachMm=mm。
- 依赖：当前直接：标准库极值/区间搜索；计划调用 std-profile；当前构建：core + dts，无PCL/OpenCV/ONNX Runtime。；目标：std-profile/std-metrology按实际调用启用；保持DTS轻量零重依赖。
- 当前契约：输入 clean:PointCloud, faces:Record, dome:Record, pick:Record → 输出 root:Record, point:Point2D。搜索区锚在钣金面朝胶条端到胶条顶点；优先凹凸凹凸起，无凸起退谷底，输出 DtsRoot 和 Point2D。 历史不一致：旧Point2D/Line2D载荷数值为毫米，但Unit2D仅有Meter/Pixel且这些节点未覆盖默认Meter标签，不能按标签直接消费。
- 目标契约：通用极值提取下沉；胶条根部优先级、窗口和回退留场景；坐标/原列索引与输入轮廓绑定。 迁移按DTS来源/旧版本显式将位置/长度乘0.001并记录XZ平面映射，不能直接信任旧Meter标签；Measurement输出单位另按其自身unit处理。
- 迁移前置：dts.pick_metal；dts.seal_dome
- 关联算子：dts.pick_metal；dts.seal_dome
- 验收：台肩伪凸起、多根候选、无凸起回退和越界样本保持选点及原因。 增加旧毫米数值+Meter标签夹具，验证0.001换算、往返和1000倍错量拒绝；方向不随长度缩放。
- 说明：不能把 DtsRoot 当任意 Point2D 丢掉来源/搜索质量。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。
- M3契约更新（2.0.0）：场景v2使用显式Profile的XZ/acquisition/frame/hand/source上下文；工作数值mm但裸几何真实m；Record与实际几何hash绑定。旧v1不自动猜单位，显式dtsProfile.v1整图升级保留资产。；验收：既有默认/相邻基准和失败行为不变；签名、源索引、单位及桥v1/v2避免重复缩放；独立宿主数值一致。
- M3边界验收（manifest_verified）：原生Profile公共链、DTS场景v2与显式旧图迁移；旧源输入壳保留至M7以支持既有宿主注入。
- 阶段依赖边界：仅标准库和编译期Eigen；无PCL/OpenCV/ORT；场景DTS依赖公开轮廓/度量核及原生Profile契约。
- 公开接口：kernels/include/lyflow/kernel/profile.h；kernels/include/lyflow/kernel/profile_analysis.h；kernels/include/lyflow/kernel/metrology.h；packs/std-profile/include/lyflow_profile/context.h
- 阶段验收边界：真实全包manifest描述核验；完整数值、SDK、CLI与图升级回归另由运行证据证明。
- M3迁包来源：`dts` / `packs/dts/ops/seal_root.cpp:55` @ `36c969149e119614b7d221c8006d1a311d6d2995`；原目标包和依赖规划保留。

## 074 · dts.flush

- 当前：`solution-dts` / `2.0.0` / 胶条面差测量。目标：`std-metrology`；拆公共能力/适配；**M3**。
- 源码：[solutions/dts/ops/flush.cpp:68](../../solutions/dts/ops/flush.cpp#L68)。
- 端口：`root:Record`, `metal:Line2D` → `flush:Measurement`, `foot:Point2D`, `footInfo:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前直接：标准库点线垂距，无 PCL/OpenCV；当前构建：core + dts，无PCL/OpenCV/ONNX Runtime。；目标：std-geometry + core的小型测量数据；PCL/OpenCV/ONNX不得成为测量内核依赖。
- 当前契约：输入 root:Record, metal:Line2D → 输出 flush:Measurement, foot:Point2D。DtsRoot Record + Line2D 均 mm；正号由 sealBulge 与 positiveDirection 决定；Measurement.unit=mm，不填写公差/verdict。 历史不一致：旧Point2D/Line2D载荷数值为毫米，但Unit2D仅有Meter/Pixel且这些节点未覆盖默认Meter标签，不能按标签直接消费。
- 目标契约：通用点到基准线有符号面差与垂足；DtsRoot 解包、胶条凸起方向映射留场景适配；显式同帧同单位和正法向。 迁移按DTS来源/旧版本显式将位置/长度乘0.001并记录XZ平面映射，不能直接信任旧Meter标签；Measurement输出单位另按其自身unit处理。
- 迁移前置：M1；M2；dts.seal_root
- 关联算子：dts.seal_root
- 验收：+z/-z、deeper/toward_sensor、反转线方向及非有限结果，值、符号和垂足均有基线。 增加旧毫米数值+Meter标签夹具，验证0.001换算、往返和1000倍错量拒绝；方向不随长度缩放。
- 说明：不得与 gap.flush 共用一个默认符号；先提炼内核，再迁参数。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。
- M3契约更新（2.0.0）：场景v2使用显式Profile的XZ/acquisition/frame/hand/source上下文；工作数值mm但裸几何真实m；Record与实际几何hash绑定。旧v1不自动猜单位，显式dtsProfile.v1整图升级保留资产。；验收：既有默认/相邻基准和失败行为不变；签名、源索引、单位及桥v1/v2避免重复缩放；独立宿主数值一致。
- M3边界验收（manifest_verified）：原生Profile公共链、DTS场景v2与显式旧图迁移；旧源输入壳保留至M7以支持既有宿主注入。
- 阶段依赖边界：仅标准库和编译期Eigen；无PCL/OpenCV/ORT；场景DTS依赖公开轮廓/度量核及原生Profile契约。
- 公开接口：kernels/include/lyflow/kernel/profile.h；kernels/include/lyflow/kernel/profile_analysis.h；kernels/include/lyflow/kernel/metrology.h；packs/std-profile/include/lyflow_profile/context.h
- 阶段验收边界：真实全包manifest描述核验；完整数值、SDK、CLI与图升级回归另由运行证据证明。
- M3迁包来源：`dts` / `packs/dts/ops/flush.cpp:46` @ `36c969149e119614b7d221c8006d1a311d6d2995`；原目标包和依赖规划保留。

## 075 · dts.profile_bundle

- 当前：`solution-dts` / `2.0.0` / 单剖面结果汇总。目标：`solution-dts`；保留场景职责；**M3**。
- 源码：[solutions/dts/ops/bundle.cpp:79](../../solutions/dts/ops/bundle.cpp#L79)。
- 端口：`faces:Record`, `dome:Record`, `pick:Record`, `root:Record`, `metal:Line2D`, `flush:Measurement`, `footInfo:Record`, `foot:Point2D?` → `bundle:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前直接：core Record/Measurement 封装，标准库；当前构建：core + dts，无PCL/OpenCV/ONNX Runtime。；目标：std-profile/std-metrology按实际调用启用；保持DTS轻量零重依赖。
- 当前契约：输入 faces:Record, dome:Record, pick:Record, root:Record, metal:Line2D, flush:Measurement, foot:Point2D? → 输出 bundle:Record。汇总 DtsFaces/Dome/MetalPick/Root、metal、flush、foot 给宿主画图和落库；不是通用算法结果。
- 目标契约：DTS 结果适配留场景；引用原生轮廓/结果句柄而非把大数组塞入 Record；字段版本和缺项语义显式。
- 迁移前置：dts.flush；dts.pick_metal；dts.seal_root
- 关联算子：dts.flush；dts.pick_metal；dts.seal_root
- 验收：旧宿主展示/导出必需字段完整；失败/未激活不改成合格；量值与几何单位对应。
- 说明：保留场景汇总不等于未来通用 Profile 契约仍叫 DtsFaces。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。
- M3契约更新（2.0.0）：场景v2使用显式Profile的XZ/acquisition/frame/hand/source上下文；工作数值mm但裸几何真实m；Record与实际几何hash绑定。旧v1不自动猜单位，显式dtsProfile.v1整图升级保留资产。；验收：既有默认/相邻基准和失败行为不变；签名、源索引、单位及桥v1/v2避免重复缩放；独立宿主数值一致。
- M3边界验收（manifest_verified）：原生Profile公共链、DTS场景v2与显式旧图迁移；旧源输入壳保留至M7以支持既有宿主注入。
- 阶段依赖边界：仅标准库和编译期Eigen；无PCL/OpenCV/ORT；场景DTS依赖公开轮廓/度量核及原生Profile契约。
- 公开接口：kernels/include/lyflow/kernel/profile.h；kernels/include/lyflow/kernel/profile_analysis.h；kernels/include/lyflow/kernel/metrology.h；packs/std-profile/include/lyflow_profile/context.h
- 阶段验收边界：真实全包manifest描述核验；完整数值、SDK、CLI与图升级回归另由运行证据证明。
- M3迁包来源：`dts` / `packs/dts/ops/bundle.cpp:47` @ `36c969149e119614b7d221c8006d1a311d6d2995`；原目标包和依赖规划保留。
