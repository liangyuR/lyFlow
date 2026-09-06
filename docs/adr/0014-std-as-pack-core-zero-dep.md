# ADR-0014：内置点云算子搬进标准算子包，core 零第三方依赖

日期：2026-09-06　状态：已采纳

## 背景

M4 结束时 core 里有 16 个内置算子，其中 6 个直接 `#include <pcl/...>`，
另外 8 个虽然不碰 PCL，但同样是点云域的算法。`core/CMakeLists.txt` 顶上是
`find_package(PCL 1.12 REQUIRED)`，`lyflow_core.dll` 的导入表里有 6 个 `pcl_*.dll`。

同时 [ADR-0013](0013-op-packs-static.md) 已经给出了「领域算子留在自己仓库、
和内置算子一样注册进注册表」的机制，`xyz-gap-inspector` 的 21 个 `gap.*` 算子在用它。

于是有两套不对称的东西：内置算子享受 core 的构建，外部算子走包机制。
这个不对称有三个具体代价：

- 想读一读平台本身（执行器、缓存、类型系统）要先装好 PCL；
- 改一行 `core/include/lyflow/status.h`，PCL 的那几个重 TU 跟着重编；
- 「LyFlow 是什么」这个问题没有干净答案 —— 它既是平台，又捆了一个点云工具箱。

## 决定

**把 14 个点云算子整体搬进仓库内的标准算子包 `packs/std-pointcloud/`，
走的是和 gap 包完全相同的 `lyflow_op_pack` 机制。core 只保留 `gen.synthetic`
与 `util.reroute`，不再 `find_package(PCL)`，不再链接任何第三方库。**

四条配套决定：

1. **core 留下的两个算子不是「漏网的」。** `gen.synthetic` 是测试基础设施 ——
   core 自己的 doctest 要有点云可用，而它不能依赖任何包；`util.reroute` 是编辑器
   语义（Any 推导、连线整理），不是算法。
2. **PCL 的配置只有一处。** 标准包导出一个 INTERFACE 目标 `lyflow_pcl_support`
   （include 目录 + PCL 链接 + 告警屏蔽）。别的包要 PCL 就 `LINK lyflow_pcl_support`，
   不各自 `find_package`。gap 包已经改成这样。
3. **仓库内的包默认启用。** CMake 选项 `LYFLOW_STD_PACKS`（默认 ON）自动加入
   `packs/*`；`LYFLOW_OP_PACKS` 仍然是外部包列表，排在标准包之后。
   `LYFLOW_STD_PACKS=0` 就是纯平台构建 —— 用它来证明 core 真的零依赖。
4. **用户视角零变化。** 算子 id、版本、参数、分类、文案一字不改；默认构建导出的
   manifest 与拆包前**逐字节相同**，只多了每个包内算子的一个 `pack` 字段。

## 怎么做到 manifest 逐字节相同

manifest 的算子顺序就是注册顺序。拆包前的顺序是
`gen.synthetic` → 14 个点云算子 → `util.reroute` → 外部包。
所以 `registerBuiltinOps()` 写成夹心：

```cpp
ops::registerGenSynthetic(r);
registerStdPacks(r);        // packs/*
ops::registerUtilReroute(r);
registerExternalPacks(r);   // LYFLOW_OP_PACKS
```

代价是「包永远排在内置之后」这条 ADR-0013 的说法不再成立，
换来的是老图、e2e、CLI 脚本、schema 样例一行都不用改。

## 顺带放开的两条限制

- **每包一个 PCH。** 拆包前所有包共用一个对象库，所以 `PCH` 全局只能有一份，
  两个包给了不同的 PCH 直接 `FATAL_ERROR`。现在每个包一个 OBJECT 库，
  标准包用 `lyflow_pcl/pcl_pch.h`，gap 包用自己的 `gap_pch.h`，互不相干。
  没有这一条，标准包和 gap 包压根没法同时存在。
- **包名可以带连字符。** 包名要进 C++ 命名空间，所以生成注册入口时用
  `string(MAKE_C_IDENTIFIER)` 过一遍：目录叫 `std-pointcloud`，
  注册入口是 `lyflow::packs::std_pointcloud::registerPackOps`。

## core 里剩下的一个钩子

`lyflow_output_save`（C ABI）与 CLI 的 `lyflow dump` 要把点云写到磁盘，
而写盘格式的知识现在全在标准包里。core 因此留了一个函数指针钩子
（`lyflow/cloud_io.h`）：标准包在 `registerPackOps` 里 `setCloudWriter`，
没有任何包装过写盘实现时返回 `unsupported`。

这是 core 里唯一一处「平台需要包提供能力」的地方。把它做成钩子而不是把
PCD/PLY 的读写抄进 core，是因为抄一遍等于把 PCL 又拉回来。

## 后果

- ✅ `LYFLOW_STD_PACKS=0` 时 `lyflow_core.dll` 的导入表只剩 KERNEL32 与 CRT，
  没有 `pcl_*` / `boost_*` / `flann` / `lz4`；`bin/` 里只有 `lyflow_core.dll` 一个 DLL。
- ✅ 标准包和领域包是同一等公民，「加一个算子」的流程对两者一致。
- ✅ core 的 doctest 不依赖任何包：算子形状由 `core/tests/test_ops.h` 里的
  `test.*` 提供，点云由 `gen.synthetic` 生成。
- ❌ **默认构建里，改一个 core 头文件仍然会重编包的 TU**（它们 include
  `lyflow/registry.h` → `manifest.h` → `status.h`）。零依赖买到的是「可以不装 PCL
  地开发 core」，不是「装了 PCL 也不重编」。想要后者只能加一层稳定的算子 ABI，
  那正是 ADR-0013 排除掉的方案 1。
- ❌ 多了一层目录跳转：读 `filter.voxel_grid` 的实现要去 `packs/`，不在 `core/src/ops/`。
- ❌ `pack` 字段进了 manifest schema。前端不解释它，但它是跨语言契约的一部分了，
  以后想改格式要走 schema 版本。
