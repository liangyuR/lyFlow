// 节点之间复制 / 粘贴整组参数、全部恢复默认（节点右键菜单）。这里只算要改什么、不碰 store；
// 写进去由 lib/editActions 用 setParam 一条撤销完成（绑定的路由、稀疏写、配方覆盖的提示都在 setParam 里）。
//
// 剪贴板只在内存里（ui store），不进系统剪贴板：写系统剪贴板会顶掉复制好的节点，读它要 WebView2 的权限、
// 菜单打开那一刻也没法同步知道「粘贴参数」能不能点。

import { resolveGraphBinding, withBoundValues } from "./graphParams";
import { effectiveParams, effectiveValue, valueEquals } from "./params";
import { levelOf, promotedBy, type SubPath } from "./subgraph";
import type { GraphDoc } from "../types/graph";
import type { OperatorDesc } from "../types/manifest";

export interface ParamClipboard {
  /** 算子 id：只往同一种算子上粘。 */
  op: string;
  /** 子图节点（`sub:`）的 id 只在一张图里有意义：记下是哪张图的，别的图里不粘。其余算子为 null。 */
  docId: string | null;
  /** 从哪个节点复制的（给人看的名字，写进提示里）。 */
  from: string;
  /** 完整的有效值（稀疏的展开、被图参数绑定的取图参数此刻的值）。 */
  values: Record<string, unknown>;
}

export interface ParamEditPlan {
  edits: { nodeId: string; name: string; value: unknown }[];
  /** 真正要改的节点数。 */
  nodes: number;
  /** 不是同一种算子、没动的节点数（只有粘贴有）。 */
  skippedOp: number;
  /** 值不归节点自己管（被图参数绑定、在子图里提升成了子图参数）、没动的参数个数。 */
  locked: number;
}

/** 这个参数此刻不归节点自己管：被图参数绑着（值在图参数 / 配方里），或在子图里提升成了子图参数（值在实例上）。 */
export function paramLock(doc: GraphDoc, path: SubPath, nodeId: string, name: string): "bound" | "promoted" | null {
  if (resolveGraphBinding(doc, path, nodeId, name)) return "bound";
  const seg = path[path.length - 1];
  if (seg && promotedBy(doc.subgraphs?.[seg.subgraphId], nodeId, name)) return "promoted";
  return null;
}

/** 复制这个节点此刻的参数。提升成子图参数的那几个不带（它的值在子图实例上，不是这个节点的）。没有参数返回 null。 */
export function snapshotParams(
  doc: GraphDoc,
  path: SubPath,
  nodeId: string,
  ops: ReadonlyMap<string, OperatorDesc>,
  overrides: Readonly<Record<string, unknown>>,
): ParamClipboard | null {
  const node = levelOf(doc, path).nodes.find((n) => n.id === nodeId);
  const op = node ? ops.get(node.op) : undefined;
  if (!node || !op || op.params.length === 0) return null;
  const names = op.params.map((p) => p.name);
  const values = effectiveParams(op, withBoundValues(doc, path, node, names, overrides));
  for (const name of names) if (paramLock(doc, path, nodeId, name) === "promoted") delete values[name];
  return {
    op: node.op,
    docId: node.op.startsWith("sub:") ? (doc.id ?? null) : null,
    from: node.ui?.title ?? op.label ?? node.id,
    values: structuredClone(values),
  };
}

/** 剪贴板能不能粘到这个节点上（菜单里「粘贴参数」可不可点）。 */
export function canPasteParams(doc: GraphDoc, clip: ParamClipboard | null, op: string | undefined): boolean {
  return !!clip && clip.op === op && (clip.docId === null || clip.docId === (doc.id ?? null));
}

/** 热重载之后算子的参数可能换了形状：值的类型、数组长度对不上默认值的不粘。 */
function sameShape(value: unknown, def: unknown): boolean {
  if (Array.isArray(def)) return Array.isArray(value) && value.length === def.length;
  if (def === null || def === undefined) return true;
  return typeof value === typeof def;
}

/** 粘到这些节点上要改哪些参数：同一种算子才粘；值已经一样的不改（不记一条空撤销）。 */
export function planParamPaste(
  doc: GraphDoc,
  path: SubPath,
  clip: ParamClipboard,
  ids: readonly string[],
  ops: ReadonlyMap<string, OperatorDesc>,
): ParamEditPlan {
  const level = levelOf(doc, path);
  const plan: ParamEditPlan = { edits: [], nodes: 0, skippedOp: 0, locked: 0 };
  for (const id of ids) {
    const node = level.nodes.find((n) => n.id === id);
    const op = node ? ops.get(node.op) : undefined;
    if (!node || !op || !canPasteParams(doc, clip, node.op)) {
      plan.skippedOp += 1;
      continue;
    }
    let touched = false;
    for (const p of op.params) {
      if (!(p.name in clip.values)) continue;
      const value = clip.values[p.name];
      if (!sameShape(value, p.default)) continue;
      if (paramLock(doc, path, id, p.name)) {
        plan.locked += 1;
        continue;
      }
      if (valueEquals(effectiveValue(op, node, p.name), value)) continue;
      plan.edits.push({ nodeId: id, name: p.name, value: structuredClone(value) });
      touched = true;
    }
    if (touched) plan.nodes += 1;
  }
  return plan;
}

/** 这些节点全部恢复默认要改哪些参数：节点上显式写着的（稀疏存储，默认值本来就不写）；不归节点管的不动。 */
export function planParamReset(
  doc: GraphDoc,
  path: SubPath,
  ids: readonly string[],
  ops: ReadonlyMap<string, OperatorDesc>,
): ParamEditPlan {
  const level = levelOf(doc, path);
  const plan: ParamEditPlan = { edits: [], nodes: 0, skippedOp: 0, locked: 0 };
  for (const id of ids) {
    const node = level.nodes.find((n) => n.id === id);
    const op = node ? ops.get(node.op) : undefined;
    if (!node || !op) continue;
    let touched = false;
    for (const p of op.params) {
      if (node.params?.[p.name] === undefined) continue;
      if (paramLock(doc, path, id, p.name)) {
        plan.locked += 1;
        continue;
      }
      plan.edits.push({ nodeId: id, name: p.name, value: structuredClone(p.default) });
      touched = true;
    }
    if (touched) plan.nodes += 1;
  }
  return plan;
}
