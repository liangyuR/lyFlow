// 预览里的选点与测距（docs/measure-plan.md）。纯函数，不依赖 three：矩阵按 16 个数传进来，
// node:test 直接测。

import { num } from "./format";

export interface Viewport {
  x: number;
  y: number;
  w: number;
  h: number;
}

/** 屏幕空间最近点（M4）。matrix = camera.projectionMatrix × camera.matrixWorldInverse，列主序
 *  （three 的 Matrix4.elements）；点云的 world 矩阵恒为单位阵。viewport 是这一栏在画布里的矩形，
 *  click 与它同一坐标系（CSS px，左上为原点）。相机背后、远近平面之外、非有限坐标（NaN 槽）都跳过。
 *  离光标差不到 1 px 的几个点里取离相机近的 —— 透视下前后叠着的点，用户想点的是看得见的那个。 */
export function pickNearest(
  xyz: ArrayLike<number>,
  count: number,
  matrix: ArrayLike<number>,
  viewport: Viewport,
  click: { x: number; y: number },
  radiusPx = 8,
): { index: number; distPx: number } | null {
  const m = matrix;
  const r2 = radiusPx * radiusPx;
  let best = -1;
  let bestD2 = Infinity;
  let bestDepth = Infinity;
  for (let i = 0; i < count; i += 1) {
    const x = xyz[i * 3]!;
    const y = xyz[i * 3 + 1]!;
    const z = xyz[i * 3 + 2]!;
    if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z)) continue;
    const w = m[3]! * x + m[7]! * y + m[11]! * z + m[15]!;
    if (!(w > 0)) continue;
    const nz = (m[2]! * x + m[6]! * y + m[10]! * z + m[14]!) / w;
    if (nz < -1 || nz > 1) continue;
    const nx = (m[0]! * x + m[4]! * y + m[8]! * z + m[12]!) / w;
    const ny = (m[1]! * x + m[5]! * y + m[9]! * z + m[13]!) / w;
    const sx = viewport.x + ((nx + 1) / 2) * viewport.w;
    const sy = viewport.y + ((1 - ny) / 2) * viewport.h;
    const d2 = (sx - click.x) ** 2 + (sy - click.y) ** 2;
    if (d2 > r2) continue;
    const d = Math.sqrt(d2);
    const bd = Math.sqrt(bestD2);
    if (d < bd - 1 || (Math.abs(d - bd) <= 1 && nz < bestDepth)) {
      best = i;
      bestD2 = d2;
      bestDepth = nz;
    }
  }
  return best < 0 ? null : { index: best, distPx: Math.sqrt(bestD2) };
}

// ------------------------------------------------------------ 一组测量

export interface Pick {
  xyz: [number, number, number];
  /** 在哪一栏点的：0 = A（单栏也是 0），1 = B。 */
  pane: 0 | 1;
}

export interface Measure {
  p1: Pick | null;
  p2: Pick | null;
  /** 选点之后云换了（同一节点重跑）：点位是在之前那片云上选的（M7）。 */
  stale: boolean;
}

export const NO_MEASURE: Measure = { p1: null, p2: null, stale: false };

/** 第 1 次点 = P1，第 2 次 = P2，第 3 次重新开始一组（M2）。 */
export function addPick(m: Measure, pick: Pick): Measure {
  if (!m.p1 || m.p2) return { p1: pick, p2: null, stale: false };
  return { p1: m.p1, p2: pick, stale: false };
}

export function pickCount(m: Measure): 0 | 1 | 2 {
  return m.p2 ? 2 : m.p1 ? 1 : 0;
}

/** P2 − P1。 */
export function deltaOf(m: Measure): [number, number, number] | null {
  if (!m.p1 || !m.p2) return null;
  const [a, b] = [m.p1.xyz, m.p2.xyz];
  return [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
}

export function distanceOf(m: Measure): number | null {
  const d = deltaOf(m);
  return d ? Math.hypot(d[0], d[1], d[2]) : null;
}

/** 距离的写法：米 + 毫米并列（M5）。 */
export function lengthText(v: number): string {
  return `${num(v)} m · ${num(v * 1000)} mm`;
}

/** 坐标与 Δ 只给 4 位有效数字：readout 在矮小的预览里，6 位的三元组一行放不下；
 *  要精读的是距离，距离照旧 6 位（lengthText）。 */
function short(x: number): string {
  if (!Number.isFinite(x)) return "—";
  return String(Number(x.toPrecision(4)));
}

function vec(v: readonly number[], sign = false): string {
  return `(${v.map((x) => (sign && x > 0 ? `+${short(x)}` : short(x))).join(", ")}) m`;
}

/** readout 的几行。compare = 对比模式，点位后面标 A / B；2D 剖面多一行 XY 平面距离。 */
export function measureLines(
  m: Measure,
  mode: "3d" | "2d",
  compare = false,
): { key: string; label: string; text: string }[] {
  const pane = (p: Pick) => (compare ? ` · ${p.pane === 0 ? "A" : "B"}` : "");
  const out: { key: string; label: string; text: string }[] = [];
  if (!m.p1) return out;
  out.push({ key: "p1", label: `P1${pane(m.p1)}`, text: vec(m.p1.xyz) });
  if (!m.p2) return out;
  out.push({ key: "p2", label: `P2${pane(m.p2)}`, text: vec(m.p2.xyz) });
  const d = deltaOf(m)!;
  out.push({ key: "dist", label: "|d|", text: lengthText(Math.hypot(d[0], d[1], d[2])) });
  if (mode === "2d") out.push({ key: "distXY", label: "|d| XY", text: lengthText(Math.hypot(d[0], d[1])) });
  out.push({ key: "delta", label: "Δ", text: vec(d, true) });
  return out;
}
