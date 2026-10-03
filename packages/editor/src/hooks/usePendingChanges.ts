// 「还没跑的改动」：屏幕上那次结果之后，参数、静音、节点改了哪些（与调参记录里那一次比，lib/runHistory 的 pendingBase）。
// 工具栏的「改了 N 处未跑」与调参页顶上那一行共用。敲回车、下拉框、勾选框改的值不自动运行（ADR-0011），F5 之前常常
// 已经攒了几处，以前只看得到一个「已过时」。

import { useMemo } from "react";

import { changeCount, diffRuns, pendingBase, type RunDiff, type RunRecord } from "../lib/runHistory";
import { augmentOperators, locateEventNode } from "../lib/subgraph";
import { useExecutionStore } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { runParamsOf, useRecipeStore } from "../store/recipe";
import { useRunHistoryStore } from "../store/runHistory";
import type { ParamChange } from "../lib/runHistory";

export interface PendingChanges {
  base: RunRecord;
  diff: RunDiff;
  count: number;
}

export function usePendingChanges(): PendingChanges | null {
  const records = useRunHistoryStore((s) => s.records);
  const resultRunId = useExecutionStore((s) => s.resultRunId);
  const doc = useGraphStore((s) => s.doc);
  const baseOps = useManifestStore((s) => s.operatorsById);
  // 图参数的取值叠着当前配方：配方集合、选着哪个变了也要重算
  const recipeSet = useRecipeStore((s) => s.set);
  const current = useRecipeStore((s) => s.current);
  return useMemo(() => {
    void recipeSet;
    void current;
    const base = pendingBase(records, resultRunId);
    if (!base) return null;
    const ops = augmentOperators(baseOps, { ...base.doc.subgraphs, ...doc.subgraphs });
    const diff = diffRuns(base, { doc, params: runParamsOf(doc) }, ops);
    const count = changeCount(diff);
    return count > 0 ? { base, diff, count } : null;
  }, [records, resultRunId, doc, baseOps, recipeSet, current]);
}

/** 这一处能不能单独改回去：节点参数（节点还在）、图参数（还在）。静音、增删节点不行（用 Ctrl+Z）。 */
export function canRevert(c: ParamChange): boolean {
  const doc = useGraphStore.getState().doc;
  if (c.from === undefined) return false;
  if (c.id.startsWith("gp:")) return !!doc.params?.[c.name];
  return locateEventNode(doc, c.id) !== null;
}

/** 把这一处改回上一次运行时的值，一条撤销。节点参数按路径 id 找到它在哪一层（子图定义里的也行）；
 *  图参数按现在的配方写（选着配方写进配方，「基础」写 default —— 与在那一行上改一样）。 */
export function revertChange(c: ParamChange): void {
  const graph = useGraphStore.getState();
  if (c.id.startsWith("gp:")) {
    graph.editGraphParamValue(c.name, c.from);
    return;
  }
  const at = locateEventNode(graph.doc, c.id);
  if (at) graph.setParam(at.localId, c.name, c.from, at.path);
}
