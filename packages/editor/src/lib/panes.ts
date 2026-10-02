// 三栏（算子面板 | 画布 | 右栏）怎么分一行的宽度。纯函数，Workspace 每次渲染算一遍。

export interface SidePane {
  /** 记住的宽度：拖出来的、localStorage 里的。 */
  width: number;
  /** 最窄能缩到多少。 */
  min: number;
}

/** 窗口（或宿主给的容器）窄到放不下「两侧面板 + 最窄的画布」时，两侧面板让位：右栏先缩到它的最窄，
 *  不够再缩左栏。只算显示出来的宽度 —— 记住的宽度不动，窗口拉回来面板回到原来的宽。
 *  以前两侧都不缩：1024 宽的窗口打开参数面板，画布挤成 0，面板右边一截跑到窗口外面点不着。
 *  两侧都到了最窄还放不下时就停在最窄，剩下的由画布让（桌面窗口最小 900，放得下）。
 *  total 不大于 0（还没量到）时原样返回。 */
export function fitSidePanes(total: number, left: SidePane, right: SidePane, canvasMin: number): { left: number; right: number } {
  const over = left.width + right.width + canvasMin - total;
  if (!(total > 0) || over <= 0) return { left: left.width, right: right.width };
  const fromRight = Math.max(0, Math.min(over, right.width - right.min));
  const fromLeft = Math.max(0, Math.min(over - fromRight, left.width - left.min));
  return { left: left.width - fromLeft, right: right.width - fromRight };
}
