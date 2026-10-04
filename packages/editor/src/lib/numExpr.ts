// 数字框里打的字：数、算式（0.01*2、(3+4)/2）、相对改法（*2、/2、+=5、-=0.5、*=1.5）。纯函数，NumberInput 用它。
// 以前是 type=number + parseFloat：打不了算式，「1.5abc」被当成 1.5，越界悄悄夹住、看不懂悄悄恢复，都没有一句话。
//
// 「-0.5」「+5」是绝对值（不然负数打不进去）；相对改法只认开头的 * / 与 += -= *= /=。

export type NumOp = "+" | "-" | "*" | "/";

export type NumEdit =
  | { kind: "abs"; value: number; /** 是算式（不是一个光秃秃的数）：结果要抹掉浮点尾巴。 */ expr: boolean }
  | { kind: "rel"; op: NumOp; operand: number }
  | { kind: "empty" }
  | { kind: "error"; msg: string };

export interface NumApplied {
  value: number;
  /** 越界夹到了哪一头。 */
  clamped?: "min" | "max" | undefined;
  /** 整数参数：结果不是整数，四舍五入了。 */
  rounded?: boolean | undefined;
}

/** 输入法打出来的全角符号、中文句号、乘除号、减号都换成 ASCII，空白去掉。 */
export function normalizeNumText(s: string): string {
  return s
    .normalize("NFKC")
    .replace(/。/g, ".")
    .replace(/×/g, "*")
    .replace(/÷/g, "/")
    .replace(/[−–—]/g, "-")
    .replace(/\s+/g, "");
}

const PLAIN = /^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$/;

/** 小的递归下降求值：+ - * /、括号、正负号、1e-3 这样的数。不用 eval。 */
function evaluate(src: string): number {
  let i = 0;
  const fail = (msg: string): never => {
    throw new Error(msg);
  };
  const number = (): number => {
    const m = /^(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?/.exec(src.slice(i));
    if (!m) fail(i < src.length ? `「${src[i]}」这里接不上` : "算式不完整");
    i += m![0].length;
    return Number(m![0]);
  };
  const factor = (): number => {
    const c = src[i];
    if (c === "+" || c === "-") {
      i += 1;
      const v = factor();
      return c === "-" ? -v : v;
    }
    if (c === "(") {
      i += 1;
      const v = expr();
      if (src[i] !== ")") fail("括号没配上");
      i += 1;
      return v;
    }
    return number();
  };
  const term = (): number => {
    let v = factor();
    while (src[i] === "*" || src[i] === "/") {
      const op = src[i];
      i += 1;
      const r = factor();
      if (op === "/" && r === 0) fail("不能除以 0");
      v = op === "*" ? v * r : v / r;
    }
    return v;
  };
  const expr = (): number => {
    let v = term();
    while (src[i] === "+" || src[i] === "-") {
      const op = src[i];
      i += 1;
      const r = term();
      v = op === "+" ? v + r : v - r;
    }
    return v;
  };
  const v = expr();
  if (i < src.length) fail(src[i] === ")" ? "括号没配上" : `「${src.slice(i)}」看不懂`);
  if (!Number.isFinite(v)) fail("结果不是一个有限的数");
  return v;
}

export function parseNumEdit(text: string): NumEdit {
  const s = normalizeNumText(text);
  if (s === "") return { kind: "empty" };
  try {
    const rel = /^([*/])=?(.+)$/.exec(s) ?? /^([+-])=(.+)$/.exec(s);
    if (rel) {
      const op = rel[1] as NumOp;
      const operand = evaluate(rel[2]!);
      if (op === "/" && operand === 0) return { kind: "error", msg: "不能除以 0" };
      return { kind: "rel", op, operand };
    }
    return { kind: "abs", value: evaluate(s), expr: !PLAIN.test(s) };
  } catch (e) {
    return { kind: "error", msg: e instanceof Error ? e.message : String(e) };
  }
}

/** 落到这个参数上：相对改法按 cur 算；算式的结果抹掉浮点尾巴（0.1+0.2 → 0.3）；整数四舍五入；越界夹住并记下来。 */
export function applyNumEdit(
  edit: Extract<NumEdit, { kind: "abs" } | { kind: "rel" }>,
  cur: number,
  lim: { integer: boolean; min?: number | undefined; max?: number | undefined },
): NumApplied {
  let v: number;
  if (edit.kind === "abs") {
    v = edit.expr ? Number.parseFloat(edit.value.toPrecision(12)) : edit.value;
  } else {
    const r = edit.op === "+" ? cur + edit.operand : edit.op === "-" ? cur - edit.operand : edit.op === "*" ? cur * edit.operand : cur / edit.operand;
    v = Number.parseFloat(r.toPrecision(12));
  }
  let rounded = false;
  if (lim.integer && !Number.isInteger(v)) {
    v = Math.round(v);
    rounded = true;
  }
  let clamped: "min" | "max" | undefined;
  if (lim.min !== undefined && v < lim.min) {
    v = lim.min;
    clamped = "min";
  } else if (lim.max !== undefined && v > lim.max) {
    v = lim.max;
    clamped = "max";
  }
  return { value: v, clamped, rounded };
}

/** 夹住、取整了要说一声（toast 的字）。什么都没发生返回 null。 */
export function numEditNote(
  label: string,
  results: readonly NumApplied[],
  lim: { min?: number | undefined; max?: number | undefined },
): string | null {
  const max = results.filter((r) => r.clamped === "max").length;
  const min = results.filter((r) => r.clamped === "min").length;
  const rounded = results.filter((r) => r.rounded).length;
  const of = (n: number) => (results.length > 1 ? `${results.length} 个里 ${n} 个` : "");
  const parts: string[] = [];
  if (max > 0) parts.push(`${of(max)}超出上限 ${lim.max}，已取上限`);
  if (min > 0) parts.push(`${of(min)}低于下限 ${lim.min}，已取下限`);
  if (rounded > 0) parts.push(`${of(rounded)}不是整数，已四舍五入`);
  return parts.length > 0 ? `${label}：${parts.join("；")}` : null;
}
