# 把 LyFlow 嵌进自己的进程

**Rust 宿主看文末的 [Rust 客户端](#rust-客户端)**，同一套 ABI、同一份安装布局，
只是换成一个 crate。**要把编辑器界面嵌进自己的 React 页面**看文末的
[前端编辑器](#前端编辑器lyfloweditor)。以下是 C++ 宿主。

宿主只 include 一个头：`lyflow/client.hpp`。它是 header-only 的，
只依赖同目录的 `lyflow/c_api.h`，不 include 任何 core 内部头，也不链接任何库 ——
core 是运行时加载的 DLL（[ADR-0004](adr/0004-core-as-dll.md)）。

契约版本是 **C ABI v11**（v9 的 run summary 见 [ADR-0022](adr/0022-run-summary-as-core-output.md)；
v8 的张量与下标入口见 [ADR-0019](adr/0019-output-tensor-and-indices-over-abi.md)）。
`lyflow::kClientAbiVersion` 与 core 的 `LYFLOW_ABI_VERSION` 必须一致；对不上时
`Client` 的构造函数会抛 `ClientError`，而不是等到某次调用才崩。

v10 在 `lyflow_run_options` 末尾加了 `const char* params_json`：顶层图参数的取值，
见下面「[顶层图参数](#顶层图参数)」。

v11 在它后面又加了两对：`isolate` / `isolate_count`（只运行这几个节点）与 `force` / `force_count`
（强制重算这几个节点），见下面「[部分运行：targets、isolate、force](#部分运行targetsisolateforce)」。
结构体变长了，所以 ABI 号跟着加一 —— `client.hpp` 与 `lyflow-client` 都是零初始化整个结构体再填，
不用这两项的宿主什么都不用改。

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
- 图参数声明了 `type`（完整规格，param-recipe P1.1）时，`default` 与传进来的值都先按这份规格查：
  类型、硬限位 `min`/`max`、`options`。不合法报 `bad_param`，`paramPath` 是图参数名、`nodeId` 为空，
  整次运行不执行任何节点；之后照旧走被绑定节点自己的参数规整。没有 `type` 的老图参数跳过第一步。
- 切换一组取值之前想先问一句「这组值合不合法、哪些节点会重算」：`lyflow_validate_params(graph, baseDir,
  params_json)` 与 `lyflow_plan_params(graph, baseDir, targets, n, params_json)`（v11 里追加，ABI 号不变）
  与 `lyflow_validate` / `lyflow_plan` 同义，只多一个 `params_json`。`client.hpp` 的 `validate` / `plan`
  与 `lyflow-client` 的 `validate_with_params` / `plan_with_params` 各多一个可选参数。

## 部分运行：targets、isolate、force

三个字段 id 语义相同（展开后的路径，给一个子图节点等于给它内部的全部节点），可以组合
（[docs/node-run-plan.md](node-run-plan.md)，§6 修订一）。编辑器节点标题栏上的运行按钮：
单击 = `targets: [id]`（智能运行），Shift+单击 = 再加 `force: [id]`；右键「仅此节点」= `isolate: [id]`。

- **`targets`（运行到此 / 智能运行）**：只保留目标的上游闭包，有缓存的复用、缺的或过时的照跑，下游不进计划。
- **`force`（v11，修订一 V1）**：这些节点跳过缓存查找、真跑一遍，结果覆盖结果仓里同 cacheKey 的旧结果
  （读外部文件、带隐藏随机性的算子靠这一条拿到新内容），`stats.cached` 不出现。可与 `targets`、`isolate`、
  preview 任意组合；preview 下写进预览命名空间。不在本次计划里的 id 没有效果。
- **`isolate`（v11）**：只运行这几个节点，给了它 core 就忽略 `targets`、改用同一组 id。它们自己照常查缓存
  （修订一起 isolate 不再隐含强制重算，要真跑一遍另给 `force`），区别在上游 ——
  **上游只许命中缓存。** 任何一个需要的上游在结果仓里没有当前 cacheKey 的结果，开跑前整次失败，
  一个算子都不调：`run_finished` 为 `error`，`error.code` 是 `upstream_not_ready`，
  `run_finished.diagnostics[]` 每个缺结果的上游一条（`nodeId` 指它）。那些上游没有失败，
  所以不会有它们的 `node_state`。被 demand 的惰性上游开跑前判不了，执行期撞上时那个节点以
  `upstream_not_ready` 报 error。isolate 里的静音节点照静音语义透传。
- **计划外的节点挂结果、不执行**（R7，修订一 V2 推广到所有带 `targets` 的运行，包括「运行到此」）。下游、兄弟支路这些不在本次计划里的节点，结果仓里要是有它们
  **当前** cacheKey 的全部输出，就挂进这次运行：按这次的 runId 照样取得到（`lyflow_output_info` /
  `lyflow_output_cloud` / 张量 / 下标 / `lyflow_run_outputs`），但不执行、不发任何事件。
  `run_finished.attached[]` 列出挂上的节点（只在带 targets / isolate 的运行里出现，全图运行不带；
  isolate 开跑前就失败时，挂的是整张图里有结果的那些）。没挂上的、这次也没有事件的节点，按这次的 runId 取不到输出。这是为只留最近一次运行
  索引的宿主（桌面端的 RunManager 就是）准备的：不挂的话，上一次的结果在这次运行结束时就跟着没了。
- `run_started.isolate` / `run_started.force` 原样带出这两组 id（没给是空数组），`mode` 照实写。
  isolate 与 `mode = preview` 同时给是参数错误（`bad_input`）：预览结果在另一个缓存命名空间里；force 与 preview 可以组合。
- `client.hpp` 的 `RunOptions` 没有加这两个字段（这次只接编辑器用得到的那几条路，CLI 与 MCP 也没加）；
  C++ 宿主要用就直接填 `lyflow_run_options`。Rust 是 `RunSpec::isolate` / `RunSpec::force: &[String]`。

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
primary.nodeId = "n_scan";                 // 导入器产出的积木图里的 gap.read_scan
primary.port = "primary";
primary.xyz = std::move(interleavedXyz);   // x0,y0,z0,x1,y1,z1,...（传感器帧，NaN 槽可以留着）
lyflow::InputCloud secondary = /* 同上，port = "secondary" */;
options.inputs = { std::move(primary), std::move(secondary) };
```

`port` 有两种意思，按算子声明区分：

- **是这个算子的输出端口**：整节点注入（v7 起）。被注入的节点**整个 compute 都不会被调用**，
  所以它声明的每个输出端口都要给一项 —— `gap.load_profile_pair` 有 `primary` 与 `secondary` 两个，就要给两项。
- **只是它的输入端口**：输入注入（M8b，m8-plan L18，加在 v10 里）。compute 照常调，这个输入端口的值就是
  注入的数据。`gap.read_scan` 的 `primary` / `secondary` 就是这样喂的：两个都给了就直接用它们，不读目录
  （图里原来的 `source` / `dir` 不用改；不想配目录时把 `source` 设成 `inputs`），宿主只看见一个「读剖面」节点，
  前面不必再接 `gap.load_profile_pair`。端口上不能同时有连线（`bad_input`），同一个节点也不能既注入输出又注入输入；
  名字既不是输入也不是输出报 `unknown_port`。同名的输入输出（例如 `gap.locate_template` 的 `scan`）按输出算。

两条共同的约定：

1. 缓冲只需活到 `run()` 返回，core 在内部拷一份。
2. 注入数据的摘要进 cacheKey，换一片云一定重算。

**gap 的强度在 rgb 的 R 上**（模型定位把它当特征）：注入 gap 的剖面时把 `rgb` 一起给，只给 xyz 的话
模板路径读数不变、模型路径会变。

命令行的等价物是 `lyflow run <graph> --input n_scan.primary=<a.pcd> --input n_scan.secondary=<b.pcd>`：
CLI 把 PCD 原样读出（`bridge/src/pcd.rs`，ascii / binary / binary_compressed；点序、NaN 槽、intensity、rgb 都在），
再经同一个 `lyflow_run_options.inputs` 交给 core。被 `--input` 喂了的必填输入在运行前的校验里不算 `missing_input`。

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

assert_eq!(lyflow_client::ABI_VERSION, 11); // 与 core 的 LYFLOW_ABI_VERSION 对齐

// 顶层图参数：RunSpec 的 params_json: Option<&str>，或链式的 with_params_json
let spec = RunSpec::new(&graph_json, &run_id, &base_dir, &[])
    .with_params_json(r#"{"gapOffset": 0.12}"#);
```

`ABI_VERSION` 与 C++ 侧的 `lyflow::kClientAbiVersion` 是同一个数。轮廓 / 点云的运行时注入、
不阻塞的 `RunHandle`、三种 View 的取数，与上面 C++ 各节一一对应。

**进程级单例、DLL 路径解析与热重载不在这个 crate 里** —— 那三样是宿主自己的策略。
桥接层的那一份在 [`bridge/src/core_ffi.rs`](../bridge/src/core_ffi.rs)，它就是
`pub use lyflow_client::*` 再加上这三样，可以照抄。

## 前端编辑器（`@lyflow/editor`）

节点图编辑器本身是一个 React 组件，装法、peer 依赖、`Transport`、对话框注入、主题变量
见 [`packages/editor/README.md`](../packages/editor/README.md)，最小宿主是 `examples/host-react/`。
这里只记宿主最常要调的那几个 prop：

```tsx
<LyFlowEditor
  transport={transport}          // 必填：TauriTransport / HttpTransport / StaticTransport / 自己的实现
  dialogs={dialogs}              // 可选：打开/另存/确认对话框
  graphPath="D:/工作区/流程.lyflow.json"
  onDocChange={(doc, dirty) => …}
  theme={{ "--lyflow-accent": "#ff8a3d" }}
  animations={false}             // 可选：关掉画布动效，默认 true
/>
```

`animations`（默认 `true`）管画布上的全部动效（[docs/motion-plan.md](motion-plan.md)）：节点进出场、
完成/出错的闪光与抖动、连线生长、运行时边上的数据流动、hover 反馈的过渡、自动布局的位置过渡。
设成 `false` 时这些一律直接落到终态，CSS 的循环与过渡也停掉，编辑器自己发起的「适配视图」这类视口动画也一步到位；编辑与执行的语义不受任何影响。
用户系统里设了「减少动态效果」（`prefers-reduced-motion: reduce`）时编辑器自己按 `false` 处理，
宿主不用判断。关掉之后信息不丢：正在流数据的边仍是一条静态高亮，running 的节点仍是蓝框加光。
适合的场景：录屏/截图要稳定的画面、远程桌面这类重绘很贵的环境、宿主页面自己有一套动效规范。
`examples/host-react/` 的宿主栏上有一个「动效」开关，就是这个 prop。

**自己实现 `Transport` 时**，`runGraph(doc, graphPath, options)` 的 `options` 里多了
`isolate?: string[]` 与 `force?: string[]`（展开后的路径 id）：节点标题栏的运行按钮（单击 `targets`、
Shift+单击再加 `force`）与右键三项（运行到此 / 强制重算此节点 / 仅此节点）发的就是它们，语义见上面
「[部分运行](#部分运行targetsisolateforce)」。后端要做到的最小一条：isolate 的上游没有可用结果时
发一对 `run_started`（带 `isolate`）/ `run_finished`（`error.code = upstream_not_ready`、
`diagnostics[]` 每个缺结果的上游一条），不执行任何算子 —— 编辑器据此弹 warn 级 toast、把缺结果的上游
闪一下。`run_finished.attached[]`（R7 / V2）告诉编辑器哪些计划外节点的输出按新 runId 还取得到：带 targets
的运行收场时，带了这个字段，编辑器就把节点表里这次没有事件、又不在 attached 里的节点退回 idle；
不带（老后端）就全部照旧。不认识这个字段的老后端会把它当成普通的全图运行，所以宿主换 core 时要一起换。
HTTP 契约里对应 `POST /lyflow/run` 信封的 `isolate` / `force` 字段（[http-transport.md](http-transport.md)）。
