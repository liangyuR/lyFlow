# ADR-0013：算子包在构建期编进 core，不做运行时插件 ABI

日期：2026-09-06　状态：已采纳

## 背景

要把 `xyz-gap-inspector` 的间隙/段差测量主路径接进 LyFlow。
那些算子要直接调用领域仓库的 `xyz_gap_core.lib` / `xyz_gap_io.lib`，参数与语义都属于那个领域，
放进 LyFlow 的 `core/src/ops/` 只会把一个通用工具变成某个产线的专用件。

反过来，领域仓库也不该把 LyFlow 的执行器抄一遍。需要的是一条「领域代码留在领域仓库、
但和内置算子一样注册进注册表」的路。

三种做法：

1. **运行时插件 DLL**：core 定义一套插件 ABI，包编译成独立 DLL，启动时 LoadLibrary。
2. **库算子（ADR-0010）**：用已有的 `*.lyflow-op.json` 把内置算子拼起来。
3. **构建期算子包**：包提供源文件与链接库，编进同一个 `lyflow_core.dll`。

## 决定

**走第三种。`LYFLOW_OP_PACKS` 是分号分隔的目录列表，每个目录下有一个
`lyflow_op_pack.cmake`，把源文件、include 路径、链接库追加进 `lyflow_core`；
CMake 生成一个注册入口 `lyflow::registerOpPacks(Registry&)`，
`registerBuiltinOps()` 末尾调它。不设这个变量时一切行为不变。**

包内每个注册函数的约定名字是 `lyflow::packs::<NAME>::registerPackOps(Registry&)`，
`<NAME>` 就是 `lyflow_op_pack(NAME ...)` 给的名字。怎么写一个包见 [op-packs.md](../op-packs.md)。

排除方案 1 的理由是**跨 DLL 的 C++ ABI**。算子接口收发的是 `Data`、`Status`、
`OperatorDesc` 这些 C++ 类型，里面全是 `std::string` / `std::vector` / `std::shared_ptr`。
把它们做成稳定的插件 ABI，等于把 `c_api.h` 那一整套（char* 归属、异常不跨边界、
只导出 C 函数）再对着算子接口做一遍，而收益只是「不用重编 core」——
可我们本来就有热重载（ADR-0009），重编 core 只要几秒。

排除方案 2 的理由是**它拼不出这些算子**。库算子是内置算子的组合，
而 ICP、圆拟合、模板选择这些是新的计算，不是已有算子的连线。

## 后果

**领域算子与内置算子在运行期完全无差别。** 同一个注册表、同一个 manifest、
同一套缓存键、同一条热重载路径。前端一个字都不用改就能看见 `gap.*`，
这正是 ADR-0003「前端不硬编码任何算子」的兑现。

**代价是「带包的 core」和「不带包的 core」是两个二进制。** 所以门禁要跑两遍：

```powershell
pnpm check                                                    # 不带包
$env:LYFLOW_OP_PACKS="D:\project\xyz-gap-inspector\lyflow"; pnpm check   # 带包
```

不带包那一遍是「加了包机制之后通用侧行为不变」的唯一证据。

**包的测试进同一个 doctest 目标。** `lyflow_op_pack(TEST_SOURCES ...)` 里的文件
会被追加到 `lyflow-core-tests`，而不是另起一个 exe —— 两个测试 exe 意味着
两条 CI 路径，其中一条迟早没人跑。

**六种 2D 载荷进了 core 而不是包里。** `Box2D` / `Line2D` / `Circle2D` / `Point2D` /
`Measurement` / `Record` 是通用的（任何量测领域都要画 ROI 框、报一个带判定的数值），
放进包里的话前端就没法在不知道包的前提下叠画它们。
真正属于领域的结构（比如 `GapAlignment`）走 `Record`：带类型标签的 JSON，
加一个领域结构不用改 core。

**`lyflow_core_packs` 是个永远存在的对象库**，不带包时里面只有一个占位 TU。
用 `if(LYFLOW_OP_PACKS)` 决定要不要 `add_library` 的话，
三个消费目标（DLL、dump-manifest、tests）都要写两遍源文件列表。

**包目录进了 `bridge/build.rs` 的 rerun 列表与 `core-watch.ps1` 的指纹**，
所以改包里的算子和改 `core/src/` 里的算子一样会触发增量构建与热重载。
包目录只有轮询这一条路（事件监视只装在 `core/` 上），带包时轮询从 2 s 收紧到 1 s。
