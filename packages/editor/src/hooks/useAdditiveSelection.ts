// Ctrl+Shift 拖框追加到已有选中，Ctrl 拖框替换选区。

import { useEffect, useRef, type RefObject } from "react";

import { useUiStore } from "../store/ui";

export interface KeptSelection {
  nodes: ReadonlySet<string>;
  edges: ReadonlySet<string>;
}

/** 追加框选期间保留原选区。 */
export function useAdditiveSelection(wrapper: RefObject<HTMLElement | null>): RefObject<KeptSelection | null> {
  const kept = useRef<KeptSelection | null>(null);

  useEffect(() => {
    const el = wrapper.current;
    if (!el) return;
    // 捕获阶段：赶在 React Flow 在画布上开框（它也在捕获阶段）之前
    const down = (e: PointerEvent) => {
      const onPane = e.target instanceof Element && e.target.classList.contains("react-flow__pane");
      if (e.button !== 0 || !onPane || !e.shiftKey || !(e.ctrlKey || e.metaKey)) {
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
