# xyz-gap-inspector 接入 LyFlow：设计

目标：业务系统不再链接自己的算法库，而是在进程内加载 LyFlow core DLL 执行每个测点的一张
`.lyflow.json`；参数编辑换成嵌入的 LyFlow 编辑器；算法的归属从业务仓库转到 LyFlow 的 `packs/`。

## 0. 已拍的决定

| # | 决定 | 备注 |
|---|---|---|
| I1 | 服务进程内加载 `lyflow_core.dll`，经 C ABI 执行图 | 不走 CLI 子进程。崩溃隔离靠现有 sidecar 重启策略 |
| I2 | 每个测点一张 `<point>/<point>.lyflow.json`，替代 `StandardGap.yml` | `<point>.yml`（enable / position / alias）保留 |
| I3 | 参数编辑换成嵌入的 LyFlow 编辑器，形态是 **npm 包** `@lyflow/editor` 嵌进业务 UI | GapCanvas、ParamTree、XGPC 最终退休 |
| I4 | 模型失败回退模板路径由 LyFlow 的**条件分支节点**表达，**惰性**调度 | 主路径失败才算备用路径 |
| I5 | **不双轨**，直接切换；算法源码、`gap_batch_runner`、ParamTree 在切换 commit 里删除 | 靠 39 样本 A/B 与 194 车回放事后验证，失败回退靠 git |

## 1. 分层

```
xyz-gap-inspector（业务）              LyFlow（平台 + 算法）
──────────────────────────             ─────────────────────────────────────
PLC / 相机 / 采集 / 落盘               core DLL：执行器、缓存、C ABI
测量调度（每设备一条 strand）   ──►    packs/std-pointcloud、std-ml、gap
结果 CSV / PLC 回写 / web display      @lyflow/editor（React 组件包）
数据库目录 + 模板文件                  lyflow CLI + packs/gap/tools（A/B、导入）
Tauri UI（antd）+ 嵌入的编辑器
```

业务仓库保留的算法相关代码只剩两类：结果类型的**消费者**（CSV 列、判定、场景 JSON、异常图），
以及模板 PCD 的创建/裁剪（纯 PCL 文件操作，本来就不在算法库里）。

## 2. LyFlow 侧需要新增的能力

### 2.1 嵌入 SDK：`lyflow/client.hpp`

头文件即库的 C++ 封装，包在 C ABI 之上，供业务 C++ 进程用：
加载 DLL（`LoadLibraryEx` + `LOAD_WITH_ALTERED_SEARCH_PATH`，与 Rust 侧一致）、`Client::run(graphJson, options)` 同步阻塞版与回调版、
事件回调转成 `std::function`、`outputs()` 取图级输出、`cloud(runId, node, port)` 取二进制点云。
业务只 include 这一个头，不接触 C ABI 细节。随 LyFlow 发布，进 `install(FILES)`。

### 2.2 并发运行

业务每台设备一条计算线程，现场 2 台，各自独立的 run 必须能**同时**进行。
Rust 桥接层的「同一时刻一个活跃 run」是桥接层策略，不是 core 的限制；需要用测试钉住：
N 个 `lyflow_run_start` 并发、结果仓与缓存线程安全、事件回调按 run 隔离。
gap 包里的 `OnnxRoiPredictor` 按模型路径缓存一个实例，`predict` 内部已加锁，可跨 run 共享。

### 2.3 图级命名输出

GraphDoc 新增顶层 `outputs: { <name>: { node, port } }`。C ABI 新增 `lyflow_run_outputs(run) -> JSON`，
返回每个命名输出的 `valueJson`（点云给元信息，二进制仍走 `lyflow_output_cloud`）。
业务只认名字：`gap`、`flush`、`bundle`（见 2.6），不认节点 id。编辑器里可以把任意端口「标为图输出」。

### 2.4 条件分支：错误作为值 + 惰性端口

执行器两条新语义，都是端口级声明：

- `acceptsError`：该输入端口的上游失败时，本节点**不**被连坐为 `cancelled`，而是收到一个 `Error` Data
  （新 Kind，载荷是那条 Status）。
- `lazy`：该输入端口的上游闭包不在编译期进计划，节点在 compute 里调用 `ctx.demand("b")` 时才调度；
  执行器随后重新调用 compute。图里其余部分不受影响，缓存键不变。

据此提供 `flow.fallback(a: acceptsError, b: lazy) -> out`：a 成功则透传 a，否则 demand b 并透传 b；
两条都失败则报 a 的错误并附 b 的错误。另给 `flow.select(cond: Measurement|Record, a, b)`。
事件流里被跳过的备用闭包节点状态为 `skipped`，stats 里标 `reason: not_demanded`，前端把它们画成半透明。

### 2.5 导入器机制

包可以注册「文本 → 图」的导入器：`registerImporter("StandardGap.yml", fn)`；C ABI `lyflow_import(kind, text, baseDir) -> graph JSON`。
gap 包把 Python 生成器的逻辑移植成 C++ 导入器（模板路径 / 模型路径 / 带 `flow.fallback` 的完整图）。
业务的数据库迁移 v5 靠它把每个 `StandardGap.yml` 转成 `.lyflow.json`，Python 版保留给离线脚本。

### 2.6 `gap.result_bundle`

一个汇聚节点：输入两个 Measurement、四个 ROI 框、拟合输出的 Record、裁剪窗 status、对齐 Record（可选）、
`flow.fallback` 的选择记录；输出一个 `Record{type:"GapResultBundle"}`，字段与旧 `QualityMetrics`
一一对应（`effective_roi`、`fits[]`、`icp[]`、`point_counts`、`roi_source`、`crop_status`、`fallback_reason`、`timings`）。
业务侧 `results.csv`、`metadata.json`、交互计算 JSON 的列全部由它填，14 个依赖 `results.csv` 的 Python 工具不用改格式。
`config_sha256` 换成图文件的 sha256，并追加 `pack_versions`（manifest 里的 `pack` 字段汇总）。

### 2.7 `@lyflow/editor` 包

把 `app/src` 拆成两层：`packages/editor`（画布、参数面板、Inspector、3D 视图、stores、typecheck、keymap，导出
`<LyFlowEditor transport={...} graphPath={...} />` 与 `Transport` 接口）和 `app/`（Tauri 壳，只剩窗口、菜单、文件对话框、
`TauriTransport`）。新增 `HttpTransport`：REST + WebSocket，接口一一对应现有 Tauri command
（manifest、validate、plan、run/cancel、output_info、output_cloud 二进制、library、events）。
样式用 CSS 变量，宿主可覆盖主题；快捷键作用域限制在编辑器容器内，不抢宿主的键。
包以 workspace 形式发布（业务 UI 通过文件路径或私有 registry 依赖），版本与 core 的 C ABI 版本绑定。

### 2.8 路径约定

图里的 Path 参数相对**图文件所在目录**解析（LyFlow 已有 baseDir 机制）。
模板 PCD 放 `<point>/templates/`，图里写 `templates/f1_left.pcd`。重算冻结时整目录复制即可，不改图内容。

## 3. 业务侧改动

### 3.1 数据库 v5

```
car_config/
  database_manifest.yml          schema_version: 5, lyflow_abi: 6, pack_versions: {...}
  <车型>/<device>/
    plc_bytes.yml
    <point>/
      <point>.yml                enable / point_type / position / alias  （active_equipment 删除）
      <point>.lyflow.json        测量图
      templates/                 模板 PCD（原 StandardGap/）
```

`PrepareDatabase` 的 v4→v5 迁移：对每个点调用 `lyflow_import("StandardGap.yml", ...)`，
模型路径按 `setting.yml` 的 `model_roi` 决定是否生成带 `flow.fallback` 的图；`StandardGap/` 改名 `templates/`；
`parameter_schema_version` / `parameter_template_sha256` 那套参数升级机制退休，由 LyFlow 的算子版本迁移（诊断回写）接管：
打开图时 `lyflow_validate` 返回的迁移诊断由业务落盘。

### 3.2 测量路径

`DoMeasurement()`：读 `<point>.yml` + 读图文件（按 mtime 缓存图文本与 sha256，不再每节拍解析 YAML）；
两片云仍先落盘再提交。`MeasureComputeJob` 携带图 JSON、两片云、baseDir。
`ComputeMeasurement()`：把两片云以 `gap.load_profile_pair` 的**内存输入**形式注入。
为此 LyFlow 需要「运行时注入源节点数据」：C ABI `lyflow_run_options.inputs`，键为节点 id + 端口，值为点云缓冲；
执行器把对应源节点标为 `provided`，不执行其 compute。这是比「先写临时 PCD 再读」干净得多的路，也是以后相机直连 LyFlow 的口子。
结果：`lyflow_run_outputs` 取 `gap`、`flush`、`bundle`，装配成现有的 `MeasurementResult`（保留该类型作为业务内部 DTO），
下游 CSV、判定、web display、PLC 回写零改动。失败映射：`Error` Data 的 `code` → `FailureCode`，节点所属阶段由 bundle 给出。

生产路径的 `capture_geometry=false` 对应：不取任何点云输出，只取三个命名输出。

### 3.3 重算、交互计算、异常图

三处都改为同一个 `LyFlowMeasurer` 适配类：输入图 + 两片云（或 PCD 路径）+ 选项，输出 `MeasurementResult`。
交互计算的 `parameters_override` 改为「对图的参数 patch」（节点 id + 参数名 + 值），不落盘直接跑，与编辑器的 live preview 是同一条路。
异常图渲染需要几何：取 `bundle` 里的框、线、圆坐标即可，不再需要 `DebugGeometry`。

### 3.4 服务 API

新增 `/lyflow/*`：`manifest`、`validate`、`plan`、`run`（返回 runId，事件经现有 WS 通道，事件类型 `lyflow.event`）、
`cancel`、`outputs`、`output_cloud`（二进制）、`library`。图的读写走 `/database/.../graph`（`GET`/`PUT`，整文件），
`PATCH …/configs/:name` 与 `/config/schema`、`/config/roi-types`、`/database/parameter-upgrade/*` 删除。
模板的创建/裁剪/undo/commit 端点保留（它们只动 PCD 文件），路径改到 `templates/`。

### 3.5 UI

`YmlPage` 变成 `GraphPage`：左侧 `<LyFlowEditor>`，右侧保留结论面板与模板工具栏；「计算」按钮即编辑器的运行；
交互计算页复用同一组件，加载历史点云时以内存输入注入。删除 `ParamTree/*`、`GapCanvas/*`、`net/xgpc.ts`、`cloudBuffer.ts`；
`stores/interactive.ts`、`stores/db.ts` 中与 YAML 参数相关的状态删除。

### 3.6 删除清单（同一切换 commit）

`src/gap_core`、`src/gap_detection`、`src/gap_ml`、`src/gap_io` 中除 `Sha256` 外的全部；`src/gap_batch`；
`param_config/StandardGap.yml`、`point_template.yml`、`parameter_migrations.yml`；`properties*.yml`、`roi_type.yml`、`PropertySchema`；
`DetectionConfigurationAdapter`、`DetectionConfiguration.hpp`（`PointCloud` typedef 移到业务自己的头）。
标定 `calibrateWithCylinder` 是设备标定，不是测量算法，移入业务 `src/application/calibration/`。
`MeasurementTypes.hpp` 里的结果类型保留为业务 DTO，去掉与执行相关的字段。

### 3.7 构建与部署

`vcpkg.json` 去掉 PCL、Eigen；新增对 LyFlow 安装产物的依赖（`LYFLOW_ROOT`：`bin/` 下 `lyflow_core.dll` 与全部依赖 DLL、`include/lyflow/`、`library/`）。
`GapInstall.cmake` 用 `install(DIRECTORY ${LYFLOW_ROOT}/bin/ …)` 把 LyFlow 运行时整目录带进 `backend/bin`。
LyFlow 侧提供 `cmake --install` 产出这份布局，并在 `pnpm check` 里验证安装目录能被一个最小 C++ 程序加载并跑通合成图。

## 4. 验证

- LyFlow：并发 run 测试；`flow.fallback` 惰性调度测试（备用闭包未被调度的断言）；导入器对 39 份配置生成的图与 Python 生成器**逐节点等价**；
  `@lyflow/editor` 用 `HttpTransport` 对着一个最小 HTTP 桩跑通现有 e2e 的编辑与运行子集。
- 业务：39 样本 A/B 对模型基线与模板基线 39/39（用切换后的服务，经交互计算 API 跑）；
  **194 台车离线回放**（`scripts/run_replay.ps1` + `tools/check_replay_run.py`）对 `baselines/full-194-KUN10-db0904.json` 逐值一致；
  产线节拍：单点计算延迟不高于切换前（生产日志里的 `kSlowPositionWarnMs` 统计）。
- 切换后重新冻结 194 车基线，`frozen_at_commit` 指向切换 commit。

## 5. 阶段

| 阶段 | 内容 | 仓库 |
|---|---|---|
| A | 2.1–2.8 全部 LyFlow 能力 + 安装布局 | LyFlow |
| B | 3.1–3.7 一次切换：数据库 v5、测量路径、三处调用点、API、UI、删除清单、构建 | xyz-gap-inspector 新分支 |
| C | 39 样本 A/B、194 车回放、节拍统计、基线重冻结、文档 | 两边 |

A 与 B 可以并行到接口冻结为止：先定 `client.hpp`、`lyflow_run_outputs`、`inputs` 注入、`/lyflow/*`、`@lyflow/editor` 的 `Transport` 接口，
B 对着这些接口开工。

## 6. 风险

- **进程内 DLL**：算法崩溃会带走服务进程。现有 sidecar 退避重启覆盖了这一点，但正在计算的那台车会丢结果。
- **`@lyflow/editor` 的双 React 实例**：宿主与包必须共用同一 React（peerDependency），Tauri 壳与业务 UI 的 React 18 版本要对齐。
- **惰性端口与缓存**：备用闭包被 demand 时才编译进计划，`run_started.nodes` 需要允许增量追加事件；前端 stale 逻辑要处理。
- **模板路径迁移**：现场 ProgramData 下的配置目录由 v5 迁移原地改名，迁移必须走 `RecoverableDirectoryTransaction`。
- **不双轨**意味着切换 commit 很大；用 A/B 与 194 车回放做门禁，切换 commit 合并前必须全部通过。
