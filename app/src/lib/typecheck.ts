// 连接合法性校验 + 拓扑。这一层是**手感**不是正确性（docs/architecture.md）：拖线时即时挡错、给人话原因。
// C++ 侧执行前必须独立完整校验一遍 —— 这里能被绕过（手改文件、脚本生成的图、旧版本客户端）。

import type { GraphDoc, PortRef } from "../types/graph";
import type { OperatorDesc, Port, PortType } from "../types/manifest";

export interface GraphContext {
  operatorsById: ReadonlyMap<string, OperatorDesc>;
  typesByName: ReadonlyMap<string, PortType>;
}

export type ConnectVerdict = { ok: true } | { ok: false; reason: string };

/** 通配类型：Reroute、Debug View 这类透传节点用，可与任意类型互连。 */
const ANY = "Any";

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

  if (!typesCompatible(ctx, outPort.type, inPort.type)) {
    return {
      ok: false,
      reason: `类型不匹配：${outPort.type} → ${inPort.type}`,
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
  for (const node of doc.nodes) {
    const op = ctx.operatorsById.get(node.op);
    if (!op) continue;
    for (const port of op.inputs) {
      const verdict = canConnect(ctx, doc, from, { node: node.id, port: port.name });
      if (verdict.ok) out.add(`${node.id}:${port.name}`);
    }
  }
  return out;
}
