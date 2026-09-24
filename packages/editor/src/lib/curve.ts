// curve 参数的值（docs/operator-manifest.md「transform 与 curve 的值」）：
//   { "points": [[x, y], …], "interp": "linear" | "smooth" }
// x 在 [0, 1] 内严格递增、至少两个点；y 是有限数，声明了 min/max 时也要落在里面；interp 缺省 linear。
// 判据与取值算法和 core 的 checkCurveValue / evaluateCurve（manifest.h）逐条对应 —— 两边画出、算出同一条线。
// 纯函数，不碰 DOM（node --test 直接测）。

export type CurveInterp = "linear" | "smooth";
export type CurvePoint = [number, number];

export interface CurveValue {
  points: CurvePoint[];
  interp?: CurveInterp;
}

/** 相邻两点的 x 至少差这么多：拖动时挤不过邻居，也不会挤出两个 x 相同的点。 */
export const MIN_GAP = 0.001;

export const IDENTITY_CURVE: CurveValue = Object.freeze({
  points: [
    [0, 0],
    [1, 1],
  ],
  interp: "linear",
}) as CurveValue;

/** 同 core 的 checkCurveValue：合法返回 null，否则返回一句原因。 */
export function curveProblem(value: unknown, min?: number, max?: number): string | null {
  if (typeof value !== "object" || value === null || Array.isArray(value)) {
    return '应当是 {"points": [[x, y], …], "interp": …} 这样的对象';
  }
  for (const k of Object.keys(value)) {
    if (k !== "points" && k !== "interp") return `不认识的字段 '${k}'（只有 points 与 interp）`;
  }
  const v = value as { points?: unknown; interp?: unknown };
  if (v.interp !== undefined && v.interp !== "linear" && v.interp !== "smooth") {
    return 'interp 只能是 "linear" 或 "smooth"';
  }
  if (!Array.isArray(v.points) || v.points.length < 2) return "points 至少要有两个控制点";
  let prev = 0;
  for (let i = 0; i < v.points.length; i += 1) {
    const pt: unknown = v.points[i];
    const at = `第 ${i + 1} 个控制点`;
    if (!Array.isArray(pt) || pt.length !== 2 || typeof pt[0] !== "number" || typeof pt[1] !== "number") {
      return `${at}应当是 [x, y] 两个数`;
    }
    const [x, y] = pt as [number, number];
    if (!Number.isFinite(x) || !Number.isFinite(y)) return `${at}必须是有限的数字`;
    if (x < 0 || x > 1) return `${at}的 x 必须在 [0, 1] 内，实际是 ${x}`;
    if (i > 0 && !(x > prev)) return `${at}的 x 必须大于前一个点（${prev}）`;
    if (min !== undefined && y < min) return `${at}的 y 不能小于 ${min}`;
    if (max !== undefined && y > max) return `${at}的 y 不能大于 ${max}`;
    prev = x;
  }
  return null;
}

/** 取一份能画的曲线。不合法（老文件、手改坏了）就退回 fallback（通常是 manifest 默认值），再不行用恒等线。 */
export function asCurve(value: unknown, fallback?: unknown): CurveValue {
  if (curveProblem(value) === null) return value as CurveValue;
  if (fallback !== undefined && curveProblem(fallback) === null) return fallback as CurveValue;
  return IDENTITY_CURVE;
}

/** 写回用的规范形：points 深拷一份、数抹掉浮点尾巴。interp 原样保留（没写就不写）。 */
export function normalized(points: readonly CurvePoint[], interp: CurveInterp | undefined): CurveValue {
  const out: CurveValue = {
    points: points.map(([x, y]) => [round(x), round(y)] as CurvePoint),
  };
  if (interp !== undefined) out.interp = interp;
  return out;
}

function round(v: number): number {
  return Number.parseFloat(v.toPrecision(10));
}

/** 与 core 的 evaluateCurve 同一套：线性，或 Fritsch–Carlson 单调三次 Hermite；端点外取端点值。 */
export function evaluate(curve: CurveValue, x: number): number {
  const pts = curve.points;
  const n = pts.length;
  if (n === 0) return 0;
  if (x <= pts[0]![0]) return pts[0]![1];
  if (x >= pts[n - 1]![0]) return pts[n - 1]![1];
  let k = 0;
  while (k + 1 < n && x > pts[k + 1]![0]) k += 1;
  const [x0, y0] = pts[k]!;
  const [x1, y1] = pts[k + 1]!;
  const h = x1 - x0;
  const t = (x - x0) / h;
  if (curve.interp !== "smooth") return y0 + (y1 - y0) * t;
  const m = tangents(pts);
  const t2 = t * t;
  const t3 = t2 * t;
  return (
    (2 * t3 - 3 * t2 + 1) * y0 + (t3 - 2 * t2 + t) * h * m[k]! + (-2 * t3 + 3 * t2) * y1 + (t3 - t2) * h * m[k + 1]!
  );
}

/** Fritsch–Carlson 的切线：割线斜率的平均，符号相反或一段水平时取 0，再压进半径 3 的圆。 */
function tangents(pts: readonly CurvePoint[]): number[] {
  const n = pts.length;
  const delta: number[] = [];
  for (let i = 0; i + 1 < n; i += 1) delta.push((pts[i + 1]![1] - pts[i]![1]) / (pts[i + 1]![0] - pts[i]![0]));
  const m = new Array<number>(n).fill(0);
  m[0] = delta[0]!;
  m[n - 1] = delta[n - 2]!;
  for (let i = 1; i + 1 < n; i += 1) {
    m[i] = delta[i - 1]! * delta[i]! <= 0 ? 0 : (delta[i - 1]! + delta[i]!) / 2;
  }
  for (let i = 0; i + 1 < n; i += 1) {
    if (delta[i] === 0) {
      m[i] = 0;
      m[i + 1] = 0;
      continue;
    }
    const a = m[i]! / delta[i]!;
    const b = m[i + 1]! / delta[i]!;
    const s = a * a + b * b;
    if (s > 9) {
      const tau = 3 / Math.sqrt(s);
      m[i] = tau * a * delta[i]!;
      m[i + 1] = tau * b * delta[i]!;
    }
  }
  return m;
}

/** 把第 i 个点挪到 (x, y)：x 夹在邻居之间（留 MIN_GAP）与 [0, 1] 里，y 夹进 [lo, hi]。 */
export function movePoint(
  points: readonly CurvePoint[],
  i: number,
  x: number,
  y: number,
  lo: number | undefined,
  hi: number | undefined,
): CurvePoint[] {
  const left = i > 0 ? points[i - 1]![0] + MIN_GAP : 0;
  const right = i < points.length - 1 ? points[i + 1]![0] - MIN_GAP : 1;
  const nx = Math.min(Math.max(x, Math.max(0, left)), Math.min(1, right));
  let ny = y;
  if (lo !== undefined) ny = Math.max(ny, lo);
  if (hi !== undefined) ny = Math.min(ny, hi);
  const next = points.map((p) => [p[0], p[1]] as CurvePoint);
  next[i] = [nx, ny];
  return next;
}

/** 在 x 处插一个点，y 取曲线当前在那里的值（插完线形不变）。x 太挨着已有的点就不插，返回 null。 */
export function insertAt(curve: CurveValue, x: number): { points: CurvePoint[]; index: number } | null {
  const cx = Math.min(Math.max(x, 0), 1);
  if (curve.points.some((p) => Math.abs(p[0] - cx) < MIN_GAP)) return null;
  const points = curve.points.map((p) => [p[0], p[1]] as CurvePoint);
  let index = points.findIndex((p) => p[0] > cx);
  if (index < 0) index = points.length;
  points.splice(index, 0, [cx, evaluate(curve, cx)]);
  return { points, index };
}

/** 在最宽的那一段正中间插一个点（「+ 控制点」按钮）。 */
export function insertInWidestGap(curve: CurveValue): { points: CurvePoint[]; index: number } | null {
  let best = 0;
  let at = -1;
  for (let i = 0; i + 1 < curve.points.length; i += 1) {
    const gap = curve.points[i + 1]![0] - curve.points[i]![0];
    if (gap > best) {
      best = gap;
      at = i;
    }
  }
  if (at < 0 || best < 2 * MIN_GAP) return null;
  return insertAt(curve, (curve.points[at]![0] + curve.points[at + 1]![0]) / 2);
}

/** 删掉第 i 个点。至少留两个。 */
export function removePoint(points: readonly CurvePoint[], i: number): CurvePoint[] | null {
  if (points.length <= 2 || i < 0 || i >= points.length) return null;
  return points.filter((_, k) => k !== i).map((p) => [p[0], p[1]] as CurvePoint);
}

/** 画图用的 y 范围：softMin/softMax 优先，其次 min/max，缺省 [0, 1]；再撑开到包住所有点。 */
export function yRange(
  curve: CurveValue,
  spec: { min?: number | undefined; max?: number | undefined; softMin?: number | undefined; softMax?: number | undefined },
): [number, number] {
  let lo = spec.softMin ?? spec.min ?? 0;
  let hi = spec.softMax ?? spec.max ?? 1;
  for (const [, y] of curve.points) {
    lo = Math.min(lo, y);
    hi = Math.max(hi, y);
  }
  if (!(hi > lo)) hi = lo + 1;
  return [lo, hi];
}

/** 面板行上的摘要：「3 点 · smooth」。 */
export function summarize(value: unknown): string {
  const c = asCurve(value);
  return `${c.points.length} 点 · ${c.interp === "smooth" ? "平滑" : "线性"}`;
}
