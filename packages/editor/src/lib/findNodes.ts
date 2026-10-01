// 查找节点（Ctrl+F）：整张图里的节点连子图里面的一起列出来（路径 id，ADR-0010），按名字模糊找，
// 选中后打开到它所在的那一层（revealNode）。库算子的内部只读、进不去，不列。

import { fuzzyMatchAny } from "./fuzzy";
import { fullId, nodeTitle, type PathSegment } from "./subgraph";
import { subgraphIdOf, type GraphDoc, type GraphLevel } from "../types/graph";
import type { OperatorDesc } from "../types/manifest";

export interface GraphNodeEntry {
  /** 事件里的完整 id（「子图节点/…/节点」）。 */
  id: string;
  /** 它所在的那一层。 */
  path: PathSegment[];
  localId: string;
  title: string;
  /** 外面一层层子图节点的名字，从外到里。顶层的节点是空的。 */
  parents: string[];
  /** 算子名（子图节点是子图的名字）。 */
  opLabel: string;
}

/** 子图嵌套的上限：校验期就拒绝递归引用，这里只防手改坏的文件让它转不出来。 */
const MAX_DEPTH = 16;

/** 整张图里的节点，按文档顺序、先外后里（每个子图节点后面紧跟着它里面的）。 */
export function listGraphNodes(doc: GraphDoc, ops: ReadonlyMap<string, OperatorDesc>): GraphNodeEntry[] {
  const out: GraphNodeEntry[] = [];
  const walk = (level: GraphLevel, path: PathSegment[], parents: string[]) => {
    for (const node of level.nodes) {
      const title = nodeTitle(doc, node, ops);
      const subgraphId = subgraphIdOf(node.op);
      const def = subgraphId ? doc.subgraphs?.[subgraphId] : undefined;
      out.push({
        id: fullId(path, node.id),
        path,
        localId: node.id,
        title,
        parents,
        opLabel: def ? def.name || subgraphId! : (ops.get(node.op)?.label ?? node.op),
      });
      if (subgraphId && def && path.length < MAX_DEPTH) {
        walk(def, [...path, { nodeId: node.id, subgraphId }], [...parents, title]);
      }
    }
  };
  walk(doc, [], []);
  return out;
}

export interface NodeHit {
  entry: GraphNodeEntry;
  score: number;
  /** 命中的是哪个字段，见 NODE_FIELD_LABELS。 */
  fieldIndex: number;
}

/** 顺序即优先级：名字命中比 id、算子名、所在子图的名字更值钱。 */
export const NODE_FIELD_LABELS = ["名称", "id", "算子", "所在子图"] as const;

/** 空查询原样返回全部（文档顺序）；否则按分数排，同分保持文档顺序。 */
export function searchGraphNodes(entries: readonly GraphNodeEntry[], query: string): NodeHit[] {
  const q = query.trim();
  if (!q) return entries.map((entry) => ({ entry, score: 0, fieldIndex: 0 }));
  const hits: NodeHit[] = [];
  for (const entry of entries) {
    const match = fuzzyMatchAny(q, [entry.title, entry.localId, entry.opLabel, entry.parents.join(" ")]);
    if (match) hits.push({ entry, score: match.score, fieldIndex: match.fieldIndex });
  }
  hits.sort((a, b) => b.score - a.score);
  return hits;
}
