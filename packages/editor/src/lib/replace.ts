// 把一个节点换成别的算子（节点右键「换成别的算子…」）。节点的 id、位置、标题、静音照旧，换的是 op、opVersion 与参数。
// 这里只算换完之后什么留下、什么断掉，不碰 store；写进去由 store 的 replaceNodeOp 一条撤销完成。
//
// 规则（第一版只按名字 + 类型，不猜）：
// - 连线：同名端口、类型接得上（与手拖一样的判法，含 Any、一个输入只接一条）才留，先判输入再判输出；
//   另一头的算子没注册（缺包）时判不了，同名端口在就留着。
// - 参数：新算子有同名、同类型的才带过去；枚举值不在新选项里的不带；越界的夹进新范围；等于新默认值的删键（稀疏）。
// - 绑定：被图参数（顶层）/ 子图参数（子图里）绑着、但新算子没有同名同类型参数的，把那条绑定摘掉，参数本身留着。
// - 子图入口接到这个节点上的（inputs[].to）：新算子没有同名、同类型的输入就摘掉。
// - 子图出口从这个节点出去的（outputs[].from）：新算子没有那个输出就整个不换 —— 摘掉它会连带断开每个实例外面的线。
// - 图级命名输出（doc.outputs）指着这个节点的一个输出、新算子没有那个输出的：删掉那一条。

import { locateEventNode, levelOf, type SubPath } from "./subgraph";
import { canConnect, type GraphContext } from "./typecheck";
import { valueEquals } from "./params";
import type { GraphDoc, GraphEdge, SubgraphDef } from "../types/graph";
import type { OperatorDesc, Param } from "../types/manifest";

export interface ReplacePlan {
  /** 新的稀疏参数。 */
  params: Record<string, unknown>;
  /** 节点上写着、新算子带不过去的参数名。 */
  droppedParams: string[];
  /** 带过去时夹进了新范围的参数名。 */
  clampedParams: string[];
  keptEdges: string[];
  droppedEdges: string[];
  /** 要摘掉绑定的参数名（顶层：图参数的 bind；子图里：子图参数的 bind）。 */
  unbind: string[];
  /** 子图入口里要摘掉的那几条（`入口名`）。 */
  droppedInputs: string[];
  /** 要删掉的图级命名输出的名字。 */
  droppedOutputs: string[];
  /** 换不了的原因（子图出口会断）；null = 能换。 */
  blocked: string | null;
}

/** 这次换会丢掉多少东西（搜索弹层的行尾、换完之后的提示都用它）。 */
export function replaceLosses(plan: ReplacePlan): number {
  return plan.droppedEdges.length + plan.droppedParams.length + plan.unbind.length + plan.droppedInputs.length + plan.droppedOutputs.length;
}

function sameShape(value: unknown, def: unknown): boolean {
  if (Array.isArray(def)) return Array.isArray(value) && value.length === def.length;
  if (def === null || def === undefined) return true;
  return typeof value === typeof def;
}

/** 参数换到新算子上：同名同类型才带；枚举值要在新选项里；越界夹进新范围；等于新默认值删键。 */
export function carryParams(
  oldOp: OperatorDesc | undefined,
  newOp: OperatorDesc,
  params: Record<string, unknown> | undefined,
): { params: Record<string, unknown>; dropped: string[]; clamped: string[] } {
  const out: Record<string, unknown> = {};
  const dropped: string[] = [];
  const clamped: string[] = [];
  for (const [name, value] of Object.entries(params ?? {})) {
    const decl = newOp.params.find((p) => p.name === name);
    const old = oldOp?.params.find((p) => p.name === name);
    if (!decl || (old ? old.type !== decl.type : !sameShape(value, decl.default))) {
      dropped.push(name);
      continue;
    }
    if (decl.options && decl.options.length > 0 && !decl.options.some((o) => o.value === value)) {
      dropped.push(name);
      continue;
    }
    const lo = typeof decl.min === "number" ? decl.min : -Infinity;
    const hi = typeof decl.max === "number" ? decl.max : Infinity;
    const clampOne = (x: unknown) => (typeof x === "number" ? Math.min(hi, Math.max(lo, x)) : x);
    const next = Array.isArray(value) ? value.map(clampOne) : clampOne(value);
    if (!valueEquals(next, value)) clamped.push(name);
    if (!valueEquals(next, decl.default)) out[name] = next;
  }
  return { params: out, dropped, clamped };
}

function paramFits(oldOp: OperatorDesc | undefined, newOp: OperatorDesc, name: string): boolean {
  const decl: Param | undefined = newOp.params.find((p) => p.name === name);
  if (!decl) return false;
  const old = oldOp?.params.find((p) => p.name === name);
  return !old || old.type === decl.type;
}

function portFits(newOp: OperatorDesc, side: "inputs" | "outputs", name: string, type: string): boolean {
  const port = newOp[side].find((p) => p.name === name);
  return !!port && (port.type === type || port.type === "Any" || type === "Any");
}

/** 同一种子图（不论是哪个实例）：路径上每一层的子图定义都一样。 */
function sameDefPath(a: SubPath, b: SubPath): boolean {
  return a.length === b.length && a.every((seg, i) => seg.subgraphId === b[i]!.subgraphId);
}

export function planReplace(
  ctx: GraphContext,
  doc: GraphDoc,
  path: SubPath,
  nodeId: string,
  newOp: OperatorDesc,
): ReplacePlan | null {
  const level = levelOf(doc, path);
  const node = level.nodes.find((n) => n.id === nodeId);
  if (!node) return null;
  const oldOp = ctx.operatorsById.get(node.op);
  const carried = carryParams(oldOp, newOp, node.params);

  // 连线：探针图里这个节点换成新算子、先拿掉它的线，再一条条判（先输入后输出，一个输入只留一条）
  const ops = new Map(ctx.operatorsById);
  ops.set(newOp.id, newOp);
  const probeCtx: GraphContext = { ...ctx, operatorsById: ops };
  const mine = level.edges.filter((e) => e.from.node === nodeId || e.to.node === nodeId);
  mine.sort((a, b) => (a.to.node === nodeId ? 0 : 1) - (b.to.node === nodeId ? 0 : 1));
  const probe: GraphDoc = {
    ...doc,
    nodes: level.nodes.map((n) => (n.id === nodeId ? { ...n, op: newOp.id, params: carried.params } : n)),
    edges: level.edges.filter((e) => e.from.node !== nodeId && e.to.node !== nodeId),
  };
  const keptEdges: string[] = [];
  const droppedEdges: string[] = [];
  for (const e of mine) {
    const otherId = e.to.node === nodeId ? e.from.node : e.to.node;
    const other = level.nodes.find((n) => n.id === otherId);
    const ownPortOk =
      e.to.node === nodeId ? newOp.inputs.some((p) => p.name === e.to.port) : newOp.outputs.some((p) => p.name === e.from.port);
    const judgeable = !!other && ops.has(other.op);
    const ok = judgeable ? canConnect(probeCtx, probe, e.from, e.to).ok : ownPortOk;
    if (ok) {
      keptEdges.push(e.id);
      (probe.edges as GraphEdge[]).push(e);
    } else {
      droppedEdges.push(e.id);
    }
  }

  // 绑定：顶层看图参数的 bind（`节点.参数`），子图里看子图参数的 bind
  const unbind = new Set<string>();
  const def: SubgraphDef | undefined =
    path.length > 0 ? doc.subgraphs?.[path[path.length - 1]!.subgraphId] : undefined;
  if (path.length === 0) {
    for (const gp of Object.values(doc.params ?? {})) {
      for (const b of gp.binds) {
        const dot = b.lastIndexOf(".");
        if (b.slice(0, dot) === nodeId && !paramFits(oldOp, newOp, b.slice(dot + 1))) unbind.add(b.slice(dot + 1));
      }
    }
  } else {
    for (const sp of def?.params ?? []) {
      for (const b of sp.binds ?? []) {
        if (b.node === nodeId && !paramFits(oldOp, newOp, b.param)) unbind.add(b.param);
      }
    }
  }

  const droppedInputs: string[] = [];
  let blocked: string | null = null;
  for (const input of def?.inputs ?? []) {
    if (input.to.some((t) => t.node === nodeId && !portFits(newOp, "inputs", t.port, input.type))) droppedInputs.push(input.name);
  }
  for (const output of def?.outputs ?? []) {
    if (output.from.node === nodeId && !portFits(newOp, "outputs", output.from.port, output.type)) {
      blocked = `子图输出 ${output.name} 接的是 ${output.from.port}，${newOp.label} 没有这个输出：先改子图的输出再换`;
    }
  }

  const droppedOutputs: string[] = [];
  for (const [name, o] of Object.entries(doc.outputs ?? {})) {
    const loc = locateEventNode(doc, o.node);
    if (!loc || loc.localId !== nodeId || !sameDefPath(loc.path, path)) continue;
    const port = o.port.split(".")[0]!;
    if (!newOp.outputs.some((p) => p.name === port)) droppedOutputs.push(name);
  }

  return {
    params: carried.params,
    droppedParams: carried.dropped,
    clampedParams: carried.clamped,
    keptEdges,
    droppedEdges,
    unbind: [...unbind],
    droppedInputs,
    droppedOutputs,
    blocked,
  };
}
