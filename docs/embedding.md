# 把 LyFlow 嵌进自己的进程

两条路：

- **C++ 宿主**（本文大半篇幅）：`lyflow/client.hpp` + 运行时加载的 core DLL。
  宿主自己管界面，LyFlow 只是一个算法库。
- **Rust/Tauri 宿主**（[最后一节](#rusttauri-宿主)）：宿主是 Tauri 2 的 app，
  前端嵌 `@lyflow/editor`，Rust 侧直接复用 `lyflow_lib` 的整个 IPC 层 ——
  26 条 command、`RunManager`、三条事件流、库算子扫描，一行都不用自己写。

下面先说 C++ 那条。宿主只 include 一个头：`lyflow/client.hpp`。它是 header-only 的，
只依赖同目录的 `lyflow/c_api.h`，不 include 任何 core 内部头，也不链接任何库 ——
core 是运行时加载的 DLL（[ADR-0004](adr/0004-core-as-dll.md)）。

契约版本是 **C ABI v8**（[ADR-0019](adr/0019-output-tensor-and-indices-over-abi.md)）。
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

## Rust/Tauri 宿主

宿主是自己的 Tauri 2 app（`dts-check` 就是这个形态：车门胶条面差，React 前端嵌
`@lyflow/editor`），Rust 侧把 LyFlow 的 IPC 层整个挂到自己的 `Builder` 上。
命令名与事件名**不带前缀**（LyFlow 没有做成 tauri 插件），所以前端那份
`TauriTransport` 拿来就能用，一行都不用改。

依赖上只要 `host` feature —— 它不带 `tauri-build`、不带 dialog 插件、不生成
窗口：

```toml
[dependencies]
lyflow-app = { git = "https://github.com/liangyuR/lyFlow", default-features = false, features = ["host"] }
```

crate 的 lib 名字是 `lyflow_lib`。`build.rs` 会用 CMake 编 core；外部宿主拿不到
checkout 路径，所以用环境变量按名字点包：`LYFLOW_STD_PACKS=0 LYFLOW_PACKS=dts`
得到 core + dts（一个 PCL 都不拖，见 [ADR-0015](adr/0015-algorithms-live-in-lyflow-packs.md)）。

### 三段代码

```rust
use std::sync::Arc;
use lyflow_lib::host::{self, HostConfig};

fn main() {
    // 自检不 fatal 也行 —— 宿主的界面上未必只有 LyFlow 一块。
    if let Err(e) = host::startup_self_check() {
        eprintln!("LyFlow 不可用：{e}");
    }

    let cfg = HostConfig {
        workspace_root: Some(my_workspace()),   // 相对路径按它解析，逃逸就拒绝
        library_dirs: Some(Vec::new()),         // 这个宿主不要库算子
        scenes: Some(Arc::new(MyScenes::new())),
        hot_reload: false,
    };

    host::attach(tauri::Builder::default(), cfg)
        .invoke_handler(lyflow_lib::lyflow_handler![open_camera, close_camera])
        .run(tauri::generate_context!())
        .expect("启动失败");
}
```

`attach()` 做三件事：`manage(RunManager)`、`manage(Arc<HostConfig>)`、
一个 `setup`（库算子重扫 + 可选的热重载 watcher）。

> **它占掉了 `Builder::setup`。** tauri 的 `setup` 是整份替换不是追加，所以宿主
> 要有自己的 setup 就别再调 `.setup()`，改成在自己那个里面调
> `lyflow_lib::host::setup(app.handle())`。

`lyflow_handler!` 把 LyFlow 的 26 条命令和宿主自己的命令合成一个
`invoke_handler`。它能跨 crate 是因为 `#[tauri::command]` 除了函数本身还发一对
`#[macro_export]` 的 `macro_rules!`，并在同一个模块里 `pub use` 了它们 ——
`lyflow_lib::commands::get_manifest` 这条路径对函数和对宏都解析得开，
而 `tauri::generate_handler!` 要的正是这个。宿主开了
`REMOVE_UNUSED_COMMANDS` 的话要把这些命令写进自己的 capability，
否则它们会在宏展开时被悄悄裁掉。

### 注入点云

前端点「运行」时把一个 `sceneId` 交给 `run_graph`（`TauriTransport` 已经在传了）。
Rust 侧据此找到宿主自己持有的那一片云，按 [ADR-0017](adr/0017-graph-outputs-injection-importers.md)
组装成注入项：

```rust
use std::collections::BTreeMap;
use lyflow_lib::host::{Cloud, SceneProvider};

impl SceneProvider for MyScenes {
    fn inputs(&self, scene_id: &str, graph: &lyflow_lib::GraphDoc, manifest: &serde_json::Value)
        -> Result<Vec<lyflow_lib::core_ffi::RunInput>, String>
    {
        let frame = self.sessions.lock().unwrap()
            .get(scene_id).cloned()
            .ok_or("点云会话已失效，请重新读取一次")?;
        let mut by_port = BTreeMap::new();
        by_port.insert("profile".to_string(), Cloud { xyz: frame.xyz, intensity: frame.intensity });
        host::inputs_for_source_op(graph, manifest, "dts.profile_in", &by_port)
    }
}
```

三条约定，和 C++ 那边是同一套：

1. **注入是整节点级的。** 被注入的节点 compute 整个不跑，所以它声明的**每个**
   输出端口都要给一项 —— `inputs_for_source_op` 会照着 manifest 逐个检查。
2. **按 op 找源节点，不按节点 id。** 节点 id 是图作者随手起的名字，用户在编辑器里
   重命名一个节点就该停产线的话，这个耦合太贵了。图里有 0 个或多个该 op 的节点都报错。
3. **注入数据的摘要进 cacheKey。** 换一片云一定重算；不进键的现象是
   「产线上两台车量出一模一样的值」。

没配 `scenes` 的宿主收到带 `sceneId` 的 `run_graph` 会直接报错，而不是静默地跑一张
没注入的图 —— 后者的表现是算子那句「这次运行没有注入轮廓」，离现场太远。

### 路径与 workspace_root

`workspace_root` 是 `None` 时一切照旧：`path` / `graphPath` 原样用。LyFlow 自己的壳
走系统文件对话框，拿到的本来就是用户亲自选的绝对路径。

给了 `Some(root)` 之后，所有收路径的命令（`save_graph`、`load_graph`、备份四件、
`write_file_bytes`，以及 `graphPath` 推出来的 base dir）都走一遍解析：

| 前端给的 | 结果 |
|---|---|
| `graphs/a.lyflow.json` | `<root>/graphs/a.lyflow.json` |
| `./graphs/../a.json` | `<root>/a.json`（先消 `.` 与 `..`，没逃出去就放行） |
| `../外面/a.json` | 报错「路径逃出了工作区」 |
| `D:/别处/a.json` | 原样放行 —— 绝对路径是宿主自己的 webview 给的，可信 |

消 `.` 与 `..` 是纯文本的，不碰文件系统：`canonicalize()` 要求路径已存在
（`save_graph` 写的是还不存在的文件），而且在 Windows 上会带出 `\\?\` 前缀。

## 中文路径

core 的 DLL 里所有路径都按 UTF-8 处理。宿主 **exe** 要内嵌
`activeCodePage=UTF-8` 的 manifest（仓库里那份是 `core/lyflow-utf8.manifest`），
否则 PCL 的窄字符串文件 IO 会把 UTF-8 路径按本地代码页解释，
表现是「文件存在但报不存在」。DLL 上贴这个 manifest 无效。
