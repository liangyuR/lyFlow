# LyFlow

通用视觉处理流程编辑器（Vision Flow）。用节点图的方式编排点云 / 图像算子，图在前端编辑，计算在 C++ 里跑。

## 这个项目要解决什么

视觉算法调参和流程编排目前靠改代码、重编译、重跑。LyFlow 把这个循环变成：拖节点 → 改参数 → 看结果，
同时保证**算子实现和调度仍然完全留在 C++ 侧**，前端不承担任何计算职责。

## 分层

```
┌─────────────────────────────────────────────┐
│  前端 (React + TypeScript + React Flow)     │
│  · 图编辑：拖拽 / 连线 / 类型校验 / 撤销重做  │
│  · 参数表单：由 OperatorManifest 动态生成     │
│  · 状态可视化：pending / running / done / err │
│  ✗ 不做任何执行调度                          │
└──────────────┬──────────────────────────────┘
               │  GraphDoc (下行) / ExecutionEvent (上行)
               │  OperatorManifest (启动时下行)
┌──────────────┴──────────────────────────────┐
│  Rust 桥接层                                 │
│  · IPC / 序列化 / 文件读写 / 事件推送         │
│  · 不理解算子语义，只做转发与生命周期管理      │
└──────────────┬──────────────────────────────┘
               │  FFI
┌──────────────┴──────────────────────────────┐
│  C++ 核心                                    │
│  · 算子实现 + 算子注册表（导出 Manifest）      │
│  · 执行引擎：拓扑调度 / 中间结果复用 / 显存管理 │
│  · 编成 DLL，只导出 C ABI；Rust 运行时加载     │
└─────────────────────────────────────────────┘
```

## 两条已经定下来的约定

**1. GraphDoc 是唯一数据模型，不用 React Flow 的 `Node` / `Edge` 当数据模型。**
React Flow 的类型里混了 `selected` / `dragging` / `measured` 这类 UI 运行时状态，不该进文件；
后端只关心拓扑和参数，不关心坐标；换库或大版本升级时只改映射层。
渲染前做一次 `GraphDoc → ReactFlowState` 的映射，编辑后反向写回。详见 [ADR-0002](docs/adr/0002-graphdoc-as-source-of-truth.md)。

**2. 算子描述由 C++ 侧导出，前端零硬编码。**
算子的名称、分类、输入输出端口及类型、参数定义（类型 / 范围 / 默认值 / 分组）全部来自
C++ 生成的 `OperatorManifest`，Rust 转发给前端。**加新算子只改 C++，前端不用动。**
详见 [ADR-0003](docs/adr/0003-manifest-from-cpp.md)。

## 文档

| 文档 | 内容 |
|---|---|
| [docs/architecture.md](docs/architecture.md) | 三层职责边界、数据流、执行状态机 |
| [docs/graph-doc.md](docs/graph-doc.md) | GraphDoc 数据模型与序列化格式 |
| [docs/operator-manifest.md](docs/operator-manifest.md) | 算子描述格式、参数控件映射、端口类型系统 |
| [docs/interaction-checklist.md](docs/interaction-checklist.md) | 交互清单（P0/P1/P2），对照 ComfyUI 与 Blender Geometry Nodes |
| [docs/roadmap.md](docs/roadmap.md) | 里程碑与工作量估计 |
| [docs/m2-plan.md](docs/m2-plan.md) | M2 实施计划：定死的决定、C ABI v2、执行器、结果仓 |
| [docs/m2-acceptance.md](docs/m2-acceptance.md) | M2 逐条验收记录（含未验证项与偏离决策） |
| [docs/m3-plan.md](docs/m3-plan.md) | M3 实施计划：缓存与并行、bypass/reroute、迁移、热重载、P1 交互 |
| [docs/m3-acceptance.md](docs/m3-acceptance.md) | M3 逐条验收记录（含未验证项与偏离决策） |
| [docs/m4-plan.md](docs/m4-plan.md) | M4 实施计划：子图、live preview、headless CLI、大图性能 |
| [docs/m4-acceptance.md](docs/m4-acceptance.md) | M4 逐条验收记录（含未验证项与偏离决策） |
| [docs/agent-tuning.md](docs/agent-tuning.md) | 只拿得到 CLI/MCP 时，给一组测点调稳参数的工作法 |
| [docs/mcp.md](docs/mcp.md) | MCP 服务：怎么起、工具一览、`.mcp.json` 片段、明确不做的事 |
| [docs/adr/](docs/adr/) | 架构决策记录 |
| [schema/](schema/) | GraphDoc / OperatorManifest / ExecutionEvent 的 JSON Schema + 已校验的示例 |

## 技术选型

前端用 **React Flow**，不用 Rete。理由：Rete 的核心卖点是自带 dataflow 执行引擎，
而 LyFlow 的执行调度必须在 C++ 侧（只有那里知道点云规模、中间结果能否复用、显存是否够、能否并行），
前端不需要执行引擎。剩下的 Control / Socket 类型能力用 React Flow 的 custom node + `isValidConnection` 实现更直白。
完整论证见 [ADR-0001](docs/adr/0001-react-flow-over-rete.md)。

## 目录

```
core/     C++ 核心：算子注册表 + manifest 导出 + 校验/展开/编译/执行 + 结果仓。编成 DLL
bridge/   Rust 桥接层：Tauri 壳（lyflow-app）与 headless CLI（lyflow），共用 core_ffi
packages/editor/  @lyflow/editor：节点编辑器 + 3D 预览，一个可嵌进任意 React 宿主的组件
packages/mcp/     @lyflow/mcp：给 Agent 用的 MCP 服务（stdio），消费同一份 HTTP 契约 + 本地 CLI
app/      Tauri 壳：入口、文件对话框、窗口标题、验收窗口桥
examples/host-react/  最小 Vite + React 宿主，经 HttpTransport 连后端
schema/   三份 JSON Schema —— 跨语言契约的真实来源
scripts/  构建与门禁脚本；scripts/e2e 是 CDP 验收
```

## 快速开始

需要 Visual Studio（含 C++ 工具集）、Rust、Node + pnpm，以及 **vcpkg 装的 PCL**。
CMake 与 Ninja 用 VS 自带的即可。

```powershell
vcpkg install pcl:x64-windows
```

约定 vcpkg 装在 `C:/vcpkg`，`VCPKG_ROOT` 可以覆盖。

```bash
pnpm install
pnpm dev          # core-watch + tauri dev：编 C++ -> 编 Rust -> 起 vite -> 开窗口
```

`pnpm dev` 会同时起 [`scripts/core-watch.ps1`](scripts/core-watch.ps1)：改了 `core/`
的 C++ 存盘即增量构建，app 那边**热重载**新的 core，画布上的图与撤销栈原样保留
（[ADR-0009](docs/adr/0009-hot-reload-by-copy.md)）。不想要它就 `pnpm dev --no-core-watch`，
只要监视不要 app 就 `pnpm core:watch`。

只想调界面、不想启动整个 app：

```bash
pnpm core:dump    # 把 manifest dump 到 app/public/manifest.dev.json
pnpm app:dev      # 浏览器模式，状态栏会标「静态快照」提示数据可能过期
```

一条命令验完整条链路：

```bash
pnpm check         # C++ 编译 + 自检 + core 测试 → schema 校验 → cargo test → CLI → 前端 build → MCP
pnpm e2e           # CDP 驱动真实 app 的端到端验收（自己起 tauri dev，跑完自己收尾）
pnpm e2e:http      # 另一条线：Node 桩服务器 + 系统 Chrome + examples/host-react
pnpm e2e:packaged  # 同一套断言，但跑的是 tauri build 的产物在一个干净目录里的拷贝
```

## 命令行（headless）

`lyflow` 是同一个 crate 的第二个 bin，**不依赖 Tauri**，与桌面共用同一份加载、校验、
迁移和执行代码（[ADR-0012](docs/adr/0012-headless-cli.md)）。构建：

```bash
pnpm cli:build     # cargo build --release --bin lyflow --no-default-features
# 产物：bridge/target/release/lyflow.exe，与 lyflow_core.dll 同目录
```

stdout 是 **JSON Lines**（`run` 输出的就是 ExecutionEvent 原样，一行一条），
stderr 给人看。退出码：`0` 成功、`1` 校验失败、`2` 执行失败、`3` 被 Ctrl+C 取消、`4` 参数错。

```bash
lyflow run      graph.lyflow.json [--to nodeId]... [--set nodeId.param=<json>]...
                                  [--base-dir d] [--parallel n] [--no-cache]
                                  [--preview] [--preview-points n]
lyflow validate graph.lyflow.json
lyflow plan     graph.lyflow.json [--to nodeId]...
lyflow params   graph.lyflow.json [--node nodeId]... [--only explicit|default|bound]
                                  [--set nodeId.param=<json>]... [--json]
lyflow migrate  graph.lyflow.json [--write]
lyflow manifest [--check]
lyflow dump     graph.lyflow.json nodeId:port out.pcd [--format binary|ascii|binary_compressed]
lyflow sweep    graph.lyflow.json --param nodeId.param=start:end:steps [--param ...]
                                  --metric nodeId:port.elementCount [--csv out.csv]
lyflow eval     graph.lyflow.json <样本集>
                                  [--params sets.json] [--param n.p=start:end:steps]...
                                  --metric <值路径> [--metric ...] [--holdout tag=value]
                                  [--group-by tag] [--csv out.csv]
lyflow perturb  graph.lyflow.json --after nodeId:port --region <选区 JSON>
                                  --axis x|y|z=start:end:steps --metric <值路径>
                                  <样本集> [--expect slope] [--tolerance v]
lyflow diff     a.lyflow.json b.lyflow.json [--json]

<样本集> 三选一，eval 与 perturb 共用：
  --samples samples.jsonl
  --samples-glob pat --bind n.p
  --samples-dir root --bind-pair n.pA,n.pB --pattern globA,globB
                [--sample-subdir name] [--sort-by name|mtime] [--split-half tagKey]
```

`eval` 与 `perturb` 的 `--metric` 是**值路径**：`outputs.gap`、`nodes.n_fit.quality.rmsResidualMm`、
`nodes.v.elementCount`、`run.durationMs`。路径拼错时 stderr 会列出这张图上所有可用的标量路径
（[ADR-0020](docs/adr/0020-eval-and-perturb-as-cli.md)）。
样本集的目录模式（`--samples-dir`）把「双相机配对 + 按时间前后各半打 tag」也内建了，
不用再为每个测点写生成脚本。用法见 [docs/agent-tuning.md](docs/agent-tuning.md)。

几个例子：

```bash
# 跑一张图，用 jq 挑出最终点数
lyflow run demo.lyflow.json | jq -r 'select(.kind=="node_state" and .state=="done")
                                     | "\(.nodeId) \(.stats.elementCount)"'

# 覆盖一个参数再跑（路径与界面里参数右键的「复制路径名」一致）
lyflow run demo.lyflow.json --set n_voxel.leafSize='[0.02,0.02,0.02]'

# 这张图相对默认值改了哪些参数（GraphDoc 是稀疏存储，join 由 core 做）
lyflow params demo.lyflow.json --only explicit --json \
              | jq -r '"\(.node).\(.param)=\(.value)"'

# 扫 5 组 leafSize，源头只加载一次（其余 4 次是 skipped）
lyflow sweep demo.lyflow.json --param n_voxel.minPointsPerVoxel=1:5:5 \
             --metric n_voxel:cloud.elementCount --csv sweep.csv

# 一组样本 × 一组参数 → 指标 → 内建统计，按时间前后各半留出
# 样本集直接指采集目录：一帧一个子目录，两个 glob 配成双相机一帧
lyflow eval demo.lyflow.json --samples-dir kun10/sensor --sample-subdir 4 \
            --bind-pair n_load.primaryFile,n_load.secondaryFile \
            --pattern "*Master*.pcd,*Slave*.pcd" --split-half half \
            --param n_fit.distThresh=0.2:0.8:4 --metric outputs.gap --holdout half=b

# 合成位移：在源头之后插一个 edit.translate_region，看读数跟不跟得上
lyflow perturb demo.lyflow.json --after n_frame_s:cloud \
            --region '{"kind":"halfspace","point":[0.0134,0,0],"normal":[1,0,0]}' \
            --axis x=0:0.0006:5 --samples frames.jsonl --metric outputs.gap --expect 1000

# 只移动了节点位置的两份图，diff 输出为空（ui 不算）
lyflow diff before.lyflow.json after.lyflow.json
```

## 给 Agent 用（MCP）

`@lyflow/mcp`（[packages/mcp/](packages/mcp/)）是一个 stdio 的 MCP 服务：
**描述、校验、执行走 [HTTP 契约](docs/http-transport.md)，`eval` / `perturb` / `diff` 起本地 `lyflow`**。
它是那份契约的又一个消费方，不是第四种传输（[ADR-0021](docs/adr/0021-mcp-as-transport-consumer.md)），
所以同一个二进制既能接仓库里的桩服务器，也能接阶段 B 的业务服务 —— 换后端只改一个环境变量。

13 个工具：`list_operators` / `get_operator` / `list_port_types` / `validate_graph` /
`plan_graph` / `run_graph` / `get_node_outputs` / `summarize_output` / `eval` / `perturb` /
`diff_graphs` / `get_params` / `patch_graph`。`run_graph` 现在以 run summary 为主体返回
（[ADR-0022](docs/adr/0022-run-summary-as-core-output.md)）；`patch_graph` 对应 CLI
`lyflow patch`，`dryRun` 默认 true（[ADR-0023](docs/adr/0023-patch-as-idempotent-structural-edit.md)）；
`get_params` 对应 `lyflow params`，回的是每节点每参数的生效值与来源。
输出一律裁过（Agent 每次调用都在花上下文）：点云只给点数、包围盒、每通道 min/max/mean 与前几个点，
`eval` 的统计默认压成一行一组（`compact`）、`eval_row` 与 `perturb_sample` 落盘给路径。
起法、`.mcp.json` 片段与每个工具的返回形状见 [docs/mcp.md](docs/mcp.md)，
CLI 选项与 MCP 字段的逐条对照见 [docs/agent-tuning.md](docs/agent-tuning.md) §7。

## 库算子（可复用的子图）

选中若干节点 **Ctrl+G** 合成子图，右键「保存到库」写成一个
`<id>.lyflow-op.json`，它就变成一个普通算子 `lib.<id>`，出现在面板的 `Library/` 分类下
（[ADR-0010](docs/adr/0010-subgraph-by-expansion.md)）。库目录是：

```
%APPDATA%\com.lyflow.app\library\        # 桌面与 CLI 共用同一个位置
```

额外目录可以用环境变量 `LYFLOW_LIBRARY_DIRS`（分号分隔）加。改动库文件不用重启：
app 盯着这个目录，工具栏的「库」按钮也能手动重扫。

## 加一个算子

1. 新建 `core/src/ops/<category>_<name>.cpp`
2. 在 `core/src/ops/ops.h` 加声明
3. 在 `core/src/builtin_ops.cpp` 的 `registerBuiltinOps` 加一行调用

存盘之后几秒内它就出现在节点面板里，连同它的分类、端口颜色、参数控件和取值范围。
**前端不用改任何东西**（[ADR-0003](docs/adr/0003-manifest-from-cpp.md)），
**也不用重启**（`pnpm dev` 下的热重载）。

> 热重载会清空结果缓存并取消正在跑的 run —— 缓存里的对象属于旧那份 DLL，
> 跨代持有它是未定义行为（[ADR-0009](docs/adr/0009-hot-reload-by-copy.md)）。
> 新算子的 C++ 编不过时旧的一代继续服役，界面上会弹一条失败提示。

## 状态

**M4 完成 —— 能扩展。** 图本身成了可复用、可脚本化、可交互探索的资产：

- **子图 / 库算子。** Ctrl+G 合成、双击进入、参数提升、保存成库文件。
  子图在 C++ 的 compile 之前展开成平图，执行器与缓存对它一无所知
  （[ADR-0010](docs/adr/0010-subgraph-by-expansion.md)）。
- **live preview。** 拖参数时发一次抽稀过的 run，3D 视图跟手；松手补一次正式运行。
  预览结果进独立的缓存命名空间，绝不会被当成正式结果
  （[ADR-0011](docs/adr/0011-preview-as-decimated-run.md)）。
- **headless CLI。** `lyflow run/validate/plan/migrate/manifest/dump/sweep/eval/perturb/diff`，
  JSON Lines 事件流，与桌面同一条代码路径（[ADR-0012](docs/adr/0012-headless-cli.md)）。
- **大图能用。** 300 节点的图打开 < 1 s，拖动 ≥ 30 fps；执行事件按 16 ms 合并。

**M3 —— 能用。** 在 M2「能跑」之上，把它变成一个愿意天天开着的工具：

- **不重复算。** 结果按内容寻址缓存，原图重跑全部 `skipped`（几十毫秒），
  改一个参数只重算它和它的下游；哪几个节点会重算，工具栏和虚线框直接告诉你
  （判定全在 C++，[ADR-0007](docs/adr/0007-cache-authority.md)）。
- **并行执行。** 依赖计数驱动的线程池，默认 `min(4, 核数)`，没有层同步屏障。
- **调试顺手。** Ctrl+M 静音一个节点让它透传、reroute 整理连线、Shift+F5 只跑到选中节点、
  底部抽屉里有日志/诊断/缓存统计。
- **老图打得开。** 算子升主版本要配迁移，打开老图时以一条可撤销的动作写回
  （[ADR-0008](docs/adr/0008-migration-as-diagnostic.md)）。
- **改 C++ 不用重启。** `pnpm dev` 下热重载（[ADR-0009](docs/adr/0009-hot-reload-by-copy.md)）。
- **交互清单 P0 + P1 全部完成**（[interaction-checklist](docs/interaction-checklist.md)）。

16 个算子覆盖一条真实 pipeline：`load_pcd → crop_box → voxel_grid →
statistical_outlier → ransac_plane → extract_indices → save_pcd`。

下一步是 M5「外延」：第二种数据域 Image、第三方算子插件 DLL。见 [roadmap](docs/roadmap.md)。

前端不写单元测试，验证方式是通过 CDP 驱动真实运行的 app
（[scripts/e2e](scripts/e2e/)）。逐条验收记录见
[docs/m4-acceptance.md](docs/m4-acceptance.md)。

## License

尚未确定。在选定之前，本仓库为私有，默认保留所有权利。
