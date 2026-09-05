//
// GraphDoc 的 TypeScript 镜像。见 schema/graph-doc.schema.json。
//
// ADR-0002：这是**唯一数据模型**。React Flow 的 Node/Edge 只是渲染派生产物，
// 到 M1 引入画布时也不会反过来变成真实来源。
//

export const GRAPH_SCHEMA_VERSION = 1;

export interface NodeUi {
  position?: { x: number; y: number };
  collapsed?: boolean;
  /** null = 用 manifest 的 label */
  title?: string | null;
  color?: string | null;
  /** null = 自适应 */
  width?: number | null;
}

export interface GraphNode {
  id: string;
  op: string;
  opVersion?: string;
  /**
   * 静音：透传输入到输出。这是**执行语义**不是 UI 状态，所以不在 `ui` 里。
   * M2 只是让它在三层之间原样穿过（schema / Rust / C++ 都已接住），
   * 执行语义 M3 实现。前端必须带着它 —— 任何重建节点对象而不是就地改的路径
   * （复制粘贴、迁移写回）漏掉它，用户存盘时静音状态就没了。
   */
  bypass?: boolean;
  /** 稀疏：只存与 manifest 默认值不同的项。 */
  params?: Record<string, unknown>;
  ui?: NodeUi;
}

export interface PortRef {
  node: string;
  port: string;
}

export interface GraphEdge {
  id: string;
  from: PortRef;
  to: PortRef;
}

export interface GraphDoc {
  schemaVersion: number;
  id: string;
  name?: string;
  meta?: Record<string, unknown>;
  nodes: GraphNode[];
  edges: GraphEdge[];
  groups?: unknown[];
  subgraphs?: Record<string, unknown>;
  /** 未知字段容器：老客户端打开新版本写的图时不丢数据。 */
  x?: Record<string, unknown>;
}
