# std-pointcloud —— LyFlow 的标准点云算子包

版本 **0.1.0**。仓库内的标准算子包（[ADR-0014](../../docs/adr/0014-std-as-pack-core-zero-dep.md)），
走的是和外部领域包完全相同的 `lyflow_op_pack` 机制（[ADR-0013](../../docs/adr/0013-op-packs-static.md)、
[docs/op-packs.md](../../docs/op-packs.md)）。

这个包是**全仓库唯一一处 `find_package(PCL)`**。core 不链接任何第三方库。

## 构建

默认就开着：`LYFLOW_STD_PACKS` 缺省 ON，`core/CMakeLists.txt` 自动加入 `packs/*`。
关掉它就是纯平台构建：

```powershell
$env:LYFLOW_STD_PACKS = "0"
pnpm check            # core 只剩 gen.synthetic 与 util.reroute，DLL 里没有一个 pcl_*
```

## 算子

14 个，id / 版本 / 参数 / 分类与它们还在 `core/src/ops/` 时逐字相同。

| id | 分类 | 名字 | 端口 |
|---|---|---|---|
| `io.load_pcd` | IO/Input | Load PCD | — → cloud |
| `io.save_pcd` | IO/Output | Save PCD | cloud → — |
| `filter.passthrough` | Filter/Crop | Passthrough | cloud → cloud, indices |
| `filter.voxel_grid` | Filter/Downsample | Voxel Grid | cloud → cloud |
| `filter.crop_box` | Filter/Crop | Crop Box | cloud, pose → cloud |
| `filter.random_sample` | Filter/Downsample | Random Sample | cloud → cloud |
| `filter.statistical_outlier` | Filter/Outlier | Statistical Outlier Removal | cloud → cloud, removed |
| `filter.radius_outlier` | Filter/Outlier | Radius Outlier Removal | cloud → cloud, removed |
| `features.normals` | Features | Estimate Normals | cloud → cloud |
| `segment.ransac_plane` | Segment | RANSAC Plane | cloud → inliers, plane |
| `segment.extract_indices` | Segment | Extract Indices | cloud, indices → selected, rest |
| `transform.make` | Transform | Make Transform | — → transform |
| `transform.apply` | Transform | Apply Transform | cloud, transform → cloud |
| `util.merge` | Util | Merge Clouds | a, b → cloud |

长度参数一律是**米**，与点云同单位。

## 目录

```
lyflow_op_pack.cmake            find_package(PCL) + lyflow_pcl_support + lyflow_op_pack()
include/lyflow_pcl/
  adapter.h                     LyFlow 数据模型 ↔ PCL 的唯一转换点（ADR-0005）
  pcl_path.h                    中文路径下把窄字符串喂给 PCL 的三种模式（D9）
  pcl_pch.h                     本包的预编译头
ops/
  ops.h                         14 个注册函数的声明
  register.cpp                  registerPackOps + setCloudWriter
  *.cpp                         一个算子一个文件
src/
  adapter.cpp、pcl_path.cpp     上面两个头的实现
tests/
  test_std_ops.cpp              算子语义（体素键、通道保留、迁移链…）
  test_io_pcd.cpp               中文路径下的 PCD 往返
```

## `lyflow_pcl_support`

别的包要 PCL 时链这一个 INTERFACE 目标就够了 —— include 目录、PCL 的库、
以及针对 PCL/Eigen 头的告警屏蔽都在里面：

```cmake
lyflow_op_pack(
  NAME  mypack
  LINK  lyflow_pcl_support
  ...
)
```

它同时把 `include/lyflow_pcl/` 挂上，所以包里可以直接
`#include "lyflow_pcl/adapter.h"` 做点云与 `pcl::PointCloud` 的互转。

`LYFLOW_STD_PACKS=0` 时这个目标不存在 —— 依赖它的包会在 configure 期
`FATAL_ERROR`，而不是编到一半才报找不到头。

## 写盘钩子

`register.cpp` 里的 `setCloudWriter(&ops::saveCloudToFile)` 把 PCD/PLY 的写盘能力
装进 core（`lyflow/cloud_io.h`）。C ABI 的 `lyflow_output_save` 与 CLI 的
`lyflow dump` 靠它落盘；不带这个包的构建里它们会报 `unsupported`。
