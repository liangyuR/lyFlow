// 子图在前端的三件事（ADR-0010）：把定义合成成 OperatorDesc 让节点画得出来、
// 按当前路径取出该渲染的那一层、以及合成/解散两个 change 动作。

import { graphParamBoundTo, joinBind } from "./graphParams";
import { newLocalId } from "./ids";
import { ANY, findPort, inferAnyTypes, type GraphContext } from "./typecheck";
import type { OperatorDesc, Param, Port } from "../types/manifest";
import {
  subgraphIdOf,
  SUBGRAPH_OP_PREFIX,
  type GraphDoc,
  type GraphEdge,
  type GraphLevel,
  type GraphNode,
  type PortRef,
  type SubgraphDef,
  type SubParam,
} from "../types/graph";

/** 当前所在的子图栈。nodeId 用来拼事件路径，subgraphId 用来在 doc 里取定义。 */
export interface PathSegment {
  nodeId: string;
  subgraphId: string;
}

export type SubPath = readonly PathSegment[];

/** 事件里的路径前缀（F2）。顶层是空串。 */
export function pathPrefix(path: SubPath): string {
  return path.length === 0 ? "" : path.map((p) => p.nodeId).join("/") + "/";
}

/** 某个本层节点在事件里的完整 id。 */
export function fullId(path: SubPath, localId: string): string {
  return pathPrefix(path) + localId;
}

/** 事件 id → 本层的哪个节点。不属于当前层级时返回 null。 */
export function localIdOf(path: SubPath, eventId: string): string | null {
  const prefix = pathPrefix(path);
  if (!eventId.startsWith(prefix)) return null;
  const rest = eventId.slice(prefix.length);
  if (!rest) return null;
  const slash = rest.indexOf("/");
  return slash < 0 ? rest : rest.slice(0, slash);
}

/** 当前层级的 nodes/edges。路径失效（子图被删了）时退回顶层。 */
export function levelOf(doc: GraphDoc, path: SubPath): GraphLevel {
  const last = path[path.length - 1];
  if (!last) return doc;
  const def = doc.subgraphs?.[last.subgraphId];
  return def ?? doc;
}

/** 路径还指得到东西吗。删掉子图节点之后要靠它把用户弹回上一层。 */
export function pathIsValid(doc: GraphDoc, path: SubPath): boolean {
  let level: GraphLevel = doc;
  for (const seg of path) {
    const node = level.nodes.find((n) => n.id === seg.nodeId);
    const def = doc.subgraphs?.[seg.subgraphId];
    if (!node || !def || subgraphIdOf(node.op) !== seg.subgraphId) return false;
    level = def;
  }
  return true;
}

// ------------------------------------------------- 子图定义 → OperatorDesc

const NO_PARAMS: Param[] = [];

/** 与 C++ 的 synthesizeOperator 同构：端口与参数来自 inputs/outputs/params。 */
export function synthesizeOperator(subgraphId: string, def: SubgraphDef): OperatorDesc {
  const inputs: Port[] = (def.inputs ?? []).map((i) => ({
    name: i.name,
    type: i.type,
    label: i.label ?? i.name,
    doc: i.doc ?? "",
    required: (i.to?.length ?? 0) > 0,
  }));
  const outputs: Port[] = (def.outputs ?? []).map((o) => ({
    name: o.name,
    type: o.type,
    label: o.label ?? o.name,
    doc: o.doc ?? "",
  }));
  const params: Param[] = (def.params ?? []).map((p) => {
    const { binds: _binds, ...rest } = p;
    return rest as Param;
  });
  return {
    id: SUBGRAPH_OP_PREFIX + subgraphId,
    version: def.version ?? "1.0.0",
    label: def.name || subgraphId,
    category: def.category || "Subgraph",
    keywords: def.keywords ?? [],
    doc: def.doc ?? "",
    inputs,
    outputs,
    params: params.length > 0 ? params : NO_PARAMS,
    capabilities: { cancellable: false, previewable: false, deterministic: true },
  };
}

const augmentCache = new WeakMap<object, WeakMap<object, ReadonlyMap<string, OperatorDesc>>>();

/** manifest 的算子表 + 本文档里的 `sub:` 合成项。按对象身份缓存，doc 不变就不重建。 */
export function augmentOperators(
  base: ReadonlyMap<string, OperatorDesc>,
  subgraphs: GraphDoc["subgraphs"],
): ReadonlyMap<string, OperatorDesc> {
  if (!subgraphs || Object.keys(subgraphs).length === 0) return base;
  let perBase = augmentCache.get(base as object);
  if (!perBase) {
    perBase = new WeakMap();
    augmentCache.set(base as object, perBase);
  }
  const hit = perBase.get(subgraphs);
  if (hit) return hit;
  const merged = new Map(base);
  for (const [id, def] of Object.entries(subgraphs)) {
    merged.set(SUBGRAPH_OP_PREFIX + id, synthesizeOperator(id, def));
  }
  perBase.set(subgraphs, merged);
  return merged;
}

// ------------------------------------------------------------ 合成 / 解散

interface Boundary {
  inputs: { name: string; type: string; to: PortRef[]; from: PortRef }[];
  outputs: { name: string; type: string; from: PortRef; to: PortRef[] }[];
}

function uniqueName(taken: Set<string>, base: string): string {
  if (!taken.has(base)) {
    taken.add(base);
    return base;
  }
  for (let i = 2; ; i += 1) {
    const candidate = `${base}_${i}`;
    if (!taken.has(candidate)) {
      taken.add(candidate);
      return candidate;
    }
  }
}

/** 跨边界的边 → 子图的输入/输出端口。名字取内部端口名，冲突加后缀。 */
function boundaryOf(
  ctx: GraphContext,
  level: GraphLevel,
  doc: GraphDoc,
  picked: ReadonlySet<string>,
): Boundary {
  const anyTypes = inferAnyTypes(ctx, { ...doc, nodes: level.nodes, edges: level.edges });
  const typeOf = (ref: PortRef, side: "input" | "output"): string => {
    const node = level.nodes.find((n) => n.id === ref.node);
    const op = node ? ctx.operatorsById.get(node.op) : undefined;
    const port = findPort(op, ref.port, side);
    if (!port) return ANY;
    return port.type === ANY ? (anyTypes.get(ref.node) ?? ANY) : port.type;
  };

  const inputNames = new Set<string>();
  const outputNames = new Set<string>();
  const inputs: Boundary["inputs"] = [];
  const outputs: Boundary["outputs"] = [];

  for (const edge of level.edges) {
    const fromIn = picked.has(edge.from.node);
    const toIn = picked.has(edge.to.node);
    if (fromIn === toIn) continue;
    if (!fromIn && toIn) {
      // 外面进来：同一个外部端口喂进来的几条边合并成一个输入（schema 的 to 是数组）
      const same = inputs.find(
        (i) => i.from.node === edge.from.node && i.from.port === edge.from.port,
      );
      if (same) {
        same.to.push({ ...edge.to });
        continue;
      }
      inputs.push({
        name: uniqueName(inputNames, edge.to.port),
        type: typeOf(edge.to, "input"),
        to: [{ ...edge.to }],
        from: { ...edge.from },
      });
    } else {
      const same = outputs.find(
        (o) => o.from.node === edge.from.node && o.from.port === edge.from.port,
      );
      if (same) {
        same.to.push({ ...edge.to });
        continue;
      }
      outputs.push({
        name: uniqueName(outputNames, edge.from.port),
        type: typeOf(edge.from, "output"),
        from: { ...edge.from },
        to: [{ ...edge.to }],
      });
    }
  }
  return { inputs, outputs };
}

export interface ComposeResult {
  subgraphId: string;
  nodeId: string;
}

/** 把选中的节点收成一个子图。就地改 draft，返回新节点与子图的 id。 */
export function composeSubgraph(
  ctx: GraphContext,
  doc: GraphDoc,
  path: SubPath,
  ids: readonly string[],
  taken: Set<string>,
): ComposeResult | null {
  const level = levelOf(doc, path);
  const picked = new Set(ids.filter((id) => level.nodes.some((n) => n.id === id)));
  if (picked.size === 0) return null;

  const boundary = boundaryOf(ctx, level, doc, picked);
  const inner = level.nodes.filter((n) => picked.has(n.id));
  const innerEdges = level.edges.filter((e) => picked.has(e.from.node) && picked.has(e.to.node));

  const subgraphId = newLocalId("sg", new Set(Object.keys(doc.subgraphs ?? {})));
  const nodeId = newLocalId("n", taken);
  taken.add(nodeId);

  // 落点取选区的中心，不然新节点会跑到原点去
  let x = 0;
  let y = 0;
  for (const n of inner) {
    x += n.ui?.position?.x ?? 0;
    y += n.ui?.position?.y ?? 0;
  }
  const position = { x: Math.round(x / inner.length), y: Math.round(y / inner.length) };

  const def: SubgraphDef = {
    name: `子图 ${Object.keys(doc.subgraphs ?? {}).length + 1}`,
    nodes: inner.map((n) => JSON.parse(JSON.stringify(n)) as GraphNode),
    edges: innerEdges.map((e) => JSON.parse(JSON.stringify(e)) as GraphEdge),
    inputs: boundary.inputs.map((i) => ({ name: i.name, type: i.type, to: i.to })),
    outputs: boundary.outputs.map((o) => ({ name: o.name, type: o.type, from: o.from })),
    params: [],
  };

  // 被收进去的节点上有参数绑着顶层图参数（纳入配方）：在子图里把它提升成外参，绑定改指到新的
  // 实例上 —— 与「在子图里纳入配方」走出来的是同一种形状（K2），图参数的值照样管着它。
  // 不改的话 bind 指着一个已经不在顶层的节点，存下来就是 unknown_bind
  if (path.length === 0) {
    for (const gp of Object.values(doc.params ?? {})) {
      gp.binds = gp.binds.map((bind) => {
        const dot = bind.lastIndexOf(".");
        const target = { node: bind.slice(0, dot), param: bind.slice(dot + 1) };
        if (dot <= 0 || !picked.has(target.node)) return bind;
        const node = inner.find((n) => n.id === target.node);
        const decl = node ? ctx.operatorsById.get(node.op)?.params.find((p) => p.name === target.param) : undefined;
        const name = uniqueName(new Set(def.params.map((p) => p.name)), target.param);
        const copy = JSON.parse(JSON.stringify(decl ?? { type: gp.type ?? "float" })) as Partial<Param>;
        delete copy.roiBackdrop;
        def.params.push({
          ...(copy as Param),
          name,
          default: JSON.parse(JSON.stringify(gp.default)) as unknown,
          binds: [{ node: target.node, param: target.param }],
        });
        return joinBind(nodeId, name);
      });
    }
  }

  doc.subgraphs = { ...(doc.subgraphs ?? {}), [subgraphId]: def };

  const newEdges: GraphEdge[] = [];
  for (const i of boundary.inputs) {
    const id = newLocalId("e", taken);
    taken.add(id);
    newEdges.push({ id, from: { ...i.from }, to: { node: nodeId, port: i.name } });
  }
  for (const o of boundary.outputs) {
    for (const t of o.to) {
      const id = newLocalId("e", taken);
      taken.add(id);
      newEdges.push({ id, from: { node: nodeId, port: o.name }, to: { ...t } });
    }
  }

  const host = levelOf(doc, path);
  host.nodes = host.nodes.filter((n) => !picked.has(n.id));
  host.edges = host.edges.filter((e) => !picked.has(e.from.node) && !picked.has(e.to.node));
  host.nodes.push({
    id: nodeId,
    op: SUBGRAPH_OP_PREFIX + subgraphId,
    opVersion: def.version ?? "1.0.0",
    params: {},
    ui: { position },
  });
  host.edges.push(...newEdges);
  return { subgraphId, nodeId };
}

/** 解散：把子图内容内联回本层。返回内联出来的节点 id。 */
export function dissolveSubgraph(
  doc: GraphDoc,
  path: SubPath,
  nodeId: string,
  taken: Set<string>,
): string[] {
  const level = levelOf(doc, path);
  const host = level.nodes.find((n) => n.id === nodeId);
  const subgraphId = host ? subgraphIdOf(host.op) : null;
  const def = subgraphId ? doc.subgraphs?.[subgraphId] : undefined;
  if (!host || !def || !subgraphId) return [];

  // 内部 id 在本层可能已经被占，统一重新分配
  const rename = new Map<string, string>();
  for (const n of def.nodes) {
    const id = newLocalId("n", taken);
    taken.add(id);
    rename.set(n.id, id);
  }

  const params = host.params ?? {};
  const inlined: GraphNode[] = def.nodes.map((n) => {
    const copy = JSON.parse(JSON.stringify(n)) as GraphNode;
    copy.id = rename.get(n.id)!;
    const base = host.ui?.position ?? { x: 0, y: 0 };
    copy.ui = {
      ...copy.ui,
      position: {
        x: base.x + (copy.ui?.position?.x ?? 0),
        y: base.y + (copy.ui?.position?.y ?? 0),
      },
    };
    return copy;
  });
  // 提升参数在解散时要落回内参，否则外面调过的值会凭空消失
  for (const p of def.params ?? []) {
    // 这个外参在顶层实例上绑着图参数（纳入配方）：绑定改指到内联出来的内参上，图参数的值照样
    // 管着它们；不改的话 bind 指着一个已经不存在的节点，内参上写的显式值又会撞 param_conflict
    const graphParam = path.length === 0 ? graphParamBoundTo(doc, nodeId, p.name) : undefined;
    const gp = graphParam ? doc.params?.[graphParam] : undefined;
    if (gp) {
      const moved = (p.binds ?? [])
        .filter((b) => rename.has(b.node))
        .map((b) => joinBind(rename.get(b.node)!, b.param));
      gp.binds = [...gp.binds.filter((b) => b !== joinBind(nodeId, p.name)), ...moved];
      continue;
    }
    const value = params[p.name] ?? p.default;
    for (const bind of p.binds ?? []) {
      const target = inlined.find((n) => n.id === rename.get(bind.node));
      if (target) target.params = { ...(target.params ?? {}), [bind.param]: value };
    }
  }

  const newEdges: GraphEdge[] = [];
  for (const e of def.edges) {
    const id = newLocalId("e", taken);
    taken.add(id);
    newEdges.push({
      id,
      from: { node: rename.get(e.from.node)!, port: e.from.port },
      to: { node: rename.get(e.to.node)!, port: e.to.port },
    });
  }

  for (const edge of level.edges) {
    if (edge.to.node === nodeId) {
      const port = def.inputs.find((i) => i.name === edge.to.port);
      for (const t of port?.to ?? []) {
        const id = newLocalId("e", taken);
        taken.add(id);
        newEdges.push({
          id,
          from: { ...edge.from },
          to: { node: rename.get(t.node)!, port: t.port },
        });
      }
    } else if (edge.from.node === nodeId) {
      const port = def.outputs.find((o) => o.name === edge.from.port);
      if (!port) continue;
      const id = newLocalId("e", taken);
      taken.add(id);
      newEdges.push({
        id,
        from: { node: rename.get(port.from.node)!, port: port.from.port },
        to: { ...edge.to },
      });
    }
  }

  level.nodes = level.nodes.filter((n) => n.id !== nodeId);
  level.edges = level.edges.filter((e) => e.from.node !== nodeId && e.to.node !== nodeId);
  level.nodes.push(...inlined);
  level.edges.push(...newEdges);

  // 没人再引用这份定义就把它删掉，免得文件里堆一堆用不上的子图
  if (!isSubgraphUsed(doc, subgraphId)) {
    const rest = { ...(doc.subgraphs ?? {}) };
    delete rest[subgraphId];
    doc.subgraphs = rest;
  }
  return inlined.map((n) => n.id);
}

/** 某个节点的某个输出端口，最终由哪个**展开后**的节点产出（F2）。
 *  库算子解不开（定义在文件里），返回 null。 */
export function resolveOutput(
  doc: GraphDoc,
  path: SubPath,
  localId: string,
  port: string,
): { nodeId: string; port: string } | null {
  // `<port>.<field>`（Bundle 的一个字段，m8-plan L3）：端口名里不会有点，先把字段拆下来，
  // 按端口走完子图边界再接回去 —— 子图对外输出只认端口名。
  const dot = port.indexOf(".");
  const field = dot < 0 ? "" : port.slice(dot);
  let prefix = pathPrefix(path);
  let level = levelOf(doc, path);
  let node = level.nodes.find((n) => n.id === localId);
  let cursor = { id: localId, port: dot < 0 ? port : port.slice(0, dot) };
  for (let depth = 0; depth < 32; depth += 1) {
    if (!node) return null;
    const subgraphId = subgraphIdOf(node.op);
    if (!subgraphId) return { nodeId: prefix + cursor.id, port: cursor.port + field };
    const def = doc.subgraphs?.[subgraphId];
    const out = def?.outputs.find((o) => o.name === cursor.port);
    if (!def || !out) return null;
    prefix = `${prefix}${cursor.id}/`;
    level = def;
    cursor = { id: out.from.node, port: out.from.port };
    node = def.nodes.find((n) => n.id === cursor.id);
  }
  return null;
}

export function isSubgraphUsed(doc: GraphDoc, subgraphId: string): boolean {
  const op = SUBGRAPH_OP_PREFIX + subgraphId;
  if (doc.nodes.some((n) => n.op === op)) return true;
  for (const def of Object.values(doc.subgraphs ?? {})) {
    if (def.nodes.some((n) => n.op === op)) return true;
  }
  return false;
}

/** 内参有没有被提升过。提升过的在内部显示成只读并标注来源。 */
export function promotedBy(
  def: SubgraphDef | undefined,
  nodeId: string,
  param: string,
): SubParam | undefined {
  return def?.params?.find((p) => p.binds?.some((b) => b.node === nodeId && b.param === param));
}
