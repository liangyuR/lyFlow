// 右键拖动平移（中键与右键平移，左键留给框选：GraphCanvas 的 PAN_BUTTONS）。React Flow 的平移（d3-zoom）
// 只在空白处接右键：节点、连线上带 nopan，右键按在上面拖不动（中键它特意放行了，右键没有）。另外 Windows 上
// contextmenu 在右键**松开**时才发，松在哪个节点 / 连线上就弹哪个的菜单 —— 拖着平移完一松手，冒出来一个菜单。
// 这里：右键按在节点 / 连线上时自己平移；右键拖过几像素之后的那一次 contextmenu 吞掉（空白处、节点上都是）。

import { useReactFlow } from "@xyflow/react";
import { useEffect, type RefObject } from "react";

/** 右键按下后挪了超过这么多才算拖动；短于它是一次右键单击，照常弹菜单。 */
const DRAG_PX = 4;
/** 这些上面按下右键时自己平移；空白处（.react-flow__pane）d3-zoom 自己会。 */
const MANUAL_PAN_ON = ".react-flow__node, .react-flow__edge, .react-flow__edgeupdater, .react-flow__nodesselection";

export function useRightDragPan(wrapper: RefObject<HTMLElement | null>): void {
  const { getViewport, setViewport } = useReactFlow();

  useEffect(() => {
    const el = wrapper.current;
    if (!el) return;
    let gesture: { x: number; y: number; vx: number; vy: number; zoom: number; manual: boolean; moved: boolean } | null =
      null;
    let swallowMenu = false;

    const end = () => {
      if (gesture?.moved) swallowMenu = true;
      gesture = null;
      window.removeEventListener("pointermove", onMove, true);
      window.removeEventListener("pointerup", onUp, true);
      window.removeEventListener("pointercancel", onUp, true);
    };
    const onMove = (e: PointerEvent) => {
      if (!gesture) return;
      // 在窗口外面松的右键收不到 pointerup：按键状态里没有右键了就当它结束了
      if ((e.buttons & 2) === 0) {
        end();
        return;
      }
      const dx = e.clientX - gesture.x;
      const dy = e.clientY - gesture.y;
      if (!gesture.moved && Math.hypot(dx, dy) > DRAG_PX) gesture.moved = true;
      if (gesture.manual && gesture.moved) {
        void setViewport({ x: gesture.vx + dx, y: gesture.vy + dy, zoom: gesture.zoom });
      }
    };
    const onUp = (e: PointerEvent) => {
      if (e.button === 2 || (e.buttons & 2) === 0) end();
    };
    const onDown = (e: PointerEvent) => {
      if (e.button !== 2 || e.pointerType === "touch") return;
      swallowMenu = false;
      const target = e.target instanceof Element ? e.target : null;
      if (!target?.closest(".react-flow")) return;
      const vp = getViewport();
      gesture = {
        x: e.clientX,
        y: e.clientY,
        vx: vp.x,
        vy: vp.y,
        zoom: vp.zoom,
        manual: target.closest(MANUAL_PAN_ON) !== null,
        moved: false,
      };
      window.addEventListener("pointermove", onMove, true);
      window.addEventListener("pointerup", onUp, true);
      window.addEventListener("pointercancel", onUp, true);
    };
    // 捕获阶段、挂在画布外层：比 React Flow 节点 / 连线上的 onContextMenu（React 在根上冒泡时才分发）先到
    const onContextMenu = (e: MouseEvent) => {
      if (!swallowMenu && !gesture?.moved) return;
      swallowMenu = false;
      e.preventDefault();
      e.stopPropagation();
    };

    el.addEventListener("pointerdown", onDown, true);
    el.addEventListener("contextmenu", onContextMenu, true);
    return () => {
      el.removeEventListener("pointerdown", onDown, true);
      el.removeEventListener("contextmenu", onContextMenu, true);
      end();
      swallowMenu = false;
    };
  }, [wrapper, getViewport, setViewport]);
}
