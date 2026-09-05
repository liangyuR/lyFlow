//
// 子序列模糊匹配 + 打分。
//
// 节点搜索面板是 P0 里最影响手感的一项：用户会输 "vg" 期待命中 Voxel Grid，
// 输 "降采样" 期待命中中文 keyword。所以匹配要同时照顾：
//   - 缩写（首字母连击）
//   - 连续子串
//   - 中文关键词（不能只靠 ASCII 分词）
//
// 刻意不引 fuse.js 之类：几十行的东西，引进来还要调一堆权重参数。
//

export interface FuzzyMatch {
  score: number;
  /** 命中的字符下标，用来在 UI 里高亮。 */
  indices: number[];
}

const SCORE_CONSECUTIVE = 8;
const SCORE_WORD_START = 12;
const SCORE_CAMEL_START = 10;
const PENALTY_GAP = -1;

function isWordSeparator(c: string): boolean {
  return c === " " || c === "." || c === "_" || c === "-" || c === "/";
}

function isUpper(c: string): boolean {
  return c !== c.toLowerCase() && c === c.toUpperCase();
}

/**
 * 在 text 中按顺序找 query 的每个字符。返回 null 表示没匹配上。
 * 大小写不敏感；中文字符直接参与匹配。
 */
export function fuzzyMatch(query: string, text: string): FuzzyMatch | null {
  if (query.length === 0) return { score: 0, indices: [] };
  if (text.length === 0) return null;

  const q = query.toLowerCase();
  const t = text.toLowerCase();

  const indices: number[] = [];
  let score = 0;
  let ti = 0;
  let lastHit = -2;

  for (let qi = 0; qi < q.length; qi++) {
    const qc = q[qi]!;
    let found = -1;
    while (ti < t.length) {
      if (t[ti] === qc) {
        found = ti;
        break;
      }
      ti++;
    }
    if (found < 0) return null;

    if (found === lastHit + 1) {
      score += SCORE_CONSECUTIVE;
    } else {
      // 跳过的距离越远扣得越多，但有下限，避免长字符串被判死刑
      score += Math.max(PENALTY_GAP * (found - lastHit - 1), -6);
    }

    const prev = found > 0 ? text[found - 1]! : "";
    if (found === 0 || isWordSeparator(prev)) {
      score += SCORE_WORD_START;
    } else if (isUpper(text[found]!) && !isUpper(prev)) {
      score += SCORE_CAMEL_START;
    }

    indices.push(found);
    lastHit = found;
    ti = found + 1;
  }

  // 越短的目标越可能是用户想要的（"Voxel Grid" 优先于 "Voxel Grid Covariance"）
  score -= text.length * 0.1;
  return { score, indices };
}

/**
 * 在多个候选字段里取最佳匹配。fields 按优先级降序传入，
 * 靠前的字段命中会加权 —— label 命中比 keyword 命中更值钱。
 */
export function fuzzyMatchAny(
  query: string,
  fields: readonly string[],
): { score: number; fieldIndex: number; indices: number[] } | null {
  let best: { score: number; fieldIndex: number; indices: number[] } | null = null;
  for (let i = 0; i < fields.length; i++) {
    const m = fuzzyMatch(query, fields[i]!);
    if (!m) continue;
    const weighted = m.score - i * 4;
    if (!best || weighted > best.score) {
      best = { score: weighted, fieldIndex: i, indices: m.indices };
    }
  }
  return best;
}
