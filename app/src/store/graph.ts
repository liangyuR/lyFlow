// 图 store —— GraphDoc 是唯一真实数据源（ADR-0002）：改图只能走这里封闭的那组
// 语义化动作，纯 UI 操作不进撤销栈，历史存整份 doc 快照而非 patch（见 README）。

import { enablePatches, produce } from "immer";
import { create } from "zustand";

import { newDocId, newLocalId } from "../lib/ids";
import { pruneUnknownParams, sparseSet } from "../lib/params";
import { canConnect, type ConnectVerdict, type GraphContext } from "../lib/typecheck";
import type { MigrationAction } from "../types/execution";
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

  /** 静音（交互清单 P1 #25）。是执行语义，所以进 doc、进撤销栈。 */
  setBypass(ids: readonly string[], value: boolean): void;
  /** 折叠：只显示标题与已连端口。纯 UI，但存进文件里下次打开还在。 */
  setCollapsed(ids: readonly string[], value: boolean): void;
  renameNode(id: string, title: string | null): void;
  /** 把一条边改接到别的输入端口（#19 拖离输入端后重新落点）。 */
  reconnectEdge(edgeId: string, to: PortRef): ConnectVerdict;
  /** 把一个节点插到一条边中间（#21 / #24 双击连线）。 */
  insertOnEdge(edgeId: string, nodeId: string, inPort: string, outPort: string): boolean;
  /** 自动布局的落点。整段算一条撤销记录（E8）。 */
  applyLayout(moves: readonly { id: string; position: { x: number; y: number } }[]): void;
  /** 原地复制选中节点（Ctrl+D）。 */
  duplicateNodes(ids: readonly string[]): PasteResult;
  /** 把 C++ 给的迁移动作写回 doc（ADR-0008）。返回真正改动的节点数。 */
  applyMigrations(actions: readonly MigrationAction[]): number;

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

    setBypass(ids, value) {
      if (ids.length === 0) return;
      const target = new Set(ids);
      const label = value
        ? ids.length === 1 ? "静音节点" : `静音 ${ids.length} 个节点`
        : ids.length === 1 ? "取消静音" : `取消静音 ${ids.length} 个节点`;
      transact(label, (d) => {
        for (const n of d.nodes) {
          if (!target.has(n.id)) continue;
          // 稀疏存储：false 就把键删掉，老图的 diff 不该被默认值污染
          if (value) n.bypass = true;
          else delete n.bypass;
        }
      });
    },

    setCollapsed(ids, value) {
      if (ids.length === 0) return;
      const target = new Set(ids);
      transact(value ? "折叠节点" : "展开节点", (d) => {
        for (const n of d.nodes) {
          if (!target.has(n.id)) continue;
          n.ui = { ...n.ui, collapsed: value };
        }
      });
    },

    renameNode(id, title) {
      transact("重命名节点", (d) => {
        const node = d.nodes.find((n) => n.id === id);
        if (!node) return;
        // null = 回到 manifest 的 label
        node.ui = { ...node.ui, title: title && title.trim() ? title : null };
      });
    },

    reconnectEdge(edgeId, to) {
      const { doc } = get();
      const edge = doc.edges.find((e) => e.id === edgeId);
      if (!edge) return { ok: false, reason: "连线不存在" };
      // 先把老边摘掉再判：不然「重连到同一个端口」会撞上「输入端口已有连线」
      const without: GraphDoc = { ...doc, edges: doc.edges.filter((e) => e.id !== edgeId) };
      const verdict = canConnect(ctx(), without, edge.from, to);
      if (!verdict.ok) {
        set({ lastRejection: verdict.reason });
        return verdict;
      }
      transact("改接连线", (d) => {
        const target = d.edges.find((e) => e.id === edgeId);
        if (target) target.to = { ...to };
      });
      return verdict;
    },

    insertOnEdge(edgeId, nodeId, inPort, outPort) {
      const { doc } = get();
      const edge = doc.edges.find((e) => e.id === edgeId);
      if (!edge) return false;
      const taken = allIds(doc);
      const a = newLocalId("e", taken);
      taken.add(a);
      const b = newLocalId("e", taken);
      transact("插入到连线中间", (d) => {
        d.edges = d.edges.filter((e) => e.id !== edgeId);
        d.edges.push({ id: a, from: { ...edge.from }, to: { node: nodeId, port: inPort } });
        d.edges.push({ id: b, from: { node: nodeId, port: outPort }, to: { ...edge.to } });
      });
      return true;
    },

    applyLayout(moves) {
      if (moves.length === 0) return;
      transact(moves.length === 1 ? "整理布局" : `整理 ${moves.length} 个节点的布局`, (d) => {
        for (const m of moves) {
          const node = d.nodes.find((n) => n.id === m.id);
          if (node) node.ui = { ...node.ui, position: m.position };
        }
      });
    },

    duplicateNodes(ids) {
      if (ids.length === 0) return { nodeIds: [] };
      const { doc } = get();
      const kept = new Set(ids);
      const nodes = doc.nodes.filter((n) => kept.has(n.id));
      if (nodes.length === 0) return { nodeIds: [] };
      const edges = doc.edges.filter((e) => kept.has(e.from.node) && kept.has(e.to.node));
      const origin = nodes.reduce(
        (acc, n) => ({
          x: Math.min(acc.x, n.ui?.position?.x ?? 0),
          y: Math.min(acc.y, n.ui?.position?.y ?? 0),
        }),
        { x: Infinity, y: Infinity },
      );
      // 偏移一点，否则复制出来的节点完全盖在原件上，用户以为什么都没发生
      return get().pasteNodes(
        { nodes: JSON.parse(JSON.stringify(nodes)), edges: JSON.parse(JSON.stringify(edges)) },
        { x: (Number.isFinite(origin.x) ? origin.x : 0) + 40, y: (Number.isFinite(origin.y) ? origin.y : 0) + 40 },
      );
    },

    applyMigrations(actions) {
      if (actions.length === 0) return 0;
      const byNode = new Map(actions.map((a) => [a.nodeId, a]));
      let changed = 0;
      transact(
        actions.length === 1 ? "迁移 1 个节点" : `迁移 ${actions.length} 个节点`,
        (d) => {
          for (const node of d.nodes) {
            const action = byNode.get(node.id);
            if (!action) continue;
            node.op = action.op;
            node.opVersion = action.opVersion;
            // params 是**完整**对象而不是补丁：改名参数没法用补丁表达
            node.params = { ...action.params };
            changed += 1;
          }
        },
      );
      return changed;
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
