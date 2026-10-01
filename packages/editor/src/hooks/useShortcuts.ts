// 快捷键的分发。铁律：在输入框里打字时，除了表里标了 inTextField 的什么都不拦。
// 键表本身在 lib/keymap.ts —— 处理器和 `?` 面板都从那一张表生成（E7）。

import { useEffect, type RefObject } from "react";

import { matchShortcut } from "../lib/keymap";
import { subgraphIdOf } from "../types/graph";
import { augmentOperators, levelOf } from "../lib/subgraph";
import { copyText, readClipboard } from "../lib/clipboard";
import { materializeBindings } from "../lib/graphParams";
import { stepHistory } from "../lib/history";
import { decodeNodeClipboard, encodeNodeClipboard } from "../lib/nodeClipboard";
import { revealError } from "../lib/revealError";
import { useCompareStore } from "../store/compare";
import { useExecutionStore } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { currentOverrides } from "../store/recipe";
import { usePeekStore, type PeekWindow } from "../store/peek";
import { useUiStore } from "../store/ui";
import { useModalStore } from "../lib/modal";

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
  /** F：把视图对准选中的节点（没选中时提示一句）。 */
  onFitSelection: () => void;
  /** 画布坐标，粘贴和搜索面板需要知道往哪儿放 */
  cursorFlowPosition: () => { x: number; y: number };
  cursorScreenPosition: () => { x: number; y: number };
}

export function useShortcuts(
  handlers: ShortcutHandlers,
  root: RefObject<HTMLElement | null>,
) {
  useEffect(() => {
    const onKeyDown = (e: KeyboardEvent) => {
      const graph = useGraphStore.getState();
      const ui = useUiStore.getState();

      // 编辑器自己的对话框开着（起配方名、确认删除……）：按键全归它，快捷键一个都不响
      if (useModalStore.getState().current) return;
      // 搜索弹层、查找节点和快捷键面板自己处理按键，别在这里抢
      if (ui.searchPopup) {
        if (e.key === "Escape") ui.closeSearch();
        return;
      }
      if (ui.finderOpen) return;
      if (ui.helpOpen && e.key === "Escape") {
        ui.setHelpOpen(false);
        return;
      }
      if (e.key === "Escape" && useExecutionStore.getState().runStatus !== "running") {
        const peek = usePeekStore.getState();
        const top = peek.windows.reduce<PeekWindow | null>(
          (best, w) => (best === null || w.z > best.z ? w : best),
          null,
        );
        if (top) {
          e.preventDefault();
          peek.close(top.id);
          return;
        }
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
        case "nextError":
        case "prevError":
          // 没有出错的节点时不吞掉按键
          if (revealError(hit.id === "nextError" ? 1 : -1)) e.preventDefault();
          return;
        case "cancel":
          // Esc 先退子图再取消运行：在子图里按 Esc，用户想的是「出去」
          if (ui.path.length > 0 && useExecutionStore.getState().runStatus !== "running") {
            e.preventDefault();
            ui.exitTo(ui.path.length - 1);
            return;
          }
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
          stepHistory("undo");
          return;
        case "redo":
          e.preventDefault();
          stepHistory("redo");
          return;

        case "copy":
        case "cut": {
          const ids = ui.selectedNodes;
          if (ids.size === 0) return;
          e.preventDefault();
          const doc = graph.doc;
          // 从当前这一层取：选区是本层的 id。以前取的是顶层的 doc.nodes —— 在子图里复制要么一个都没有，
          // 要么拿到顶层同名的那个节点；剪切更糟，剪贴板里是错的，本层的节点却真删了
          const level = levelOf(doc, ui.path);
          const ops = augmentOperators(useManifestStore.getState().operatorsById, doc.subgraphs);
          const nodes = materializeBindings(
            doc,
            ui.path,
            level.nodes.filter((n) => ids.has(n.id)),
            ops,
            currentOverrides(),
          );
          // 只带走两端都在选区内的边 —— 粘贴时内部连线得以保留
          const edges = level.edges.filter((edge) => ids.has(edge.from.node) && ids.has(edge.to.node));
          const clip = { nodes: JSON.parse(JSON.stringify(nodes)), edges: JSON.parse(JSON.stringify(edges)) };
          ui.setClipboard(clip);
          // 也写一份到系统剪贴板：另一个窗口、重开之后照样粘得进来（写不进就只有应用内这一份）
          void copyText(encodeNodeClipboard(clip));
          if (hit.id === "cut") graph.deleteNodes([...ids]);
          ui.showToast(`已复制 ${nodes.length} 个节点`);
          return;
        }
        case "paste": {
          e.preventDefault();
          const at = handlers.cursorFlowPosition();
          // 系统剪贴板里是节点（另一个窗口复制的、或者一整张图的 JSON）就粘它，否则用应用内的那一份
          void readClipboard().then((text) => {
            const u = useUiStore.getState();
            const clip = (text ? decodeNodeClipboard(text) : null) ?? u.clipboard;
            if (!clip || clip.nodes.length === 0) return;
            const result = useGraphStore.getState().pasteNodes(clip, at);
            if (result.nodeIds.length > 0) u.setSelection(result.nodeIds, []);
            else u.showToast("剪贴板里的算子在当前 core 里不存在", "warn");
          });
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

        case "compose": {
          const ids = [...ui.selectedNodes];
          if (ids.length === 0) {
            ui.showToast("先选中要合成的节点", "warn");
            return;
          }
          e.preventDefault();
          const result = graph.composeSubgraph(ids);
          if (result) {
            ui.setSelection([result.nodeId], []);
            ui.showToast(`已合成子图（${ids.length} 个节点）`);
          }
          return;
        }
        case "dissolve": {
          const ids = [...ui.selectedNodes];
          if (ids.length !== 1) return;
          e.preventDefault();
          const inlined = graph.dissolveSubgraph(ids[0]!);
          if (inlined.length > 0) {
            ui.setSelection(inlined, []);
            ui.showToast(`已解散，内联了 ${inlined.length} 个节点`);
          } else {
            ui.showToast("选中的不是子图节点", "warn");
          }
          return;
        }
        case "enterSubgraph": {
          const ids = [...ui.selectedNodes];
          if (ids.length !== 1) return;
          const level = levelOf(graph.doc, ui.path);
          const node = level.nodes.find((n) => n.id === ids[0]);
          const subgraphId = node ? subgraphIdOf(node.op) : null;
          if (!subgraphId) return;
          e.preventDefault();
          ui.enterSubgraph({ nodeId: ids[0]!, subgraphId });
          return;
        }

        case "layout":
          e.preventDefault();
          handlers.onLayout();
          return;
        case "fitView":
          e.preventDefault();
          handlers.onFitView();
          return;
        case "fitSelection":
          e.preventDefault();
          handlers.onFitSelection();
          return;
        case "toggleDrawer":
          e.preventDefault();
          ui.toggleDrawer();
          return;
        case "paramPanel":
          e.preventDefault();
          ui.toggleParamPanel();
          return;
        case "compare":
          e.preventDefault();
          useCompareStore.getState().toggle();
          return;
        case "measure":
          e.preventDefault();
          ui.setViewerMeasuring(!ui.viewerMeasuring);
          return;
        case "help":
          e.preventDefault();
          ui.setHelpOpen(!ui.helpOpen);
          return;
        case "findNode":
          e.preventDefault();
          ui.setFinderOpen(true);
          return;
      }
    };

    // 挂在编辑器根元素上（A2-3）：宿主页面里的其它输入不该被我们拦截。
    const el = root.current;
    if (!el) return;
    const owner = el.ownerDocument;
    // 焦点掉回 body（刚才那个输入框被卸载了之类）时谁都收不到键，
    // 这一条只接管「无主」的按键，宿主自己的控件仍然不受影响。
    const onOrphanKeyDown = (e: KeyboardEvent) => {
      if (e.target !== owner.body) return;
      onKeyDown(e);
    };
    el.addEventListener("keydown", onKeyDown);
    owner.addEventListener("keydown", onOrphanKeyDown);
    return () => {
      el.removeEventListener("keydown", onKeyDown);
      owner.removeEventListener("keydown", onOrphanKeyDown);
    };
  }, [handlers, root]);
}
