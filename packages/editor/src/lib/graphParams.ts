// 顶层图参数（param-recipe P1）的纯函数：绑定怎么写、谁绑着谁、规格从 manifest 怎么抄、
// 名字怎么起、有效值怎么合成。改图的动作在 store/graph.ts，这里不碰任何 store（node --test 直接测）。
//
// 有效值 = default ← 当前配方的覆盖（K3/K4：总是从基础重新叠，不叠在上一个配方上）。
// P1 里覆盖恒为空，但每个读值的地方都经 effectiveGraphValues 拿，P3 接上配方时不用改调用方。

import type { GraphDoc, GraphNode, GraphParam, ParamSpec } from "../types/graph";
import type { OperatorDesc, Param } from "../types/manifest";
import type { SubPath } from "./subgraph";

/** `<节点>.<参数>`，按**最后一个** `.` 切：节点 id 可以含 `.`，参数名不含。 */
export function splitBind(bind: string): { node: string; param: string } | null {
  const dot = bind.lastIndexOf(".");
  if (dot <= 0 || dot === bind.length - 1) return null;
  return { node: bind.slice(0, dot), param: bind.slice(dot + 1) };
}

export function joinBind(node: string, param: string): string {
  return `${node}.${param}`;
}

/** 哪个顶层图参数绑着顶层节点的这个参数。只看顶层：binds 的目标只能是顶层节点（含子图实例）。 */
export function graphParamBoundTo(doc: GraphDoc, nodeId: string, param: string): string | undefined {
  const target = joinBind(nodeId, param);
  for (const [name, gp] of Object.entries(doc.params ?? {})) {
    if (gp.binds?.includes(target)) return name;
  }
  return undefined;
}

/** 当前层级里一个参数「最终由哪个图参数提供」。 */
export interface GraphBinding {
  graphParam: string;
  /** 从这一层往外经过的子图参数名（内 → 外），顶层节点上是空数组。 */
  via: string[];
  /** 链条的顶端：图参数真正绑着的那个顶层节点参数。 */
  top: { node: string; param: string };
}

/** 顺着子图提升链往外找（K2 的逐层提升链）：内参 → 子图参数 → …… → 顶层实例上的图参数。
 *  链在哪一层断了（某一层没提升、或顶层实例没绑图参数）就返回 null。 */
export function resolveGraphBinding(
  doc: GraphDoc,
  path: SubPath,
  nodeId: string,
  param: string,
): GraphBinding | null {
  let node = nodeId;
  let name = param;
  const via: string[] = [];
  for (let i = path.length - 1; i >= 0; i -= 1) {
    const seg = path[i]!;
    const def = doc.subgraphs?.[seg.subgraphId];
    const sp = def?.params?.find((p) => p.binds?.some((b) => b.node === node && b.param === name));
    if (!sp) return null;
    via.push(sp.name);
    node = seg.nodeId;
    name = sp.name;
  }
  const graphParam = graphParamBoundTo(doc, node, name);
  return graphParam ? { graphParam, via, top: { node, param: name } } : null;
}

/** 一个图参数的有效值：当前配方覆盖了就用覆盖，否则用 default（K3/K4）。 */
export function graphParamValue(
  doc: GraphDoc,
  name: string,
  overrides: Readonly<Record<string, unknown>>,
): unknown {
  if (Object.prototype.hasOwnProperty.call(overrides, name)) return overrides[name];
  return doc.params?.[name]?.default;
}

/** 运行时交给 core 的 `{名字: 值}`（RunOptions.params）。只认图里还声明着的名字 ——
 *  配方里多出来的名字是 P3 失配报告的事，不该让 core 报 unknown_param 把整次运行拦下。
 *  图没有任何图参数时返回 undefined：载荷与 P1 之前逐字节相同。 */
export function effectiveGraphValues(
  doc: GraphDoc,
  overrides: Readonly<Record<string, unknown>>,
): Record<string, unknown> | undefined {
  const names = Object.keys(doc.params ?? {});
  if (names.length === 0) return undefined;
  const out: Record<string, unknown> = {};
  for (const name of names) out[name] = graphParamValue(doc, name, overrides);
  return out;
}

/** 从被绑定目标的参数声明抄一份图参数规格（P1.1）。
 *  - name、default 不抄：名字是 params 的键，default 由调用方给「当前有效值」；
 *  - visibleWhen / enabledWhen、roiBackdrop 不抄：它们指的是**同一算子**的其它参数名，
 *    到了图这一层对不上（与子图提升时丢 roiBackdrop 同一个理由），semantic 照抄；
 *  - label 写成调用方给的「节点标题 · 参数 label」，在参数面板里一眼认得出是谁。 */
export function specFromParam(decl: Param, label: string): ParamSpec & Pick<GraphParam, "type"> {
  const {
    name: _name,
    default: _default,
    visibleWhen: _visible,
    enabledWhen: _enabled,
    roiBackdrop: _backdrop,
    ...rest
  } = decl as Param & { binds?: unknown };
  const spec = { ...rest, label } as ParamSpec & Pick<GraphParam, "type"> & { binds?: unknown };
  delete spec.binds; // 从子图参数（SubParam）抄的时候它带着内部的 binds
  return spec;
}

/** 图参数名为什么不能用。null = 能用。名字是宿主、CLI `--param`、配方文件引用它的键：
 *  不能空、不能含 `.`（CLI 的 eval 靠「左边有没有 `.`」区分图参数与扫描轴）、不能含 `=`。 */
export function graphParamNameProblem(doc: GraphDoc, name: string, self?: string): string | null {
  if (!name.trim()) return "名字不能为空";
  if (name !== name.trim()) return "名字首尾不能有空格";
  if (name.includes(".")) return "名字不能含 .（CLI 用它区分图参数与 <节点>.<参数>）";
  if (name.includes("=")) return "名字不能含 =";
  if (name !== self && doc.params?.[name] !== undefined) return `已经有图参数 ${name}`;
  return null;
}

/** 以 base 为底起一个不重名的图参数名：base、base_2、base_3……（与子图提升参数同一条规则）。 */
export function uniqueGraphParamName(doc: GraphDoc, base: string): string {
  const clean = base.replace(/[.=\s]/g, "_") || "param";
  let name = clean;
  for (let i = 2; doc.params?.[name] !== undefined; i += 1) name = `${clean}_${i}`;
  return name;
}

/** 节点在显示层面的「有效参数」：被图参数绑定的那几个换成图参数的有效值（P1.4）。
 *  返回一份节点的浅拷贝，节点本身不动；没有任何绑定时原样返回（引用不变，组件不白重渲）。
 *  只用于**显示与联动判断**（Inspector、2D 拖框）—— 写回永远走 store 的动作。 */
export function withBoundValues(
  doc: GraphDoc,
  path: SubPath,
  node: GraphNode,
  paramNames: readonly string[],
  overrides: Readonly<Record<string, unknown>>,
): GraphNode {
  if (!doc.params || Object.keys(doc.params).length === 0) return node;
  let params: Record<string, unknown> | null = null;
  for (const name of paramNames) {
    const binding = resolveGraphBinding(doc, path, node.id, name);
    if (!binding) continue;
    params ??= { ...(node.params ?? {}) };
    params[name] = graphParamValue(doc, binding.graphParam, overrides);
  }
  return params ? { ...node, params } : node;
}

/** 第一个被绑定目标在 manifest 里的声明。老格式的图参数没有 type，规格就借它；
 *  参数面板「已改动」的判据（与算子默认不同）也拿它的 default 比。 */
export function boundDecl(
  doc: GraphDoc,
  gp: GraphParam,
  ops: ReadonlyMap<string, OperatorDesc>,
): Param | null {
  for (const bind of gp.binds) {
    const t = splitBind(bind);
    const node = t ? doc.nodes.find((n) => n.id === t.node) : undefined;
    const decl = t && node ? ops.get(node.op)?.params.find((p) => p.name === t.param) : undefined;
    if (decl) return decl;
  }
  return null;
}

/** 一个图参数用什么控件画（Inspector 的简表与参数面板的「图参数」分组共用）。给了 type 的用它自己的
 *  规格（P1.1）；老格式没有 type，就借第一个被绑定目标的声明，label 换成图参数自己的。 */
export function graphParamSpecOf(
  doc: GraphDoc,
  name: string,
  gp: GraphParam,
  ops: ReadonlyMap<string, OperatorDesc>,
): Param | null {
  if (gp.type) {
    const { binds: _binds, ...spec } = gp;
    return { ...spec, name, type: gp.type } as Param;
  }
  const decl = boundDecl(doc, gp, ops);
  return decl ? { ...decl, name, label: gp.label ?? decl.label ?? name, default: gp.default } : null;
}
