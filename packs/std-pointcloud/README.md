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
$env:LYFLOW_PACKS = ""
pnpm check
```

不启用外部包时，纯平台保留 `gen.synthetic`、`util.reroute`、`flow.fallback` 和 `flow.select` 四个内置算子，不链接 PCL。

## 算子

本包注册 20 个算子。下表采用当前显示名称与分类；运行库实际提供的算子及版本以 manifest 为准。

| id | 分类 | 名字 | 端口 |
|---|---|---|---|
| `io.load_pcd` | 输入输出/输入 | 点云读取 | — → cloud |
| `io.save_pcd` | 输入输出/输出 | 点云保存 | cloud → — |
| `filter.passthrough` | 过滤/裁剪 | 直通滤波 | cloud → cloud, indices |
| `filter.voxel_grid` | 过滤/降采样 | 体素降采样 | cloud → cloud |
| `filter.crop_box` | 过滤/裁剪 | 三维包围盒裁剪 | cloud, pose → cloud |
| `filter.random_sample` | 过滤/降采样 | 随机采样 | cloud → cloud |
| `filter.statistical_outlier` | 过滤/离群点 | 统计离群点剔除 | cloud → cloud, removed |
| `filter.radius_outlier` | 过滤/离群点 | 半径离群点剔除 | cloud → cloud, removed |
| `features.normals` | 特征 | 点云法线估计 | cloud → cloud |
| `segment.ransac_plane` | 分割 | RANSAC 平面拟合 | cloud → inliers, plane |
| `segment.extract_indices` | 分割 | 点云索引提取 | cloud, indices → selected, rest |
| `transform.make` | 变换 | 刚体变换构造 | — → transform |
| `transform.apply` | 变换 | 点云变换 | cloud, transform → cloud |
| `util.merge` | 工具 | 点云合并 | a, b → cloud |

2D 量测域的四个（[ADR-0015](../../docs/adr/0015-algorithms-live-in-lyflow-packs.md)）：

| id | 分类 | 名字 | 端口 |
|---|---|---|---|
| `filter.crop_box2d` | 过滤/裁剪 | 二维矩形裁剪 | cloud, box → cloud |
| `fit.line_2d` | 拟合/直线 | 二维直线拟合 | cloud, [clipTo] → line, inliers |
| `fit.circle_2d` | 拟合/圆 | 二维圆拟合 | cloud → circle, inliers |
| `register.icp_2d` | 配准/ICP | 二维 ICP 配准 | source, target, [init] → transform, result |

编辑域一个（M5，给 `lyflow perturb` 用，见
[ADR-0020](../../docs/adr/0020-eval-and-perturb-as-cli.md)）：

| id | 分类 | 名字 | 端口 |
|---|---|---|---|
| `edit.translate_region` | 编辑 | 点云选区平移 | cloud → cloud |

分割域一个（标定的左右分割，替代 xyz-gap-inspector 里的过渡实现 `calibration/split.rs`）：

| id | 分类 | 名字 | 端口 |
|---|---|---|---|
| `segment.region_growing_2d` | 分割 | 区域生长 2D | cloud → left, right, result |

XY 平面上按法线夹角的区域生长（PCL `RegionGrowing` 平滑模式 + 二维协方差法线），取点数最多的两簇、
x 均值小的从 `left` 出。与 split.rs 逐位一致（`test_2d_ops.cpp` 拿它生成的期望值比对），
剩下不到两簇报 `split_failed`。

长度参数一律是**米**，与点云同单位。

## 目录

```
lyflow_op_pack.cmake            find_package(PCL) + lyflow_pcl_support + lyflow_op_pack()
include/lyflow_pcl/
  adapter.h                     LyFlow 数据模型 ↔ PCL 的唯一转换点（ADR-0005）
  pcl_path.h                    中文路径下把窄字符串喂给 PCL 的三种模式（D9）
  pcl_pch.h                     本包的预编译头
ops/
  ops.h                         算子注册函数的声明
  register.cpp                  registerPackOps + setCloudWriter
  *.cpp                         一个算子一个文件
algo/
  cloud2d.*                     Cloud2D = pcl::PointCloud<pcl::PointXYZRGB> 与转换
  fit2d.*                       直线/圆拟合（双迹线、定半径重定圆心）
  icp2d.*                       2D point-to-plane ICP
  profile_geometry.*            剖面法线估计
  crop2d.*                      XY 盒裁剪（开/闭区间）
  region_growing2d.*            XY 区域生长 + 最大两簇分左右（不依赖 PCL）
src/
  adapter.cpp、pcl_path.cpp     上面两个头的实现
tests/
  test_std_ops.cpp              算子语义（体素键、通道保留、迁移链…）
  test_io_pcd.cpp               中文路径下的 PCD 往返
  test_2d_ops.cpp               2D 算子（开闭区间、拟合、ICP 恢复平移、区域生长与 split.rs 逐位对拍）
  data/region_growing_2d_cases.h  split.rs 生成的输入与期望下标（勿手改）
```

## ICP 的鲁棒损失、多尺度与质量门

`register.icp_2d` 1.1.0 增加可选参数，默认仍为单尺度最小二乘：
`robustLoss=none`、`multiScaleLevels=1`、`reportQuality=false`、`qualityGate=false`。
默认路径不新增质量近邻扫描，也不常驻额外的 PCL 转换云；共享 `Icp2DOptions` 的 gap 调用保留原默认值。

| 参数 | 用法 |
| --- | --- |
| `robustLoss`、`robustScale` | `huber` 将大于阈值的平面残差降权；`tukey` 将绝对值大于等于阈值的残差权重设为零。阈值单位 m，启用时须为正。几何配对仍受 `maxMatchingDist` 限制。 |
| `multiScaleLevels`、`coarseVoxelSize` | 层数 1–6。多尺度时最粗层用指定 XY 网格每格保留首点，中间层网格逐次减半；最后一层使用原始全量点。每一层的位姿作为下一层的初值。`coarseVoxelSize` 只在层数大于 1 时要求为正。 |
| `maxMatchingDist`、`maxIterations` | 距离为最终层的阈值，向上每粗一层乘 2，启用的 `robustScale` 同比例放宽；`maxIterations` 是每层上限。少于 3 个有限点的粗层跳过，最终层照常运行。 |
| `reportQuality` | 在最终位姿额外做一次近邻扫描。默认关闭，结果中只有 `qualityEvaluated=false`，不填写未计算的质量数值。 |
| `qualityGate` | 自动启用质量评估，按 `minMatches`（默认 3）、`minInlierRatio`（0.5）、`maxPlaneRmse`（0.001 m）、`requireConverged`（true）筛选配准结果。 |

例如在节点参数里启用三层 Huber 配准，并观察最终质量：

```json
{
  "robustLoss": "huber",
  "robustScale": 0.0005,
  "multiScaleLevels": 3,
  "coarseVoxelSize": 0.002,
  "reportQuality": true
}
```

`result` 保留 `fitness`、`iterations`、`converged`；其中 `fitness` 沿用旧的末轮配对口径，
`iterations` 和 `converged` 属于最终全量层。新增 `totalIterations` 和 `levels` 记录各层样本数、
抽稀尺度、配对阈值、迭代次数及收敛情况。鲁棒损失不能扩大初始配对范围，仍需合理的初值或粗层距离。

质量评估开启后，`sourceCount` / `targetCount` 为有限 XY 点数，`matchedCount` 为最终位姿下
`maxMatchingDist` 内的近邻对数，`inlierRatio=matchedCount/sourceCount`。
`normalMatchedCount` 进一步要求法线有效，`effectiveMatchedCount` 再要求鲁棒权重大于零；
`minMatches` 检查的是最后一项。`pointRmse` 是几何距离 RMSE，`planeRmse` 是未加权平面残差 RMSE，
`weightedPlaneRmse` 使用鲁棒权重，均为 m。没有对应匹配时 RMSE 为 `null`；`weightSum` 给出有效权重总和。

质量门拒绝返回 `registration_rejected`，错误消息列出不合格项与实测数值，
该节点不发布 `transform` / `result`，下游可以通过接受 Error 的 fallback 端口处理。
质量门通过时 `qualityGateEnabled=true`。配准质量信号可供上层判定定位有效性，尺寸规格仍由量测与验收算子处理。

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
#include "algo/region_growing2d.h"  // lyflow::std_pc::regionGrowing2D
```

`algo/` 里的 2D 算法一律吃 `pcl::PointCloud<pcl::PointXYZRGB>`（迁移时沿用的点类型）。
RANSAC 等参数外露，默认值取自迁出来的那份（xyz-gap-inspector 的 `GapUtils` / `Icp2D`）；
它们不承诺与那份数值相同，改算法以本包与 `packs/gap` 的 doctest 为准。

## 写盘钩子

`register.cpp` 里的 `setCloudWriter(&ops::saveCloudToFile)` 把 PCD/PLY 的写盘能力
装进 core（`lyflow/cloud_io.h`）。C ABI 的 `lyflow_output_save` 与 CLI 的
`lyflow dump` 靠它落盘；不带这个包的构建里它们会报 `unsupported`。

## P1 三维分析与单扫描

源码逐项分析、取舍和未覆盖模式见 [P1 点云分析](../../docs/p1-analysis-cloud.md)。新算子为：

- `cloud.statistics`、`cloud.pca`、`cloud.bounds_obb`：最大Z、质心、AABB/PCA包围盒与退化轴标志；不是最小体积盒或箱面识别。
- `cloud.group_pack/get/subset/concat/merge/statistics`：原生点云＋原生Indices＋`cloud.Groups.v1`，最多4096组。消费者校验来源ID、帧、区间和索引；`group_get.indices`仍指向原始来源云，不指向提取的新云。concat支持成对连接的已有组元数据，ID加`a/`、`b/`命名空间，通道不一致拒绝。
- `segment.euclidean_clusters`、`segment.multi_planes`：真正XYZ簇和顺序多平面；保留原始成员索引、可选通道和余点。聚类ID不是跨帧跟踪身份。
- `register.rigid_fit`、`register.icp_3d`、`register.local_search_3d`：对应点拟合、点点/点面局部SE(3)配准及至多9初值搜索，方向为source→target。拒绝共线、点面不可观测、缺法线、未收敛、低匹配比例和高RMSE；不是全局模型匹配。最终候选排序、RMSE 和匹配比例均根据输出 float32 Transform 的原生坐标运算重新评估，`aligned` 的 XYZ 与复用该变换的 `transform.apply` 一致；大坐标下无法交付的精度直接拒绝。
- `io.load_cloud`：PCD（ASCII/二进制/压缩）、点顶点PLY（ASCII/小端二进制）、STL（ASCII/小端二进制面积加权采样）。文件最多256MiB、头最多64KiB，每点字段最多128字节；先检查宣称点数/正文/解压尺寸，再加载。保留XYZ、强度、完整法线与RGB，拒绝额外未知字段、alpha、不完整通道、非有限数和不符合声明整数类型/宽度的 ASCII 值；不保存网格拓扑。文件单位m/mm必须显式指定，初姿平移为米。
- `profile.analyze`、`profile.gap_flush`：明确XY/XZ/YZ平面、米制单扫描直线与双区间间隙/面差；质量基于实际输出的 float32 直线重新评估，面差与该直线一致；不伪装双相机输入，不自动推断缝型。
- `profile.project_pair`：应用显式master/slave→common及可选common→output外参，不猜原生云不存在的pose元数据。

相机/云帧是调用者明确声明；没有自动证明外参真实有效。空统计通过valid=false和无效Measurement表示；非法数据/退化/质量不达标返回诊断。新回归入口是`tests/test_p1_cloud.cpp`，采用自制合成数据和确定性文件，不代表现场设备或生产质量验收。

聚类组 ID 按规范 XYZ 点序计算的质心、点数和完整 XYZ 字典序分配；同质心同点数簇不会依赖输入顺序平手。成员索引仍指向原始点云，组 ID 仅代表本次结果规范顺序，不承诺跨帧跟踪。
