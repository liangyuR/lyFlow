// 查找节点（Ctrl+F）：整张图里的节点连子图里面的一起列出来（路径 id，ADR-0010），按名字模糊找，
// 选中后打开到它所在的那一层（revealNode）。库算子的内部只读、进不去，不列。
// 查询里能夹筛选词：is:muted（静音的）、is:error（这次出错的）、op:<算子 id 或名字的一段>，其余的照旧模糊找。

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
  /** 算子 id（子图节点是 sub:<id>）。op: 筛选认它。 */
  opId: string;
  /** 静音着（bypass）。 */
  muted: boolean;
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
        opId: node.op,
        muted: node.bypass === true,
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

/** 文档里的哪一个节点：子图定义是共用的，同一个定义的两个实例里列出来的是同一个节点（静音、参数都是同一份）。
 *  顶层的节点没有定义，只看 id。 */
export function docNodeKey(entry: GraphNodeEntry): string {
  return `${entry.path[entry.path.length - 1]?.subgraphId ?? ""}\u0000${entry.localId}`;
}

/** 顺序即优先级：名字命中比 id、算子名、所在子图的名字更值钱。 */
export const NODE_FIELD_LABELS = ["名称", "id", "算子", "所在子图"] as const;

/** 查询拆成筛选词与要模糊找的字。 */
export interface FinderQuery {
  text: string;
  muted: boolean;
  error: boolean;
  /** 这次判了不合格的（fail / high / low）。 */
  ng: boolean;
  /** 这次判了接近边界的。 */
  margin: boolean;
  /** op: 后面的字（小写），每一个都得在算子 id 或算子名里。 */
  ops: string[];
}

const IS_WORDS: Record<string, "muted" | "error" | "ng" | "margin"> = {
  muted: "muted",
  mute: "muted",
  bypass: "muted",
  静音: "muted",
  error: "error",
  err: "error",
  出错: "error",
  ng: "ng",
  不合格: "ng",
  margin: "margin",
  边界: "margin",
};

/** 「is:muted 体素」→ 只看静音的、再按「体素」找。认不得的 is:xxx 当普通字（找不到东西，比悄悄忽略好懂）。
 *  冒号全角半角都认。 */
export function parseFinderQuery(query: string): FinderQuery {
  const out: FinderQuery = { text: "", muted: false, error: false, ng: false, margin: false, ops: [] };
  const rest: string[] = [];
  for (const word of query.trim().split(/\s+/).filter(Boolean)) {
    const m = /^(is|op)[:：](.+)$/i.exec(word);
    const key = m?.[1]!.toLowerCase();
    const value = m?.[2] ?? "";
    const lower = value.toLowerCase();
    // 只认自己的键：is:constructor、is:__proto__ 会撞上 Object.prototype 上的东西
    if (key === "is" && Object.hasOwn(IS_WORDS, lower)) {
      out[IS_WORDS[lower]!] = true;
      continue;
    }
    if (key === "op") {
      out.ops.push(lower);
      continue;
    }
    rest.push(word);
  }
  out.text = rest.join(" ");
  return out;
}

/** 空查询原样返回全部（文档顺序）；否则按分数排，同分保持文档顺序。筛选词先筛，剩下的字再模糊找；
 *  isError 按完整 id 问这次运行里它出没出错（不给就当都没出错），toneOf 问它这次最差的判定。 */
export function searchGraphNodes(
  all: readonly GraphNodeEntry[],
  query: string,
  isError?: (id: string) => boolean,
  toneOf?: (id: string) => "ng" | "margin" | "ok" | null,
): NodeHit[] {
  const f = parseFinderQuery(query);
  const entries = all.filter(
    (e) =>
      (!f.muted || e.muted) &&
      (!f.error || (isError?.(e.id) ?? false)) &&
      (!f.ng || toneOf?.(e.id) === "ng") &&
      (!f.margin || toneOf?.(e.id) === "margin") &&
      f.ops.every((o) => e.opId.toLowerCase().includes(o) || e.opLabel.toLowerCase().includes(o)),
  );
  const q = f.text;
  if (!q) return entries.map((entry) => ({ entry, score: 0, fieldIndex: 0 }));
  const hits: NodeHit[] = [];
  for (const entry of entries) {
    const match = fuzzyMatchAny(q, [entry.title, entry.localId, entry.opLabel, entry.parents.join(" ")]);
    if (match) hits.push({ entry, score: match.score, fieldIndex: match.fieldIndex });
  }
  hits.sort((a, b) => b.score - a.score);
  return hits;
}
