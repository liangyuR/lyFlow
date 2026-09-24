// 参数面板「按节点」页的数据模型（param-recipe P2.3–P2.5、P2.8）：从 doc + manifest 收齐所有行，
// 再按搜索、过滤 chip 与折叠状态压成一条扁平的列表喂给虚拟化列表。纯函数，不碰 store 与 DOM
// （node --test 直接测；e2e 的期望值也照这里写明的判据对着 doc 独立算一遍）。
//
// 行的全集 =「图参数」分组的每个图参数 + 当前层级每个节点的每个**可见**参数（visibleWhen 不满足的不算）
// + 子图实例展开进去的定义里的节点（逐层，按实例各算一份：同一个定义在不同实例下的绑定链不同）。
// 库算子（lib.）的定义在库文件里，编辑器拿不到，不展开。

import {
  boundDecl,
  graphParamSpecOf,
  graphParamValue,
  resolveGraphBinding,
  withBoundValues,
  type GraphBinding,
} from "./graphParams";
import { effectiveParams, groupParams, isEnabled, isVisible, valueEquals } from "./params";
import { fullId, pathPrefix, promotedBy, type SubPath } from "./subgraph";
import {
  LIBRARY_OP_PREFIX,
  subgraphIdOf,
  type GraphDoc,
  type GraphLevel,
  type GraphNode,
  type GraphParam,
} from "../types/graph";
import type { OperatorDesc, Param, ParamType } from "../types/manifest";

export interface RowDiag {
  severity: "error" | "warning";
  message: string;
  code?: string | undefined;
}

/** 面板要的诊断来源。键都是**展开后**的节点 id（子图内部是 `outer/inner`）。 */
export interface PanelDiagnostics {
  /** 节点 → 参数名 → 诊断。编辑期校验与上次运行的错误合在一起（同一参数两边都有时校验在前）。 */
  byNode: ReadonlyMap<string, ReadonlyMap<string, readonly RowDiag[]>>;
  /** 图参数名 → 诊断（nodeId 为空、paramPath 是名字的那些，P1.2）。 */
  byGraphParam: ReadonlyMap<string, readonly RowDiag[]>;
}

export const NO_DIAGNOSTICS: PanelDiagnostics = { byNode: new Map(), byGraphParam: new Map() };

interface RowBase {
  key: string;
  type: ParamType | null;
  /** 与算子默认不同（行首的蓝点）。 */
  modified: boolean;
  /** 已纳入配方：本身就是图参数，或值由图参数提供。 */
  recipe: boolean;
  diags: readonly RowDiag[];
  /** 搜索用的全文（小写）：参数名、label、节点标题、节点 id、值的文本。 */
  haystack: string;
}

export interface GraphParamRow extends RowBase {
  kind: "gp";
  name: string;
  gp: GraphParam;
  spec: Param | null;
  value: unknown;
}

export interface NodeParamRow extends RowBase {
  kind: "param";
  /** 节点所在的层级（ui.path 或更深：展开进子图定义的那几行）。 */
  path: SubPath;
  node: GraphNode;
  op: OperatorDesc;
  param: Param;
  /** 展开后的节点 id。诊断、2D 视图缓存都按它认。 */
  fullNodeId: string;
  /** 显示的值：被图参数绑定的换成图参数的有效值（P1.4）。 */
  value: unknown;
  /** 这个节点的完整有效参数（联动条件、ROI 缩略图用）。 */
  effective: Record<string, unknown>;
  enabled: boolean;
  /** 只读的原因。null = 可编辑。 */
  readOnly: string | null;
  binding: GraphBinding | null;
  /** 这个内参被提升成了哪个子图参数（F4）。 */
  promoted: string | null;
  /** 在子图定义里：几个实例共享（改它影响所有实例）。顶层节点是 0。 */
  shared: number;
  /** manifest 里的分组（advanced 的归到「高级」，与 groupParams 同一规则）。 */
  group: string;
  advanced: boolean;
  /** 缩进层数：顶层 0，展开进子图定义一层加一。 */
  depth: number;
}

export type PanelRow = GraphParamRow | NodeParamRow;

export interface NodeSection {
  key: string;
  path: SubPath;
  node: GraphNode;
  op: OperatorDesc | undefined;
  title: string;
  depth: number;
  fullNodeId: string;
  rows: NodeParamRow[];
  /** 子图实例：定义的 id 与定义里的节点（逐层展开）。 */
  subgraphId: string | null;
  children: NodeSection[];
  /** 这一节在子图定义里时，那个定义被几个实例共享（顶层是 0）。 */
  shared: number;
  library: boolean;
}

export interface PanelModel {
  graphRows: GraphParamRow[];
  sections: NodeSection[];
}

const MAX_DEPTH = 8;

/** 值的文本（搜索用）。字符串原样，其余 JSON。 */
function valueText(v: unknown): string {
  if (typeof v === "string") return v;
  try {
    return JSON.stringify(v) ?? "";
  } catch {
    return "";
  }
}

function titleOf(node: GraphNode, op: OperatorDesc | undefined): string {
  return node.ui?.title || op?.label || node.id;
}

/** 整张 doc 里用着这个子图定义的节点数（顶层 + 所有定义里）。 */
export function instanceCount(doc: GraphDoc, subgraphId: string): number {
  let n = 0;
  const count = (lvl: GraphLevel) => {
    for (const node of lvl.nodes) if (subgraphIdOf(node.op) === subgraphId) n += 1;
  };
  count(doc);
  for (const def of Object.values(doc.subgraphs ?? {})) count(def);
  return n;
}

export interface BuildInput {
  doc: GraphDoc;
  /** manifest 的算子表合上本文档的 sub: 合成项（augmentOperators）。 */
  ops: ReadonlyMap<string, OperatorDesc>;
  path: SubPath;
  overrides: Readonly<Record<string, unknown>>;
  diagnostics: PanelDiagnostics;
}

const NO_DIAGS: readonly RowDiag[] = [];

export function buildPanelModel({ doc, ops, path, overrides, diagnostics }: BuildInput): PanelModel {
  const graphRows: GraphParamRow[] = [];
  for (const [name, gp] of Object.entries(doc.params ?? {})) {
    const spec = graphParamSpecOf(doc, name, gp, ops);
    const value = graphParamValue(doc, name, overrides);
    const decl = boundDecl(doc, gp, ops);
    graphRows.push({
      kind: "gp",
      key: `gp:${name}`,
      name,
      gp,
      spec,
      value,
      type: spec?.type ?? null,
      modified: decl ? !valueEquals(value, decl.default) : false,
      recipe: true,
      diags: diagnostics.byGraphParam.get(name) ?? NO_DIAGS,
      haystack: [name, gp.label ?? "", gp.binds.join(" "), valueText(value), "图参数"].join(" ").toLowerCase(),
    });
  }

  const sectionsOf = (level: GraphLevel, at: SubPath, depth: number, shared: number): NodeSection[] => {
    const def = at.length > 0 ? doc.subgraphs?.[at[at.length - 1]!.subgraphId] : undefined;
    return level.nodes.map((node) => {
      const op = ops.get(node.op);
      const title = titleOf(node, op);
      const full = fullId(at, node.id);
      const key = `n:${full}`;
      const rows: NodeParamRow[] = [];
      if (op) {
        const names = op.params.map((p) => p.name);
        const shown = withBoundValues(doc, at, node, names, overrides);
        const effective = effectiveParams(op, shown);
        const diagMap = diagnostics.byNode.get(full);
        for (const g of groupParams(op.params)) {
          for (const param of g.params) {
            if (!isVisible(param, effective)) continue;
            const binding = resolveGraphBinding(doc, at, node.id, param.name);
            const promoted = promotedBy(def, node.id, param.name)?.name ?? null;
            const value = effective[param.name];
            rows.push({
              kind: "param",
              key: `${full}.${param.name}`,
              path: at,
              node,
              op,
              param,
              fullNodeId: full,
              value,
              effective,
              enabled: isEnabled(param, effective),
              // 已提升的内参在内部只读（F4）；整条链通到图参数的例外，改它 = 改图参数（P1 的取舍 5）
              readOnly: promoted && !binding ? `已提升为子图参数 ${promoted}，在外层实例上改` : null,
              binding,
              promoted,
              shared,
              group: g.name,
              advanced: g.advanced,
              depth,
              type: param.type,
              modified: !valueEquals(value, param.default),
              recipe: binding !== null,
              diags: diagMap?.get(param.name) ?? NO_DIAGS,
              // 节点 id 用展开后的全路径：子图定义里的行按实例（`s2/in1`）也搜得到
              haystack: [param.name, param.label ?? "", title, full, valueText(value)].join(" ").toLowerCase(),
            });
          }
        }
      }
      const subgraphId = subgraphIdOf(node.op);
      const inner = subgraphId ? doc.subgraphs?.[subgraphId] : undefined;
      const children =
        inner && depth < MAX_DEPTH
          ? sectionsOf(inner, [...at, { nodeId: node.id, subgraphId: subgraphId! }], depth + 1,
              instanceCount(doc, subgraphId!))
          : [];
      return {
        key,
        path: at,
        node,
        op,
        title,
        depth,
        fullNodeId: full,
        rows,
        subgraphId: inner ? subgraphId : null,
        children,
        shared,
        library: node.op.startsWith(LIBRARY_OP_PREFIX),
      };
    });
  };

  const level: GraphLevel =
    path.length === 0 ? doc : (doc.subgraphs?.[path[path.length - 1]!.subgraphId] ?? doc);
  return { graphRows, sections: sectionsOf(level, path, 0, path.length === 0 ? 0 : sharedOf(doc, path)) };
}

/** 当前层级本身就在子图定义里（进了子图）时，这一层被几个实例共享。 */
function sharedOf(doc: GraphDoc, path: SubPath): number {
  const last = path[path.length - 1];
  return last ? instanceCount(doc, last.subgraphId) : 0;
}

// ------------------------------------------------------------ 过滤与压平

export type PanelChip = "all" | "modified" | "recipe" | "diag";

export interface PanelFilter {
  query: string;
  chip: PanelChip;
  type: ParamType | null;
  /** 折叠状态里「与默认相反」的那些键。默认：图参数分组、节点、普通组展开；advanced 组、子图定义收起。 */
  toggled: Readonly<Record<string, boolean>>;
}

export type PanelItem =
  | { kind: "gp-head"; key: string; open: boolean; count: number; errors: number }
  | { kind: "gp-row"; key: string; row: GraphParamRow }
  | {
      kind: "node-head";
      key: string;
      section: NodeSection;
      open: boolean;
      total: number;
      modified: number;
      /** 过滤生效时这一节里命中的行数（含展开进去的定义）。 */
      matched: number | null;
    }
  | { kind: "group-head"; key: string; name: string; advanced: boolean; open: boolean; count: number; depth: number }
  | { kind: "def-head"; key: string; section: NodeSection; open: boolean; shared: number; nodes: number }
  | { kind: "row"; key: string; row: NodeParamRow }
  | { kind: "empty"; key: string; text: string };

export interface PanelCounts {
  all: number;
  modified: number;
  recipe: number;
  diag: number;
  /** 当前搜索 + chip 下各类型的行数（「类型 ▾」下拉用）。 */
  byType: Partial<Record<ParamType, number>>;
}

export interface PanelView {
  items: PanelItem[];
  counts: PanelCounts;
  /** 过滤是否生效（有搜索词、chip 不是「全部」或选了类型）。生效时折叠一概不算。 */
  filtering: boolean;
}

function chipHolds(row: PanelRow, chip: PanelChip): boolean {
  switch (chip) {
    case "all":
      return true;
    case "modified":
      return row.modified;
    case "recipe":
      return row.recipe;
    case "diag":
      return row.diags.length > 0;
  }
}

/** 搜索词按空白切开，每一段都要出现在全文里（不分大小写）。 */
function queryHolds(row: PanelRow, terms: readonly string[]): boolean {
  return terms.every((t) => row.haystack.includes(t));
}

export function queryTerms(query: string): string[] {
  return query.toLowerCase().split(/\s+/).filter(Boolean);
}

/** 模型里的全部行（图参数 + 各节参数，逐层展开）。 */
export function allRows(model: PanelModel): PanelRow[] {
  const out: PanelRow[] = [...model.graphRows];
  const walk = (secs: readonly NodeSection[]) => {
    for (const s of secs) {
      out.push(...s.rows);
      walk(s.children);
    }
  };
  walk(model.sections);
  return out;
}

export function countRows(model: PanelModel, filter: Pick<PanelFilter, "query" | "chip">): PanelCounts {
  const terms = queryTerms(filter.query);
  const counts: PanelCounts = { all: 0, modified: 0, recipe: 0, diag: 0, byType: {} };
  for (const row of allRows(model)) {
    if (!queryHolds(row, terms)) continue;
    counts.all += 1;
    if (row.modified) counts.modified += 1;
    if (row.recipe) counts.recipe += 1;
    if (row.diags.length > 0) counts.diag += 1;
    if (row.type && chipHolds(row, filter.chip)) counts.byType[row.type] = (counts.byType[row.type] ?? 0) + 1;
  }
  return counts;
}

export function flattenPanel(model: PanelModel, filter: PanelFilter): PanelView {
  const terms = queryTerms(filter.query);
  const filtering = terms.length > 0 || filter.chip !== "all" || filter.type !== null;
  const passes = (row: PanelRow) =>
    queryHolds(row, terms) && chipHolds(row, filter.chip) && (filter.type === null || row.type === filter.type);
  // 折叠：键在 toggled 里就与默认相反；过滤生效时全部展开（命中的行不能藏在收起的组里）
  const isOpen = (key: string, byDefault: boolean) => filtering || (filter.toggled[key] ? !byDefault : byDefault);

  const items: PanelItem[] = [];

  const gpRows = model.graphRows.filter(passes);
  if (gpRows.length > 0) {
    const open = isOpen("gp", true);
    const errors = model.graphRows.filter((r) => r.diags.some((d) => d.severity === "error")).length;
    items.push({ kind: "gp-head", key: "gp", open, count: filtering ? gpRows.length : model.graphRows.length, errors });
    if (open) for (const row of gpRows) items.push({ kind: "gp-row", key: row.key, row });
  } else if (!filtering && model.graphRows.length === 0) {
    // 还没有图参数：分组照样在最上面，告诉人怎么往里放
    items.push({ kind: "gp-head", key: "gp", open: true, count: 0, errors: 0 });
    items.push({ kind: "empty", key: "gp-empty", text: "还没有图参数：点参数行右端的书签「纳入配方」" });
  }

  /** 一节（及其展开进去的定义）里命中的行数。 */
  const matchedIn = (s: NodeSection): number =>
    s.rows.filter(passes).length + s.children.reduce((n, c) => n + matchedIn(c), 0);

  const emit = (s: NodeSection) => {
    const matched = filtering ? matchedIn(s) : null;
    if (filtering && matched === 0) return;
    const open = isOpen(s.key, true);
    items.push({
      kind: "node-head",
      key: s.key,
      section: s,
      open,
      total: s.rows.length,
      modified: s.rows.filter((r) => r.modified).length,
      matched,
    });
    if (!open) return;
    const rows = s.rows.filter(passes);
    let group: string | null = null;
    let groupOpen = true;
    for (let i = 0; i < rows.length; i += 1) {
      const row = rows[i]!;
      const gkey = `${s.key}|g:${row.group}|${row.advanced ? "a" : "b"}`;
      if (gkey !== group) {
        group = gkey;
        groupOpen = isOpen(gkey, !row.advanced);
        // 没名字的普通组不画标题（与 Inspector 一样）；advanced 组一定有标题（「高级」），它要能点开
        if (row.group || row.advanced) {
          const count = rows.filter(
            (r) => r.group === row.group && r.advanced === row.advanced,
          ).length;
          items.push({
            kind: "group-head",
            key: gkey,
            name: row.group,
            advanced: row.advanced,
            open: groupOpen,
            count,
            depth: s.depth,
          });
        } else {
          groupOpen = true;
        }
      }
      if (groupOpen) items.push({ kind: "row", key: row.key, row });
    }
    if (s.subgraphId && s.children.length > 0) {
      const dkey = `${s.key}|def`;
      const childMatched = s.children.reduce((n, c) => n + matchedIn(c), 0);
      if (filtering && childMatched === 0) return;
      const dopen = isOpen(dkey, false);
      items.push({
        kind: "def-head",
        key: dkey,
        section: s,
        open: dopen,
        shared: instanceShared(s),
        nodes: s.children.length,
      });
      if (dopen) for (const c of s.children) emit(c);
    }
  };
  for (const s of model.sections) emit(s);

  if (items.every((it) => it.kind === "gp-head" || it.kind === "empty")) {
    items.push({
      kind: "empty",
      key: "empty",
      text: filtering ? "没有符合条件的参数" : "当前层级没有节点",
    });
  }
  return { items, counts: countRows(model, filter), filtering };
}

function instanceShared(s: NodeSection): number {
  return s.children[0]?.shared ?? 0;
}

/** 某个节点那一节的键（画布选中 → 面板定位）。 */
export function sectionKey(path: SubPath, nodeId: string): string {
  return `n:${pathPrefix(path)}${nodeId}`;
}
