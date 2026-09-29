// 两个节点输出的差异表（交互清单 #35，docs/compare-plan.md §2）。纯函数：输入两侧的输出统计与
// 各自正在画的那片云，输出逐字段的 A / B / Δ 行。Δ = A − B。
//
// 规则只有一条 ——「同路径的数值字段逐个比，数组逐分量比，字符串按相等比」，下面按 OutputValue 的
// 各字段摊开；点云的点数与包围盒走载荷头（全量，不受显示抽稀影响）。

import { grouped, num, signed } from "./format";
import type { CloudPayload, OutputStat, OutputValue } from "../types/execution";

export interface CompareSide {
  /** node_state 事件里那份 stats.outputs。 */
  outputs: readonly OutputStat[] | undefined;
  /** 这一侧场景里正在画的那片云（自己的，不含借来的底图）。 */
  cloud: CloudPayload | null;
  /** 那片云是哪个端口（含 `<port>.<field>`）。 */
  cloudPort: string | null;
  /** 来自预览运行（ADR-0011，源头抽稀）。 */
  preview: boolean;
}

export interface DiffRow {
  /** `<port>.<字段路径>`，例如 cloud.points / gap.value / info.data.score */
  key: string;
  /** 端口类型（来自 stats）。 */
  type: string;
  kind: "number" | "vector" | "text" | "count" | "only";
  a: string;
  b: string;
  /** A − B；text 与 only 为 null。 */
  delta: string | null;
  changed: boolean;
  /** kind=only 时：只有哪一侧有。 */
  side?: "A" | "B";
}

export interface DiffResult {
  rows: DiffRow[];
  changed: number;
  same: number;
  only: number;
  /** 两侧来自不同模式的运行（一侧预览、一侧正式）：数字不可比，界面整表变淡并挂徽标。 */
  modeMismatch: boolean;
}

export interface Tolerance {
  absTol: number;
  relTol: number;
}

export const DEFAULT_TOLERANCE: Tolerance = { absTol: 1e-9, relTol: 1e-6 };

/** Record 递归的深度，与 peek/ValueView 的 MAX_DEPTH 同；再深的整块按文本比。 */
const MAX_DEPTH = 2;

type Scalar = number | null | undefined;
type Field =
  | { kind: "number"; key: string; a: Scalar; b: Scalar; unit?: [string | undefined, string | undefined] }
  | { kind: "vector"; key: string; a: readonly number[] | undefined; b: readonly number[] | undefined; maxOnly?: boolean }
  | { kind: "text"; key: string; a: string | undefined; b: string | undefined }
  | { kind: "count"; key: string; a: number | undefined; b: number | undefined };

export function withinTolerance(a: number, b: number, tol: Tolerance = DEFAULT_TOLERANCE): boolean {
  if (a === b) return true;
  if (!Number.isFinite(a) || !Number.isFinite(b)) return false;
  return Math.abs(a - b) <= Math.max(tol.absTol, tol.relTol * Math.max(Math.abs(a), Math.abs(b)));
}

// ---------------------------------------------------------------------------
// 端口配对（§2.2）

interface Entry {
  port: string;
  type: string;
  stat: OutputStat;
}

/** Bundle 端口只比字段项（core 已展开成 `<port>.<field>`），整端口那条跳过，免得同一个数出两遍。
 *  字段项一个都没有时（老载荷）留着整端口，至少比 elementCount。 */
function entriesOf(outputs: readonly OutputStat[] | undefined): Entry[] {
  const all = outputs ?? [];
  return all
    .filter((s) => !(s.type.startsWith("Bundle") && all.some((o) => o.port.startsWith(`${s.port}.`))))
    .map((s) => ({ port: s.port, type: s.type, stat: s }));
}

type Pair = { a: Entry; b: Entry } | { only: "A" | "B"; entry: Entry };

function pairEntries(as: Entry[], bs: Entry[]): Pair[] {
  const pairs: Pair[] = [];
  const restA: Entry[] = [];
  const usedB = new Set<Entry>();
  // 1. 同名
  for (const a of as) {
    const b = bs.find((x) => x.port === a.port && !usedB.has(x));
    if (b) {
      pairs.push({ a, b });
      usedB.add(b);
    } else {
      restA.push(a);
    }
  }
  // 2. （类型，出现顺序）
  const restB = bs.filter((b) => !usedB.has(b));
  const leftA: Entry[] = [];
  for (const a of restA) {
    const i = restB.findIndex((b) => b.type === a.type);
    if (i >= 0) {
      pairs.push({ a, b: restB[i]! });
      restB.splice(i, 1);
    } else {
      leftA.push(a);
    }
  }
  // 3. 仅一侧有
  for (const entry of leftA) pairs.push({ only: "A", entry });
  for (const entry of restB) pairs.push({ only: "B", entry });
  return pairs;
}

// ---------------------------------------------------------------------------
// 每种类型摊开成字段（§2.3）

function pair(v: unknown): [number, number] | undefined {
  return Array.isArray(v) && v.length === 2 && v.every((n) => typeof n === "number")
    ? (v as [number, number])
    : undefined;
}

function nums(v: unknown): number[] | undefined {
  return Array.isArray(v) && v.every((n) => typeof n === "number") ? (v as number[]) : undefined;
}

function scalar(v: unknown): Scalar {
  if (v === null) return null;
  return typeof v === "number" ? v : undefined;
}

function sub(p: readonly number[] | undefined, q: readonly number[] | undefined): number[] | undefined {
  return p && q && p.length === q.length ? p.map((x, i) => x - q[i]!) : undefined;
}

function add(p: readonly number[] | undefined, q: readonly number[] | undefined): number[] | undefined {
  return p && q && p.length === q.length ? p.map((x, i) => x + q[i]!) : undefined;
}

function half(p: readonly number[] | undefined): number[] | undefined {
  return p?.map((x) => x / 2);
}

function boundsOf(cloud: CloudPayload | null): { min?: number[]; max?: number[] } {
  if (!cloud || cloud.bounds.length < 6) return {};
  const b = Array.from(cloud.bounds);
  return { min: b.slice(0, 3), max: b.slice(3, 6) };
}

/** 这一侧的云是不是这个端口的。 */
function cloudFor(side: CompareSide, port: string): CloudPayload | null {
  return side.cloudPort === port ? side.cloud : null;
}

function fieldsFor(type: string, port: string, ea: Entry, eb: Entry, sa: CompareSide, sb: CompareSide): Field[] {
  const va: OutputValue = ea.stat.value ?? { kind: "" };
  const vb: OutputValue = eb.stat.value ?? { kind: "" };
  const k = (name: string) => `${port}.${name}`;

  switch (type) {
    case "PointCloud": {
      const ca = cloudFor(sa, ea.port);
      const cb = cloudFor(sb, eb.port);
      const ba = boundsOf(ca);
      const bb = boundsOf(cb);
      return [
        { kind: "count", key: k("points"), a: ca?.totalPoints ?? ea.stat.elementCount, b: cb?.totalPoints ?? eb.stat.elementCount },
        { kind: "vector", key: k("bounds.min"), a: ba.min, b: bb.min },
        { kind: "vector", key: k("bounds.max"), a: ba.max, b: bb.max },
        { kind: "vector", key: k("size"), a: sub(ba.max, ba.min), b: sub(bb.max, bb.min) },
      ];
    }
    case "Box2D": {
      const [amin, amax, bmin, bmax] = [pair(va.min), pair(va.max), pair(vb.min), pair(vb.max)];
      return [
        { kind: "vector", key: k("min"), a: amin, b: bmin },
        { kind: "vector", key: k("max"), a: amax, b: bmax },
        { kind: "vector", key: k("size"), a: sub(amax, amin), b: sub(bmax, bmin) },
        { kind: "vector", key: k("center"), a: half(add(amax, amin)), b: half(add(bmax, bmin)) },
      ];
    }
    case "Line2D": {
      // 一侧有线段、一侧没有：两套字段都列出来，缺的那侧显示「—」
      const seg = va.hasSegment || vb.hasSegment;
      const ray = !va.hasSegment || !vb.hasSegment;
      const out: Field[] = [];
      if (ray) {
        out.push({ kind: "vector", key: k("point"), a: va.hasSegment ? undefined : va.point, b: vb.hasSegment ? undefined : vb.point });
        out.push({ kind: "vector", key: k("dir"), a: va.hasSegment ? undefined : va.dir, b: vb.hasSegment ? undefined : vb.dir });
      }
      if (seg) {
        const len = (v: OutputValue) => {
          const d = v.hasSegment ? sub(v.end, v.start) : undefined;
          return d ? Math.hypot(d[0]!, d[1]!) : undefined;
        };
        out.push({ kind: "vector", key: k("start"), a: va.hasSegment ? va.start : undefined, b: vb.hasSegment ? vb.start : undefined });
        out.push({ kind: "vector", key: k("end"), a: va.hasSegment ? va.end : undefined, b: vb.hasSegment ? vb.end : undefined });
        out.push({ kind: "number", key: k("length"), a: len(va), b: len(vb) });
      }
      return out;
    }
    case "Circle2D":
      return [
        { kind: "vector", key: k("center"), a: va.center, b: vb.center },
        { kind: "number", key: k("radius"), a: va.radius, b: vb.radius },
      ];
    case "Point2D":
      return [{ kind: "vector", key: k("p"), a: va.p, b: vb.p }];
    case "Measurement": {
      const unit: [string | undefined, string | undefined] = [va.unit, vb.unit];
      return [
        { kind: "number", key: k("value"), a: scalar(va.value), b: scalar(vb.value), unit },
        { kind: "number", key: k("nominal"), a: va.nominal, b: vb.nominal, unit },
        { kind: "number", key: k("upper"), a: va.upper, b: vb.upper, unit },
        { kind: "number", key: k("lower"), a: va.lower, b: vb.lower, unit },
        { kind: "text", key: k("ok"), a: va.ok === undefined ? undefined : String(va.ok), b: vb.ok === undefined ? undefined : String(vb.ok) },
        { kind: "text", key: k("verdict"), a: va.verdict, b: vb.verdict },
        { kind: "text", key: k("message"), a: va.message, b: vb.message },
      ];
    }
    case "Record": {
      const out: Field[] = [];
      recordFields(out, k("data"), va.data, vb.data, 0);
      return out;
    }
    case "Plane":
      return [
        { kind: "vector", key: k("normal"), a: va.normal, b: vb.normal },
        { kind: "number", key: k("d"), a: va.d, b: vb.d },
      ];
    case "Transform":
      return [{ kind: "vector", key: k("m"), a: va.m, b: vb.m, maxOnly: true }];
    case "Tensor":
      return [
        { kind: "text", key: k("shape"), a: va.shape ? `[${va.shape.join(", ")}]` : undefined, b: vb.shape ? `[${vb.shape.join(", ")}]` : undefined },
        { kind: "count", key: k("count"), a: va.count, b: vb.count },
        { kind: "number", key: k("min"), a: scalar(va.min), b: scalar(vb.min) },
        { kind: "number", key: k("max"), a: scalar(va.max), b: scalar(vb.max) },
        { kind: "number", key: k("mean"), a: scalar(va.mean), b: scalar(vb.mean) },
      ];
    case "Image": {
      // 像素不进差异表（走二进制），只比尺寸与逐通道均值
      const size = (v: typeof va) =>
        v.width === undefined ? undefined : `${v.width}×${v.height}×${v.channels} ${v.depth ?? ""}`.trim();
      const means = (v: typeof va) =>
        Array.isArray(v.mean) ? v.mean.map((m) => (typeof m === "number" ? m : NaN)) : undefined;
      return [
        { kind: "text", key: k("size"), a: size(va), b: size(vb) },
        { kind: "vector", key: k("mean"), a: means(va), b: means(vb) },
      ];
    }
    case "Indices":
      return [{ kind: "count", key: k("count"), a: ea.stat.elementCount, b: eb.stat.elementCount }];
    case "Error":
      return [{ kind: "text", key: k("message"), a: va.message, b: vb.message }];
    default:
      return [{ kind: "count", key: k("elementCount"), a: ea.stat.elementCount, b: eb.stat.elementCount }];
  }
}

function isPlainObject(v: unknown): v is Record<string, unknown> {
  return typeof v === "object" && v !== null && !Array.isArray(v);
}

function textOf(v: unknown): string | undefined {
  if (v === undefined) return undefined;
  return typeof v === "string" ? v : JSON.stringify(v);
}

function recordFields(out: Field[], prefix: string, a: unknown, b: unknown, depth: number): void {
  const objA = isPlainObject(a);
  const objB = isPlainObject(b);
  if (depth < MAX_DEPTH && (objA || objB) && (objA || a === undefined) && (objB || b === undefined)) {
    // 一侧整块没有时把有的那侧摊开，另一侧全是「—」
    const ra = objA ? a : {};
    const rb = objB ? b : {};
    const keys = [...Object.keys(ra), ...Object.keys(rb).filter((key) => !(key in ra))];
    for (const key of keys) recordFields(out, `${prefix}.${key}`, ra[key], rb[key], depth + 1);
    return;
  }
  const numLike = (v: unknown) => typeof v === "number" || v === null || v === undefined;
  if (numLike(a) && numLike(b) && (typeof a === "number" || typeof b === "number")) {
    out.push({ kind: "number", key: prefix, a: a as Scalar, b: b as Scalar });
    return;
  }
  const va = nums(a);
  const vb = nums(b);
  if ((va || vb) && (va || a === undefined) && (vb || b === undefined)) {
    out.push({ kind: "vector", key: prefix, a: va, b: vb });
    return;
  }
  out.push({ kind: "text", key: prefix, a: textOf(a), b: textOf(b) });
}

// ---------------------------------------------------------------------------
// 字段 → 行

function vec(v: readonly number[] | undefined): string {
  return v ? `(${v.map(num).join(", ")})` : "—";
}

function withUnit(text: string, unit: string | undefined): string {
  return unit && text !== "—" ? `${text} ${unit}` : text;
}

function percent(delta: number, base: number): string {
  if (base === 0) return "";
  const p = (delta / base) * 100;
  const text = Number(p.toPrecision(2));
  return ` (${p > 0 ? "+" : ""}${text}%)`;
}

function rowOf(f: Field, type: string, tol: Tolerance): DiffRow {
  switch (f.kind) {
    case "number": {
      const [ua, ub] = f.unit ?? [undefined, undefined];
      const a = withUnit(num(f.a), ua);
      const b = withUnit(num(f.b), ub);
      const hasA = typeof f.a === "number";
      const hasB = typeof f.b === "number";
      if (!hasA && !hasB) {
        // 两侧都没有这个字段（undefined）不出行由调用方过滤；都是 null（都没测出）算相同
        return { key: f.key, type, kind: "number", a, b, delta: null, changed: f.a !== f.b };
      }
      if (!hasA || !hasB) return { key: f.key, type, kind: "number", a, b, delta: "—", changed: true };
      if (f.unit && (ua ?? "") !== (ub ?? "")) {
        return { key: f.key, type, kind: "number", a, b, delta: "单位不同", changed: true };
      }
      const changed = !withinTolerance(f.a as number, f.b as number, tol);
      const d = (f.a as number) - (f.b as number);
      return { key: f.key, type, kind: "number", a, b, delta: changed ? withUnit(signed(d), ua) : "0", changed };
    }
    case "vector": {
      const a = vec(f.a);
      const b = vec(f.b);
      if (!f.a && !f.b) return { key: f.key, type, kind: "vector", a, b, delta: null, changed: false };
      if (!f.a || !f.b || f.a.length !== f.b.length) {
        return { key: f.key, type, kind: "vector", a, b, delta: "—", changed: true };
      }
      const pa = f.a;
      const pb = f.b;
      const flags = pa.map((x, i) => !withinTolerance(x, pb[i]!, tol));
      const changed = flags.some(Boolean);
      const d = pa.map((x, i) => (flags[i] ? x - pb[i]! : 0));
      let delta: string;
      if (f.maxOnly) {
        const worst = Math.max(...d.map(Math.abs));
        delta = changed ? `max|Δ| ${num(worst)}` : "0";
      } else {
        delta = `(${d.map((x) => (x === 0 ? "0" : signed(x))).join(", ")})`;
      }
      return { key: f.key, type, kind: "vector", a, b, delta, changed };
    }
    case "text": {
      const a = f.a ?? "—";
      const b = f.b ?? "—";
      return { key: f.key, type, kind: "text", a, b, delta: null, changed: f.a !== f.b };
    }
    case "count": {
      const a = f.a === undefined ? "—" : grouped(f.a);
      const b = f.b === undefined ? "—" : grouped(f.b);
      if (f.a === undefined || f.b === undefined) {
        return { key: f.key, type, kind: "count", a, b, delta: f.a === f.b ? null : "—", changed: f.a !== f.b };
      }
      const d = f.a - f.b;
      const delta = d === 0 ? "0" : `${d > 0 ? "+" : ""}${grouped(d)}${percent(d, f.b)}`;
      return { key: f.key, type, kind: "count", a, b, delta, changed: d !== 0 };
    }
  }
}

/** 两侧的差异表。Δ = A − B；容差默认 absTol 1e-9 / relTol 1e-6。 */
export function diffSides(a: CompareSide, b: CompareSide, tol: Tolerance = DEFAULT_TOLERANCE): DiffResult {
  const rows: DiffRow[] = [];
  for (const p of pairEntries(entriesOf(a.outputs), entriesOf(b.outputs))) {
    if ("only" in p) {
      const count = grouped(p.entry.stat.elementCount);
      rows.push({
        key: p.entry.port,
        type: p.entry.type,
        kind: "only",
        a: p.only === "A" ? count : "—",
        b: p.only === "B" ? count : "—",
        delta: null,
        changed: false,
        side: p.only,
      });
      continue;
    }
    // 两侧类型不同（同名端口换了类型，少见）：只比 elementCount
    const type = p.a.type === p.b.type ? p.a.type : "";
    // 同名配对用端口名；按类型配上的两个端口名不同，键用 A 侧的名字加「/B 侧」
    const port = p.a.port === p.b.port ? p.a.port : `${p.a.port}/${p.b.port}`;
    for (const f of fieldsFor(type, port, p.a, p.b, a, b)) {
      if (f.a === undefined && f.b === undefined && f.kind !== "count") continue;
      rows.push(rowOf(f, p.a.type, tol));
    }
  }
  let changed = 0;
  let same = 0;
  let only = 0;
  for (const r of rows) {
    if (r.kind === "only") only += 1;
    else if (r.changed) changed += 1;
    else same += 1;
  }
  return { rows, changed, same, only, modeMismatch: a.preview !== b.preview };
}
