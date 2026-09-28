// 两节点输出并排对比的状态（交互清单 #35，docs/compare-plan.md §3.1）。
// A 跟随选中 / 钉住的节点，不在这里；这里只有 B —— 一个显式的槽，外加可选的冻结快照。
// 单独一个 store 而不塞进 ui：快照里挂着几十兆的 Float32Array，不该让每个订阅 ui 的组件跟着比对引用。

import { create } from "zustand";

import { levelOf, pathIsValid, type SubPath } from "../lib/subgraph";
import { useUiStore } from "./ui";
import type { BaseCloud } from "../lib/basecloud";
import type { ViewerContent } from "../lib/viewRule";
import type { CloudPayload, OutputStat } from "../types/execution";
import type { GraphDoc } from "../types/graph";

export interface CompareSlot {
  path: SubPath;
  /** 局部 id；取数时 fullId(path, nodeId)。 */
  nodeId: string;
}

/** B 冻结时存下来的东西。整份一起冻（与 Edge Peek §4.5 同一理由：只冻状态文字的话，
 *  重跑一开始 stats 就没了，冻结的 Measurement 立刻变空）。直接持有解码后的载荷，不靠 cloudCache。 */
export interface CompareSnapshot {
  runId: string;
  preview: boolean;
  label: string;
  content: ViewerContent;
  outputs: readonly OutputStat[] | undefined;
  cloud: CloudPayload | null;
  cloudPort: string | null;
  base: BaseCloud | null;
  maxPoints: number;
}

/** 把 B 此刻显示着的结果冻成快照。由 Viewer3D 挂上（它才知道画面上有什么）；
 *  还没结果、或 Viewer3D 没挂着时返回 null。 */
export type CompareCapture = (slot: CompareSlot) => CompareSnapshot | null;

export const COMPARE_NO_RESULT = "B 还没有结果，运行一次后可以冻结";
export const COMPARE_NO_NODE = "先选中一个节点再对比";
export const COMPARE_B_GONE = "对比基准节点已删除，已退出对比";

interface CompareState {
  on: boolean;
  b: CompareSlot | null;
  /** 非 null = 已冻结。 */
  snapshot: CompareSnapshot | null;
  capture: CompareCapture | null;

  enter(slot: CompareSlot, snapshot: CompareSnapshot | null): void;
  exit(): void;
  /** 工具栏按钮与 Ctrl+Shift+D：开着就退出；没开就以当前节点（钉住优先于选中）进入，
   *  当前节点已有结果就顺手冻住（C2）。 */
  toggle(): void;
  /** 换 B（右键「设为对比基准」）：没在对比就进入；换节点同时解冻，跟随最新。 */
  setB(slot: CompareSlot): void;
  freeze(snapshot: CompareSnapshot): void;
  unfreeze(): void;
  /** B 的节点没了（删除、撤销）就退出并提示。 */
  prune(doc: GraphDoc): void;
  setCapture(capture: CompareCapture | null): void;
}

/** 当前的 A：钉住优先于选中，与 Viewer3D 的 activeId 同一条规则。 */
export function activeSlot(): CompareSlot | null {
  const ui = useUiStore.getState();
  const selected = ui.selectedNodes.size === 1 ? [...ui.selectedNodes][0]! : null;
  const nodeId = ui.pinnedNode ?? selected;
  return nodeId ? { path: ui.path, nodeId } : null;
}

export const useCompareStore = create<CompareState>((set, get) => ({
  on: false,
  b: null,
  snapshot: null,
  capture: null,

  enter(slot, snapshot) {
    set({ on: true, b: slot, snapshot });
    // 参数面板开着且预览收起：先展开，否则进入了也看不见
    const ui = useUiStore.getState();
    if (ui.paramPanel.open && !ui.paramPanel.viewerOpen) ui.setPanelViewerOpen(true);
  },
  exit() {
    if (!get().on && get().b === null) return;
    set({ on: false, b: null, snapshot: null });
  },
  toggle() {
    if (get().on) {
      get().exit();
      return;
    }
    const slot = activeSlot();
    if (!slot) {
      useUiStore.getState().showToast(COMPARE_NO_NODE);
      return;
    }
    const snapshot = get().capture?.(slot) ?? null;
    get().enter(slot, snapshot);
    if (!snapshot) useUiStore.getState().showToast(COMPARE_NO_RESULT);
  },
  setB(slot) {
    if (!get().on) {
      get().enter(slot, null);
      return;
    }
    set({ b: slot, snapshot: null });
  },
  freeze(snapshot) {
    if (!get().on) return;
    set({ snapshot });
  },
  unfreeze() {
    if (get().snapshot) set({ snapshot: null });
  },
  prune(doc) {
    const b = get().b;
    if (!b) return;
    const alive = pathIsValid(doc, b.path) && levelOf(doc, b.path).nodes.some((n) => n.id === b.nodeId);
    if (alive) return;
    get().exit();
    useUiStore.getState().showToast(COMPARE_B_GONE);
  },
  setCapture(capture) {
    set({ capture });
  },
}));
