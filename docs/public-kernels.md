# 公共 C++ 内核

M2 将已有几何、位姿、约束校验和度量算法抽到可独立链接的 C++17 静态库。算子继续负责端口、参数、取消、诊断与注册；宿主需要直接调用这些计算时，可以使用公开头和 CMake 导出目标，无需创建 Registry、运行图或加载 `lyflow_core.dll`。

这是已有方法的抽取，尚未迁移 DTS/gap/glue 的业务定义，也未将 PCL 轮廓拟合改为另一种算法。整体进度见[迁移计划](operator-package-migration-plan.md)。

## 目标和边界

| 安装目标 | 公开头 | 能力与依赖 |
| --- | --- | --- |
| `lyflow::data_contract` | `lyflow/data.h`、`lyflow/pose.h`、`lyflow/calibration.h` 等 | 原生结构及有界 JSON 契约；头文件接口，不提供执行器 |
| `lyflow::geometry_kernel` | `lyflow/kernel/geometry.h`、`rigid.h`、`constraints.h` | 二维几何关系、刚体/相似变换、位姿与三维约束校验；依赖标准库及原生契约，无 Eigen/PCL/OpenCV/ORT |
| `lyflow::metrology_kernel` | `lyflow/kernel/metrology.h` | 版本化平面标定距离、加权残差统计；依赖标准库及原生契约，无视觉后端 |

这些目标不链接 Registry、ComputeFn、Data 运行时分派或 core 执行器。公开 API 使用原生几何结构、`std::array`、`std::variant` 和有界 JSON；包含原生结构头不等于需要链接执行器。使用这些头中的其他运行时方法，例如构造 `Data` 或访问 Frame，仍须按[嵌入协议](embedding.md)接入运行库。

当前约束校验在 `geometry_kernel` 内，没有单独的约束求解器目标。三维迭代拟合、可观性和质量接受条件仍属于 `std-pointcloud`，带其 PCL/CPU 后端依赖；仅校验一份约束记录不会估计工件位姿。

## 安装后使用

SDK 中的 [CMake 示例](../examples/public-kernels/CMakeLists.txt)和[完整源码](../examples/public_kernels.cpp)使用精确产品版本及组件检查：

```cmake
find_package(lyflow 1.4.1 EXACT REQUIRED COMPONENTS geometry_kernel metrology_kernel)
add_executable(my-kernel-host main.cpp)
target_compile_features(my-kernel-host PRIVATE cxx_std_17)
target_link_libraries(my-kernel-host PRIVATE lyflow::geometry_kernel lyflow::metrology_kernel)
```

在已安装的 SDK 中，指定安装前缀即可构建示例，不依赖源码树私有目录：

```powershell
cmake -S "$sdk/examples/public-kernels" -B build/kernel-host -DCMAKE_PREFIX_PATH="$sdk"
cmake --build build/kernel-host --config Release
ctest --test-dir build/kernel-host -C Release --output-on-failure
```

SDK 只导出本次构建实际启用的内核目标；缺少所请求组件时配置失败。`lyflow::client` 是另一条通过动态 C ABI 接入完整运行库的路径，使用静态计算内核不需要链接该目标。安装包仍可同时包含运行库，验证独立消费时应检查实际可执行文件的导入表。

## 三维约束

`kernel::constraints::construct3D` 将最多 65536 字节的 items JSON 文本构造成 `cloud.Constraints3D.v1` 的数据部分；`parse3D` 校验已有数据并返回公开的 `Constraint` 数组。二者通过 `Error{code,field,message}` 返回失败，失败时不替换调用方原有输出。

约束保留原契约：显式匹配的 sourceFrame/targetFrame、米制 unit、1..4096 项、唯一且有界的 ID，支持 point/line/plane/axis。坐标及有符号 offset 不超过 1e12 米；正权重按最大权重归一，最大/最小比例不超过 1e12。非点约束的方向使用原缩放范数方法归一化，避免直接平方造成上溢或下溢。

`mixed` 保留混合点线面；`weighted_321` 要求带 datum=0/1/2 的 axis/plane，每组至少 3/2/1 条、组内平行且组间正交。字段白名单、浮点有限性、安全整数及 JSON 深度/体积限制继续生效。构造结果保留原始 items 的权重和方向，归一值只存在于解析后的计算结构中。

后端模块与算子注册也分开：`pointcloud-algorithms` 保留当前 PCL 拟合/轮廓实现，`matching-pointcloud` 单独承载匹配适配并依赖 `matching-cpu`，gap 选择点云算法模块不会顺带注册或引入全局匹配能力。

Eigen 投影矩阵留在点云拟合器适配层；公共内核只存 `std::array<double,3>`，不暴露后端矩阵类型。`ResidualAccumulator` 只累计调用方按原顺序提供的残差范数与权重，不改变残差、稳健损失或业务合格判据。

## 算子归属与验证

M2 将 `transform.make`、`register.constraints_3d` 移入 `std-geometry`，将 `image.calibrated_distance` 移入 `std-metrology`；ID、节点版本、参数、端口及计算契约保持原样。旧图加载时按算子 ID 查找，不需要改写 ID；精简构建要选择新的所属包。

源码树测试区分独立静态内核、包级算子和跨包图流程。约束内核已经使用公开内核/原生契约头与随仓 JSON/doctest 的 C++17 编译器独立链接执行，覆盖结构、预算、极值、3-2-1 与失败原子性；点云侧保留旧拟合回归并增加与 Eigen 缩放范数的对照。无重依赖组合已完成原生测试及安装 SDK 的独立消费验证，PE 导入检查确认该轻量产物不引入 PCL/OpenCV/ORT，静态内核示例不依赖 core 执行器。完整后端组合及 PR 的 CI/Codex 审查仍待最终验收，不能由轻量构建推断全部通过。
