import { niceStep } from "./params";

// 换了一片云要不要重新取景。同一个坐标系里（两个包围盒相交、对角线差不到 4 倍）就不动相机：调参数重跑、
// 在链上逐个点节点时，视角连同双击设好的转心都留着；平移了 100 m、放大了 1000 倍、完全不相交的才重新取景。
// 不用「新云的中心在不在视野里」作判据：放大到云的一角时中心在视野外，照样会复位。

/** a、b 是 [minx, miny, minz, maxx, maxy, maxz]。 */
export function sameFrame(a: ArrayLike<number>, b: ArrayLike<number>): boolean {
  for (let i = 0; i < 3; i += 1) {
    if (a[i]! > b[i + 3]! || b[i]! > a[i + 3]!) return false;
  }
  const diag = (x: ArrayLike<number>) => Math.hypot(x[3]! - x[0]!, x[4]! - x[1]!, x[5]! - x[2]!);
  const da = diag(a);
  const db = diag(b);
  // 退化成一个点的云：两边都是点（又相交）才算同一个
  if (!(da > 0) || !(db > 0)) return da === db;
  const ratio = da / db;
  return ratio >= 0.25 && ratio <= 4;
}

// 标准视角（预览栏的 俯 / 前 / 侧 / 轴，鼠标在预览上按 1–4）：绕着现在的转心转过去、距离不变 ——
// 缩放与双击设好的转心都留着，画面不跳；要看全貌照旧按 ⤢。

export type ViewPreset = "top" | "front" | "side" | "iso";

/** ⤢ 取景（fitToBounds）的方向：斜 45°。「轴」也是它。 */
export const ISO_DIR: readonly [number, number, number] = [1, -1, 0.7];

/** 正上方往下看时视线与「上」（+Z）平行，lookAt 定不出屏幕的上方：往 −Y 偏一丝，屏幕上方就是 +Y（与 2D 剖面一样）。 */
const TILT = 1e-3;

const PRESET_DIRS: Record<ViewPreset, readonly [number, number, number]> = {
  top: [0, -Math.sin(TILT), Math.cos(TILT)],
  // 从 −Y 往 +Y 看：X 朝右、Z 朝上
  front: [0, -1, 0],
  // 从 +X 往 −X 看：Y 朝右、Z 朝上
  side: [1, 0, 0],
  iso: ISO_DIR,
};

/** 相机挪到哪：绕 target、离它的距离与现在一样。 */
export function presetPosition(
  view: ViewPreset,
  target: readonly [number, number, number],
  from: readonly [number, number, number],
): [number, number, number] {
  const d = Math.hypot(from[0] - target[0], from[1] - target[1], from[2] - target[2]) || 1;
  const dir = PRESET_DIRS[view];
  const n = Math.hypot(dir[0], dir[1], dir[2]);
  return [target[0] + (dir[0] / n) * d, target[1] + (dir[1] / n) * d, target[2] + (dir[2] / n) * d];
}

/** 快捷键 id → 视角。 */
export function presetOfShortcut(id: string): ViewPreset | null {
  return id === "viewTop" ? "top" : id === "viewFront" ? "front" : id === "viewSide" ? "side" : id === "viewIso" ? "iso" : null;
}

/** 快捷键按在哪个预览上：useShortcuts 往鼠标底下那个预览发这个事件，预览自己转过去（detail.done 置 true）。 */
export const VIEW_PRESET_EVENT = "lyflow:view-preset";

// 预览的网格跟着云走（以前固定 4 m、16 格、铺在原点）：格子取 1/2/5 × 10^n，让云的长边上有 10–25 格；
// 中心对齐到整格上、铺在云最低点的下面一丝。毫米级的小零件、x = 1000 m 的云都看得到网格，栏上写着一格多大。

export interface GridSpec {
  /** 一格多大（米）。 */
  cell: number;
  /** 一共几格（偶数：中心正好落在一条线上）。 */
  divisions: number;
  center: [number, number];
  /** 网格所在的高度：云的最低点往下一丝（不和最底下那层点重叠闪烁）。 */
  z: number;
  /** 栏上写的：「格 10 mm」。 */
  text: string;
}

export function gridText(cell: number): string {
  return cell < 1 ? `格 ${Number((cell * 1000).toPrecision(3))} mm` : `格 ${Number(cell.toPrecision(3))} m`;
}

/** b 是包围盒 [minx, miny, minz, maxx, maxy, maxz]。keepCell：上一次的格子 —— 没重新取景时（同一坐标系里重跑、换节点）
 *  只要云上还有 6–40 格就不换档，拖参数时网格不在两档之间来回跳。不是 6 个有限数返回 null。 */
export function gridSpec(b: ArrayLike<number>, keepCell: number | null = null): GridSpec | null {
  if (b.length < 6) return null;
  for (let i = 0; i < 6; i += 1) if (!Number.isFinite(b[i]!)) return null;
  const span = Math.max(b[3]! - b[0]!, b[4]! - b[1]!) || b[5]! - b[2]! || 0.01;
  const across = keepCell ? span / keepCell : 0;
  const cell = keepCell && across >= 6 && across <= 40 ? keepCell : niceStep(span / 10);
  const snap = (v: number) => Math.round(v / cell) * cell;
  const cx = snap((b[0]! + b[3]!) / 2);
  const cy = snap((b[1]! + b[4]!) / 2);
  const half = Math.max(b[3]! - cx, cx - b[0]!, b[4]! - cy, cy - b[1]!, 0);
  return {
    cell,
    divisions: 2 * (Math.ceil(half / cell - 1e-9) + 2),
    center: [cx, cy],
    z: b[2]! - cell * 1e-3,
    text: gridText(cell),
  };
}
