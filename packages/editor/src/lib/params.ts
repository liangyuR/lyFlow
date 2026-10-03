// 参数值的读写规则。核心是**稀疏存储**（docs/graph-doc.md，理由见 README「参数的稀疏存储」）：
// 读 = manifest 默认值 ← 节点覆盖值；写回默认值时必须**删键**，而不是存一个等于默认的副本。

import type { GraphDoc, GraphNode } from "../types/graph";
import type { Condition, OperatorDesc, Param } from "../types/manifest";

/** 深比较。参数值是标量、字符串、数字数组（vec/color/transform），或 curve 那样的普通对象
 *  （{points, interp}）—— 对象按键比，与键的书写顺序无关（core 的 cacheKey 也不看顺序）。 */
export function valueEquals(a: unknown, b: unknown): boolean {
  if (a === b) return true;
  if (Array.isArray(a) && Array.isArray(b)) {
    return a.length === b.length && a.every((v, i) => valueEquals(v, b[i]));
  }
  if (isPlainObject(a) && isPlainObject(b)) {
    const ka = Object.keys(a);
    if (ka.length !== Object.keys(b).length) return false;
    return ka.every((k) => Object.prototype.hasOwnProperty.call(b, k) && valueEquals(a[k], b[k]));
  }
  return false;
}

function isPlainObject(v: unknown): v is Record<string, unknown> {
  return typeof v === "object" && v !== null && !Array.isArray(v);
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

/** 1、2、5 × 10^k 里不大于 x 的那个（x > 0）：步长取整，免得拖出 2499.995 这种值。 */
export function niceStep(x: number): number {
  const base = 10 ** Math.floor(Math.log10(x));
  const m = x / base;
  return Number.parseFloat(((m >= 5 ? 5 : m >= 2 ? 2 : 1) * base).toPrecision(12));
}

/** 数字框拖一格、按一下 ↑↓ 走多少。ref 是当前值。
 *  manifest 声明了 step 就是它；有有限的范围（soft 优先）就取范围的 1/200；都没有就看当前值的量级，取它的 1/100
 *  （10 → 0.1、1e-3 → 1e-5；当前值是 0 时看默认值）；什么都没有就 0.01。整数至少 1。修整成 1/2/5 × 10^k。
 *  以前没范围的一律 0.01、整数一律 1：gap / dts 的参数大多没写范围，10 mm 拖到 20 要拖 4000 px，maxIterations
 *  只能一格一格挪；更糟的是 1e-3 量级的值拖 1 px 就按 0.01 量化成了 0。 */
export function stepFor(param: Param, integer: boolean, ref?: number): number {
  if (param.step !== undefined && param.step > 0) return param.step;
  const lo = param.softMin ?? param.min;
  const hi = param.softMax ?? param.max;
  let raw: number;
  if (lo !== undefined && hi !== undefined && Number.isFinite(lo) && Number.isFinite(hi) && hi > lo) {
    raw = (hi - lo) / 200;
  } else {
    const fallback = typeof param.default === "number" ? Math.abs(param.default) : 0;
    const v = ref !== undefined && Number.isFinite(ref) && ref !== 0 ? Math.abs(ref) : fallback;
    raw = v > 0 ? 10 ** (Math.floor(Math.log10(v)) - 2) : 0.01;
  }
  const step = niceStep(raw);
  return integer ? Math.max(1, Math.round(step)) : step;
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
/** 包着路径的那一对引号：直的、弯的（中文输入法）都认，两头得配成对。 */
const QUOTE_PAIRS: Record<string, string> = { '"': '"', "'": "'", "\u201c": "\u201d", "\u2018": "\u2019" };

/** 路径框里粘进来的字：去掉首尾空白，再去掉包着的一对引号（资源管理器「复制文件地址」给的是 "D:\x.pcd"，
 *  以前原样存下、跑的时候才报「文件不存在」）。只去一对、两头得配成对；中间的引号不动。 */
export function cleanPathText(text: string): string {
  const t = text.trim();
  if (t.length >= 2 && QUOTE_PAIRS[t[0]!] === t[t.length - 1]) return t.slice(1, -1).trim();
  return t;
}

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
