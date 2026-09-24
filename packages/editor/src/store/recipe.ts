// 配方（param-recipe P3）。配方 = 图参数的稀疏覆盖，只有一层（K1）；core 不知道配方的存在，
// 编辑器在运行、校验、编译计划时把「default ← 当前配方覆盖」合成成 {名字: 值} 交给它（K3）。
//
// 这里存三样东西，性质各不相同：
// - set：内存里的配方集合。**进撤销栈**（K7）：graph store 的每条历史同时快照 doc 与 set，
//   所以改配方的动作都在 graph store 里（它握着历史），这里只提供读与整份替换；
// - current：当前配方。**不进撤销栈**（切换配方不算一步撤销），也不进 doc；
// - 存盘簿记（saved / savedFiles / diskText）与 K6 ② 的「刚在基础上改过的行」：纯运行时状态。
//
// 读值一律经 runParamsOf / useGraphParamOverrides。这个模块不 import graph store：graph store 要反过来读它。

import { useMemo } from "react";
import { create } from "zustand";

import { effectiveGraphValues } from "../lib/graphParams";
import { EMPTY_SET, findRecipe, KIND_LABEL, recipeReport, type RecipeSet } from "../lib/recipes";
import type { SubPath } from "../lib/subgraph";
import type { GraphDoc } from "../types/graph";

const NONE: Readonly<Record<string, unknown>> = Object.freeze({});

/** K6 ②：选着配方时改了一个没纳入配方的参数（改的是图，影响所有配方）。行上据此提示，
 *  并给「改为只在本配方生效」—— 那一步要知道改之前的值（它成为新图参数的 default）。 */
export interface BaseEdit {
  nodeId: string;
  param: string;
  path: SubPath;
  before: unknown;
  after: unknown;
  /** 改的时候选着的配方。 */
  recipe: string;
}

export type RecipeStatus = "none" | "loading" | "ready" | "error";

interface RecipeState {
  /** 内存里的配方集合（进撤销栈，K7）。 */
  set: RecipeSet;
  /** 当前配方的名字。null = 「基础」（只用 default）。 */
  current: string | null;
  /** 当前配方的内存 id：改名、撤销改名时当前配方跟着这个配方走，而不是按名字丢掉。 */
  currentId: string | null;
  /** 最近一次当前配方是怎么变的：user = 人在下拉框、矩阵、管理页里切的；auto = 打开图时选默认配方、
   *  撤销把当前配方撤没了之类。「自动运行」只跟着 user 的切换跑（P3.8），打开一张图不该自己跑起来。 */
  switchedBy: "user" | "auto";
  /** 当前配方存的值 { 图参数名: 值 }（就是那个配方的 values 对象本身）。基础下是空。 */
  overrides: Readonly<Record<string, unknown>>;
  /** 配方目录（图文件同目录的 `<图名>.recipes/`）。null = 图还没存过盘，不能建配方（P3.2）。 */
  dir: string | null;
  status: RecipeStatus;
  /** 目录里读不出来的文件。 */
  problems: readonly { file: string; message: string }[];
  /** 最近一次存盘（或载入）时的那份集合。配方的 dirty = set 不是它（与 graph store 的 savedDoc 同一个判法：
   *  撤销回到保存点时 dirty 复原）。 */
  saved: RecipeSet;
  /** 存盘簿记：配方的内存 id → 上次写下（或读到）的文件名。改名、删除靠它认。 */
  savedFiles: ReadonlyMap<string, string>;
  /** 文件名 → 上次读到或写下的文本。存盘前拿它判断「文件已被外部修改」。 */
  diskText: ReadonlyMap<string, string>;
  /** K6 ② 的行（键 = 面板行的 key，`<展开后的节点 id>.<参数>`）。 */
  baseEdits: Readonly<Record<string, BaseEdit>>;
}

export const useRecipeStore = create<RecipeState>(() => ({
  set: EMPTY_SET,
  current: null,
  currentId: null,
  switchedBy: "auto",
  overrides: NONE,
  dir: null,
  status: "none",
  problems: [],
  saved: EMPTY_SET,
  savedFiles: new Map(),
  diskText: new Map(),
  baseEdits: {},
}));

function overridesOf(set: RecipeSet, current: string | null): Readonly<Record<string, unknown>> {
  return findRecipe(set, current)?.values ?? NONE;
}

/** 当前的配方集合。graph store 在每条历史里快照它。 */
export function recipeSet(): RecipeSet {
  return useRecipeStore.getState().set;
}

/** 整份替换配方集合（graph store 的动作、撤销重做、载入用）。当前配方被删掉了就退回「基础」。 */
export function applyRecipeSet(set: RecipeSet): void {
  const s = useRecipeStore.getState();
  if (s.set === set) return;
  const entry = s.currentId === null ? undefined : set.recipes.find((r) => r.id === s.currentId);
  const current = entry?.name ?? null;
  useRecipeStore.setState({
    set,
    current,
    currentId: entry?.id ?? null,
    overrides: overridesOf(set, current),
    ...(current !== s.current ? { switchedBy: "auto" as const } : {}),
  });
}

/** 切换当前配方（P3.3 / P3.8）。不进撤销栈、不改 doc、不弹窗；未保存的改动留在内存里（K6 ③）。
 *  有效值总是从基础重新叠（K4）：overrides 换成新配方自己的 values，不叠在上一个上。 */
export function selectRecipe(name: string | null, by: "user" | "auto" = "user"): void {
  const s = useRecipeStore.getState();
  const entry = findRecipe(s.set, name);
  const current = entry?.name ?? null;
  if (current === s.current) return;
  useRecipeStore.setState({
    current,
    currentId: entry?.id ?? null,
    overrides: overridesOf(s.set, current),
    baseEdits: {},
    switchedBy: by,
  });
}

/** 当前配方的覆盖。非 React 代码（store 动作、运行、校验）用它。 */
export function currentOverrides(): Readonly<Record<string, unknown>> {
  return useRecipeStore.getState().overrides;
}

/** 这次运行 / 校验 / 编译该交给 core 的图参数取值（K3）。图没有图参数时是 undefined。 */
export function runParamsOf(doc: GraphDoc): Record<string, unknown> | undefined {
  return effectiveGraphValues(doc, currentOverrides());
}

/** React 组件订阅当前覆盖：切配方时 Inspector、面板跟着重算显示值。 */
export function useGraphParamOverrides(): Readonly<Record<string, unknown>> {
  return useRecipeStore((s) => s.overrides);
}

/** 配方集合有没有没存的改动。 */
export function recipesDirty(): boolean {
  const s = useRecipeStore.getState();
  return s.set !== s.saved;
}

export function useRecipesDirty(): boolean {
  return useRecipeStore((s) => s.set !== s.saved);
}

/** 当前配方的 ①–③ 失配（P3.7：有它们的配方不能运行）。拼成一句给运行入口报错；没有返回 null。 */
export function currentRecipeBlocker(doc: GraphDoc): string | null {
  const s = useRecipeStore.getState();
  const entry = findRecipe(s.set, s.current);
  if (!entry) return null;
  const report = recipeReport(doc, entry);
  if (report.blocking === 0) return null;
  const first = report.items.filter((m) => m.kind !== "spec").slice(0, 3);
  const detail = first.map((m) => `${m.param}（${KIND_LABEL[m.kind]}：${m.message}）`).join("；");
  const more = report.blocking > first.length ? ` 等 ${report.blocking} 处` : "";
  return `配方「${entry.name}」有 ${report.blocking} 处失配，不能运行：${detail}${more}。到「配方管理」按建议修复，或切到别的配方`;
}

/** 每个配方的失配报告，按 doc 与 set 缓存（面板、下拉框、矩阵都要，doc 每变一次各算一遍不划算）。 */
export function useRecipeReports(doc: GraphDoc): ReadonlyMap<string, ReturnType<typeof recipeReport>> {
  const set = useRecipeStore((s) => s.set);
  return useMemo(() => {
    const out = new Map<string, ReturnType<typeof recipeReport>>();
    for (const r of set.recipes) out.set(r.name, recipeReport(doc, r));
    return out;
  }, [doc, set]);
}

/** 载入、换图时把一切清零。 */
export function resetRecipes(dir: string | null, status: RecipeStatus): void {
  useRecipeStore.setState({
    set: EMPTY_SET,
    current: null,
    currentId: null,
    switchedBy: "auto",
    overrides: NONE,
    dir,
    status,
    problems: [],
    saved: EMPTY_SET,
    savedFiles: new Map(),
    diskText: new Map(),
    baseEdits: {},
  });
}

export function noteBaseEdit(key: string, edit: BaseEdit): void {
  const s = useRecipeStore.getState();
  const prev = s.baseEdits[key];
  // 连着改好几次：「改之前」留最早那一次的
  const next = prev && prev.recipe === edit.recipe ? { ...edit, before: prev.before } : edit;
  useRecipeStore.setState({ baseEdits: { ...s.baseEdits, [key]: next } });
}

export function clearBaseEdit(key: string): void {
  const s = useRecipeStore.getState();
  if (!s.baseEdits[key]) return;
  const next = { ...s.baseEdits };
  delete next[key];
  useRecipeStore.setState({ baseEdits: next });
}
