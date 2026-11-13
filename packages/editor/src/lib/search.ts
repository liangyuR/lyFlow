// 算子搜索。节点面板和画布上的搜索弹层共用这一份排序逻辑 ——
// 两处给出不一样的排序会让人觉得软件在骗自己。

import { fuzzyMatch, normalizeSearchText } from './fuzzy';
import { augmentOperators, subgraphReaches } from './subgraph';
import { SUBGRAPH_OP_PREFIX, type GraphDoc } from '../types/graph';
import type { OperatorDesc, SnippetDesc } from '../types/manifest';

export interface OperatorHit {
  op: OperatorDesc;
  score: number;
  /** 命中的是哪个字段，见 FIELD_LABELS */
  fieldIndex: number;
  indices: number[];
}

export const FIELD_LABELS = ['名称', 'id', '关键词', '说明'] as const;

interface FieldMatch {
  score: number;
  fieldIndex: number;
  indices: number[];
}

const FIELD_PENALTIES = [0, 40, 30, 450];

function wordStart(text: string, index: number): boolean {
  if (index === 0) return true;
  const previous = text[index - 1]!;
  const current = text[index]!;
  return (
    !/[a-z0-9]/i.test(normalizeSearchText(previous)) ||
    (/[a-z]/.test(previous) && /[A-Z]/.test(current))
  );
}

function editDistance(left: string, right: string, limit: number): number {
  if (Math.abs(left.length - right.length) > limit) return limit + 1;
  let previous = Array.from({ length: right.length + 1 }, (_, i) => i);
  let beforePrevious = previous;
  for (let i = 1; i <= left.length; i++) {
    const row = [i];
    for (let j = 1; j <= right.length; j++) {
      let distance = Math.min(
        row[j - 1]! + 1,
        previous[j]! + 1,
        previous[j - 1]! + (left[i - 1] === right[j - 1] ? 0 : 1),
      );
      if (i > 1 && j > 1 && left[i - 1] === right[j - 2] && left[i - 2] === right[j - 1]) {
        distance = Math.min(distance, beforePrevious[j - 2]! + 1);
      }
      row.push(distance);
    }
    if (Math.min(...row) > limit) return limit + 1;
    beforePrevious = previous;
    previous = row;
  }
  return previous[right.length]!;
}

function tokenMatch(query: string, text: string): { score: number; indices: number[] } | null {
  const normalized = normalizeSearchText(text);
  const match = fuzzyMatch(query, text);
  if (match) {
    const at = normalized.indexOf(query);
    let quality = 0;
    if (query === normalized) quality = 1400;
    else if (at >= 0) {
      const before = at === 0 || !/[\p{L}\p{N}]/u.test(normalized[at - 1]!);
      const after =
        at + query.length === normalized.length ||
        !/[\p{L}\p{N}]/u.test(normalized[at + query.length]!);
      quality = /\p{Script=Han}/u.test(query)
        ? 1400
        : before && after
          ? 1320
          : before
            ? 1140
            : 1040;
    } else {
      const span = match.indices[match.indices.length - 1]! - match.indices[0]! + 1;
      const chunks = match.indices.every((index, i) =>
        i === 0
          ? wordStart(text, index)
          : index === match.indices[i - 1]! + 1 || wordStart(text, index),
      );
      if (chunks) quality = 820;
      else if (span <= Math.max(query.length + 2, query.length * 1.65)) quality = 720;
    }
    if (quality > 0)
      return { score: quality - Math.min(normalized.length, 600) * 0.1, indices: match.indices };
  }
  if (!/^[a-z]{4,64}$/.test(query)) return null;
  const limit = query.length >= 10 ? 2 : 1;
  const words =
    normalizeSearchText(text.replace(/([\p{Ll}\p{N}])(\p{Lu})/gu, '$1 $2')).match(/[a-z]{4,64}/g) ??
    [];
  let best: { score: number; indices: number[] } | null = null;
  for (const word of new Set(words)) {
    if (query[0] !== word[0] && !(query[0] === word[1] && query[1] === word[0])) continue;
    const distance = editDistance(query, word, limit);
    if (distance > limit || distance / Math.max(query.length, word.length) > 0.25) continue;
    const indices = fuzzyMatch(word, text)?.indices;
    if (!indices) continue;
    const score = 500 - distance * 80 - Math.min(normalized.length, 600) * 0.1;
    if (!best || score > best.score) best = { score, indices };
  }
  return best;
}

function matchFields(query: string, fields: readonly string[]): FieldMatch | null {
  const tokens = [...new Set(query.split(/\s+/u).filter(Boolean))];
  const matches: FieldMatch[] = [];
  for (const token of tokens) {
    let best: FieldMatch | null = null;
    for (let fieldIndex = 0; fieldIndex < fields.length; fieldIndex++) {
      const match = tokenMatch(token, fields[fieldIndex]!);
      if (!match) continue;
      const score = match.score - FIELD_PENALTIES[fieldIndex]!;
      if (!best || score > best.score) best = { score, fieldIndex, indices: match.indices };
    }
    if (!best) return null;
    matches.push(best);
  }
  if (matches.length === 0) return null;
  const primary = matches.reduce((best, match) =>
    FIELD_PENALTIES[match.fieldIndex]! < FIELD_PENALTIES[best.fieldIndex]! ? match : best,
  );
  const indices = [
    ...new Set(
      matches
        .filter((match) => match.fieldIndex === primary.fieldIndex)
        .flatMap((match) => match.indices),
    ),
  ].sort((a, b) => a - b);
  let score = matches.reduce((total, match) => total + match.score, 0) / matches.length;
  if (tokens.length > 1) {
    const phrase = tokens.join(' ');
    let bonus = 0;
    for (let i = 0; i < fields.length; i++) {
      const normalized = normalizeSearchText(fields[i]!).replace(/\s+/gu, ' ');
      if (normalized.includes(phrase))
        bonus = Math.max(bonus, (normalized === phrase ? 200 : 80) - FIELD_PENALTIES[i]!);
    }
    score += bonus;
  }
  return { score, fieldIndex: primary.fieldIndex, indices };
}

export function searchOperators(operators: readonly OperatorDesc[], query: string): OperatorHit[] {
  const q = normalizeSearchText(query).trim();
  if (!q) return [];
  const hits: OperatorHit[] = [];
  for (const op of operators) {
    const match = matchFields(q, [op.label, op.id, (op.keywords ?? []).join(' '), op.doc ?? '']);
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

export const SNIPPET_FIELD_LABELS = ['名称', 'id', '分类', '说明'] as const;

/** 片段搜索：与算子同一套模糊匹配（名称、id、分类、说明），搜索弹层与面板的片段分支共用。 */
export function searchSnippets(snippets: readonly SnippetDesc[], query: string): SnippetHit[] {
  const q = normalizeSearchText(query).trim();
  if (!q) return [];
  const hits: SnippetHit[] = [];
  for (const snippet of snippets) {
    const match = matchFields(q, [
      snippet.label,
      snippet.id,
      snippet.category ?? '',
      snippet.doc ?? '',
    ]);
    if (match) hits.push({ snippet, ...match });
  }
  hits.sort((a, b) => b.score - a.score);
  return hits;
}

/** 搜索里能加的算子：manifest 里的，加上这张图里的子图（sub:，与 Ctrl+D 复制出来的是同一份定义）。
 *  around 是当前所在的那几层子图：会套进它们自己的子图不列（放进去就成了递归）。 */
export function searchableOps(
  base: readonly OperatorDesc[],
  subgraphs: GraphDoc['subgraphs'],
  around: ReadonlySet<string>,
): OperatorDesc[] {
  if (!subgraphs || Object.keys(subgraphs).length === 0) return [...base];
  const subs = [...augmentOperators(new Map(), subgraphs).values()].filter(
    (op) =>
      op.id.startsWith(SUBGRAPH_OP_PREFIX) &&
      !subgraphReaches(subgraphs, op.id.slice(SUBGRAPH_OP_PREFIX.length), around),
  );
  return [...base, ...subs];
}
