// 预览的显示设置：着色、色带、点大小、显示点数记在 localStorage（重启后还是上次的），手动着色范围按着色模式分开记
// （跟数据强相关，不落盘）。以前四样都是组件里的 state，每次开 app 回到默认；手动范围只有一份，强度下填的
// 0–255 切到高度后整片云变成一种颜色。

import type { RampName } from "./ramps";
import type { ShadingMode } from "./cloudScene";

export interface ViewerPrefs {
  shading: ShadingMode;
  ramp: RampName;
  pointSize: number;
  maxPoints: number;
}

export const VIEWER_PREFS_KEY = "lyflow.viewer.display";
/** 主预览「最多显示多少点」的可选项（抽样在 C++ 侧做）。 */
export const MAX_POINTS_CHOICES: readonly number[] = [100_000, 500_000, 2_000_000, 8_000_000];
export const POINT_SIZE_MIN = 0.5;
export const POINT_SIZE_MAX = 6;
export const DEFAULT_VIEWER_PREFS: ViewerPrefs = {
  shading: "intensity",
  ramp: "viridis",
  pointSize: 1.6,
  maxPoints: 2_000_000,
};

// 只引类型：ui store 读它，不该为此把 three 拉进来。写成 Record —— 加了新的着色 / 色带而这里漏了，编译不过
const SHADINGS: Record<ShadingMode, true> = { intensity: true, height: true, normal: true, rgb: true, flat: true };
const RAMP_NAMES: Record<RampName, true> = { viridis: true, gray: true, jet: true };

export function clampPointSize(v: number): number {
  return Math.min(POINT_SIZE_MAX, Math.max(POINT_SIZE_MIN, v));
}

/** 读回存着的那一份：逐项校验，坏的那一项回到默认，别的照用（改过版本、手改过存储都不至于整份作废）。 */
export function parseViewerPrefs(raw: string | null): ViewerPrefs {
  let v: Record<string, unknown> = {};
  try {
    const parsed: unknown = raw ? JSON.parse(raw) : null;
    if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) v = parsed as Record<string, unknown>;
  } catch {
    // 不是 JSON：整份当没有
  }
  const d = DEFAULT_VIEWER_PREFS;
  return {
    shading: typeof v.shading === "string" && Object.hasOwn(SHADINGS, v.shading) ? (v.shading as ShadingMode) : d.shading,
    ramp: typeof v.ramp === "string" && Object.hasOwn(RAMP_NAMES, v.ramp) ? (v.ramp as RampName) : d.ramp,
    pointSize:
      typeof v.pointSize === "number" && Number.isFinite(v.pointSize) ? clampPointSize(v.pointSize) : d.pointSize,
    maxPoints:
      typeof v.maxPoints === "number" && MAX_POINTS_CHOICES.includes(v.maxPoints) ? v.maxPoints : d.maxPoints,
  };
}

export function loadViewerPrefs(): ViewerPrefs {
  try {
    return parseViewerPrefs(globalThis.localStorage?.getItem(VIEWER_PREFS_KEY) ?? null);
  } catch {
    return DEFAULT_VIEWER_PREFS;
  }
}

export function saveViewerPrefs(p: ViewerPrefs): void {
  try {
    globalThis.localStorage?.setItem(VIEWER_PREFS_KEY, JSON.stringify(p));
  } catch {
    // 记不住就记不住，这一次照样生效
  }
}

/** 范围框里显示几位小数：至少 3 位，跨度小就多给 —— 点云以米为单位，亚毫米的高度范围按固定 3 位两端都是同一个数，
 *  填一个界时另一个界取的就是那个取整过的数，整片云塌成一种颜色。按跨度给，取整差不过跨度的千分之一。 */
export function rangeDigits(lo: number, hi: number): number {
  const span = hi - lo;
  if (!(span > 0) || !Number.isFinite(span)) return 3;
  return Math.min(12, Math.max(3, 3 - Math.floor(Math.log10(span))));
}

export function roundTo(v: number, digits: number): number {
  if (!Number.isFinite(v)) return 0;
  const f = 10 ** digits;
  return Math.round(v * f) / f;
}

/** 每种着色各自的手动范围；没有这一项 = 自动（取数据实际的最小 / 最大）。 */
export type ManualRanges = Readonly<Partial<Record<ShadingMode, readonly [number, number]>>>;

/** 这种着色此刻用的范围。 */
export function rangeFor(
  manual: ManualRanges,
  mode: ShadingMode,
  auto: readonly [number, number],
): { range: readonly [number, number]; auto: boolean } {
  const m = manual[mode];
  return m ? { range: m, auto: false } : { range: auto, auto: true };
}

/** 填了这种着色的一个界：另一个界取它此刻的值（自动时就是自动算出来的那个）。别的着色不动。 */
export function withRangeEnd(
  manual: ManualRanges,
  mode: ShadingMode,
  end: 0 | 1,
  value: number,
  current: readonly [number, number],
): ManualRanges {
  return { ...manual, [mode]: end === 0 ? [value, current[1]] : [current[0], value] };
}

/** 这种着色回到自动。别的着色不动。 */
export function withRangeAuto(manual: ManualRanges, mode: ShadingMode): ManualRanges {
  if (!manual[mode]) return manual;
  const next = { ...manual };
  delete next[mode];
  return next;
}
