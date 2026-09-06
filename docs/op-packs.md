# 算子包

算子包让**领域算子留在领域仓库**，但和内置算子一样注册进 LyFlow 的注册表。
为什么不是运行时插件 DLL、不是库算子，见 [ADR-0013](adr/0013-op-packs-static.md)。

一句话：包提供源文件与链接库，LyFlow 在构建期把它们编进同一个 `lyflow_core.dll`。

## 用一个包构建

`LYFLOW_OP_PACKS` 是**分号分隔的目录列表**，每个目录下要有一个 `lyflow_op_pack.cmake`：

```powershell
$env:LYFLOW_OP_PACKS = "D:\project\xyz-gap-inspector\lyflow"
pnpm check          # 或 pnpm core:build / pnpm dev
```

不设这个变量就是普通构建，行为与没有包机制时完全一致。
`scripts/build-core.ps1`、`bridge/build.rs`、`scripts/core-watch.ps1` 都读同一个变量。

包目录进了 `build.rs` 的 rerun 列表与 `core-watch.ps1` 的源码指纹，
所以改包里的算子会触发增量构建和热重载，和改 `core/src/` 一样。

### 带包和不带包混着跑

cargo 会按 feature 集把 `lyflow-app` 建好几遍（app、lib 的 test、CLI），
每一份有自己的 `OUT_DIR` 和自己的 `lyflow_core.dll`。开发构建里每个 exe
**加载的是自己那一份**（`build.rs` 发的 `LYFLOW_CORE_BIN`），
所以两种模式来回切不会串味 —— 不带包跑过 `pnpm check` 之后，
带包的 `pnpm tauri dev` 仍然是 32 个算子，反过来也一样。
`target/debug/` 与 `deps/` 里那份拷贝只服务 `tauri build` 与打包。

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

### lyflow_op_pack.cmake

CMake 在 include 这个文件之前会把 `LYFLOW_PACK_DIR` 设成包目录的绝对路径。
文件里只需要调一次 `lyflow_op_pack()`：

```cmake
file(GLOB MYPACK_SOURCES CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/ops/*.cpp")

lyflow_op_pack(
  NAME         mypack                     # 注册函数的命名空间，必填
  SOURCES      ${MYPACK_SOURCES}
  TEST_SOURCES "${LYFLOW_PACK_DIR}/tests/test_foo.cpp"
  PCH          "${LYFLOW_PACK_DIR}/ops/mypack_pch.h"
  INCLUDES     "D:/somewhere/include"
  LINK         "D:/somewhere/lib/mylib.lib"
  DEFINES      MYPACK_SOMETHING=1
  OPTIONS      /wd4996
)
```

| 关键字 | 作用 |
|---|---|
| `NAME` | 必填。注册入口是 `lyflow::packs::<NAME>::registerPackOps(Registry&)` |
| `SOURCES` | 编进 `lyflow_core`（以及 dump-manifest 与测试 exe） |
| `TEST_SOURCES` | 追加到 `lyflow-core-tests` 这一个 doctest 目标，不另起 exe |
| `PCH` | 包的预编译头。领域包往往每个 TU 都拖一遍 PCL + Eigen + 领域头，不预编译一次增量重建要十秒，热重载就跟不上手 |
| `INCLUDES` | 追加到包的 include 路径（core 自己的 include 已经在里面） |
| `LINK` | 追加到 `lyflow_core` 与两个 exe 的链接库 |
| `DEFINES` / `OPTIONS` | 只作用于包自己的 TU |

所有包共用一个对象库，所以 **`PCH` 全局只能有一份**：两个包给了不同的 PCH 会直接
`FATAL_ERROR`，而不是悄悄让后一个赢。

包的 TU 已经带上了 PCL 的 include 与 `/EHsc /W4 /permissive- /utf-8 /bigobj`，
外加一组针对第三方头的告警屏蔽（PCL/Eigen 的噪声不是包作者能修的）。

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
在 3D 视图里叠画几何。core 已经有六种 2D 量测域的通用载荷：

| 类型 | 载荷 | 3D 视图 |
|---|---|---|
| `Box2D` | min/max 两角，米 | 画成矩形框 |
| `Line2D` | 过一点 + 单位方向，可选带两端点 | 画成线段（无端点时按视图尺度画一条长线） |
| `Circle2D` | 圆心 + 半径 | 画成圆 |
| `Point2D` | 一个点 | 画成十字 |
| `Measurement` | 值 + 单位 + ok + 消息 + 判定 + 上下限 | Inspector 的「输出」一栏 |
| `Record` | 带类型标签的 JSON | Inspector 里显示 JSON |

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
pnpm check
$env:LYFLOW_OP_PACKS="…\mypack"; pnpm check
```

不带包那一遍是「包机制没有改变通用侧行为」的唯一证据。
