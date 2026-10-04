// 连接合法性校验 + 拓扑。这一层是**手感**不是正确性（docs/architecture.md）：拖线时即时挡错、给人话原因。
// C++ 侧执行前必须独立完整校验一遍 —— 这里能被绕过（手改文件、脚本生成的图、旧版本客户端）。

import type { GraphDoc, GraphEdge, PortRef } from "../types/graph";
import type { OperatorDesc, Port, PortType } from "../types/manifest";

export interface GraphContext {
  operatorsById: ReadonlyMap<string, OperatorDesc>;
  typesByName: ReadonlyMap<string, PortType>;
}

export type ConnectVerdict = { ok: true } | { ok: false; reason: string };

/** 通配类型：Reroute、Debug View 这类透传节点用，实际类型由连线推导（E6）。 */
export const ANY = "Any";

/** nodeId → 该节点全部 Any 端口的实际类型。推不出来的节点不在表里。 */
export type AnyTypes = ReadonlyMap<string, string>;

const NO_ANY: AnyTypes = new Map();

export function findPort(
  op: OperatorDesc | undefined,
  name: string,
  side: "input" | "output",
): Port | undefined {
  if (!op) return undefined;
  return (side === "input" ? op.inputs : op.outputs).find((p) => p.name === name);
}

/** 端口类型是否可连。V1 规则刻意简单（docs/operator-manifest.md）：
 *  精确匹配 / Any 通配 / manifest 声明的 castableTo 单向转换，不做泛型。 */
export function typesCompatible(
  ctx: GraphContext,
  fromType: string,
  toType: string,
): boolean {
  if (fromType === toType) return true;
  if (fromType === ANY || toType === ANY) return true;
  // castableTo 是有方向的：PointCloudXYZI 能当 PointCloud 用，反过来不行。
  return ctx.typesByName.get(fromType)?.castableTo?.includes(toType) ?? false;
}

/** 沿连线把具体类型传播到 Any 端口，迭代到定点（E6）。C++ 的 buildPlan 里有
 *  同样一份实现 —— 一个节点的**全部** Any 端口共用一个类型变量，reroute 正是这个语义。 */
export function inferAnyTypes(ctx: GraphContext, doc: GraphDoc): AnyTypes {
  const resolved = new Map<string, string>();
  const hasAny = (op: OperatorDesc | undefined) =>
    !!op && [...op.inputs, ...op.outputs].some((p) => p.type === ANY);

  const nodeOp = new Map<string, OperatorDesc | undefined>();
  let anyNodes = 0;
  for (const n of doc.nodes) {
    const op = ctx.operatorsById.get(n.op);
    nodeOp.set(n.id, op);
    if (hasAny(op)) anyNodes += 1;
  }
  if (anyNodes === 0) return NO_ANY;

  const concrete = (nodeId: string, port: Port | undefined): string | null => {
    if (!port) return null;
    if (port.type !== ANY) return port.type;
    return resolved.get(nodeId) ?? null;
  };

  for (let round = 0; round <= doc.nodes.length; round += 1) {
    let changed = false;
    for (const e of doc.edges) {
      const outPort = findPort(nodeOp.get(e.from.node), e.from.port, "output");
      const inPort = findPort(nodeOp.get(e.to.node), e.to.port, "input");
      if (!outPort || !inPort) continue;
      const fromT = concrete(e.from.node, outPort);
      const toT = concrete(e.to.node, inPort);
      if (fromT && !toT && inPort.type === ANY) {
        resolved.set(e.to.node, fromT);
        changed = true;
      } else if (toT && !fromT && outPort.type === ANY) {
        resolved.set(e.from.node, toT);
        changed = true;
      }
    }
    if (!changed) break;
  }
  return resolved;
}

/** 端口的实际类型。推导表里没有就用声明类型（可能还是 Any）。 */
export function portType(port: Port, nodeId: string, anyTypes: AnyTypes): string {
  if (port.type !== ANY) return port.type;
  return anyTypes.get(nodeId) ?? ANY;
}

/** 反向邻接：给定节点的所有上游节点 id。 */
function upstreamOf(doc: GraphDoc, nodeId: string): string[] {
  return doc.edges.filter((e) => e.to.node === nodeId).map((e) => e.from.node);
}

/** 从 `from` 连到 `to` 会不会成环。等价于问：`from` 是不是已经在 `to` 的下游。
 *  DFS 沿上游走，图规模是几十个节点，不需要更聪明的做法。 */
export function wouldCreateCycle(doc: GraphDoc, fromNode: string, toNode: string): boolean {
  if (fromNode === toNode) return true;
  const seen = new Set<string>();
  const stack = [fromNode];
  while (stack.length > 0) {
    const current = stack.pop()!;
    if (current === toNode) return true;
    if (seen.has(current)) continue;
    seen.add(current);
    stack.push(...upstreamOf(doc, current));
  }
  return false;
}

/** 能不能从 from 连到 to。返回的 reason 会直接显示给用户，所以写人话。 */
export function canConnect(
  ctx: GraphContext,
  doc: GraphDoc,
  from: PortRef,
  to: PortRef,
  anyTypes?: AnyTypes,
): ConnectVerdict {
  if (from.node === to.node) {
    return { ok: false, reason: "不能连到自己" };
  }

  const fromNode = doc.nodes.find((n) => n.id === from.node);
  const toNode = doc.nodes.find((n) => n.id === to.node);
  if (!fromNode || !toNode) {
    return { ok: false, reason: "节点不存在" };
  }

  const fromOp = ctx.operatorsById.get(fromNode.op);
  const toOp = ctx.operatorsById.get(toNode.op);
  if (!fromOp || !toOp) {
    return { ok: false, reason: "算子未注册（core 里可能已经删掉了）" };
  }

  const outPort = findPort(fromOp, from.port, "output");
  const inPort = findPort(toOp, to.port, "input");
  if (!outPort) return { ok: false, reason: `${fromOp.label} 没有输出端口 ${from.port}` };
  if (!inPort) return { ok: false, reason: `${toOp.label} 没有输入端口 ${to.port}` };

  // 按推导后的实际类型判：串了两级 reroute 之后仍然接得住类型不匹配（E6）。
  const types = anyTypes ?? inferAnyTypes(ctx, doc);
  const fromType = portType(outPort, from.node, types);
  const toType = portType(inPort, to.node, types);
  if (!typesCompatible(ctx, fromType, toType)) {
    return {
      ok: false,
      reason: `类型不匹配：${fromType} → ${toType}`,
    };
  }

  // 输入端口是单连接：多输入合并要由算子显式声明多个端口，否则求值顺序是隐式的（docs/graph-doc.md）。
  // 但不算「已经存在的同一条边」—— 重连同一对端口该是 no-op，而不是报「端口已被占用」。
  const occupied = doc.edges.find(
    (e) => e.to.node === to.node && e.to.port === to.port,
  );
  if (occupied && !(occupied.from.node === from.node && occupied.from.port === from.port)) {
    return { ok: false, reason: "输入端口已有连线（输入是单连接）" };
  }

  if (wouldCreateCycle(doc, from.node, to.node)) {
    return { ok: false, reason: "会形成环" };
  }

  return { ok: true };
}

/** 能不能把 from 接到 to —— to 已经被别的线占着时顶掉它（拖线松在已接线的输入上 = 给它换个来源，Blender、ComfyUI
 *  都是这样）。ok 时 replaces 是要顶掉的那条线（没占着时为 null）。类型、成环在拿掉那条线之后的图上判（Any 跟着重推）。
 *  只给界面上的拖线用：store 的 connect 照旧拒绝被占的输入，CLI 的 --connect 也不替人顶掉（ADR-0023）。 */
export function canConnectReplacing(
  ctx: GraphContext,
  doc: GraphDoc,
  from: PortRef,
  to: PortRef,
): { ok: true; replaces: string | null } | { ok: false; reason: string } {
  const occupant = doc.edges.find(
    (e) => e.to.node === to.node && e.to.port === to.port && !(e.from.node === from.node && e.from.port === from.port),
  );
  const verdict = occupant
    ? canConnect(ctx, { ...doc, edges: doc.edges.filter((e) => e.id !== occupant.id) }, from, to)
    : canConnect(ctx, doc, from, to);
  return verdict.ok ? { ok: true, replaces: occupant?.id ?? null } : verdict;
}

/** 拖线时用：从 from 拖出去，哪些已经接着线的输入可以被顶掉（端口上亮成「替换」，与「能落」「不能落」区分开）。 */
export function replaceableTargets(ctx: GraphContext, doc: GraphDoc, from: PortRef): Set<string> {
  const out = new Set<string>();
  for (const e of doc.edges) {
    if (e.from.node === from.node && e.from.port === from.port) continue;
    const verdict = canConnectReplacing(ctx, doc, from, e.to);
    if (verdict.ok && verdict.replaces) out.add(`${e.to.node}:${e.to.port}`);
  }
  return out;
}

/** 拖线时用：给定拖出的源端口，哪些输入端口是可落点。
 *  UI 拿它把不兼容的端口置灰 —— 让类型系统「看得见」。 */
export function compatibleTargets(
  ctx: GraphContext,
  doc: GraphDoc,
  from: PortRef,
): Set<string> {
  const out = new Set<string>();
  const types = inferAnyTypes(ctx, doc);
  for (const node of doc.nodes) {
    const op = ctx.operatorsById.get(node.op);
    if (!op) continue;
    for (const port of op.inputs) {
      const verdict = canConnect(ctx, doc, from, { node: node.id, port: port.name }, types);
      if (verdict.ok) out.add(`${node.id}:${port.name}`);
    }
  }
  return out;
}

/** 拖线松在一个节点的身子上（没对准端口，React Flow 认为没接上）该怎么办。以前一律当成松在空白处、弹「添加算子」的
 *  搜索面板 —— 用户明明是想接到这个节点上。现在：这个节点上恰好一个端口能接就接它；不止一个，提示对准端口；
 *  一个都接不上，说为什么；松回自己身上什么也不做。side 是拖的那一头：拖输出就在这个节点的输入里找，反着拖输入就在它的输出里找。 */
export type DropOnNode =
  | { kind: "none" }
  | { kind: "self" }
  | { kind: "connect"; from: PortRef; to: PortRef; replaces?: string }
  | { kind: "reject"; reason: string };

export function dropOnNode(
  ctx: GraphContext,
  doc: GraphDoc,
  ref: PortRef,
  side: "output" | "input",
  nodeId: string,
): DropOnNode {
  if (nodeId === ref.node) return { kind: "self" };
  const node = doc.nodes.find((n) => n.id === nodeId);
  const op = node ? ctx.operatorsById.get(node.op) : undefined;
  if (!op) return { kind: "none" };
  // 从一个已经接着线的输入端口反着拖出来的：接到谁身上都是「输入是单连接」，别说成是那个节点的问题
  if (side === "input" && doc.edges.some((e) => e.to.node === ref.node && e.to.port === ref.port)) {
    return { kind: "reject", reason: "这个输入端口已有连线（输入是单连接）：拖它的线头才是改接" };
  }
  const types = inferAnyTypes(ctx, doc);
  const pairs = (side === "output" ? op.inputs : op.outputs).map((p) =>
    side === "output"
      ? { from: ref, to: { node: nodeId, port: p.name } }
      : { from: { node: nodeId, port: p.name }, to: ref },
  );
  const verdicts = pairs.map((pair) => ({ pair, verdict: canConnect(ctx, doc, pair.from, pair.to, types) }));
  const ok = verdicts.filter((v) => v.verdict.ok);
  if (ok.length === 1) return { kind: "connect", ...ok[0]!.pair };
  if (ok.length > 1) return { kind: "reject", reason: `${op.label} 上有 ${ok.length} 个端口能接，拖到要接的那个端口上` };
  const first = verdicts[0]?.verdict;
  return {
    kind: "reject",
    reason: first && !first.ok ? `接不到 ${op.label} 上：${first.reason}` : `${op.label} 没有${side === "output" ? "输入" : "输出"}端口`,
  };
}

/** 拖线松在空白处、在搜索里挑了一个新算子：新算子的哪个端口接拖出来的这一头。拖的是输出就在它的输入里挑，
 *  拖的是输入就在它的输出里挑。类型相同 > 能转（castableTo）> 通配（Any）；同分时必填的优先，再按声明顺序。
 *  接不上返回 null。以前固定取第一个端口：从 RANSAC 的 inliers（Indices）拖出来选「提取索引」，接到了它的
 *  cloud 上 —— 类型不对、线没接上，节点光秃秃地落下。拖出的这一头还是没推出类型的 Any 时，等于取第一个。 */
export function pendingPort(
  ctx: GraphContext,
  doc: GraphDoc,
  pending: PortRef,
  side: "output" | "input",
  op: OperatorDesc,
  anyTypes?: AnyTypes,
): string | null {
  const node = doc.nodes.find((n) => n.id === pending.node);
  const own = node ? findPort(ctx.operatorsById.get(node.op), pending.port, side) : undefined;
  if (!own) return null;
  const mine = portType(own, pending.node, anyTypes ?? inferAnyTypes(ctx, doc));
  let best: { name: string; score: number } | null = null;
  for (const p of side === "output" ? op.inputs : op.outputs) {
    const [from, to] = side === "output" ? [mine, p.type] : [p.type, mine];
    if (!typesCompatible(ctx, from, to)) continue;
    const kind = from === ANY || to === ANY ? 1 : from === to ? 3 : 2;
    const score = kind * 2 + (p.required ? 1 : 0);
    if (!best || score > best.score) best = { name: p.name, score };
  }
  return best?.name ?? null;
}

/** 拖出来的这一头的实际类型（Any 按连线推导）。给搜索弹层写「接不上 Indices」用。 */
export function pendingType(ctx: GraphContext, doc: GraphDoc, pending: PortRef, side: "output" | "input"): string | null {
  const node = doc.nodes.find((n) => n.id === pending.node);
  const own = node ? findPort(ctx.operatorsById.get(node.op), pending.port, side) : undefined;
  return own ? portType(own, pending.node, inferAnyTypes(ctx, doc)) : null;
}

/** 把一个节点插到一条连线中间用哪一对端口：拿掉这条线之后，上游接得上它的某个输入、它的某个输出接得上下游 ——
 *  恰好一对时给出来；没有、或不止一对（没有唯一解，宁可不动）时 null。拖节点到线上、从算子面板拖到线上都按它。 */
export function insertPortsFor(
  ctx: GraphContext,
  doc: GraphDoc,
  edge: GraphEdge,
  nodeId: string,
  anyTypes?: AnyTypes,
): { inPort: string; outPort: string } | null {
  const node = doc.nodes.find((n) => n.id === nodeId);
  const op = node ? ctx.operatorsById.get(node.op) : undefined;
  if (!op) return null;
  const without: GraphDoc = { ...doc, edges: doc.edges.filter((e) => e.id !== edge.id) };
  const types = anyTypes ?? inferAnyTypes(ctx, without);
  let found: { inPort: string; outPort: string } | null = null;
  for (const inPort of op.inputs) {
    if (!canConnect(ctx, without, edge.from, { node: nodeId, port: inPort.name }, types).ok) continue;
    for (const outPort of op.outputs) {
      if (!canConnect(ctx, without, { node: nodeId, port: outPort.name }, edge.to, types).ok) continue;
      if (found) return null;
      found = { inPort: inPort.name, outPort: outPort.name };
    }
  }
  return found;
}

/** 拖线松在一个具体的端口上、React Flow 却没接上：只判这一个端口 —— 已接着线的输入就给它换来源（顶掉原来那条），
 *  接不上说它自己的原因。以前当成松在节点身上、在整个节点里另找能接的：松在合并的 a（已被占）上接到了 b，松在 ICP 的
 *  source 上接到了 target —— 语义反了，还没有任何提示。target 是松手处的端口，targetSide 是它在自己节点上的哪一侧。 */
export function dropOnPort(
  ctx: GraphContext,
  doc: GraphDoc,
  ref: PortRef,
  side: "output" | "input",
  target: PortRef,
  targetSide: "output" | "input",
): DropOnNode {
  if (target.node === ref.node) return { kind: "self" };
  if (targetSide === side) {
    return { kind: "reject", reason: side === "output" ? "这是输出端口：拖到输入端口上" : "这是输入端口：拖到输出端口上" };
  }
  const [from, to] = side === "output" ? [ref, target] : [target, ref];
  // 松在已接线的输入上 = 换来源（顶掉原来那条）；成环、类型不对说原因
  const verdict = canConnectReplacing(ctx, doc, from, to);
  if (!verdict.ok) return { kind: "reject", reason: verdict.reason };
  return verdict.replaces ? { kind: "connect", from, to, replaces: verdict.replaces } : { kind: "connect", from, to };
}

/** 反向：拖的是输入端口时，哪些输出端口可以当源。#20 的置灰要两个方向都覆盖。 */
export function compatibleSources(
  ctx: GraphContext,
  doc: GraphDoc,
  to: PortRef,
): Set<string> {
  const out = new Set<string>();
  const types = inferAnyTypes(ctx, doc);
  for (const node of doc.nodes) {
    const op = ctx.operatorsById.get(node.op);
    if (!op) continue;
    for (const port of op.outputs) {
      const verdict = canConnect(ctx, doc, { node: node.id, port: port.name }, to, types);
      if (verdict.ok) out.add(`${node.id}:${port.name}`);
    }
  }
  return out;
}
