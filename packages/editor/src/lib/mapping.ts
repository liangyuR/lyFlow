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
/** `data.lazy` 供自定义边组件挂 tooltip 用（见 GraphCanvas 的 LazyEdge）。 */
export type LyEdge = Edge<{ lazy: boolean }>;

export interface Selection {
  nodes: ReadonlySet<string>;
  edges: ReadonlySet<string>;
}

/** 节点的量测尺寸。UI 运行时状态，不进 GraphDoc；但 React Flow / MiniMap 靠它判断节点
 *  有没有尺寸，所以存在画布组件的旁路缓存里、映射时合并进来（见 README「踩过的坑」）。 */
export type MeasuredSizes = ReadonlyMap<string, { width: number; height: number }>;

const EMPTY_SELECTION: Selection = { nodes: new Set(), edges: new Set() };

/** 上一轮的映射结果：按 id 存的对象 + 整批的数组引用。作用是让**没变的节点/连线保持
 *  同一个对象**。
 *
 *  这不是优化，是正确性。React Flow 的 `adoptUserNodes` 只在
 *  `userNode === internals.userNode` 时走快路径；引用一变就重建内部节点，`measured`
 *  只认 `userNode.measured`、`handleBounds` 一并作废，于是整张图重新量测。而量测结果
 *  又经 `onNodesChange` -> 画布的 measured 旁路缓存 -> 这里流回节点对象，构成
 *  「量测 -> setNodes -> 重建 -> 再量测」的自激环，报 Maximum update depth exceeded。
 *  整批都没变时连数组引用一起复用，React Flow 的 StoreUpdater 就直接跳过 setNodes。 */
export interface MappingCache {
  nodes: Map<string, LyNode>;
  edges: Map<string, LyEdge>;
  lastNodes: LyNode[] | null;
  lastEdges: LyEdge[] | null;
}

export function createMappingCache(): MappingCache {
  return { nodes: new Map(), edges: new Map(), lastNodes: null, lastEdges: null };
}

/** GraphDoc -> React Flow。注意方向：这是单向的**派生**，React Flow 的交互结果不会写回
 *  这里产生的对象，而是转成语义化动作打给 graph store（见 GraphCanvas）。
 *
 *  给了 `cache` 就顺带做引用归一：同样的输入拿到同样的对象。归一是幂等的，所以在
 *  `useMemo` 里调用（渲染期写 ref）是安全的 —— StrictMode 的双跑会得到同一个结果。 */
export function toReactFlow(
  doc: GraphDoc,
  ctx: GraphContext,
  selection: Selection = EMPTY_SELECTION,
  measured?: MeasuredSizes,
  anyTypes: AnyTypes = inferAnyTypes(ctx, doc),
  cache?: MappingCache,
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
    // 惰性边（ADR-0016）：目标输入端口 lazy===true，上游闭包不进初始计划，
    // 主路径成功时不跑。画成虚线，配合自定义边类型挂 tooltip（见 GraphCanvas）。
    const lazy = isLazyInput(doc, ctx, e.to.node, e.to.port);
    return {
      id: e.id,
      source: e.from.node,
      sourceHandle: e.from.port,
      target: e.to.node,
      targetHandle: e.to.port,
      selected: selection.edges.has(e.id),
      // 拖离输入端时另一端跟着鼠标走，而不是直接删掉（交互清单 P1 #19）
      reconnectable: "target",
      ...(lazy ? { type: "lazy" as const } : {}),
      // 连线按源端口类型着色 —— 让类型系统「看得见」（交互清单 P0 #8）
      style: lazy
        ? { stroke: color, strokeWidth: 2, strokeDasharray: "6 4" }
        : { stroke: color, strokeWidth: 2 },
      data: { lazy },
    };
  });

  if (!cache) return { nodes, edges };
  cache.lastNodes = keepIdentity(cache.nodes, cache.lastNodes, nodes, sameNode);
  cache.lastEdges = keepIdentity(cache.edges, cache.lastEdges, edges, sameEdge);
  return { nodes: cache.lastNodes, edges: cache.lastEdges };
}

/** 逐个换成缓存里的等价对象；整批都换成了上一轮的原对象时，连数组一起复用。 */
function keepIdentity<T extends { id: string }>(
  byId: Map<string, T>,
  previous: T[] | null,
  next: T[],
  same: (a: T, b: T) => boolean,
): T[] {
  const live = new Set<string>();
  for (let i = 0; i < next.length; i += 1) {
    const fresh = next[i] as T;
    live.add(fresh.id);
    const cached = byId.get(fresh.id);
    if (cached && same(cached, fresh)) next[i] = cached;
    else byId.set(fresh.id, fresh);
  }
  // 删掉的 id 不能留在缓存里：同名节点被重新建出来时会拿到一个过期的对象。
  for (const id of [...byId.keys()]) if (!live.has(id)) byId.delete(id);
  if (previous === null || previous.length !== next.length) return next;
  for (let i = 0; i < next.length; i += 1) if (previous[i] !== next[i]) return next;
  return previous;
}

/** 节点对象的全部字段。漏一个就会漏掉一次真实更新（画布不跟着 doc 走），
 *  多一个只是白重建一次，所以宁可写全。 */
function sameNode(a: LyNode, b: LyNode): boolean {
  return (
    a.id === b.id &&
    a.type === b.type &&
    a.selected === b.selected &&
    a.width === b.width &&
    a.position.x === b.position.x &&
    a.position.y === b.position.y &&
    a.measured?.width === b.measured?.width &&
    a.measured?.height === b.measured?.height &&
    a.data.opId === b.data.opId &&
    a.data.title === b.data.title &&
    a.data.collapsed === b.data.collapsed &&
    a.data.bypass === b.data.bypass &&
    a.data.anyType === b.data.anyType &&
    a.data.subgraphId === b.data.subgraphId &&
    a.data.library === b.data.library
  );
}

/** 连线上 style.stroke 与 lazy（决定 type/dasharray/tooltip）是算出来的，其余都是
 *  常量或直接来自 doc。lazy 只由目标端口的 manifest 声明决定，几乎不会变，但既然
 *  它能改变渲染就必须比进来，否则改了 manifest 之后连线不会跟着重画。 */
function sameEdge(a: LyEdge, b: LyEdge): boolean {
  return (
    a.id === b.id &&
    a.source === b.source &&
    a.sourceHandle === b.sourceHandle &&
    a.target === b.target &&
    a.targetHandle === b.targetHandle &&
    a.selected === b.selected &&
    a.style?.stroke === b.style?.stroke &&
    a.data?.lazy === b.data?.lazy
  );
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

/** 一条边是不是惰性的，只看目标节点的目标输入端口（ADR-0016）：
 *  这个端口的上游闭包不进初始计划，算子 compute 返回 Demand 时才被调度。 */
function isLazyInput(
  doc: GraphDoc,
  ctx: GraphContext,
  nodeId: string,
  portName: string,
): boolean {
  const node = doc.nodes.find((n) => n.id === nodeId);
  const op = node ? ctx.operatorsById.get(node.op) : undefined;
  return findPort(op, portName, "input")?.lazy === true;
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
