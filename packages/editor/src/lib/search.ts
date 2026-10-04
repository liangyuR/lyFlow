// 算子搜索。节点面板和画布上的搜索弹层共用这一份排序逻辑 ——
// 两处给出不一样的排序会让人觉得软件在骗自己。

import { fuzzyMatchAny } from "./fuzzy";
import { augmentOperators, subgraphReaches } from "./subgraph";
import { SUBGRAPH_OP_PREFIX, type GraphDoc } from "../types/graph";
import type { OperatorDesc, SnippetDesc } from "../types/manifest";

export interface OperatorHit {
  op: OperatorDesc;
  score: number;
  /** 命中的是哪个字段，见 FIELD_LABELS */
  fieldIndex: number;
  indices: number[];
}

/** 顺序即优先级：label 命中比 keyword 命中更值钱（fuzzyMatchAny 按下标降权）。 */
export const FIELD_LABELS = ["名称", "id", "关键词", "说明"] as const;

export function searchOperators(
  operators: readonly OperatorDesc[],
  query: string,
): OperatorHit[] {
  const q = query.trim();
  if (!q) return [];
  const hits: OperatorHit[] = [];
  for (const op of operators) {
    const match = fuzzyMatchAny(q, [
      op.label,
      op.id,
      (op.keywords ?? []).join(" "),
      op.doc ?? "",
    ]);
    if (match) hits.push({ op, ...match });
  }
  hits.sort((a, b) => b.score - a.score);
  return hits;
}

export interface SnippetHit {
  snippet: SnippetDesc;
  score: number;
  /** 命中的是哪个字段，见 SNIPPET_FIELD_LABELS */
  fieldIndex: number;
  indices: number[];
}

export const SNIPPET_FIELD_LABELS = ["名称", "id", "分类", "说明"] as const;

/** 片段搜索：与算子同一套模糊匹配（名称、id、分类、说明），搜索弹层与面板的片段分支共用。 */
export function searchSnippets(snippets: readonly SnippetDesc[], query: string): SnippetHit[] {
  const q = query.trim();
  if (!q) return [];
  const hits: SnippetHit[] = [];
  for (const snippet of snippets) {
    const match = fuzzyMatchAny(q, [snippet.label, snippet.id, snippet.category ?? "", snippet.doc ?? ""]);
    if (match) hits.push({ snippet, ...match });
  }
  hits.sort((a, b) => b.score - a.score);
  return hits;
}

/** 搜索里能加的算子：manifest 里的，加上这张图里的子图（sub:，与 Ctrl+D 复制出来的是同一份定义）。
 *  around 是当前所在的那几层子图：会套进它们自己的子图不列（放进去就成了递归）。 */
export function searchableOps(
  base: readonly OperatorDesc[],
  subgraphs: GraphDoc["subgraphs"],
  around: ReadonlySet<string>,
): OperatorDesc[] {
  if (!subgraphs || Object.keys(subgraphs).length === 0) return [...base];
  const subs = [...augmentOperators(new Map(), subgraphs).values()].filter(
    (op) => op.id.startsWith(SUBGRAPH_OP_PREFIX) && !subgraphReaches(subgraphs, op.id.slice(SUBGRAPH_OP_PREFIX.length), around),
  );
  return [...base, ...subs];
}
