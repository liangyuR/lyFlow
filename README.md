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
| [docs/adr/](docs/adr/) | 架构决策记录 |
| [schema/](schema/) | GraphDoc / OperatorManifest / ExecutionEvent 的 JSON Schema + 已校验的示例 |

## 技术选型

前端用 **React Flow**，不用 Rete。理由：Rete 的核心卖点是自带 dataflow 执行引擎，
而 LyFlow 的执行调度必须在 C++ 侧（只有那里知道点云规模、中间结果能否复用、显存是否够、能否并行），
前端不需要执行引擎。剩下的 Control / Socket 类型能力用 React Flow 的 custom node + `isValidConnection` 实现更直白。
完整论证见 [ADR-0001](docs/adr/0001-react-flow-over-rete.md)。

## 目录

```
core/     C++ 核心：算子注册表 + manifest 导出 + 校验/编译/执行 + 结果仓。编成 DLL
bridge/   Rust 桥接层：Tauri 壳，运行时加载 core DLL，IPC / 事件推流 / 文件读写
app/      前端：Vite + React + TS + React Flow + three.js。节点编辑器 + 3D 预览
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
pnpm check         # C++ 编译 + 自检 + core 测试 → schema 校验 → cargo test → 前端 build
pnpm e2e           # CDP 驱动真实 app 的端到端验收（自己起 tauri dev，跑完自己收尾）
pnpm e2e:packaged  # 同一套断言，但跑的是 tauri build 的产物在一个干净目录里的拷贝
```

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

**M3 完成 —— 能用。** 在 M2「能跑」之上，把它变成一个愿意天天开着的工具：

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

下一步是 M4「能扩展」：子图、live preview、CLI。见 [roadmap](docs/roadmap.md)。

前端不写单元测试，验证方式是通过 CDP 驱动真实运行的 app
（[scripts/e2e](scripts/e2e/)：`pnpm e2e` 188 项断言）。
逐条验收记录见 [docs/m3-acceptance.md](docs/m3-acceptance.md)。

## License

尚未确定。在选定之前，本仓库为私有，默认保留所有权利。
