# M2 实施计划 —— 能跑

目标不变：**点运行，C++ 真的执行，前端看到状态，选中节点看到点云。**

这份计划按「一次做到终态」设计，不留过渡形态。凡是 M3/M4 会推翻的接口，
在这里就按最终形态定；凡是 M3/M4 才用到但现在加只要几行的能力，顺手做掉。
每一节末尾的「验收」是可机器断言的，实现顺序见最后一节。

---

## 0. 定死的决定

| # | 决定 | 一句话理由 |
|---|---|---|
| D1 | core 由 CMake 构建为 **DLL**，只导出 C ABI；Rust 用 `libloading` **运行时加载**，不静态链 import lib | PCL 是 vcpkg 动态三元组，几十个 DLL 用 cc crate 手工链是死路；运行时加载让 M3 的热重载只是「卸载再加载」，不用改任何调用点 |
| D2 | `include/` 下**零 PCL 头**。PCL 只出现在 `core/src/ops/pcl/*.cpp`，经适配器拷贝 | 保住「加算子秒级反馈」；核心数据模型不焊死在 `pcl::PointCloud<PointXYZ>` |
| D3 | 执行接口从第一天就是**异步 + 回调 + 可取消**，同一时刻一个活跃 run，新 run 抢占旧 run | architecture.md 的原话；live preview 的前提 |
| D4 | 结果留在 C++ 的**结果仓**，从第一天就按 **cacheKey** 内容寻址，runId 只是索引；点云走**二进制 IPC**，绝不 JSON | 3D 视图的唯一可行路径；M3 开缓存复用时只是不再清空 + 加 LRU，结构不变 |
| D5 | 校验返回**全部诊断**而不是第一个错误 | 前端一次标出所有红框；事后改要动三层 |
| D6 | 参数校验与默认值合并**集中在执行器**，manifest 驱动；算子拿到的是已校验的强类型参数 | 15 个算子各写一遍校验，paramPath 永远缺漏 |
| D7 | JSON 解析用 vendored `nlohmann/json` 单头；C++ 测试用 vendored `doctest` 单头 | 不走 vcpkg，保持 core 自身秒级构建 |
| D8 | core DLL 固定按 **RelWithDebInfo** 编，不跟 cargo profile 联动 | Rust 永远用 /MD，vcpkg debug 库是 /MDd，混用崩在无关的地方 |
| D9 | exe 内嵌 manifest 声明 `activeCodePage=UTF-8` | PCL 的文件 IO 走窄字符串 ANSI 代码页，中文路径否则打不开 |
| D10 | 去掉 `PointCloudXYZI` 类型；intensity/normals/rgb 是 PointCloud 的**可选通道** | 类型系统说一套、数据模型做一套，迟早出事 |

D1、D2、D4 各写一份 ADR（0004 core-as-dll、0005 pcl-boundary、0006 result-store-binary-ipc）。

---

## 1. 构建与依赖

### core/CMakeLists.txt

```
lyflow_core            SHARED   src/*.cpp src/ops/*.cpp src/ops/pcl/*.cpp
  PUBLIC  include/
  PRIVATE src/  third_party/            (nlohmann/json.hpp, doctest.h)
  PRIVATE PCL::common PCL::io PCL::filters PCL::kdtree PCL::search
          PCL::segmentation PCL::sample_consensus PCL::features
  target_precompile_headers(src/ops/pcl/pcl_pch.h)   仅 pcl/ 子目录的 TU 用
  编译选项 /W4 /permissive- /utf-8 /bigobj
  定义 LYFLOW_BUILDING_DLL -> c_api.h 里 LYFLOW_API = __declspec(dllexport)

lyflow-dump-manifest   EXE  链 lyflow_core
lyflow-core-tests      EXE  doctest，tests/*.cpp，链 lyflow_core 之外还直接编 src/（测内部类）
```

- `CMAKE_TOOLCHAIN_FILE` 取 `$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`，`VCPKG_ROOT` 缺省 `C:/vcpkg`。
- vcpkg 的 applocal 会把 PCL/boost/flann/lz4 DLL 拷到 `build/core/` 的输出目录旁，build.rs 直接整目录拷。
- `lyflow-core-tests` 和 `lyflow-dump-manifest` 也嵌 UTF-8 manifest（`.manifest` 文件加进 `target_sources`）。

### bridge/build.rs

```
cc crate  ->  cmake crate
  cmake::Config::new("../core").generator("Ninja").profile("RelWithDebInfo")
       .define("CMAKE_TOOLCHAIN_FILE", ...).build()
  不发 rustc-link-lib：DLL 在运行时用 libloading 加载（见 §7）
  把 <build>/bin/*.dll 拷到 target/<profile>/ 和 target/<profile>/deps/   (cargo test 与 tauri dev 都要找得到)
  rerun-if-changed: core/ 整树（保留现有递归收集逻辑）
tauri_build::WindowsAttributes::app_manifest(<activeCodePage>UTF-8</activeCodePage> + 原有 Tauri 内容)
```

> **实现后的结论（假设二）**：`app_manifest` **不是合并，是整份替换**
> （tauri-build 2.6.3 `src/lib.rs` 的 `res.set_manifest(&manifest)`）。
> 所以走的是计划里写的备选方案：把 Tauri 默认清单的内容（Common-Controls v6
> 依赖）抄进 `bridge/lyflow-app.manifest`，和 `activeCodePage` 放在同一份文件里。
> 漏抄的后果是文件对话框掉回 Windows 95 样式，而且不报错。

### tauri.conf.json

`bundle.resources: { "../build/core/bin/*.dll": "./" }`，Windows 上 resource 目录就是 exe 目录。
实现时要验证一次安装包能启动，这条是 M2 验收项。

> **实现后的结论（假设一）**：`bundle.resources` 的目标 `"./"` 在 Windows 上确实
> 落到 exe 同目录 —— NSIS 与 MSI 的产物里 `lyflow_core.dll` 和那二十多个 PCL/boost
> DLL 都躺在 `LyFlow.exe` 旁边。`core_ffi::dll_path()` 按「exe 同目录」找，两边对得上。
> 验证方式见 m2-acceptance.md。

### scripts/

- `build-core.ps1`：加 toolchain 参数，默认 RelWithDebInfo，构建后跑 `lyflow-core-tests`。
- `check.ps1`：C++ 编译 + 自检 + core 测试 → schema 校验（manifest、**execution-event 样例**）→ cargo test → 前端 build。

**验收**：`pnpm check` 全绿，`tauri dev` 启动，manifest 链路行为与 M1 完全一致。

---

## 2. Schema 与类型表变更

### execution-event.schema.json

- `node_state` 增加：
  - `stats: { elementCount, byteSize, outputs: [{ port, type, elementCount }] }`
  - `errors: error[]`（全部诊断），`error` 保留为 `errors[0]` 的快捷方式
- `error.phase` 已含 `compile`，补 `code` 的建议集合写进 description：
  `unknown_op | unknown_port | type_mismatch | missing_input | unknown_param | bad_param | cycle | io | cancelled | upstream_failed | internal`
- 下游因上游失败未执行的节点：`state: cancelled` + `errors[0].code = upstream_failed`。`skipped` 严格保留给缓存命中。
- `run_started` 增加 `targets: string[]`（Run to node 的目标集，全图运行时为空）和
  `nodes: [{ id, cacheKey, level }]`（编译结果。M3 的 stale 精确标记与「将重算 N 个节点」提示靠它）。

### graph-doc.schema.json

- `node` 增加 `bypass: boolean`（默认 false）。bypass 是执行语义不是 UI 状态，所以不放 `ui`。
  M2 只在 schema、Rust 结构体和 C++ parse 里接住它，执行语义 M3 实现。

### 类型表（builtin_ops.cpp）

- 删 `PointCloudXYZI`。
- 加 `Plane`（颜色 `#e0a030`，RANSAC 平面系数）。

### operator-manifest.schema.json

不变。`capabilities.cancellable` 的 description 补一句：PCL 内部算法不可中断，此类算子填 false。

---

## 3. 数据模型（data.h 修订）

```cpp
struct PointCloud {
  std::uint64_t id;                 // 进程内唯一，构造时自增分配。Indices 靠它对账
  std::vector<float> xyz;           // 3n
  std::vector<float> intensity;     // 空 或 n
  std::vector<float> normals;       // 空 或 3n
  std::vector<std::uint8_t> rgb;    // 空 或 3n
  Bounds bounds() const;
  /// 唯一的「按下标取子集」入口。所有滤波类算子必须经它出结果，属性通道才不会被悄悄丢掉
  PointCloud select(const std::vector<std::int32_t>& keep) const;
  PointCloud selectInverse(const std::vector<std::int32_t>& drop) const;
};
struct Indices  { std::uint64_t sourceCloudId; std::vector<std::int32_t> values; };
struct Transform{ float m[16]; /* 行主序 */ };
struct Plane    { float normal[3]; float d; };   // n·p + d = 0
class Data { Kind { None, PointCloud, Indices, Transform, Plane }; ... }
```

- `Indices` 消费方（extract_indices）校验 `sourceCloudId == cloud.id`，不符返回 `bad_input` 并带 `portName`。
- `kindFromTypeName`：`PointCloud/Indices/Transform/Plane` 一一对应，`Any` 接受任意。
- `Data::byteSize()` 把所有通道算进去；`elementCount()` 点云=点数、Indices=个数、其余=1。

---

## 4. 算子接口

```cpp
// include/lyflow/operator.h
class ParamView {           // 已经过执行器校验并合并默认值，取值不会失败
  bool   flag(name);  std::int64_t integer(name);  double number(name);
  const std::string& text(name);  const std::string& choice(name);     // enum
  std::array<float,3> vec3(name); std::array<float,4> vec4(name);
  std::filesystem::path path(name);   // 相对路径已按 baseDir 解析
};
class Inputs  { const Data& get(name) const; bool has(name) const; };
class Outputs { void set(name, Data); };
class ExecContext {
  bool cancelled() const;                        // 协作式；在循环里轮询
  void progress(float ratio, std::string_view msg = {});
  void log(LogLevel, std::string);
  const std::filesystem::path& baseDir() const;
};
using ComputeFn = Status (*)(const Inputs&, const ParamView&, Outputs&, ExecContext&);

struct OperatorDesc { ...; ComputeFn compute = nullptr; };   // Registry::validate 要求非空
```

- 算子里只剩两类错误需要自己报：跨参数语义（如 passthrough 的 min ≥ max，带 `paramPath="min"`）和 IO/输入内容问题（带 `portName` 或 `paramPath="path"`）。
- 算子抛出的任何异常由执行器兜住转成 `internal`，绝不穿到 ABI。

### PCL 适配器（src/ops/pcl/adapter.h，私有）

```cpp
pcl::PointCloud<pcl::PointXYZ>::Ptr toPcl(const PointCloud&);
std::vector<std::int32_t>          fromPclIndices(const pcl::PointIndices&);
PointCloud                         fromPcl(const pcl::PointCloud<pcl::PointXYZ>&, const PointCloud* attrsFrom = nullptr);
```
PCL 算子的产出尽量是 **Indices**，再用 `PointCloud::select` 出云，这样属性通道保住、拷贝只有一次。

---

## 5. 执行器（src/exec/）

```
graph_json ──parse──► GraphDoc ──validate──► Diagnostics ──compile──► Plan ──execute──► events + ResultStore
```

- **parse**（nlohmann）：nodes/edges/params 进内部结构；`x`、`ui`、`groups` 忽略。结构性问题（重复 id、边指向不存在的节点/端口）也进 Diagnostics，不抛异常。
- **validate**，全部走完再返回，每条 `{nodeId, Status}`：
  - 算子存在（含 aliases 重定向）；`opVersion` 主版本不同 → error，次版本不同 → log warn
  - 端口存在；输入端口至多一条边；required 输入已连；类型兼容（相等 / castableTo / Any）
  - 参数：未知参数名 → error；按 ParamType 校验形态；min/max 硬边界；enum 在 options 内；Path 非空（required）
  - 环检测（Kahn），环上的每个节点各一条 `cycle`
- **compile** → `Plan`：拓扑序 + 每节点的 `level`（同层可并行，M2 不用但记下）+ 合并默认值后的 ParamView + 每个输出的消费者数 + **cacheKey**。支持 `targets`：只保留目标的上游闭包。
  - `cacheKey = xxh3_128(op.id, op.version, 规范化参数 JSON, 按端口名排序的上游 cacheKey, op.externalKey(params))`。
    规范化 = 合并默认值、键排序、数字统一格式。`externalKey` 是算子可选钩子，IO 算子返回文件大小 + mtime。
    `deterministic=false` 的算子把 runId 混进去，永远不命中。xxHash 单头 vendored。
- **execute**：顺序执行。节点前 `pending→running`，成功 `done`+stats，失败 `error`+errors，其下游全部 `cancelled/upstream_failed`，其余节点照常跑（**不中止整图**，让用户一次看到所有能看到的）。取消：`atomic<bool>`，节点间检查 + `ctx.cancelled()`。
- **事件**：`seq` 单调，`at` ISO-8601，由 `EventSink` 序列化成 JSON 字符串交回调。
- **ResultStore**：`cacheKey → Data` 内容寻址，外加 `runId → (nodeId, port) → cacheKey` 索引，mutex 保护。
  M2 的 `run_free` 删索引并顺带删无人引用的 Data；M3 把「顺带删」换成 LRU 字节预算，其余不动。
  `previewCloud(runId,node,port,maxPoints)` 等步长抽样，返回 `xyz/intensity/bounds/total`。

### tests/（doctest）

拓扑与环、诊断全量返回且 paramPath 正确、默认值合并、类型兼容矩阵、取消在第 k 个节点生效、上游失败传播、ResultStore 抽样点数与 bounds、`select` 保留全部通道、中文路径下 load/save PCD。
点云全部用 `gen.synthetic` 在代码里生成，仓库不进二进制数据。

---

## 6. C ABI v2（c_api.h）

```c
LYFLOW_API const char* lyflow_version(void);
LYFLOW_API char* lyflow_manifest_json(void);
LYFLOW_API char* lyflow_manifest_problems(void);
LYFLOW_API void  lyflow_string_free(char*);

/* 只校验，同步。返回诊断 JSON 数组（可能为空数组） */
LYFLOW_API char* lyflow_validate(const char* graph_json, const char* base_dir);

typedef void (*lyflow_event_cb)(const char* event_json, void* user);
typedef struct lyflow_run lyflow_run;
typedef struct { const char* run_id; const char* base_dir; const char* const* targets; size_t target_count; } lyflow_run_options;

/* 异步。立即返回句柄；事件在 core 的工作线程上回调。cb 在 join 返回后不再被调用 */
LYFLOW_API lyflow_run* lyflow_run_start(const char* graph_json, const lyflow_run_options*, lyflow_event_cb, void* user);
LYFLOW_API void lyflow_run_cancel(lyflow_run*);
LYFLOW_API void lyflow_run_join(lyflow_run*);
LYFLOW_API void lyflow_run_free(lyflow_run*);           /* 必须在 join 之后；同时释放该 run 在结果仓的全部结果 */

/* 结果仓：run 结束后按 runId 取。preview 抽到 max_points 以内 */
typedef struct {
  uint32_t point_count, total_points, flags;   /* flags bit0: has_intensity */
  float bounds[6];
  const float* xyz; const float* intensity;
  void* handle;
} lyflow_cloud_view;
LYFLOW_API int  lyflow_output_cloud(const char* run_id, const char* node_id, const char* port, uint32_t max_points, lyflow_cloud_view* out);  /* 0 ok, 非 0 = 无此结果/类型不是点云 */
LYFLOW_API void lyflow_cloud_view_free(lyflow_cloud_view*);
LYFLOW_API char* lyflow_output_info(const char* run_id, const char* node_id);   /* 该节点所有输出的 type/elementCount JSON */
```

c_api.cpp 每个入口 `try/catch(...)`，异常转成事件里的 `internal` 或返回码，**绝不跨 ABI**。

---

## 7. Rust 桥接

### core_ffi.rs

- 不再 `extern "C" { ... }` 静态声明。`struct Core { lib: libloading::Library, version: Symbol<...>, run_start: Symbol<...>, ... }`，
  启动时从 exe 目录加载 `lyflow_core.dll`，所有调用经函数表。M3 热重载就是 drop 这个 struct 再建一个。
- `RunHandle`（`Drop` 里 cancel→join→free），持有 `Arc<Core>` 保证 DLL 活到 join 之后。
- `extern "C" fn trampoline(json, user)`：`catch_unwind` 包住，把 `&str` 解析成 `serde_json::Value` 后 `AppHandle::emit("execution-event", v)`。`user` 是 `Box<EmitCtx>` 的裸指针，生命期由 `RunHandle` 持有，join 后才 drop。
- `CloudView` 安全封装：`Drop` 调 `lyflow_cloud_view_free`。

### execution.rs（Tauri managed state）

```
RunManager { active: Option<RunHandle>, finished: Option<RunHandle> }
start(doc, base_dir, targets):
   若 active 存在 → cancel+join+free（前端收到它的 run_finished/cancelled）
   run_id = ULID；spawn 线程持有 RunHandle 直到 join；join 后 finished = 该 handle，旧 finished 释放
   返回 run_id
```
内存上界：最多两份 run 的结果（正在跑的 + 上一次完成的）。

### commands.rs 新增

| command | 返回 |
|---|---|
| `validate_graph(doc, baseDir)` | `Diagnostic[]` |
| `run_graph(doc, baseDir, targets?)` | `runId` |
| `cancel_run(runId)` | `()` |
| `get_output_info(runId, nodeId)` | JSON |
| `get_output_cloud(runId, nodeId, port, maxPoints)` | `tauri::ipc::Response` 二进制 |

二进制布局（小端）：`u32 magic 'LYPC'` `u32 pointCount` `u32 totalPoints` `u32 flags` `f32 bounds[6]` `f32 xyz[3n]` `[f32 intensity[n]]`。

### 测试

- 跑 `gen.synthetic → filter.voxel_grid` 两节点图：事件顺序 `run_started, pending/running/done ×2, run_finished(ok)`，`seq` 连续，取 output 的 pointCount 与 total 关系正确。
- 坏参数图：`run_finished(error)`，对应节点 `errors[].paramPath == "leafSize"`。
- 取消：起一个 20 节点的重图，立刻 cancel，收到 `run_finished(cancelled)` 且 join 在 1s 内返回。
- 中文路径下 `io.save_pcd` → `io.load_pcd` 往返。

---

## 8. 前端

### transport/index.ts

新增 `validateGraph / runGraph / cancelRun / getOutputInfo / getOutputCloud(→ArrayBuffer) / onExecutionEvent(cb)→unlisten`。
static 模式全部抛「浏览器模式不能运行」，状态栏已有 transport 标识。

### store/execution.ts（不进 GraphDoc，不进撤销栈）

```
runId, runStatus: idle|running|ok|error|cancelled, startedAt, durationMs
nodes: Map<nodeId, { state, durationMs, progress?, message?, errors: Diagnostic[], stats? }>
logs: LogEntry[]（环形 500 条）
stale: boolean        // run 之后 doc 有任何 change 动作 → true；节点高亮整体变虚
```
- 事件过滤：`runId !== current` 丢弃；`seq` 不连续 → console.warn 并继续。
- `paramErrors(nodeId): Map<paramPath, message>` 派生选择器给 ParamControls。

### UI

- **Toolbar**：Run（F5）、Cancel（Esc 在运行中）、耗时、结果摘要「done 7 / error 1」。未保存的图含相对路径参数 → 运行前提示先保存（baseDir 需要）。
- **OperatorNode**：读 execution store 渲染状态环：pending 灰、running 蓝色描边动画、done 绿、error 红、cancelled 琥珀、skipped 绿虚线；stale 时整体降不透明度。节点上显示 `stats.elementCount`（如「12.3 万点」）。
- **ParamControls**：`paramErrors` 命中 → 红框 + 悬浮消息；Inspector 顶部列出该节点全部 errors。
- **右键节点**：Run to node（数据通路已备，UI 一行）。
- **Viewer3D**（`three` + OrbitControls，随包打，不走 CDN）：
  - 右侧可拖分栏；选中节点变化或 `run_finished` → 取该节点第一个 PointCloud 输出，`maxPoints` 默认 2,000,000，可调。
  - `Points` + `BufferGeometry`，直接 `new Float32Array(buffer, offset)` 零拷贝进 attribute。
  - 着色：intensity（若有）/ 高度 / 单色；点大小滑块；fit-to-bounds；坐标轴与地面网格。
  - 空态文案：「未运行」「该节点无点云输出」「运行出错」。
  - 内存缓存 `runId+node+port+maxPoints → ArrayBuffer`，切换节点不重复拉。
- 交互清单标记：P0 #14、#15 完成；P1 #23（stale）、#27（Run to node）、#30（3D 预览）完成。

---

## 9. 算子清单（15 个）

| id | 实现 | 输入 → 输出 | 备注 |
|---|---|---|---|
| `gen.synthetic` | 手写 | → cloud | 平面 + 立方体 + 高斯噪声 + 离群点，seed 参数。测试与演示都靠它 |
| `io.load_pcd` | PCL | → cloud | PCD/PLY；paramPath=path 报错；中文路径 |
| `io.save_pcd` | PCL | cloud → | sink；binary/binary_compressed/ascii |
| `filter.passthrough` | 手写 | cloud → cloud | 已有描述，补 compute；min≥max 语义错 |
| `filter.voxel_grid` | 手写 | cloud → cloud | 哈希体素；centroid/nearest；minPointsPerVoxel |
| `filter.crop_box` | 手写 | cloud, [transform] → cloud | AABB，可选 Transform 输入定义盒子姿态 |
| `filter.random_sample` | 手写 | cloud → cloud | 固定 seed 可复现 |
| `filter.statistical_outlier` | PCL | cloud → cloud, removed:Indices | meanK / stddevMul |
| `filter.radius_outlier` | PCL | cloud → cloud, removed:Indices | radius / minNeighbors |
| `segment.ransac_plane` | PCL | cloud → inliers:Indices, plane:Plane | distanceThreshold / maxIterations / 可选轴约束 |
| `segment.extract_indices` | 手写 | cloud, indices → selected, rest | 校验 sourceCloudId；两个输出免去 negative 参数 |
| `features.normals` | PCL | cloud → cloud | 写 normals 通道；kSearch / radius 二选一 |
| `transform.make` | 手写 | → transform | 平移 + 欧拉角参数 |
| `transform.apply` | 手写 | cloud, transform → cloud | 同时变换 normals |
| `util.merge` | 手写 | a, b → cloud | 通道求交：一方没有的通道丢弃并 log warn |

演示 pipeline：`load_pcd → crop_box → voxel_grid → statistical_outlier → ransac_plane → extract_indices(rest) → save_pcd`，
每个节点都能在 3D 视图里点开看。

`capabilities.cancellable`：手写算子 true（循环内轮询），PCL 算子 false。

---

## 10. 实现顺序

每一步结束 `pnpm check` 必须绿；步骤内部不留半成品接口。

1. **构建骨架**（§1 + D8/D9）：core 转 DLL、cmake crate、vcpkg toolchain、DLL 拷贝、UTF-8 manifest、vendored nlohmann/doctest、tests 目标。行为与 M1 一致。
2. **契约**（§2 + 三份 ADR）：schema、类型表、roadmap 链接到本文。
3. **数据模型 + 算子接口**（§3、§4）：data.h/status.h/operator.h 定稿，Registry 校验 compute 非空，已有 3 个算子补空 compute 让链路先编过。
4. **执行器**（§5）+ doctest：先用 `gen.synthetic` 和手写算子测全流程。
5. **C ABI v2**（§6）。
6. **PCL 算子 + 其余手写算子**（§9）。
7. **Rust**（§7）+ cargo test。
8. **前端**（§8）。
9. **验收脚本**：CDP 脚本扩展。

---

## 11. 验收（M2 完成的定义）

自动化，全部在 `pnpm check` + CDP 脚本中：

- [ ] 15 个算子 manifest 自检干净，schema 校验过
- [ ] core 测试：环、诊断全量、默认值、取消、上游失败传播、select 通道保留、中文路径 PCD 往返
- [ ] Rust 测试：事件顺序与 seq、坏参数 paramPath、取消 1s 内 join、二进制输出头部字段正确
- [ ] CDP：搭演示 pipeline → F5 → 节点依次变色 → run_finished ok → 选中每个节点 3D 视图点数 > 0
- [ ] CDP：把 voxel leafSize 改成 0 → 运行 → 对应输入框红框，其他节点照常执行，下游标 cancelled
- [ ] CDP：运行中 Esc → run_finished cancelled，UI 无残留 running
- [ ] CDP：运行后改任一参数 → 全部节点标 stale
- [ ] 安装包（`tauri build`）在干净目录能启动并跑通演示 pipeline（DLL 随包）
- [ ] 中文目录下保存图 + 读 PCD + 存 PCD

---

## 12. 明确不做

- 并行执行（Plan 已带 level，M3 开）
- bypass 的执行语义、Reroute 与 `Any` 类型推导（schema 字段已接住，M3 实现）
- 缓存命中与 `skipped`（cacheKey 与内容寻址仓已在位，M3 只是打开复用 + LRU 预算）
- Live preview、算子热重载、子图
- 3D 视图里的选点、测量、多节点对比
