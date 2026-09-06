# 把 LyFlow 嵌进自己的 C++ 进程

宿主只 include 一个头：`lyflow/client.hpp`。它是 header-only 的，
只依赖同目录的 `lyflow/c_api.h`，不 include 任何 core 内部头，也不链接任何库 ——
core 是运行时加载的 DLL（[ADR-0004](adr/0004-core-as-dll.md)）。

契约版本是 **C ABI v7**（[ADR-0017](adr/0017-graph-outputs-injection-importers.md)）。
`lyflow::kClientAbiVersion` 与 core 的 `LYFLOW_ABI_VERSION` 必须一致；对不上时
`Client` 的构造函数会抛 `ClientError`，而不是等到某次调用才崩。

## 安装布局

```
<prefix>/
  bin/                     lyflow_core.dll + 全部依赖 DLL（PCL、boost、onnxruntime…）+ lyflow.exe
  include/lyflow/          client.hpp、c_api.h 与其余公共头
  library/                 库算子目录（ADR-0010），初始为空
  examples/embed_minimal.cpp
  lyflow-config.cmake
  lyflow-config-version.cmake
```

产出方式：

```powershell
pnpm run core:build                                  # 先构建 core（含算子包）
powershell -File scripts/install-lyflow.ps1          # 默认 build/install
powershell -File scripts/install-lyflow.ps1 -Prefix D:\lyflow-runtime
```

`LYFLOW_INSTALL_PREFIX` 环境变量等价于 `-Prefix`。脚本最后会自检布局，
少一样就报错 —— 缺一个依赖 DLL 的表现是宿主那边一句「找不到模块」，离现场很远。

`bin/` 要**整目录**带进宿主的安装包：

```cmake
install(DIRECTORY "${LYFLOW_ROOT}/bin/" DESTINATION "backend/bin")
```

## CMake 消费方

```cmake
find_package(lyflow CONFIG REQUIRED)     # -DCMAKE_PREFIX_PATH=<prefix>
target_link_libraries(my_app PRIVATE lyflow::client)
```

`lyflow::client` 是 INTERFACE 目标，只带 include 路径与 `cxx_std_17`。
配置文件另外给出：

| 变量 | 内容 |
|---|---|
| `LYFLOW_BIN_DIR` | 运行时目录。宿主 exe 边上要有这一份 |
| `LYFLOW_CORE_DLL` | `Client` 构造函数要的路径 |
| `LYFLOW_CLI` | `lyflow.exe`，离线跑图/导入用 |
| `LYFLOW_LIBRARY_DIR` | 库算子目录 |
| `LYFLOW_ABI_VERSION` | 整数，与头文件里的常量对照 |
| `LYFLOW_SOURCE_COMMIT` | 产出这份安装目录的 LyFlow commit（工作区脏时带 `-dirty`），宿主用来钉版本 |

完整可编译的例子见 `examples/consumer/`（`pnpm check` 每次都会对着安装目录编它并跑一遍）。

## 最小用法

```cpp
#include "lyflow/client.hpp"

lyflow::Client client("D:/lyflow-runtime/bin/lyflow_core.dll");
if (!client.problems().empty()) throw std::runtime_error(client.problems());

const std::string diags = client.validate(graphJson, baseDir);
if (diags != "[]") { /* 诊断数组，逐条报给用户 */ }

lyflow::RunOptions options;
options.runId = "device-1-point-7";
options.baseDir = pointDir;          // 图里的相对路径参数据它解析
options.maxParallel = 2;

lyflow::RunResult result = client.run(graphJson, options);
if (!result.ok()) { /* result.diagnostics 里是 warn/error 级别的日志 */ }
// result.outputs 是 lyflow_run_outputs 的原始 JSON，宿主用自己的 JSON 库解析
```

`Client` 不解析 JSON。SDK 里引一个 JSON 库会和宿主自己那份撞版本，
而宿主必然已经有一个。事件与输出都以原始文本交出去。

## 取点云

```cpp
lyflow::CloudView view = client.cloud(result.runId, "n_merge", "cloud", /*maxPoints=*/0);
if (view.valid()) {
  const float* xyz = view.xyz();          // 3 * view.pointCount()
  const float* intensity = view.intensity();  // 或 nullptr
}
```

`CloudView` 是 RAII，析构时把缓冲还给 core。

**`RunResult` 必须还活着。** 它持有这次运行在结果仓里的索引
（`RunResult::retain`），一析构就等于 `lyflow_run_free`，之后 `cloud()` 取不到东西。
`Client` 也必须比 `RunResult` 活得久 —— 索引的释放要调回 DLL。

生产路径上不取任何点云时，跳过这一段即可；`lyflow_run_outputs` 给的三个命名输出
不涉及二进制通道。

## 注入内存里的点云

相机采到的两片云不必先落盘（[ADR-0017](adr/0017-graph-outputs-injection-importers.md)）：

```cpp
lyflow::InputCloud primary;
primary.nodeId = "n_load";
primary.port = "primary";
primary.xyz = std::move(interleavedXyz);   // x0,y0,z0,x1,y1,z1,...
lyflow::InputCloud secondary = /* 同上，port = "secondary" */;
options.inputs = { std::move(primary), std::move(secondary) };
```

三条约定：

1. 被注入的节点**整个 compute 都不会被调用**，所以它声明的每个输出端口都要给一项。
   `gap.load_profile_pair` 有 `primary` 与 `secondary` 两个，就要给两项。
2. 缓冲只需活到 `run()` 返回，core 在内部拷一份。
3. 注入数据的摘要进 cacheKey，换一片云一定重算。

## 回调版与取消

```cpp
lyflow::RunResult result = client.runAsync(graphJson, options, [&](const char* eventJson) {
  // 在 core 的工作线程上被调用。runAsync 返回后不会再被调用。
  bus.publish(eventJson);
});
```

事件 JSON 的契约是 `schema/execution-event.schema.json`。
惰性分支相关的两种（`plan_extended` 与 `stats.reason = "not_demanded"`）见
[ADR-0016](adr/0016-error-as-value-and-lazy-ports.md)。

当前 SDK 只有同步与回调两种形态，没有暴露 `cancel`：`run`/`runAsync` 内部就 join 了。
需要中途取消时用 C ABI 的 `lyflow_run_cancel`。

## 并发

core 允许多个 run 同时进行：结果仓与缓存内部各自加锁，事件回调按 runId 隔离
（`core/tests/test_flow.cpp` 里有八路并发的钉子）。
一个 `Client` 可以被多个线程共用，每台设备一条计算线程各起各的 run 是支持的形态。

Rust 桥接层那边「同一时刻一个活跃 run」是桥接层的策略，不是 core 的限制。

## 导入外部格式

```cpp
const std::string graph = client.import("StandardGap.yml", yamlText, pointDir);
if (graph.front() == '[') { /* 诊断数组 */ }
```

可用的 `kind` 见 manifest 的 `importers` 段（`client.manifest()`）。
命令行等价物是 `lyflow import <file> --kind <kind> -o <out.lyflow.json>`。

## 中文路径

core 的 DLL 里所有路径都按 UTF-8 处理。宿主 **exe** 要内嵌
`activeCodePage=UTF-8` 的 manifest（仓库里那份是 `core/lyflow-utf8.manifest`），
否则 PCL 的窄字符串文件 IO 会把 UTF-8 路径按本地代码页解释，
表现是「文件存在但报不存在」。DLL 上贴这个 manifest 无效。
