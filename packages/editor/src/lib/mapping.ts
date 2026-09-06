// GraphDoc ↔ React Flow 的映射层（ADR-0002 的落地点）。**整个前端只有这里
// （加上直接渲染画布的组件）允许出现 React Flow 类型**，换库时改的就是这一个文件。

import type { Edge, Node } from "@xyflow/react";

import { subgraphIdOf, type GraphDoc } from "../types/graph";
import type { AnyTypes, GraphContext } from "./typecheck";
import { ANY, findPort, inferAnyTypes } from "./typecheck";

/** 节点上只放 op id 和 UI 状态。算子描述由节点组件按 op 现查 —— 热重载的前提。 */
export interface OperatorNodeData extends Record<string, unknown> {
  opId: string;
  /** null 表示用 manifest 的 label */
  title: string | null;
  collapsed: boolean;
  /** 静音（E5）。执行语义，来自 doc 而不是 UI 状态。 */
  bypass: boolean;
  /** 该节点 Any 端口推导出的实际类型，null = 推不出来（E6）。 */
  anyType: string | null;
  /** 引用的子图 id，null = 普通算子。双击进入靠它（F2）。 */
  subgraphId: string | null;
  /** 库算子（`lib.`）。可以「展开为内联子图」，但不能直接进去编辑。 */
  library: boolean;
}

export type LyNode = Node<OperatorNodeData, "operator">;
export type LyEdge = Edge;

export interface Selection {
  nodes: ReadonlySet<string>;
  edges: ReadonlySet<string>;
}

/** 节点的量测尺寸。UI 运行时状态，不进 GraphDoc；但 React Flow / MiniMap 靠它判断节点
 *  有没有尺寸，所以存在画布组件的旁路缓存里、映射时合并进来（见 README「踩过的坑」）。 */
export type MeasuredSizes = ReadonlyMap<string, { width: number; height: number }>;

const EMPTY_SELECTION: Selection = { nodes: new Set(), edges: new Set() };

/** GraphDoc -> React Flow。注意方向：这是单向的**派生**，React Flow 的交互结果不会写回
 *  这里产生的对象，而是转成语义化动作打给 graph store（见 GraphCanvas）。 */
export function toReactFlow(
  doc: GraphDoc,
  ctx: GraphContext,
  selection: Selection = EMPTY_SELECTION,
  measured?: MeasuredSizes,
  anyTypes: AnyTypes = inferAnyTypes(ctx, doc),
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
        bypass: n.bypass === true,
        anyType: anyTypes.get(n.id) ?? null,
        subgraphId: subgraphIdOf(n.op),
        library: n.op.startsWith("lib."),
      },
      ...(size ? { measured: size } : {}),
      ...(n.ui?.width != null ? { width: n.ui.width } : {}),
    };
  });

  const edges: LyEdge[] = doc.edges.map((e) => {
    const color = edgeColor(doc, ctx, e.from.node, e.from.port, anyTypes);
    return {
      id: e.id,
      source: e.from.node,
      sourceHandle: e.from.port,
      target: e.to.node,
      targetHandle: e.to.port,
      selected: selection.edges.has(e.id),
      // 拖离输入端时另一端跟着鼠标走，而不是直接删掉（交互清单 P1 #19）
      reconnectable: "target",
      // 连线按源端口类型着色 —— 让类型系统「看得见」（交互清单 P0 #8）
      style: { stroke: color, strokeWidth: 2 },
    };
  });

  return { nodes, edges };
}

/** 连线颜色取自源端口的**实际**类型：串了 reroute 之后颜色也要跟着源变（E6）。 */
function edgeColor(
  doc: GraphDoc,
  ctx: GraphContext,
  nodeId: string,
  portName: string,
  anyTypes: AnyTypes,
): string {
  const node = doc.nodes.find((n) => n.id === nodeId);
  const op = node ? ctx.operatorsById.get(node.op) : undefined;
  const port = findPort(op, portName, "output");
  if (!port) return "#6b7280";
  const type = port.type === ANY ? (anyTypes.get(nodeId) ?? ANY) : port.type;
  return ctx.typesByName.get(type)?.color ?? "#6b7280";
}

/** 点到线段的距离。拖节点到连线上要用（交互清单 P1 #21）。 */
export function distanceToSegment(
  point: { x: number; y: number },
  a: { x: number; y: number },
  b: { x: number; y: number },
): number {
  const dx = b.x - a.x;
  const dy = b.y - a.y;
  const lengthSq = dx * dx + dy * dy;
  if (lengthSq === 0) return Math.hypot(point.x - a.x, point.y - a.y);
  let t = ((point.x - a.x) * dx + (point.y - a.y) * dy) / lengthSq;
  t = Math.max(0, Math.min(1, t));
  return Math.hypot(point.x - (a.x + t * dx), point.y - (a.y + t * dy));
}

export interface Rect {
  x: number;
  y: number;
  w: number;
  h: number;
}

/** 线段有没有穿过这个矩形。「把节点拖到线上」这个手势的字面意思就是它 ——
 *  只判中心距离的话，画布缩到一半时容差就只剩几个屏幕像素，够不着。 */
export function segmentHitsRect(a: { x: number; y: number }, b: { x: number; y: number }, r: Rect): boolean {
  const inside = (p: { x: number; y: number }) =>
    p.x >= r.x && p.x <= r.x + r.w && p.y >= r.y && p.y <= r.y + r.h;
  const steps = 32;
  for (let i = 0; i <= steps; i += 1) {
    const t = i / steps;
    if (inside({ x: a.x + (b.x - a.x) * t, y: a.y + (b.y - a.y) * t })) return true;
  }
  return false;
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

/** 从 React Flow 的位置变更里提取语义化的移动。只关心 position，其余
 *  （dimensions/select/dragging）都是 UI 运行时状态，不该进 GraphDoc（ADR-0002）。 */
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
