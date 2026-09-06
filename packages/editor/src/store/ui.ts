// UI store —— 选中、视口、搜索面板、剪贴板这些**不该进撤销栈、不该进文件**的
// 状态（ADR-0002）。混进 GraphDoc 会同时污染文件格式和撤销栈。

import { create } from "zustand";

import type { PathSegment, SubPath } from "../lib/subgraph";
import type { GraphDoc, GraphNode, PortRef } from "../types/graph";

export interface SearchPopup {
  /** 屏幕坐标，用来定位弹层 */
  screen: { x: number; y: number };
  /** 画布坐标，用来放置新节点 */
  flow: { x: number; y: number };
  /** 从某个端口拖出连线、中途松手弹出的搜索面板（交互清单 P1 #18）。
   *  选中算子后会自动把新节点接上去；M1 先把数据通路留好。 */
  pendingFrom?: PortRef;
  /** pendingFrom 是它自己节点上的哪一侧。output = 新节点接在它下游。 */
  pendingSide?: "input" | "output";
}

export interface Clipboard {
  nodes: GraphNode[];
  edges: GraphDoc["edges"];
}

/** 正在拖出的那一端。side 是这一端在自己节点上的方向。 */
export interface PendingConnection {
  node: string;
  port: string;
  side: "input" | "output";
}

export type DrawerTab = "log" | "diagnostics" | "cache";

interface UiState {
  /** 当前所在的子图栈（F2）。空 = 顶层。纯导航状态，不进 doc 也不进撤销栈。 */
  path: SubPath;
  /** 自动运行：预览松手后自动补一次正式运行（ADR-0011）。 */
  autoRun: boolean;
  /** 预览点数上限。0 = 用 core 的默认值。 */
  previewMaxPoints: number;
  /** 正在拖参数：这期间发的是 preview run。 */
  previewing: boolean;
  selectedNodes: ReadonlySet<string>;
  selectedEdges: ReadonlySet<string>;
  searchPopup: SearchPopup | null;
  clipboard: Clipboard | null;
  /** 短暂提示（连线被拒绝的原因、保存成功之类）。 */
  toast: { text: string; kind: "info" | "warn" } | null;
  /** 面板里高亮的算子，用于在没选中节点时展示算子说明。 */
  inspectedOperator: string | null;

  /** 拖线中的那一端。null = 没在拖（交互清单 P1 #20）。 */
  pendingFrom: PendingConnection | null;
  /** 与 pendingFrom 兼容的端口集合，键是 `nodeId:portName`。 */
  compatiblePorts: ReadonlySet<string>;

  /** 底部抽屉。null = 收起。 */
  drawer: DrawerTab | null;
  /** 快捷键面板开着没有（`?`）。 */
  helpOpen: boolean;
  /** 抽屉里点了某条诊断 → 定位到这个节点/参数。 */
  focusedDiagnostic: { nodeId: string; paramPath?: string | undefined } | null;

  setSelection(nodes: readonly string[], edges: readonly string[]): void;
  clearSelection(): void;
  openSearch(popup: SearchPopup): void;
  closeSearch(): void;
  setClipboard(c: Clipboard): void;
  showToast(text: string, kind?: "info" | "warn"): void;
  hideToast(): void;
  setInspectedOperator(id: string | null): void;
  beginConnection(from: PendingConnection, compatible: ReadonlySet<string>): void;
  endConnection(): void;
  toggleDrawer(tab?: DrawerTab): void;
  setHelpOpen(open: boolean): void;
  focusDiagnostic(nodeId: string, paramPath?: string): void;

  /** 进入一个子图节点。选中会被清掉 —— 层级换了，旧的选中没有意义。 */
  enterSubgraph(segment: PathSegment): void;
  /** 退到第 depth 层（0 = 顶层）。 */
  exitTo(depth: number): void;
  setPath(path: SubPath): void;
  setAutoRun(on: boolean): void;
  setPreviewMaxPoints(n: number): void;
  setPreviewing(on: boolean): void;
}

const NO_PORTS: ReadonlySet<string> = new Set();

function sameIds(a: ReadonlySet<string>, b: readonly string[]): boolean {
  if (a.size !== b.length) return false;
  for (const id of b) if (!a.has(id)) return false;
  return true;
}

const NO_PATH: SubPath = [];

export const useUiStore = create<UiState>((set, get) => ({
  path: NO_PATH,
  autoRun: true,
  previewMaxPoints: 200_000,
  previewing: false,
  selectedNodes: new Set(),
  selectedEdges: new Set(),
  searchPopup: null,
  clipboard: null,
  toast: null,
  inspectedOperator: null,
  pendingFrom: null,
  compatiblePorts: NO_PORTS,
  drawer: null,
  helpOpen: false,
  focusedDiagnostic: null,

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
  beginConnection(from, compatible) {
    set({ pendingFrom: from, compatiblePorts: compatible });
  },
  endConnection() {
    if (!get().pendingFrom) return;
    set({ pendingFrom: null, compatiblePorts: NO_PORTS });
  },
  toggleDrawer(tab) {
    const current = get().drawer;
    // 点同一个页签是收起，点别的页签是切过去
    if (tab === undefined) set({ drawer: current ? null : "log" });
    else set({ drawer: current === tab ? null : tab });
  },
  setHelpOpen(open) {
    set({ helpOpen: open });
  },
  focusDiagnostic(nodeId, paramPath) {
    set({ focusedDiagnostic: { nodeId, paramPath }, selectedNodes: new Set([nodeId]) });
  },

  enterSubgraph(segment) {
    set({
      path: [...get().path, segment],
      selectedNodes: new Set(),
      selectedEdges: new Set(),
    });
  },
  exitTo(depth) {
    const path = get().path;
    if (depth >= path.length) return;
    set({
      path: path.slice(0, depth),
      selectedNodes: new Set(),
      selectedEdges: new Set(),
    });
  },
  setPath(path) {
    set({ path, selectedNodes: new Set(), selectedEdges: new Set() });
  },
  setAutoRun(on) {
    set({ autoRun: on });
  },
  setPreviewMaxPoints(n) {
    set({ previewMaxPoints: n });
  },
  setPreviewing(on) {
    if (get().previewing === on) return;
    set({ previewing: on });
  },
}));
