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
| [docs/adr/](docs/adr/) | 架构决策记录 |
| [schema/](schema/) | GraphDoc / OperatorManifest / ExecutionEvent 的 JSON Schema + 已校验的示例 |

## 技术选型

前端用 **React Flow**，不用 Rete。理由：Rete 的核心卖点是自带 dataflow 执行引擎，
而 LyFlow 的执行调度必须在 C++ 侧（只有那里知道点云规模、中间结果能否复用、显存是否够、能否并行），
前端不需要执行引擎。剩下的 Control / Socket 类型能力用 React Flow 的 custom node + `isValidConnection` 实现更直白。
完整论证见 [ADR-0001](docs/adr/0001-react-flow-over-rete.md)。

## 目录

```
core/     C++ 核心：算子注册表 + manifest 导出。M0 只有描述，计算实体是 M2
bridge/   Rust 桥接层：Tauri 壳，FFI 链入 core，IPC / 文件读写
app/      前端：Vite + React + TS。M0 是节点面板，画布是 M1
schema/   三份 JSON Schema —— 跨语言契约的真实来源
scripts/  构建与门禁脚本
```

## 快速开始

需要 Visual Studio（含 C++ 工具集）、Rust、Node + pnpm。CMake 用 VS 自带的即可。

```bash
pnpm install
pnpm dev          # tauri dev：编 C++ -> 编 Rust -> 起 vite -> 开窗口
```

只想调界面、不想启动整个 app：

```bash
pnpm core:dump    # 把 manifest dump 到 app/public/manifest.dev.json
pnpm app:dev      # 浏览器模式，状态栏会标「静态快照」提示数据可能过期
```

一条命令验完整条链路：

```bash
pnpm check
```

## 加一个算子

1. 新建 `core/src/ops/<category>_<name>.cpp`
2. 在 `core/src/ops/ops.h` 加声明
3. 在 `core/src/builtin_ops.cpp` 的 `registerBuiltinOps` 加一行调用

重启后它就出现在节点面板里，连同它的分类、端口颜色、参数控件和取值范围。
**前端不用改任何东西**（[ADR-0003](docs/adr/0003-manifest-from-cpp.md)）。

> `tauri dev` 只 watch `bridge/`，改了 `core/` 的 C++ 需要重启才生效。
> 算子热重载是 M3 的事。

## 状态

M0 完成 —— 三层契约打通，「C++ 加算子 → 前端自动出现」这条链路可跑。
还不能连线、不能编辑参数、不能执行；那是 M1 和 M2。见 [roadmap](docs/roadmap.md)。

## License

尚未确定。在选定之前，本仓库为私有，默认保留所有权利。
