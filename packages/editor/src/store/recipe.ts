// 当前配方（param-recipe K3/K6）。配方 = 图参数的稀疏覆盖，只有一层；core 不知道配方的存在，
// 编辑器在运行、校验、编译计划时把「default ← 当前配方覆盖」合成成 {名字: 值} 交给它。
//
// P1 里当前配方恒为「基础」（current = null），覆盖恒为空：在被绑定参数的行上编辑 = 改图参数的
// default（K6 ① 在基础下的那一半）。P3 在这里接上配方集合、切换与撤销（K7），调用方不用改 ——
// 读值一律经 runParamsOf / useGraphParamOverrides，写值一律经 graph store 的 editGraphParamValue。
//
// 这个模块不 import graph store：graph store 要反过来读它（写回节点时用「当前有效值」）。

import { create } from "zustand";

import { effectiveGraphValues } from "../lib/graphParams";
import type { GraphDoc } from "../types/graph";

const NONE: Readonly<Record<string, unknown>> = Object.freeze({});

interface RecipeState {
  /** 当前配方的名字。null = 「基础」（只用 default）。P1 恒为 null。 */
  current: string | null;
  /** 当前配方存的值 { 图参数名: 值 }，只有与基础不同的那些。P1 恒为空。 */
  overrides: Readonly<Record<string, unknown>>;
}

export const useRecipeStore = create<RecipeState>(() => ({
  current: null,
  overrides: NONE,
}));

/** 当前配方的覆盖。非 React 代码（store 动作、运行、校验）用它。 */
export function currentOverrides(): Readonly<Record<string, unknown>> {
  return useRecipeStore.getState().overrides;
}

/** 这次运行 / 校验 / 编译该交给 core 的图参数取值（K3）。图没有图参数时是 undefined。 */
export function runParamsOf(doc: GraphDoc): Record<string, unknown> | undefined {
  return effectiveGraphValues(doc, currentOverrides());
}

/** React 组件订阅当前覆盖：P3 切配方时 Inspector 跟着重算显示值。 */
export function useGraphParamOverrides(): Readonly<Record<string, unknown>> {
  return useRecipeStore((s) => s.overrides);
}
