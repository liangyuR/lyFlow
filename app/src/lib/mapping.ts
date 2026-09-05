//
// GraphDoc ↔ React Flow 的映射层。
//
// ADR-0002 的落地点。**这是整个前端唯一允许出现 React Flow 类型的地方**
// （加上直接渲染画布的组件）。其余代码只认 GraphDoc。
//
// 换 React Flow 大版本或者换库时，改的是这一个文件。
//

import type { Edge, Node } from "@xyflow/react";

import type { GraphDoc } from "../types/graph";
import type { GraphContext } from "./typecheck";
import { findPort } from "./typecheck";

/** 节点上只放 op id 和 UI 状态。算子描述由节点组件按 op 现查 —— 热重载的前提。 */
export interface OperatorNodeData extends Record<string, unknown> {
  opId: string;
  /** null 表示用 manifest 的 label */
  title: string | null;
  collapsed: boolean;
}

export type LyNode = Node<OperatorNodeData, "operator">;
export type LyEdge = Edge;

export interface Selection {
  nodes: ReadonlySet<string>;
  edges: ReadonlySet<string>;
}

/**
 * 节点的量测尺寸。
 *
 * 这是 ADR-0002 的一个必要副产品：量测尺寸是 UI 运行时状态，绝不能进 GraphDoc
 * （后端不关心，也不该进文件）。但 React Flow 判断一个节点「有没有尺寸」看的是
 * 我们传进去的那个对象，而尺寸只通过 onNodesChange 的 dimensions 事件回传。
 * 我们把那些事件丢掉了，于是 MiniMap 认为所有节点都没有尺寸、一个都不画。
 *
 * 所以尺寸要有个去处：存在画布组件的旁路缓存里，映射时合并进来。
 * GraphDoc 依然干净。
 */
export type MeasuredSizes = ReadonlyMap<string, { width: number; height: number }>;

const EMPTY_SELECTION: Selection = { nodes: new Set(), edges: new Set() };

/**
 * GraphDoc -> React Flow。
 *
 * 注意方向：这是单向的**派生**。React Flow 的交互结果不会直接写回这里产生的
 * 对象，而是转成语义化动作打给 graph store（见 GraphCanvas）。
 */
export function toReactFlow(
  doc: GraphDoc,
  ctx: GraphContext,
  selection: Selection = EMPTY_SELECTION,
  measured?: MeasuredSizes,
): { nodes: LyNode[]; edges: LyEdge[] } {
  const nodes: LyNode[] = doc.nodes.map((n) => {
    const size = measured?.get(n.id);
    return {
      id: n.id,
      type: "operator" as const,
      position: n.ui?.position ?? { x: 0, y: 0 },
      selected: selection.nodes.has(n.id),
      data: {
        opId: n.op,
        title: n.ui?.title ?? null,
        collapsed: n.ui?.collapsed ?? false,
      },
      ...(size ? { measured: size } : {}),
      ...(n.ui?.width != null ? { width: n.ui.width } : {}),
    };
  });

  const edges: LyEdge[] = doc.edges.map((e) => {
    const color = edgeColor(doc, ctx, e.from.node, e.from.port);
    return {
      id: e.id,
      source: e.from.node,
      sourceHandle: e.from.port,
      target: e.to.node,
      targetHandle: e.to.port,
      selected: selection.edges.has(e.id),
      // 连线按源端口类型着色 —— 让类型系统「看得见」（交互清单 P0 #8）
      style: { stroke: color, strokeWidth: 2 },
    };
  });

  return { nodes, edges };
}

/** 连线颜色取自源端口的类型。查不到时给灰色，不要静默用默认色掩盖问题。 */
function edgeColor(
  doc: GraphDoc,
  ctx: GraphContext,
  nodeId: string,
  portName: string,
): string {
  const node = doc.nodes.find((n) => n.id === nodeId);
  const op = node ? ctx.operatorsById.get(node.op) : undefined;
  const port = findPort(op, portName, "output");
  if (!port) return "#6b7280";
  return ctx.typesByName.get(port.type)?.color ?? "#6b7280";
}

/** 从 React Flow 的变更里提取量测尺寸。 */
export function extractSizes(
  changes: readonly { type: string; id: string; dimensions?: { width: number; height: number } }[],
): { id: string; width: number; height: number }[] {
  const out: { id: string; width: number; height: number }[] = [];
  for (const c of changes) {
    if (c.type === "dimensions" && c.dimensions) {
      out.push({ id: c.id, width: c.dimensions.width, height: c.dimensions.height });
    }
  }
  return out;
}

/**
 * 从 React Flow 的位置变更里提取语义化的移动。
 * 只关心 position，其余（dimensions/select/dragging）都是 UI 运行时状态，
 * 不该进 GraphDoc（ADR-0002）。
 */
export function extractMoves(
  changes: readonly { type: string; id: string; position?: { x: number; y: number } }[],
): { id: string; position: { x: number; y: number } }[] {
  const moves: { id: string; position: { x: number; y: number } }[] = [];
  for (const c of changes) {
    if (c.type === "position" && c.position) {
      moves.push({ id: c.id, position: c.position });
    }
  }
  return moves;
}
