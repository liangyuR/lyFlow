//
// 全局快捷键。
//
// 一条铁律贯穿全文件：**在输入框里打字时，除了 Esc 什么都不拦**。
// 在参数输入框里按 Ctrl+A 却全选了画布上的节点，或者按 Delete 却删了节点，
// 是这类编辑器最经典的事故。
//

import { useEffect } from "react";

import { useExecutionStore } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useUiStore } from "../store/ui";

function inTextField(target: EventTarget | null): boolean {
  const el = target as HTMLElement | null;
  if (!el) return false;
  const tag = el.tagName;
  return (
    tag === "INPUT" ||
    tag === "TEXTAREA" ||
    tag === "SELECT" ||
    el.isContentEditable === true
  );
}

export interface ShortcutHandlers {
  onSave: () => void;
  onSaveAs: () => void;
  onOpen: () => void;
  onNew: () => void;
  onRun: () => void;
  onCancel: () => void;
  /** 画布坐标，粘贴和搜索面板需要知道往哪儿放 */
  cursorFlowPosition: () => { x: number; y: number };
  cursorScreenPosition: () => { x: number; y: number };
}

export function useShortcuts(handlers: ShortcutHandlers) {
  useEffect(() => {
    const onKeyDown = (e: KeyboardEvent) => {
      const graph = useGraphStore.getState();
      const ui = useUiStore.getState();
      const mod = e.ctrlKey || e.metaKey;

      // 搜索弹层自己处理按键，别在这里抢
      if (ui.searchPopup) {
        if (e.key === "Escape") ui.closeSearch();
        return;
      }

      // -- 运行 ------------------------------------------------------------
      // F5 和 Esc 要在 inTextField 之前处理：用户很可能刚改完一个参数、
      // 焦点还在输入框里就按 F5。这两个键在输入框里都没有别的含义，
      // 不会和「在输入框里打字时什么都不拦」那条铁律冲突。
      if (e.key === "F5") {
        e.preventDefault();
        handlers.onRun();
        return;
      }
      if (e.key === "Escape" && useExecutionStore.getState().runStatus === "running") {
        e.preventDefault();
        handlers.onCancel();
        return;
      }

      if (inTextField(e.target)) return;

      // -- 文件 ------------------------------------------------------------
      if (mod && e.key.toLowerCase() === "s") {
        e.preventDefault();
        if (e.shiftKey) handlers.onSaveAs();
        else handlers.onSave();
        return;
      }
      if (mod && e.key.toLowerCase() === "o") {
        e.preventDefault();
        handlers.onOpen();
        return;
      }
      if (mod && e.key.toLowerCase() === "n") {
        e.preventDefault();
        handlers.onNew();
        return;
      }

      // -- 撤销重做 --------------------------------------------------------
      if (mod && e.key.toLowerCase() === "z") {
        e.preventDefault();
        // Ctrl+Shift+Z 和 Ctrl+Y 都要认：两派用户各有习惯
        if (e.shiftKey) graph.redo();
        else graph.undo();
        return;
      }
      if (mod && e.key.toLowerCase() === "y") {
        e.preventDefault();
        graph.redo();
        return;
      }

      // -- 剪贴板 ----------------------------------------------------------
      if (mod && (e.key.toLowerCase() === "c" || e.key.toLowerCase() === "x")) {
        const ids = ui.selectedNodes;
        if (ids.size === 0) return;
        e.preventDefault();
        const doc = graph.doc;
        const nodes = doc.nodes.filter((n) => ids.has(n.id));
        // 只带走两端都在选区内的边 —— 粘贴时内部连线得以保留
        const edges = doc.edges.filter((edge) => ids.has(edge.from.node) && ids.has(edge.to.node));
        ui.setClipboard({
          nodes: JSON.parse(JSON.stringify(nodes)),
          edges: JSON.parse(JSON.stringify(edges)),
        });
        if (e.key.toLowerCase() === "x") graph.deleteNodes([...ids]);
        ui.showToast(`已复制 ${nodes.length} 个节点`);
        return;
      }

      if (mod && e.key.toLowerCase() === "v") {
        const clip = ui.clipboard;
        if (!clip || clip.nodes.length === 0) return;
        e.preventDefault();
        const result = graph.pasteNodes(clip, handlers.cursorFlowPosition());
        if (result.nodeIds.length > 0) {
          ui.setSelection(result.nodeIds, []);
        } else {
          ui.showToast("剪贴板里的算子在当前 core 里不存在", "warn");
        }
        return;
      }

      if (mod && e.key.toLowerCase() === "a") {
        e.preventDefault();
        ui.setSelection(
          graph.doc.nodes.map((n) => n.id),
          [],
        );
        return;
      }

      // -- 搜索面板 --------------------------------------------------------
      // Tab 和空格都是节点编辑器的通用习惯（Blender 用 Shift+A / ComfyUI 用双击）
      if (!mod && (e.key === "Tab" || e.key === " ")) {
        e.preventDefault();
        ui.openSearch({
          screen: handlers.cursorScreenPosition(),
          flow: handlers.cursorFlowPosition(),
        });
        return;
      }
    };

    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, [handlers]);
}
