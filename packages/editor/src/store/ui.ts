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

/** 参数面板的三个页签（param-recipe P2.2）。配方矩阵与配方管理在 P3 填内容。 */
export type ParamPanelTab = "nodes" | "matrix" | "recipes";

/** 3D 视图的相机（G7）。放在 store 里是因为参数面板的 ROI 行要能把它切到 2D 拖框（P2.7）。 */
export type ViewerMode = "3d" | "2d";

/** 鼠标指着的那条连线的两端（docs/motion-plan.md H3）。 */
export interface HoverEdge {
  id: string;
  from: PortRef;
  to: PortRef;
}

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

  pinnedNode: string | null;

  /** 拖线中的那一端。null = 没在拖（交互清单 P1 #20）。 */
  pendingFrom: PendingConnection | null;
  /** 与 pendingFrom 兼容的端口集合，键是 `nodeId:portName`。 */
  compatiblePorts: ReadonlySet<string>;

  /** 鼠标指着的节点（H2）：与它相连的边加亮、其余的淡下去。纯 UI 状态，不进 doc 也不进撤销（H5）。 */
  hoverNodeId: string | null;
  /** 鼠标指着的连线（H3）：两端的端口与节点跟着亮。 */
  hoverEdge: HoverEdge | null;
  /** 拖节点、框选期间暂停 hover 高亮（H2）：那时满屏淡化只会干扰，而且拖动的每一帧
   *  都会穿过别的节点，没必要让几百条边跟着来回切。拖连线看的是 pendingFrom。 */
  hoverPaused: boolean;
  setHoverNode(id: string | null): void;
  setHoverEdge(edge: HoverEdge | null): void;
  setHoverPaused(paused: boolean): void;

  /** 底部抽屉。null = 收起。 */
  drawer: DrawerTab | null;

  /** 参数面板（param-recipe P2.1）：开着时替代 Inspector；最大化时画布收起。宽度在 LyFlowEditor 里
   *  （拖分栏、记在 localStorage），不在这里。纯 UI 状态，不进 doc、不进撤销。 */
  paramPanel: { open: boolean; maximized: boolean; tab: ParamPanelTab; viewerOpen: boolean };
  /** open 不给就是开 ↔ 关。关掉时顺手还原最大化，下次打开不会一上来就看不见画布。 */
  toggleParamPanel(open?: boolean): void;
  setParamPanelMaximized(on: boolean): void;
  setParamPanelTab(tab: ParamPanelTab): void;
  /** 面板开着时 3D 视图收成一条标题栏，点开才展开（面板要竖向的地方）。 */
  setPanelViewerOpen(open: boolean): void;
  viewerMode: ViewerMode;
  setViewerMode(mode: ViewerMode): void;
  /** 快捷键面板开着没有（`?`）。 */
  helpOpen: boolean;
  /** 抽屉里点了某条诊断 → 定位到这个节点/参数。 */
  focusedDiagnostic: { nodeId: string; paramPath?: string | undefined } | null;

  /** 自动连线没能唯一确定的那些输入与它们的候选输出（m8-plan L13），键都是 `nodeId:portName`。
   *  端口据此高亮；手动连上、点空白处或下一次自动连线时清掉。null = 没有。 */
  autoHint: { targets: ReadonlySet<string>; candidates: ReadonlySet<string> } | null;
  setAutoHint(ambiguous: readonly { to: PortRef; candidates: readonly PortRef[] }[]): void;
  clearAutoHint(): void;

  /** 每个节点在 2D 视图里选的那一组框（m8-plan L20，键是 lib/roiFrames 的 frame key）。
   *  切换条与 Inspector 的槽参数组共用它；不进 doc、不进撤销。没选过 = 第一组。 */
  roiFrame: Readonly<Record<string, string>>;
  setRoiFrame(nodeId: string, key: string): void;

  setSelection(nodes: readonly string[], edges: readonly string[]): void;
  clearSelection(): void;
  openSearch(popup: SearchPopup): void;
  closeSearch(): void;
  setClipboard(c: Clipboard): void;
  showToast(text: string, kind?: "info" | "warn"): void;
  hideToast(): void;
  setInspectedOperator(id: string | null): void;
  setPinnedNode(id: string | null): void;
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
  pinnedNode: null,
  pendingFrom: null,
  compatiblePorts: NO_PORTS,
  hoverNodeId: null,
  hoverEdge: null,
  hoverPaused: false,
  drawer: null,
  paramPanel: { open: false, maximized: false, tab: "nodes", viewerOpen: false },
  viewerMode: "3d",
  helpOpen: false,
  focusedDiagnostic: null,
  autoHint: null,
  roiFrame: {},

  // 三个 hover setter 都先比对再写：mouseenter 会连发，而每次写入都会让所有连线的
  // selector 跑一遍（与 setSelection 同一个道理）。
  setHoverNode(id) {
    if (get().hoverNodeId === id) return;
    set({ hoverNodeId: id });
  },
  setHoverEdge(edge) {
    const cur = get().hoverEdge;
    if (cur === edge || (cur && edge && cur.id === edge.id)) return;
    set({ hoverEdge: edge });
  },
  setHoverPaused(paused) {
    if (get().hoverPaused === paused) return;
    // 暂停时顺手清掉：拖完松手时鼠标多半已经不在原来那个节点上了
    set(paused ? { hoverPaused: true, hoverNodeId: null, hoverEdge: null } : { hoverPaused: false });
  },

  setRoiFrame(nodeId, key) {
    if (get().roiFrame[nodeId] === key) return;
    set({ roiFrame: { ...get().roiFrame, [nodeId]: key } });
  },

  setAutoHint(ambiguous) {
    if (ambiguous.length === 0) {
      if (get().autoHint) set({ autoHint: null });
      return;
    }
    const targets = new Set<string>();
    const candidates = new Set<string>();
    for (const a of ambiguous) {
      targets.add(`${a.to.node}:${a.to.port}`);
      for (const c of a.candidates) candidates.add(`${c.node}:${c.port}`);
    }
    set({ autoHint: { targets, candidates } });
  },
  clearAutoHint() {
    if (get().autoHint) set({ autoHint: null });
  },

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
  setPinnedNode(id) {
    if (get().pinnedNode === id) return;
    set({ pinnedNode: id });
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
  toggleParamPanel(open) {
    const cur = get().paramPanel;
    const next = open ?? !cur.open;
    if (next === cur.open) return;
    set({ paramPanel: { ...cur, open: next, maximized: next ? cur.maximized : false } });
  },
  setParamPanelMaximized(on) {
    const cur = get().paramPanel;
    if (cur.maximized === on) return;
    set({ paramPanel: { ...cur, maximized: on } });
  },
  setParamPanelTab(tab) {
    const cur = get().paramPanel;
    if (cur.tab === tab) return;
    set({ paramPanel: { ...cur, tab } });
  },
  setPanelViewerOpen(open) {
    const cur = get().paramPanel;
    if (cur.viewerOpen === open) return;
    set({ paramPanel: { ...cur, viewerOpen: open } });
  },
  setViewerMode(mode) {
    if (get().viewerMode === mode) return;
    set({ viewerMode: mode });
  },
  focusDiagnostic(nodeId, paramPath) {
    set({ focusedDiagnostic: { nodeId, paramPath }, selectedNodes: new Set([nodeId]) });
  },

  enterSubgraph(segment) {
    set({
      path: [...get().path, segment],
      selectedNodes: new Set(),
      selectedEdges: new Set(),
      // 换了一层：指着的那个节点/连线不在这一层了，mouseleave 也不会再来
      hoverNodeId: null,
      hoverEdge: null,
    });
  },
  exitTo(depth) {
    const path = get().path;
    if (depth >= path.length) return;
    set({
      path: path.slice(0, depth),
      selectedNodes: new Set(),
      selectedEdges: new Set(),
      hoverNodeId: null,
      hoverEdge: null,
    });
  },
  setPath(path) {
    set({ path, selectedNodes: new Set(), selectedEdges: new Set(), hoverNodeId: null, hoverEdge: null });
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
