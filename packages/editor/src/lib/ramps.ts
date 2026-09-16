import * as THREE from "three";

export type RampName = "viridis" | "gray" | "jet";

/** matplotlib viridis 的 11 个采样点，线性插值就够看。 */
export const VIRIDIS: [number, number, number][] = [
  [0.267, 0.005, 0.329],
  [0.283, 0.141, 0.458],
  [0.254, 0.265, 0.53],
  [0.207, 0.372, 0.553],
  [0.164, 0.471, 0.558],
  [0.128, 0.567, 0.551],
  [0.135, 0.659, 0.518],
  [0.267, 0.749, 0.441],
  [0.478, 0.821, 0.318],
  [0.741, 0.873, 0.15],
  [0.993, 0.906, 0.144],
];

export function viridisRamp(t: number, out: THREE.Color) {
  const x = Math.max(0, Math.min(1, t)) * (VIRIDIS.length - 1);
  const i = Math.min(VIRIDIS.length - 2, Math.floor(x));
  const f = x - i;
  const a = VIRIDIS[i]!;
  const b = VIRIDIS[i + 1]!;
  out.setRGB(a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f);
}

/** 灰度。打印和做对比图时比彩色可靠。 */
export function grayRamp(t: number, out: THREE.Color) {
  const v = 0.12 + 0.85 * Math.max(0, Math.min(1, t));
  out.setRGB(v, v, v);
}

/** 蓝→青→黄→红。饱和度高，找异常点最快。 */
export function jetRamp(t: number, out: THREE.Color) {
  const x = Math.max(0, Math.min(1, t));
  if (x < 0.5) out.setRGB(0.15 + 0.1 * x, 0.4 + 1.2 * x, 1.0 - 0.6 * x);
  else out.setRGB(0.35 + 1.3 * (x - 0.5), 1.0 - 1.2 * (x - 0.5), 0.4 - 0.7 * (x - 0.5));
}

export const RAMPS: Record<RampName, (t: number, out: THREE.Color) => void> = {
  viridis: viridisRamp,
  gray: grayRamp,
  jet: jetRamp,
};
