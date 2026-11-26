# 公共 C++ 内核

M2 将已有几何、位姿、约束校验和度量算法抽到可独立链接的 C++17 静态库。算子继续负责端口、参数、取消、诊断与注册；宿主需要直接调用这些计算时，可以使用公开头和 CMake 导出目标，无需创建 Registry、运行图或加载 `lyflow_core.dll`。

M3已抽取轮廓数值方法并迁移DTS试点，M4a公开稳健拟合后端；M4b继续公开完整直线特征和度量方法，场景Gap/glue仍待后续切换。各方法保持独立，不把PCL邻域拟合换成采样窗口方法。整体进度见[迁移计划](operator-package-migration-plan.md)。

## 目标和边界

| 安装目标                          | 公开头                                                      | 能力与依赖                                                                                     |
| --------------------------------- | ----------------------------------------------------------- | ---------------------------------------------------------------------------------------------- |
| `lyflow::data_contract`           | `lyflow/data.h`、`lyflow/pose.h`、`lyflow/calibration.h` 等 | 原生结构及有界 JSON 契约；头文件接口，不提供执行器                                             |
| `lyflow::geometry_kernel`         | `lyflow/kernel/geometry.h`、`rigid.h`、`constraints.h`      | 二维几何关系、刚体/相似变换、位姿与三维约束校验；依赖标准库及原生契约，无 Eigen/PCL/OpenCV/ORT |
| `lyflow::metrology_kernel`        | `lyflow/kernel/metrology.h`、`profile_dimensions.h`         | 平面标定距离、采样间隙/面差、点线面差、圆间隙与位移分量；无视觉后端                            |
| `lyflow::profile_kernel`          | `lyflow/kernel/profile.h`                                   | 原采样序中值、分片、稳定排序视图、TLS/圆拟合与面分割；标准库，显式毫米工作坐标                 |
| `lyflow::profile_analysis_kernel` | `lyflow/kernel/profile_analysis.h`                          | 原TLS分析与刚体投影；Eigen5.0.1仅在库编译时使用，公开接口和安装后消费者无需Eigen               |
| `lyflow::profile_robust_kernel`   | `lyflow/kernel/profile_robust.h`、`profile_features.h`      | PCL双迹线RANSAC、完整近端重拟合/方向回退/端点质量、圆LM/固定半径/软先验；链接`metrology_kernel`，独立消费需同版本PCL |

这些目标不链接 Registry、ComputeFn、Data 运行时分派或 core 执行器。公开 API 使用原生几何结构、`std::array`、`std::variant` 和有界 JSON；包含原生结构头不等于需要链接执行器。使用这些头中的其他运行时方法，例如构造 `Data` 或访问 Frame，仍须按[嵌入协议](embedding.md)接入运行库。

当前约束校验在 `geometry_kernel` 内，没有单独的约束求解器目标。三维迭代拟合、可观性和质量接受条件仍属于 `std-pointcloud`，带其 PCL/CPU 后端依赖；仅校验一份约束记录不会估计工件位姿。

## 安装后使用

SDK 中的 CMake 示例 `examples/public-kernels/CMakeLists.txt` 和完整源码 `examples/public_kernels.cpp` 使用精确产品版本及组件检查：

```cmake
find_package(lyflow 2.0.5 EXACT REQUIRED COMPONENTS geometry_kernel metrology_kernel profile_kernel profile_analysis_kernel)
add_executable(my-kernel-host main.cpp)
target_compile_features(my-kernel-host PRIVATE cxx_std_17)
target_link_libraries(my-kernel-host PRIVATE lyflow::geometry_kernel lyflow::metrology_kernel
  lyflow::profile_kernel lyflow::profile_analysis_kernel)
```

在已安装的 SDK 中，指定安装前缀即可构建示例，不依赖源码树私有目录：

```powershell
cmake -S "$sdk/examples/public-kernels" -B build/kernel-host -DCMAKE_PREFIX_PATH="$sdk"
cmake --build build/kernel-host --config Release
ctest --test-dir build/kernel-host -C Release --output-on-failure
```

SDK 只导出本次构建实际启用的内核目标；四核完整示例仅在这四个目标均启用时安装，精简组合不安装需要缺失组件的示例；缺少所请求组件时配置失败。`lyflow::client` 是另一条通过动态 C ABI 接入完整运行库的路径，使用静态计算内核不需要链接该目标。安装包仍可同时包含运行库，验证独立消费时应检查实际可执行文件的导入表。

## 三维约束

`kernel::constraints::construct3D` 将最多 65536 字节的 items JSON 文本构造成 `cloud.Constraints3D.v1` 的数据部分；`parse3D` 校验已有数据并返回公开的 `Constraint` 数组。二者通过 `Error{code,field,message}` 返回失败，失败时不替换调用方原有输出。

约束保留原契约：显式匹配的 sourceFrame/targetFrame、米制 unit、1..4096 项、唯一且有界的 ID，支持 point/line/plane/axis。坐标及有符号 offset 不超过 1e12 米；正权重按最大权重归一，最大/最小比例不超过 1e12。非点约束的方向使用原缩放范数方法归一化，避免直接平方造成上溢或下溢。

`mixed` 保留混合点线面；`weighted_321` 要求带 datum=0/1/2 的 axis/plane，每组至少 3/2/1 条、组内平行且组间正交。字段白名单、浮点有限性、安全整数及 JSON 深度/体积限制继续生效。构造结果保留原始 items 的权重和方向，归一值只存在于解析后的计算结构中。

后端模块与算子注册也分开：`pointcloud-algorithms` 保留当前 PCL 拟合/轮廓实现，`matching-pointcloud` 单独承载匹配适配并依赖 `matching-cpu`，单选点云算法模块不会自动引入匹配模块。当前Gap积木还调用`std-pointcloud`与`std-ml`的注册节点；完整工作流必须启用这些包，而当前整包`std-pointcloud`仍携带匹配依赖。后续M4切片须消除这类隐式注册依赖，不能由模块链接隔离推断Gap工作流已经可独立运行。

Eigen 投影矩阵留在点云拟合器适配层；公共内核只存 `std::array<double,3>`，不暴露后端矩阵类型。`ResidualAccumulator` 只累计调用方按原顺序提供的残差范数与权重，不改变残差、稳健损失或业务合格判据。

## 算子归属与验证

M2 将 `transform.make`、`register.constraints_3d` 移入 `std-geometry`，将 `image.calibrated_distance` 移入 `std-metrology`；ID、节点版本、参数、端口及计算契约保持原样。旧图加载时按算子 ID 查找，不需要改写 ID；精简构建要选择新的所属包。

源码树测试区分独立静态内核、包级算子和跨包图流程。约束内核已经使用公开内核/原生契约头与随仓 JSON/doctest 的 C++17 编译器独立链接执行，覆盖结构、预算、极值、3-2-1 与失败原子性；点云侧保留旧拟合回归并增加与 Eigen 缩放范数的对照。无重依赖组合已完成原生测试及安装 SDK 的独立消费验证，PE 导入检查确认该轻量产物不引入 PCL/OpenCV/ORT，静态内核示例不依赖 core 执行器。M2完整后端组合及CI/Codex已在[PR63](https://github.com/liangyuR/lyFlow/pull/63)验收合并；M3新增能力须重新执行整体验证，不能由M2或轻量构建推断通过。

## 轮廓工作坐标与数据所有权

`profile_kernel::PlanarSamples` 的两轴是调用方明确选出的分析平面坐标，工作单位为毫米，不是隐含世界XZ。公共算子依据Profile描述显式从m/mm转换，算法内部保留旧窗口、比较阈值与float输入数值路径。`View`只对索引稳定排序，不改源采样序；使用者须保持其借用的PlanarSamples存活。`fitLine`的首尾索引均包含端点。

`profile_analysis::CloudView`借用XYZ及可选通道向量；调用方维持这些缓冲的生命周期，结果由输出结构持有。TLS先将中心/方向按既有float路径收窄再重算RMS，不能用特征值平方根替代；刚体投影保留原矩阵组合及法线处理。原`profile.analyze`/`profile.project_pair`仅迁归属，端口和版本保持；新的原生Profile接口为`profile.fit_line`/`profile.transform_pair`，显式处理掩码和帧。

`signedPointLine`要求各输入使用相同的明确单位；`Normalize`用于一般几何，`DeclaredUnit`要求已归一方向且保留旧float方向的数值路径。法向正方向由调用者指定。`sampledGapFlush`计算实际样本左右边缘间隙，以及同一横坐标上右减左的面差；这与点到线的法向距离是不同业务定义。

轻量构建仅需固定Eigen头文件，不需PCL/OpenCV/ORT。运行`python scripts/fetch-profile-math-deps.py --output-dir build/profile-math-deps`准备并校验，或提供精确5.0.1的`Eigen3_DIR`。安装SDK带`licenses/profile/eigen/NOTICE`及来源清单；导出静态目标不携带源码树或Eigen查找依赖。独立消费门禁显式禁用Eigen发现，调用全部四个内核并检查结果有限、单位与帧契约及PE导入；数值判据由`kernels/tests`负责。

## 可选的稳健拟合后端

M4a公开原点云及Gap使用的RANSAC/LM实现；DTS的TLS与工作毫米算法仍使用原轻量目标。`profile_robust_kernel`不注册节点、不调用执行器，也不把旧`gap.*`节点变成新ID别名。

单独使用后端时，构建选择`LYFLOW_STD_PACKS=OFF`和`LYFLOW_MODULES=profile-robust-kernel`；需要默认轻量Profile链的构建无需选择该模块。安装SDK后显式请求后端组件：

```cmake
find_package(lyflow 2.0.5 EXACT REQUIRED COMPONENTS profile_robust_kernel)
add_executable(my-robust-host main.cpp)
target_compile_features(my-robust-host PRIVATE cxx_std_17)
target_link_libraries(my-robust-host PRIVATE lyflow::profile_robust_kernel)
```

该组件使用独立导出文件，仅在请求时查找与SDK编译版本一致的PCL。完整SDK中的四个轻量组件仍可在禁用PCL发现的环境独立使用。PCL后端消费需要其运行依赖，不需要`lyflow_core.dll`、OpenCV或ONNX Runtime。

`profile_robust::Samples`借用调用方的`Point`数组，单位由调用方明确统一；M4中的Gap路径使用米。调用方先显式映射到分析平面XY（z=0）；旧线拟合仍消费XYZ、圆拟合只消费XY，不会替调用方猜测或更换原坐标轴。结果保留输入槽索引，不以排序或压缩后的序号冒充来源。线与圆保持原RANSAC阈值、迭代、系数优化和重收内点顺序；固定半径、软先验和越界夹限是不同路径，不改写为TLS或代数圆拟合。

`CancelCheck`在输入复制、自有细化循环和PCL调用返回后检查；PCL内部的RANSAC调用期间无法抢占。失败或取消不替换调用方已有结果，非法借用缓冲或选项抛出`std::invalid_argument`。

## M4b完整特征与度量

`profile_features::fitLine`加入近端样本选段、阈值1/3重拟合、free/band/fixed方向及中位回退、ROI/内点端点、innerSlot与质量结果。输入、ROI和toward统一为米。返回槽号指向原数组；固定方向回退保留重复的最终内点权重，不能去重后再算质量，也不把这些权重描述成RANSAC内部抽样轨迹。非有限槽经内部视图排除，输入不改写。

`profile_dimensions`在`metrology_kernel`提供点线面差、两种圆间隙和两点位移分解。输入几何真实为米，补偿显式为毫米；点线面差的距离与垂足由`metrology::signedPointLine`以双精度计算，圆间隙和位移分量保留既有计算顺序；调用方负责按需要换算显示单位。重合圆心、像素、非有限参数及溢出明确拒绝，失败不改变结果。零位移允许生成有限的零长度显示段，不放宽原v1几何上下文校验。

点到无限直线的有符号距离只由`metrology::signedPointLine`实现：面差、ROI边界求交、轮廓最近样本和旧Gap引擎都调用它，`canonicalNormalReference`给出方向朝+x（竖直朝+y）后的右法向参考。带线段裁剪的精确几何距离仍在`geometry_kernel`。

公开节点的帧、平面、来源hash、单位以及旧算子patch版本见[公共Profile算子](profile-public-operators.md)。安装示例同时执行这些新增接口；轻量消费仍显式禁用PCL查找，稳健消费检查实际PCL依赖闭包并隔离core/CV/ORT。SDK只暴露静态计算，安装后消费验证不代替场景导入或现场验收。
