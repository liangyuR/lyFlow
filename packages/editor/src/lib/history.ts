// 撤销 / 重做一步，并用一句 toast 说是哪一步。Ctrl+Z 是盲按的 —— 工具栏按钮的 title 才写着下一步是什么，
// 连按几下时不看提示就不知道退到了哪。快捷键与工具栏按钮都走这里，两处说的一样。

import { useGraphStore } from "../store/graph";
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
