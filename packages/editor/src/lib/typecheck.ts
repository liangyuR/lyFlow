// 连接合法性校验 + 拓扑。这一层是**手感**不是正确性（docs/architecture.md）：拖线时即时挡错、给人话原因。
// C++ 侧执行前必须独立完整校验一遍 —— 这里能被绕过（手改文件、脚本生成的图、旧版本客户端）。

import type { GraphDoc, PortRef } from "../types/graph";
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

/** 拓扑排序，返回 null 表示有环。M1 用不到执行顺序，但它是环检测的权威实现 ——
 *  wouldCreateCycle 只管增量连线，这个负责校验一整张图（比如打开别人存的文件）。 */
export function topoSort(doc: GraphDoc): string[] | null {
  const indegree = new Map<string, number>();
  const downstream = new Map<string, string[]>();
  for (const n of doc.nodes) {
    indegree.set(n.id, 0);
    downstream.set(n.id, []);
  }
  for (const e of doc.edges) {
    if (!indegree.has(e.to.node) || !indegree.has(e.from.node)) continue; // 悬空边，交给结构校验
    indegree.set(e.to.node, (indegree.get(e.to.node) ?? 0) + 1);
    downstream.get(e.from.node)?.push(e.to.node);
  }

  const queue = [...indegree.entries()].filter(([, d]) => d === 0).map(([id]) => id);
  const order: string[] = [];
  while (queue.length > 0) {
    const id = queue.shift()!;
    order.push(id);
    for (const next of downstream.get(id) ?? []) {
      const d = (indegree.get(next) ?? 0) - 1;
      indegree.set(next, d);
      if (d === 0) queue.push(next);
    }
  }
  return order.length === doc.nodes.length ? order : null;
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
