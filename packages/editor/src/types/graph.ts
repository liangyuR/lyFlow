// GraphDoc 的 TypeScript 镜像，契约在 schema/graph-doc.schema.json。
// ADR-0002：这是**唯一数据模型**，React Flow 的 Node/Edge 只是渲染派生产物。

import type { Param } from "./manifest";

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
  /** 静音：透传输入到输出。是**执行语义**不是 UI 状态，所以不在 `ui` 里。
   *  重建节点对象的路径必须带上它，否则存盘就丢（见 README「节点字段的穿透」）。 */
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

// ------------------------------------------------------- 子图（ADR-0010）

/** 子图的一个输入。一条外部边可扇出到多个内部端口。 */
export interface SubInput {
  name: string;
  type: string;
  label?: string;
  doc?: string;
  to: PortRef[];
}

/** 子图的一个输出。只能来自一个内部端口。 */
export interface SubOutput {
  name: string;
  type: string;
  label?: string;
  doc?: string;
  from: PortRef;
}

export interface SubParamBind {
  node: string;
  param: string;
}

/** 提升出来的对外参数（F4）：一份 manifest 的 param 声明加一组绑定。 */
export interface SubParam extends Param {
  binds: SubParamBind[];
}

export interface SubgraphDef {
  name?: string;
  doc?: string;
  category?: string;
  version?: string;
  keywords?: string[];
  nodes: GraphNode[];
  edges: GraphEdge[];
  inputs: SubInput[];
  outputs: SubOutput[];
  params: SubParam[];
}

/** `sub:` 前缀标记一个引用图内子图定义的节点。 */
export const SUBGRAPH_OP_PREFIX = "sub:";
/** 库算子的 id 前缀，由 C++ 扫描库目录时注册。 */
export const LIBRARY_OP_PREFIX = "lib.";

export function subgraphIdOf(opId: string): string | null {
  return opId.startsWith(SUBGRAPH_OP_PREFIX) ? opId.slice(SUBGRAPH_OP_PREFIX.length) : null;
}

export interface GraphDoc {
  schemaVersion: number;
  id: string;
  name?: string;
  meta?: Record<string, unknown>;
  nodes: GraphNode[];
  edges: GraphEdge[];
  groups?: unknown[];
  subgraphs?: Record<string, SubgraphDef>;
  /** 图级命名输出（ADR-0017）。宿主按名字取值，不认节点 id；
   *  子图内部的端口写展开后的路径 id（`outer/inner`）。 */
  outputs?: Record<string, GraphOutput>;
  /** 未知字段容器：老客户端打开新版本写的图时不丢数据。 */
  x?: Record<string, unknown>;
}

export interface GraphOutput {
  node: string;
  port: string;
  label?: string;
}

/** 图里的一个层级：顶层是 doc 本身，进了子图就是那份定义。 */
export interface GraphLevel {
  nodes: GraphNode[];
  edges: GraphEdge[];
}
