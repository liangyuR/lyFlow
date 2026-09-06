# 算子包

算子包让**算法留在自己的仓库/目录**，但和平台自带的算子一样注册进 LyFlow 的注册表。
为什么不是运行时插件 DLL、不是库算子，见 [ADR-0013](adr/0013-op-packs-static.md)。

一句话：包提供源文件与链接库，LyFlow 在构建期把它们编进同一个 `lyflow_core.dll`。

包有两类，机制**完全相同**，只是从哪儿找到它们不一样：

| | 仓库内的包 | 外部包 |
|---|---|---|
| 在哪 | 本仓库的 `packs/*` | 任意目录 |
| 怎么加入 | `LYFLOW_STD_PACKS`（默认 ON）自动扫 | `LYFLOW_OP_PACKS` 显式列出 |
| 注册顺序 | 紧跟 `gen.synthetic` | 紧跟 `util.reroute` |
| 例子 | `packs/std-pointcloud`、`packs/std-ml`、`packs/gap` | 任何自己写的包 |

core 本身只有 `gen.synthetic` 与 `util.reroute` 两个算子，不链接任何第三方库
（[ADR-0014](adr/0014-std-as-pack-core-zero-dep.md)）。

仓库内的包不一定默认编：每个包在 `lyflow_op_pack()` 里声明 `DEFAULT ON|OFF`。

| 包 | 版本 | DEFAULT | 算子 | 依赖 |
|---|---|---|---|---|
| `std-pointcloud` | 0.1.0 | ON | 18 个点云 / 2D 量测算子 | PCL |
| `std-ml` | 0.1.0 | ON | `ml.onnx_run` | onnxruntime |
| `gap` | 0.2.0 | **OFF** | 21 个 `gap.*` | PCL、yaml-cpp、onnxruntime |

`DEFAULT OFF` 的包用 **`LYFLOW_PACKS`** 按名字打开（分号分隔的**包名**，
不是目录 —— 那是 `LYFLOW_OP_PACKS` 的事）：

```powershell
$env:LYFLOW_PACKS = "gap"
pnpm check          # 或 pnpm check:gap，它还会跑两条 A/B
```

领域包默认关，是因为纯平台开发者不该为一个领域包装 yaml-cpp
（[ADR-0015](adr/0015-algorithms-live-in-lyflow-packs.md)）。

## 用一个包构建

`LYFLOW_OP_PACKS` 是**分号分隔的目录列表**，每个目录下要有一个 `lyflow_op_pack.cmake`：

```powershell
$env:LYFLOW_OP_PACKS = "D:\project\xyz-gap-inspector\lyflow"
pnpm check          # 或 pnpm core:build / pnpm dev
```

不设这个变量就是默认构建：core + `packs/*`。
`scripts/build-core.ps1`、`bridge/build.rs`、`scripts/core-watch.ps1` 都读同一个变量。

三个开关（`LYFLOW_STD_PACKS`、`LYFLOW_PACKS`、`LYFLOW_OP_PACKS`）都按
「CMake 缓存变量 → 同名环境变量 → 默认值」的顺序取值，**空串一律按「没设」处理** —— `-DLYFLOW_STD_PACKS=` 传了个空值，
不该悄悄变成「关掉标准包」；要关就显式写 `0`。

包目录进了 `build.rs` 的 rerun 列表与 `core-watch.ps1` 的源码指纹，
所以改包里的算子会触发增量构建和热重载，和改 `core/src/` 一样；
`packs/` 同样在这两条监视链里。

### 纯平台构建

```powershell
$env:LYFLOW_STD_PACKS = "0"
pnpm check
```

`packs/*` 一个都不编。manifest 里只剩 `gen.synthetic` 与 `util.reroute`，
`lyflow_core.dll` 的导入表里只有 KERNEL32 与 CRT。
这一趟是「core 真的零依赖」的唯一证据，也是不装 PCL 就能开发平台本身的路子。
依赖 `lyflow_pcl_support` 的包（gap、以及任何用 PCL 的包）在这一模式下会在
configure 期直接报错 —— 那个目标由标准包提供。

### 带包和不带包混着跑

cargo 会按 feature 集把 `lyflow-app` 建好几遍（app、lib 的 test、CLI），
每一份有自己的 `OUT_DIR` 和自己的 `lyflow_core.dll`。开发构建里每个 exe
**加载的是自己那一份**（`build.rs` 发的 `LYFLOW_CORE_BIN`），
所以两种模式来回切不会串味 —— 不带外部包跑过 `pnpm check` 之后，
带 gap 包的 `pnpm tauri dev` 仍然是 37 个算子，反过来也一样。
`target/debug/` 与 `deps/` 里那份拷贝只服务 `tauri build` 与打包。

包用 `file(COPY ...)` 往 `bin/` 里放的 DLL（`std-ml` 的 `onnxruntime.dll` 就是）
不会因为下次不带那个包构建就消失。`std-ml` 默认开，所以这一条现在只在
`LYFLOW_STD_PACKS=0` 与默认之间来回切时咬人：纯平台构建的 `bin/` 是另一个目录，
但 `bridge/target/` 里那份共享拷贝会留着上一次的。

## 一个包长什么样

```
mypack/
  lyflow_op_pack.cmake     # 必须叫这个名字
  ops/
    register.cpp           # 定义 lyflow::packs::mypack::registerPackOps
    foo.cpp
  tests/
    test_foo.cpp           # 可选
  README.md
```

放进本仓库 `packs/` 下就是标准包，别的一切不变。`packs/std-pointcloud` 是现成的样板。

### lyflow_op_pack.cmake

CMake 在 include 这个文件之前会把 `LYFLOW_PACK_DIR` 设成包目录的绝对路径。
文件里只需要调一次 `lyflow_op_pack()`：

```cmake
file(GLOB MYPACK_SOURCES CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/ops/*.cpp")

lyflow_op_pack(
  NAME         mypack                     # 注册函数的命名空间，必填
  VERSION      0.1.0                      # 进 manifest 的 pack 字段，可选
  DEFAULT      ON                         # 仓库内的包默认编不编，可选（默认编）
  SOURCES      ${MYPACK_SOURCES}
  TEST_SOURCES "${LYFLOW_PACK_DIR}/tests/test_foo.cpp"
  PCH          "${LYFLOW_PACK_DIR}/ops/mypack_pch.h"
  INCLUDES     "D:/somewhere/include"
  LINK         lyflow_pcl_support "D:/somewhere/lib/mylib.lib"
  DEFINES      MYPACK_SOMETHING=1
  OPTIONS      /wd4996
)
```

| 关键字 | 作用 |
|---|---|
| `NAME` | 必填。注册入口是 `lyflow::packs::<NAME>::registerPackOps(Registry&)`。带连字符的名字先过一遍 `MAKE_C_IDENTIFIER`：`std-pointcloud` → `lyflow::packs::std_pointcloud` |
| `VERSION` | 可选。manifest 里每个算子的 `pack` 字段是 `名字@版本`，不给版本就只有名字 |
| `DEFAULT` | 可选，只对 `packs/*` 里的包生效。`OFF` 时要 `LYFLOW_PACKS=<名字>` 才编。宏会把结果写回 `LYFLOW_PACK_ENABLED`，包在调用之后读它决定要不要检查依赖、拷 DLL |
| `SOURCES` | 编进 `lyflow_core`（以及 dump-manifest 与测试 exe） |
| `TEST_SOURCES` | 追加到 `lyflow-core-tests` 这一个 doctest 目标，不另起 exe |
| `PCH` | 包的预编译头。领域包往往每个 TU 都拖一遍 PCL + Eigen + 领域头，不预编译一次增量重建要十秒，热重载就跟不上手 |
| `INCLUDES` | 追加到包的 include 路径（core 自己的 include 已经在里面） |
| `LINK` | 追加到 `lyflow_core` 与两个 exe 的链接库。要 PCL 就写 `lyflow_pcl_support`（见下） |
| `DEFINES` / `OPTIONS` | 只作用于包自己的 TU |

**每个包一个对象库**，所以每个包可以有自己的 PCH，互不干扰
（ADR-0014 之前是全局只能有一份，两个包给了不同的 PCH 会 `FATAL_ERROR`）。

包的 TU 带上了 core 的 include 与 `/EHsc /W4 /permissive- /utf-8 /bigobj`，
外加一组针对第三方头的告警屏蔽（PCL/Eigen 的噪声不是包作者能修的）。
**PCL 的 include 不再自动有** —— 要它就 `LINK lyflow_pcl_support`。

### 包之间的顺序

`packs/*` 按字母序 include，但**提供公共设施的包排在前面**（core 的 CMakeLists 里
有一小段显式优先级）：`std-pointcloud` 定义 `lyflow_pcl_support` 与 `lyflow_std_algo`，
`std-ml` 解析 `LYFLOW_ONNXRUNTIME_ROOT`，两者都要早于用它们的 `gap`。
加一个提供公共目标的包时记得也加进那张表。

### 要 PCL 的包

标准包 `packs/std-pointcloud` 是全仓库唯一一处 `find_package(PCL)`，
并导出一个 INTERFACE 目标 `lyflow_pcl_support`：include 目录、PCL 的库、
针对 PCL/Eigen 头的告警屏蔽都在里面。别的包链它就够了，不要自己 `find_package`。

它同时把 `packs/std-pointcloud/include/` 挂上，所以链了它的包可以直接
`#include "lyflow_pcl/adapter.h"`（LyFlow 点云 ↔ `pcl::PointCloud` 的唯一转换点，
ADR-0005）与 `#include "lyflow_pcl/pcl_path.h"`（中文路径下喂给 PCL 的窄字符串）。

### 要 2D 量测算法的包

标准包还导出第二个 INTERFACE 目标 **`lyflow_std_algo`**（[ADR-0015](adr/0015-algorithms-live-in-lyflow-packs.md)），
把 `packs/std-pointcloud/algo/` 挂成 include 根：

```cpp
#include "algo/fit2d.h"    // fitLine2D / fitAxisLine2D / fitCircle2D / fitCircleFixedRadius2D
#include "algo/icp2d.h"    // Icp2D 类
#include "algo/crop2d.h"   // insideBox2D / cropBox2D（open 是严格开区间）
#include "algo/profile_geometry.h"  // estimateProfileNormals
#include "algo/cloud2d.h"  // Cloud2D = pcl::PointCloud<pcl::PointXYZRGB>、toCloud2D
```

这些函数都在 `lyflow::std_pc` 命名空间里，吃的是 `pcl::PointXYZRGB` 的云。
领域包**不要再抄一份拟合**：`packs/gap` 就是靠它把 `GapUtils.cpp` 里的
直线/圆拟合、ICP、盒裁剪整段删掉的。

### 注册入口

```cpp
// ops/register.cpp
#include "lyflow/registry.h"

namespace lyflow::packs::mypack {

void registerFoo(Registry& r);   // 每个算子一个，写法与内置算子完全相同

void registerPackOps(Registry& r) {
  registerFoo(r);
}

}  // namespace lyflow::packs::mypack
```

算子本身怎么写和内置算子没有任何区别 —— `OperatorDesc` + 一个
`Status compute(const Inputs&, const ParamView&, Outputs&, ExecContext&)`，
约定见 `core/README.md`「写 compute 时的约定」。
`Registry::validate()` 对包里的算子和内置算子一视同仁，
所以 `lyflow-dump-manifest --check` 会把包作者的笔误一起挡下。

## 端口类型

包**不能**往类型表里加类型 —— 前端要在不知道任何包的前提下给端口着色、
在 3D 视图里叠画几何。core 已经有六种 2D 量测域的通用载荷，外加一个给推理用的 `Tensor`：

| 类型 | 载荷 | 3D 视图 |
|---|---|---|
| `Box2D` | min/max 两角，米 | 画成矩形框 |
| `Line2D` | 过一点 + 单位方向，可选带两端点 | 画成线段（无端点时按视图尺度画一条长线） |
| `Circle2D` | 圆心 + 半径 | 画成圆 |
| `Point2D` | 一个点 | 画成十字 |
| `Measurement` | 值 + 单位 + ok + 消息 + 判定 + 上下限 | Inspector 的「输出」一栏 |
| `Record` | 带类型标签的 JSON | Inspector 里显示 JSON |
| `Tensor` | 形状 + float32 数据，行主序 | Inspector 里显示形状与 min/max/mean |

领域专有的结构走 `Record`：`Record{ type: "GapAlignment", data: {...} }`。
加一个领域结构因此不用改 core，代价是它在图上只是一团 JSON，没有专门的可视化。

这些值经 `Data::valueJson()` 出现在三个地方：
`lyflow_output_info` 的 `value`、执行事件 `node_state.stats.outputs[].value`、
以及前端的 Inspector 与 3D 叠画。非有限的数写成 `null`（JSON 没有 NaN）。

## 单位

内置算子的长度参数一律是**米**，与点云同单位。包可以自己选（比如
`gap.*` 用毫米，与它的 YAML 配置一致），但要在参数的 `unit` 字段里写清楚，
并在算子内部换算 —— 端口上流动的几何值必须是米，否则叠画会错一千倍。

## 门禁

带包和不带包**两种模式都要绿**：

```powershell
pnpm check                                      # 默认：core + 默认开的 packs/*
$env:LYFLOW_PACKS="gap"; pnpm check             # 带仓库内默认关闭的包
$env:LYFLOW_OP_PACKS="…\mypack"; pnpm check     # 带外部包
```

`pnpm check:gap` 是 gap 包的完整门禁：`LYFLOW_PACKS=gap` 的 `pnpm check`
外加两条 A/B（模板路径、模型路径），见 `packs/gap/README.md`。

不带外部包那一遍是「包机制没有改变通用侧行为」的唯一证据。
另有一条更强的，改了包机制本身时值得跑：
`$env:LYFLOW_STD_PACKS="0"; pnpm check` —— 连标准包都不编。
