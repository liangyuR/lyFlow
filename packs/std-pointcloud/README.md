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

19 个。前 14 个的 id / 版本 / 参数 / 分类与它们还在 `core/src/ops/` 时逐字相同。

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

2D 量测域的四个（[ADR-0015](../../docs/adr/0015-algorithms-live-in-lyflow-packs.md)）：

| id | 分类 | 名字 | 端口 |
|---|---|---|---|
| `filter.crop_box2d` | Filter/Crop | Crop Box 2D | cloud, box → cloud |
| `fit.line_2d` | Fit/Line | Fit Line 2D | cloud, [clipTo] → line, inliers |
| `fit.circle_2d` | Fit/Circle | Fit Circle 2D | cloud → circle, inliers |
| `register.icp_2d` | Register/ICP | ICP 2D | source, target, [init] → transform, result |

编辑域一个（M5，给 `lyflow perturb` 用，见
[ADR-0020](../../docs/adr/0020-eval-and-perturb-as-cli.md)）：

| id | 分类 | 名字 | 端口 |
|---|---|---|---|
| `edit.translate_region` | 编辑 | 平移选区 | cloud → cloud |

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
algo/
  cloud2d.*                     Cloud2D = pcl::PointCloud<pcl::PointXYZRGB> 与转换
  fit2d.*                       直线/圆拟合（双迹线、定半径重定圆心）
  icp2d.*                       2D point-to-plane ICP
  profile_geometry.*            剖面法线估计
  crop2d.*                      XY 盒裁剪（开/闭区间）
src/
  adapter.cpp、pcl_path.cpp     上面两个头的实现
tests/
  test_std_ops.cpp              算子语义（体素键、通道保留、迁移链…）
  test_io_pcd.cpp               中文路径下的 PCD 往返
  test_2d_ops.cpp               2D 四个算子（开闭区间、拟合、ICP 恢复平移）
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

## `lyflow_std_algo`

第二个 INTERFACE 目标，把 `algo/` 挂成 include 根，让别的包直接调这些算法
而不是再抄一遍（ADR-0015）：

```cmake
lyflow_op_pack(NAME mypack LINK lyflow_std_algo ...)
```

```cpp
#include "algo/fit2d.h"   // lyflow::std_pc::fitLine2D / fitCircle2D / fitCircleFixedRadius2D
#include "algo/icp2d.h"   // lyflow::std_pc::Icp2D
#include "algo/crop2d.h"  // lyflow::std_pc::insideBox2D / cropBox2D
```

`algo/` 里的 2D 算法一律吃 `pcl::PointCloud<pcl::PointXYZRGB>`，
与它们迁出来的那份（xyz-gap-inspector 的 `GapUtils` / `Icp2D`）同一个点类型 ——
换点类型就要重新证明一遍逐位一致，而 `packs/gap` 的两条 A/B 正压在这上面。

## 写盘钩子

`register.cpp` 里的 `setCloudWriter(&ops::saveCloudToFile)` 把 PCD/PLY 的写盘能力
装进 core（`lyflow/cloud_io.h`）。C ABI 的 `lyflow_output_save` 与 CLI 的
`lyflow dump` 靠它落盘；不带这个包的构建里它们会报 `unsupported`。
