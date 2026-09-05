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

## 状态

早期设计阶段，尚无可运行代码。当前仓库内容是架构约定与接口契约。

## License

尚未确定。在选定之前，本仓库为私有，默认保留所有权利。
