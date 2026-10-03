// 撤销 / 重做一步，并用一句 toast 说是哪一步。Ctrl+Z 是盲按的 —— 工具栏按钮的 title 才写着下一步是什么，
// 连按几下时不看提示就不知道退到了哪。快捷键与工具栏按钮都走这里，两处说的一样。

import { useGraphStore, type HistoryEntry } from "../store/graph";
import type { RecipeSet } from "../lib/recipes";
import { useUiStore } from "../store/ui";

/** 栈是空的时什么都不做。 */
export function stepHistory(dir: "undo" | "redo"): void {
  const graph = useGraphStore.getState();
  const stack = dir === "undo" ? graph.past : graph.future;
  const label = stack[stack.length - 1]?.label;
  if (label === undefined) return;
  if (dir === "undo") graph.undo();
  else graph.redo();
  useUiStore.getState().showToast(`${dir === "undo" ? "已撤销" : "已重做"}：${label}`);
}

/** 撤销历史列表的一行：做完这一步之后的样子。steps 是点它要走几步（负 = 撤销、正 = 重做、0 = 现在）。 */
export interface HistoryRow {
  label: string;
  steps: number;
  kind: "future" | "current" | "past" | "origin";
  /** 这一步之后的图与配方正是存盘的那一份。 */
  saved: boolean;
}

/** 新的在上：还能重做的（最远的在最上）、现在、撤销得回去的、最早的「打开时」。saved 是存盘时的图与配方集合
 *  （没存过盘、或从备份恢复的算没保存时是 null）。存盘点已经掉出撤销栈（只留 100 步）时哪一行都不标。 */
export function historyRows(
  past: readonly HistoryEntry[],
  future: readonly HistoryEntry[],
  now: { doc: unknown; recipes: RecipeSet },
  saved: { doc: unknown; recipes: RecipeSet } | null,
): HistoryRow[] {
  const isSaved = (doc: unknown, recipes: RecipeSet) => saved !== null && doc === saved.doc && recipes === saved.recipes;
  const rows: HistoryRow[] = [];
  for (let j = 0; j < future.length; j += 1) {
    const e = future[j]!;
    rows.push({ label: e.label, steps: future.length - j, kind: "future", saved: isSaved(e.doc, e.recipes) });
  }
  for (let i = past.length - 1; i >= 0; i -= 1) {
    const after = i === past.length - 1 ? now : { doc: past[i + 1]!.doc, recipes: past[i + 1]!.recipes };
    rows.push({ label: past[i]!.label, steps: i - (past.length - 1), kind: i === past.length - 1 ? "current" : "past", saved: isSaved(after.doc, after.recipes) });
  }
  const origin = past[0] ? { doc: past[0].doc, recipes: past[0].recipes } : now;
  rows.push({ label: "打开时", steps: -past.length, kind: past.length === 0 ? "current" : "origin", saved: isSaved(origin.doc, origin.recipes) });
  return rows;
}

/** 撤销历史列表里点了一行：一次走到那一步，说一声走了几步、到了哪。 */
export function jumpHistory(steps: number, label: string): void {
  const moved = useGraphStore.getState().travel(steps);
  if (moved === 0) return;
  const n = Math.abs(moved);
  useUiStore.getState().showToast(moved < 0 ? `已撤销 ${n} 步：回到「${label}」之后` : `已重做 ${n} 步：到「${label}」`);
}
