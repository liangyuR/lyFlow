// 参数值的读写规则。核心是**稀疏存储**（docs/graph-doc.md，理由见 README「参数的稀疏存储」）：
// 读 = manifest 默认值 ← 节点覆盖值；写回默认值时必须**删键**，而不是存一个等于默认的副本。

import type { GraphDoc, GraphNode } from "../types/graph";
import type { Condition, OperatorDesc, Param } from "../types/manifest";

/** 深比较。参数值只可能是标量、字符串或数字数组（vec/color/transform）。 */
export function valueEquals(a: unknown, b: unknown): boolean {
  if (a === b) return true;
  if (Array.isArray(a) && Array.isArray(b)) {
    return a.length === b.length && a.every((v, i) => valueEquals(v, b[i]));
  }
  return false;
}

export function defaultParams(op: OperatorDesc): Record<string, unknown> {
  const out: Record<string, unknown> = {};
  for (const p of op.params) out[p.name] = p.default;
  return out;
}

/** 节点当前的完整参数值 = manifest 默认值 ← 节点覆盖值。 */
export function effectiveParams(
  op: OperatorDesc,
  node: Pick<GraphNode, "params">,
): Record<string, unknown> {
  const out = defaultParams(op);
  for (const [k, v] of Object.entries(node.params ?? {})) {
    // 只认 manifest 里仍然存在的参数。算子删过参数的老图会残留无主键值，
    // 那些应该被忽略而不是渲染成幽灵控件。
    if (k in out) out[k] = v;
  }
  return out;
}

export function effectiveValue(
  op: OperatorDesc,
  node: Pick<GraphNode, "params">,
  paramName: string,
): unknown {
  const override = node.params?.[paramName];
  if (override !== undefined) return override;
  return op.params.find((p) => p.name === paramName)?.default;
}

/** 写一个参数，返回新的稀疏 params。
 *  值等于 manifest 默认值时删除该键 —— 这是稀疏存储的关键一半。 */
export function sparseSet(
  op: OperatorDesc,
  params: Record<string, unknown> | undefined,
  name: string,
  value: unknown,
): Record<string, unknown> {
  const next = { ...(params ?? {}) };
  const def = op.params.find((p) => p.name === name)?.default;
  if (valueEquals(value, def)) {
    delete next[name];
  } else {
    next[name] = value;
  }
  return next;
}

/** 丢弃 manifest 里已经不存在的参数键。打开老图时用。 */
export function pruneUnknownParams(
  op: OperatorDesc,
  params: Record<string, unknown> | undefined,
): Record<string, unknown> {
  const known = new Set(op.params.map((p) => p.name));
  const out: Record<string, unknown> = {};
  for (const [k, v] of Object.entries(params ?? {})) {
    if (known.has(k)) out[k] = v;
  }
  return out;
}

// ---------------------------------------------------------------- 参数联动

export function isConditionMet(
  cond: Condition | undefined,
  effective: Record<string, unknown>,
): boolean {
  if (!cond) return true;
  const actual = effective[cond.param];
  if (cond.eq !== undefined) return valueEquals(actual, cond.eq);
  if (cond.ne !== undefined) return !valueEquals(actual, cond.ne);
  if (cond.in !== undefined) return cond.in.some((v) => valueEquals(actual, v));
  // 只写了 param 没写判据：视为无条件成立，而不是永假 —— 静默隐藏一个参数
  // 比多显示一个参数难排查得多。
  return true;
}

export function isVisible(param: Param, effective: Record<string, unknown>): boolean {
  return isConditionMet(param.visibleWhen, effective);
}

export function isEnabled(param: Param, effective: Record<string, unknown>): boolean {
  return isConditionMet(param.enabledWhen, effective);
}

/** 分组：先按 group 字段聚合，再把 advanced 的收到末尾的「高级」组。
 *  保持 manifest 里的声明顺序 —— 算子作者排的顺序是有意义的。 */
export interface ParamGroup {
  name: string;
  advanced: boolean;
  params: Param[];
}

export function groupParams(params: readonly Param[]): ParamGroup[] {
  const basic = new Map<string, Param[]>();
  const advanced = new Map<string, Param[]>();

  for (const p of params) {
    const target = p.advanced ? advanced : basic;
    const key = p.group ?? "";
    const list = target.get(key);
    if (list) list.push(p);
    else target.set(key, [p]);
  }

  const out: ParamGroup[] = [];
  for (const [name, list] of basic) out.push({ name, advanced: false, params: list });
  for (const [name, list] of advanced) {
    out.push({ name: name || "高级", advanced: true, params: list });
  }
  return out;
}

// ---------------------------------------------------------------- 相对路径

/** Windows 盘符、UNC，以及 POSIX 的绝对路径。 */
function isAbsolutePath(p: string): boolean {
  return /^[a-zA-Z]:[\\/]/.test(p) || p.startsWith("\\\\") || p.startsWith("/");
}

/** 图里有没有用到相对路径的参数。相对路径按**图文件所在目录**解析（C++ 侧的 baseDir），
 *  没保存过的图带相对路径就跑不了 —— 与其让 core 报「文件不存在」，不如先说「先保存」。 */
export function hasRelativePathParam(
  doc: GraphDoc,
  operatorsById: ReadonlyMap<string, OperatorDesc>,
): boolean {
  // 子图里的路径参数一样要查：展开之后它们和顶层节点没有区别（F1）
  const nodes = [...doc.nodes];
  for (const def of Object.values(doc.subgraphs ?? {})) nodes.push(...def.nodes);
  for (const node of nodes) {
    const op = operatorsById.get(node.op);
    if (!op) continue;
    for (const param of op.params) {
      if (param.type !== "path") continue;
      const value = effectiveValue(op, node, param.name);
      if (typeof value === "string" && value !== "" && !isAbsolutePath(value)) return true;
    }
  }
  return false;
}
