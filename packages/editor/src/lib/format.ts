// 给人看的数字格式。纯函数，Inspector、Edge Peek 的值视图与对比的差异表共用一份 ——
// 同一个数在三处显示得不一样，用户会以为是三个数。

/** 6 位有效数字，去掉末尾的 0。非有限值显示成「—」。 */
export function num(v: number | undefined | null): string {
  if (v === undefined || v === null || !Number.isFinite(v)) return "—";
  return String(Number(v.toPrecision(6)));
}

/** 带符号：正数前面加「+」，差值列用（Δ = A − B）。 */
export function signed(v: number): string {
  const text = num(v);
  return v > 0 ? `+${text}` : text;
}

/** 整数按三位分组，组间一个空格：2 000 000。不走 toLocaleString，免得跟着系统区域变。 */
export function grouped(n: number): string {
  const sign = n < 0 ? "-" : "";
  return sign + String(Math.abs(Math.round(n))).replace(/\B(?=(\d{3})+(?!\d))/g, " ");
}
