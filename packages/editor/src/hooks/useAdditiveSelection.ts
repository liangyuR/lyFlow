// 按着 Shift / Ctrl 在空白处拖框：框住的加进已有的选中，而不是换掉 —— 与按着它们点选一致（GraphCanvas 的
// MULTI_SELECTION_KEYS）。React Flow 的框选总是替换：开框时 resetSelectedElements 清空，框一变就按框里的重设。
// 这里在按下的那一刻记住当时的选中，到松手为止 React Flow 发来的「取消选中它们」都不认（GraphCanvas 的
// applySelectChanges）。顺带按着 Shift / Ctrl 单击空白处也不再清掉选中。

import { useEffect, useRef, type RefObject } from "react";

import { useUiStore } from "../store/ui";

export interface KeptSelection {
  nodes: ReadonlySet<string>;
  edges: ReadonlySet<string>;
}

/** 返回的 ref 在按着 Shift / Ctrl 按下空白处到松手之间是那一刻的选中，其余时候是 null。 */
export function useAdditiveSelection(wrapper: RefObject<HTMLElement | null>): RefObject<KeptSelection | null> {
  const kept = useRef<KeptSelection | null>(null);

  useEffect(() => {
    const el = wrapper.current;
    if (!el) return;
    // 捕获阶段：赶在 React Flow 在画布上开框（它也在捕获阶段）之前
    const down = (e: PointerEvent) => {
      const onPane = e.target instanceof Element && e.target.classList.contains("react-flow__pane");
      if (e.button !== 0 || !onPane || !(e.shiftKey || e.ctrlKey)) {
        kept.current = null;
        return;
      }
      const ui = useUiStore.getState();
      kept.current = { nodes: new Set(ui.selectedNodes), edges: new Set(ui.selectedEdges) };
    };
    // 冒泡阶段、挂在 window 上：React Flow 在根容器上处理完松手（框选收尾、单击空白处清选中）之后才放手
    const up = () => {
      kept.current = null;
    };
    el.addEventListener("pointerdown", down, true);
    window.addEventListener("pointerup", up);
    window.addEventListener("pointercancel", up);
    return () => {
      el.removeEventListener("pointerdown", down, true);
      window.removeEventListener("pointerup", up);
      window.removeEventListener("pointercancel", up);
    };
  }, [wrapper]);

  return kept;
}
