// 图 store —— GraphDoc 是唯一真实数据源（ADR-0002）：改图只能走这里的语义化动作。
// M4 起动作作用于**当前层级**（ui.path），撤销栈仍然是整份 doc 快照（ADR-0010）。

import { enablePatches, produce } from "immer";
import { create } from "zustand";

import { newDocId, newLocalId } from "../lib/ids";
import { pruneUnknownParams, sparseSet } from "../lib/params";
import {
  augmentOperators,
  composeSubgraph as composeInto,
  dissolveSubgraph as dissolveFrom,
  levelOf,
  promotedBy,
  type ComposeResult,
} from "../lib/subgraph";
import { canConnect, type ConnectVerdict, type GraphContext } from "../lib/typecheck";
import type { MigrationAction } from "../types/execution";
import {
  GRAPH_SCHEMA_VERSION,
  subgraphIdOf,
  type GraphDoc,
  type GraphLevel,
  type GraphNode,
  type NodeUi,
  type PortRef,
  type SubParam,
} from "../types/graph";
import { useManifestStore } from "./manifest";
import { useUiStore } from "./ui";

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

/** 从 manifest store 取算子/类型索引，并合上本文档里的 `sub:` 定义。 */
function ctx(doc: GraphDoc): GraphContext {
  const m = useManifestStore.getState();
  return {
    operatorsById: augmentOperators(m.operatorsById, doc.subgraphs),
    typesByName: m.typesByName,
  };
}

/** 当前层级。ui.path 是导航状态，改图的动作都作用在它指的那一层。 */
function level(doc: GraphDoc): GraphLevel {
  return levelOf(doc, useUiStore.getState().path);
}

/** 当前层级的「像一份 doc」的视图，喂给只认 GraphDoc 的校验函数。 */
function levelDoc(doc: GraphDoc): GraphDoc {
  const lvl = level(doc);
  return lvl === doc ? doc : { ...doc, nodes: lvl.nodes, edges: lvl.edges };
}

/** 全文档已用的 id。跨层唯一不是必须的，但重名会让路径读起来很费劲。 */
function allIds(doc: GraphDoc): Set<string> {
  const s = new Set<string>();
  const add = (lvl: GraphLevel) => {
    for (const n of lvl.nodes) s.add(n.id);
    for (const e of lvl.edges) s.add(e.id);
  };
  add(doc);
  for (const def of Object.values(doc.subgraphs ?? {})) add(def);
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

  // -- 子图（ADR-0010）-----------------------------------------------------
  /** 把选中的节点合成一个子图。返回新节点与子图的 id。 */
  composeSubgraph(ids: readonly string[]): ComposeResult | null;
  /** 解散一个子图节点，内容内联回本层。返回内联出来的节点 id。 */
  dissolveSubgraph(nodeId: string): string[];
  /** 把当前子图里某个内参提升成对外参数（F4）。返回外参名。 */
  promoteParam(nodeId: string, paramName: string): string | null;
  /** 取消提升。内参回到可编辑，值保持提升时的那个。 */
  unpromoteParam(paramName: string): void;
  renameSubgraph(subgraphId: string, name: string): void;

  // -- 图级输出（ADR-0017）-------------------------------------------------
  /** 把一个端口标成图级命名输出。名字重了自动加后缀，返回最终用的名字。 */
  markGraphOutput(ref: PortRef, name?: string): string;
  removeGraphOutput(name: string): void;

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
      const op = ctx(get().doc).operatorsById.get(opId);
      if (!op) {
        set({ lastRejection: `算子未注册：${opId}` });
        return null;
      }
      const id = newLocalId("n", allIds(get().doc));
      transact(`添加 ${op.label}`, (d) => {
        level(d).nodes.push({
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
        const lvl = level(d);
        lvl.nodes = lvl.nodes.filter((n) => !kill.has(n.id));
        // 删节点自动清理相连边（交互清单 P0 #5）
        lvl.edges = lvl.edges.filter((e) => !kill.has(e.from.node) && !kill.has(e.to.node));
      });
    },

    moveNodes(moves) {
      if (moves.length === 0) return;
      // 拖动过程中每帧都调，所以走 mutate 不记撤销；
      // 一次拖动的撤销由 begin/commit 包住整体记一条。
      mutate((d) => {
        const lvl = level(d);
        for (const m of moves) {
          const node = lvl.nodes.find((n) => n.id === m.id);
          if (!node) continue;
          node.ui = { ...node.ui, position: m.position };
        }
      });
    },

    setParam(nodeId, name, value) {
      const { doc } = get();
      const node = level(doc).nodes.find((n) => n.id === nodeId);
      if (!node) return;
      const op = ctx(doc).operatorsById.get(node.op);
      if (!op) return;

      const nextParams = sparseSet(op, node.params, name, value);
      const apply = (d: GraphDoc) => {
        const target = level(d).nodes.find((n) => n.id === nodeId);
        if (target) target.params = nextParams;
      };

      // 滑块拖动时 setParam 每帧都来，靠外层 begin/commit 合成一条撤销。
      if (get().pendingSnapshot) mutate(apply);
      else transact(`修改 ${op.label}.${name}`, apply);
    },

    setNodeUi(nodeId, patch) {
      transact("修改节点外观", (d) => {
        const node = level(d).nodes.find((n) => n.id === nodeId);
        if (node) node.ui = { ...node.ui, ...patch };
      });
    },

    connect(from, to) {
      const { doc } = get();
      const verdict = canConnect(ctx(doc), levelDoc(doc), from, to);
      if (!verdict.ok) {
        set({ lastRejection: verdict.reason });
        return verdict;
      }
      const id = newLocalId("e", allIds(doc));
      transact("连线", (d) => {
        level(d).edges.push({ id, from: { ...from }, to: { ...to } });
      });
      return verdict;
    },

    disconnect(edgeIds) {
      if (edgeIds.length === 0) return;
      const kill = new Set(edgeIds);
      transact(edgeIds.length === 1 ? "断开连线" : `断开 ${edgeIds.length} 条连线`, (d) => {
        const lvl = level(d);
        lvl.edges = lvl.edges.filter((e) => !kill.has(e.id));
      });
    },

    pasteNodes(payload, at) {
      const taken = allIds(get().doc);
      const idMap = new Map<string, string>();
      const manifest = ctx(get().doc).operatorsById;

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
        const lvl = level(d);
        lvl.nodes.push(...newNodes);
        lvl.edges.push(...newEdges);
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
        for (const n of level(d).nodes) {
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
        for (const n of level(d).nodes) {
          if (!target.has(n.id)) continue;
          n.ui = { ...n.ui, collapsed: value };
        }
      });
    },

    renameNode(id, title) {
      transact("重命名节点", (d) => {
        const node = level(d).nodes.find((n) => n.id === id);
        if (!node) return;
        // null = 回到 manifest 的 label
        node.ui = { ...node.ui, title: title && title.trim() ? title : null };
      });
    },

    reconnectEdge(edgeId, to) {
      const { doc } = get();
      const lvl = level(doc);
      const edge = lvl.edges.find((e) => e.id === edgeId);
      if (!edge) return { ok: false, reason: "连线不存在" };
      // 先把老边摘掉再判：不然「重连到同一个端口」会撞上「输入端口已有连线」
      const without: GraphDoc = { ...levelDoc(doc), edges: lvl.edges.filter((e) => e.id !== edgeId) };
      const verdict = canConnect(ctx(doc), without, edge.from, to);
      if (!verdict.ok) {
        set({ lastRejection: verdict.reason });
        return verdict;
      }
      transact("改接连线", (d) => {
        const target = level(d).edges.find((e) => e.id === edgeId);
        if (target) target.to = { ...to };
      });
      return verdict;
    },

    insertOnEdge(edgeId, nodeId, inPort, outPort) {
      const { doc } = get();
      const edge = level(doc).edges.find((e) => e.id === edgeId);
      if (!edge) return false;
      const taken = allIds(doc);
      const a = newLocalId("e", taken);
      taken.add(a);
      const b = newLocalId("e", taken);
      transact("插入到连线中间", (d) => {
        const lvl = level(d);
        lvl.edges = lvl.edges.filter((e) => e.id !== edgeId);
        lvl.edges.push({ id: a, from: { ...edge.from }, to: { node: nodeId, port: inPort } });
        lvl.edges.push({ id: b, from: { node: nodeId, port: outPort }, to: { ...edge.to } });
      });
      return true;
    },

    applyLayout(moves) {
      if (moves.length === 0) return;
      transact(moves.length === 1 ? "整理布局" : `整理 ${moves.length} 个节点的布局`, (d) => {
        const lvl = level(d);
        for (const m of moves) {
          const node = lvl.nodes.find((n) => n.id === m.id);
          if (node) node.ui = { ...node.ui, position: m.position };
        }
      });
    },

    duplicateNodes(ids) {
      if (ids.length === 0) return { nodeIds: [] };
      const { doc } = get();
      const lvl = level(doc);
      const kept = new Set(ids);
      const nodes = lvl.nodes.filter((n) => kept.has(n.id));
      if (nodes.length === 0) return { nodeIds: [] };
      const edges = lvl.edges.filter((e) => kept.has(e.from.node) && kept.has(e.to.node));
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
          // 迁移诊断的 nodeId 是路径，顶层节点的路径就是它自己
          const apply = (nodes: GraphNode[]) => {
            for (const node of nodes) {
              const action = byNode.get(node.id);
              if (!action) continue;
              node.op = action.op;
              node.opVersion = action.opVersion;
              // params 是**完整**对象而不是补丁：改名参数没法用补丁表达
              node.params = { ...action.params };
              changed += 1;
            }
          };
          apply(d.nodes);
          for (const def of Object.values(d.subgraphs ?? {})) apply(def.nodes);
        },
      );
      return changed;
    },

    composeSubgraph(ids) {
      if (ids.length === 0) return null;
      const path = useUiStore.getState().path;
      let result: ComposeResult | null = null;
      transact(ids.length === 1 ? "合成子图" : `把 ${ids.length} 个节点合成子图`, (d) => {
        result = composeInto(ctx(d), d, path, ids, allIds(d));
      });
      return result;
    },

    dissolveSubgraph(nodeId) {
      const path = useUiStore.getState().path;
      let inlined: string[] = [];
      transact("解散子图", (d) => {
        inlined = dissolveFrom(d, path, nodeId, allIds(d));
      });
      return inlined;
    },

    promoteParam(nodeId, paramName) {
      const { doc } = get();
      const path = useUiStore.getState().path;
      const last = path[path.length - 1];
      if (!last) {
        set({ lastRejection: "只有在子图里才能提升参数" });
        return null;
      }
      const def = doc.subgraphs?.[last.subgraphId];
      const node = def?.nodes.find((n) => n.id === nodeId);
      const op = node ? ctx(doc).operatorsById.get(node.op) : undefined;
      const param = op?.params.find((p) => p.name === paramName);
      if (!def || !node || !param) return null;
      if (promotedBy(def, nodeId, paramName)) {
        set({ lastRejection: "这个参数已经提升过了" });
        return null;
      }
      const taken = new Set(def.params.map((p) => p.name));
      let name = paramName;
      for (let i = 2; taken.has(name); i += 1) name = `${paramName}_${i}`;
      // 提升时把当前值当成外参的默认值，界面上的数字不会因为提升而跳变
      const current = node.params?.[paramName];
      transact(`提升参数 ${name}`, (d) => {
        const target = d.subgraphs?.[last.subgraphId];
        if (!target) return;
        const promoted: SubParam = {
          ...param,
          name,
          default: current !== undefined ? current : param.default,
          binds: [{ node: nodeId, param: paramName }],
        };
        target.params = [...(target.params ?? []), promoted];
      });
      return name;
    },

    unpromoteParam(paramName) {
      const path = useUiStore.getState().path;
      const last = path[path.length - 1];
      if (!last) return;
      transact(`取消提升 ${paramName}`, (d) => {
        const def = d.subgraphs?.[last.subgraphId];
        if (!def) return;
        const promoted = def.params?.find((p) => p.name === paramName);
        if (!promoted) return;
        // 把外参当前的默认值写回内参，取消提升不该悄悄改变行为
        for (const bind of promoted.binds ?? []) {
          const node = def.nodes.find((n) => n.id === bind.node);
          if (node) node.params = { ...(node.params ?? {}), [bind.param]: promoted.default };
        }
        def.params = def.params.filter((p) => p.name !== paramName);
        // 外层节点上那个键也要跟着走，否则展开时会报 unknown_param
        const strip = (nodes: GraphNode[]) => {
          for (const n of nodes) {
            if (subgraphIdOf(n.op) === last.subgraphId && n.params) delete n.params[paramName];
          }
        };
        strip(d.nodes);
        for (const other of Object.values(d.subgraphs ?? {})) strip(other.nodes);
      });
    },

    renameSubgraph(subgraphId, name) {
      transact("重命名子图", (d) => {
        const def = d.subgraphs?.[subgraphId];
        if (def) def.name = name;
      });
    },

    markGraphOutput(ref, name) {
      const { doc } = get();
      const existing = doc.outputs ?? {};
      // 同一个端口已经标过就复用原来的名字，右键两次不会冒出两条
      const already = Object.entries(existing).find(
        ([, o]) => o.node === ref.node && o.port === ref.port,
      );
      if (already) return already[0];
      const base = name ?? ref.port;
      let final = base;
      for (let i = 2; existing[final] !== undefined; i += 1) final = `${base}_${i}`;
      transact(`标为输出 ${final}`, (d) => {
        d.outputs = { ...(d.outputs ?? {}), [final]: { node: ref.node, port: ref.port } };
      });
      return final;
    },

    removeGraphOutput(name) {
      if (get().doc.outputs?.[name] === undefined) return;
      transact(`取消输出 ${name}`, (d) => {
        const next = { ...(d.outputs ?? {}) };
        delete next[name];
        d.outputs = next;
      });
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
      useUiStore.getState().setPath([]);
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
      useUiStore.getState().setPath([]);
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

/** 当前层级的子图定义。Inspector 与画布都要读它。 */
export function currentSubgraph(doc: GraphDoc, path: readonly { subgraphId: string }[]) {
  const last = path[path.length - 1];
  return last ? doc.subgraphs?.[last.subgraphId] : undefined;
}
