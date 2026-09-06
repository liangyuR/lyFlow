// 自动布局（E8）。只在两种时机触发：文档缺 ui.position 时打开即布局，
// 以及用户主动点「整理」。永远不自动覆盖用户摆好的位置。

import dagre from "@dagrejs/dagre";

import type { GraphDoc } from "../types/graph";

/** 没量到真实尺寸时的兜底。节点宽度基本固定，高度按端口数变。 */
const DEFAULT_SIZE = { width: 220, height: 90 };

export interface LayoutOptions {
  /** 只排这些节点，其余原地不动。空/未给 = 整图。 */
  only?: ReadonlySet<string>;
  /** 量测到的真实尺寸，画布组件的旁路缓存。 */
  measured?: ReadonlyMap<string, { width: number; height: number }>;
  /** 子集布局时的落点左上角。整图布局从 (0,0) 开始。 */
  origin?: { x: number; y: number };
}

export interface LayoutMove {
  id: string;
  position: { x: number; y: number };
}

/** 分层布局，LR 方向（数据从左往右流，和 pipeline 的读法一致）。
 *  返回位置变更列表，调用方走 graph store 的 moveNodes 才进撤销栈。 */
export function layoutGraph(doc: GraphDoc, options: LayoutOptions = {}): LayoutMove[] {
  const only = options.only && options.only.size > 0 ? options.only : null;
  const nodes = only ? doc.nodes.filter((n) => only.has(n.id)) : doc.nodes;
  if (nodes.length === 0) return [];

  const g = new dagre.graphlib.Graph();
  g.setGraph({ rankdir: "LR", nodesep: 40, ranksep: 90, marginx: 20, marginy: 20 });
  g.setDefaultEdgeLabel(() => ({}));

  const ids = new Set(nodes.map((n) => n.id));
  for (const n of nodes) {
    const size = options.measured?.get(n.id) ?? DEFAULT_SIZE;
    g.setNode(n.id, { width: size.width, height: size.height });
  }
  for (const e of doc.edges) {
    if (ids.has(e.from.node) && ids.has(e.to.node)) g.setEdge(e.from.node, e.to.node);
  }

  dagre.layout(g);

  // 子集布局落回原来那片区域的左上角，否则「整理选中的三个节点」会把它们扔到原点。
  let dx = options.origin?.x ?? 0;
  let dy = options.origin?.y ?? 0;
  if (only && !options.origin) {
    let minX = Infinity;
    let minY = Infinity;
    for (const n of nodes) {
      minX = Math.min(minX, n.ui?.position?.x ?? 0);
      minY = Math.min(minY, n.ui?.position?.y ?? 0);
    }
    if (Number.isFinite(minX)) {
      dx = minX;
      dy = minY;
    }
  }

  const moves: LayoutMove[] = [];
  for (const n of nodes) {
    const laid = g.node(n.id);
    if (!laid) continue;
    // dagre 给的是中心点，React Flow 要左上角
    moves.push({
      id: n.id,
      position: {
        x: Math.round(laid.x - laid.width / 2) + dx,
        y: Math.round(laid.y - laid.height / 2) + dy,
      },
    });
  }
  return moves;
}

/** 文档里有没有节点缺坐标。脚本生成的图必须能打开（docs/graph-doc.md 的承诺）。 */
export function needsInitialLayout(doc: GraphDoc): boolean {
  return doc.nodes.length > 0 && doc.nodes.some((n) => n.ui?.position == null);
}
