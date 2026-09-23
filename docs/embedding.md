# 把 LyFlow 嵌进自己的进程

**Rust 宿主看文末的 [Rust 客户端](#rust-客户端)**，同一套 ABI、同一份安装布局，
只是换成一个 crate。以下是 C++ 宿主。

宿主只 include 一个头：`lyflow/client.hpp`。它是 header-only 的，
只依赖同目录的 `lyflow/c_api.h`，不 include 任何 core 内部头，也不链接任何库 ——
core 是运行时加载的 DLL（[ADR-0004](adr/0004-core-as-dll.md)）。

契约版本是 **C ABI v10**（v9 的 run summary 见 [ADR-0022](adr/0022-run-summary-as-core-output.md)；
v8 的张量与下标入口见 [ADR-0019](adr/0019-output-tensor-and-indices-over-abi.md)）。
`lyflow::kClientAbiVersion` 与 core 的 `LYFLOW_ABI_VERSION` 必须一致；对不上时
`Client` 的构造函数会抛 `ClientError`，而不是等到某次调用才崩。

v10 在 `lyflow_run_options` 末尾加了 `const char* params_json`：顶层图参数的取值，
见下面「[顶层图参数](#顶层图参数)」。

v9 加的那一个入口是 `lyflow_run_summary(runId)`：一次运行的结构化收尾。
`RunResult::summary` 就是它的原文，`RunHandle::runSummary()` 也能单独取。
**宿主的成败判定读 `summary.status`（`ok` / `degraded` / `failed`），
不要自己从事件流重建** —— 那要同时处理缓存命中、惰性分支没被 demand、
失败被 `acceptsError` 端口接住三种情况。

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

## 顶层图参数

图在顶层声明了 `params`（[graph-doc.md](graph-doc.md)「顶层图参数」）时，宿主按名字传值，
不改图 JSON 里的节点参数：

```cpp
lyflow::RunOptions options;
options.setParamsJson(R"({"gapOffset": 0.12, "modelPath": "D:/models/v12s0.onnx"})");
lyflow::RunResult result = client.run(graphJson, options);
```

- C ABI：`lyflow_run_options.params_json`，一个 JSON 对象 `{名字: 值}`；NULL 或空串 = 不覆盖，全用图里的 `default`。
- C++：`lyflow::RunOptions`（`client.hpp`）的字段 `std::string paramsJson`，
  或链式的 `RunOptions& setParamsJson(std::string json)`。
- 图没声明的名字 → 这次运行的校验阶段报 `unknown_param`，一个节点都不跑。
- 值在展开期写进被绑定的节点参数，所以缓存键跟着变：改一个顶层参数，只有它绑定的节点及其下游会重算。
- CLI 的对应物是 `--param <名字>=<json>`，同一张图、同一组值，两边结果一致。

## 取点云

```cpp
lyflow::CloudView view = client.cloud(result.runId, "n_merge", "cloud", /*maxPoints=*/0);
if (view.valid()) {
  const float* xyz = view.xyz();          // 3 * view.pointCount()
  const float* intensity = view.intensity();  // 或 nullptr
}
```

`CloudView` 是 RAII，析构时把缓冲还给 core。

端口是 Bundle（`Bundle<kind>`，例如 gap 包的 `gap.read_scan` 输出的 `scan`）时，按字段取：
`client.cloud(runId, "n_scan", "scan.merged", 0)`。`lyflow_output_cloud / tensor / indices / save`
都认 `<port>.<field>`，签名不变；`lyflow_output_info` 对 Bundle 端口在它后面逐个列出字段。

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
   导入器默认产出的积木图（M8a）用 `gap.read_scan` 读剖面，它只有一个 Bundle 输出，
   不能直接注入：把它的 `source` 改成 `inputs`，在 `primary` / `secondary` 上接一个
   `gap.load_profile_pair`，注入那一个节点。
2. 缓冲只需活到 `run()` 返回，core 在内部拷一份。
3. 注入数据的摘要进 cacheKey，换一片云一定重算。

## 回调版

```cpp
lyflow::RunResult result = client.runAsync(graphJson, options, [&](const char* eventJson) {
  // 在 core 的工作线程上被调用。runAsync 返回后不会再被调用。
  bus.publish(eventJson);
});
```

事件 JSON 的契约是 `schema/execution-event.schema.json`。
惰性分支相关的两种（`plan_extended` 与 `stats.reason = "not_demanded"`）见
[ADR-0016](adr/0016-error-as-value-and-lazy-ports.md)。

## 不阻塞的 run 句柄与取消

`run` 与 `runAsync` 都在内部 join 了，调用线程被占住，也就没有办法在中途取消。
需要「先返回、后取消」的宿主（HTTP 服务的 `POST /lyflow/run` + `POST /lyflow/cancel`
就是这个形态）用 `startRun`：

```cpp
lyflow::RunHandle handle = client.startRun(graphJson, options, [&](const char* eventJson) {
  bus.publish(eventJson);              // 同样在 core 的工作线程上
});
registry.keep(handle.runId(), std::move(handle));   // 句柄存起来，函数就可以返回了

// 另一个请求线程上：
handle.cancel();                       // 尽力而为，不阻塞
handle.join();                         // 等这次运行真的结束；可重复调用
handle.status();                       // "ok" / "error" / "cancelled"
handle.outputs();                      // lyflow_run_outputs 的原始 JSON
handle.diagnostics();                  // warn/error 级别的日志事件
lyflow::CloudView view = handle.cloud("n_merge", "cloud", 20000);
```

| 成员 | 说明 |
|---|---|
| `runId()` | 这次运行的 id（`options.runId` 为空时是 `"embed-run"`） |
| `valid()` | `run_start` 成功给出了句柄 |
| `cancel()` | 置取消位，立即返回。core 保证这次运行最终仍发出一条 `run_finished` |
| `join()` | 等运行结束。幂等，多线程调用安全 |
| `status()` / `diagnostics()` | join 之前也能读，读到的是「到目前为止」；join 之后才是最终值 |
| `outputs()` | 图级命名输出的 JSON，要 join 之后才完整 |
| `cloud()` | 等价于 `client.cloud(runId(), …)`，句柄活着就取得到 |
| `result()` | join 之后装成一个 `RunResult`（不含 `events`），点云索引转交给它 |

三条约定：

1. **句柄是 move-only 的，析构会先 `join()`**。想让运行在后台继续，就得让句柄活着 ——
   丢掉句柄等于同步等它跑完。
2. 句柄持有这次运行在结果仓里的索引，与 `RunResult::retain` 是同一样东西：
   句柄（或它 `result()` 出来的 `RunResult`）一析构，`cloud()` 就再也取不到东西。
3. `Client` 必须比所有句柄活得久。

`run` 与 `runAsync` 现在就是 `startRun` + `result()` 的两个包装，行为不变。

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

## Rust 客户端

Rust 宿主用 crate `lyflow-client`（`crates/lyflow-client`），它是 `client.hpp` 的对应物：
**没有 build.rs**（不构建 core，也就不要求宿主装 CMake / Ninja / vcpkg），只依赖
`libloading`，core 同样是运行时按给定路径加载的 DLL。

```toml
[dependencies]
lyflow-client = { path = "…/LyFlow/crates/lyflow-client" }
```

```rust
use lyflow_client::{Core, RunSpec};

// 路径由宿主给 —— 安装布局里的 bin/，或自己构建的产物
let core = Core::load_from(Path::new("D:/lyflow-runtime/bin/lyflow_core.dll"))?;
core.self_check().map_err(|e| /* 算子描述不干净，拒绝启动 */ e)?;

assert_eq!(lyflow_client::ABI_VERSION, 10); // 与 core 的 LYFLOW_ABI_VERSION 对齐

// 顶层图参数：RunSpec 的 params_json: Option<&str>，或链式的 with_params_json
let spec = RunSpec::new(&graph_json, &run_id, &base_dir, &[])
    .with_params_json(r#"{"gapOffset": 0.12}"#);
```

`ABI_VERSION` 与 C++ 侧的 `lyflow::kClientAbiVersion` 是同一个数。轮廓 / 点云的运行时注入、
不阻塞的 `RunHandle`、三种 View 的取数，与上面 C++ 各节一一对应。

**进程级单例、DLL 路径解析与热重载不在这个 crate 里** —— 那三样是宿主自己的策略。
桥接层的那一份在 [`bridge/src/core_ffi.rs`](../bridge/src/core_ffi.rs)，它就是
`pub use lyflow_client::*` 再加上这三样，可以照抄。
