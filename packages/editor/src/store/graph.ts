// 图 store —— GraphDoc 是唯一真实数据源（ADR-0002）：改图只能走这里的语义化动作。
// M4 起动作作用于**当前层级**（ui.path），撤销栈仍然是整份 doc 快照（ADR-0010）。

import { current, enablePatches, isDraft, produce } from "immer";
import { create } from "zustand";

import { healPlan, planAutoConnect, unconnectedRequiredInputs, type AutoAmbiguity } from "../lib/autoconnect";
import {
  graphParamNameProblem,
  graphParamValue,
  joinBind,
  materializeBindings,
  resolveGraphBinding,
  specFromParam,
  splitBind,
  uniqueGraphParamName,
} from "../lib/graphParams";
import { newDocId, newLocalId } from "../lib/ids";
import {
  applyFixes,
  findRecipe,
  graphRefOf,
  newRecipeId,
  recipeNameProblem,
  renameParamInSet,
  replaceRecipe,
  touch,
  withoutValue,
  withValue,
  type Mismatch,
  type RecipeEntry,
  type RecipeSet,
} from "../lib/recipes";
import { effectiveValue, pruneUnknownParams, sparseSet, valueEquals } from "../lib/params";
import {
  augmentOperators,
  composeSubgraph as composeInto,
  dissolveSubgraph as dissolveFrom,
  inlineLibraryNode,
  levelOf,
  fullId,
  pathIsValid,
  promotedBy,
  type ComposeResult,
  type SubPath,
} from "../lib/subgraph";
import { planReplace, type ReplacePlan } from "../lib/replace";
import { canConnect, type ConnectVerdict, type GraphContext } from "../lib/typecheck";
import type { MigrationAction, MigrationEdits } from "../types/execution";
import type { OperatorDesc, Param, SnippetDesc } from "../types/manifest";
import {
  GRAPH_SCHEMA_VERSION,
  LIBRARY_OP_PREFIX,
  subgraphIdOf,
  type GraphDoc,
  type GraphLevel,
  type GraphNode,
  type NodeUi,
  type ParamSpec,
  type PortRef,
  type SubgraphDef,
  type SubParam,
} from "../types/graph";
import { useManifestStore } from "./manifest";
import {
  applyRecipeSet,
  clearBaseEdit,
  currentOverrides,
  noteBaseEdit,
  recipeSet,
  useRecipeStore,
} from "./recipe";
import { useCompareStore } from "./compare";
import { useUiStore } from "./ui";

enablePatches();

const MAX_HISTORY = 100;

/** 一条撤销记录：doc 与内存里的配方集合一起快照（param-recipe K7）—— 改配方值、新建删除配方与改图
 *  进同一个撤销栈，Ctrl+Z 只有一种直觉。当前选着哪个配方不在这里（切换配方不算一步撤销）。 */
export interface HistoryEntry {
  label: string;
  doc: GraphDoc;
  recipes: RecipeSet;
}

const nowIso = () => new Date().toISOString();

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

/** 迁移诊断里的连线改写（ADR-0025），与 bridge 的 GraphDoc::apply_migration 同一套语义：
 *  删边、插节点（摆在被迁移节点的左下方）、加边。id 撞了就加 _2、_3…… */
function applyMigrationEdits(d: GraphDoc, nodeId: string, edits: MigrationEdits): void {
  const same = (a: PortRef, b: PortRef) => a.node === b.node && a.port === b.port;
  d.edges = d.edges.filter(
    (e) => !edits.removeEdges.some((r) => same(e.from, r.from) && same(e.to, r.to)),
  );
  const near = d.nodes.find((n) => n.id === nodeId)?.ui?.position;
  edits.addNodes.forEach((n, k) => {
    if (d.nodes.some((x) => x.id === n.id)) return;
    const ui: NodeUi = {};
    if (near) ui.position = { x: near.x - 220, y: near.y + 140 * (k + 1) };
    if (n.title) ui.title = n.title;
    d.nodes.push({
      id: n.id,
      op: n.op,
      ...(n.opVersion ? { opVersion: n.opVersion } : {}),
      params: { ...n.params },
      ...(Object.keys(ui).length > 0 ? { ui } : {}),
    });
  });
  for (const e of edits.addEdges) {
    const base = e.id ?? "m";
    let id = base;
    for (let k = 2; d.edges.some((x) => x.id === id); k += 1) id = `${base}_${k}`;
    d.edges.push({ id, from: { ...e.from }, to: { ...e.to } });
  }
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

/** 「纳入配方」在 draft 上的那一步（promoteToGraphParam 与 K6 ② 的「改为只在本配方生效」共用）：
 *  逐层提升到顶层、在顶层加图参数（规格从声明抄，default = value）、删掉顶层节点上的显式值。 */
function promoteInDraft(
  d: GraphDoc,
  path: SubPath,
  nodeId: string,
  decl: Param,
  value: unknown,
  name: string,
  label: string,
): boolean {
  const top = liftToTop(d, path, nodeId, decl, value);
  const target = top ? d.nodes.find((n) => n.id === top.node) : undefined;
  if (!top || !target) return false;
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
  return true;
}

export interface PasteResult {
  nodeIds: string[];
  /** 原件 id → 副本 id（粘贴时剪贴板里的 id → 新 id）。 */
  idMap: ReadonlyMap<string, string>;
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
  /** 同一个事务开始时的配方集合（K7：拖着滑块改的可能是配方里的值）。 */
  pendingRecipes: RecipeSet | null;

  /** 最近一次被拒绝的操作原因，给 UI 弹提示用。 */
  lastRejection: string | null;

  // -- 事务 ---------------------------------------------------------------
  /** 拖动/滑块这类连续操作：开始时拍一张，结束时整体记一条撤销。 */
  begin(): void;
  /** 收尾一段事务。不给 label 就按这段里实际改了什么起名（「修改 体素 · 体素边长」「移动 3 个节点」）。 */
  commit(label?: string): void;
  /** 一次走好几步（撤销历史列表里点一行）：负数撤销、正数重做，到头就停。返回实际走了几步（带符号）。 */
  travel(steps: number): number;
  /** 一个手势里的几个动作记成一条撤销（删掉选中的节点与连线、加节点再接上……）：fn 里照常调各个动作。
   *  外面已经有事务（拖动中、外层的 batch）就并进那一条，由开它的那一方记；fn 里调 cancel() 撤回 fn
   *  做的全部改动、不记撤销。以前这些手势记成好几条，Ctrl+Z 一次只撤回一半。 */
  batch<T>(label: string, fn: (cancel: () => void) => T): T;

  // -- 语义化动作 ---------------------------------------------------------
  addNode(opId: string, position: { x: number; y: number }): string | null;
  /** 拖入节点：加节点 + 按类型自动连线（L13），整个算一条撤销。界面的三种加节点方式都走它；
   *  addNode 保持「只加节点」，脚本与验收用它精确搭图。 */
  addNodeAuto(opId: string, position: { x: number; y: number }): AutoConnectResult;
  /** 插入片段：带自动连线的粘贴（L14）。插完是普通节点，没有展开 / 收回。 */
  insertSnippet(snippet: SnippetDesc, at: { x: number; y: number }): AutoConnectResult;
  deleteNodes(ids: readonly string[]): void;
  /** 删节点并把上下游接回去（规则同静音透传，lib/autoconnect 的 healPlan）。一条撤销。 */
  deleteNodesHealing(ids: readonly string[]): { wired: number; unresolved: number };
  moveNodes(moves: readonly { id: string; position: { x: number; y: number } }[]): void;
  /** at：节点所在的层级，不给 = 当前层级（ui.path）。参数面板展开进子图定义时给的是那一层的路径。 */
  setParam(nodeId: string, name: string, value: unknown, at?: SubPath): void;
  /** 同一个参数一次写进几个节点（多选时的检查器）。每个节点照 setParam 的路由（被图参数绑定的改图参数、
   *  选着配方时记一笔），整体一条撤销；已经在外层事务里（数字框拖动的 begin / commit）就并进去。 */
  /** value 给函数时按每个节点此刻的值算（多选里的相对改法）：先全读出来再写 —— 两个节点绑着同一个图参数时只改一次。
   *  算出来没变的节点不写；一个都没变就不记撤销。 */
  setParamMany(nodeIds: readonly string[], name: string, value: unknown | ((cur: unknown, nodeId: string) => unknown)): void;
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
  /** 原地复制选中节点（Ctrl+D）。keepInputs：副本的输入接到原件的同一个上游、输出空着（Shift+D，并排调两组参数比一比）；
   *  在子图里，从子图入口进来的那几条（不是边，是 inputs[].to）也接上。一条撤销。 */
  duplicateNodes(ids: readonly string[], opts?: { keepInputs?: boolean }): PasteResult;
  /** 把 C++ 给的迁移动作写回 doc（ADR-0008）。返回真正改动的节点数。 */
  applyMigrations(actions: readonly MigrationAction[]): number;

  // -- 子图（ADR-0010）-----------------------------------------------------
  /** 把选中的节点合成一个子图。返回新节点与子图的 id。 */
  composeSubgraph(ids: readonly string[]): ComposeResult | null;
  /** 解散一个子图节点，内容内联回本层。返回内联出来的节点 id。 */
  dissolveSubgraph(nodeId: string): string[];
  /** 把这一层的一个节点换成别的算子（右键「换成别的算子…」）：id、位置、标题、静音照旧，连线、参数、绑定按
   *  lib/replace 的规则能留的留下。一条撤销。换不了（子图出口会断、算子不存在）返回 null 并写 lastRejection。 */
  replaceNodeOp(nodeId: string, opId: string): ReplacePlan | null;
  /** 库算子「展开为内联子图」：def 是 core 给的库定义（transport.getLibraryDefinition）。
   *  一条撤销。返回新子图的 id；不是库算子时 null、什么都不改。 */
  inlineLibrary(nodeId: string, def: SubgraphDef): string | null;
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

  // -- 配方（param-recipe P3）：改配方集合的动作都在这里，因为撤销栈在这里（K7）-------------
  /** 在某个配方里写一个值（稀疏：等于基础就删掉这条覆盖）。滑块拖动时在 begin/commit 里合成一条撤销。 */
  setRecipeValue(recipe: string, param: string, value: unknown): void;
  /** 「恢复基础」：删掉这条覆盖（P3.4）。 */
  clearRecipeValue(recipe: string, param: string): void;
  /** 「写回基础」：把这个配方的值写进 default，再删掉这条覆盖（P3.4）。一次撤销。 */
  writeRecipeValueToBase(recipe: string, param: string): void;
  /** 矩阵的「复制选中 → 配方」（P3.5）：目标配方里这些参数的有效值变成给定的值。返回真正改了几格。 */
  copyRecipeCells(cells: readonly { param: string; value: unknown }[], target: string): number;
  /** 新建配方：空的，或复制 copyFrom 的值（P3.6）。名字不合法返回 false 并写 lastRejection。 */
  createRecipe(name: string, copyFrom?: string | null): boolean;
  renameRecipe(name: string, next: string): boolean;
  deleteRecipe(name: string): void;
  /** 设为默认（index.json 的 default；打开图时选它）。null = 取消默认。 */
  setDefaultRecipe(name: string | null): void;
  /** 导入一个外部配方（已经读好、名字已经定好）。存盘时写进配方目录。 */
  addImportedRecipe(entry: RecipeEntry): boolean;
  /** 按失配报告的建议修（单条或整份，P3.7）。一次撤销。 */
  fixRecipe(name: string, items: readonly Mismatch[]): void;
  /** 用磁盘上的版本替换这几个配方（存盘前发现文件被外部修改、用户选「重新载入」）。一次撤销。 */
  replaceRecipes(label: string, next: RecipeSet): void;
  /** K6 ② 的「改为只在本配方生效」：刚才在基础上的那次改动撤回、这个参数纳入配方（default = 改之前的值）、
   *  新值写进当前配方。整个是一次撤销。返回新图参数名。 */
  moveBaseEditToRecipe(key: string): string | null;

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
  /** 从自动备份恢复出来的图：内容不在盘上，算没保存（标题带 *、关窗口会问）。保存点清空 —— 撤销回不到「已保存」。 */
  markUnsaved(): void;
  setName(name: string): void;
  clearRejection(): void;
}

/** 改配方集合的一步：拿到改完的 doc（有的动作先改图再改配方，比如「写回基础」要新的 default 判稀疏）。 */
type RecipeStep = (recipes: RecipeSet, doc: GraphDoc) => RecipeSet;

export const useGraphStore = create<GraphState>((set, get) => {
  /** 正在跑的 batch 有几层。> 0 时 transact 只改不记，由 batch 合成一条。 */
  let batchDepth = 0;
  /** 这段事务里实际改了什么（拖动参数、挪节点每帧记一句、去重）：commit 不给名字时拿它起名，撤销历史里看得出是哪一步。 */
  let hints: string[] = [];
  const note = (h: string) => {
    if (get().pendingSnapshot && !hints.includes(h)) hints.push(h);
  };
  const hintLabel = (): string | null =>
    hints.length === 0 ? null : hints.length === 1 ? hints[0]! : `${hints[0]} 等 ${hints.length} 处`;

  /** 记一条撤销，然后应用变更。用于单步操作。recipes 给了就在同一步里改配方集合（K7）。 */
  const transact = (label: string, recipe: (draft: GraphDoc) => void, recipes?: RecipeStep) => {
    if (batchDepth > 0) {
      mutate(recipe, recipes);
      return;
    }
    const { doc, past } = get();
    const before = recipeSet();
    const next = produce(doc, recipe);
    const nextRecipes = recipes ? recipes(before, next) : before;
    if (next === doc && nextRecipes === before) return; // 什么都没改，不要污染撤销栈
    set({
      doc: next,
      past: [...past, { label, doc, recipes: before }].slice(-MAX_HISTORY),
      future: [],
      dirty: next !== get().savedDoc,
    });
    applyRecipeSet(nextRecipes);
  };

  /** 应用变更但不记撤销。用于事务进行中的中间状态（拖动的每一帧）。 */
  const mutate = (recipe: (draft: GraphDoc) => void, recipes?: RecipeStep) => {
    const { doc } = get();
    const next = produce(doc, recipe);
    if (next !== doc) set({ doc: next, dirty: next !== get().savedDoc });
    if (recipes) applyRecipeSet(recipes(recipeSet(), next));
  };

  /** 只改一个配方的一步（改值、恢复基础、修失配）。配方不存在时什么都不做。 */
  const editRecipe = (label: string, name: string, fn: (e: RecipeEntry, doc: GraphDoc) => RecipeEntry) => {
    const step: RecipeStep = (recipes, doc) => {
      const entry = findRecipe(recipes, name);
      if (!entry) return recipes;
      const next = fn(entry, doc);
      return next === entry ? recipes : replaceRecipe(recipes, name, touch(next, doc, nowIso()));
    };
    if (get().pendingSnapshot) mutate(() => {}, step);
    else transact(label, () => {}, step);
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
    pendingRecipes: null,
    lastRejection: null,

    begin() {
      // 已经在事务里就不要覆盖起点 —— 嵌套 begin 应当是幂等的
      if (get().pendingSnapshot) return;
      hints = [];
      set({ pendingSnapshot: get().doc, pendingRecipes: recipeSet() });
    },

    commit(label) {
      const { pendingSnapshot, pendingRecipes, doc, past } = get();
      if (!pendingSnapshot) return;
      const named = label ?? hintLabel() ?? "编辑";
      hints = [];
      const recipesBefore = pendingRecipes ?? recipeSet();
      if (pendingSnapshot === doc && recipesBefore === recipeSet()) {
        set({ pendingSnapshot: null, pendingRecipes: null }); // 拖了但没动，不记
        return;
      }
      set({
        past: [...past, { label: named, doc: pendingSnapshot, recipes: recipesBefore }].slice(-MAX_HISTORY),
        future: [],
        pendingSnapshot: null,
        pendingRecipes: null,
        dirty: doc !== get().savedDoc,
      });
    },

    batch(label, fn) {
      const opened = get().pendingSnapshot === null;
      if (opened) get().begin();
      const start = { doc: get().doc, recipes: recipeSet() };
      let cancelled = false;
      batchDepth += 1;
      try {
        return fn(() => {
          cancelled = true;
        });
      } finally {
        batchDepth -= 1;
        if (cancelled) {
          set({ doc: start.doc, dirty: start.doc !== get().savedDoc });
          applyRecipeSet(start.recipes);
        }
        // 没撤回、也没改动时 commit 自己不记
        if (opened) get().commit(label);
      }
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
        // 子图里：子图入口接到被删节点上的那几条（inputs[].to）也摘掉，以前留着，下次运行 core 报 unknown_port
        if (lvl !== d) {
          for (const input of (lvl as SubgraphDef).inputs ?? []) {
            const kept = input.to.filter((t) => !kill.has(t.node));
            if (kept.length !== input.to.length) input.to = kept;
          }
        }
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

    deleteNodesHealing(ids) {
      if (ids.length === 0) return { wired: 0, unresolved: 0 };
      const doc = get().doc;
      const plan = healPlan(ctx(doc), levelDoc(doc), new Set(ids));
      let wired = 0;
      get().batch(ids.length === 1 ? "删除并接通" : `删除 ${ids.length} 个节点并接通`, () => {
        get().deleteNodes(ids);
        for (const w of plan.wires) if (get().connect(w.from, w.to).ok) wired += 1;
      });
      return { wired, unresolved: plan.unresolved + plan.wires.length - wired };
    },

    moveNodes(moves) {
      if (moves.length === 0) return;
      // 拖动过程中每帧都调，所以走 mutate 不记撤销；
      // 一次拖动的撤销由 begin/commit 包住整体记一条。
      // 下标在原图（不是 draft）上查好，draft 上只碰挪了的那几个：不按挪动逐个 find（全选拖几百个节点时每帧平方级），
      // 也不在 draft 上把节点挨个读一遍（immer 读一个就建一个代理，一千个节点的图上拖一个节点每帧上千个）
      const index = new Map<string, number>();
      level(get().doc).nodes.forEach((n, i) => index.set(n.id, i));
      const at = moves.flatMap((m) => {
        const i = index.get(m.id);
        return i === undefined ? [] : [[i, m.position] as const];
      });
      if (at.length === 0) return;
      if (get().pendingSnapshot) {
        const one = at.length === 1 ? level(get().doc).nodes[at[0]![0]] : undefined;
        note(one ? `移动 ${one.ui?.title ?? ctx(get().doc).operatorsById.get(one.op)?.label ?? one.id}` : `移动 ${at.length} 个节点`);
      }
      mutate((d) => {
        const nodes = level(d).nodes;
        for (const [i, position] of at) {
          const node = nodes[i]!;
          node.ui = { ...node.ui, position };
        }
      });
    },

    setParam(nodeId, name, value, at) {
      const { doc } = get();
      const path = at ?? useUiStore.getState().path;
      // 被图参数绑定的参数（P1.4）：改的是图参数，不写成节点上的显式值 —— 那正是 param_conflict
      // 的来路。Inspector、参数面板、2D 拖框、粘贴、重置都经这里，所以在这一处路由而不是各处各判一遍。
      const binding = resolveGraphBinding(doc, path, nodeId, name);
      if (binding) {
        note(`修改图参数 ${doc.params?.[binding.graphParam]?.label ?? binding.graphParam}`);
        get().editGraphParamValue(binding.graphParam, value);
        return;
      }
      const node = levelAt(doc, at).nodes.find((n) => n.id === nodeId);
      if (!node) return;
      const op = ctx(doc).operatorsById.get(node.op);
      if (!op) return;
      // 撤销记录里写节点的名字与参数的 label（以前是「修改 Voxel Grid.leafSize」「拖动参数」，几个同类节点分不出是哪个）
      const what = `修改 ${node.ui?.title ?? op.label} · ${op.params.find((p) => p.name === name)?.label || name}`;

      // K6 ②：选着配方时改一个没纳入配方的参数 —— 照常改图（影响所有配方），行上记一笔，
      // 给「改为只在本配方生效」用（它要知道改之前的值）
      const recipe = useRecipeStore.getState().current;
      const before = effectiveValue(op, node, name);
      if (recipe !== null && !valueEquals(before, value)) {
        noteBaseEdit(`${fullId(path, nodeId)}.${name}`, {
          nodeId,
          param: name,
          path,
          before: plain(before),
          after: plain(value),
          recipe,
        });
      }

      const nextParams = sparseSet(op, node.params, name, plain(value));
      const apply = (d: GraphDoc) => {
        const target = levelAt(d, at).nodes.find((n) => n.id === nodeId);
        if (target) target.params = nextParams;
      };

      // 滑块拖动时 setParam 每帧都来，靠外层 begin/commit 合成一条撤销。
      if (get().pendingSnapshot) {
        note(what);
        mutate(apply);
      } else transact(what, apply);
    },

    setParamMany(nodeIds, name, value) {
      if (nodeIds.length === 0) return;
      let writes: { id: string; value: unknown }[] = nodeIds.map((id) => ({ id, value }));
      if (typeof value === "function") {
        const { doc } = get();
        const path = useUiStore.getState().path;
        const lvl = levelAt(doc, undefined);
        const ops = ctx(doc).operatorsById;
        const update = value as (cur: unknown, nodeId: string) => unknown;
        writes = [];
        for (const id of nodeIds) {
          const binding = resolveGraphBinding(doc, path, id, name);
          const node = lvl.nodes.find((n) => n.id === id);
          const op = node ? ops.get(node.op) : undefined;
          const cur = binding ? graphParamValue(doc, binding.graphParam, currentOverrides()) : node && op ? effectiveValue(op, node, name) : undefined;
          if (cur === undefined) continue;
          const next = update(cur, id);
          if (!valueEquals(next, cur)) writes.push({ id, value: next });
        }
        if (writes.length === 0) return;
      }
      const outer = get().pendingSnapshot !== null;
      if (!outer) get().begin();
      for (const w of writes) get().setParam(w.id, name, w.value);
      if (!outer) get().commit(`修改 ${nodeIds.length} 个节点的 ${name}`);
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
          // 静音的节点粘出来还是静音的（以前丢了）
          ...(n.bypass ? { bypass: true } : {}),
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

      if (newNodes.length === 0) return { nodeIds: [], idMap };

      transact(newNodes.length === 1 ? "粘贴节点" : `粘贴 ${newNodes.length} 个节点`, (d) => {
        const lvl = level(d);
        lvl.nodes.push(...newNodes);
        lvl.edges.push(...newEdges);
      });
      return { nodeIds: newNodes.map((n) => n.id), idMap };
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
      // null = 回到 manifest 的 label。没变就什么都不做：不记撤销、不把文档标成改过
      const next = title && title.trim() ? title : null;
      const current = level(get().doc).nodes.find((n) => n.id === id);
      if (!current || (current.ui?.title ?? null) === next) return;
      transact("重命名节点", (d) => {
        const node = level(d).nodes.find((n) => n.id === id);
        if (!node) return;
        node.ui = { ...node.ui, title: next };
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

    duplicateNodes(ids, opts) {
      if (ids.length === 0) return { nodeIds: [], idMap: new Map() };
      const { doc } = get();
      const lvl = level(doc);
      const kept = new Set(ids);
      // 被图参数绑定的参数写成此刻的有效值：副本不带绑定，原来会悄悄回到算子默认值
      const nodes = materializeBindings(
        doc,
        useUiStore.getState().path,
        lvl.nodes.filter((n) => kept.has(n.id)),
        ctx(doc).operatorsById,
        currentOverrides(),
      );
      if (nodes.length === 0) return { nodeIds: [], idMap: new Map() };
      const edges = lvl.edges.filter((e) => kept.has(e.from.node) && kept.has(e.to.node));
      const origin = nodes.reduce(
        (acc, n) => ({
          x: Math.min(acc.x, n.ui?.position?.x ?? 0),
          y: Math.min(acc.y, n.ui?.position?.y ?? 0),
        }),
        { x: Infinity, y: Infinity },
      );
      // 偏移一点，否则复制出来的节点完全盖在原件上，用户以为什么都没发生
      const paste = () =>
        get().pasteNodes(
          { nodes: JSON.parse(JSON.stringify(nodes)), edges: JSON.parse(JSON.stringify(edges)) },
          { x: (Number.isFinite(origin.x) ? origin.x : 0) + 40, y: (Number.isFinite(origin.y) ? origin.y : 0) + 40 },
        );
      if (!opts?.keepInputs) return paste();
      const label = nodes.length === 1 ? "复制并保留输入" : `复制 ${nodes.length} 个节点并保留输入`;
      return get().batch(label, () => {
        const made = paste();
        // 从选区外面进来的边：副本也接一条（选区里面的连线 pasteNodes 已经照着复制了）
        for (const e of lvl.edges) {
          const copy = made.idMap.get(e.to.node);
          if (copy && !made.idMap.has(e.from.node)) get().connect(e.from, { node: copy, port: e.to.port });
        }
        // 在子图里：从子图入口进来的那几条不是边，是 inputs[].to，副本的端口也加进去
        transact(label, (d) => {
          const def = level(d);
          if (def === d) return;
          for (const input of (def as SubgraphDef).inputs ?? []) {
            const extra = input.to.flatMap((t) => {
              const copy = made.idMap.get(t.node);
              return copy ? [{ node: copy, port: t.port }] : [];
            });
            if (extra.length > 0) input.to.push(...extra);
          }
        });
        return made;
      });
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
          // 连线改写只落在顶层（ADR-0025）：与 lyflow migrate --write 同一套语义
          for (const action of actions) {
            if (action.edits && d.nodes.some((n) => n.id === action.nodeId)) {
              applyMigrationEdits(d, action.nodeId, action.edits);
            }
          }
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

    replaceNodeOp(nodeId, opId) {
      const { doc } = get();
      const path = useUiStore.getState().path;
      const c = ctx(doc);
      const op = c.operatorsById.get(opId);
      const plan = op ? planReplace(c, doc, path, nodeId, op) : null;
      if (!op || !plan || plan.blocked) {
        set({ lastRejection: plan?.blocked ?? `换不了：找不到算子 ${opId}` });
        return null;
      }
      const dropEdges = new Set(plan.droppedEdges);
      const unbind = new Set(plan.unbind);
      const dropInputs = new Set(plan.droppedInputs.map((x) => `${x.input}\u0000${x.port}`));
      transact(`换成 ${op.label}`, (d) => {
        const lvl = level(d);
        const node = lvl.nodes.find((n) => n.id === nodeId);
        if (!node) return;
        node.op = op.id;
        // 写新算子的版本：留着旧的会被当成要迁移
        node.opVersion = op.version;
        node.params = { ...plan.params };
        lvl.edges = lvl.edges.filter((e) => !dropEdges.has(e.id));
        if (lvl === d) {
          for (const gp of Object.values(d.params ?? {})) {
            const kept = gp.binds.filter((b) => {
              const dot = b.lastIndexOf(".");
              return !(b.slice(0, dot) === nodeId && unbind.has(b.slice(dot + 1)));
            });
            if (kept.length !== gp.binds.length) gp.binds = kept;
          }
        } else {
          const def = lvl as SubgraphDef;
          for (const sp of def.params ?? []) {
            const kept = (sp.binds ?? []).filter((b) => !(b.node === nodeId && unbind.has(b.param)));
            if (kept.length !== (sp.binds ?? []).length) sp.binds = kept;
          }
          for (const input of def.inputs ?? []) {
            const kept = input.to.filter((t) => t.node !== nodeId || !dropInputs.has(`${input.name}\u0000${t.port}`));
            if (kept.length !== input.to.length) input.to = kept;
          }
        }
        for (const name of plan.droppedOutputs) {
          if (d.outputs) delete d.outputs[name];
        }
      });
      for (const p of plan.droppedParams) clearBaseEdit(`${fullId(path, nodeId)}.${p}`);
      return plan;
    },

    inlineLibrary(nodeId, def) {
      const path = useUiStore.getState().path;
      const node = levelOf(get().doc, path).nodes.find((n) => n.id === nodeId);
      if (!node || !node.op.startsWith(LIBRARY_OP_PREFIX)) return null;
      let subgraphId: string | null = null;
      transact("展开库算子", (d) => {
        subgraphId = inlineLibraryNode(d, path, nodeId, def);
      });
      return subgraphId;
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
        done = promoteInDraft(d, path, nodeId, decl, value, name, label);
      });
      if (done) clearBaseEdit(`${fullId(path, nodeId)}.${paramName}`);
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
      transact(
        `重命名图参数 ${name} → ${next}`,
        (d) => {
          // 保持键的位置：存盘是给人 diff 的，改个名不该让这一段挪到末尾
          d.params = Object.fromEntries(
            Object.entries(d.params ?? {}).map(([k, v]) => [k === name ? next : k, v]),
          );
        },
        // 内存里每个配方的那个键跟着改名（同一步撤销）：不然改完名所有配方都是一条失配 ①
        (recipes, d) => renameParamInSet(recipes, d, name, next, nowIso()),
      );
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
      // K6 ①：选着某个配方时写进那个配方，选着「基础」时写 default。想改基础就切到「基础」，
      // 或者在这一行点「写回基础」
      const current = useRecipeStore.getState().current;
      if (current === null) get().setGraphParamDefault(name, value);
      else get().setRecipeValue(current, name, value);
    },

    setRecipeValue(recipe, param, value) {
      if (!get().doc.params?.[param]) return;
      editRecipe(`配方 ${recipe}：修改 ${param}`, recipe, (e, doc) => withValue(e, doc, param, value));
    },

    clearRecipeValue(recipe, param) {
      editRecipe(`配方 ${recipe}：${param} 恢复基础`, recipe, (e) => withoutValue(e, param));
    },

    writeRecipeValueToBase(recipe, param) {
      const entry = findRecipe(recipeSet(), recipe);
      if (!entry || !Object.prototype.hasOwnProperty.call(entry.values, param) || !get().doc.params?.[param]) return;
      const value = entry.values[param];
      transact(
        `写回基础 ${param}（来自配方 ${recipe}）`,
        (d) => {
          const into = d.params?.[param];
          if (into) into.default = plain(value);
        },
        (recipes, doc) => {
          const e = findRecipe(recipes, recipe);
          return e ? replaceRecipe(recipes, recipe, touch(withoutValue(e, param), doc, nowIso())) : recipes;
        },
      );
    },

    copyRecipeCells(cells, target) {
      const entry = findRecipe(recipeSet(), target);
      const { doc } = get();
      if (!entry) return 0;
      let next = entry;
      let changed = 0;
      for (const c of cells) {
        if (!doc.params?.[c.param]) continue;
        const after = withValue(next, doc, c.param, c.value);
        if (after !== next) changed += 1;
        next = after;
      }
      if (changed === 0) return 0;
      transact(`复制 ${changed} 格到配方 ${target}`, () => {}, (recipes, d) =>
        replaceRecipe(recipes, target, touch(next, d, nowIso())),
      );
      return changed;
    },

    createRecipe(name, copyFrom) {
      const recipes = recipeSet();
      if (useRecipeStore.getState().dir === null) {
        set({ lastRejection: "图还没存过盘：先保存图，配方存在图文件旁边的 <图名>.recipes/ 里" });
        return false;
      }
      const problem = recipeNameProblem(recipes, name);
      if (problem) {
        set({ lastRejection: problem });
        return false;
      }
      const source = copyFrom ? findRecipe(recipes, copyFrom) : undefined;
      const { doc } = get();
      const entry: RecipeEntry = {
        id: newRecipeId(),
        name,
        values: source ? structuredClone(source.values) : {},
        note: source?.note,
        graph: graphRefOf(doc),
        updatedAt: nowIso(),
      };
      transact(source ? `复制配方 ${source.name} → ${name}` : `新建配方 ${name}`, () => {}, (r) => ({
        ...r,
        recipes: [...r.recipes, entry],
      }));
      return true;
    },

    renameRecipe(name, next) {
      const recipes = recipeSet();
      const entry = findRecipe(recipes, name);
      if (!entry) return false;
      if (next === name) return true;
      const problem = recipeNameProblem(recipes, next, name);
      if (problem) {
        set({ lastRejection: problem });
        return false;
      }
      // 当前配方改了名照样是当前配方：recipe store 按内存 id 认它
      transact(`重命名配方 ${name} → ${next}`, () => {}, (r, doc) =>
        replaceRecipe(r, name, touch({ ...entry, name: next }, doc, nowIso())),
      );
      return true;
    },

    deleteRecipe(name) {
      if (!findRecipe(recipeSet(), name)) return;
      transact(`删除配方 ${name}`, () => {}, (r) => ({
        recipes: r.recipes.filter((e) => e.name !== name),
        defaultName: r.defaultName === name ? null : r.defaultName,
      }));
    },

    setDefaultRecipe(name) {
      const recipes = recipeSet();
      if (name !== null && !findRecipe(recipes, name)) return;
      if (recipes.defaultName === name) return;
      transact(name ? `设 ${name} 为默认配方` : "取消默认配方", () => {}, (r) => ({ ...r, defaultName: name }));
    },

    addImportedRecipe(entry) {
      const recipes = recipeSet();
      if (useRecipeStore.getState().dir === null) {
        set({ lastRejection: "图还没存过盘：先保存图，配方存在图文件旁边的 <图名>.recipes/ 里" });
        return false;
      }
      const problem = recipeNameProblem(recipes, entry.name);
      if (problem) {
        set({ lastRejection: problem });
        return false;
      }
      transact(`导入配方 ${entry.name}`, () => {}, (r) => ({ ...r, recipes: [...r.recipes, entry] }));
      return true;
    },

    fixRecipe(name, items) {
      if (items.length === 0) return;
      if (!findRecipe(recipeSet(), name)) return;
      const label = items.length === 1 ? `修复配方 ${name} 的失配` : `修复配方 ${name} 的 ${items.length} 处失配`;
      transact(label, () => {}, (recipes, doc) => {
        const e = findRecipe(recipes, name);
        if (!e) return recipes;
        const next = applyFixes(e, items, doc, nowIso());
        return next === e ? recipes : replaceRecipe(recipes, name, next);
      });
    },

    replaceRecipes(label, next) {
      transact(label, () => {}, () => next);
    },

    moveBaseEditToRecipe(key) {
      const edit = useRecipeStore.getState().baseEdits[key];
      const current = useRecipeStore.getState().current;
      if (!edit || current === null || edit.recipe !== current) return null;
      const found = lookupParam(edit.nodeId, edit.param, edit.path);
      if (!found) return null;
      const { doc, path, ops, op, decl } = found;
      if (resolveGraphBinding(doc, path, edit.nodeId, edit.param)) return null;
      const name = uniqueGraphParamName(doc, edit.param);
      const label = graphParamLabel(doc, path, edit.nodeId, decl, ops);
      let done = false;
      transact(
        `改为只在配方 ${current} 生效：${name}`,
        (d) => {
          // 节点上的值回到改之前 —— 它成为新图参数的 default，别的配方看到的还是原来那样
          const target = levelOf(d, path).nodes.find((n) => n.id === edit.nodeId);
          if (!target) return;
          target.params = sparseSet(op, target.params, edit.param, plain(edit.before));
          done = promoteInDraft(d, path, edit.nodeId, decl, edit.before, name, label);
        },
        (recipes, next) => {
          const e = findRecipe(recipes, current);
          if (!e || !next.params?.[name]) return recipes;
          return replaceRecipe(recipes, current, touch(withValue(e, next, name, edit.after), next, nowIso()));
        },
      );
      if (!done) return null;
      clearBaseEdit(key);
      return name;
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
        future: [...future, { label: entry.label, doc, recipes: recipeSet() }],
        // 撤销回到保存点 = 文件里就是这一份，标题栏的 ● 该消失（P1.6）
        dirty: entry.doc !== get().savedDoc,
        pendingSnapshot: null,
        pendingRecipes: null,
      });
      applyRecipeSet(entry.recipes);
    },

    redo() {
      const { past, future, doc } = get();
      const entry = future[future.length - 1];
      if (!entry) return;
      set({
        doc: entry.doc,
        future: future.slice(0, -1),
        past: [...past, { label: entry.label, doc, recipes: recipeSet() }],
        dirty: entry.doc !== get().savedDoc,
        pendingSnapshot: null,
        pendingRecipes: null,
      });
      applyRecipeSet(entry.recipes);
    },

    travel(steps) {
      if (steps === 0) return 0;
      let { past, future, doc } = get();
      let recipes = recipeSet();
      past = past.slice();
      future = future.slice();
      const from = steps < 0 ? past : future;
      const to = steps < 0 ? future : past;
      let moved = 0;
      while (moved < Math.abs(steps) && from.length > 0) {
        const entry = from.pop()!;
        to.push({ label: entry.label, doc, recipes });
        doc = entry.doc;
        recipes = entry.recipes;
        moved += 1;
      }
      if (moved === 0) return 0;
      hints = [];
      set({ doc, past, future, dirty: doc !== get().savedDoc, pendingSnapshot: null, pendingRecipes: null });
      applyRecipeSet(recipes);
      return steps < 0 ? -moved : moved;
    },

    canUndo: () => get().past.length > 0,
    canRedo: () => get().future.length > 0,

    newDoc() {
      useUiStore.getState().setPath([]);
      // 新图里可能恰好有同 id 的节点，prune 认不出「已经不是那个节点了」
      useCompareStore.getState().exit();
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
        pendingRecipes: null,
      });
    },

    loadDoc(doc, path) {
      useUiStore.getState().setPath([]);
      useCompareStore.getState().exit();
      set({
        doc,
        savedDoc: doc,
        filePath: path,
        dirty: false,
        epoch: get().epoch + 1,
        past: [],
        future: [],
        pendingSnapshot: null,
        pendingRecipes: null,
      });
    },

    markSaved(path, saved) {
      const doc = saved ?? get().doc;
      set({ filePath: path, savedDoc: doc, dirty: get().doc !== doc });
    },

    markUnsaved() {
      set({ savedDoc: null, dirty: true });
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

/** 配方集合的异步载入落地时，把历史里还指着「载入前那份空集合」的快照换成载入的结果 ——
 *  否则打开图之后、配方读完之前做的那一步，撤销时会把配方集合一起撤成空的。 */
export function rebaseHistoryRecipes(from: RecipeSet, to: RecipeSet): void {
  const s = useGraphStore.getState();
  const fix = (list: HistoryEntry[]) =>
    list.some((e) => e.recipes === from) ? list.map((e) => (e.recipes === from ? { ...e, recipes: to } : e)) : list;
  const past = fix(s.past);
  const future = fix(s.future);
  if (past !== s.past || future !== s.future) useGraphStore.setState({ past, future });
}

/** 当前层级的子图定义。Inspector 与画布都要读它。 */
export function currentSubgraph(doc: GraphDoc, path: readonly { subgraphId: string }[]) {
  const last = path[path.length - 1];
  return last ? doc.subgraphs?.[last.subgraphId] : undefined;
}
