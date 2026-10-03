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
