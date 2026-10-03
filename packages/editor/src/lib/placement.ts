// 按鼠标位置弹出的浮层（右键菜单）怎么摆进窗口：放不下就翻到鼠标的另一边，比窗口还高就贴顶、限高（里面滚）。

export interface Placement {
  left: number;
  top: number;
  /** 比窗口还高时给的高度上限；放得下时没有。 */
  maxHeight?: number;
}

/** at 是鼠标位置，size 是浮层不限高时的大小，viewport 是窗口的宽高。离窗口边至少留 margin。 */
export function placeMenu(
  at: { x: number; y: number },
  size: { width: number; height: number },
  viewport: { width: number; height: number },
  margin = 4,
): Placement {
  const room = viewport.height - 2 * margin;
  const tall = size.height > room;
  const height = tall ? room : size.height;
  // 右边放不下就翻到鼠标左边，上下同理；翻过去还放不下就贴着窗口边
  let left = at.x + size.width > viewport.width - margin ? at.x - size.width : at.x;
  left = Math.max(margin, Math.min(left, viewport.width - margin - size.width));
  let top = at.y + height > viewport.height - margin ? at.y - height : at.y;
  top = Math.max(margin, Math.min(top, viewport.height - margin - height));
  return tall ? { left, top, maxHeight: room } : { left, top };
}

/** 把一个节点挪进画布视野要平移多少（键盘沿连线走时用）：已经在视野里（减去四边留白）返回 null；否则返回新的视野中心
 *  （flow 坐标，交给 setCenter，缩放不变），只挪最少的那一点 —— 走一条长链时画面不来回跳。某一边放不下这个节点就让它居中。
 *  view 是 React Flow 的视口（屏幕 = flow × zoom + (x, y)），pane 是画布的宽高，rect 是节点的 flow 坐标与大小。 */
export function revealShift(
  view: { x: number; y: number; zoom: number },
  pane: { width: number; height: number },
  rect: { x: number; y: number; w: number; h: number },
  inset: { top: number; right: number; bottom: number; left: number },
): { x: number; y: number } | null {
  const z = view.zoom;
  const axis = (start: number, size: number, lo: number, hi: number) => {
    if (size > hi - lo) return (lo + hi) / 2 - (start + size / 2);
    if (start < lo) return lo - start;
    if (start + size > hi) return hi - (start + size);
    return 0;
  };
  const dx = axis(rect.x * z + view.x, rect.w * z, inset.left, pane.width - inset.right);
  const dy = axis(rect.y * z + view.y, rect.h * z, inset.top, pane.height - inset.bottom);
  if (dx === 0 && dy === 0) return null;
  return { x: (pane.width / 2 - (view.x + dx)) / z, y: (pane.height / 2 - (view.y + dy)) / z };
}

/** 画布坐标下的矩形（节点的位置与大小）。 */
export interface FlowRect {
  x: number;
  y: number;
  w: number;
  h: number;
}

/** 选中一个节点按 Tab、从它接出的新节点放在哪：它右边隔一段、同一高度；那里已经有节点（它接着的下游、别的分支）
 *  就往下让到不压着谁 —— 新节点是并出来的一条分支，不插进原来那条链。新节点的大小不给就按选中的这个估。 */
export function branchSlot(
  anchor: FlowRect,
  others: readonly FlowRect[],
  gap: { x: number; y: number } = { x: 80, y: 40 },
  size: { w?: number; h?: number } = {},
): { x: number; y: number } {
  const w = size.w ?? anchor.w;
  const h = size.h ?? anchor.h;
  const x = anchor.x + anchor.w + gap.x;
  let y = anchor.y;
  for (let i = 0; i < 100; i += 1) {
    const hit = others.filter((o) => o.x < x + w && x < o.x + o.w && o.y < y + h && y < o.y + o.h);
    if (hit.length === 0) break;
    y = Math.max(...hit.map((o) => o.y + o.h)) + gap.y;
  }
  return { x, y };
}

/** 还没放下的节点大概多高：标题栏、上下留白，再加上端口那几行（左边输入、右边输出，取多的那一边）。
 *  与样式表里 .node__head / .node__body 的尺寸对得上（一行端口的节点量出来是 68）。 */
export function estimateNodeHeight(inputs: number, outputs: number): number {
  return 46 + 22 * Math.max(1, inputs, outputs);
}
