//
// 图 store —— GraphDoc 是唯一真实数据源（ADR-0002）。
//
// 两条规则：
//
// 1. **改图只能走这里的语义化动作**，不能直接写 doc。动作的集合是封闭的：
//    addNode / deleteNodes / moveNodes / setParam / setNodeUi / connect /
//    disconnect / pasteNodes。这样撤销栈的每一步都对应一个用户能理解的操作，
//    而不是「节点数组第 3 项的 x 变了」。
//
// 2. **纯 UI 操作不进撤销栈**。画布平移缩放、选中变化都在 store/ui.ts 里。
//    按 Ctrl+Z 结果只是取消了一次选中，用户会觉得撤销坏了。
//
// 历史用整份 doc 快照，不用 patch。理由：immer 的结构共享让未改动的节点在
// 新旧快照之间共用同一份对象，几十个节点的图一次快照的增量只有被改动的那部分；
// 而 patch 的路径是基于数组下标的，删一个节点会让之前所有 patch 的下标失效，
// 要么维护稳定路径要么每次重算，复杂度换不来这点内存。
//

import { enablePatches, produce } from "immer";
import { create } from "zustand";

import { newDocId, newLocalId } from "../lib/ids";
import { pruneUnknownParams, sparseSet } from "../lib/params";
import { canConnect, type ConnectVerdict, type GraphContext } from "../lib/typecheck";
import { GRAPH_SCHEMA_VERSION, type GraphDoc, type GraphNode, type NodeUi, type PortRef } from "../types/graph";
import { useManifestStore } from "./manifest";

enablePatches();

const MAX_HISTORY = 100;

export interface HistoryEntry {
  label: string;
  doc: GraphDoc;
}

export function emptyDoc(): GraphDoc {
  return {
    schemaVersion: GRAPH_SCHEMA_VERSION,
    id: newDocId(),
    name: "未命名",
    nodes: [],
    edges: [],
    groups: [],
    subgraphs: {},
    x: {},
  };
}

/** 从 manifest store 取算子/类型索引。图 store 需要它来做连线校验。 */
function ctx(): GraphContext {
  const m = useManifestStore.getState();
  return { operatorsById: m.operatorsById, typesByName: m.typesByName };
}

function allIds(doc: GraphDoc): Set<string> {
  const s = new Set<string>();
  for (const n of doc.nodes) s.add(n.id);
  for (const e of doc.edges) s.add(e.id);
  return s;
}

export interface PasteResult {
  nodeIds: string[];
}

interface GraphState {
  doc: GraphDoc;
  filePath: string | null;
  dirty: boolean;

  past: HistoryEntry[];
  future: HistoryEntry[];

  /** 事务开始时的快照。null 表示当前没有进行中的事务。 */
  pendingSnapshot: GraphDoc | null;

  /** 最近一次被拒绝的操作原因，给 UI 弹提示用。 */
  lastRejection: string | null;

  // -- 事务 ---------------------------------------------------------------
  /** 拖动/滑块这类连续操作：开始时拍一张，结束时整体记一条撤销。 */
  begin(): void;
  commit(label: string): void;

  // -- 语义化动作 ---------------------------------------------------------
  addNode(opId: string, position: { x: number; y: number }): string | null;
  deleteNodes(ids: readonly string[]): void;
  moveNodes(moves: readonly { id: string; position: { x: number; y: number } }[]): void;
  setParam(nodeId: string, name: string, value: unknown): void;
  setNodeUi(nodeId: string, patch: Partial<NodeUi>): void;
  connect(from: PortRef, to: PortRef): ConnectVerdict;
  disconnect(edgeIds: readonly string[]): void;
  pasteNodes(payload: { nodes: GraphNode[]; edges: GraphDoc["edges"] }, at: { x: number; y: number }): PasteResult;

  // -- 历史 ---------------------------------------------------------------
  undo(): void;
  redo(): void;
  canUndo(): boolean;
  canRedo(): boolean;

  // -- 文档 ---------------------------------------------------------------
  newDoc(): void;
  loadDoc(doc: GraphDoc, path: string | null): void;
  markSaved(path: string): void;
  setName(name: string): void;
  clearRejection(): void;
}

export const useGraphStore = create<GraphState>((set, get) => {
  /** 记一条撤销，然后应用变更。用于单步操作。 */
  const transact = (label: string, recipe: (draft: GraphDoc) => void) => {
    const { doc, past } = get();
    const next = produce(doc, recipe);
    if (next === doc) return; // recipe 什么都没改，不要污染撤销栈
    set({
      doc: next,
      past: [...past, { label, doc }].slice(-MAX_HISTORY),
      future: [],
      dirty: true,
    });
  };

  /** 应用变更但不记撤销。用于事务进行中的中间状态（拖动的每一帧）。 */
  const mutate = (recipe: (draft: GraphDoc) => void) => {
    const { doc } = get();
    const next = produce(doc, recipe);
    if (next === doc) return;
    set({ doc: next, dirty: true });
  };

  return {
    doc: emptyDoc(),
    filePath: null,
    dirty: false,
    past: [],
    future: [],
    pendingSnapshot: null,
    lastRejection: null,

    begin() {
      // 已经在事务里就不要覆盖起点 —— 嵌套 begin 应当是幂等的
      if (get().pendingSnapshot) return;
      set({ pendingSnapshot: get().doc });
    },

    commit(label) {
      const { pendingSnapshot, doc, past } = get();
      if (!pendingSnapshot) return;
      if (pendingSnapshot === doc) {
        set({ pendingSnapshot: null }); // 拖了但没动，不记
        return;
      }
      set({
        past: [...past, { label, doc: pendingSnapshot }].slice(-MAX_HISTORY),
        future: [],
        pendingSnapshot: null,
        dirty: true,
      });
    },

    addNode(opId, position) {
      const op = ctx().operatorsById.get(opId);
      if (!op) {
        set({ lastRejection: `算子未注册：${opId}` });
        return null;
      }
      const id = newLocalId("n", allIds(get().doc));
      transact(`添加 ${op.label}`, (d) => {
        d.nodes.push({
          id,
          op: op.id,
          opVersion: op.version,
          params: {}, // 稀疏：默认值不落盘
          ui: { position },
        });
      });
      return id;
    },

    deleteNodes(ids) {
      if (ids.length === 0) return;
      const kill = new Set(ids);
      const label = ids.length === 1 ? "删除节点" : `删除 ${ids.length} 个节点`;
      transact(label, (d) => {
        d.nodes = d.nodes.filter((n) => !kill.has(n.id));
        // 删节点自动清理相连边（交互清单 P0 #5）
        d.edges = d.edges.filter((e) => !kill.has(e.from.node) && !kill.has(e.to.node));
      });
    },

    moveNodes(moves) {
      if (moves.length === 0) return;
      // 拖动过程中每帧都调，所以走 mutate 不记撤销；
      // 一次拖动的撤销由 begin/commit 包住整体记一条。
      mutate((d) => {
        for (const m of moves) {
          const node = d.nodes.find((n) => n.id === m.id);
          if (!node) continue;
          node.ui = { ...node.ui, position: m.position };
        }
      });
    },

    setParam(nodeId, name, value) {
      const { doc } = get();
      const node = doc.nodes.find((n) => n.id === nodeId);
      if (!node) return;
      const op = ctx().operatorsById.get(node.op);
      if (!op) return;

      const nextParams = sparseSet(op, node.params, name, value);
      const apply = (d: GraphDoc) => {
        const target = d.nodes.find((n) => n.id === nodeId);
        if (target) target.params = nextParams;
      };

      // 滑块拖动时 setParam 每帧都来，靠外层 begin/commit 合成一条撤销。
      if (get().pendingSnapshot) mutate(apply);
      else transact(`修改 ${op.label}.${name}`, apply);
    },

    setNodeUi(nodeId, patch) {
      transact("修改节点外观", (d) => {
        const node = d.nodes.find((n) => n.id === nodeId);
        if (node) node.ui = { ...node.ui, ...patch };
      });
    },

    connect(from, to) {
      const { doc } = get();
      const verdict = canConnect(ctx(), doc, from, to);
      if (!verdict.ok) {
        set({ lastRejection: verdict.reason });
        return verdict;
      }
      const id = newLocalId("e", allIds(doc));
      transact("连线", (d) => {
        d.edges.push({ id, from: { ...from }, to: { ...to } });
      });
      return verdict;
    },

    disconnect(edgeIds) {
      if (edgeIds.length === 0) return;
      const kill = new Set(edgeIds);
      transact(edgeIds.length === 1 ? "断开连线" : `断开 ${edgeIds.length} 条连线`, (d) => {
        d.edges = d.edges.filter((e) => !kill.has(e.id));
      });
    },

    pasteNodes(payload, at) {
      const taken = allIds(get().doc);
      const idMap = new Map<string, string>();
      const manifest = ctx().operatorsById;

      // 粘贴的节点整体平移到目标位置，保持相对布局
      const origin = payload.nodes.reduce(
        (acc, n) => ({
          x: Math.min(acc.x, n.ui?.position?.x ?? 0),
          y: Math.min(acc.y, n.ui?.position?.y ?? 0),
        }),
        { x: Infinity, y: Infinity },
      );
      const dx = Number.isFinite(origin.x) ? at.x - origin.x : 0;
      const dy = Number.isFinite(origin.y) ? at.y - origin.y : 0;

      const newNodes: GraphNode[] = [];
      for (const n of payload.nodes) {
        const op = manifest.get(n.op);
        if (!op) continue; // 剪贴板里的算子在当前 core 里不存在，跳过
        const id = newLocalId("n", taken);
        taken.add(id);
        idMap.set(n.id, id);
        newNodes.push({
          id,
          op: n.op,
          opVersion: n.opVersion ?? op.version,
          params: pruneUnknownParams(op, n.params),
          ui: {
            ...n.ui,
            position: {
              x: (n.ui?.position?.x ?? 0) + dx,
              y: (n.ui?.position?.y ?? 0) + dy,
            },
          },
        });
      }

      // 只保留两端都在选区内的边 —— 这就是「复制粘贴保留内部连线」
      const newEdges = payload.edges
        .filter((e) => idMap.has(e.from.node) && idMap.has(e.to.node))
        .map((e) => {
          const id = newLocalId("e", taken);
          taken.add(id);
          return {
            id,
            from: { node: idMap.get(e.from.node)!, port: e.from.port },
            to: { node: idMap.get(e.to.node)!, port: e.to.port },
          };
        });

      if (newNodes.length === 0) return { nodeIds: [] };

      transact(newNodes.length === 1 ? "粘贴节点" : `粘贴 ${newNodes.length} 个节点`, (d) => {
        d.nodes.push(...newNodes);
        d.edges.push(...newEdges);
      });
      return { nodeIds: newNodes.map((n) => n.id) };
    },

    undo() {
      const { past, future, doc } = get();
      const entry = past[past.length - 1];
      if (!entry) return;
      set({
        doc: entry.doc,
        past: past.slice(0, -1),
        future: [...future, { label: entry.label, doc }],
        dirty: true,
        pendingSnapshot: null,
      });
    },

    redo() {
      const { past, future, doc } = get();
      const entry = future[future.length - 1];
      if (!entry) return;
      set({
        doc: entry.doc,
        future: future.slice(0, -1),
        past: [...past, { label: entry.label, doc }],
        dirty: true,
        pendingSnapshot: null,
      });
    },

    canUndo: () => get().past.length > 0,
    canRedo: () => get().future.length > 0,

    newDoc() {
      set({
        doc: emptyDoc(),
        filePath: null,
        dirty: false,
        past: [],
        future: [],
        pendingSnapshot: null,
      });
    },

    loadDoc(doc, path) {
      set({
        doc,
        filePath: path,
        dirty: false,
        past: [],
        future: [],
        pendingSnapshot: null,
      });
    },

    markSaved(path) {
      set({ filePath: path, dirty: false });
    },

    setName(name) {
      transact("重命名", (d) => {
        d.name = name;
      });
    },

    clearRejection() {
      set({ lastRejection: null });
    },
  };
});
