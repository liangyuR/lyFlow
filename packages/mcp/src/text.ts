const SENTENCE_END = /[。！？!?]|\.\s|\n/;
const MAX_SENTENCE = 160;

export function firstSentence(doc: string | undefined): string {
  if (!doc) return "";
  const text = doc.trim();
  const m = SENTENCE_END.exec(text);
  let head = m && m.index > 0 ? text.slice(0, m.index) : text;
  head = head.trim();
  if (head.length > MAX_SENTENCE) head = `${head.slice(0, MAX_SENTENCE)}…`;
  return head;
}

function distance(a: string, b: string): number {
  const rows = a.length + 1;
  const cols = b.length + 1;
  let prev = new Array<number>(cols);
  let cur = new Array<number>(cols);
  for (let j = 0; j < cols; j += 1) prev[j] = j;
  for (let i = 1; i < rows; i += 1) {
    cur[0] = i;
    for (let j = 1; j < cols; j += 1) {
      const cost = a[i - 1] === b[j - 1] ? 0 : 1;
      cur[j] = Math.min(
        (prev[j] as number) + 1,
        (cur[j - 1] as number) + 1,
        (prev[j - 1] as number) + cost,
      );
    }
    const swap = prev;
    prev = cur;
    cur = swap;
  }
  return prev[cols - 1] as number;
}

export function nearest(target: string, candidates: string[], count: number): string[] {
  const needle = target.toLowerCase();
  return candidates
    .map((id) => {
      const lower = id.toLowerCase();
      const contains = lower.includes(needle) || needle.includes(lower);
      return { id, score: distance(needle, lower) - (contains ? 100 : 0) };
    })
    .sort((a, b) => a.score - b.score || a.id.localeCompare(b.id))
    .slice(0, count)
    .map((c) => c.id);
}

export function matches(query: string, fields: (string | undefined)[]): boolean {
  const needle = query.trim().toLowerCase();
  if (!needle) return true;
  return fields.some((f) => (f ?? "").toLowerCase().includes(needle));
}
