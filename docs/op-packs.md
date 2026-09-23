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
| `std-pointcloud` | 0.1.0 | ON | 19 个点云 / 2D 量测 / 编辑算子 | PCL |
| `std-ml` | 0.1.0 | ON | `ml.onnx_run` | onnxruntime |
| `gap` | 0.2.0 | **OFF** | 21 个 `gap.*` | PCL、yaml-cpp、onnxruntime |

`DEFAULT OFF` 的包用 **`LYFLOW_PACKS`** 按名字打开（分号分隔的**包名**，
不是目录 —— 那是 `LYFLOW_OP_PACKS` 的事）：

```powershell
$env:LYFLOW_PACKS = "gap"
pnpm check          # 或 pnpm check:gap
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

## 输出端口契约

**算子声明过的每一个输出端口，`compute` 都必须写。**少写一个，执行器在该节点上报
`output_not_written`（错误里带端口名），而不是让下游收到一个空 `Data` 再报「上游没有产出」。

```cpp
op.outputs = {
    Port{"gap", "Measurement", "Gap", "间隙。", true},
    Port{"flush", "Measurement", "Flush", "面差。", true},
};
```

这两个端口在**每一条**返回 `Status::Ok()` 的路径上都要 `outputs.set(...)`。
「这次算不出来」不是不写的理由 —— 写一个 `ok=false` 的 `Measurement`、一个空点云、
一条没有端点的直线，都比不写强：下游至少能判断，而不是整条链崩在一个 `output_not_written` 上。

`Port.required` **只对输入有意义**。它说的是「这个输入端口必须连上」，
放在输出端口上不会让那个输出变成可选 —— 输出端口没有「可选」这回事。
真的可能没有的东西，要么如上给一个明确表示「无效」的值，要么干脆别声明这个端口。

（静音节点是唯一的例外：`bypass` 的节点找不到可透传的源时输出是空的，报错留给下游。）

## 约束写在哪里

算子的约束只有三个去处，**不写成给人读的散文**：

| 约束依赖什么 | 写在哪 | 什么时候报 |
|---|---|---|
| 只依赖参数与连接关系（「`dirMode` 不是 `free` 就必须接 `refLine`」「半径上限不能小于下限」） | `OperatorDesc::validate` | 加载期，`buildPlan` 里，图根本跑不起来 |
| 数据到达端口时就能核对的数值不变量（点数、有限性、张量形状、Record 类型） | 端口 `contract`（下一节） | 输入绑定时，第一帧 |
| 依赖数据内容的（弧太短、内点太少、截取没生效） | 运行期信号：quality 字段、warn 日志、错误值 | 运行期，每帧 |

用法说明（「这个旋钮什么时候该开」「有哪个更合适的替代算子」）写进 `op.doc` 或对应参数的 `doc`。

### 加载期校验（`validate`）

`OperatorDesc` 上可选的 C++ 函数指针，不进 manifest JSON：

```cpp
using ValidateFn = std::vector<lyflow::Issue> (*)(const lyflow::ParamView& params,
                                                  const std::set<std::string>& connectedInputs);

struct Issue {            // lyflow/status.h
  Severity severity;      // Error / Warning
  Status status;          // code 通常是 bad_param；paramPath / portName 用来定位
};
```

它是**纯函数**，只看两样东西：解析后的参数（已合并默认值与绑定值，包括子图提升参数与顶层图参数
写进来的值）和已连接的输入端口名集合。**不给任何数据** —— 点云、上游输出一概拿不到；
需要看数据才能判断的，就不属于这里。

```cpp
std::vector<lyflow::Issue> validateFitLine(const lyflow::ParamView& params,
                                           const std::set<std::string>& connected) {
  std::vector<lyflow::Issue> issues;
  if (params.choice("dirMode") != "free" && connected.count("refLine") == 0) {
    issues.push_back(lyflow::Issue::error("bad_param", "dirMode 不是 free 时必须接 refLine",
                                          "dirMode", "refLine"));
  }
  return issues;
}

op.validate = &validateFitLine;
```

`buildPlan` 在参数解析与连边之后对每个节点调用它：

- **error** 进 `Phase::Validate` 的诊断，该节点无效，plan 被阻断 —— `lyflow validate` 非零退出，
  `lyflow run` 在任何节点开始执行之前就失败。`phase` 由 `buildPlan` 统一写成 `validate`，钩子里写什么都不算数。
- **warning** 出现在 `lyflow validate` 的诊断数组里（`severity: "warning"`），运行时走 warn 日志通道，
  不阻断执行。

规矩：**只依赖参数与连接关系的检查写在 `validate` 里，`compute` 里不再保留一份副本。**
两处判断同一个错误，迟早会不一致。

## 端口契约

有一类约束是一个能在**数据到达端口的那一刻**就核对的数值不变量 ——
「这必须是 1280 个点」「这个张量必须全是有限值」。这类约束写进端口的 `contract`（[ADR-0024](adr/0024-port-contracts-four-kinds.md)），第一帧违反就报，
不用等到下游某个算子因为形状不对而拟合失败，再倒回来 grep 是谁定的这条规矩。

只有四种键，四行例子：

```jsonc
"contract": { "elementCount": { "eq": 1280 } }       // 点云恰好 1280 个点；Indices 是下标个数
"contract": { "finite": true }                        // 坐标 / 张量元素 / 测量值不能有 NaN、Inf
"contract": { "shape": [2, -1, 1280] }                // 张量形状，-1 = 这一维随便
"contract": { "recordType": "GapLabels" }             // Record 的 type 字串必须是这个
```

`elementCount` 可以写 `{ "min": n }` / `{ "max": n }` / 两者都写，但不能跟 `eq` 混用。

什么时候该写：**能写成这四种之一的数值不变量就写**（点数、有限性、张量形状、Record 类型）。
判断标准很直接：这条约束能不能在值到达端口的那一刻，不看语义地、只用一个数字或一个布尔值核对完？
能就是契约；只看参数与连接关系就能判的是 `validate`；都不是的，做成运行期信号（quality 字段、
warn 日志或错误值）。

C++ 侧的写法（`core/include/lyflow/manifest.h` 里的辅助函数，因为 `Port` 是聚合初始化、
尾部字段要么全填要么不填，这两个函数省得每个算子都重写前面几个 `false`/`0`）：

```cpp
op.inputs = {
    withContract(Port{"primary", "PointCloud", "Primary", "Master 剖面，1280 槽。", true},
                 {{"elementCount", {{"eq", 1280}}}}),
};
op.outputs = {
    withExample(Port{"quality", "Record", "Quality", "GapQuality。", true},
                {{"kind", "Record"}, {"type", "GapQuality"},
                 {"data", {{"inlierCount", 812}, {"ok", true}}}}),
};
```

违反时长什么样：该节点 `error`，`code` 是 `contract_violation`，`message` 带期望与实际
（例如「元素数应当是 1280，实际是 1230」），`portName` 指向那个输入端口；同一条违反同时进
run summary 的 `contractViolations`（`{ node, port, expected, actual }`，见
[ADR-0022](adr/0022-run-summary-as-core-output.md)）。静音节点的透传与流过 `acceptsError`
端口的 Error 值不检查 —— 前者只是搬运，后者流的是失败本身。没声明契约的端口一次遍历都不做，
默认零开销。

gap 包是第一个用上它的：20 个输入端口声明了契约。第一条就是空槽 bug 那个真实案例 ——
`gap.profile_tensor` 的两片输入剖面 `elementCount.eq = 1280`。**它刻意没有声明 `finite`**，
虽然 m6-plan §3 原本是这么写的：这个算子要的恰恰是**保留了 NaN 空槽**的原始剖面
（`load` 时把 `dropNonFinite` 关掉），声明 `finite: true` 会把它唯一正确的输入判成违反。
契约照源码写，不照计划写。

另一批值得看的是 `gap.result_bundle`：六个 Record 入口（`fits` / `fitBase` / `fitRef` /
`cropStatus` / `alignment` / `fallback`）各声明了 `recordType`。那个算子原本是 duck typing 的
—— 接错一份 Record 不报错，只会让 bundle 里对应的那几格悄悄空着，而 bundle 正是业务侧写
`results.csv` 的唯一来源。这类「静默的空格」正是契约最该拦的东西。

核对当前状态：`lyflow manifest | jq '[.operators[].inputs[] | select(.contract)] | length'`。

顺带一提端口 `example`（上面 `withExample` 那半段）：它跟 `contract` 是两件不同的事 ——
`example` 不参与任何校验，只回答「这里长什么样」。Record 端口只有一个 `type` 字串的话，
「`data.inlierCount` 到底存不存在」得翻算子实现才知道；一份从真实 run 裁出来的样例就够消掉
这一次试错。不上 JSON Schema 是因为那份 schema 的维护成本现在不值。

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

`pnpm check:gap` 是 gap 包的门禁：`LYFLOW_PACKS=gap` 的 `pnpm check`。
`packs/gap/tools/` 下的 A/B 是历史对拍工具，不是门禁，见 `packs/gap/README.md`。

不带外部包那一遍是「包机制没有改变通用侧行为」的唯一证据。
另有一条更强的，改了包机制本身时值得跑：
`$env:LYFLOW_STD_PACKS="0"; pnpm check` —— 连标准包都不编。
