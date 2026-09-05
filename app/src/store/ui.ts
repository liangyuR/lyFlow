// UI store —— 选中、视口、搜索面板、剪贴板这些**不该进撤销栈、不该进文件**的
// 状态（ADR-0002）。混进 GraphDoc 会同时污染文件格式和撤销栈。

import { create } from "zustand";

import type { GraphDoc, GraphNode, PortRef } from "../types/graph";

export interface SearchPopup {
  /** 屏幕坐标，用来定位弹层 */
  screen: { x: number; y: number };
  /** 画布坐标，用来放置新节点 */
  flow: { x: number; y: number };
  /** 从某个端口拖出连线、中途松手弹出的搜索面板（交互清单 P1 #18）。
   *  选中算子后会自动把新节点接上去；M1 先把数据通路留好。 */
  pendingFrom?: PortRef;
}

export interface Clipboard {
  nodes: GraphNode[];
  edges: GraphDoc["edges"];
}

interface UiState {
  selectedNodes: ReadonlySet<string>;
  selectedEdges: ReadonlySet<string>;
  searchPopup: SearchPopup | null;
  clipboard: Clipboard | null;
  /** 短暂提示（连线被拒绝的原因、保存成功之类）。 */
  toast: { text: string; kind: "info" | "warn" } | null;
  /** 面板里高亮的算子，用于在没选中节点时展示算子说明。 */
  inspectedOperator: string | null;

  setSelection(nodes: readonly string[], edges: readonly string[]): void;
  clearSelection(): void;
  openSearch(popup: SearchPopup): void;
  closeSearch(): void;
  setClipboard(c: Clipboard): void;
  showToast(text: string, kind?: "info" | "warn"): void;
  hideToast(): void;
  setInspectedOperator(id: string | null): void;
}

function sameIds(a: ReadonlySet<string>, b: readonly string[]): boolean {
  if (a.size !== b.length) return false;
  for (const id of b) if (!a.has(id)) return false;
  return true;
}

export const useUiStore = create<UiState>((set, get) => ({
  selectedNodes: new Set(),
  selectedEdges: new Set(),
  searchPopup: null,
  clipboard: null,
  toast: null,
  inspectedOperator: null,

  setSelection(nodes, edges) {
    // 必须比对后再写：选中会流回画布、画布又回调 onSelectionChange，无条件
    // set 新 Set 会形成渲染死循环（见 README「踩过的坑」）。
    const prev = get();
    if (sameIds(prev.selectedNodes, nodes) && sameIds(prev.selectedEdges, edges)) return;
    set({ selectedNodes: new Set(nodes), selectedEdges: new Set(edges) });
  },
  clearSelection() {
    if (get().selectedNodes.size === 0 && get().selectedEdges.size === 0) return;
    set({ selectedNodes: new Set(), selectedEdges: new Set() });
  },
  openSearch(popup) {
    set({ searchPopup: popup });
  },
  closeSearch() {
    set({ searchPopup: null });
  },
  setClipboard(c) {
    set({ clipboard: c });
  },
  showToast(text, kind = "info") {
    set({ toast: { text, kind } });
  },
  hideToast() {
    set({ toast: null });
  },
  setInspectedOperator(id) {
    set({ inspectedOperator: id });
  },
}));
