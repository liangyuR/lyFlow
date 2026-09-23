// 界面上「加节点」「插片段」的公共尾巴：调 store 的语义化动作、选中新节点、把自动连线的结果
// 用一句话告诉人（m8-plan L13 / L14）。拖到画布、面板双击、搜索面板三处都走这里。

import { useGraphStore, type AutoConnectResult } from "../store/graph";
import { findSnippet } from "../store/manifest";
import { useUiStore } from "../store/ui";
import type { SnippetDesc } from "../types/manifest";

function report(result: AutoConnectResult, what: string): void {
  const ui = useUiStore.getState();
  if (result.nodeIds.length === 0) return;
  ui.setSelection(result.nodeIds, []);
  const parts: string[] = [];
  if (result.wired > 0) parts.push(`自动连了 ${result.wired} 条`);
  if (result.ambiguous.length > 0) {
    parts.push(`${result.ambiguous.length} 个输入有多个候选没连（已高亮）`);
  }
  if (result.missing.length > 0) parts.push(`缺算子 ${[...new Set(result.missing)].join(", ")}`);
  if (parts.length > 0) {
    ui.showToast(`${what}：${parts.join("，")}`, result.ambiguous.length > 0 || result.missing.length > 0 ? "warn" : "info");
  }
}

export function addNodeWithAutoConnect(
  opId: string,
  position: { x: number; y: number },
): AutoConnectResult {
  const result = useGraphStore.getState().addNodeAuto(opId, position);
  report(result, "已添加");
  return result;
}

export function insertSnippet(snippet: SnippetDesc, at: { x: number; y: number }): AutoConnectResult {
  const result = useGraphStore.getState().insertSnippet(snippet, at);
  report(result, `已插入「${snippet.label}」`);
  return result;
}

export function insertSnippetById(id: string, at: { x: number; y: number }): AutoConnectResult | null {
  const snippet = findSnippet(id);
  if (!snippet) {
    useUiStore.getState().showToast(`片段不存在：${id}`, "warn");
    return null;
  }
  return insertSnippet(snippet, at);
}
