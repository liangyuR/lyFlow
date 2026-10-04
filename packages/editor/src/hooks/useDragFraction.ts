// 上下拖的分隔把手：右栏里预览与检查器（参数面板）之间、底部抽屉的上沿。记的是那一块占所在那一列的比例 ——
// 窗口拉高拉矮比例不变，不会出现「记住的 500 px 在矮窗口里把下面那块挤没」。

import { useCallback, useState } from "react";

import { readStoredNumber, removeStored, writeStoredNumber } from "../lib/prefs";
import { rootOf } from "../lib/root";

export interface DragFractionOptions {
  /** 比例相对它的高度算：那一整列（右栏、整个编辑器）。 */
  column: () => HTMLElement | null;
  /** 被拖的那一块：从它现在的高度起算。 */
  pane: () => HTMLElement | null;
  /** 那一块贴着列的哪一边：预览贴顶（往下拖变高），抽屉贴底（往上拖变高）。 */
  edge: "top" | "bottom";
  /** 这一块至少多高、列里剩下的至少多高。 */
  minPx: number;
  restMinPx: number;
  /** 松手时记进 localStorage 的键。 */
  persistKey: string;
}

/** fraction 是 null 时用样式表里的默认（没拖过）；拖过之后是记住的比例。 */
export function useDragFraction({ column, pane, edge, minPx, restMinPx, persistKey }: DragFractionOptions) {
  const [fraction, setFraction] = useState<number | null>(() => {
    const v = readStoredNumber(persistKey);
    return v !== null && v > 0 && v < 1 ? v : null;
  });

  const onPointerDown = useCallback(
    (e: React.PointerEvent) => {
      const col = column();
      const box = pane();
      if (e.button !== 0 || !col || !box) return;
      const total = col.getBoundingClientRect().height;
      if (total <= 0) return;
      e.preventDefault();
      const startY = e.clientY;
      const startPx = box.getBoundingClientRect().height;
      const root = rootOf(col);
      root?.classList.add("lyflow-is-resizing", "lyflow-is-resizing--rows");
      // 只按一下不拖就什么都不记：没拖过的保持样式表的默认（比如对比时预览更高的那一档）
      let latest: number | null = null;
      const move = (ev: PointerEvent) => {
        const dy = ev.clientY - startY;
        const px = edge === "top" ? startPx + dy : startPx - dy;
        latest = Math.max(minPx, Math.min(total - restMinPx, px)) / total;
        setFraction(latest);
      };
      const up = () => {
        window.removeEventListener("pointermove", move);
        window.removeEventListener("pointerup", up);
        window.removeEventListener("pointercancel", up);
        root?.classList.remove("lyflow-is-resizing", "lyflow-is-resizing--rows");
        if (latest !== null) writeStoredNumber(persistKey, Math.round(latest * 1000) / 1000);
      };
      window.addEventListener("pointermove", move);
      window.addEventListener("pointerup", up);
      window.addEventListener("pointercancel", up);
    },
    [column, pane, edge, minPx, restMinPx, persistKey],
  );

  /** 双击把手：回到样式表的默认，忘掉记住的比例。 */
  const reset = useCallback(() => {
    setFraction(null);
    removeStored(persistKey);
  }, [persistKey]);

  return { fraction, onPointerDown, reset };
}
