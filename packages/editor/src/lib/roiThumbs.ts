// ROI 缩略图的底图范围（param-recipe P2.7）：参数面板的 ROI 行画一个小框示意「框在底图上的哪里」。
// 底图的包围盒只有 3D 视图取过那片云之后才知道，所以视图每显示一片云就在这里记一笔（按展开后的节点 id）；
// 没记过的节点，缩略图退回用同组几个框的并集当范围。纯 UI 旁路缓存，不进任何 store。

import { roiFramesOf } from "./roiFrames";
import type { GraphNode } from "../types/graph";
import type { OperatorDesc } from "../types/manifest";

/** [xMin, yMin, xMax, yMax]，米。 */
export type Rect = [number, number, number, number];

const bounds = new Map<string, Rect>();
let version = 0;
const listeners = new Set<() => void>();

/** 3D 视图显示了这个节点的云（或底图）：记下 XY 包围盒。bounds 是 [minXYZ, maxXYZ] 六个数。 */
export function rememberRoiBounds(fullNodeId: string, b: ArrayLike<number> | null): void {
  if (!b || b.length < 6) return;
  const r: Rect = [b[0]!, b[1]!, b[3]!, b[4]!];
  if (!r.every(Number.isFinite) || !(r[2] > r[0]) || !(r[3] > r[1])) return;
  const old = bounds.get(fullNodeId);
  if (old && old.every((v, i) => v === r[i])) return;
  bounds.set(fullNodeId, r);
  version += 1;
  for (const fn of listeners) fn();
}

/** 缩略图订阅（useSyncExternalStore）：视图取到一片新云，已经挂着的缩略图跟着换底图范围。 */
export function subscribeRoiBounds(fn: () => void): () => void {
  listeners.add(fn);
  return () => listeners.delete(fn);
}

export function roiBoundsVersion(): number {
  return version;
}

export function knownRoiBounds(fullNodeId: string): Rect | null {
  return bounds.get(fullNodeId) ?? null;
}

export interface ThumbBox {
  param: string;
  /** 米。 */
  rect: Rect;
  current: boolean;
}

export interface Thumb {
  /** 画布范围，米。 */
  extent: Rect;
  boxes: ThumbBox[];
  /** 这个参数属于哪一组框（lib/roiFrames 的 frame key），「进入拖框」时切到它。 */
  frame: string | null;
  /** 范围是不是来自真的底图（否则是几个框的并集）。 */
  fromCloud: boolean;
}

/** 某个 roi 参数的缩略图：同组可见的框都画，当前这个高亮。effective 是节点的完整有效值。 */
export function roiThumb(
  op: OperatorDesc,
  effective: Record<string, unknown>,
  param: string,
  fullNodeId: string,
): Thumb {
  const frames = roiFramesOf(op, { params: effective } as GraphNode);
  const frame = frames.find((f) => f.params.some((p) => p.name === param)) ?? null;
  const params = frame ? frame.params : op.params.filter((p) => p.name === param);
  const boxes: ThumbBox[] = [];
  for (const p of params) {
    const v = effective[p.name];
    if (!Array.isArray(v) || v.length !== 4) continue;
    const s = p.unit === "mm" ? 0.001 : 1;
    const r = (v as number[]).map((x) => Number(x) * s) as Rect;
    if (!(r[2] > r[0]) || !(r[3] > r[1])) continue;
    boxes.push({ param: p.name, rect: r, current: p.name === param });
  }
  const cloud = frame && frame.files.length === 0 ? knownRoiBounds(fullNodeId) : null;
  let extent: Rect;
  if (cloud) {
    extent = cloud;
  } else if (boxes.length > 0) {
    // 没有底图：几个框的并集往外放 40%，框不贴边
    const u: Rect = [
      Math.min(...boxes.map((b) => b.rect[0])),
      Math.min(...boxes.map((b) => b.rect[1])),
      Math.max(...boxes.map((b) => b.rect[2])),
      Math.max(...boxes.map((b) => b.rect[3])),
    ];
    const px = (u[2] - u[0]) * 0.4;
    const py = (u[3] - u[1]) * 0.4;
    extent = [u[0] - px, u[1] - py, u[2] + px, u[3] + py];
  } else {
    extent = [-1, -1, 1, 1];
  }
  return { extent, boxes, frame: frame?.key ?? null, fromCloud: cloud !== null };
}
