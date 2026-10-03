// 调参记录：每一次正式运行记下交给 core 的那份图与图参数、跑完的量测读数。调参是「改一个阈值 → 跑 → 看 gap 变了多少」
// 的循环，以前上一次是多少只能靠脑子记；撤销历史记的是改动，不是结果。这里只放纯函数，store 在 store/runHistory.ts。

import { num } from "./format";
import { docNodeKey, listGraphNodes } from "./findNodes";
import { effectiveParams, valueEquals } from "./params";
import { levelOf } from "./subgraph";
import type { NodeExecution } from "../store/execution";
import type { RunStatus } from "../types/execution";
import type { GraphDoc } from "../types/graph";
import type { OperatorDesc } from "../types/manifest";

/** 一次运行里一个量测输出的读数。 */
export interface RunReading {
  /** 完整 id（子图里的是路径 id）。 */
  id: string;
  port: string;
  value: number | null;
  unit: string | null;
  verdict: string | null;
}

export interface RunRecord {
  /** 第几次（这张图打开以来，从 1 数）。 */
  seq: number;
  runId: string;
  /** 开跑的时刻（Date.now）。 */
  at: number;
  /** 交给 core 的那份图（store 里的文档是不可变的，留引用不费内存）。 */
  doc: GraphDoc;
  /** 交给 core 的图参数取值（当前配方已经叠上去了）。 */
  params: Readonly<Record<string, unknown>> | undefined;
  recipe: string | null;
  /** 运行到哪几个节点（完整 id）；空 = 整张图。 */
  targets: readonly string[];
  status: RunStatus | "running";
  durationMs: number | null;
  readings: RunReading[];
}

/** 两次运行之间改了什么（一个参数一条）。 */
export interface ParamChange {
  /** 节点的完整 id；图参数是 `gp:<名字>`。 */
  id: string;
  /** 「子图 › 节点」或「图参数」。 */
  where: string;
  param: string;
  from: unknown;
  to: unknown;
}

export interface RunDiff {
  changes: ParamChange[];
  /** 节点名（带层级）。 */
  added: string[];
  removed: string[];
  muted: string[];
  unmuted: string[];
}

/** 节点表里这次运行的量测读数，按 id、端口排好（同一张图两次之间顺序稳定，好对着看）。 */
export function runReadingsOf(nodes: ReadonlyMap<string, NodeExecution>): RunReading[] {
  const out: RunReading[] = [];
  for (const [id, n] of nodes) {
    for (const o of n.stats?.outputs ?? []) {
      const v = o.value;
      if (v?.kind !== "Measurement") continue;
      out.push({
        id,
        port: o.port,
        value: typeof v.value === "number" && Number.isFinite(v.value) ? v.value : null,
        unit: v.unit ?? null,
        verdict: v.verdict || null,
      });
    }
  }
  return out.sort((a, b) => (a.id === b.id ? a.port.localeCompare(b.port) : a.id.localeCompare(b.id)));
}

/** 上一次同一个读数是多少（没有就 null）。 */
export function previousReading(prev: RunRecord | undefined, r: RunReading): RunReading | null {
  return prev?.readings.find((p) => p.id === r.id && p.port === r.port) ?? null;
}

/** 「比上一次」：records 新的在前，从第 from 条之后往更早找，第一条没被取消、而且有这个读数的（被顶掉的、
 *  运行到别处没算它的都跳过）。 */
export function previousReadingIn(records: readonly RunRecord[], from: number, r: RunReading): RunReading | null {
  for (const p of records.slice(from + 1)) {
    if (p.status === "cancelled") continue;
    const hit = previousReading(p, r);
    if (hit) return hit;
  }
  return null;
}

/** 参数值写短：数字六位有效数字，向量逐个，字符串原样（太长截掉）。 */
export function shortValue(v: unknown): string {
  if (typeof v === "number") return num(v);
  if (Array.isArray(v)) return `[${v.map(shortValue).join(", ")}]`;
  if (typeof v === "string") return v.length > 24 ? `${v.slice(0, 23)}…` : v || "（空）";
  if (typeof v === "boolean") return v ? "开" : "关";
  if (v === undefined || v === null) return "—";
  const text = JSON.stringify(v);
  return text.length > 24 ? `${text.slice(0, 23)}…` : text;
}

/** 从 prev 到 next 改了什么：每个节点的有效参数（缺省值也算进去，稀疏存储的增删不算改动）、静音、增删节点、图参数。
 *  子图定义里的节点按路径 id 对上；同一个 id 换了算子的当成删了再加。 */
export function diffRuns(
  prev: Pick<RunRecord, "doc" | "params">,
  next: Pick<RunRecord, "doc" | "params">,
  ops: ReadonlyMap<string, OperatorDesc>,
): RunDiff {
  const out: RunDiff = { changes: [], added: [], removed: [], muted: [], unmuted: [] };
  const index = (doc: GraphDoc) => new Map(listGraphNodes(doc, ops).map((e) => [e.id, e]));
  const before = index(prev.doc);
  const after = index(next.doc);
  const nameOf = (e: { title: string; parents: string[] }) => [...e.parents, e.title].join(" › ");
  // 子图定义是共用的：两个实例里列出来的是文档里同一个节点，改一处只算一处
  const seen = new Set<string>();
  const once = (e: Parameters<typeof docNodeKey>[0], tag: string) => {
    const key = `${tag}\u0001${docNodeKey(e)}`;
    if (seen.has(key)) return false;
    seen.add(key);
    return true;
  };
  for (const [id, a] of after) {
    const b = before.get(id);
    if (!b || b.opId !== a.opId) {
      if (once(a, "+")) out.added.push(nameOf(a));
      if (b && once(b, "-")) out.removed.push(nameOf(b));
      continue;
    }
    if (!once(a, "=")) continue;
    if (a.muted !== b.muted) (a.muted ? out.muted : out.unmuted).push(nameOf(a));
    const op = ops.get(a.opId);
    if (!op) continue;
    const nodeA = levelOf(next.doc, a.path).nodes.find((n) => n.id === a.localId);
    const nodeB = levelOf(prev.doc, b.path).nodes.find((n) => n.id === b.localId);
    if (!nodeA || !nodeB || nodeA.params === nodeB.params) continue;
    const pa = effectiveParams(op, nodeA);
    const pb = effectiveParams(op, nodeB);
    for (const param of op.params) {
      if (valueEquals(pa[param.name], pb[param.name])) continue;
      out.changes.push({ id, where: nameOf(a), param: param.label || param.name, from: pb[param.name], to: pa[param.name] });
    }
  }
  for (const [id, b] of before) if (!after.has(id) && once(b, "-")) out.removed.push(nameOf(b));
  const names = new Set([...Object.keys(prev.params ?? {}), ...Object.keys(next.params ?? {})]);
  for (const name of names) {
    const from = prev.params?.[name];
    const to = next.params?.[name];
    if (valueEquals(from, to)) continue;
    const label = next.doc.params?.[name]?.label ?? prev.doc.params?.[name]?.label ?? name;
    out.changes.push({ id: `gp:${name}`, where: "图参数", param: label, from, to });
  }
  return out;
}

/** 一行字说完改了什么：「体素 · Leaf Size 0.02 → 0.03；…」，太多写「等 N 处」。没改写「参数没变」。 */
export function diffText(d: RunDiff, max = 3): string {
  const parts = [
    ...d.changes.map((c) => `${c.where} · ${c.param} ${shortValue(c.from)} → ${shortValue(c.to)}`),
    ...d.muted.map((n) => `静音 ${n}`),
    ...d.unmuted.map((n) => `取消静音 ${n}`),
    ...d.added.map((n) => `加了 ${n}`),
    ...d.removed.map((n) => `删了 ${n}`),
  ];
  if (parts.length === 0) return "参数没变";
  return parts.length > max ? `${parts.slice(0, max).join("；")} 等 ${parts.length} 处` : parts.join("；");
}
