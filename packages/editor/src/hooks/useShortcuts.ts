// 快捷键的分发。铁律：在输入框里打字时，除了表里标了 inTextField 的什么都不拦。
// 键表本身在 lib/keymap.ts —— 处理器和 `?` 面板都从那一张表生成（E7）。

import { useEffect, type RefObject } from "react";

import { matchShortcut } from "../lib/keymap";
import { subgraphIdOf } from "../types/graph";
import { levelOf } from "../lib/subgraph";
import { copyNodes, deleteHealing, deleteSelection } from "../lib/editActions";
import { stepHistory } from "../lib/history";
import { decodeNodeClipboard } from "../lib/nodeClipboard";
import { revealError } from "../lib/revealError";
import { useCompareStore } from "../store/compare";
import { useExecutionStore } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { usePeekStore, type PeekWindow } from "../store/peek";
import { useUiStore } from "../store/ui";
import { useModalStore } from "../lib/modal";

/** 打字用的 input。勾选框、滑块、颜色、文件这些 input 不打字 —— 以前一律当输入框，点过勾选框、拖过滑块之后
 *  焦点留在上面，Ctrl+Z、Delete、F 都没反应。 */
const TYPING_INPUTS = new Set(["text", "search", "number", "email", "url", "tel", "password", "date", "datetime-local", "month", "time", "week"]);

function inTextField(target: EventTarget | null): boolean {
  const el = target as HTMLElement | null;
  if (!el) return false;
  const tag = el.tagName;
  if (tag === "INPUT") return TYPING_INPUTS.has((el as HTMLInputElement).type);
  return tag === "TEXTAREA" || tag === "SELECT" || el.isContentEditable === true;
}

/** 按钮、链接、各种 input 这些控件：Tab 在它们之间挪焦点、Space 按下它们。 */
const CONTROL_SELECTOR =
  'button, a[href], summary, input, select, textarea, [role="button"], [role="menuitem"], [role="tab"], [role="checkbox"], [role="switch"], [role="slider"], [role="option"]';

function onControl(target: EventTarget | null): boolean {
  return target instanceof Element && target.closest(CONTROL_SELECTOR) !== null;
}

/** 焦点在输入框里时先让它提交：数字框、文字框都是失焦才提交，不先提交的话 F5 跑的、Ctrl+S 存的是打字之前的值
 *  （以前就是这样：打了 1234 没失焦就按 F5，跑的还是 1000；Ctrl+S 干脆不响应）。失焦触发的 onBlur 是同步的，
 *  这一句返回时 store 里已经是新值；再把焦点放回去，接着改不用重新点。 */
function commitFocusedField(target: EventTarget | null): void {
  if (!inTextField(target) || !(target instanceof HTMLElement)) return;
  target.blur();
  target.focus();
}

/** 页面上选着一段（不全是空白的）文字。 */
function hasTextSelection(doc: Document): boolean {
  const sel = doc.getSelection();
  return sel != null && !sel.isCollapsed && sel.toString().trim() !== "";
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
    // 挂在编辑器根元素上（A2-3）：宿主页面里的其它输入不该被我们拦截。
    const el = root.current;
    if (!el) return;
    const owner = el.ownerDocument;
    // 最近一次按下鼠标落没落在画布上。没落在画布上、又选着一段文字（日志、诊断、文档……）时，
    // Ctrl+C / Ctrl+X 归浏览器：要的是那段文字。以前复制走的是选中的节点，Ctrl+X 还把节点删了
    let pointerOnCanvas = true;
    const onPointerDown = (e: PointerEvent) => {
      pointerOnCanvas = e.target instanceof Element && e.target.closest(".react-flow") !== null;
    };

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
      // 输入框里的 Esc 归那个框（撤回打的字、清空搜索），一次只退一层。这个监听挂在编辑器根元素上，比框自己的
      // onKeyDown（React 在根容器上才分发）先到，框里 stopPropagation 也拦不住它：以前运行中在参数框里按 Esc 想撤回
      // 打的字，运行跟着被取消；在子图里按，顺带退出了子图；开着的查看器窗口也被关掉一个。框处理完会失焦，再按一次才是全局的
      if (e.key === "Escape" && inTextField(e.target)) return;
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
      // 键表说了这个动作在输入框里也响应才响应（F5、F8、存盘这几个打不出字符的）——
      // 用户很可能刚改完参数、焦点还在输入框里就按 F5。
      // 下拉框里没有可撤的文字：Ctrl+Z / Ctrl+Y 归编辑器（以前交给浏览器，选完一项按 Ctrl+Z 什么也不发生）。
      // 字母跳选、方向键、Space 仍归下拉框
      const undoOnSelect = (hit.id === "undo" || hit.id === "redo") && (e.target as HTMLElement | null)?.tagName === "SELECT";
      if (!hit.inTextField && inTextField(e.target) && !undoOnSelect) return;

      switch (hit.id) {
        case "run":
          e.preventDefault();
          commitFocusedField(e.target);
          handlers.onRun();
          return;
        case "runToNode":
          e.preventDefault();
          commitFocusedField(e.target);
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
          commitFocusedField(e.target);
          handlers.onSave();
          return;
        case "saveAs":
          e.preventDefault();
          commitFocusedField(e.target);
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
          if (!pointerOnCanvas && hasTextSelection(owner)) return;
          e.preventDefault();
          copyNodes(ids, hit.id === "cut");
          return;
        }
        case "paste":
          // 不拦、也不在这里读剪贴板：navigator.clipboard.readText() 在 WebView2 里要权限，
          // 会弹一个「想要查看剪贴板」的框，没人点就一直挂着。放这次按键过去，浏览器自己发 paste 事件，
          // 剪贴板的内容就在事件里（onPaste）
          return;
        case "duplicate": {
          if (ui.selectedNodes.size === 0) return;
          e.preventDefault();
          const result = graph.duplicateNodes([...ui.selectedNodes]);
          if (result.nodeIds.length > 0) ui.setSelection(result.nodeIds, []);
          return;
        }
        case "selectAll":
          e.preventDefault();
          // 这一层的节点。以前取顶层的：在子图里 Ctrl+A 选上的是一组这一层没有的 id
          ui.setSelection(levelOf(graph.doc, ui.path).nodes.map((n) => n.id), []);
          return;
        case "delete": {
          if (ui.selectedNodes.size === 0 && ui.selectedEdges.size === 0) return;
          e.preventDefault();
          deleteSelection([...ui.selectedNodes], [...ui.selectedEdges]);
          return;
        }

        case "deleteHeal": {
          // 从链中间拿掉一步：删掉选中的节点，上下游按静音透传的规则接回去（一条撤销）。以前只能 Delete，上下游
          // 全断开，后面分出几支就要再拖几次线
          if (ui.selectedNodes.size === 0) return;
          e.preventDefault();
          deleteHealing([...ui.selectedNodes]);
          return;
        }
        case "mute": {
          const ids = [...ui.selectedNodes];
          if (ids.length === 0) return;
          e.preventDefault();
          // 以第一个选中节点的当前状态为准整体切换，避免多选时互相翻转。从这一层找它：
          // 以前在顶层找，子图里找不到，于是永远是「静音」，再按一次也取消不了
          const first = levelOf(graph.doc, ui.path).nodes.find((n) => n.id === ids[0]);
          graph.setBypass(ids, !(first?.bypass === true));
          return;
        }
        case "collapse": {
          const ids = [...ui.selectedNodes];
          if (ids.length === 0) return;
          e.preventDefault();
          const first = levelOf(graph.doc, ui.path).nodes.find((n) => n.id === ids[0]);
          graph.setCollapsed(ids, !(first?.ui?.collapsed === true));
          return;
        }
        case "rename": {
          // 与双击标题同一个改名框（节点上就地改）。多选时改谁不明确，不响应
          const ids = [...ui.selectedNodes];
          if (ids.length !== 1) return;
          e.preventDefault();
          ui.setRenamingNode(ids[0]!);
          return;
        }
        case "search":
          // 焦点在按钮、勾选框这些控件上时 Tab / Space 是它们自己的（挪焦点、按下去）。以前一律拿去开算子搜索：
          // 用键盘点不了工具栏的按钮，Tab 也走不出去
          if (onControl(e.target)) return;
          e.preventDefault();
          // 只选中了一条连线（没有节点）：选中的算子插到它中间。框选会把相连的线一起选上，所以「没选节点」不能省
          ui.openSearch({
            screen: handlers.cursorScreenPosition(),
            flow: handlers.cursorFlowPosition(),
            ...(ui.selectedEdges.size === 1 && ui.selectedNodes.size === 0 ? { insertEdge: [...ui.selectedEdges][0]! } : {}),
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

    // Ctrl+V 粘节点。系统剪贴板里是节点（另一个窗口复制的、或者一整张图的 JSON）就粘它。应用内的那一份只在系统剪贴板
    // 指望不上的时候顶上：上次复制没写进去、或者读出来是空的。系统剪贴板里是别的字 = 复制了节点之后又去别处复制了
    // 东西 —— 以前照样粘出那几个旧节点
    const onPaste = (e: ClipboardEvent) => {
      if (useModalStore.getState().current || inTextField(e.target)) return;
      const ui = useUiStore.getState();
      if (ui.searchPopup || ui.finderOpen) return;
      const text = e.clipboardData?.getData("text/plain") ?? "";
      const decoded = text ? decodeNodeClipboard(text) : null;
      const clip = decoded ?? (text === "" || ui.clipboardOnlyInApp ? ui.clipboard : null);
      if (!clip || clip.nodes.length === 0) {
        if (text !== "" && !decoded) ui.showToast("剪贴板里不是节点", "warn");
        return;
      }
      e.preventDefault();
      const result = useGraphStore.getState().pasteNodes(clip, handlers.cursorFlowPosition());
      if (result.nodeIds.length > 0) ui.setSelection(result.nodeIds, []);
      else ui.showToast("剪贴板里的算子在当前 core 里不存在", "warn");
    };

    // 焦点掉回 body（刚才那个输入框被卸载了之类）时谁都收不到键，
    // 这一条只接管「无主」的按键，宿主自己的控件仍然不受影响。
    const onOrphanKeyDown = (e: KeyboardEvent) => {
      if (e.target !== owner.body) return;
      onKeyDown(e);
    };
    const onOrphanPaste = (e: ClipboardEvent) => {
      if (e.target !== owner.body) return;
      onPaste(e);
    };
    el.addEventListener("keydown", onKeyDown);
    owner.addEventListener("keydown", onOrphanKeyDown);
    el.addEventListener("paste", onPaste);
    owner.addEventListener("paste", onOrphanPaste);
    owner.addEventListener("pointerdown", onPointerDown, true);
    return () => {
      el.removeEventListener("keydown", onKeyDown);
      owner.removeEventListener("keydown", onOrphanKeyDown);
      el.removeEventListener("paste", onPaste);
      owner.removeEventListener("paste", onOrphanPaste);
      owner.removeEventListener("pointerdown", onPointerDown, true);
    };
  }, [handlers, root]);
}
