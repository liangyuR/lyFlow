// 按类型自动连线（m8-plan L13）。这是**编辑动作**：算出来的边写成普通的边，执行语义不变，
// 没有任何运行时的隐式上下文。规则只有一条 —— 未连的输入在图里「恰好有一个」类型兼容的
// 输出就连上，多个就不连、把候选报给界面去高亮。

import type { GraphDoc, PortRef } from "../types/graph";
import { ANY, canConnect, findPort, inferAnyTypes, portType, typesCompatible, type GraphContext } from "./typecheck";

export interface AutoWire {
  from: PortRef;
  to: PortRef;
}

export interface AutoAmbiguity {
  to: PortRef;
  candidates: PortRef[];
}

export interface AutoConnectPlan {
  wires: AutoWire[];
  ambiguous: AutoAmbiguity[];
}

const key = (r: PortRef) => `${r.node}:${r.port}`;

/** 一个输出端口在「谁是候选」里被遮住了没有：它已经连到了某个节点上，而那个节点自己又产出
 *  同一种类型 —— 那个节点的输出取代了它。例子：read_scan.scan 接进 locate_template，后者也出
 *  ScanPair，于是往下游再拖节点时只剩 locate_template.scan 一个候选。不这么做的话，模板路径
 *  上每一步都会重新产出 ScanPair，「恰好一个候选」在任何一张真实的图里都不成立。 */
function shadowedOutputs(ctx: GraphContext, doc: GraphDoc, anyTypes: ReturnType<typeof inferAnyTypes>): Set<string> {
  const out = new Set<string>();
  const typesOut = new Map<string, Set<string>>();
  const outputTypesOf = (nodeId: string): Set<string> => {
    const hit = typesOut.get(nodeId);
    if (hit) return hit;
    const node = doc.nodes.find((n) => n.id === nodeId);
    const op = node ? ctx.operatorsById.get(node.op) : undefined;
    const set = new Set<string>();
    for (const p of op?.outputs ?? []) set.add(portType(p, nodeId, anyTypes));
    typesOut.set(nodeId, set);
    return set;
  };
  for (const e of doc.edges) {
    const node = doc.nodes.find((n) => n.id === e.from.node);
    const op = node ? ctx.operatorsById.get(node.op) : undefined;
    const port = findPort(op, e.from.port, "output");
    if (!port) continue;
    const type = portType(port, e.from.node, anyTypes);
    if (type === ANY) continue;
    if (outputTypesOf(e.to.node).has(type)) out.add(key(e.from));
  }
  return out;
}

/** 给定一组要接的输入端口，算出能唯一确定的边与有歧义的端口。
 *  `excludeSources` 里的节点不当候选（插片段时片段自己的节点不算，免得自己接自己）。 */
export function planAutoConnect(
  ctx: GraphContext,
  doc: GraphDoc,
  targets: readonly PortRef[],
  excludeSources: ReadonlySet<string> = new Set(),
): AutoConnectPlan {
  const anyTypes = inferAnyTypes(ctx, doc);
  const shadowed = shadowedOutputs(ctx, doc, anyTypes);
  const connected = new Set(doc.edges.map((e) => key(e.to)));

  // 候选输出一次列出来：类型推不出来（还是 Any）的不算 —— 一个空的 reroute 跟谁都「兼容」
  const sources: { ref: PortRef; type: string }[] = [];
  for (const node of doc.nodes) {
    if (excludeSources.has(node.id)) continue;
    const op = ctx.operatorsById.get(node.op);
    for (const p of op?.outputs ?? []) {
      const type = portType(p, node.id, anyTypes);
      if (type === ANY) continue;
      const ref = { node: node.id, port: p.name };
      if (shadowed.has(key(ref))) continue;
      sources.push({ ref, type });
    }
  }

  const plan: AutoConnectPlan = { wires: [], ambiguous: [] };
  for (const to of targets) {
    if (connected.has(key(to))) continue;
    const node = doc.nodes.find((n) => n.id === to.node);
    const op = node ? ctx.operatorsById.get(node.op) : undefined;
    const port = findPort(op, to.port, "input");
    if (!port) continue;
    const inType = portType(port, to.node, anyTypes);
    // Any 输入（flow.fallback 的 a / b 这类）谁都能接，猜就是瞎猜
    if (inType === ANY) continue;
    const candidates = sources
      .filter((s) => s.ref.node !== to.node && typesCompatible(ctx, s.type, inType))
      .filter((s) => canConnect(ctx, doc, s.ref, to, anyTypes).ok)
      .map((s) => s.ref);
    if (candidates.length === 1) plan.wires.push({ from: candidates[0]!, to });
    else if (candidates.length > 1) plan.ambiguous.push({ to, candidates });
  }
  return plan;
}

/** 一个节点上还没接的必需输入。拖入单个节点时要试的就是这些。 */
export function unconnectedRequiredInputs(
  ctx: GraphContext,
  doc: GraphDoc,
  nodeId: string,
): PortRef[] {
  const node = doc.nodes.find((n) => n.id === nodeId);
  const op = node ? ctx.operatorsById.get(node.op) : undefined;
  const connected = new Set(doc.edges.filter((e) => e.to.node === nodeId).map((e) => e.to.port));
  return (op?.inputs ?? [])
    .filter((p) => p.required !== false && !connected.has(p.name))
    .map((p) => ({ node: nodeId, port: p.name }));
}
