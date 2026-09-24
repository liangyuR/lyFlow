// 图 store —— GraphDoc 是唯一真实数据源（ADR-0002）：改图只能走这里的语义化动作。
// M4 起动作作用于**当前层级**（ui.path），撤销栈仍然是整份 doc 快照（ADR-0010）。

import { current, enablePatches, isDraft, produce } from "immer";
import { create } from "zustand";

import { planAutoConnect, unconnectedRequiredInputs, type AutoAmbiguity } from "../lib/autoconnect";
import {
  graphParamNameProblem,
  graphParamValue,
  joinBind,
  resolveGraphBinding,
  specFromParam,
  splitBind,
  uniqueGraphParamName,
} from "../lib/graphParams";
import { newDocId, newLocalId } from "../lib/ids";
import { effectiveValue, pruneUnknownParams, sparseSet, valueEquals } from "../lib/params";
import {
  augmentOperators,
  composeSubgraph as composeInto,
  dissolveSubgraph as dissolveFrom,
  levelOf,
  pathIsValid,
  promotedBy,
  type ComposeResult,
  type SubPath,
} from "../lib/subgraph";
import { canConnect, type ConnectVerdict, type GraphContext } from "../lib/typecheck";
import type { MigrationAction } from "../types/execution";
import type { OperatorDesc, Param, SnippetDesc } from "../types/manifest";
import {
  GRAPH_SCHEMA_VERSION,
  subgraphIdOf,
  type GraphDoc,
  type GraphLevel,
  type GraphNode,
  type NodeUi,
  type ParamSpec,
  type PortRef,
  type SubParam,
} from "../types/graph";
import { useManifestStore } from "./manifest";
import { currentOverrides, useRecipeStore } from "./recipe";
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

/** 指定层级（参数面板里展开进子图定义的那几行，param-recipe P2.3）；不给就是当前层级。 */
function levelAt(doc: GraphDoc, at: SubPath | undefined): GraphLevel {
  return at ? levelOf(doc, at) : level(doc);
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

/** immer 的 draft 在 recipe 结束后就失效，抄进新对象之前先取出一份普通数据。
 *  manifest 里的声明也深拷一份：不拷的话 autoFreeze 会把 manifest store 里那份一起冻上。 */
function plain<T>(x: T): T {
  return structuredClone(isDraft(x) ? (current(x as never) as T) : x);
}

/** 节点在界面上叫什么：用户起的标题，其次算子 label。 */
function titleOf(node: GraphNode | undefined, ops: ReadonlyMap<string, OperatorDesc>): string {
  if (!node) return "?";
  return node.ui?.title || ops.get(node.op)?.label || node.id;
}

/** 图参数的默认 label（P1.1）：「节点标题 · 参数 label」。在子图里纳入时把路径上的实例标题
 *  也带上（「实例 / 内部节点 · 参数」）—— 同一个子图的两个实例各纳入一次，label 得分得开。 */
function graphParamLabel(
  doc: GraphDoc,
  path: SubPath,
  nodeId: string,
  decl: Param,
  ops: ReadonlyMap<string, OperatorDesc>,
): string {
  const titles: string[] = [];
  let lvl: GraphLevel = doc;
  for (const seg of path) {
    titles.push(titleOf(lvl.nodes.find((n) => n.id === seg.nodeId), ops));
    lvl = doc.subgraphs?.[seg.subgraphId] ?? lvl;
  }
  titles.push(titleOf(lvl.nodes.find((n) => n.id === nodeId), ops));
  return `${titles.join(" / ")} · ${decl.label || decl.name}`;
}

/** K2 的逐层提升链，在 draft 上做：从当前层的 `nodeId.<decl.name>` 往外，哪一层还没提升就在
 *  那个子图定义里提升成子图参数（默认值 = 这一层的当前值，所以同一子图的其它实例行为不变），
 *  一直走到顶层。返回顶层要绑的目标、它的声明与当前值。顶层（path 为空）原样返回。 */
function liftToTop(
  d: GraphDoc,
  path: SubPath,
  nodeId: string,
  decl: Param,
  value: unknown,
): { node: string; param: string; decl: Param; value: unknown } | null {
  let node = nodeId;
  let param = decl.name;
  let curDecl: Param = decl;
  let cur = value;
  for (let i = path.length - 1; i >= 0; i -= 1) {
    const seg = path[i]!;
    const def = d.subgraphs?.[seg.subgraphId];
    if (!def) return null;
    let sp = def.params?.find((p) => p.binds?.some((b) => b.node === node && b.param === param));
    if (!sp) {
      const taken = new Set((def.params ?? []).map((p) => p.name));
      let name = param;
      for (let k = 2; taken.has(name); k += 1) name = `${param}_${k}`;
      // 与 promoteParam 同一个抄法：roiBackdrop 指的是内部算子的参数名，不带出去（semantic 照带）
      const copy = plain(curDecl) as Param & { binds?: unknown };
      delete copy.roiBackdrop;
      delete copy.binds;
      sp = { ...copy, name, default: plain(cur), binds: [{ node, param }] } as SubParam;
      def.params = [...(def.params ?? []), sp];
    }
    const parent: GraphLevel | undefined = i === 0 ? d : d.subgraphs?.[path[i - 1]!.subgraphId];
    const inst = parent?.nodes.find((n) => n.id === seg.nodeId);
    if (!inst) return null;
    // 往外一层的「当前值」= 这个实例上外参的值（显式写了用它，没写用外参默认值）
    cur = inst.params?.[sp.name] !== undefined ? inst.params[sp.name] : sp.default;
    curDecl = sp;
    node = seg.nodeId;
    param = sp.name;
  }
  return { node, param, decl: curDecl, value: cur };
}

/** 解除绑定 / 删图参数时把当前值写回节点（行为不变）。仍走稀疏存储：值回到算子默认就删键。 */
function writeBack(
  d: GraphDoc,
  ops: ReadonlyMap<string, OperatorDesc>,
  bind: string,
  value: unknown,
): void {
  const target = splitBind(bind);
  const node = target ? d.nodes.find((n) => n.id === target.node) : undefined;
  if (!target || !node) return;
  const op = ops.get(node.op);
  node.params = op
    ? sparseSet(op, node.params, target.param, plain(value))
    : { ...(node.params ?? {}), [target.param]: plain(value) };
}

export interface PasteResult {
  nodeIds: string[];
}

/** 拖入节点 / 插入片段之后自动连线的结果（m8-plan L13 / L14）。 */
export interface AutoConnectResult {
  nodeIds: string[];
  /** 自动连上的边数。 */
  wired: number;
  /** 有多个候选、没有连的那些输入。 */
  ambiguous: AutoAmbiguity[];
  /** 片段里引用了、当前 core 没有的算子（这些节点没插进来）。 */
  missing: string[];
}

interface GraphState {
  doc: GraphDoc;
  filePath: string | null;
  dirty: boolean;
  /** 最近一次存盘（或打开）时的那份 doc。dirty = doc 不是它：撤销回到保存点时 dirty 复原（P1.6）。
   *  比的是对象身份 —— 撤销栈存的就是整份快照，回到保存点拿回来的正是同一个对象。 */
  savedDoc: GraphDoc | null;
  /** 「换了一整张图」的计数：newDoc / loadDoc 各加一。画布的动效差分（docs/motion-plan.md N1）
   *  靠它区分「编辑」与「打开文件」—— 打开一张图不该满屏播进场。不进撤销栈、不进文件。 */
  epoch: number;

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
  /** 拖入节点：加节点 + 按类型自动连线（L13），整个算一条撤销。界面的三种加节点方式都走它；
   *  addNode 保持「只加节点」，脚本与验收用它精确搭图。 */
  addNodeAuto(opId: string, position: { x: number; y: number }): AutoConnectResult;
  /** 插入片段：带自动连线的粘贴（L14）。插完是普通节点，没有展开 / 收回。 */
  insertSnippet(snippet: SnippetDesc, at: { x: number; y: number }): AutoConnectResult;
  deleteNodes(ids: readonly string[]): void;
  moveNodes(moves: readonly { id: string; position: { x: number; y: number } }[]): void;
  /** at：节点所在的层级，不给 = 当前层级（ui.path）。参数面板展开进子图定义时给的是那一层的路径。 */
  setParam(nodeId: string, name: string, value: unknown, at?: SubPath): void;
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
  // -- 顶层图参数（param-recipe P1.3）：每个都是一次撤销 ---------------------
  /** 「纳入配方」= 提升为图参数（K2）：当前有效值成为 default，节点上的显式值删除，加一条 bind；
   *  规格从被绑定目标的声明抄（P1.1）。在子图里调就做整条提升链（内参 → 子图参数 → 这个实例上
   *  绑成图参数），同一子图的其它实例以当前值作默认值、行为不变。返回图参数名。 */
  promoteToGraphParam(nodeId: string, paramName: string, at?: SubPath): string | null;
  /** 把当前层的一个参数也绑到已有的图参数上（子图里同样走提升链）。它的值从此取图参数的值。 */
  bindToGraphParam(name: string, nodeId: string, paramName: string): boolean;
  /** 解除一条绑定（bind 是 `节点.参数`）：把图参数当前的有效值写回节点，行为不变。 */
  unbindFromGraphParam(name: string, bind: string): void;
  /** 删掉图参数：当前有效值写回它绑定的每一个目标，行为不变。 */
  removeGraphParam(name: string): void;
  /** 改名。名字规则见 graphParamNameProblem；不合法时返回 false 并写 lastRejection。 */
  renameGraphParam(name: string, next: string): boolean;
  /** 改「基础」值（default）。滑块拖动时在 begin/commit 里合成一条撤销。 */
  setGraphParamDefault(name: string, value: unknown): void;
  /** 改规格（label、限位、单位、group……）。patch 里值为 undefined 的键删掉。 */
  setGraphParamSpec(name: string, patch: Partial<ParamSpec>): void;
  /** 在被图参数绑定的那一行上编辑（P1.4 / K6）：选着配方写进配方，选着「基础」写 default。
   *  P1 当前配方恒为「基础」。setParam 命中被绑定的参数时也走这里 —— 不再写成节点上的显式值。 */
  editGraphParamValue(name: string, value: unknown): void;

  /** 把当前子图里某个内参提升成对外参数（F4）。返回外参名。 */
  promoteParam(nodeId: string, paramName: string, at?: SubPath): string | null;
  /** 取消提升。内参回到可编辑，值保持提升时的那个。at 同 setParam。 */
  unpromoteParam(paramName: string, at?: SubPath): void;
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
  /** 存盘成功。doc 是真正写下去的那一份（存盘是异步的，期间用户可能又改了）；不给就是当前的。 */
  markSaved(path: string, doc?: GraphDoc): void;
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
      dirty: next !== get().savedDoc,
    });
  };

  /** 应用变更但不记撤销。用于事务进行中的中间状态（拖动的每一帧）。 */
  const mutate = (recipe: (draft: GraphDoc) => void) => {
    const { doc } = get();
    const next = produce(doc, recipe);
    if (next === doc) return;
    set({ doc: next, dirty: next !== get().savedDoc });
  };

  /** 当前层级（ui.path）的一个参数：节点、它的算子（含 sub: 合成的）、声明。找不到返回 null。 */
  const lookupParam = (nodeId: string, paramName: string, at?: SubPath) => {
    const { doc } = get();
    const path = at ?? useUiStore.getState().path;
    if (!pathIsValid(doc, path)) {
      set({ lastRejection: "当前层级已失效，回到顶层再试" });
      return null;
    }
    const ops = ctx(doc).operatorsById;
    const node = levelOf(doc, path).nodes.find((n) => n.id === nodeId);
    const op = node ? ops.get(node.op) : undefined;
    const decl = op?.params.find((p) => p.name === paramName);
    if (!node || !op || !decl) {
      set({ lastRejection: `找不到参数 ${nodeId}.${paramName}` });
      return null;
    }
    return { doc, path, ops, node, op, decl };
  };

  return {
    doc: emptyDoc(),
    filePath: null,
    dirty: false,
    savedDoc: null,
    epoch: 0,
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
        dirty: doc !== get().savedDoc,
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

    addNodeAuto(opId, position) {
      const { doc } = get();
      const c = ctx(doc);
      const op = c.operatorsById.get(opId);
      if (!op) {
        set({ lastRejection: `算子未注册：${opId}` });
        return { nodeIds: [], wired: 0, ambiguous: [], missing: [opId] };
      }
      const taken = allIds(doc);
      const id = newLocalId("n", taken);
      taken.add(id);
      const node: GraphNode = { id, op: op.id, opVersion: op.version, params: {}, ui: { position } };
      const view = levelDoc(doc);
      const withNode: GraphDoc = { ...view, nodes: [...view.nodes, node] };
      const plan = planAutoConnect(c, withNode, unconnectedRequiredInputs(c, withNode, id));
      const edges = plan.wires.map((w) => {
        const eid = newLocalId("e", taken);
        taken.add(eid);
        return { id: eid, from: { ...w.from }, to: { ...w.to } };
      });
      const label = edges.length > 0 ? `添加 ${op.label}（自动连 ${edges.length} 条）` : `添加 ${op.label}`;
      transact(label, (d) => {
        const lvl = level(d);
        lvl.nodes.push(node);
        lvl.edges.push(...edges);
      });
      useUiStore.getState().setAutoHint(plan.ambiguous);
      return { nodeIds: [id], wired: edges.length, ambiguous: plan.ambiguous, missing: [] };
    },

    insertSnippet(snippet, at) {
      const { doc } = get();
      const c = ctx(doc);
      const taken = allIds(doc);
      const idMap = new Map<string, string>();
      const missing: string[] = [];
      const origin = snippet.nodes.reduce(
        (acc, n) => ({
          x: Math.min(acc.x, n.ui?.position?.x ?? 0),
          y: Math.min(acc.y, n.ui?.position?.y ?? 0),
        }),
        { x: Infinity, y: Infinity },
      );
      const dx = Number.isFinite(origin.x) ? at.x - origin.x : at.x;
      const dy = Number.isFinite(origin.y) ? at.y - origin.y : at.y;

      const nodes: GraphNode[] = [];
      for (const n of snippet.nodes) {
        const op = c.operatorsById.get(n.op);
        if (!op) {
          missing.push(n.op);
          continue;
        }
        const id = newLocalId("n", taken);
        taken.add(id);
        idMap.set(n.id, id);
        const ui: NodeUi = {
          position: { x: (n.ui?.position?.x ?? 0) + dx, y: (n.ui?.position?.y ?? 0) + dy },
        };
        if (n.ui?.title) ui.title = n.ui.title;
        nodes.push({ id, op: op.id, opVersion: op.version, params: pruneUnknownParams(op, n.params), ui });
      }
      if (nodes.length === 0) {
        set({ lastRejection: `片段 ${snippet.label} 里的算子当前 core 一个都没有` });
        return { nodeIds: [], wired: 0, ambiguous: [], missing };
      }
      const inner = (snippet.edges ?? [])
        .filter((e) => idMap.has(e.from.node) && idMap.has(e.to.node))
        .map((e) => {
          const eid = newLocalId("e", taken);
          taken.add(eid);
          return {
            id: eid,
            from: { node: idMap.get(e.from.node)!, port: e.from.port },
            to: { node: idMap.get(e.to.node)!, port: e.to.port },
          };
        });

      const view = levelDoc(doc);
      const merged: GraphDoc = { ...view, nodes: [...view.nodes, ...nodes], edges: [...view.edges, ...inner] };
      // 对外端口提示给了就按它接（可选输入也接，比如 result_bundle 的 rois / scan），没给就接
      // 片段里所有没连上的必需输入。候选只在插入之前就在图里的那些节点里找。
      const hinted = snippet.ports?.inputs;
      const targets = hinted
        ? hinted.filter((h) => idMap.has(h.node)).map((h) => ({ node: idMap.get(h.node)!, port: h.port }))
        : nodes.flatMap((n) => unconnectedRequiredInputs(c, merged, n.id));
      const plan = planAutoConnect(c, merged, targets, new Set(nodes.map((n) => n.id)));
      const wires = plan.wires.map((w) => {
        const eid = newLocalId("e", taken);
        taken.add(eid);
        return { id: eid, from: { ...w.from }, to: { ...w.to } };
      });

      transact(`插入片段 ${snippet.label}`, (d) => {
        const lvl = level(d);
        lvl.nodes.push(...nodes);
        lvl.edges.push(...inner, ...wires);
      });
      useUiStore.getState().setAutoHint(plan.ambiguous);
      return { nodeIds: nodes.map((n) => n.id), wired: wires.length, ambiguous: plan.ambiguous, missing };
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
        // 顶层图参数指着被删节点的 bind 一并摘掉，否则存下来就是一条 unknown_bind。
        // 图参数本身留着（可能还绑着别人，也可能用户马上要重新绑）
        if (lvl === d && d.params) {
          for (const gp of Object.values(d.params)) {
            const kept = gp.binds.filter((b) => !kill.has(splitBind(b)?.node ?? ""));
            if (kept.length !== gp.binds.length) gp.binds = kept;
          }
        }
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

    setParam(nodeId, name, value, at) {
      const { doc } = get();
      // 被图参数绑定的参数（P1.4）：改的是图参数，不写成节点上的显式值 —— 那正是 param_conflict
      // 的来路。Inspector、参数面板、2D 拖框、粘贴、重置都经这里，所以在这一处路由而不是各处各判一遍。
      const binding = resolveGraphBinding(doc, at ?? useUiStore.getState().path, nodeId, name);
      if (binding) {
        get().editGraphParamValue(binding.graphParam, value);
        return;
      }
      const node = levelAt(doc, at).nodes.find((n) => n.id === nodeId);
      if (!node) return;
      const op = ctx(doc).operatorsById.get(node.op);
      if (!op) return;

      const nextParams = sparseSet(op, node.params, name, plain(value));
      const apply = (d: GraphDoc) => {
        const target = levelAt(d, at).nodes.find((n) => n.id === nodeId);
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

    promoteParam(nodeId, paramName, at) {
      const { doc } = get();
      const path = at ?? useUiStore.getState().path;
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
        // roiBackdrop 指的是内部算子的参数名，到了子图这一层对不上，不带出去（semantic 照带）
        const decl = { ...param };
        delete decl.roiBackdrop;
        const promoted: SubParam = {
          ...decl,
          name,
          default: current !== undefined ? current : param.default,
          binds: [{ node: nodeId, param: paramName }],
        };
        target.params = [...(target.params ?? []), promoted];
      });
      return name;
    },

    unpromoteParam(paramName, at) {
      const path = at ?? useUiStore.getState().path;
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
        // 顶层实例上这个外参要是绑着图参数（纳入配方的提升链），那条 bind 也跟着走 ——
        // 外参没了，留着它就是一条 unknown_bind
        const instances = new Set(
          d.nodes.filter((n) => subgraphIdOf(n.op) === last.subgraphId).map((n) => n.id),
        );
        for (const gp of Object.values(d.params ?? {})) {
          const kept = gp.binds.filter((b) => {
            const t = splitBind(b);
            return !(t && t.param === paramName && instances.has(t.node));
          });
          if (kept.length !== gp.binds.length) gp.binds = kept;
        }
      });
    },

    promoteToGraphParam(nodeId, paramName, at) {
      const found = lookupParam(nodeId, paramName, at);
      if (!found) return null;
      const { doc, path, ops, node, op, decl } = found;
      const already = resolveGraphBinding(doc, path, nodeId, paramName);
      if (already) {
        set({ lastRejection: `已经由图参数 ${already.graphParam} 提供` });
        return null;
      }
      const name = uniqueGraphParamName(doc, paramName);
      const label = graphParamLabel(doc, path, nodeId, decl, ops);
      // 当前有效值成为 default：纳入前后界面上的数字不跳、运行结果逐位相同（验收 3）
      const value = effectiveValue(op, node, paramName);
      let done = false;
      transact(`纳入配方 ${name}`, (d) => {
        const top = liftToTop(d, path, nodeId, decl, value);
        const target = top ? d.nodes.find((n) => n.id === top.node) : undefined;
        if (!top || !target) return;
        d.params = {
          ...(d.params ?? {}),
          [name]: {
            ...specFromParam(plain(top.decl), label),
            default: plain(top.value),
            binds: [joinBind(top.node, top.param)],
          },
        };
        // 一处定义：被绑定的参数不能再在节点上写值（param_conflict）
        if (target.params && top.param in target.params) {
          const next = { ...target.params };
          delete next[top.param];
          target.params = next;
        }
        done = true;
      });
      return done ? name : null;
    },

    bindToGraphParam(name, nodeId, paramName) {
      const found = lookupParam(nodeId, paramName);
      if (!found) return false;
      const { doc, path, op, node, decl } = found;
      const gp = doc.params?.[name];
      if (!gp) {
        set({ lastRejection: `没有图参数 ${name}` });
        return false;
      }
      const already = resolveGraphBinding(doc, path, nodeId, paramName);
      if (already) {
        if (already.graphParam === name) return true;
        set({ lastRejection: `已经由图参数 ${already.graphParam} 提供` });
        return false;
      }
      // 规格以图参数为准（控件、第一道校验），被绑定目标自己的规格 core 照样查（P1.2）：
      // 类型都对不上的话两道校验必有一道永远不过，不如在这里就拦下
      if (gp.type && gp.type !== decl.type) {
        set({ lastRejection: `类型不同：图参数 ${name} 是 ${gp.type}，${paramName} 是 ${decl.type}` });
        return false;
      }
      let done = false;
      transact(`绑定到图参数 ${name}`, (d) => {
        const top = liftToTop(d, path, nodeId, decl, effectiveValue(op, node, paramName));
        const target = top ? d.nodes.find((n) => n.id === top.node) : undefined;
        const into = d.params?.[name];
        if (!top || !target || !into) return;
        into.binds = [...into.binds, joinBind(top.node, top.param)];
        if (target.params && top.param in target.params) {
          const next = { ...target.params };
          delete next[top.param];
          target.params = next;
        }
        done = true;
      });
      return done;
    },

    unbindFromGraphParam(name, bind) {
      const { doc } = get();
      const gp = doc.params?.[name];
      if (!gp || !gp.binds.includes(bind)) return;
      // 「当前值」= 当前配方下的有效值（P1 = default）：解除之后这个节点跑出来的还是刚才那样
      const value = graphParamValue(doc, name, currentOverrides());
      const ops = ctx(doc).operatorsById;
      transact(`解除绑定 ${bind}`, (d) => {
        writeBack(d, ops, bind, value);
        const into = d.params?.[name];
        if (into) into.binds = into.binds.filter((b) => b !== bind);
      });
    },

    removeGraphParam(name) {
      const { doc } = get();
      const gp = doc.params?.[name];
      if (!gp) return;
      const value = graphParamValue(doc, name, currentOverrides());
      const ops = ctx(doc).operatorsById;
      transact(`删除图参数 ${name}`, (d) => {
        for (const bind of gp.binds) writeBack(d, ops, bind, value);
        const next = { ...(d.params ?? {}) };
        delete next[name];
        // 最后一个也删了就连键一起拿掉：老图原本就没有 params，存盘不该多出一个空对象
        if (Object.keys(next).length === 0) delete d.params;
        else d.params = next;
      });
    },

    renameGraphParam(name, next) {
      const { doc } = get();
      if (!doc.params?.[name]) return false;
      if (next === name) return true;
      const problem = graphParamNameProblem(doc, next, name);
      if (problem) {
        set({ lastRejection: problem });
        return false;
      }
      transact(`重命名图参数 ${name} → ${next}`, (d) => {
        // 保持键的位置：存盘是给人 diff 的，改个名不该让这一段挪到末尾
        d.params = Object.fromEntries(
          Object.entries(d.params ?? {}).map(([k, v]) => [k === name ? next : k, v]),
        );
      });
      return true;
    },

    setGraphParamDefault(name, value) {
      const gp = get().doc.params?.[name];
      if (!gp || valueEquals(gp.default, value)) return;
      const apply = (d: GraphDoc) => {
        const into = d.params?.[name];
        if (into) into.default = plain(value);
      };
      // 滑块与数字框拖动时每帧都来，靠外层 begin/commit 合成一条撤销（同 setParam）
      if (get().pendingSnapshot) mutate(apply);
      else transact(`修改图参数 ${name}`, apply);
    },

    setGraphParamSpec(name, patch) {
      if (!get().doc.params?.[name]) return;
      transact(`修改图参数 ${name} 的规格`, (d) => {
        const into = d.params?.[name] as Record<string, unknown> | undefined;
        if (!into) return;
        for (const [k, v] of Object.entries(patch)) {
          // binds / default / type 各有各的动作，规格补丁不许顺手改它们
          if (k === "binds" || k === "default" || k === "type") continue;
          if (v === undefined) delete into[k];
          else into[k] = plain(v);
        }
      });
    },

    editGraphParamValue(name, value) {
      // K6 ①：选着某个配方时写进那个配方（P3 接在这里），选着「基础」时写 default。
      // P1 没有配方，当前恒为「基础」。
      if (useRecipeStore.getState().current === null) get().setGraphParamDefault(name, value);
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
        // 撤销回到保存点 = 文件里就是这一份，标题栏的 ● 该消失（P1.6）
        dirty: entry.doc !== get().savedDoc,
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
        dirty: entry.doc !== get().savedDoc,
        pendingSnapshot: null,
      });
    },

    canUndo: () => get().past.length > 0,
    canRedo: () => get().future.length > 0,

    newDoc() {
      useUiStore.getState().setPath([]);
      const doc = emptyDoc();
      set({
        doc,
        savedDoc: doc,
        filePath: null,
        dirty: false,
        epoch: get().epoch + 1,
        past: [],
        future: [],
        pendingSnapshot: null,
      });
    },

    loadDoc(doc, path) {
      useUiStore.getState().setPath([]);
      set({
        doc,
        savedDoc: doc,
        filePath: path,
        dirty: false,
        epoch: get().epoch + 1,
        past: [],
        future: [],
        pendingSnapshot: null,
      });
    },

    markSaved(path, saved) {
      const doc = saved ?? get().doc;
      set({ filePath: path, savedDoc: doc, dirty: get().doc !== doc });
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
