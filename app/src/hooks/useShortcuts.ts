// 全局快捷键的分发。铁律：在输入框里打字时，除了表里标了 inTextField 的什么都不拦。
// 键表本身在 lib/keymap.ts —— 处理器和 `?` 面板都从那一张表生成（E7）。

import { useEffect } from "react";

import { matchShortcut } from "../lib/keymap";
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
  onRunToSelected: () => void;
  onCancel: () => void;
  onLayout: () => void;
  onFitView: () => void;
  /** 画布坐标，粘贴和搜索面板需要知道往哪儿放 */
  cursorFlowPosition: () => { x: number; y: number };
  cursorScreenPosition: () => { x: number; y: number };
}

export function useShortcuts(handlers: ShortcutHandlers) {
  useEffect(() => {
    const onKeyDown = (e: KeyboardEvent) => {
      const graph = useGraphStore.getState();
      const ui = useUiStore.getState();

      // 搜索弹层和快捷键面板自己处理按键，别在这里抢
      if (ui.searchPopup) {
        if (e.key === "Escape") ui.closeSearch();
        return;
      }
      if (ui.helpOpen && e.key === "Escape") {
        ui.setHelpOpen(false);
        return;
      }

      const hit = matchShortcut(e);
      if (!hit) return;
      // 键表说了这个动作在输入框里也响应才响应。F5/Esc 是唯二的例外 ——
      // 用户很可能刚改完参数、焦点还在输入框里就按 F5。
      if (!hit.inTextField && inTextField(e.target)) return;

      switch (hit.id) {
        case "run":
          e.preventDefault();
          handlers.onRun();
          return;
        case "runToNode":
          e.preventDefault();
          handlers.onRunToSelected();
          return;
        case "cancel":
          if (useExecutionStore.getState().runStatus !== "running") return;
          e.preventDefault();
          handlers.onCancel();
          return;

        case "save":
          e.preventDefault();
          handlers.onSave();
          return;
        case "saveAs":
          e.preventDefault();
          handlers.onSaveAs();
          return;
        case "open":
          e.preventDefault();
          handlers.onOpen();
          return;
        case "new":
          e.preventDefault();
          handlers.onNew();
          return;

        case "undo":
          e.preventDefault();
          graph.undo();
          return;
        case "redo":
          e.preventDefault();
          graph.redo();
          return;

        case "copy":
        case "cut": {
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
          if (hit.id === "cut") graph.deleteNodes([...ids]);
          ui.showToast(`已复制 ${nodes.length} 个节点`);
          return;
        }
        case "paste": {
          const clip = ui.clipboard;
          if (!clip || clip.nodes.length === 0) return;
          e.preventDefault();
          const result = graph.pasteNodes(clip, handlers.cursorFlowPosition());
          if (result.nodeIds.length > 0) ui.setSelection(result.nodeIds, []);
          else ui.showToast("剪贴板里的算子在当前 core 里不存在", "warn");
          return;
        }
        case "duplicate": {
          if (ui.selectedNodes.size === 0) return;
          e.preventDefault();
          const result = graph.duplicateNodes([...ui.selectedNodes]);
          if (result.nodeIds.length > 0) ui.setSelection(result.nodeIds, []);
          return;
        }
        case "selectAll":
          e.preventDefault();
          ui.setSelection(graph.doc.nodes.map((n) => n.id), []);
          return;
        case "delete": {
          if (ui.selectedNodes.size === 0 && ui.selectedEdges.size === 0) return;
          e.preventDefault();
          if (ui.selectedEdges.size > 0) graph.disconnect([...ui.selectedEdges]);
          if (ui.selectedNodes.size > 0) graph.deleteNodes([...ui.selectedNodes]);
          ui.clearSelection();
          return;
        }

        case "mute": {
          const ids = [...ui.selectedNodes];
          if (ids.length === 0) return;
          e.preventDefault();
          // 以第一个选中节点的当前状态为准整体切换，避免多选时互相翻转
          const first = graph.doc.nodes.find((n) => n.id === ids[0]);
          graph.setBypass(ids, !(first?.bypass === true));
          return;
        }
        case "collapse": {
          const ids = [...ui.selectedNodes];
          if (ids.length === 0) return;
          e.preventDefault();
          const first = graph.doc.nodes.find((n) => n.id === ids[0]);
          graph.setCollapsed(ids, !(first?.ui?.collapsed === true));
          return;
        }
        case "search":
          e.preventDefault();
          ui.openSearch({
            screen: handlers.cursorScreenPosition(),
            flow: handlers.cursorFlowPosition(),
          });
          return;

        case "layout":
          e.preventDefault();
          handlers.onLayout();
          return;
        case "fitView":
          e.preventDefault();
          handlers.onFitView();
          return;
        case "toggleDrawer":
          e.preventDefault();
          ui.toggleDrawer();
          return;
        case "help":
          e.preventDefault();
          ui.setHelpOpen(!ui.helpOpen);
          return;
      }
    };

    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, [handlers]);
}
