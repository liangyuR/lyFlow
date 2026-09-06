# 阶段 A：LyFlow 为被嵌入做准备

上游设计：[gap-inspector-integration-design.md](gap-inspector-integration-design.md) §2。本计划把 §2 的八项能力拆成两个实施包：
**A1** core / C ABI / 算子包 / 安装布局；**A2** 前端拆成 `@lyflow/editor` 与 `HttpTransport`。A1 先做，A2 对着 A1 冻结的接口做。

## A1 决定

| # | 决定 |
|---|---|
| A1-1 | C ABI 升到 **v7**，一次加齐：`lyflow_run_outputs`、`lyflow_import`、`lyflow_run_options.inputs`、`lyflow_run_options.input_count`。v7 之后阶段 B 期间 ABI 冻结 |
| A1-2 | **并发 run**：core 本来允许多个 `lyflow_run_start` 并存，用测试钉住：8 个并发 run、各自事件回调只收到自己的 runId、结果仓与缓存无数据竞争（测试开 `/fsanitize=address` 不现实，用 ThreadSanitizer 风格的断言 + 高重复次数）。Rust 桥接层的单活跃策略不变 |
| A1-3 | **图级输出**：GraphDoc 顶层 `outputs: { name: { node, port } }`（schema、Rust 结构体、C++ parse、前端类型）。校验：node/port 存在、名字唯一。`lyflow_run_outputs(run)` 返回 `{ name: { type, value?, elementCount, node, port } }`，点云给元信息。子图内部端口用路径 id |
| A1-4 | **Error 作为值**：`Data::Kind::Error`，载荷 = Status（phase、code、message、paramPath、portName、nodeId）。端口声明 `acceptsError=true`（`Port` 结构体加字段，manifest schema 加可选布尔）时，上游失败的节点仍视为「已产出」，下游拿到 Error Data；未声明的端口维持 `cancelled/upstream_failed` |
| A1-5 | **惰性端口**：`Port.lazy=true`。编译期：惰性端口的上游闭包中**只被惰性路径依赖**的节点标记 `deferred`，不进初始拓扑序；`run_started.nodes` 只列非 deferred 节点。运行期：算子 compute 返回 `Status::Demand("b")`，执行器把该闭包编译进计划（发 `plan_extended` 事件，字段同 `run_started.nodes`），跑完后**重新调用** compute。未被 demand 的 deferred 节点在 `run_finished` 前发 `node_state: skipped, stats.reason="not_demanded"`。缓存键不受影响 |
| A1-6 | 新算子（core 内置，属于平台语义）：`flow.fallback(a: acceptsError, b: lazy) -> out:Any`，`flow.select(cond, a: lazy, b: lazy) -> out`（cond 为 Measurement 的 ok 或 Record 的布尔字段 `path` 参数）。`Any` 推导：out 的类型 = a 与 b 的公共类型，不一致报 type_mismatch |
| A1-7 | **导入器**：`Registry::addImporter(kind, fn: (text, baseDir) -> graph JSON)`；`lyflow_import(kind, text, base_dir)`；CLI `lyflow import --kind StandardGap.yml file -o graph.lyflow.json`。gap 包注册 `StandardGap.yml` 导入器（C++ 移植 `packs/gap/tools/lyflow_graph_from_config.py` 的三种形态：模板路径、模型路径、带 `flow.fallback` 的完整图；用 `--mode template|model|auto`，auto 依 setting 的 model_roi 决定），并给 39 份配置做**逐节点等价**测试（与 Python 生成器输出比较：节点集合、边集合、参数值） |
| A1-8 | **运行时注入**：`lyflow_run_options.inputs[]`，每项 `{ node_id, port, kind=PointCloud, xyz, intensity?, count }`；执行器把该节点标 `provided`，跳过 compute，输出从注入数据构造（`sourceCloudId` 新分配）。cacheKey 混入注入数据的 xxh3 |
| A1-9 | **`gap.result_bundle`**：输入 gap、flush（Measurement）、四框、fits Record（fit_line / fit_gap_circles 的 Record 输出）、cropStatus Record、alignment Record（可选）、fallback Record（可选）；输出 `Record{type:"GapResultBundle"}`，字段与 `xyz-gap-inspector/src/gap_core/MeasurementTypes.hpp` 的 `QualityMetrics` 一一对应，多加 `pack_versions`、`graph_sha256`（由执行器注入到 ctx）。生成器/导入器产出的图都接上它并声明 `outputs: {gap, flush, bundle}` |
| A1-10 | **嵌入 SDK** `include/lyflow/client.hpp`：header-only，Windows 用 `LoadLibraryExW` + `LOAD_WITH_ALTERED_SEARCH_PATH`；`Client(dllPath)`、`manifest()`、`validate()`、`import()`、`Run run(graph, RunOptions)` 同步版返回 `RunResult{status, outputs(json), events(vector), diagnostics}`，回调版 `runAsync(graph, opts, onEvent)`；`cloud(runId,node,port)` 返回 `CloudView` RAII。示例程序 `examples/embed_minimal.cpp` 跑合成图，进 `pnpm check` |
| A1-11 | **安装布局**：`cmake --install` 产出 `<prefix>/bin`（core DLL + 全部依赖 DLL + lyflow CLI）、`<prefix>/include/lyflow/`、`<prefix>/library/`（空目录）、`<prefix>/lyflow-config.cmake`（导入目标 `lyflow::client` INTERFACE，带 include 路径与 `LYFLOW_BIN_DIR`）。`scripts/install-lyflow.ps1` 一键产出到 `LYFLOW_INSTALL_PREFIX`（默认 `build/install`）。`pnpm check` 里用一个独立的 CMake 项目 `examples/consumer/` 对着安装目录 `find_package(lyflow)` 编译并运行 `embed_minimal` |

## A1 验收

- [ ] 默认 `pnpm check` 与 `pnpm check:gap` 全绿；两条 A/B 39/39
- [ ] 并发：8 个 run 同时跑 39 张 gap 图（随机分配），结果与串行一致，事件按 runId 隔离
- [ ] `flow.fallback`：合成图 a 失败 → b 被 demand 且结果正确；a 成功 → b 闭包节点 `skipped/not_demanded`，且 b 闭包里的算子 compute **未被调用**（用 `test.*` 计数算子断言）
- [ ] 39 份 StandardGap.yml：C++ 导入器与 Python 生成器逐节点等价（三种模式）；导入的模型模式图带 `flow.fallback` 时，A/B 对模型基线 39/39，其中回退触发的样本与批测器 `fallback_reason` 非空的样本集合一致
- [ ] 注入：用内存点云注入替代 `gap.load_profile_pair` 跑 R1/R5，数值与文件路径版逐位相同
- [ ] `lyflow_run_outputs` 对 39 张图给出 gap/flush/bundle；bundle 的 `effective_roi`、`fits[].inlier_count`、`point_counts`、`roi_source`、`crop_status` 与 diagnostics.jsonl 基线一致
- [ ] `examples/consumer` 从安装目录 `find_package(lyflow)` 编译，`embed_minimal` 跑通合成图并读到 outputs
- [ ] e2e：`plan_extended` 与 `not_demanded` 在前端正确显示（半透明），stale 逻辑不误报
- [ ] 文档：ADR-0016（error-as-value 与惰性端口）、ADR-0017（图级输出与注入）、`docs/embedding.md`（client.hpp 用法、安装布局）、schema 更新、`core/README.md`

## A2 决定

| # | 决定 |
|---|---|
| A2-1 | pnpm workspace 新增 `packages/editor`（包名 `@lyflow/editor`）：从 `app/src` 迁入 components、stores、lib、types、transport 接口；导出 `<LyFlowEditor transport graphPath? onDocChange? theme?>`、`Transport` 类型、`TauriTransport`、`HttpTransport`、`StaticTransport`。`app/` 只剩入口、窗口、菜单、文件对话框、devbridge |
| A2-2 | `HttpTransport(baseUrl, token?)`：REST 与 Tauri command 一一对应（`GET /lyflow/manifest`、`POST /lyflow/validate`、`/plan`、`/run`、`/cancel`、`GET /lyflow/runs/:id/outputs`、`GET /lyflow/runs/:id/clouds/:node/:port` 二进制、`/library`、`/import`）；事件走 WebSocket `/lyflow/events`，消息体就是 ExecutionEvent。契约写进 `docs/http-transport.md`，并提供一个最小 Node 桩服务器 `packages/editor/test-server/`（调用 `lyflow` CLI 或直接经 Rust bridge）供 e2e |
| A2-3 | React、React Flow、three、zustand 为 peerDependencies；样式全部用 `--lyflow-*` CSS 变量并给默认值；快捷键监听挂在编辑器根元素，不挂 window |
| A2-4 | `app/` 的 e2e 不变（仍 CDP 驱动 Tauri）；新增 `pnpm e2e:http`：用 Node 桩 + 浏览器（Playwright 或 CDP 驱动系统 Chrome）跑编辑与运行子集 |
| A2-5 | 前端显示 `plan_extended` 追加的节点、`not_demanded` 半透明、`Error` 类型端口颜色、图级输出的「标为输出」右键项与 Inspector 列表 |

## A2 验收

- [ ] `pnpm check` 全绿；`pnpm e2e` 全绿（Tauri 壳行为不变）
- [ ] `pnpm e2e:http` 全绿：HttpTransport 下打开图、改参数、运行、看 3D、取输出
- [ ] `@lyflow/editor` 能被一个最小 Vite + React 宿主（`examples/host-react/`）安装并渲染，宿主自己的 React 实例被复用（peerDependency 检查）
- [ ] 文档：`packages/editor/README.md`、`docs/http-transport.md`、ADR-0018（editor-as-package）

## 不做

- 业务仓库改动（阶段 B）
- 运行时插件 DLL
- 编辑器的多文档/多标签
