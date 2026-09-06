# ADR-0015：算法住在 LyFlow 的包里，通用的进标准包

日期：2026-09-06　状态：已采纳

## 背景

[ADR-0013](0013-op-packs-static.md) 让领域算子留在自己的仓库，
[ADR-0014](0014-std-as-pack-core-zero-dep.md) 让内置点云算子变成仓库内的标准包。
接完 gap 之后，`xyz-gap-inspector/lyflow/` 里的 21 个 `gap.*` 算子有两个问题：

- **算法在别人家。** 包链接 `xyz_gap_core.lib` / `xyz_gap_ml.lib` / `xyz_gap_io.lib`，
  三个静态库要先在业务仓库里构建一遍 Release，还要靠 `GAP_BUILD_DIR` 指过去。
  LyFlow 这边改不动算法，也没法给算法写测试。
- **通用算法被锁在领域包里。** 直线拟合、圆拟合、2D ICP、盒裁剪都不是「间隙测量」
  独有的，但它们只有 `gap.*` 用得上；换一个领域就要再抄一遍。

## 决定

**算法源码搬进 LyFlow 的 `packs/`；其中通用的那部分进标准包，领域包只留领域逻辑。**

具体四条：

1. **`packs/gap/`** 收纳 `ops/`（21 个 `gap.*`）、`algo/`（从 gap-inspector 复制的算法源码，
   命名空间不改）、`tools/`（图生成器与 A/B 脚本）、`tests/`、`README.md`。
   不再有 `GAP_BUILD_DIR`，不再链接任何预构建的 `.lib`。
2. **通用 2D 几何进 `packs/std-pointcloud/`**：`fit.line_2d`、`fit.circle_2d`、
   `register.icp_2d`、`filter.crop_box2d`。算法实现在包内 `algo/`，
   经第二个 INTERFACE 目标 **`lyflow_std_algo`** 导出头文件给别的包调用。
   gap 的算子与 gap 的 `algo/` 都改成调它，`GapUtils.cpp` 里对应的实现**删掉**。
3. **`packs/std-ml/`** 是新的标准包，只有一个 `ml.onnx_run`：`Tensor` 进、`Tensor` 出。
   onnxruntime 的依赖只在这个包里 —— 推理引擎是重依赖，不该和点云包绑死。
4. **core 新增端口类型 `Tensor`**：`shape` + float32 数据，行主序。
   不进二进制 IPC，Inspector 只看得到形状与 min/max/mean。

## 为什么 ONNX 拆成三步

模型路径原来是一个 `gap.onnx_segment`：读两片剖面、构造六通道、跑推理、逐槽 argmax。
现在是

```
gap.profile_tensor  (primary, secondary → Tensor[2,6,1280])
ml.onnx_run         (Tensor → Tensor[2,8,1280])
gap.labels_from_logits (Tensor → Record{GapLabels})
```

**推理本身是通用的，通道构造与 argmax 是领域约定。** 拆开之后换一个模型
（不同通道、不同类别数）只改两头，中间那一步一行不用动；
而 `ml.onnx_run` 立刻可以给 Image 域用。代价是图上多两个节点。

## 包的启用：DEFAULT 与 LYFLOW_PACKS

`packs/*` 里每个包在 `lyflow_op_pack()` 里声明 `DEFAULT ON|OFF`：

| 包 | DEFAULT | 理由 |
|---|---|---|
| `std-pointcloud` | ON | 平台的点云工具箱 |
| `std-ml` | ON | 只多一个 onnxruntime，且 `fetch-onnxruntime.ps1` 一条命令备好 |
| `gap` | OFF | 领域包不该拖累纯平台开发者，但它在同一仓库 |

打开默认关闭的包：`LYFLOW_PACKS=gap`（分号分隔的**仓库内包名**，
与 `LYFLOW_OP_PACKS` 的「外部目录列表」是两回事）。
`LYFLOW_STD_PACKS=0` 仍然是「一个仓库内的包都不编」。

DEFAULT 在 `lyflow_op_pack()` 里声明，而包的 cmake 往往要先找依赖才能凑齐参数 ——
所以宏会把「本包这次编不编」写回 `LYFLOW_PACK_ENABLED`，
包在调用之后读它，决定要不要报缺依赖、要不要拷 DLL。没启用的包一个目标都不建。

## 第三方依赖

| 依赖 | 怎么来 |
|---|---|
| PCL | vcpkg（`C:\vcpkg`），只有 `packs/std-pointcloud` `find_package` |
| yaml-cpp | vcpkg：`vcpkg install yaml-cpp:x64-windows`，只有 `packs/gap` 用 |
| onnxruntime 1.19.2 | `scripts/fetch-onnxruntime.ps1` 备到 `third_party/onnxruntime/`（gitignore），`LYFLOW_ONNXRUNTIME_ROOT` 可以覆盖 |

缺 onnxruntime 时 CMake 直接 FATAL 并打印那条命令。**不静默跳过** ——
一个「悄悄少了一个算子」的构建比一个报错的构建贵得多。

## 逐位一致怎么保住

gap 的两条 A/B（模板路径、模型路径）39/39 且 |Δ| = 0 是硬约束。三条做法：

- **点类型不换。** `lyflow_std_algo` 的 2D 算法一律吃 `pcl::PointCloud<pcl::PointXYZRGB>`，
  与 gap 原来那份同一个类型；换成 `PointXYZ` 就要重新证明一遍。
- **PCL 调用逐字对应。** `fitLine2D` / `fitCircle2D` 里的 RANSAC 迭代次数、
  `random=false`、`setOptimizeCoefficients`、双迹线的伴线搜索、定半径最小二乘重定圆心，
  全部照抄；参数外露，但默认值就是原值。
- **推理线程数默认 1**（intra 与 inter 都是），归约顺序固定。

## 后果

- ✅ 算法有主了：LyFlow 能改它、能给它写 doctest、能让别的领域复用。
- ✅ `packs/gap/algo/` 里没有直线/圆拟合、ICP、盒裁剪的实现，只有领域逻辑
  （双迹线截取、端点、距离与求值、roll 裁剪窗、掩膜精修、框推导、`MeasurementEngine`）。
- ✅ 纯平台开发者（`LYFLOW_STD_PACKS=0`）与只要点云的开发者都不受影响。
- ❌ **同一份算法暂时有两份拷贝**：LyFlow 的 `packs/gap/algo/` 与 gap-inspector 的
  `src/`。业务仓库接入 LyFlow 之前不能删那一份，这段时间要靠 A/B 兜住漂移。
- ❌ 图上多了两个节点（ONNX 三步），生成器与 e2e 断言跟着改了一处。
- ❌ `packs/` 之间有了顺序依赖（谁定义 `lyflow_std_algo`、谁解析
  `LYFLOW_ONNXRUNTIME_ROOT`），core 的 CMake 里因此有一小段显式的优先级列表。
