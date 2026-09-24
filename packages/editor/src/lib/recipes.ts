// 参数配方（param-recipe P3）的纯函数：配方文件怎么读写、放在哪、名字怎么起、规格摘要怎么算、
// 四类失配怎么判、建议怎么修。不碰 store、不碰传输层（node --test 直接测）。
//
// 配方 = 顶层图参数的稀疏覆盖，只有一层（K1）；有效值 = default ← 这个配方存的值（K4，总是从基础叠）。
// 文件格式、目录约定与四类失配的规则写在 docs/recipe.md；P4 的 CLI 在 Rust 里再实现一遍，
// 两边对着同一组夹具（schema/fixtures/recipes/）验，规则改了两边与夹具一起改。

import { curveProblem, summarize as summarizeCurve } from "./curve";
import { valueEquals } from "./params";
import { sha256Hex } from "./sha256";
import { summarize as summarizeTransform } from "./transform";
import type { GraphDoc, GraphParam } from "../types/graph";
import type { EnumOption } from "../types/manifest";

export const RECIPE_SCHEMA_VERSION = 1;
export const RECIPE_EXT = ".lyflow-recipe.json";
/** 目录下可选的索引：{ default?: 名字, order?: [名字…] }。 */
export const RECIPE_INDEX = "index.json";
/** 30 s 自动备份写的那一份（整个内存里的配方集合）。不是配方文件，列目录时不当配方读。 */
export const RECIPE_AUTOSAVE = "autosave~.json";
/** 下拉框里的「基础」：只用 default。配方不能叫这个名字，免得两个「基础」分不清。 */
export const BASE_LABEL = "基础";

export interface RecipeGraphRef {
  id: string;
  specDigest: string;
}

export interface RecipeEntry {
  /** 内存里的稳定身份：编辑、改名都不变，复制 / 导入出来的是新的。不进文件 —— 存盘时靠它
   *  认出「这是改名」而不是「删一个、新建一个」。 */
  id: string;
  name: string;
  /** { 图参数名: 值 }，只存与基础不同的那些。 */
  values: Readonly<Record<string, unknown>>;
  note?: string | undefined;
  /** 文件里记着的图 id 与规格摘要（失配 ④ 拿它比）。读来的文件没写就是 null。 */
  graph: RecipeGraphRef | null;
  updatedAt: string | null;
}

/** 内存里的配方集合。不可变：每次改动换一个新对象，撤销栈（K7）存的就是它的快照。 */
export interface RecipeSet {
  recipes: readonly RecipeEntry[];
  /** index.json 的 default：打开图时选它（P3.3）。 */
  defaultName: string | null;
}

export const EMPTY_SET: RecipeSet = Object.freeze({ recipes: Object.freeze([]) as readonly RecipeEntry[], defaultName: null });

let idSeq = 0;
/** 内存身份。只在这一次会话里唯一就够了。 */
export function newRecipeId(): string {
  idSeq += 1;
  return `r${Date.now().toString(36)}${idSeq.toString(36)}`;
}

const has = (o: object, k: string) => Object.prototype.hasOwnProperty.call(o, k);

// ------------------------------------------------------------ 目录与文件名

/** 图文件同目录的 `<图文件名去扩展名>.recipes/`。`.lyflow.json` 整个去掉（`demo.lyflow.json` → `demo.recipes`），
 *  其它文件去最后一个扩展名。分隔符跟着图路径走（Windows 的反斜杠、HTTP 工作区的相对路径都原样）。 */
export function recipeDirOf(graphPath: string): string {
  const cut = Math.max(graphPath.lastIndexOf("/"), graphPath.lastIndexOf("\\"));
  const parent = cut >= 0 ? graphPath.slice(0, cut + 1) : "";
  const file = graphPath.slice(cut + 1);
  let stem = file;
  if (/\.lyflow\.json$/i.test(file)) stem = file.slice(0, -".lyflow.json".length);
  else if (file.lastIndexOf(".") > 0) stem = file.slice(0, file.lastIndexOf("."));
  return `${parent}${stem}.recipes`;
}

export function joinPath(dir: string, name: string): string {
  const sep = dir.includes("\\") && !dir.includes("/") ? "\\" : "/";
  return dir.endsWith("/") || dir.endsWith("\\") ? `${dir}${name}` : `${dir}${sep}${name}`;
}

export function baseNameOf(path: string): string {
  const cut = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
  return path.slice(cut + 1);
}

export function recipeFileName(name: string): string {
  return `${name}${RECIPE_EXT}`;
}

/** `A.lyflow-recipe.json` → `A`；不是配方文件返回 null。 */
export function recipeStem(fileName: string): string | null {
  return fileName.toLowerCase().endsWith(RECIPE_EXT) && fileName.length > RECIPE_EXT.length
    ? fileName.slice(0, -RECIPE_EXT.length)
    : null;
}

const BAD_CHARS = /[\\/:*?"<>|\u0000-\u001f]/;
const RESERVED = /^(con|prn|aux|nul|com[1-9]|lpt[1-9])$/i;

/** 配方名为什么不能用。null = 能用。名字就是文件名（`<名字>.lyflow-recipe.json`），所以按 Windows
 *  文件名的规矩来；重名不分大小写（Windows 的文件系统不分）。self = 改名时它自己现在的名字。 */
export function recipeNameProblem(set: RecipeSet, name: string, self?: string | null): string | null {
  if (!name.trim()) return "名字不能为空";
  if (name !== name.trim()) return "名字首尾不能有空格";
  if (name.length > 80) return "名字太长（最多 80 个字符）";
  if (BAD_CHARS.test(name)) return '名字里不能有 \\ / : * ? " < > | 这些字符';
  if (name.endsWith(".")) return "名字不能以 . 结尾";
  if (RESERVED.test(name)) return `${name} 是 Windows 的保留名`;
  if (name === BASE_LABEL) return `「${BASE_LABEL}」指的是只用默认值，配方不能叫这个名字`;
  const lower = name.toLowerCase();
  const clash = set.recipes.find((r) => r.name.toLowerCase() === lower && r.name !== self);
  if (clash) return `已经有配方 ${clash.name}`;
  return null;
}

/** 以 base 为底起一个不重名的配方名：base、base 2、base 3…… */
export function uniqueRecipeName(set: RecipeSet, base: string): string {
  const clean = base.replace(BAD_CHARS, "_").trim() || "配方";
  let name = clean;
  for (let i = 2; recipeNameProblem(set, name) !== null; i += 1) name = `${clean} ${i}`;
  return name;
}

// ------------------------------------------------------------ 规格摘要（specDigest）

/** 按 Unicode 码点比（Rust 的 String 比较按 UTF-8 字节，与码点序相同；JS 默认按 UTF-16 码元，
 *  在 BMP 之外会不一样 —— 两边要算出同一个摘要，只能都按码点）。 */
export function compareCodePoints(a: string, b: string): number {
  const x = Array.from(a);
  const y = Array.from(b);
  const n = Math.min(x.length, y.length);
  for (let i = 0; i < n; i += 1) {
    const d = x[i]!.codePointAt(0)! - y[i]!.codePointAt(0)!;
    if (d !== 0) return d;
  }
  return x.length - y.length;
}

const finite = (v: unknown): v is number => typeof v === "number" && Number.isFinite(v);

/** 规格摘要的规范形（docs/recipe.md「specDigest」）：每个图参数一个五元组
 *  `[名字, type, min, max, options]`，按名字的码点序排；没有 type 的老格式只留名字（其余四项 null ——
 *  它们在 core 里本来就不算规格，P1.2）；options 只取 value、按各自的 JSON 文本排序、空的算 null。
 *  label、单位、group、softMin/softMax、default、binds 都不进摘要：改它们不改变「什么值合法」。
 *  序列化用 JSON.stringify（无空白；数用 ECMAScript 的 Number::toString，Rust 侧用 ryu-js 同一格式）。 */
export function specCanonical(doc: Pick<GraphDoc, "params">): string {
  const rows = Object.entries(doc.params ?? {}).map(([name, gp]) => {
    const typed = typeof gp.type === "string";
    return [
      name,
      typed ? gp.type : null,
      typed && finite(gp.min) ? gp.min : null,
      typed && finite(gp.max) ? gp.max : null,
      typed ? optionsCanonical(gp.options) : null,
    ] as const;
  });
  rows.sort((a, b) => compareCodePoints(a[0], b[0]));
  return JSON.stringify(rows);
}

function optionsCanonical(options: readonly EnumOption[] | undefined): unknown[] | null {
  if (!Array.isArray(options) || options.length === 0) return null;
  return options
    .map((o) => o.value)
    .sort((a, b) => compareCodePoints(JSON.stringify(a), JSON.stringify(b)));
}

const digestCache = new WeakMap<object, string>();

/** `sha256:<64 位小写十六进制>`，对 specCanonical 的 UTF-8 字节。按 doc.params 的对象身份缓存：
 *  失配报告随 doc 每变一次对每个配方都要它，而图参数本身很少变（immer 的结构共享让它们的身份不变）。 */
export function specDigest(doc: Pick<GraphDoc, "params">): string {
  const key = doc.params;
  const hit = key ? digestCache.get(key) : undefined;
  if (hit) return hit;
  const digest = `sha256:${sha256Hex(specCanonical(doc))}`;
  if (key) digestCache.set(key, digest);
  return digest;
}

export function graphRefOf(doc: GraphDoc): RecipeGraphRef {
  return { id: doc.id, specDigest: specDigest(doc) };
}

// ------------------------------------------------------------ 读写文件

/** 配方文件的文本（schema/recipe.schema.json）。values 按图参数在 doc 里的顺序排（diff 稳定），
 *  图里已经没有的名字（失配 ①）排在后面、原样保留 —— 删不删由用户在失配报告里决定。 */
export function serializeRecipe(entry: RecipeEntry, doc: GraphDoc): string {
  const values: Record<string, unknown> = {};
  for (const k of Object.keys(doc.params ?? {})) if (has(entry.values, k)) values[k] = entry.values[k];
  for (const [k, v] of Object.entries(entry.values)) if (!has(values, k)) values[k] = v;
  const out: Record<string, unknown> = {
    schemaVersion: RECIPE_SCHEMA_VERSION,
    name: entry.name,
    graph: entry.graph ?? graphRefOf(doc),
    values,
  };
  if (entry.note) out["note"] = entry.note;
  out["updatedAt"] = entry.updatedAt ?? new Date().toISOString();
  return `${JSON.stringify(out, null, 2)}\n`;
}

export interface ParsedRecipe {
  name: string;
  values: Record<string, unknown>;
  note?: string | undefined;
  graph: RecipeGraphRef | null;
  updatedAt: string | null;
  /** 读得出来、但不完全合格的地方（缺字段、多字段）。不拦加载。 */
  notes: string[];
}

/** 宽松地读一个配方文件：读得出值就读，缺的补上并记一笔。真读不出来（不是 JSON、不是对象、
 *  schemaVersion 比编辑器新）才返回 error。fallbackName 是文件名去扩展名。 */
export function parseRecipeText(text: string, fallbackName: string): ParsedRecipe | { error: string } {
  let raw: unknown;
  try {
    raw = JSON.parse(text);
  } catch (e) {
    return { error: `不是合法的 JSON：${e instanceof Error ? e.message : String(e)}` };
  }
  if (typeof raw !== "object" || raw === null || Array.isArray(raw)) return { error: "顶层应当是一个对象" };
  const o = raw as Record<string, unknown>;
  const notes: string[] = [];
  const version = o["schemaVersion"];
  if (typeof version === "number" && version > RECIPE_SCHEMA_VERSION) {
    return { error: `schemaVersion ${version} 比这个编辑器认识的（${RECIPE_SCHEMA_VERSION}）新` };
  }
  if (version === undefined) notes.push("缺 schemaVersion");
  const name = typeof o["name"] === "string" && o["name"].trim() ? o["name"] : fallbackName;
  if (typeof o["name"] !== "string") notes.push("缺 name，用了文件名");
  let values: Record<string, unknown> = {};
  const v = o["values"];
  if (typeof v === "object" && v !== null && !Array.isArray(v)) values = { ...(v as Record<string, unknown>) };
  else notes.push("values 不是对象，当作空配方");
  const g = o["graph"] as Record<string, unknown> | undefined;
  const graph =
    g && typeof g === "object" && typeof g["id"] === "string" && typeof g["specDigest"] === "string"
      ? { id: g["id"], specDigest: g["specDigest"] }
      : null;
  const known = new Set(["schemaVersion", "name", "graph", "values", "note", "updatedAt"]);
  const extra = Object.keys(o).filter((k) => !known.has(k));
  if (extra.length > 0) notes.push(`不认识的字段 ${extra.join("、")}（存盘时去掉）`);
  return {
    name,
    values,
    note: typeof o["note"] === "string" ? o["note"] : undefined,
    graph,
    updatedAt: typeof o["updatedAt"] === "string" ? o["updatedAt"] : null,
    notes,
  };
}

export function entryFromParsed(p: ParsedRecipe, name = p.name): RecipeEntry {
  return { id: newRecipeId(), name, values: p.values, note: p.note, graph: p.graph, updatedAt: p.updatedAt };
}

export interface RecipeIndex {
  defaultName: string | null;
  order: string[];
}

export function parseIndex(text: string): RecipeIndex {
  try {
    const o = JSON.parse(text) as { default?: unknown; order?: unknown };
    return {
      defaultName: typeof o?.default === "string" ? o.default : null,
      order: Array.isArray(o?.order) ? o.order.filter((x): x is string => typeof x === "string") : [],
    };
  } catch {
    return { defaultName: null, order: [] };
  }
}

export function serializeIndex(set: RecipeSet): string {
  const out: Record<string, unknown> = {};
  if (set.defaultName && set.recipes.some((r) => r.name === set.defaultName)) out["default"] = set.defaultName;
  out["order"] = set.recipes.map((r) => r.name);
  return `${JSON.stringify(out, null, 2)}\n`;
}

/** 按 index.json 的 order 排；没列到的按名字（码点序）接在后面。 */
export function orderEntries(entries: readonly RecipeEntry[], order: readonly string[]): RecipeEntry[] {
  const rank = new Map(order.map((n, i) => [n, i]));
  return [...entries].sort((a, b) => {
    const ra = rank.get(a.name);
    const rb = rank.get(b.name);
    if (ra !== undefined && rb !== undefined) return ra - rb;
    if (ra !== undefined) return -1;
    if (rb !== undefined) return 1;
    return compareCodePoints(a.name, b.name);
  });
}

// ------------------------------------------------------------ 集合上的改动（都返回新对象）

export function findRecipe(set: RecipeSet, name: string | null): RecipeEntry | undefined {
  return name === null ? undefined : set.recipes.find((r) => r.name === name);
}

export function replaceRecipe(set: RecipeSet, name: string, next: RecipeEntry): RecipeSet {
  let hit = false;
  const recipes = set.recipes.map((r) => {
    if (r.name !== name) return r;
    hit = true;
    return next;
  });
  if (!hit) return set;
  // 改名时 default 跟着走
  const defaultName = set.defaultName === name ? next.name : set.defaultName;
  return { recipes, defaultName };
}

/** 被人改过的配方：记下改的时刻，并认定它现在是对着**当前这张图**维护的（graph 换成当前的 id 与摘要，
 *  失配 ④ 随之消失）。在撤销事务里做，所以撤销会连这两项一起还原。 */
export function touch(entry: RecipeEntry, doc: GraphDoc, now: string): RecipeEntry {
  return { ...entry, graph: graphRefOf(doc), updatedAt: now };
}

/** 写一个值（稀疏：等于基础就删掉这条覆盖）。返回原对象 = 什么都没变。 */
export function withValue(entry: RecipeEntry, doc: GraphDoc, param: string, value: unknown): RecipeEntry {
  const base = doc.params && has(doc.params, param) ? doc.params[param]?.default : undefined;
  const had = has(entry.values, param);
  if (base !== undefined && valueEquals(value, base)) return had ? withoutValue(entry, param) : entry;
  if (had && valueEquals(entry.values[param], value)) return entry;
  return { ...entry, values: { ...entry.values, [param]: structuredClone(value) } };
}

export function withoutValue(entry: RecipeEntry, param: string): RecipeEntry {
  if (!has(entry.values, param)) return entry;
  const values = { ...entry.values };
  delete values[param];
  return { ...entry, values };
}

/** 图参数改名：每个配方里那个键跟着改名（位置不变）。 */
export function renameParamInSet(set: RecipeSet, doc: GraphDoc, from: string, to: string, now: string): RecipeSet {
  let changed = false;
  const recipes = set.recipes.map((r) => {
    if (!has(r.values, from)) return r;
    changed = true;
    const values = Object.fromEntries(Object.entries(r.values).map(([k, v]) => [k === from ? to : k, v]));
    return touch({ ...r, values }, doc, now);
  });
  return changed ? { ...set, recipes } : set;
}

// ------------------------------------------------------------ 四类失配

export type MismatchKind = "extra" | "type" | "range" | "spec";

export type RecipeFix =
  | { action: "delete" }
  | { action: "set"; value: unknown }
  /** ④ 的建议：把文件里记的图 id 与摘要换成当前图的。 */
  | { action: "rebase" };

export interface Mismatch {
  kind: MismatchKind;
  /** ①–③ 是图参数名；④ 是 null（整个配方的事）。 */
  param: string | null;
  message: string;
  fix: RecipeFix;
  fixLabel: string;
}

export interface RecipeReport {
  items: Mismatch[];
  /** ①–③ 的条数：不为 0 时这个配方不能运行（P3.7）。④ 只提示。 */
  blocking: number;
  /** 按参数名查 ①–③ 那一条（矩阵单元格的三态用）。 */
  byParam: ReadonlyMap<string, Mismatch>;
}

export const KIND_LABEL: Record<MismatchKind, string> = {
  extra: "多出",
  type: "类型不符",
  range: "越界",
  spec: "规格变了",
};

/** 失配报告（P3.7）。顺序：④（整个配方）在前，然后按参数名的码点序。每个参数至多一条，
 *  取第一个不满足的：① → ② → ③。没有 type 的老格式图参数只查 ①（core 也不按它校验，P1.2）。 */
export function recipeReport(doc: GraphDoc, entry: Pick<RecipeEntry, "values" | "graph">, digest?: string): RecipeReport {
  const items: Mismatch[] = [];
  const current = digest ?? specDigest(doc);
  if (!entry.graph) {
    items.push({
      kind: "spec",
      param: null,
      message: "文件里没有记录图的 id 与规格摘要，不知道它是对着哪张图写的",
      fix: { action: "rebase" },
      fixLabel: "记成当前图",
    });
  } else if (entry.graph.id !== doc.id || entry.graph.specDigest !== current) {
    const parts: string[] = [];
    if (entry.graph.id !== doc.id) parts.push(`图 id 不同（配方记的是 ${entry.graph.id}）`);
    if (entry.graph.specDigest !== current) parts.push("图参数的规格（名字、类型、限位、options）与写配方时不同");
    items.push({
      kind: "spec",
      param: null,
      message: `${parts.join("；")}。不阻止运行，值照常逐个检查`,
      fix: { action: "rebase" },
      fixLabel: "按当前图更新记录",
    });
  }
  const byParam = new Map<string, Mismatch>();
  const names = Object.keys(entry.values).sort(compareCodePoints);
  for (const name of names) {
    // 只认 doc.params 自己的键：`constructor`、`toString` 这类名字不能从原型链上摸到一个「图参数」
    const gp = doc.params && has(doc.params, name) ? doc.params[name] : undefined;
    const value = entry.values[name];
    let m: Mismatch | null = null;
    if (!gp) {
      m = {
        kind: "extra",
        param: name,
        message: `图里没有图参数 ${name}（改名或删掉了？）`,
        fix: { action: "delete" },
        fixLabel: "删除这个值",
      };
    } else {
      const c = checkValue(gp, value);
      if (c) m = { ...c, param: name };
    }
    if (m) {
      items.push(m);
      byParam.set(name, m);
    }
  }
  return { items, blocking: byParam.size, byParam };
}

type Check = Omit<Mismatch, "param">;

const short = (v: unknown): string => {
  const s = JSON.stringify(v) ?? String(v);
  return s.length > 48 ? `${s.slice(0, 45)}…` : s;
};

/** 四舍五入，.5 远离 0（与 Rust 的 f64::round 同一个口径，JS 的 Math.round 对负数是向上）。 */
export function roundHalfAway(x: number): number {
  return Math.sign(x) * Math.round(Math.abs(x));
}

const NUMERIC = /^\s*[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?\s*$/;

/** 能不能当成一个数：有限的数，或写成十进制数的字符串。 */
function asNumber(v: unknown): number | undefined {
  if (finite(v)) return v;
  if (typeof v === "string" && NUMERIC.test(v)) {
    const n = Number(v);
    return Number.isFinite(n) ? n : undefined;
  }
  return undefined;
}

function clampNum(x: number, min: number | undefined, max: number | undefined, integer: boolean): number {
  let lo = min;
  let hi = max;
  if (integer) {
    if (lo !== undefined) lo = Math.ceil(lo);
    if (hi !== undefined) hi = Math.floor(hi);
  }
  if (lo !== undefined && x < lo) return lo;
  if (hi !== undefined && x > hi) return hi;
  return x;
}

/** 数（或每个分量）越过 min / max 的第一处；都在界内返回 null。文案与 core 的 checkRange 一致。 */
function rangeProblem(xs: readonly number[], min: number | undefined, max: number | undefined): string | null {
  for (let i = 0; i < xs.length; i += 1) {
    const at = xs.length > 1 ? `第 ${i + 1} 个分量` : "";
    if (min !== undefined && xs[i]! < min) return `${at}不能小于 ${min}`;
    if (max !== undefined && xs[i]! > max) return `${at}不能大于 ${max}`;
  }
  return null;
}

function typeCheck(message: string, converted: unknown): Check {
  return converted === undefined
    ? { kind: "type", message, fix: { action: "delete" }, fixLabel: "删除这个值（用基础）" }
    : { kind: "type", message, fix: { action: "set", value: converted }, fixLabel: `改为 ${short(converted)}` };
}

function rangeCheck(message: string, clamped: unknown): Check {
  return { kind: "range", message, fix: { action: "set", value: clamped }, fixLabel: `夹到限位：${short(clamped)}` };
}

const VEC_LEN: Partial<Record<string, number>> = { vec2f: 2, vec3f: 3, vec4f: 4, transform: 16 };

/** 一个值对着图参数规格查 ②（类型不符）与 ③（越界 / 不在 options 里）。与 core 的 coerceParam + checkRange
 *  同一套判据（P1.2），另外给出修复建议：能转换就转换（再夹进限位），不能就删；越界夹到限位；不在 options 里改为默认。 */
export function checkValue(gp: GraphParam, v: unknown): Check | null {
  const min = finite(gp.min) ? gp.min : undefined;
  const max = finite(gp.max) ? gp.max : undefined;
  switch (gp.type) {
    case undefined:
      return null;
    case "bool": {
      if (typeof v === "boolean") return null;
      let b: boolean | undefined;
      if (v === 0 || v === 1) b = v === 1;
      else if (typeof v === "string" && /^(true|false)$/i.test(v.trim())) b = v.trim().toLowerCase() === "true";
      return typeCheck(`应当是 true / false，实际是 ${short(v)}`, b);
    }
    case "int":
    case "flags": {
      if (finite(v) && Number.isInteger(v)) {
        const p = rangeProblem([v], min, max);
        return p ? rangeCheck(p, clampNum(v, min, max, true)) : null;
      }
      const n = typeof v === "boolean" ? undefined : asNumber(v);
      const what = gp.type === "flags" ? "整数位掩码" : "整数";
      return typeCheck(`应当是${what}，实际是 ${short(v)}`, n === undefined ? undefined : clampNum(roundHalfAway(n), min, max, true));
    }
    case "float": {
      if (finite(v)) {
        const p = rangeProblem([v], min, max);
        return p ? rangeCheck(p, clampNum(v, min, max, false)) : null;
      }
      const n = typeof v === "boolean" ? undefined : asNumber(v);
      return typeCheck(`应当是数字，实际是 ${short(v)}`, n === undefined ? undefined : clampNum(n, min, max, false));
    }
    case "vec2f":
    case "vec3f":
    case "vec4f":
    case "transform":
    case "color": {
      const color = gp.type === "color";
      const n = color ? 3 : VEC_LEN[gp.type]!;
      const okLen = (len: number) => len === n || (color && len === 4);
      if (Array.isArray(v) && okLen(v.length) && v.every(finite)) {
        const p = rangeProblem(v, min, max);
        return p ? rangeCheck(p, v.map((x) => clampNum(x, min, max, false))) : null;
      }
      let conv: number[] | undefined;
      if (finite(v) && gp.type !== "transform") conv = new Array<number>(n).fill(v);
      else if (Array.isArray(v) && v.every(finite) && v.length > (color ? 4 : n)) conv = v.slice(0, color ? 4 : n);
      else if (color && typeof v === "string") conv = hexColor(v);
      const want = color ? "3 或 4 个数（RGB / RGBA）" : `${n} 个数`;
      return typeCheck(
        `应当是 ${want}的数组，实际是 ${short(v)}`,
        conv?.map((x) => clampNum(x, min, max, false)),
      );
    }
    case "enum": {
      const options = (gp.options ?? []).map((o) => o.value);
      if (typeof v === "string") {
        if (options.length === 0 || options.includes(v)) return null;
        return {
          kind: "range",
          message: `'${v}' 不在选项里（${options.join(" / ")}）`,
          fix: { action: "delete" },
          fixLabel: "改为默认（删除这条覆盖）",
        };
      }
      const s = typeof v === "number" || typeof v === "boolean" ? String(v) : undefined;
      return typeCheck(
        `应当是选项里的字符串，实际是 ${short(v)}`,
        s !== undefined && (options.length === 0 || options.includes(s)) ? s : undefined,
      );
    }
    case "string":
    case "text":
    case "path": {
      if (typeof v === "string") return null;
      const s = typeof v === "number" || typeof v === "boolean" ? String(v) : undefined;
      return typeCheck(`应当是字符串，实际是 ${short(v)}`, s);
    }
    case "curve": {
      const shape = curveProblem(v);
      if (shape) return typeCheck(`曲线的格式不对：${shape}`, undefined);
      const p = curveProblem(v, min, max);
      if (!p) return null;
      const c = v as { points: [number, number][]; interp?: string };
      const clamped: Record<string, unknown> = { points: c.points.map(([x, y]) => [x, clampNum(y, min, max, false)]) };
      if (c.interp !== undefined) clamped["interp"] = c.interp;
      return rangeCheck(p, clamped);
    }
  }
  return null;
}

/** `#rrggbb` / `#rrggbbaa` → [r, g, b(, a)]，各分量 = 字节 / 255。 */
function hexColor(s: string): number[] | undefined {
  const m = /^#([0-9a-f]{6}|[0-9a-f]{8})$/i.exec(s.trim());
  if (!m) return undefined;
  const hex = m[1]!;
  const out: number[] = [];
  for (let i = 0; i < hex.length; i += 2) out.push(parseInt(hex.slice(i, i + 2), 16) / 255);
  return out;
}

/** 按建议修（P3.7「全部按建议修复」给整份报告，单条的按钮给一条）。改过就 touch。 */
export function applyFixes(entry: RecipeEntry, items: readonly Mismatch[], doc: GraphDoc, now: string): RecipeEntry {
  let next = entry;
  let rebase = false;
  for (const m of items) {
    if (m.fix.action === "rebase") {
      rebase = true;
      continue;
    }
    if (m.param === null) continue;
    next = m.fix.action === "delete" ? withoutValue(next, m.param) : withValue(next, doc, m.param, m.fix.value);
  }
  if (next === entry && !rebase) return entry;
  return touch(next, doc, now);
}

// ------------------------------------------------------------ 显示

/** 单元格与「配方 · 基础 X」标签上的一小段文字。 */
export function formatValue(v: unknown, spec?: { type?: string | undefined; options?: readonly EnumOption[] | undefined }): string {
  const num = (x: number) => String(Number(x.toPrecision(4)));
  if (v === undefined) return "—";
  switch (spec?.type) {
    case "bool":
      return v === true ? "开" : v === false ? "关" : short(v);
    case "enum": {
      const o = spec.options?.find((x) => x.value === v);
      return o ? o.label || String(o.value) : short(v);
    }
    case "transform":
      return Array.isArray(v) && v.length === 16 ? summarizeTransform(v) : short(v);
    case "curve":
      return curveProblem(v) === null ? summarizeCurve(v) : short(v);
    case "color":
      if (Array.isArray(v) && v.every(finite)) {
        const hex = v.slice(0, 3).map((x) => Math.round(Math.max(0, Math.min(1, x)) * 255).toString(16).padStart(2, "0"));
        return `#${hex.join("")}${v.length === 4 ? ` α${num(v[3]!)}` : ""}`;
      }
      return short(v);
    default:
      break;
  }
  if (finite(v)) return num(v);
  if (Array.isArray(v) && v.every(finite)) return `[${v.map(num).join(", ")}]`;
  if (typeof v === "string") return v.length > 40 ? `${v.slice(0, 37)}…` : v;
  return short(v);
}
