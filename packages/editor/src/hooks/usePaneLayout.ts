// 编辑器的三栏（算子面板 | 画布 | 右栏）与右栏里预览的高度：拖分栏、记住宽度、窗口窄了两侧让位（lib/panes.ts）。
// 原来都写在 Workspace 里，和打开、保存、执行事件那些搅在一起。

import { useCallback, useEffect, useLayoutEffect, useRef, useState, type RefObject } from "react";

import { fitSidePanes } from "../lib/panes";
import { readStoredNumber, writeStoredNumber } from "../lib/prefs";
import { rootOf } from "../lib/root";
import { useDragFraction } from "./useDragFraction";

const kMinCanvasWidth = 320;
/** 两条分栏把手一共占的宽（styles.css 的 .app__splitter：右边那条净占 4px，左边那条压在面板边框上）。 */
const kSplittersWidth = 4;
const kPaletteMin = 180;
const kInspectorMin = 260;
const kPanelMin = 360;
/** 参数面板宽度记在 localStorage 的这个键下（P2.1「记住宽度」）。 */
const PANEL_WIDTH_KEY = "lyflow.paramPanel.width";
/** 左侧算子面板的宽度，同样拖了就记住。 */
const PALETTE_WIDTH_KEY = "lyflow.palette.width";
/** 右栏里预览占多高（占右栏的比例）：检查器模式与参数面板模式各一个。没拖过时用样式表的默认。 */
const VIEWER_FRACTION_KEY = "lyflow.viewer.fraction";
const PANEL_VIEWER_FRACTION_KEY = "lyflow.viewer.fraction.panel";

/** 记住的宽度（localStorage）。读不到、存的不是正数都退回默认 —— 这只是个方便，不是状态。 */
function storedWidth(key: string | undefined, fallback: number): number {
  const v = key ? readStoredNumber(key) : null;
  return v !== null && v > 0 ? v : fallback;
}

/** 可拖分栏（右侧面板、左侧算子面板）。一条 4px 把手 + 全局 pointermove，
 *  不引分栏库 —— 一个库的成本是十几 KB 加一套 API，这里只要一个数字。
 *  给了 persistKey 就在松手时把宽度记进 localStorage，下次打开还是这个宽（参数面板，P2.1）。
 *  reserve 是拖动时要给其余部分留的宽度：左右两栏互相限制，所以在拖的那一刻才取。 */
function useDragSplit(
  initial: number,
  min: number,
  max: number,
  container: RefObject<HTMLElement | null>,
  reserve: () => number,
  persistKey?: string,
  side: "left" | "right" = "right",
) {
  const [width, setWidth] = useState(() => Math.max(min, Math.min(max, storedWidth(persistKey, initial))));
  const dragging = useRef(false);
  const latest = useRef(width);
  latest.current = width;

  useEffect(() => {
    const move = (e: PointerEvent) => {
      if (!dragging.current) return;
      const rect = container.current?.getBoundingClientRect();
      const limit = rect ? Math.max(min, Math.min(max, rect.width - reserve())) : max;
      const next = side === "left" ? e.clientX - (rect ? rect.left : 0) : (rect ? rect.right : window.innerWidth) - e.clientX;
      setWidth(Math.max(min, Math.min(limit, next)));
    };
    const up = () => {
      if (dragging.current && persistKey) writeStoredNumber(persistKey, Math.round(latest.current));
      dragging.current = false;
      rootOf(container.current)?.classList.remove("lyflow-is-resizing");
    };
    window.addEventListener("pointermove", move);
    window.addEventListener("pointerup", up);
    return () => {
      window.removeEventListener("pointermove", move);
      window.removeEventListener("pointerup", up);
    };
  }, [container, min, max, reserve, persistKey, side]);

  const onPointerDown = useCallback(() => {
    dragging.current = true;
    // 挂在这个编辑器的根上而不是 body：同一页面的别的东西不该跟着禁掉指针事件
    rootOf(container.current)?.classList.add("lyflow-is-resizing");
  }, [container]);

  return { width, onPointerDown };
}

/** 编辑器这一行有多宽（跟着窗口 / 宿主容器变），给 fitSidePanes 用。只在窄到放不下 wanted 时才让 Workspace 重画：
 *  宽的时候窗口怎么拖都不重渲染整棵树。量到的值放在 ref 里，面板开关、拖分栏引起的重渲染照样读到最新的。 */
function useRowWidth(container: RefObject<HTMLElement | null>, wanted: number): number {
  const width = useRef(0);
  const wantedRef = useRef(wanted);
  wantedRef.current = wanted;
  const [, setTight] = useState<number | null>(null);
  useLayoutEffect(() => {
    const el = container.current;
    if (!el) return;
    const measure = () => {
      width.current = el.getBoundingClientRect().width;
      setTight(width.current < wantedRef.current ? width.current : null);
    };
    measure();
    if (typeof ResizeObserver === "undefined") return;
    const observer = new ResizeObserver(measure);
    observer.observe(el);
    return () => observer.disconnect();
  }, [container]);
  return width.current;
}

export interface PaneLayout {
  paletteWidth: number;
  rightWidth: number;
  onPaletteSplitterDown: () => void;
  onRightSplitterDown: () => void;
  /** 挂到右栏与预览上：上下拖时按它们量。 */
  rightCol: RefObject<HTMLElement>;
  viewerBox: RefObject<HTMLDivElement>;
  viewer: {
    /** 参数面板开着、预览收起时没有把手。 */
    shown: boolean;
    /** null = 没拖过，用样式表的默认。 */
    fraction: number | null;
    onPointerDown: (e: React.PointerEvent) => void;
    reset: () => void;
  };
}

/** root 是编辑器的根：分栏按它的宽度算。panel 是参数面板的开关（它与 Inspector 各记各的宽度和预览高度）。 */
export function usePaneLayout(
  root: RefObject<HTMLElement | null>,
  panel: { open: boolean; viewerOpen: boolean },
): PaneLayout {
  // 三栏互相限制：拖哪一栏都给另外两栏（画布至少 kMinCanvasWidth）留够地方。宽度在拖的那一刻从 ref 取
  const widths = useRef({ palette: 0, right: 0 });
  const reserveForPalette = useCallback(() => widths.current.right + kMinCanvasWidth + kSplittersWidth, []);
  const reserveForRight = useCallback(() => widths.current.palette + kMinCanvasWidth + kSplittersWidth, []);
  const palettePane = useDragSplit(280, kPaletteMin, 640, root, reserveForPalette, PALETTE_WIDTH_KEY, "left");
  const rightPane = useDragSplit(380, kInspectorMin, 900, root, reserveForRight);
  // 参数面板（param-recipe P2.1）另有一份宽度：它比 Inspector 宽得多，两者来回切时各记各的
  const panelPane = useDragSplit(640, kPanelMin, 1800, root, reserveForRight, PANEL_WIDTH_KEY);
  const leftWanted = { width: palettePane.width, min: kPaletteMin };
  const rightWanted = panel.open ? { width: panelPane.width, min: kPanelMin } : { width: rightPane.width, min: kInspectorMin };
  const rowWidth = useRowWidth(root, leftWanted.width + rightWanted.width + kMinCanvasWidth + kSplittersWidth);
  // 窗口窄了两侧先让（右栏先缩），画布至少留 kMinCanvasWidth；拖分栏时的上限也按让过之后的宽算
  const fitted = fitSidePanes(rowWidth - kSplittersWidth, leftWanted, rightWanted, kMinCanvasWidth);
  widths.current = { palette: fitted.left, right: fitted.right };

  // 右栏里预览与下面（检查器 / 参数面板）之间可以上下拖，两种模式各记各的比例（参数面板开着时下面要的地方多）
  const rightCol = useRef<HTMLElement>(null);
  const viewerBox = useRef<HTMLDivElement>(null);
  const getRightCol = useCallback(() => rightCol.current, []);
  const getViewerBox = useCallback(() => viewerBox.current, []);
  const inspectorViewer = useDragFraction({
    column: getRightCol, pane: getViewerBox, edge: "top", minPx: 160, restMinPx: 160, persistKey: VIEWER_FRACTION_KEY,
  });
  const panelViewer = useDragFraction({
    column: getRightCol, pane: getViewerBox, edge: "top", minPx: 160, restMinPx: 200, persistKey: PANEL_VIEWER_FRACTION_KEY,
  });
  const viewerSplit = panel.open ? panelViewer : inspectorViewer;

  return {
    paletteWidth: fitted.left,
    rightWidth: fitted.right,
    onPaletteSplitterDown: palettePane.onPointerDown,
    onRightSplitterDown: panel.open ? panelPane.onPointerDown : rightPane.onPointerDown,
    rightCol,
    viewerBox,
    viewer: {
      shown: !(panel.open && !panel.viewerOpen),
      fraction: viewerSplit.fraction,
      onPointerDown: viewerSplit.onPointerDown,
      reset: viewerSplit.reset,
    },
  };
}
