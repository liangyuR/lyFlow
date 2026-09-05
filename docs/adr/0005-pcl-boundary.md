# ADR-0005：PCL 关在 `src/ops/pcl/` 里，`include/` 下零 PCL 头

- 状态：已采纳
- 日期：2026-09-05

## 背景

最省事的做法是让 `PointCloud` 直接是 `pcl::PointCloud<pcl::PointXYZ>`：
算子拿到就能用，进出不用拷贝，PCL 的几百个算法全部直接可用。

## 决策

1. `core/include/` 下**不出现任何 PCL 头**。公共数据模型是自己的
   `lyflow::PointCloud`（SoA 的 `std::vector<float>` + 可选通道）。
2. `pcl::` 只出现在 `core/src/ops/pcl/*.cpp` 和它私有的 `adapter.h` / `pcl_pch.h`。
3. 进出 PCL 一律经 `adapter`：`toPcl` / `fromPcl` / `fromBlob` / `toBlob`。
4. **PCL 算子的产出尽量是 `Indices` 而不是点云**，再由 `PointCloud::select` 出云。

## 理由

**编译时间就是「加算子」的边际成本。** 只要 `include/lyflow/data.h` 里出现一个
PCL 头，整棵依赖树都要吃 Eigen 的模板 —— 执行器、注册表、JSON 写出器、
每一个手写算子，全都跟着变成十几秒一个 TU。ADR-0003 的全部意义是
「加算子只改 C++、反馈是秒级的」，而这条边界就是它在 M2 之后还成立的保证。
现在的分工是：`src/ops/pcl/` 吃一个专门的 PCH，其余目录一点 PCL 都不碰。

**数据模型不该焊死在一个库的形状上。** M5 要接 Image 域（OpenCV）。
如果 `Data` 是围绕 `pcl::PointCloud<PointXYZ>` 长出来的，那时要么塞不进去，
要么长成「点云的特例 + 图像的特例」的怪物。数据模型是这个项目最难改的东西，
它应该只反映本项目自己的抽象。

**属性通道会被 PCL 悄悄吃掉。** PCL 的模板点类型必须在编译期确定
（`PointXYZ` / `PointXYZI` / `PointXYZRGBNormal` / …），而
「这片云带哪些通道」是运行时才知道的。让 PCL 直接出云，等于每次都要在
「为所有组合各实例化一份模板」和「悄悄丢掉几个通道」之间选一个。
所以约定成：PCL 只回答「留哪些点」（Indices），通道搬运统一交给
`PointCloud::select` —— 一处代码，加一个通道只改一次。

## 代价

- 每次进出 PCL 拷一份 xyz。这是明码标价的：百万点约 12MB 的拷贝，
  相对 KD 树构建的开销是零头。产出 Indices 的约定让这个拷贝只发生一次。
- `adapter::fromBlob` / `toBlob` 要自己解析 `PCLPointCloud2` 的字段表
  （x/y/z/intensity/normal_*/rgb，datatype 分派）。这段代码不长，但必须有测试
  —— 它是「读进来的 pcd 少了强度」这类问题的唯一嫌疑人。

## 例外

`pcl_path.h` / `pcl_path.cpp` 放在 `src/ops/pcl/` 下，但它一个 PCL 头都不 include。
放在这里是因为它服务的对象只有 PCL 的窄字符串 IO；从依赖角度它属于「无 PCL」那一侧。

## 复议条件

如果某个算法必须在 PCL 的点类型上做原地迭代（比如需要 PCL 的 organized 结构），
可以在 `src/ops/pcl/` 内部随便用 —— 边界约束的是**接口**，不是实现。
只有当「公共数据模型必须暴露 PCL 类型」时才需要复议这条 ADR。
