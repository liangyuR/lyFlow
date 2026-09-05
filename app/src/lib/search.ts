//
// 算子搜索。节点面板和画布上的搜索弹层共用这一份排序逻辑 ——
// 两处给出不一样的排序会让人觉得软件在骗自己。
//

import { fuzzyMatchAny } from "./fuzzy";
import type { OperatorDesc } from "../types/manifest";

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
