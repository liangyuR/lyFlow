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
